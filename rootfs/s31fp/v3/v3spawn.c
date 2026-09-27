/* v3spawn N prog [args] - exec cost: posix_spawn + wait, N times, with the
 * current environment (so LD_PRELOAD/S31* set by the caller apply); prints
 * mean and median microseconds per launch. A C loop, not a shell loop, so
 * the harness's own forks do not dominate (busybox applets fork). */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static long long us(void) { struct timespec t; syscall(SYS_clock_gettime64, CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static int cmp(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }
int main(int argc, char **argv)
{
	int n = argc > 2 ? atoi(argv[1]) : 0, st;
	long long *t = calloc(n + 1, sizeof(*t)), sum = 0;
	if (n < 1) { fprintf(stderr, "usage: v3spawn N prog [args]\n"); return 2; }
	for (int i = 0; i < n; i++) {
		pid_t p; long long t0 = us();
		if (posix_spawn(&p, argv[2], 0, 0, argv + 2, environ)) { perror("spawn"); return 1; }
		waitpid(p, &st, 0);
		t[i] = us() - t0; sum += t[i];
	}
	qsort(t, n, sizeof(*t), cmp);
	printf("SPAWN %s: n %d mean %lld us median %lld us min %lld max %lld\n", argv[2], n, sum / n, t[n / 2], t[0], t[n - 1]);
	return 0;
}
