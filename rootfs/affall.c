/*
 * affall <hexmask> - set the CPU affinity of every thread on the board.
 *
 * For experiments on a WARM SMP board: with the default PIE policy every
 * process ends up with affinity {0} (the boot shell is narrowed at its first
 * PIE trap and fork() inherits it), so a runtime change of
 * esp32s31_pie_bounce has nothing to act on until the masks are widened again.
 * `affall 3` does that. Kernel threads that refuse (per-CPU kthreads) are
 * skipped. Not for init scripts - boot with PIE_BOUNCE=N instead.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	unsigned long mask = argc > 1 ? strtoul(argv[1], NULL, 16) : 3;
	int ok = 0, bad = 0, c;
	struct dirent *p, *t;
	cpu_set_t set;
	char path[64];
	DIR *proc = opendir("/proc"), *tasks;

	CPU_ZERO(&set);
	for (c = 0; c < 8; c++)
		if (mask & (1UL << c))
			CPU_SET(c, &set);
	while (proc && (p = readdir(proc))) {
		if (p->d_name[0] < '0' || p->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/task", p->d_name);
		tasks = opendir(path);
		while (tasks && (t = readdir(tasks))) {
			if (t->d_name[0] < '0' || t->d_name[0] > '9')
				continue;
			if (sched_setaffinity(atoi(t->d_name), sizeof(set), &set))
				bad++;
			else
				ok++;
		}
		if (tasks)
			closedir(tasks);
	}
	printf("affall %lx: %d threads set, %d refused\n", mask, ok, bad);
	return 0;
}
