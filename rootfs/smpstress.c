/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded SMP correctness probe: cross-CPU futex handoffs, shared atomics,
 * and live FPU state across scheduling. No SDL/client modifications. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static atomic_int turn, errors, ready, go;
static atomic_uint completed[2], total;
static unsigned iterations = 10000;
static int cpus[2] = {0, 1};
static int timer_test;
static uint64_t last_ns, max_sleep_ns;
static atomic_uint clock_errors;

static void timeout(int sig)
{
    static const char msg[] = "smpstress: FAIL timeout (lost wakeup or stalled CPU)\n";
    (void)sig;
    (void)write(2, msg, sizeof(msg)-1);
    _exit(2);
}

/* Keep explicit caller-saved and callee-saved F registers live through a
 * direct syscall. libc is deliberately outside this interval. */
static int fp_yield(unsigned pattern)
{
    register long call asm("a7") = SYS_sched_yield;
    register long result asm("a0");
    unsigned a, b, c, d;
    asm volatile(
        "fmv.w.x f0,%5\n\t"
        "fmv.w.x f8,%5\n\t"
        "fmv.w.x f18,%5\n\t"
        "fmv.w.x f31,%5\n\t"
        "ecall\n\t"
        "fmv.x.w %0,f0\n\t"
        "fmv.x.w %1,f8\n\t"
        "fmv.x.w %2,f18\n\t"
        "fmv.x.w %3,f31"
        : "=r"(a), "=r"(b), "=r"(c), "=r"(d), "=r"(result)
        : "r"(pattern), "r"(call)
        : "f0", "f8", "f18", "f31", "memory");
    return a == pattern && b == pattern && c == pattern && d == pattern;
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpus[id], &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask)) {
        perror("sched_setaffinity");
        _exit(1);
    }
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go)) sched_yield();
    for (unsigned i = 0; i < iterations; ++i) {
        for (;;) {
            int value = atomic_load_explicit(&turn, memory_order_acquire);
            if (value == id) break;
            if (syscall(SYS_futex, &turn, FUTEX_WAIT_PRIVATE, value,
                        NULL, NULL, 0) < 0 && errno != EAGAIN && errno != EINTR) {
                perror("futex wait");
                _exit(1);
            }
        }
        struct timespec stamp;
        clock_gettime(CLOCK_MONOTONIC, &stamp);
        uint64_t now_ns = (uint64_t)stamp.tv_sec*1000000000u + stamp.tv_nsec;
        if (now_ns < last_ns) atomic_fetch_add(&clock_errors, 1);
        last_ns = now_ns; /* turn's release/acquire serializes these writes */
        if (timer_test && id == 1) {
            struct timespec delay = { .tv_nsec = 1000000 };
            while (nanosleep(&delay, &delay) && errno == EINTR) { }
            clock_gettime(CLOCK_MONOTONIC, &stamp);
            uint64_t elapsed = (uint64_t)stamp.tv_sec*1000000000u + stamp.tv_nsec - now_ns;
            if (elapsed > max_sleep_ns) max_sleep_ns = elapsed;
        }
        if (sched_getcpu() != cpus[id] ||
            !fp_yield(0x3f000000u + (unsigned)id * 0x10000u + i))
            atomic_fetch_add(&errors, 1);
        atomic_fetch_add(&total, 1);
        atomic_fetch_add(&completed[id], 1);
        atomic_store_explicit(&turn, 1-id, memory_order_release);
        if (syscall(SYS_futex, &turn, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) < 0) {
            perror("futex wake");
            _exit(1);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[2];
    struct timespec start, end;
    if (argc > 1) iterations = (unsigned)strtoul(argv[1], NULL, 0);
    if (argc > 2) cpus[1] = atoi(argv[2]); /* 0: same-CPU control */
    if (argc > 3) timer_test = atoi(argv[3]);
    if (!iterations || iterations > 1000000 || cpus[1] < 0 || cpus[1] > 1)
        return 1;
    signal(SIGALRM, timeout);
    alarm(120);
    for (int i = 0; i < 2; ++i) {
        int err = pthread_create(&threads[i], NULL, worker, (void *)(intptr_t)i);
        if (err) { errno = err; perror("pthread_create"); return 1; }
    }
    while (atomic_load(&ready) != 2) sched_yield();
    clock_gettime(CLOCK_MONOTONIC, &start);
    atomic_store(&go, 1);
    for (int i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    alarm(0);
    long long us = (end.tv_sec-start.tv_sec)*1000000LL +
                   (end.tv_nsec-start.tv_nsec)/1000;
    int bad = atomic_load(&errors) || atomic_load(&clock_errors) || atomic_load(&total) != 2*iterations;
    printf("smpstress: %s cpus=%d,%d iterations=%u completed=%u,%u "
           "errors=%d elapsed_us=%lld roundtrip_us=%lld clock_errors=%u max_sleep_us=%llu\n",
           bad ? "FAIL" : "PASS", cpus[0], cpus[1], iterations,
           atomic_load(&completed[0]), atomic_load(&completed[1]),
           atomic_load(&errors), us, us/iterations, atomic_load(&clock_errors),
           (unsigned long long)(max_sleep_ns/1000));
    return bad;
}
