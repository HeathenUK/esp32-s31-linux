/*
 * h1s - take one capture with the hart0 PC sampler and print it.
 *
 *   h1s <s31_h1s_ctrl> <s31_h1s_buf> [samples<=4000] > pcs.txt
 *
 * The two addresses come from `nm hello_world.elf | grep s31_h1s_` and move
 * with every loader build (bootloader/main/s31_vcpu.c, "H1 PC SAMPLER").
 * One hex PC per line, the format scripts/board/h1s-report.py reads.
 *
 * Why a tool: dumping 4000 words with a devmem loop took ~100 ms a word on a
 * loaded board (2026-09-21) - six minutes of forks perturbing the very
 * workload being profiled. This maps the SRAM once; the capture itself costs
 * hart 1 nothing, and the dump is one write.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long ctrl_pa, buf_pa, base, span;
	volatile uint32_t *ctrl, *buf;
	unsigned int n, i, waited = 0;
	uint8_t *map;
	int fd;

	if (argc < 3) {
		fprintf(stderr, "usage: h1s <ctrl_addr> <buf_addr> [samples]\n");
		return 2;
	}
	ctrl_pa = strtoul(argv[1], NULL, 0);
	buf_pa = strtoul(argv[2], NULL, 0);
	n = argc > 3 ? atoi(argv[3]) : 4000;
	if (n < 1 || n > 4000)
		n = 4000;
	base = (buf_pa < ctrl_pa ? buf_pa : ctrl_pa) & ~0xfffUL;
	span = ((buf_pa > ctrl_pa ? buf_pa + 16000 : ctrl_pa + 4) - base + 0xfff) & ~0xfffUL;

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	map = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	ctrl = (volatile uint32_t *)(map + (ctrl_pa - base));
	buf = (volatile uint32_t *)(map + (buf_pa - base));

	*ctrl = n;
	while (!(*ctrl & 0x80000000u)) {
		usleep(100000);
		if (++waited > 100) {		/* 10 s for a 4 s capture */
			fprintf(stderr, "h1s: sampler never finished (ctrl=%08x) - wrong addresses for this loader?\n", *ctrl);
			return 1;
		}
	}
	n = *ctrl & 0xffff;
	for (i = 0; i < n; i++)
		printf("%08x\n", buf[i]);
	return 0;
}
