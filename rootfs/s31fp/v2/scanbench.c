/* scanbench <elf>... - cost of scan2.h over a binary's executable segment
 * WITHOUT running it: reads the file, finds PF_X PT_LOADs, times the scan
 * (cold read and scan reported separately). Test-only. */
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "scan2.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static unsigned hits[NSIG];
static void hit(void *c, unsigned s, uint8_t *a) { (void)c; (void)a; hits[s]++; }
int main(int argc, char **argv)
{
	scan_init();
	for (int f = 1; f < argc; f++) {
		struct stat st; int fd = open(argv[f], O_RDONLY);
		if (fd < 0 || fstat(fd, &st) < 0) { perror(argv[f]); continue; }
		uint8_t *buf = malloc(st.st_size);
		double t0 = now();
		ssize_t n = read(fd, buf, st.st_size); close(fd);
		double t1 = now(), ts = 0; size_t tl = 0; unsigned nh = 0;
		Elf32_Ehdr *eh = (Elf32_Ehdr *)buf;
		for (int i = 0; i < eh->e_phnum && n > 0; i++) {
			Elf32_Phdr *ph = (Elf32_Phdr *)(buf + eh->e_phoff + i * eh->e_phentsize);
			if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
			double a = now();
			nh += scan_seg(buf + ph->p_offset, ph->p_filesz, hit, 0);
			ts += now() - a; tl += ph->p_filesz;
		}
		printf("%s: text %zu B, read %.2f ms, scan %.2f ms (%.1f ns/B), %u routines found:",
		       argv[f], tl, t1 - t0, ts, ts * 1e6 / (tl ? tl : 1), nh);
		for (unsigned s = 0; s < NSIG; s++) if (hits[s]) printf(" %s", s31fp_sigs[s].name + 2);
		printf("\n");
		for (unsigned s = 0; s < NSIG; s++) hits[s] = 0;
		free(buf);
	}
	return 0;
}
