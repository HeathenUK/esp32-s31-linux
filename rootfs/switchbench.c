/*
 * switchbench - decompose the cost of getting from one process to another.
 *
 * ctxbench ping-pongs a byte through a pipe and divides by two. That number
 * (~590-1290 us here, against 2-5 us on comparable hardware) is the board's
 * largest unexplained cost, but it is a SUM: two syscalls, the pipe's own
 * machinery, a wakeup, and the switch itself. Optimising against a sum is how
 * the last attribution here got retracted.
 *
 * Four arms, each measuring one more layer than the last:
 *
 *   A  bare syscall          getpid() through syscall(), never cached
 *   B  self-pipe             write+read in ONE process: syscalls + pipe, and
 *                            provably no context switch
 *   C  sched_yield           two runnable processes yielding: switches with
 *                            almost no syscall payload
 *   D  pipe ping-pong        everything, i.e. what ctxbench measures
 *
 * B - A is the pipe's cost. C is the switch. D - B should then be the wakeup
 * plus the switch, and if D - B is far larger than C the cost is in the
 * WAKEUP path, not in switching.
 *
 * Every arm also reports the context switches the kernel actually counted, so
 * the denominator is measured rather than assumed - arm C in particular is a
 * lie if the scheduler declines to switch.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static long long now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Total switches the kernel has performed, both kinds, for this process. */
static long self_switches(void)
{
	char buf[4096];
	long v = 0, n = 0;
	FILE *f = fopen("/proc/self/status", "r");

	if (!f)
		return 0;
	while (fgets(buf, sizeof(buf), f)) {
		if (!strncmp(buf, "voluntary_ctxt_switches:", 24))
			v = atol(buf + 24);
		else if (!strncmp(buf, "nonvoluntary_ctxt_switches:", 27))
			n = atol(buf + 27);
	}
	fclose(f);
	return v + n;
}

static void report(const char *tag, long long ns, int iters, long sw)
{
	printf("  %-16s %8.2f us/op   switches=%-8ld %6.2f us/switch\n",
	       tag, (double)ns / iters / 1000.0, sw,
	       sw ? (double)ns / sw / 1000.0 : 0.0);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 2000;
	long long t0;
	long s0;
	int i;
	int p[2], a[2], b[2];
	char c = 'x';

	printf("switchbench: %d iterations per arm\n", iters);

	/* A: bare syscall, no pipe, no switch. */
	s0 = self_switches();
	t0 = now_ns();
	for (i = 0; i < iters; i++)
		syscall(SYS_getpid);
	report("A syscall", now_ns() - t0, iters, self_switches() - s0);

	/* B: the pipe's own cost, still one process. */
	if (pipe(p)) { perror("pipe"); return 1; }
	s0 = self_switches();
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		if (write(p[1], &c, 1) != 1) break;
		if (read(p[0], &c, 1) != 1) break;
	}
	report("B self-pipe", now_ns() - t0, iters, self_switches() - s0);
	close(p[0]); close(p[1]);

	/* C: switches with almost no payload. Needs a second runnable task. */
	{
		pid_t k = fork();

		if (k == 0) {
			for (;;)
				sched_yield();
			_exit(0);
		}
		s0 = self_switches();
		t0 = now_ns();
		for (i = 0; i < iters; i++)
			sched_yield();
		report("C sched_yield", now_ns() - t0, iters,
		       self_switches() - s0);
		kill(k, SIGKILL);
		waitpid(k, NULL, 0);
	}

	/* D: the full round trip, one switch each way. */
	if (pipe(a) || pipe(b)) { perror("pipe"); return 1; }
	{
		pid_t k = fork();

		if (k == 0) {
			for (;;) {
				if (read(a[0], &c, 1) != 1) _exit(0);
				if (write(b[1], &c, 1) != 1) _exit(0);
			}
		}
		s0 = self_switches();
		t0 = now_ns();
		for (i = 0; i < iters; i++) {
			if (write(a[1], &c, 1) != 1) break;
			if (read(b[0], &c, 1) != 1) break;
		}
		report("D ping-pong", now_ns() - t0, iters,
		       self_switches() - s0);
		kill(k, SIGKILL);
		waitpid(k, NULL, 0);
	}
	return 0;
}
