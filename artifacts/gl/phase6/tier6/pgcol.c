/*
 * pgcol <pid> <substr>... - the I-cache page colours of a process's
 * resident executable pages. GL phase 6 tier 6. s31, MIT.
 *
 * For every r-xp mapping of <pid> whose path contains one of <substr>,
 * prints one line: "PC <name> <file offset of mapping> <colours>" where
 * <colours> has one character per page of the mapping: 0-3 = PA bits 12-13
 * of the resident frame (the hart-1 I-cache colour: 32 kB, 2-way, 16 kB a
 * way, physically indexed - measured with icgeo), '.' = not resident.
 * Then "PH <name> n0 n1 n2 n3" - resident pages per colour.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char p[64], l[512];
	if (argc < 3) return 2;
	snprintf(p, sizeof p, "/proc/%s/maps", argv[1]);
	FILE *m = fopen(p, "r");
	snprintf(p, sizeof p, "/proc/%s/pagemap", argv[1]);
	int pm = open(p, O_RDONLY);
	if (!m || pm < 0) { perror(p); return 1; }
	while (fgets(l, sizeof l, m)) {
		unsigned long s, e, off; char perm[8], path[400] = "";
		if (sscanf(l, "%lx-%lx %7s %lx %*s %*s %399s", &s, &e, perm, &off, path) < 4) continue;
		if (strcmp(perm, "r-xp")) continue;
		int hit = 0;
		for (int i = 2; i < argc; i++) if (strstr(path, argv[i])) hit = 1;
		if (!hit) continue;
		const char *nm = strrchr(path, '/'); nm = nm ? nm + 1 : path;
		int h[4] = {0};
		printf("PC %s 0x%lx ", nm, off);
		for (unsigned long a = s; a < e; a += 4096) {
			uint64_t v = 0;
			pread(pm, &v, 8, (a >> 12) * 8);
			if (v >> 63) { int c = (int)(v & 3); h[c]++; putchar('0' + c); }
			else putchar('.');
		}
		printf("\nPH %s %d %d %d %d\n", nm, h[0], h[1], h[2], h[3]);
	}
	return 0;
}
