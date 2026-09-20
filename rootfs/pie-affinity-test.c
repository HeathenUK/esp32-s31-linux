/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static unsigned char src[256] __attribute__((aligned(16)));
static unsigned char dst[256] __attribute__((aligned(16)));
static void *(*volatile stock_copy)(void *, const void *, size_t) = memcpy;
extern int pie_sleep_copy(const void *, void *);

static unsigned long mask(void)
{
    unsigned long m = 0;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(m), &m) < 0) return ~0UL;
    return m;
}
static int setmask(unsigned long m)
{
    return syscall(SYS_sched_setaffinity, 0, sizeof(m), &m);
}
static void pause10(void)
{
    struct timespec t = { .tv_nsec = 10000000 };
    while (nanosleep(&t, &t) && errno == EINTR) { }
}
static int pin_lent(void)
{
    for (int i = 0; i < 250; i++) {
        if (!setmask(2)) return 0;
        if (errno != EINVAL) return 1;
        pause10();
    }
    return 1;
}
static int waitmask(unsigned long want)
{
    for (int i = 0; i < 250; i++) {
        if (mask() == want) return 0;
        pause10();
    }
    return 1;
}
static int copy_and_restrict(void)
{
    if (pin_lent()) return 1;
    stock_copy(dst, src, sizeof(src));
    if (mask() != 1) return 2;
    for (unsigned i = 0; i < sizeof(src); i++)
        if (dst[i] != src[i]) return 3;
    return 0;
}
int main(void)
{
    int failures = 0, rc, status;
    for (unsigned i = 0; i < sizeof(src); i++) src[i] = i ^ 0xa5;

    rc = copy_and_restrict();
    if (!rc) rc = waitmask(2) ? 4 : 0;
    printf("pie-affinity: stock_memcpy_restore %s rc=%d\n", rc ? "FAIL" : "PASS", rc);
    failures += !!rc;

    rc = copy_and_restrict();
    if (!rc) {
        pid_t child = fork();
        if (child == 0) _exit(waitmask(2));
        if (child < 0 || waitpid(child, &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status)) rc = 5;
    }
    printf("pie-affinity: fork_restore %s rc=%d\n", rc ? "FAIL" : "PASS", rc);
    failures += !!rc;

    rc = copy_and_restrict();
    if (!rc) {
        if (setmask(1)) rc = 6;
        for (int i = 0; i < 40; i++) pause10();
        if (mask() != 1) rc = 7;
    }
    printf("pie-affinity: explicit_affinity_preserved %s rc=%d\n", rc ? "FAIL" : "PASS", rc);
    failures += !!rc;

    rc = pin_lent();
    if (!rc) {
        for (int i = 0; i < 16; i++) dst[i] = 0;
        /* q0 stays live across a direct sleep syscall and CPU migration. */
        int woke_cpu = pie_sleep_copy(src, dst);
        if (woke_cpu != 1) rc = 8;
        for (int i = 0; i < 16; i++) if (dst[i] != src[i]) rc = 9;
    }
    printf("pie-affinity: live_vector_migration %s rc=%d\n", rc ? "FAIL" : "PASS", rc);
    failures += !!rc;
    printf("pie-affinity: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return !!failures;
}
