/*
 * icgeo - the S31 L1 I-cache geometry, measured on the hart it runs on.
 * GL phase 6 tier 6. s31, MIT.
 *
 *   icgeo <codefile> [cpu]     (default CPU0 = hart 1; CPU1 = hart 0)
 *
 * <codefile> is created by the first run's "gen" pass (icgeo -g FILE) and
 * must then be dropped from the page cache, so that the pages it runs from
 * are filled by SD DMA exactly as program text is. (The first version wrote
 * the jump chain with CPU stores into anonymous RWX pages and executed it:
 * that WEDGED THE WHOLE BOARD, hart0 included, on the first pass (no
 * console, no hart0 post-mortem; reset.py recovered it). Most likely the
 * I-cache refilled lines still dirty in the shared write-back D-cache - not
 * established. Do not execute CPU-written code on this SoC this way.)
 *
 * Every 64-byte line of the file holds the same three instructions,
 *   lw t0,0(a0); addi a0,a0,4; jr t0
 * so a chain is just a table of line addresses in data memory, and the code
 * is never written. The chain is run R times and the I-bus next-level reads
 * (line refills) counted with the L1 access counters
 * (CACHE_L1_CACHE_ACS_CNT_CTRL_REG 0x2C000180, IBUS0 nxtlvl_rd 0x2C000190 =
 * hart 0, IBUS1 0x2C0001a0 = hart 1; memory s31-cache-counters). A chain that
 * fits reads ~0 refills per line per pass, one that thrashes ~1.
 *   size:   k file-contiguous pages (whatever colours they got), all 64 lines
 *   same:   k pages with equal PA bits 12-14 (one colour under any geometry)
 *   b13:    3 pages, PA bit 12 equal, bit 13 = 0,0,1 (fits iff bit 13 indexes)
 *   b12:    4 pages, PA bit 12 = 0,0,1,1 with equal VA bit 12 (fits iff the
 *           index is physical and bit 12 is in it)
 *   va12:   3 pages with equal VA bit 12 but PA bit 12 = 0,0,1 (fits iff the
 *           index is physical: under a virtual index they would thrash)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NP 96
#define PG 4096
static uint8_t *base;
static uint32_t pa[NP];
static volatile uint32_t *cc;
static int bus_off;

static void stop(void) { }
static uint32_t tab[16 * 64 + 1];

static double run(const int *pg, int k, int nl, const char *tag)
{
	int n = 0;
	for (int l = 0; l < nl; l++)
		for (int p = 0; p < k; p++)
			tab[n++] = (uint32_t)(uintptr_t)(base + pg[p] * PG + l * 64);
	tab[n] = (uint32_t)(uintptr_t)stop;
	void (*f)(uint32_t *) = (void (*)(uint32_t *))(uintptr_t)tab[0];
	int R = 4000;
	for (int i = 0; i < 50; i++) f(tab + 1);
	cc[0x180 / 4] = 0x00330033; cc[0x180 / 4] = 0x33;
	uint32_t a0 = cc[bus_off / 4];
	for (int i = 0; i < R; i++) f(tab + 1);
	uint32_t a1 = cc[bus_off / 4];
	cc[0x180 / 4] = 0;
	double r = (double)(a1 - a0) / R / (k * nl);
	printf("%-5s k=%2d lines=%4d refills/line/pass %.3f  pages", tag, k, k * nl, r);
	for (int p = 0; p < k; p++)
		printf(" v%x/p%x", (unsigned)((uintptr_t)(base + pg[p] * PG) >> 12) & 7, (pa[pg[p]] >> 12) & 7);
	printf("\n");
	return r;
}

/* pick k pages matching pred, excluding used; returns count found */
static int pick(int *out, int k, int (*pred)(int i, int n, const int *sel))
{
	int n = 0;
	for (int i = 0; i < NP && n < k; i++)
		if (pred(i, n, out)) out[n++] = i;
	return n;
}
static int p_same(int i, int n, const int *s) { return n == 0 || ((pa[i] ^ pa[s[0]]) & 0x7000) == 0; }
static int p_b13(int i, int n, const int *s)
{
	if (n == 0) return 1;
	uint32_t d = pa[i] ^ pa[s[0]];
	if (d & 0x1000) return 0;
	return n < 2 ? !(d & 0x2000) : !!(d & 0x2000);
}
static int p_va12(int i, int n, const int *s)
{
	if (n == 0) return 1;
	uintptr_t dv = ((uintptr_t)(base + i * PG) ^ (uintptr_t)(base + s[0] * PG)) & 0x1000;
	uint32_t d = (pa[i] ^ pa[s[0]]) & 0x3000;
	if (dv) return 0;
	return n < 2 ? !d : d == 0x1000;
}
static int p_b12(int i, int n, const int *s)
{
	if (n == 0) return 1;
	uintptr_t dv = ((uintptr_t)(base + i * PG) ^ (uintptr_t)(base + s[0] * PG)) & 0x1000;
	uint32_t d = (pa[i] ^ pa[s[0]]) & 0x1000;
	if (dv) return 0;
	return n < 2 ? !d : !!d;
}

