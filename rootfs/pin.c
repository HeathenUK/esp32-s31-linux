/* pin <cpu> <cmd...>  - run a command with its CPU affinity fixed.
 * pin -p <cpu> <pid>  - move a running process (every thread) to a CPU.
 * busybox here has no taskset, and the hart0 PC sampler (h1s) sees hart 1 =
 * CPU0 only, so a process that wanders onto CPU1 is invisible to it. */
#define _GNU_SOURCE
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	cpu_set_t s;
	if (argc >= 4 && !strcmp(argv[1], "-p")) {
		char path[64]; DIR *d; struct dirent *e; int n = 0;
		CPU_ZERO(&s); CPU_SET(atoi(argv[2]), &s);
		snprintf(path, sizeof path, "/proc/%s/task", argv[3]);
		d = opendir(path);
		if (!d) { perror(path); return 1; }
		while ((e = readdir(d)))
			if (e->d_name[0] != '.') {
				if (sched_setaffinity(atoi(e->d_name), sizeof s, &s)) perror("sched_setaffinity");
				else n++;
			}
		closedir(d);
		printf("pinned %d threads of %s to cpu %s\n", n, argv[3], argv[2]);
		return n ? 0 : 1;
	}
	if (argc < 3) { fprintf(stderr, "usage: pin <cpu> <cmd...> | pin -p <cpu> <pid>\n"); return 2; }
	CPU_ZERO(&s); CPU_SET(atoi(argv[1]), &s);
	if (sched_setaffinity(0, sizeof s, &s)) { perror("sched_setaffinity"); return 1; }
	execvp(argv[2], argv + 2); perror("exec"); return 1;
}
