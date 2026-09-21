/*
 * oncpu <hexmask> <command> [args...] - run a command on chosen CPUs.
 *
 * The board has no taskset (busybox is built without it), which is why every
 * placement question here has been answered by luck-of-the-boot instead of by
 * experiment. Affinity is inherited across fork and exec, so this covers a
 * whole process tree: `oncpu 1 sdlbench1 ...` keeps the client and everything
 * it spawns on CPU0.
 *
 * Without a command it reads or writes an existing task's mask instead:
 *   oncpu -p <pid>            print "<pid> <hexmask>"
 *   oncpu -s <hexmask> <tid>  set one task's (or thread's) mask
 *
 * Masks are hex, like taskset: 1 = CPU0, 2 = CPU1, 3 = either.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	cpu_set_t set;
	unsigned long mask;
	int i;

	if (argc >= 4 && argv[1][0] == '-' && argv[1][1] == 's') {
		int tid = atoi(argv[3]);

		mask = strtoul(argv[2], NULL, 16);
		CPU_ZERO(&set);
		for (i = 0; i < 8; i++)
			if (mask & (1UL << i))
				CPU_SET(i, &set);
		if (sched_setaffinity(tid, sizeof(set), &set)) {
			perror("sched_setaffinity");
			return 1;
		}
		return 0;
	}
	if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'p') {
		int pid = atoi(argv[2]);

		if (sched_getaffinity(pid, sizeof(set), &set)) {
			perror("sched_getaffinity");
			return 1;
		}
		mask = 0;
		for (i = 0; i < 8; i++)
			if (CPU_ISSET(i, &set))
				mask |= 1UL << i;
		printf("%d %lx\n", pid, mask);
		return 0;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: oncpu <hexmask> <command> [args...]\n"
				"       oncpu -p <pid>            print its mask\n"
				"       oncpu -s <hexmask> <tid>  set one task's mask\n");
		return 2;
	}
	mask = strtoul(argv[1], NULL, 16);
	CPU_ZERO(&set);
	for (i = 0; i < 8; i++)
		if (mask & (1UL << i))
			CPU_SET(i, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		perror("sched_setaffinity");
		return 1;
	}
	execvp(argv[2], argv + 2);
	perror(argv[2]);
	return 127;
}
