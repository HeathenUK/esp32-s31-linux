// SPDX-License-Identifier: GPL-2.0-only
/*
 * Context switch cost without the blocking path.
 *
 * A pipe round trip here costs ~900 us whether the peer is a thread or a
 * process, which is too uniform to be switch_mm and suspiciously close to one
 * timer tick - a blocking round trip can be paced by the tick rather than by
 * the switch. Two runnable threads calling sched_yield() never block and never
 * let the CPU idle, so what is left is the switch itself.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

static volatile long spins;
static volatile int stop;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void *peer(void *arg)
{
	(void)arg;
	while (!stop) {
		sched_yield();
		spins++;
	}
	return NULL;
}

int main(void)
{
	const long n = 20000;
	pthread_t th;
	double t0, t1, solo, duo;

	/* sched_yield with nothing else runnable: the syscall cost alone. */
	t0 = now();
	for (long i = 0; i < n; i++)
		sched_yield();
	t1 = now();
	solo = (t1 - t0) * 1e6 / n;
	printf("yield, nothing else runnable: %7.2f us  (syscall only)\n", solo);

	pthread_create(&th, NULL, peer, NULL);
	t0 = now();
	for (long i = 0; i < n; i++)
		sched_yield();
	t1 = now();
	duo = (t1 - t0) * 1e6 / n;
	stop = 1;
	pthread_join(th, NULL);

	printf("yield, peer runnable:         %7.2f us  (syscall + a switch)\n", duo);
	printf("=> context switch is about    %7.2f us\n", duo - solo);
	return 0;
}
