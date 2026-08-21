// SPDX-License-Identifier: GPL-2.0-only
/*
 * Is a file on the cramfs XIP mount actually mapped in place?
 *
 * cramfs with a linear image and an MTD that implements ->_point() can map a
 * file's pages straight out of the flash window. Those pages are not page
 * cache: they never count in RSS, never age, and never refault. If instead the
 * mapping falls back to ordinary page cache, the pages are evictable and cost
 * RAM - which is the difference between XIP paying for itself and not.
 *
 * Maps the file PROT_READ|PROT_EXEC, touches every page, then prints the
 * kernel's own accounting for that VMA.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long addr, lo, hi;
	char line[512];
	struct stat st;
	volatile unsigned sum = 0;
	unsigned char *p;
	size_t i;
	FILE *f;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: xipmap <file>\n");
		return 1;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0 || fstat(fd, &st)) {
		perror("open");
		return 1;
	}
	p = mmap(NULL, st.st_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	/* fault every page in */
	for (i = 0; i < (size_t)st.st_size; i += 4096)
		sum += p[i];

	addr = (unsigned long)p;
	printf("%s: %ld bytes mapped at %lx (checksum %u)\n",
	       argv[1], (long)st.st_size, addr, sum);

	f = fopen("/proc/self/smaps", "r");
	if (!f)
		return 1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
			if (addr >= lo && addr < hi) {
				fputs("  ", stdout);
				fputs(line, stdout);
				while (fgets(line, sizeof(line), f)) {
					if (strchr("0123456789abcdef", line[0]) &&
					    strstr(line, "-"))
						break;
					if (!strncmp(line, "Rss:", 4) ||
					    !strncmp(line, "Pss:", 4) ||
					    !strncmp(line, "Private_Clean:", 14) ||
					    !strncmp(line, "Shared_Clean:", 13) ||
					    !strncmp(line, "VmFlags:", 8))
						printf("  %s", line);
					if (!strncmp(line, "VmFlags:", 8))
						break;
				}
				break;
			}
		}
	}
	fclose(f);
	return 0;
}
