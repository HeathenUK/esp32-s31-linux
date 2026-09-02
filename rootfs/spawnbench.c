/* Where does a 56 ms process spawn actually go: fork, or exec? */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 20;
	const char *prog = argc > 2 ? argv[2] : "/bin/true";
	double t0, t1;
	int i, st;

	t0 = now();
	for (i = 0; i < n; i++) {
		pid_t p = fork();
		if (p == 0) _exit(0);
		waitpid(p, &st, 0);
	}
	t1 = now();
	printf("fork+exit      : %.1f ms each\n", (t1 - t0) / n);

	t0 = now();
	for (i = 0; i < n; i++) {
		pid_t p = vfork();
		if (p == 0) _exit(0);
		waitpid(p, &st, 0);
	}
	t1 = now();
	printf("vfork+exit     : %.1f ms each\n", (t1 - t0) / n);

	t0 = now();
	for (i = 0; i < n; i++) {
		pid_t p = fork();
		if (p == 0) { execl(prog, prog, (char *)NULL); _exit(1); }
		waitpid(p, &st, 0);
	}
	t1 = now();
	printf("fork+exec %-6s: %.1f ms each\n", prog, (t1 - t0) / n);

	t0 = now();
	for (i = 0; i < n; i++) {
		pid_t p = vfork();
		if (p == 0) { execl(prog, prog, (char *)NULL); _exit(1); }
		waitpid(p, &st, 0);
	}
	t1 = now();
	printf("vfork+exec     : %.1f ms each\n", (t1 - t0) / n);
	return 0;
}
