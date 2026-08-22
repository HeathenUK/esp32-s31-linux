// SPDX-License-Identifier: GPL-2.0-only
/*
 * Does an mmap write to /dev/fb0 reach the display?
 *
 * This exists because "the cursor does not move" turned out to have nothing to
 * do with the cursor. X draws into the framebuffer through a shared mapping,
 * and the pixels do land - reading /dev/fb0 back shows them - but the panel
 * never updates, because nothing turns those writes into a DRM commit.
 *
 * The fbdev emulation is supposed to do that via deferred I/O: userspace faults
 * on a clean page, the page joins a dirty list, a timer fires, and the driver's
 * ->dirty callback damages the plane. A write() to the same device commits
 * correctly, so the damage path itself works - which narrows it to the mmap
 * half.
 *
 * The interesting question is not "does it commit" but "does it commit MORE
 * THAN ONCE". If deferred I/O cannot re-protect the pages after a flush -
 * page_mkclean() does nothing for a VM_PFNMAP mapping, which is how DMA memory
 * is usually mapped to userspace - then the first write commits and every one
 * after it is silent. That is indistinguishable, from userspace, from a
 * compositor that has stopped drawing.
 *
 * So: write, wait, sample the driver's counter, repeat. A healthy result is one
 * commit per round. One commit in round 1 and nothing afterwards is the bug.
 *
 * Usage: fbdefio [rounds] [ms_between]
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LCD_DEBUGFS "/sys/kernel/debug/esp32s31_lcd/updates"

static unsigned int updates(void)
{
	char line[512];
	unsigned int v = 0;
	FILE *f = fopen(LCD_DEBUGFS, "r");

	if (!f)
		return 0;
	if (fgets(line, sizeof(line), f))
		sscanf(line, "updates=%u", &v);
	fclose(f);
	return v;
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 6;
	int gap_ms = argc > 2 ? atoi(argv[2]) : 300;
	/* Big enough to span several pages; the exact geometry does not matter
	 * because we only care whether a write is noticed, not what it looks
	 * like. */
	size_t len = 640 * 384 * 2;
	unsigned char *p;
	int fd, i, committed = 0;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("/dev/fb0");
		return 1;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	printf("round  updates  delta  verdict\n");
	for (i = 0; i < rounds; i++) {
		unsigned int a = updates(), b;
		size_t off;

		/* Touch one byte in each of several distinct pages, so this
		 * cannot be mistaken for a single-page special case. */
		for (off = 0; off < len; off += 4096 * 7)
			p[off] = (unsigned char)(i * 37 + off);

		usleep(gap_ms * 1000);
		b = updates();
		if (b > a)
			committed++;
		printf("  %-4d %8u %6u  %s\n", i + 1, b, b - a,
		       b > a ? "committed" : "SILENT");
		fflush(stdout);
	}

	munmap(p, len);
	close(fd);

	printf("\n%d of %d rounds committed\n", committed, rounds);
	if (committed == 0)
		printf("mmap writes never reach the panel: deferred I/O is not running\n");
	else if (committed < rounds)
		printf("only the first write(s) commit: deferred I/O cannot re-arm,\n"
		       "which is what page_mkclean() failing on a VM_PFNMAP mapping looks like\n");
	else
		printf("deferred I/O is healthy; look elsewhere for the missing damage\n");
	return 0;
}
