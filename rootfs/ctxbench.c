// SPDX-License-Identifier: GPL-2.0-only
/*
 * What does a context switch cost here?
 *
 * Traps are ~1.4 us and register reads ~0.15 us, both ordinary, yet an
 * interrupt appears to cost ~95 us. Every USB interrupt wakes a kworker, so if
 * switching is expensive that is where the time goes.
 *
 * Two processes ping-pong a byte through a pipe. Each round trip is two
 * switches plus four small syscalls, and the syscall cost is known and
 * subtracted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	const long n = 5000;
	int a[2], b[2];
	char c = 'x';
	double t0, t1;
	pid_t pid;

	if (pipe(a) || pipe(b)) { perror("pipe"); return 1; }

	pid = fork();
	if (pid < 0) { perror("fork"); return 1; }

	if (pid == 0) {
		for (long i = 0; i < n; i++) {
			if (read(a[0], &c, 1) != 1) _exit(1);
			if (write(b[1], &c, 1) != 1) _exit(1);
		}
		_exit(0);
	}

	t0 = now();
	for (long i = 0; i < n; i++) {
		if (write(a[1], &c, 1) != 1) break;
		if (read(b[0], &c, 1) != 1) break;
	}
	t1 = now();
	waitpid(pid, NULL, 0);

	printf("%ld round trips in %.3f s = %.2f us per round trip\n",
	       n, t1 - t0, (t1 - t0) * 1e6 / n);
	printf("  a round trip is 2 context switches + 4 syscalls (~1.4 us each)\n");
	printf("  so roughly %.2f us per context switch\n",
	       ((t1 - t0) * 1e6 / n - 4 * 1.4) / 2);
	return 0;
}
