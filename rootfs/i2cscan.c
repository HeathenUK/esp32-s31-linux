// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal I2C bus scanner.
 *
 * BusyBox here is built without the i2c applets, so this stands in for
 * i2cdetect: it walks the 7-bit address space and reports which addresses
 * acknowledge. A one-byte read is used rather than a zero-length write
 * because some devices ignore the latter.
 *
 *   i2cscan /dev/i2c-0
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/i2c-0";
	int fd, addr, found = 0;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	for (addr = 0; addr < 0x80; addr++) {
		struct i2c_rdwr_ioctl_data work;
		struct i2c_msg msg;
		unsigned char byte = 0;

		if ((addr & 0x0f) == 0)
			printf("%02x: ", addr);

		/* Reserved ranges are never probed, as on i2cdetect. */
		if (addr < 0x03 || addr > 0x77) {
			printf("   ");
			goto eol;
		}

		msg.addr = addr;
		msg.flags = I2C_M_RD;
		msg.len = 1;
		msg.buf = &byte;
		work.msgs = &msg;
		work.nmsgs = 1;

		if (ioctl(fd, I2C_RDWR, &work) >= 0) {
			printf("%02x ", addr);
			found++;
		} else {
			printf("-- ");
		}
eol:
		if ((addr & 0x0f) == 0x0f)
			printf("\n");
	}

	printf("%d device(s) responded\n", found);
	close(fd);
	return 0;
}
