// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copy the live scanout buffer out of /dev/mem so it can be looked at.
 *
 * dd cannot do this: the kernel refuses read() on RAM through /dev/mem, while
 * mmap() of the same range is allowed.
 *
 * With no arguments the address comes from the driver, which is the only way
 * to be right: the buffer is not at a fixed place. Without scaling it follows
 * the compositor's page flips, and with scaling it is allocated from CMA and
 * moves with the memory map. Hardcoding 0x50c00000 or 0x50d00000 used to work
 * and now reads unrelated memory - which looks like a corrupted frame rather
 * than a mistake, so it is worth failing loudly instead.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define LCD_DEBUGFS "/sys/kernel/debug/esp32s31_lcd/updates"

/* Pull "scanout=0x... size=..." out of the driver's debugfs line. */
static int query_scanout(unsigned long *base, size_t *len)
{
	char buf[512];
	char *p;
	FILE *f = fopen(LCD_DEBUGFS, "r");

	if (!f)
		return -1;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	p = strstr(buf, "scanout=");
	if (!p)
		return -1;		/* driver too old to publish it */
	*base = strtoul(p + 8, NULL, 0);
	p = strstr(buf, "size=");
	if (p)
		*len = strtoul(p + 5, NULL, 0);
	return *base ? 0 : -2;	/* -2: published, but nothing scanning out yet */
}

int main(int argc, char **argv)
{
	unsigned long base = 0;
	size_t len = 800 * 480 * 2;

	if (argc > 1) {
		base = strtoul(argv[1], NULL, 0);
		if (argc > 2)
			len = strtoul(argv[2], NULL, 0);
	} else {
		int rc = query_scanout(&base, &len);

		if (rc == -2) {
			fprintf(stderr,
				"fbdump: nothing is scanning out yet - start a display client first\n");
			return 1;
		}
		if (rc) {
			fprintf(stderr,
				"fbdump: cannot read %s - mount debugfs, or pass an address\n",
				LCD_DEBUGFS);
			return 1;
		}
	}
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
