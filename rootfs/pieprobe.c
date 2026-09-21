/*
 * pieprobe - how PIE-bound is a running task?  (docs/smp-finish-plan.md, step 3)
 *
 *   pieprobe <tid> <trials> [timeout_ms]
 *
 * PIE (the vector unit) exists only on hart 1 = CPU0.  When a task executes a
 * PIE instruction on the lent CPU (CPU1) the kernel narrows its affinity to
 * CPU0 (esp32s31_ext_lent_cpu_illegal).  That narrowing is visible from
 * outside, so no kernel change is needed to measure it:
 *
 *   each trial: note the task's CPU time, confine it to CPU1, and poll its
 *   affinity every millisecond until the kernel has thrown it back to CPU0.
 *
 * Two numbers per trial: wall ms until the trap, and the CPU time the task got
 * on CPU1 before it (clock ticks from /proc - coarse, but it separates "slept
 * the whole time" from "ran 200 ms without touching PIE").  A trial that hits
 * the timeout means the task ran (or slept) that long on CPU1 with no PIE.
 *
 * The task's original affinity is restored at the end.  A measurement, by hand,
 * on a warm board - never from an init script.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long cpu_ticks(int tid)
{
	char path[64], buf[512], *p;
	unsigned long ut = 0, st = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", tid, tid);
	f = fopen(path, "r");
	if (!f) {
		snprintf(path, sizeof(path), "/proc/%d/stat", tid);
		f = fopen(path, "r");
	}
	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	p = strrchr(buf, ')');		/* comm may contain spaces */
	if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
		return -1;
	return (long)(ut + st);
}

static long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	int tid, trials, timeout_ms, i, trapped = 0;
	cpu_set_t orig, cpu1, cur;
	struct timespec ms = { 0, 1000000 };

	if (argc < 3) {
		fprintf(stderr, "usage: pieprobe <tid> <trials> [timeout_ms]\n");
		return 2;
	}
	tid = atoi(argv[1]);
	trials = atoi(argv[2]);
	timeout_ms = argc > 3 ? atoi(argv[3]) : 2000;

	if (sched_getaffinity(tid, sizeof(orig), &orig)) {
		perror("sched_getaffinity");
		return 1;
	}
	CPU_ZERO(&cpu1);
	CPU_SET(1, &cpu1);
	printf("pieprobe tid=%d orig_mask=%s%s tick_hz=%ld\n", tid,
	       CPU_ISSET(0, &orig) ? "0" : "", CPU_ISSET(1, &orig) ? "1" : "",
	       sysconf(_SC_CLK_TCK));

	for (i = 0; i < trials; i++) {
		long t0, c0 = cpu_ticks(tid), c1;
		int hit = 0;

		if (sched_setaffinity(tid, sizeof(cpu1), &cpu1)) {
			perror("sched_setaffinity");
			break;
		}
		t0 = now_us();
		while (now_us() - t0 < timeout_ms * 1000L) {
			nanosleep(&ms, NULL);
			if (sched_getaffinity(tid, sizeof(cur), &cur))
				break;
			if (CPU_ISSET(0, &cur) && !CPU_ISSET(1, &cur)) {
				hit = 1;
				break;
			}
		}
		c1 = cpu_ticks(tid);
		trapped += hit;
		printf("trial %d: %s after %ld ms wall, %ld cpu ticks on CPU1\n", i,
		       hit ? "PIE trap" : "NO trap", (now_us() - t0) / 1000, c1 - c0);
		if (!hit)	/* still confined to CPU1: let it go before the next trial */
			sched_setaffinity(tid, sizeof(orig), &orig);
		nanosleep(&(struct timespec){ 0, 50000000 }, NULL);
	}
	sched_setaffinity(tid, sizeof(orig), &orig);
	printf("pieprobe tid=%d: %d of %d trials trapped\n", tid, trapped, i);
	return 0;
}
