// SPDX-License-Identifier: GPL-2.0-only
/*
 * Write single bytes to an I2C device register.
 *
 * BusyBox here has no i2c applets and the rootfs has no amixer, so this is how
 * codec registers get set by hand during bring-up. Deliberately free of stdio:
 * it has to be small enough to paste down a 115200 console.
 *
 *   i2cpoke /dev/i2c-0 0x10 0x46 0xbf
 */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv)
{
	unsigned char buf[2];
	int fd;

	if (argc != 5)
		return 2;

	fd = open(argv[1], O_RDWR);
	if (fd < 0)
		return 3;
	if (ioctl(fd, I2C_SLAVE_FORCE, (int)strtol(argv[2], NULL, 0)) < 0)
		return 4;

	buf[0] = strtol(argv[3], NULL, 0);
	buf[1] = strtol(argv[4], NULL, 0);
	if (write(fd, buf, 2) != 2)
		return 5;
	return 0;
}
