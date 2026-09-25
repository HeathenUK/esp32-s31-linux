// SPDX-License-Identifier: GPL-2.0-only
/*
 * mmcka - does a bare command keep the microSD card out of its idle state?
 *
 *   mmcka <period_ms> <count> [dev]     CMD13 (SEND_STATUS) every period_ms
 *
 * Found 2026-09-24 (paging plan item 5): a 4 KiB read that follows >= ~6-12 ms
 * of card idle pays ~5.8 ms of card access time (sdtrace c2d 5.8 ms against
 * 0.2-0.3 ms back to back; CMD17 512 B the same, polling on or off, CPUs
 * busy or idle - so it is the card, not the host). A 4 KiB READ every 3 ms
 * keeps it fast. This tool asks whether a data-less CMD13 does as well,
 * through the stock MMC_IOC_CMD ioctl (block.c), which is the cheapest
 * thing a driver-side keepalive could send. Prints the status word of the
 * first and last response and the ioctl round trip.
 */
#include <fcntl.h>
#include <linux/mmc/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MMC_RSP_PRESENT	(1 << 0)
#define MMC_RSP_CRC	(1 << 2)
#define MMC_RSP_OPCODE	(1 << 4)
#define MMC_CMD_AC	(0 << 5)
#define MMC_RSP_R1	(MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	int period = argc > 1 ? atoi(argv[1]) : 3;
	int count = argc > 2 ? atoi(argv[2]) : 1000;
	const char *dev = argc > 3 ? argv[3] : "/dev/mmcblk0";
	unsigned int rca = 0;
	double sum = 0, mx = 0;
	FILE *f = fopen("/sys/block/mmcblk0/device/rca", "r");
	int fd, i;

	if (!f || fscanf(f, "%x", &rca) != 1) {
		fprintf(stderr, "cannot read rca\n");
		return 1;
	}
	fclose(f);
	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	for (i = 0; i < count; i++) {
		struct mmc_ioc_cmd c;
		double t0, t1;

		memset(&c, 0, sizeof(c));
		c.opcode = 13;
		c.arg = rca << 16;
		c.flags = MMC_RSP_R1 | MMC_CMD_AC;
		t0 = now_ms();
		if (ioctl(fd, MMC_IOC_CMD, &c)) {
			perror("MMC_IOC_CMD");
			return 1;
		}
		t1 = now_ms();
		sum += t1 - t0;
		if (t1 - t0 > mx)
			mx = t1 - t0;
		if (i == 0 || i == count - 1)
			printf("cmd13 #%d status 0x%08x\n", i, c.response[0]);
		usleep(period * 1000);
	}
	printf("mmcka rca 0x%04x n=%d avg %.3f max %.3f ms\n", rca, count, sum / count, mx);
	return 0;
}
