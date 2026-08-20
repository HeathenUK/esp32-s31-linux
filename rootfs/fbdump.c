// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copy the live scanout buffer out of /dev/mem so it can be looked at.
 *
 * dd cannot do this: the kernel refuses read() on RAM through /dev/mem, while
 * mmap() of the same range is allowed.
 *
 * Note the buffer to dump is the one the hardware is scanning out, which is not
 * necessarily the one the compositor last rendered into - the driver currently
 * reports "update wants 0x50c00000 but scanout is at 0x50d00000".
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long base = argc > 1 ? strtoul(argv[1], NULL, 0) : 0x50d00000UL;
	size_t len = argc > 2 ? strtoul(argv[2], NULL, 0) : 800 * 480 * 2;
	int fd = open("/dev/mem", O_RDONLY);
	void *p;

	if (fd < 0) { perror("open /dev/mem"); return 1; }
	p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, base);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	if (write(STDOUT_FILENO, p, len) != (ssize_t)len) {
		perror("write");
		return 1;
	}
	return 0;
}
