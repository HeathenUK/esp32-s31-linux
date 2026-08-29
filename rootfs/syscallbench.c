// SPDX-License-Identifier: GPL-2.0-only
/*
 * What a syscall costs here, split into its parts.
 *
 * The trap entry path was already moved into `.text..fast` (RAM), which took a
 * read+write pair from 43.0 us to 9.0 us - see the generic-entry note. So
 * ~4.5 us per syscall should be the floor, and yet clock_gettime() measured
 * 7.2 us from inside lvdesk. That difference matters, because it points at two
 * completely different fixes:
 *
 *   entry-bound      -> the trap path, already optimised; little left
 *   clocksource-bound-> reading the timer, which on this SoC may be an
 *                       uncached MMIO read over the bus, and which no amount
 *                       of syscall tuning touches
 *   vDSO-able        -> clock_gettime need not be a syscall at all
 *
 * getpid() is the control: a syscall that does essentially nothing once it has
 * entered the kernel. The gap between it and clock_gettime() is the clock
 * read. If getpid() is itself dear, the entry path is still the problem.
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#define N 20000

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

int main(void)
{
	uint64_t a, b;
	volatile long sink = 0;
	int i;
	struct timespec t;

	/*
	 * Raw syscall, not the libc wrapper: musl caches getpid() in some
	 * versions and would measure a function call rather than a trap.
	 */
	a = now_ns();
	for (i = 0; i < N; i++)
		sink += syscall(SYS_getpid);
	b = now_ns();
	printf("getpid          %6llu ns/call\n",
	       (unsigned long long)((b - a) / N));

	a = now_ns();
	for (i = 0; i < N; i++)
		clock_gettime(CLOCK_MONOTONIC, &t);
	b = now_ns();
	printf("clock_gettime   %6llu ns/call\n",
	       (unsigned long long)((b - a) / N));

	/*
	 * The RAW syscall, for comparison with the libc call above. On a
	 * 32-bit target this is clock_gettime64 - the plain number does not
	 * exist since the time_t transition. If the libc version is markedly
	 * faster than this, a vDSO is answering it in userspace; if they match,
	 * every time query is paying a full trap.
	 */
	{
		struct { long long sec; long long nsec; } kt;

		a = now_ns();
		for (i = 0; i < N; i++)
			sink += syscall(SYS_clock_gettime64, CLOCK_MONOTONIC, &kt);
		b = now_ns();
		printf("clock_gettime   %6llu ns/call  (raw syscall, no vDSO)\n",
		       (unsigned long long)((b - a) / N));
	}

	/* A pure userspace loop of the same shape, to price the harness. */
	a = now_ns();
	for (i = 0; i < N; i++)
		sink += i;
	b = now_ns();
	printf("empty loop      %6llu ns/iter  (harness cost)\n",
	       (unsigned long long)((b - a) / N));

	return sink == 12345 ? 1 : 0;
}
