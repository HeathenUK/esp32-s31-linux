// SPDX-License-Identifier: GPL-2.0-only
/*
 * Process switch versus thread switch.
 *
 * A context switch here costs ~435 us with USB and the coprocessor hook both
 * out of the way, against 2-5 us on comparable hardware. Threads in one process
 * share an address space, so switching between them skips switch_mm and the TLB
 * work; processes do not. If threads are cheap and processes are not, the cost
 * is the address-space switch.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int a[2], b[2];
static const long n = 3000;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void *echo_side(void *arg)
{
	char c;

	(void)arg;
	for (long i = 0; i < n; i++) {
		if (read(a[0], &c, 1) != 1) break;
		if (write(b[1], &c, 1) != 1) break;
	}
	return NULL;
}

static double ping(void)
{
	char c = 'x';
	double t0 = now();

	for (long i = 0; i < n; i++) {
		if (write(a[1], &c, 1) != 1) break;
		if (read(b[0], &c, 1) != 1) break;
	}
	return (now() - t0) * 1e6 / n;
}

int main(void)
{
	pthread_t th;
	pid_t pid;
	double us;

	if (pipe(a) || pipe(b)) { perror("pipe"); return 1; }
	pthread_create(&th, NULL, echo_side, NULL);
	us = ping();
	pthread_join(th, NULL);
	printf("thread  round trip: %8.2f us  (no address-space switch)\n", us);

	if (pipe(a) || pipe(b)) { perror("pipe"); return 1; }
	pid = fork();
	if (pid == 0) { echo_side(NULL); _exit(0); }
	us = ping();
	waitpid(pid, NULL, 0);
	printf("process round trip: %8.2f us  (switch_mm on every switch)\n", us);
	return 0;
}
