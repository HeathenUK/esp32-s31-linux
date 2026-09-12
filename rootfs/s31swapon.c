/*
 * s31swapon - swapon(2) with a priority, which busybox cannot do.
 *
 * WHY THIS EXISTS. The point of zram here is to be the FIRST place a page
 * goes, with the microSD swapfile as overflow: compressing a page into PSRAM
 * costs CPU and no I/O, while the card costs a 2.49 ms request each way. That
 * ordering is expressed as swap priority, and busybox's swapon takes only
 * [-a] [-e] [DEVICE] - no -p, and no fstab "pri=" either.
 *
 * The init script assumed the kernel would assign descending priorities in
 * swapon order and that enabling zram first would therefore make it primary.
 * It does not work out that way in practice: with both devices enabled,
 * /proc/swaps showed BOTH at priority -1, and equal priority means the kernel
 * round-robins between them. So half the swap traffic was going to the card
 * anyway, and the tiering the comment described had never happened.
 *
 *   s31swapon <device|file> [priority]
 *
 * Priority is 0..32767, higher is preferred, default 100. Omit it and you get
 * an explicit priority anyway, which is the whole point - never leave it to
 * the default.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/swap.h>

int main(int argc, char **argv)
{
	int prio = argc > 2 ? atoi(argv[2]) : 100;

	if (argc < 2) {
		fprintf(stderr, "usage: s31swapon <device|file> [priority]\n");
		return 1;
	}
	if (prio < 0 || prio > SWAP_FLAG_PRIO_MASK) {
		fprintf(stderr, "s31swapon: priority must be 0..%d\n",
			SWAP_FLAG_PRIO_MASK);
		return 1;
	}
	if (swapon(argv[1], SWAP_FLAG_PREFER |
		   (prio << SWAP_FLAG_PRIO_SHIFT)) < 0) {
		fprintf(stderr, "s31swapon: %s: %s\n", argv[1],
			strerror(errno));
		return 1;
	}
	printf("s31swapon: %s at priority %d\n", argv[1], prio);
	return 0;
}
