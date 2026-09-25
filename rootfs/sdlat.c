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
 * Usage: sdlat <device> [request_size] [count] [seq|rand|fixed] [gap_ms]
 *
 * fixed re-reads block 0 every time (a keepalive that never leaves one
 * sector: does the card count a cached re-read as activity?).
 *
 * gap_ms (default 0 = back to back) sleeps between reads, so the tool can
 * SAMPLE the read latency of a loaded card instead of becoming its load:
 * back to back at ~2-5 ms a read it would issue 200-500 reads/s against the
 * ~70/s a -mem 20 Quake faults in (paging plan item 5, 2026-09-24).
 *
 * request_size is KiB by default ("4", "4k"); a 'b' suffix means BYTES
 * ("512b", "1024b"). The bytes form exists to price one SD command leg by
 * contrast (docs/sd-paging-programme-2026-09-24.md, A2): the mmc block
 * layer issues a 512 B request as CMD17 (READ_SINGLE_BLOCK, no stop) and a
 * 1024 B request as CMD18 (READ_MULTIPLE_BLOCK) + a SOFTWARE CMD12 from the
 * dw_mmc BH (block.c:1700-1712 picks the opcode on data.blocks > 1; dw_mmc
 * never sets MMC_CAP_CMD23, so the stop is never folded into a CMD23). So
 *
 *     min(1024b) - min(512b) - ~13 us of extra wire
 *
 * is the whole cost of the stop leg: one more CMD write, one more IRQ, one
 * more BH pass. O_DIRECT needs a 512-multiple, so the smallest legal
 * request is 512b; the buffer is 4096-aligned whatever the size.
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

/*
 * "4" or "4k" -> 4096; "512b" -> 512; "1m" -> 1048576. Sets *bytes_form so
 * the report line says "512B" rather than "0k" and old "4k" logs still
 * parse the same way.
 */
static size_t parse_size(const char *s, int *bytes_form)
{
	char *end;
	unsigned long v = strtoul(s, &end, 10);

	*bytes_form = 0;
	if (end == s)
		return 0;
	switch (*end) {
	case 'b':
	case 'B':
		*bytes_form = 1;
		return v;
	case 'm':
	case 'M':
		return v * 1024 * 1024;
	case 'k':
	case 'K':
	case '\0':
		return v * 1024;
	default:
		return 0;
	}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/mmcblk0";
	int count = argc > 3 ? atoi(argv[3]) : 200;
	int rnd = argc > 4 && !strcmp(argv[4], "rand");
	int fixed = argc > 4 && !strcmp(argv[4], "fixed");
	int gap_ms = argc > 5 ? atoi(argv[5]) : 0;
	int bytes_form = 0;
	size_t len = argc > 2 ? parse_size(argv[2], &bytes_form) : 4096;
	unsigned long long devsz = 0;
	void *buf;
	double *lat;
	int fd, i, flags;
	unsigned long long span;

	if (!len || (len & 511)) {
		fprintf(stderr,
			"bad request size '%s': KiB ('4', '4k'), MiB ('1m') or bytes ('512b'), a multiple of 512\n",
			argc > 2 ? argv[2] : "");
		return 1;
	}
	if (count < 1) {
		fprintf(stderr, "bad count\n");
		return 1;
	}

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
			: fixed ? 0ULL : (unsigned long long)i;
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
		if (gap_ms > 0)
			usleep(gap_ms * 1000);
	}

	qsort(lat, count, sizeof(*lat), cmp);
	if (bytes_form)
		printf("%s %zuB %s n=%d:", dev, len, rnd ? "rand" : "seq", count);
	else
		printf("%s %zuk %s n=%d:", dev, len / 1024, rnd ? "rand" : "seq",
		       count);
	printf(" min %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f ms",
	       lat[0], lat[count / 2], lat[count * 9 / 10],
	       lat[count * 99 / 100], lat[count - 1]);
	printf("   -> %.2f MB/s at p50\n", (double)len / 1024.0 / 1024.0 /
	       (lat[count / 2] / 1000.0));
	free(lat);
	free(buf);
	close(fd);
	return 0;
}
