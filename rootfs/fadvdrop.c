/*
 * fadvdrop - drop the page cache for named files.
 *
 * WHY. On this board a file that an application has ALSO copied into its own
 * memory is held twice in PSRAM, and PSRAM is the binding constraint: 15.4 MB
 * total, of which 4.2 MB is unreclaimable slab and 6 MB is reserved for the
 * display.
 *
 * prboom with level_precache=1 copies WAD lumps into its heap, and the
 * kernel keeps its own page-cache copy of the same bytes.  doom1.wad is
 * 4,196,020 bytes - a quarter of the machine, duplicated. That duplication
 * competes for the memory whose shortage is causing the ~450 major faults per
 * timedemo, which are the frame dips.
 *
 * POSIX_FADV_DONTNEED drops CLEAN page cache for a range. It is per-file, not
 * per-descriptor, so this works from outside the application entirely - no
 * preload, no modification to prboom, nothing that knows what a WAD is.
 *
 * WHAT IT CANNOT DO. Dirty pages are not dropped (nothing here writes the
 * WAD), and pages another process has mapped are not dropped either. If the
 * application reads the file again it faults it back in, so dropping the
 * cache of a file that is still being read is a pessimisation - the whole
 * case rests on precache having already taken its copy.
 *
 *   fadvdrop [-v] [-n secs] <file> [file...]
 *
 *     -n secs   keep going, dropping every `secs` seconds, until killed.
 *               A long-running game touches more of the WAD as it goes, so
 *               one shot at the start is not the same as staying on top of it.
 *     -v        report Cached before and after, from /proc/meminfo
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long cached_kb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char k[64];
	long v, out = -1;

	while (f && fscanf(f, "%63s %ld kB\n", k, &v) == 2) {
		if (!strcmp(k, "Cached:")) {
			out = v;
			break;
		}
	}
	if (f)
		fclose(f);
	return out;
}

static int drop(const char *path, int verbose)
{
	int fd = open(path, O_RDONLY);
	int err;

	if (fd < 0) {
		fprintf(stderr, "fadvdrop: %s: cannot open\n", path);
		return -1;
	}
	/*
	 * Length 0 means "to the end of the file", which is what we want and
	 * saves a stat.
	 */
	err = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	close(fd);
	if (err) {
		fprintf(stderr, "fadvdrop: %s: fadvise failed (%d)\n", path,
			err);
		return -1;
	}
	if (verbose)
		fprintf(stderr, "fadvdrop: dropped %s\n", path);
	return 0;
}

int main(int argc, char **argv)
{
	int verbose = 0, every = 0, i, first;

	for (first = 1; first < argc && argv[first][0] == '-'; first++) {
		if (!strcmp(argv[first], "-v"))
			verbose = 1;
		else if (!strcmp(argv[first], "-n") && first + 1 < argc)
			every = atoi(argv[++first]);
		else {
			fprintf(stderr, "usage: fadvdrop [-v] [-n secs] "
				"<file>...\n");
			return 1;
		}
	}
	if (first >= argc) {
		fprintf(stderr, "usage: fadvdrop [-v] [-n secs] <file>...\n");
		return 1;
	}
	for (;;) {
		long before = verbose ? cached_kb() : -1;

		for (i = first; i < argc; i++)
			drop(argv[i], verbose > 1);
		if (verbose) {
			long after = cached_kb();

			fprintf(stderr, "fadvdrop: Cached %ld -> %ld kB "
				"(freed %ld kB)\n", before, after,
				before - after);
		}
		if (every <= 0)
			break;
		sleep((unsigned)every);
	}
	return 0;
}
