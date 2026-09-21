/*
 * pingpong - cross-CPU wake round-trip, the number that decides what the lent
 * CPU costs.
 *
 *   pingpong <hexmaskA> <hexmaskB> [iters]      e.g. pingpong 1 2 20000
 *
 * Two threads bounce a futex back and forth, each pinned where you say. Run it
 * 1/1 (both on the boot CPU) and 1/2 (across the harts) and the difference IS
 * the cost of reaching CPU1 - which on this board is not a hardware IPI into a
 * running Linux but a doorbell taken by FreeRTOS on hart 0 and injected into
 * the guest by the monitor (docs/smp-plan.md). Every wake-up that crosses to
 * the lent CPU pays it, which is why it is the first thing to attack and the
 * right thing to measure before and after.
 *
 * WHY NOT MEASURE THIS WITH THE DESKTOP: the windowed canary has a ~17%
 * per-boot lottery (docs/worklog-2026-09-19.md), so a few percent cannot be
 * resolved across boots without more boots than anyone should sit through.
 * This runs in seconds, entirely within one boot, and measures the mechanism
 * rather than something downstream of it.
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int turn;			/* 0: A's turn, 1: B's turn */
static int iters = 20000;

static int fwait(int *addr, int val)
{
	return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);
}

static int fwake(int *addr)
{
	return syscall(SYS_futex, addr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static void pin(unsigned long mask)
{
	cpu_set_t set;
	int i;

	CPU_ZERO(&set);
	for (i = 0; i < 8; i++)
		if (mask & (1UL << i))
			CPU_SET(i, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		perror("sched_setaffinity");
}

static unsigned long mask_b;

static void *side_b(void *arg)
{
	int i;

	(void)arg;
	pin(mask_b);
	for (i = 0; i < iters; i++) {
		while (__atomic_load_n(&turn, __ATOMIC_ACQUIRE) != 1)
			fwait(&turn, 0);
		__atomic_store_n(&turn, 0, __ATOMIC_RELEASE);
		fwake(&turn);
	}
	return NULL;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	unsigned long mask_a = argc > 1 ? strtoul(argv[1], NULL, 16) : 1;
	pthread_t th;
	double t0, dt;
	int i;

	mask_b = argc > 2 ? strtoul(argv[2], NULL, 16) : 2;
	if (argc > 3)
		iters = atoi(argv[3]);

	pin(mask_a);
	if (pthread_create(&th, NULL, side_b, NULL)) {
		perror("pthread_create");
		return 1;
	}
	usleep(100000);			/* let B reach its wait */

	t0 = now_s();
	for (i = 0; i < iters; i++) {
		__atomic_store_n(&turn, 1, __ATOMIC_RELEASE);
		fwake(&turn);
		while (__atomic_load_n(&turn, __ATOMIC_ACQUIRE) != 0)
			fwait(&turn, 1);
	}
	dt = now_s() - t0;
	pthread_join(th, NULL);

	printf("pingpong %lx<->%lx  %.2f us/roundtrip  (%d iters, %.3f s)\n",
	       mask_a, mask_b, dt * 1e6 / iters, iters, dt);
	return 0;
}
