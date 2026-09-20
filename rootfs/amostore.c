// Does a PLAIN STORE by one hart land inside the other hart's ATOMIC
// read-modify-write on this silicon?
//
// Linux's riscv ticket spinlock takes the lock with amoadd.w on the 32-bit lock
// word (next += 1 in the high half) and RELEASES it with a plain 16-bit store
// into the low half (owner++). That is only correct if the hardware makes the
// AMO indivisible against the OTHER hart's plain store. If it does not, the
// AMO's write-back carries a stale low half, the unlock is erased, and every
// later locker spins for ever - the permanent do_raw_spin_lock stalls seen on
// SMP boots here (2026-09-20, docs/worklog-2026-09-19.md).
//
//   amostore <iterations> <cpuB> <mode>
//     cpuB  1 = cross-CPU (the test)     0 = same CPU (must show ZERO losses)
//     mode  0 = thread A uses amoadd.w   1 = POSITIVE CONTROL: A does a
//               deliberately non-atomic load/add/store, which MUST show losses
//               cross-CPU or this program cannot see the effect at all
//
// Thread A (CPU0): w += 0x10000, N times.          <- the lock's acquire
// Thread B (cpuB): low16(w) = ++count, N times,    <- the lock's release
//                  checking before each store that its PREVIOUS store is still
//                  there. A mismatch is a store erased by A's write-back.
//
// THE WORKERS MAKE NO LIBC CALLS. musl's strcmp/memcmp/memchr are PIE code, PIE
// exists only on hart 1, and the kernel pins a task to CPU0 the first time it
// traps on one - which would silently turn this into a same-CPU run and a false
// pass. Raw syscalls only in the threads; each records its CPU before and
// after, and main() refuses the result if B was not where it was asked to be.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static union { volatile uint32_t w; volatile uint16_t h[2]; } lockword
	__attribute__((aligned(64)));
static unsigned long iterations = 5000000;
static int cpu_b = 1, mode;
static volatile int go;
static volatile unsigned long lost_store, a_done, b_done;
static volatile int cpu_seen[2][2] = {{-1, -1}, {-1, -1}};
static volatile int pin_failed;

static int where(void)
{
	unsigned cpu = 99;

	syscall(SYS_getcpu, &cpu, NULL, NULL);
	return (int)cpu;
}

static void pin(int cpu)
{
	unsigned long mask = 1UL << cpu;

	if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), &mask))
		pin_failed = 1;
}

static void *thread_a(void *arg)
{
	unsigned long i;

	(void)arg;
	pin(0);
	cpu_seen[0][0] = where();
	while (!go)
		;
	if (mode == 0) {
		for (i = 0; i < iterations; i++)
			__atomic_fetch_add(&lockword.w, 0x10000, __ATOMIC_SEQ_CST);
	} else {
		for (i = 0; i < iterations; i++) {	/* positive control: NOT atomic */
			uint32_t v = lockword.w;

			lockword.w = v + 0x10000;
		}
	}
	a_done = i;
	cpu_seen[0][1] = where();
	return NULL;
}

static void *thread_b(void *arg)
{
	unsigned long i;
	uint16_t mine = 0;

	(void)arg;
	pin(cpu_b);
	cpu_seen[1][0] = where();
	while (!go)
		;
	for (i = 0; i < iterations; i++) {
		if (lockword.h[0] != mine)	/* my last store was erased */
			lost_store++;
		mine++;
		lockword.h[0] = mine;		/* the plain store an unlock is */
	}
	b_done = i;
	cpu_seen[1][1] = where();
	return NULL;
}

static void on_alarm(int sig)
{
	static const char m[] = "amostore: TIMEOUT\n";

	(void)sig;
	(void)!write(2, m, sizeof(m) - 1);
	_exit(3);
}

int main(int argc, char **argv)
{
	pthread_t a, b;
	int bad_place;

	if (argc > 1) iterations = strtoul(argv[1], NULL, 0);
	if (argc > 2) cpu_b = atoi(argv[2]);
	if (argc > 3) mode = atoi(argv[3]);
	signal(SIGALRM, on_alarm);
	alarm(90);
	if (pthread_create(&a, NULL, thread_a, NULL) || pthread_create(&b, NULL, thread_b, NULL)) {
		perror("pthread_create");
		return 2;
	}
	usleep(200000);		/* both pinned and spinning on `go` */
	go = 1;
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	bad_place = pin_failed || cpu_seen[0][0] != 0 || cpu_seen[0][1] != 0 ||
		    cpu_seen[1][0] != cpu_b || cpu_seen[1][1] != cpu_b;
	printf("amostore mode=%s cpuB=%d iters=%lu | A on cpu %d->%d, B on cpu %d->%d%s | "
	       "B stores erased=%lu | A adds: expected high16=%04lx got %04x%s\n",
	       mode ? "PLAIN-RMW(positive control)" : "amoadd.w", cpu_b, iterations,
	       cpu_seen[0][0], cpu_seen[0][1], cpu_seen[1][0], cpu_seen[1][1],
	       bad_place ? " PLACEMENT WRONG - RESULT VOID" : "",
	       lost_store, a_done & 0xffff, lockword.h[1],
	       ((a_done & 0xffff) != lockword.h[1]) ? " (A ADDS LOST)" : "");
	return bad_place ? 2 : (lost_store || (a_done & 0xffff) != lockword.h[1]);
}
