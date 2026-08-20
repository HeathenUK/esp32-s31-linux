// SPDX-License-Identifier: GPL-2.0-only
/*
 * How long does this board take to wake something up?
 *
 * The SD path spends ~1.45 ms getting a command acknowledged and ~8 ms between
 * requests, while moving the data takes 22 us; the compositor sits at 0% CPU
 * while repaints land seconds late. Both are waiting rather than working, which
 * points at one cause: interrupt and wakeup latency.
 *
 * clock_nanosleep to an absolute deadline times exactly that chain - timer
 * interrupt, wakeup, scheduler, task running - and the overshoot is the answer.
 * A healthy system manages tens of microseconds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 200;
	long period_us = argc > 2 ? atol(argv[2]) : 5000;
	double *lat = calloc(n, sizeof(*lat));
	struct timespec t;
	int i;

	clock_gettime(CLOCK_MONOTONIC, &t);
	for (i = 0; i < n; i++) {
		struct timespec now;

		t.tv_nsec += period_us * 1000;
		while (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);

		lat[i] = (now.tv_sec - t.tv_sec) * 1e6 +
			 (now.tv_nsec - t.tv_nsec) / 1e3;
	}
	qsort(lat, n, sizeof(*lat), cmp);
	printf("wake latency over %d sleeps of %ld us:\n", n, period_us);
	printf("  min %.1f us   median %.1f us   p90 %.1f us   max %.1f us\n",
	       lat[0], lat[n / 2], lat[(n * 9) / 10], lat[n - 1]);
	return 0;
}
