// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pin a process's own CODE in RAM: LD_PRELOAD this and its constructor walks
 * /proc/self/maps and mlock()s every executable, file-backed mapping - the
 * binary and its libraries. Never the heap, stack or data: mlockall() on
 * lvdesk measured 2x worse input lag by pinning client pixmaps.
 *
 * Why: at ~1 MB free a game's text pages get evicted and refetched from the
 * SD card at a 2.5 ms floor per fault - Quake took 1,000-1,450 major faults
 * per demo. Locking ~450 KB of text removes those without XIP.
 *
 * LOCKTEXT_VERBOSE=1 reports what was locked to stderr.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

__attribute__((constructor)) static void locktext_init(void)
{
	char line[512];
	unsigned long lo, hi, locked = 0, failed = 0;
	char perm[8], path[400];
	FILE *f = fopen("/proc/self/maps", "r");
	int verbose = getenv("LOCKTEXT_VERBOSE") != NULL;

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399s", &lo, &hi, perm, path) < 3)
			continue;
		if (perm[2] != 'x' || path[0] != '/')
			continue;
		if (mlock((void *)lo, hi - lo) == 0)
			locked += hi - lo;
		else
			failed += hi - lo;
		if (verbose)
			fprintf(stderr, "locktext: %s %lx-%lx %s\n", path, lo, hi,
				perm);
	}
	fclose(f);
	if (verbose)
		fprintf(stderr, "locktext: locked %lu KB, failed %lu KB\n",
			locked / 1024, failed / 1024);
}