int main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "-g")) {
		static uint32_t line[16];
		for (int i = 0; i < 16; i++) line[i] = 0x00000013;	/* nop */
		line[0] = 0x00052283; line[1] = 0x00450513; line[2] = 0x00028067;
		FILE *o = fopen(argv[2], "wb");
		for (int i = 0; i < NP * PG / 64; i++) fwrite(line, 64, 1, o);
		fclose(o);
		return 0;
	}
	if (argc < 2) { fprintf(stderr, "icgeo -g FILE | icgeo FILE [cpu]\n"); return 2; }
	int cpu = argc > 2 ? atoi(argv[2]) : 0;
	cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs);
	if (sched_setaffinity(0, sizeof cs, &cs)) { perror("affinity"); return 1; }
	/* Linux CPU0 = hart 1 = IBUS1; CPU1 = hart 0 = IBUS0 */
	bus_off = cpu == 0 ? 0x1a0 : 0x190;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	cc = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x2C000000);
	if (cc == MAP_FAILED) { perror("devmem"); return 1; }
	printf("ctrl before 0x%08x cpu %d bus 0x%x\n", cc[0x180 / 4], cpu, bus_off);
	int cf = open(argv[1], O_RDONLY);
	if (cf < 0) { perror(argv[1]); return 1; }
	base = mmap(0, NP * PG, PROT_READ | PROT_EXEC, MAP_PRIVATE, cf, 0);
	if (base == MAP_FAILED) { perror("mmap"); return 1; }
	volatile uint32_t sink = 0;
	for (int i = 0; i < NP; i++) sink += *(volatile uint32_t *)(base + i * PG);
	if (mlock(base, NP * PG)) perror("mlock");
	int pm = open("/proc/self/pagemap", O_RDONLY);
	for (int i = 0; i < NP; i++) {
		uint64_t e = 0;
		pread(pm, &e, 8, ((uintptr_t)(base + i * PG) >> 12) * 8);
		if (!(e >> 63)) { printf("page %d not present\n", i); return 1; }
		pa[i] = (uint32_t)((e & ((1ULL << 55) - 1)) << 12);
	}
	int pg[16];
	for (int k = 1; k <= 12; k++) {
		for (int p = 0; p < k; p++) pg[p] = p;
		run(pg, k, 64, "size");
	}
	for (int k = 2; k <= 5; k++)
		if (pick(pg, k, p_same) == k) run(pg, k, 64, "same");
	if (pick(pg, 3, p_b13) == 3) run(pg, 3, 64, "b13");
	if (pick(pg, 4, p_b12) == 4) run(pg, 4, 64, "b12");
	if (pick(pg, 3, p_va12) == 3) run(pg, 3, 64, "va12");
	/* the same test repeated: noise */
	for (int p = 0; p < 4; p++) pg[p] = p;
	run(pg, 4, 64, "size");
	return 0;
}
