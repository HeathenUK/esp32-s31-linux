// SPDX-License-Identifier: GPL-2.0-only
/*
 * Restart a Goodix GT1x touch controller over I2C alone.
 *
 * On this board neither the touch reset nor its interrupt is wired to the SoC
 * (Espressif's own BSP marks both GPIO_NUM_NC), so the usual ways to start the
 * controller are unavailable. Rewriting the configuration block with the
 * "config fresh" flag is the remaining pure-I2C path, and is what the kernel's
 * goodix driver does in goodix_send_cfg().
 *
 *   gtcfg           read and verify only, change nothing
 *   gtcfg --write   rewrite the same config with the fresh flag set
 *
 * Success is visible without touching the panel: a controller that has restarted
 * reports the real resolution at 0x8146 instead of nonsense.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#define ADDR		0x14
#define REG_CONFIG	0x8050
#define REG_ID		0x8140
#define REG_RES		0x8146
/*
 * 239, not the 240 the kernel's goodix driver uses for the gt1x family. Solved
 * from the controller itself: of every length between 60 and 240, exactly one
 * makes the stored checksum match a checksum computed over the block, and that
 * is 239 - checksum at bytes 236..237, config_fresh at 238.
 */
#define CFG_LEN		239

static int fd;

/*
 * Chunked: this controller silently truncates a long read, which showed up as a
 * config block that was real for the first 16 bytes and zeroes after - and hence
 * a checksum that could never match.
 */
#define CHUNK 16

static int reg_read(uint16_t reg, uint8_t *buf, int len)
{
	int done = 0;

	while (done < len) {
		int n = len - done < CHUNK ? len - done : CHUNK;
		uint16_t r = reg + done;
		uint8_t a[2] = { r >> 8, r & 0xff };

		if (write(fd, a, 2) != 2)
			return -1;
		if (read(fd, buf + done, n) != n)
			return -1;
		done += n;
	}
	return 0;
}

static int reg_write(uint16_t reg, const uint8_t *buf, int len)
{
	int done = 0;

	while (done < len) {
		int n = len - done < CHUNK ? len - done : CHUNK;
		uint16_t r = reg + done;
		uint8_t out[2 + CHUNK];

		out[0] = r >> 8;
		out[1] = r & 0xff;
		memcpy(out + 2, buf + done, n);
		if (write(fd, out, n + 2) != n + 2)
			return -1;
		done += n;
	}
	return 0;
}

/* goodix_calc_cfg_checksum_16(): two's complement of the big-endian word sum. */
static uint16_t cfg_checksum(const uint8_t *cfg, int raw_len)
{
	uint16_t sum = 0;
	int i;

	for (i = 0; i < raw_len; i += 2)
		sum += (uint16_t)((cfg[i] << 8) | cfg[i + 1]);
	return (uint16_t)(~sum + 1);
}

int main(int argc, char **argv)
{
	uint8_t cfg[CFG_LEN], res[4], id[11];
	int raw = CFG_LEN - 3;
	uint16_t stored, calc;
	int do_write = argc > 1 && !strcmp(argv[1], "--write");

	fd = open("/dev/i2c-0", O_RDWR);
	if (fd < 0 || ioctl(fd, I2C_SLAVE, ADDR) < 0) {
		perror("i2c");
		return 1;
	}
	if (reg_read(REG_ID, id, sizeof(id))) {
		perror("read id");
		return 1;
	}
	printf("id=%.4s fw=%02x%02x sensor=%02x\n", id, id[5], id[4], id[10]);

	if (reg_read(REG_RES, res, 4)) {
		perror("read res");
		return 1;
	}
	printf("runtime resolution before: %ux%u\n",
	       res[0] | (res[1] << 8), res[2] | (res[3] << 8));

	if (reg_read(REG_CONFIG, cfg, CFG_LEN)) {
		perror("read cfg");
		return 1;
	}
	stored = (uint16_t)((cfg[raw] << 8) | cfg[raw + 1]);
	calc = cfg_checksum(cfg, raw);
	printf("config ver=%u %ux%u points=%u checksum stored=%04x calc=%04x %s\n",
	       cfg[0], cfg[1] | (cfg[2] << 8), cfg[3] | (cfg[4] << 8), cfg[5],
	       stored, calc, stored == calc ? "MATCH" : "MISMATCH");

	if (!do_write) {
		puts("read-only; pass --write to rewrite with the fresh flag");
		return 0;
	}
	/*
	 * Refuse to write if the checksum algorithm does not reproduce what the
	 * controller already stores - that would mean writing a block it will
	 * reject, and a bad config is only recoverable by power cycling.
	 */
	if (stored != calc) {
		puts("refusing to write: checksum algorithm does not match stored config");
		return 1;
	}
	cfg[raw] = calc >> 8;
	cfg[raw + 1] = calc & 0xff;
	cfg[raw + 2] = 1;			/* config_fresh */
	if (reg_write(REG_CONFIG, cfg, CFG_LEN)) {
		perror("write cfg");
		return 1;
	}
	puts("config rewritten with fresh flag");
	sleep(1);
	if (!reg_read(REG_RES, res, 4))
		printf("runtime resolution after:  %ux%u\n",
		       res[0] | (res[1] << 8), res[2] | (res[3] << 8));
	return 0;
}
