// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-request SD latency, the honest way.
 *
 * Two things invalidated the earlier dd-based numbers on this board:
 *
 *   - busybox dd may not honour iflag=direct, so "O_DIRECT" reads were served
 *     partly from the page cache and from readahead. That produced a marginal
 *     rate of ~70 MB/s between 4k and 64k requests, which was rejected at the
 *     time as impossible against an assumed 20 MB/s ceiling (40 MHz, 4 bits,
 *     SDR). That ceiling is WRONG and the rejection was right for the wrong
 *     reason. Measured 2026-09-04 with this tool: 512 KiB requests sustain
 *     38.7 MB/s, and - the part that rules out the card's own prefetch - the
 *     RANDOM figure is identical to the sequential one (38.65/38.74/38.75 vs
 *     38.33 MB/s). So the bus carries about twice what "40 MHz SDR 4-bit"
 *     allows; it is either DDR or a faster clock than dmesg's "Bus speed"
 *     line suggests. /sys/kernel/debug/mmc0/ios would say which, and DIAG=0
 *     compiles it out. Do not re-derive a ceiling from the dmesg clock.
 *   - every measurement was sequential, while swap and program startup are
 *     random. Sequential reads get the card's own prefetch for free.
 *
 * This opens the block device with O_DIRECT explicitly, verifies the flag took,
 * and reports the distribution rather than a mean, because the tail is what an
 * interactive workload feels.
 *
 * Usage: sdlat <device> [request_kb] [count] [seq|rand]
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/mmcblk0";
	size_t kb = argc > 2 ? (size_t)atoi(argv[2]) : 4;
	int count = argc > 3 ? atoi(argv[3]) : 200;
	int rnd = argc > 4 && !strcmp(argv[4], "rand");
	size_t len = kb * 1024;
	unsigned long long devsz = 0;
	void *buf;
	double *lat;
	int fd, i, flags;
	unsigned long long span;

	fd = open(dev, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror("open O_DIRECT");
		return 1;
	}
	flags = fcntl(fd, F_GETFL);
	if (!(flags & O_DIRECT)) {
		fprintf(stderr, "O_DIRECT did not stick - numbers would be cache\n");
		return 1;
	}
	if (ioctl(fd, BLKGETSIZE64, &devsz) || devsz == 0) {
		fprintf(stderr, "cannot size %s\n", dev);
		return 1;
	}
	if (posix_memalign(&buf, 4096, len)) {
		perror("posix_memalign");
		return 1;
	}
	lat = malloc(count * sizeof(*lat));
	if (!lat)
		return 1;

	/* Spread random offsets over the whole card so the card's own cache
	 * and any readahead cannot serve them. */
	span = devsz / len;

	for (i = 0; i < count; i++) {
		unsigned long long blk = rnd
			? ((unsigned long long)rand() * 2654435761ULL) % span
			: (unsigned long long)i;
		double t0, t1;

		if (lseek(fd, blk * len, SEEK_SET) < 0) {
			perror("lseek");
			return 1;
		}
		t0 = now_ms();
		if (read(fd, buf, len) != (ssize_t)len) {
			perror("read");
			return 1;
		}
		t1 = now_ms();
		lat[i] = t1 - t0;
	}

	qsort(lat, count, sizeof(*lat), cmp);
	printf("%s %zuk %s n=%d: min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f ms",
	       dev, kb, rnd ? "rand" : "seq", count,
	       lat[0], lat[count / 2], lat[count * 9 / 10],
	       lat[count * 99 / 100], lat[count - 1]);
	printf("   -> %.2f MB/s at p50\n", (double)len / 1024.0 / 1024.0 /
	       (lat[count / 2] / 1000.0));
	free(lat);
	free(buf);
	close(fd);
	return 0;
}
