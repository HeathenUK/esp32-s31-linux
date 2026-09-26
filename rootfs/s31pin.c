/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * s31pin - the board has no taskset (busybox built without it).
 *   s31pin -p MASK PID     set the affinity of every thread of PID
 *   s31pin MASK CMD ARGS   exec CMD under MASK (bit 0 = CPU0 = hart 1)
 * Used by the GL profiling runs: the hart0 PC sampler only sees hart 1
 * (CPU0), so a process has to be pinned there to be profiled whole.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void mask_of(unsigned long m, cpu_set_t *s)
{
	int i;

	CPU_ZERO(s);
	for (i = 0; i < 32; i++)
		if (m & (1ul << i))
			CPU_SET(i, s);
}

int main(int argc, char **argv)
{
	cpu_set_t s;

	if (argc >= 4 && argv[1][0] == '-' && argv[1][1] == 'p') {
		char path[64];
		DIR *d;
		struct dirent *e;
		int n = 0;

		mask_of(strtoul(argv[2], NULL, 0), &s);
		snprintf(path, sizeof(path), "/proc/%s/task", argv[3]);
		if (!(d = opendir(path))) { perror(path); return 1; }
		while ((e = readdir(d)))
			if (e->d_name[0] != '.' &&
			    sched_setaffinity(atoi(e->d_name), sizeof(s), &s) == 0)
				n++;
		closedir(d);
		printf("s31pin: %d thread(s) of %s -> 0x%lx\n", n, argv[3],
		       strtoul(argv[2], NULL, 0));
		return n ? 0 : 1;
	}
	if (argc >= 3) {
		mask_of(strtoul(argv[1], NULL, 0), &s);
		if (sched_setaffinity(0, sizeof(s), &s)) { perror("affinity"); return 1; }
		execvp(argv[2], argv + 2);
		perror(argv[2]);
		return 127;
	}
	fprintf(stderr, "usage: s31pin -p MASK PID | s31pin MASK CMD [ARGS]\n");
	return 2;
}
