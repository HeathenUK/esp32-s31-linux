// SPDX-License-Identifier: GPL-2.0-only
/*
 * How expensive is a trap on this chip?
 *
 * Interrupts here cost around 95 us each - roughly 30k cycles at 320 MHz, where
 * a few thousand would be normal. That is either the trap transition itself or
 * the work the handler does once it is in. A syscall isolates the first: it is a
 * trap to S-mode and back with almost no work in between.
 *
 * getpid() is invoked through syscall() rather than the libc wrapper because
 * some libcs cache the result and would measure nothing at all.
 */

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	const long n = 20000;
	double a, b, empty;

	/* Cost of the timing loop itself, so it can be subtracted out. */
	a = now();
	for (long i = 0; i < n; i++)
		__asm__ volatile ("" ::: "memory");
	b = now();
	empty = b - a;

	a = now();
	for (long i = 0; i < n; i++)
		syscall(SYS_getpid);
	b = now();

	printf("syscall: %ld in %.3f s = %.2f us each (loop overhead %.3f s)\n",
	       n, b - a, ((b - a) - empty) * 1e6 / n, empty);

	a = now();
	for (long i = 0; i < n; i++)
		clock_gettime(CLOCK_MONOTONIC, &(struct timespec){0});
	b = now();
	printf("clock_gettime: %.2f us each (vDSO, no trap expected)\n",
	       ((b - a) - empty) * 1e6 / n);
	return 0;
}
