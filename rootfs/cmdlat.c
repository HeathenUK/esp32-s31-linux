// SPDX-License-Identifier: GPL-2.0-only
/*
 * How long does a command with NO data payload take?
 *
 * This board spends ~2 ms between writing the SDMMC command register and
 * seeing CMD_DONE, while the data phase runs at full bus speed. That has
 * survived every explanation tried: it is not the card (a different card moved
 * it 14%), not Linux's interrupt delivery (a 0.5 ms poll of MINTSTS does not
 * see it earlier), not clock gating, not the BIU or CIU clocks (80 and 40 MHz,
 * both correct), and not interrupt loss.
 *
 * CMD13 (SEND_STATUS) is addressed to the card, gets an R1 response, and moves
 * no data at all. So it isolates the command path completely:
 *
 *   ~2 ms   the controller's command state machine is the whole story, and
 *           nothing about data transfer or DMA is involved
 *   fast    something specific to DATA commands stalls before the command is
 *           issued, and the command path itself is fine
 *
 * ANSWER, measured on this board: fast. CMD13 p50 0.845 ms, min 0.672, for a
 * full round trip through userspace, the mmc core, the driver and the card. A
 * 4k data read on the same boot is p50 3.239 ms. The command path is not slow;
 * the cost appears only when DAT_EXP is set.
 *
 * Note when comparing the two: CMD13 goes through the ioctl path and data
 * reads go through blk-mq, so the 0.845 ms is an upper bound on the command
 * itself and includes software the data path does not share. That makes the
 * conclusion stronger, not weaker - the command is even cheaper than it looks.
 *
 * Usage: cmdlat <device> [count]
 */

#include <fcntl.h>
#include <linux/mmc/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef MMC_RSP_PRESENT
#define MMC_RSP_PRESENT	(1 << 0)
#define MMC_RSP_CRC	(1 << 2)
#define MMC_RSP_OPCODE	(1 << 4)
#define MMC_CMD_AC	(0 << 5)
#define MMC_RSP_SPI_S1	(1 << 7)
#define MMC_RSP_R1	(MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#endif

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
	int count = argc > 2 ? atoi(argv[2]) : 200;
	struct mmc_ioc_cmd c;
	double *lat;
	int fd, i, ok = 0;
	unsigned int rca = 0;

	/*
	 * CMD13 is addressed, so it needs the card's relative address in the
	 * top 16 bits of the argument. The mmc core does NOT fill this in for
	 * MMC_IOC_CMD - passing zero returns -110 (timeout), because the card
	 * simply is not being spoken to. The RCA is the hex suffix of the
	 * device name, e.g. mmc0:d555.
	 */
	{
		FILE *f = popen("ls -d /sys/class/mmc_host/mmc0/mmc0:* 2>/dev/null", "r");
		char path[256];

		if (f && fgets(path, sizeof(path), f)) {
			char *colon = strrchr(path, ':');

			if (colon)
				rca = (unsigned int)strtoul(colon + 1, NULL, 16);
		}
		if (f)
			pclose(f);
		if (!rca) {
			fprintf(stderr, "cannot determine card RCA\n");
			return 1;
		}
		printf("card RCA = 0x%04x\n", rca);
	}

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	lat = malloc(count * sizeof(*lat));
	if (!lat)
		return 1;

	for (i = 0; i < count; i++) {
		double t0, t1;

		memset(&c, 0, sizeof(c));
		c.opcode = 13;			/* SEND_STATUS */
		c.arg = rca << 16;
		c.flags = MMC_RSP_R1 | MMC_CMD_AC;
		c.blksz = 0;
		c.blocks = 0;

		t0 = now_ms();
		if (ioctl(fd, MMC_IOC_CMD, &c) < 0) {
			if (i == 0) {
				perror("MMC_IOC_CMD");
				fprintf(stderr,
					"  (needs CONFIG_MMC_BLOCK_MINORS/ioctl support)\n");
				return 1;
			}
			continue;
		}
		t1 = now_ms();
		lat[ok++] = t1 - t0;
	}

	if (!ok) {
		fprintf(stderr, "no commands completed\n");
		return 1;
	}
	qsort(lat, ok, sizeof(*lat), cmp);
	printf("CMD13 (no data) n=%d: min %.3f  p50 %.3f  p90 %.3f  max %.3f ms\n",
	       ok, lat[0], lat[ok / 2], lat[ok * 9 / 10], lat[ok - 1]);
	free(lat);
	close(fd);
	return 0;
}
