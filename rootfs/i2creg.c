// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read or write one I2C device register.
 *
 * BusyBox here has no i2c applets, so this stands in for i2cget/i2cset while
 * bringing up the audio codec.
 *
 *   i2creg /dev/i2c-0 0x10 0x02        -> prints the register value
 *   i2creg /dev/i2c-0 0x10 0x02 0x40   -> writes it
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv)
{
	struct i2c_rdwr_ioctl_data work;
	struct i2c_msg msg[2];
	unsigned char reg, val;
	int fd, addr;

	if (argc < 4)
		return 2;
	addr = strtol(argv[2], NULL, 0);
	reg = strtol(argv[3], NULL, 0);

	fd = open(argv[1], O_RDWR);
	if (fd < 0)
		return 3;

	if (argc >= 5) {
		unsigned char buf[2] = { reg, (unsigned char)strtol(argv[4], NULL, 0) };

		if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0)
			return 4;
		if (write(fd, buf, 2) != 2)
			return 5;
		return 0;
	}

	msg[0].addr = addr; msg[0].flags = 0;        msg[0].len = 1; msg[0].buf = &reg;
	msg[1].addr = addr; msg[1].flags = I2C_M_RD; msg[1].len = 1; msg[1].buf = &val;
	work.msgs = msg; work.nmsgs = 2;
	if (ioctl(fd, I2C_RDWR, &work) < 0)
		return 6;
	printf("%02x\n", val);
	return 0;
}
