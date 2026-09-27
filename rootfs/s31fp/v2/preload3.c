/*
 * libs31fp.so (v2 interception) - make an UNMODIFIED program's soft-double
 * arithmetic fast, at load time, with no libc/ld.so change and no per-app
 * configuration. CANDIDATE: not installed until the board tests pass.
 *
 * Every program here was linked against the same libgcc.a, which copies its
 * soft-double routines INTO the program as local functions no symbol
 * interposition can reach. This constructor finds them in the MAIN
 * EXECUTABLE's text by their full bodies (sigs3.h), and replaces each with
 * the v2 routine (v2.S) COPIED IN PLACE over libgcc's body where it fits (13
 * of 14; see copy_len), else an entry `auipc t0 / jalr x0, t0` to it (divdf3,
 * compiled C). v2 is bit-exact to libgcc including fflags and frm - it hands every
 * case it does not decide itself to libgcc's own routine, linked into this
 * library renamed (s31lg_*).
 *
 * Launch cost (the 2026-09-20 preload cost +250 ms per exec: ten memmem()
 * passes over every loaded object):
 *   - main executable only (shared libraries are not scanned);
 *   - a per-binary cache, S31FP_CACHE (default /var/lib/s31fp - ext4, not
 *     the tmpfs that /var/cache is on this board), one small
 *     file per executable keyed by dev/inode/size/mtime, holding the patch
 *     offsets - or nothing, for a program with no double routines, so those
 *     cost a stat and an open from then on. Written atomically (tmp+rename).
 *     A hit re-verifies every body before patching;
 *   - one pass over the text on a miss, filtered on the first halfword;
 *   - no libc: raw system calls, and one symbol import (environ), so the
 *     dynamic loader has next to nothing to resolve for this library.
 *
 * Patch sequence (the RAMTEXT one, see gl/bench/t6src/tinygl/source/
 * s31_ramtext.c): the S31's I-cache does not snoop the D-cache and fence.i
 * does not write the D-cache back; the kernel's exec-PTE hook does
 * (mprotect(PROT_EXEC) -> flush_icache_pte -> D-cache writeback + I-cache
 * invalidate for a page not yet clean). So: one mprotect to RW (no X), every
 * write, one mprotect back to RX, then the fence.i syscall - and no write
 * after that. Nothing in the main executable runs before its own
 * constructors, which run after this one, so no CPU-written code executes
 * before the RX step. Any failure before the writes means no patch.
 *
 * S31FP=0 disables it. S31FP_COPY=1 enables copy-in-place (default: trampoline).
 * S31FP_COLOUR=0 skips the frame colouring.
 * S31FP_DEBUG=1 reports to stderr.
 */
#include <stddef.h>
#include <stdint.h>
#include "sigs3.h"

#define HID __attribute__((visibility("hidden")))
extern char **environ;

/* ---- raw syscalls (riscv32 Linux) ---- */
static long sc(long n, long a, long b, long c, long d, long e)
{
#ifdef S31FP_TEST_REMAP_FAIL
	/* Test-only: Linux MREMAP_FIXED may remove the destination before an
	 * allocation fails. Reproduce that destructive failure, not a harmless
	 * early error. Never enabled in a shipping build. */
	if (n == 216) { sc(215, e, c, 0, 0, 0); return -12; }
#endif
#ifdef S31FP_TEST_RX_FAIL
	if (n == 226 && c == 5) return -12;
#endif
#ifdef S31FP_TEST_RW_FAIL
	if (n == 226 && c == 3) return -12;
#endif
	register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b,
		a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e,
		a5 __asm__("a5") = 0; /* mmap2's sixth argument: page offset, not garbage */
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
	return a0;
}
#define NR_openat 56
#define NR_close 57
#define NR_read 63
#define NR_write 64
#define NR_mkdirat 34
#define NR_renameat2 276
#define NR_mprotect 226
#define NR_getpid 172
#define NR_statx 291
#define NR_riscv_flush_icache 259
#define NR_pread64 67
#define NR_mmap2 222
#define NR_munmap 215
#define NR_mremap 216
#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define O_EXCL 0200
#define PROT_R 1
#define PROT_W 2
#define PROT_X 4
#define PG 4096u

static int dbg;
/* Once a fixed remap has begun, a failed syscall may have removed executable
 * pages. Never enter the application with missing/NX/partially patched text.
 * This is unconditional, unlike debug diagnostics, and uses no libc. */
__attribute__((noreturn)) static void fatal(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	sc(NR_write, 2, (long)s, n, 0, 0);
	sc(94 /* exit_group */, 125, 0, 0, 0, 0);
	for (;;) {}
}
static void say(const char *s) { size_t n = 0; if (!dbg) return; while (s[n]) n++; sc(NR_write, 2, (long)s, n, 0, 0); }
static void sayx(const char *s, unsigned long v)
{
	char b[80]; unsigned n = 0, i;
	if (!dbg) return;
	while (*s && n < 60) b[n++] = *s++;
	b[n++] = '0'; b[n++] = 'x';
	for (i = 0; i < 8; i++) b[n++] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
	b[n++] = '\n';
	sc(NR_write, 2, (long)b, n, 0, 0);
}
static const char *env(const char *k)
{
	for (char **e = environ; e && *e; e++) {
		const char *p = *e, *q = k;
		while (*q && *p == *q) p++, q++;
		if (!*q && *p == '=') return p + 1;
	}
	return 0;
}
static int same(const unsigned char *a, const unsigned char *b, unsigned n)
{
	while (n--) if (*a++ != *b++) return 0;
	return 1;
}

/* ---- the replacements ---- */
#define V(n) HID void s31v2_##n(void); HID void s31lg_##n(void);
V(muldf3) V(adddf3) V(subdf3) V(divdf3) V(fixdfsi) V(floatsidf) V(fixunsdfsi)
V(floatunsidf) V(extendsfdf2) V(truncdfsf2) V(gedf2) V(ledf2) V(eqdf2) V(unorddf2)
/* same order as sigs3.h */
static void (*const target[S31_NSIG])(void) = { s31v2_muldf3, s31v2_adddf3, s31v2_subdf3,
	s31v2_divdf3, s31v2_fixdfsi, s31v2_floatsidf, s31v2_fixunsdfsi, s31v2_floatunsidf,
	s31v2_extendsfdf2, s31v2_truncdfsf2, s31v2_gedf2, s31v2_ledf2, s31v2_eqdf2, s31v2_unorddf2 };
/* our own renamed libgcc copies ARE libgcc bodies: never patch those */
static void (*const own[S31_NSIG])(void) = { s31lg_muldf3, s31lg_adddf3, s31lg_subdf3,
	s31lg_divdf3, s31lg_fixdfsi, s31lg_floatsidf, s31lg_fixunsdfsi, s31lg_floatunsidf,
	s31lg_extendsfdf2, s31lg_truncdfsf2, s31lg_gedf2, s31lg_ledf2, s31lg_eqdf2, s31lg_unorddf2 };

/* full body, masked words skipped */
static int body_ok(const unsigned char *at, unsigned s, const unsigned char *end)
{
	const struct s31_sig *g = &s31_sigs[s];
	unsigned pos = 0;
	if (at + g->len > end) return 0;
	for (unsigned m = 0; m <= g->nmask; m++) {
		unsigned stop = m < g->nmask ? g->mask[m] : g->len;
		if (!same(at + pos, g->body + pos, stop - pos)) return 0;
		pos = stop + 4;
	}
	for (unsigned i = 0; i < S31_NSIG; i++) {
		if ((const void *)at == (const void *)own[i]) return 0;
		/* nor the signature tables themselves (rodata can share the X segment) */
		if (at + g->len > s31_sigs[i].body && at < s31_sigs[i].body + s31_sigs[i].len) return 0;
	}
	return 1;
}

#define MAXSITE 32
static unsigned nsite, soff[MAXSITE], ssig[MAXSITE];

static void scan(const unsigned char *b, unsigned len)
{
	static uint32_t filt[128];	/* 4096-bit filter on the low 12 bits of the first halfword */
	const uint16_t *p = (const uint16_t *)b, *e = (const uint16_t *)(b + len - 60);
	for (unsigned s = 0; s < S31_NSIG; s++) {
		unsigned h = (s31_sigs[s].body[0] | s31_sigs[s].body[1] << 8) & 0xfff;
		filt[h >> 5] |= 1u << (h & 31);
	}
	if (len < 64) return;
	for (; p < e; p++) {
		unsigned h = *p, f = h & 0xfff;
		if (!(filt[f >> 5] & (1u << (f & 31)))) continue;
		for (unsigned s = 0; s < S31_NSIG; s++)
			if ((s31_sigs[s].body[0] | s31_sigs[s].body[1] << 8) == h &&
			    body_ok((const unsigned char *)p, s, b + len)) {
				if (nsite < MAXSITE) {
					soff[nsite] = (unsigned)((const unsigned char *)p - b);
					ssig[nsite++] = s;
				}
				p += (s31_sigs[s].len >> 1) - 1;
				break;
			}
	}
}

/*
 * Copy-in-place (opt-in with S31FP_COPY=1):
 * where the v2 routine fits inside libgcc's body, the routine itself is
 * copied over it, so the program calls our code directly - no auipc/jalr
 * hop, and the hot code sits in the program's own (COW) pages rather than in
 * this library, which may live in XIP flash. The only non-relative
 * references in a v2 routine are its TAILREF pairs (v2.S, recorded in
 * section s31fix); each is re-encoded for the copy so it still reaches the
 * same target in this library. A routine whose pairs do not decode as
 * auipc/jalr, or that does not fit, gets the trampoline instead.
 */
HID extern const uint32_t s31v2_copytab[2 * S31_NSIG];
HID extern const uint32_t __start_s31fix[], __stop_s31fix[];
static int copy_on;
static unsigned ncopied;

static uint32_t rd32(const uint8_t *p) { const uint16_t *h = (const uint16_t *)p; return h[0] | (uint32_t)h[1] << 16; }
static void wr32(uint8_t *p, uint32_t v) { uint16_t *h = (uint16_t *)p; h[0] = (uint16_t)v; h[1] = (uint16_t)(v >> 16); }

/* can routine s be copied? (checked before anything is written) */
static unsigned copy_len(unsigned s)
{
	uintptr_t a = s31v2_copytab[2 * s], e = s31v2_copytab[2 * s + 1];
	if (!copy_on || !a || e <= a || e - a > s31_sigs[s].len || ((e - a) & 1)) return 0;
	for (const uint32_t *f = __start_s31fix; f < __stop_s31fix; f++) {
		const uint8_t *x = (const uint8_t *)(uintptr_t)*f;
		if ((uintptr_t)x < a || (uintptr_t)x >= e) continue;
		if ((uintptr_t)x + 8 > e) return 0;
		if ((rd32(x) & 0x7f) != 0x17 || (rd32(x + 4) & 0x707f) != 0x0067) return 0;
	}
	return (unsigned)(e - a);
}

static void copy_to(uint8_t *at, unsigned s, unsigned n)
{
	const uint8_t *a = (const uint8_t *)(uintptr_t)s31v2_copytab[2 * s];
	const uint16_t *src = (const uint16_t *)a;
	uint16_t *dst = (uint16_t *)at;
	for (unsigned i = 0; i < n / 2; i++) dst[i] = src[i];
	for (const uint32_t *f = __start_s31fix; f < __stop_s31fix; f++) {
		const uint8_t *x = (const uint8_t *)(uintptr_t)*f;
		if (x < a || x >= a + n) continue;
		uint32_t au = rd32(x), jr = rd32(x + 4);
		uintptr_t tgt = (uintptr_t)x + (int32_t)(au & 0xFFFFF000u) + ((int32_t)jr >> 20);
		uint8_t *y = at + (x - a);
		int32_t rel = (int32_t)(tgt - (uintptr_t)y);
		uint32_t h20 = ((uint32_t)rel + 0x800u) & 0xFFFFF000u;
		uint32_t l12 = ((uint32_t)rel - h20) & 0xFFFu;
		wr32(y, (au & 0xFFFu) | h20);
		wr32(y + 4, (jr & 0x000FFFFFu) | (l12 << 20));
	}
}

#ifdef S31FP_COLOURDBG
static void colours(uintptr_t lo, uintptr_t hi, const char *tag)
{
	long fd = sc(NR_openat, AT_FDCWD, (long)"/proc/self/pagemap", O_RDONLY, 0, 0);
	char b[160]; unsigned n = 0;
	if (fd < 0) return;
	for (const char *t = "s31fp: colours "; *t; ) b[n++] = *t++;
	for (const char *t = tag; *t; ) b[n++] = *t++;
	for (uintptr_t a = lo; a < hi && n < 140; a += PG) {
		uint64_t e = 0;
		sc(NR_pread64, fd, (long)&e, 8, (long)((a / PG) * 8), 0);
		b[n++] = ' '; b[n++] = "0123456789abcdef"[(a >> 12) & 15]; b[n++] = ':';
		b[n++] = (e >> 63) ? "0123"[e & 3] : '-';
	}
	b[n++] = '\n';
	sc(NR_write, 2, (long)b, n, 0, 0);
	sc(NR_close, fd, 0, 0, 0, 0);
}
#endif

/*
 * Frame colouring (S31FP_COLOUR=0 disables). The hart-1 I-cache is 32 kB,
 * 2-way, PHYSICALLY indexed: a way is 16 kB, so bits 12-13 of the frame
 * number pick which quarter of the sets a page lands in. The copy-in-place
 * write COW's each patched text page into a fresh frame of random colour,
 * and measured on the board (results/board-b7.txt, 20 runs of OpenTyrian's
 * synth) that is a lottery: 15 runs 19.5-20.3 us/sample, 5 runs 22.6-31.1,
 * the slow ones exactly those where two or three of the patched pages drew
 * colour 0. So each patched page is given a frame of its ORIGINAL page-cache
 * frame's colour - the copy then occupies the same cache sets the unpatched
 * code did - taken from a small populated anonymous pool (pagemap, root) and
 * moved over the text page with mremap BEFORE anything is written: the page
 * arrives RW and not executable, holding the original bytes; the patch
 * writes follow, then the single RX mprotect does the cache maintenance
 * exactly as without colouring. Pool/pagemap failures leave normal COW.
 * A failed MREMAP_FIXED is different: Linux may already have unmapped the
 * destination. Stop explicitly rather than run damaged executable text.
 * (The same placement technique as RAMTEXT tier 6,
 * gl/bench/t6src/tinygl/source/s31_ramtext.c rc_place.)
 */
#define POOL 16
static int colour_on = 1;
static unsigned ncoloured, ncoltried;

/* colours of n consecutive pages from va, in ONE pread (-1 = unknown) */
static void frame_colours(long fd, uintptr_t va, unsigned n, int *col)
{
#ifdef S31FP_TEST_REMAP_FAIL
	/* Exercise the move in QEMU too, where real PFNs are not available. */
	for (unsigned i = 0; i < n; i++) col[i] = 0;
	return;
#endif
	uint64_t e[POOL];
	long r = sc(NR_pread64, fd, (long)e, 8 * n, (long)((va / PG) * 8), 0);
	if (r != (long)(8 * n)) {
		for (unsigned i = 0; i < n; i++) col[i] = -1;
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		uint64_t pfn = e[i] & ((1ULL << 55) - 1);
		col[i] = ((e[i] >> 63) && pfn) ? (int)(pfn & 3) : -1;
	}
}

static unsigned ncopied_planned(const unsigned *clen)
{
	unsigned n = 0;
	for (unsigned i = 0; i < nsite; i++) n += clen[i] != 0;
	return n;
}

/* the pool grows 4 pages at a time until every wanted colour is found (8
 * pages expected for 3 wanted colours), at most POOL: allocation is most of
 * this step's cost */
#define CHUNK 4
static void colour_pages(uintptr_t lo, uintptr_t hi)
{
	unsigned np = (unsigned)((hi - lo) / PG), done = 0, need = 0, nch = 0;
	int want[POOL], pcol[CHUNK];
	long fd, ch[POOL / CHUNK]; unsigned used[POOL / CHUNK];
	if (np > POOL) return;
	fd = sc(NR_openat, AT_FDCWD, (long)"/proc/self/pagemap", O_RDONLY, 0, 0);
	if (fd < 0) return;
	for (unsigned k = 0; k < np; k++) (void)*(volatile const uint32_t *)(lo + k * PG);	/* resident */
	frame_colours(fd, lo, np, want);
	for (unsigned k = 0; k < np; k++) if (want[k] >= 0) { need |= 1u << k; ncoltried++; }
	while (need & ~done && nch < POOL / CHUNK) {
		long m = sc(NR_mmap2, 0, CHUNK * PG, PROT_R | PROT_W, 0x22 | 0x8000 /* PRIVATE|ANON|POPULATE */, -1);
		if (m < 0 && m > -4096) break;
		ch[nch] = m; used[nch] = 0;
		frame_colours(fd, (uintptr_t)m, CHUNK, pcol);
		for (unsigned k = 0; k < np; k++) {
			if (!(need & ~done & (1u << k))) continue;
			for (unsigned i = 0; i < CHUNK; i++) {
				if (used[nch] & (1u << i) || pcol[i] != want[k]) continue;
				const uint32_t *src = (const uint32_t *)(lo + k * PG);
				uint32_t *dst = (uint32_t *)((uintptr_t)m + i * PG);
				for (unsigned w = 0; w < PG / 4; w++) dst[w] = src[w];
				if (sc(NR_mremap, (long)dst, PG, PG, 3 /* MAYMOVE|FIXED */, (long)(lo + k * PG)) != (long)(lo + k * PG))
					fatal("s31fp: fatal: fixed remap failed; executable may be unmapped (exit 125)\n");
				used[nch] |= 1u << i;
				done |= 1u << k;
				ncoloured++;
				break;
			}
		}
		nch++;
	}
	sc(NR_close, fd, 0, 0, 0, 0);
	for (unsigned c = 0; c < nch; c++)
		if (used[c] != (1u << CHUNK) - 1)
			sc(NR_munmap, ch[c], CHUNK * PG, 0, 0, 0);	/* moved pages are no longer in it */
}

static unsigned patch(unsigned char *b)
{
	unsigned clen[MAXSITE];
	uintptr_t lo = ~(uintptr_t)0, hi = 0;
	unsigned i;
	if (!nsite) return 0;
	for (i = 0; i < nsite; i++) {
		uintptr_t a = (uintptr_t)b + soff[i];
		clen[i] = copy_len(ssig[i]);
		if (a < lo) lo = a;
		if (a + (clen[i] ? clen[i] : 8) > hi) hi = a + (clen[i] ? clen[i] : 8);
	}
	lo &= ~(uintptr_t)(PG - 1);
	hi = (hi + PG - 1) & ~(uintptr_t)(PG - 1);
#ifdef S31FP_COLOURDBG
	colours(lo, hi, "before");
#endif
	/* Acquire write permissions before colouring changes any mapping. On a
	 * failure, restore RX in case mprotect processed only part of the range. */
	if (sc(NR_mprotect, lo, hi - lo, PROT_R | PROT_W, 0, 0)) {
		if (sc(NR_mprotect, lo, hi - lo, PROT_R | PROT_X, 0, 0))
			fatal("s31fp: fatal: could not restore executable permissions (exit 125)\n");
		say("s31fp: mprotect RW failed, nothing patched\n");
		return 0;
	}
	if (colour_on && ncopied_planned(clen))
		colour_pages(lo, hi);
	for (i = 0; i < nsite; i++) {
		uint32_t *at = (uint32_t *)(b + soff[i]);
		if (clen[i]) {
			copy_to((uint8_t *)at, ssig[i], clen[i]);
			ncopied++;
			continue;
		}
		int32_t rel = (int32_t)((uintptr_t)target[ssig[i]] - (uintptr_t)at);
		uint32_t h20 = ((uint32_t)rel + 0x800u) & 0xFFFFF000u;
		uint32_t l12 = ((uint32_t)rel - h20) & 0xFFFu;
		/* 2-byte aligned (RVC): store as halfwords */
		uint16_t *hw = (uint16_t *)at;
		uint32_t i0 = 0x00000297u | h20, i1 = 0x00028067u | (l12 << 20);
		hw[0] = (uint16_t)i0; hw[1] = (uint16_t)(i0 >> 16);
		hw[2] = (uint16_t)i1; hw[3] = (uint16_t)(i1 >> 16);
	}
	/* RX: the exec-PTE hook writes the D-cache back and invalidates the I-cache */
	if (sc(NR_mprotect, lo, hi - lo, PROT_R | PROT_X, 0, 0))
		fatal("s31fp: fatal: patched text could not become executable (exit 125)\n");
	if (sc(NR_riscv_flush_icache, lo, hi, 0, 0, 0))
		fatal("s31fp: fatal: instruction-cache flush failed (exit 125)\n");
	__asm__ volatile("fence.i" ::: "memory");
#ifdef S31FP_COLOURDBG
	colours(lo, hi, "after");
#endif
	return nsite;
}

/* main executable's text via /proc/self/auxv (AT_PHDR 3, AT_PHNUM 5) */
struct phdr { uint32_t type, off, vaddr, paddr, filesz, memsz, flags, align; };
static int main_text(unsigned char **base, unsigned *len)
{
	uint32_t av[64]; long n; uint32_t phdr = 0, phnum = 0;
	long fd = sc(NR_openat, AT_FDCWD, (long)"/proc/self/auxv", O_RDONLY, 0, 0);
	if (fd < 0) return -1;
	n = sc(NR_read, fd, (long)av, sizeof(av), 0, 0);
	sc(NR_close, fd, 0, 0, 0, 0);
	for (long i = 0; i + 1 < n / 4; i += 2) {
		if (av[i] == 3) phdr = av[i + 1];
		if (av[i] == 5) phnum = av[i + 1];
	}
	if (!phdr || !phnum) return -1;
	const struct phdr *ph = (const struct phdr *)(uintptr_t)phdr;
	uintptr_t bias = 0; int have = 0;
	for (unsigned i = 0; i < phnum; i++)
		if (ph[i].type == 6) { bias = phdr - ph[i].vaddr; have = 1; }	/* PT_PHDR */
	if (!have) {	/* no PT_PHDR (a non-PIE static test binary): bias 0 if AT_PHDR lies in a segment */
		for (unsigned i = 0; i < phnum; i++)
			if (ph[i].type == 1 && phdr >= ph[i].vaddr && phdr < ph[i].vaddr + ph[i].memsz) have = 1;
		if (!have) return -1;
	}
	for (unsigned i = 0; i < phnum; i++)
		if (ph[i].type == 1 && (ph[i].flags & 1)) {	/* PT_LOAD, PF_X */
			*base = (unsigned char *)(bias + ph[i].vaddr);
			*len = ph[i].memsz;
			return 0;
		}
	return -1;
}

/* cache key: dev, inode, size, mtime of the executable */
struct statx_min { uint32_t mask, blksize; uint64_t attributes; uint32_t nlink, uid, gid; uint16_t mode, pad0;
	uint64_t ino, size, blocks, attributes_mask; int64_t atime_s; uint32_t atime_ns, pad1;
	int64_t btime_s; uint32_t btime_ns, pad2; int64_t ctime_s; uint32_t ctime_ns, pad3;
	int64_t mtime_s; uint32_t mtime_ns, pad4; uint32_t rdev_major, rdev_minor, dev_major, dev_minor; uint64_t spare[14]; };
static char *hex(char *p, uint64_t v) { for (int i = 60; i >= 0; i -= 4) { unsigned d = (v >> i) & 15; if (d || i == 0 || p[-1] != '/') ; *p++ = "0123456789abcdef"[d]; } return p; }
static int keypath(char *buf, const char *dir)
{
	struct statx_min st;
	char *p = buf;
	if (sc(NR_statx, AT_FDCWD, (long)"/proc/self/exe", 0, 0x7ff, (long)&st)) return -1;
	while (*dir && p < buf + 150) *p++ = *dir++;
	*p++ = '/';
	p = hex(p, ((uint64_t)st.dev_major << 32) | st.dev_minor); *p++ = '-';
	p = hex(p, st.ino); *p++ = '-';
	p = hex(p, st.size); *p++ = '-';
	p = hex(p, (uint64_t)st.mtime_s ^ st.mtime_ns);
	*p = 0;
	return (int)(p - buf);
}

#define MAGIC 0x31433133u	/* "31C1" */
__attribute__((constructor)) static void s31fp_init(void)
{
	const char *e = env("S31FP"), *dir = env("S31FP_CACHE");
	unsigned char *b; unsigned len, n;
	char path[240], tmp[256];
	uint32_t rec[2 + 2 * MAXSITE];
	int kl;

	if (e && e[0] == '0') return;
	dbg = env("S31FP_DEBUG") != 0;
	/* copy-in-place is OPT-IN (S31FP_COPY=1): on 2026-09-27 one prboom
	 * timedemo died of SIGSEGV at level load under it (1 of 4 copy runs,
	 * cause not found; every trampoline run completed). Exactness and the
	 * 280-process hammer pass with it on - see V2-REPORT.txt section 10. */
	e = env("S31FP_COPY");
	copy_on = e && e[0] == '1';
	e = env("S31FP_COLOUR");
	colour_on = !(e && e[0] == '0');
	if (!dir) dir = "/var/lib/s31fp";
	if (main_text(&b, &len)) { say("s31fp: no main text\n"); return; }
	kl = dir[0] ? keypath(path, dir) : -1;
	if (kl > 0) {
		long fd = sc(NR_openat, AT_FDCWD, (long)path, O_RDONLY, 0, 0);
		if (fd >= 0) {
			long r = sc(NR_read, fd, (long)rec, sizeof(rec), 0, 0);
			sc(NR_close, fd, 0, 0, 0, 0);
			if (r >= 8 && rec[0] == MAGIC && rec[1] <= MAXSITE && r == (long)(8 + 8 * rec[1])) {
				for (unsigned i = 0; i < rec[1]; i++) {
					unsigned off = rec[2 + 2 * i], s = rec[3 + 2 * i];
					if (s < S31_NSIG && off < len && body_ok(b + off, s, b + len)) {	/* re-verified */
						soff[nsite] = off; ssig[nsite++] = s;
					}
				}
				n = patch(b);
				sayx("s31fp: cache hit, patched ", n);
				sayx("s31fp: of which copied in place ", ncopied);
				sayx("s31fp: pages given their original frame colour ", ncoloured);
				return;
			}
		}
	}
	scan(b, len);
	if (kl > 0) {
		long fd, pid = sc(NR_getpid, 0, 0, 0, 0, 0);
		unsigned i;
		rec[0] = MAGIC; rec[1] = nsite;
		for (i = 0; i < nsite; i++) { rec[2 + 2 * i] = soff[i]; rec[3 + 2 * i] = ssig[i]; }
		for (i = 0; path[i]; i++) tmp[i] = path[i];
		tmp[i++] = '.';
		for (int k = 28; k >= 0; k -= 4) tmp[i++] = "0123456789abcdef"[(pid >> k) & 15];
		tmp[i] = 0;
		sc(NR_mkdirat, AT_FDCWD, (long)dir, 0755, 0, 0);
		fd = sc(NR_openat, AT_FDCWD, (long)tmp, O_WRONLY | O_CREAT | O_EXCL, 0644, 0);
		if (fd >= 0) {
			long w = sc(NR_write, fd, (long)rec, 8 + 8 * nsite, 0, 0);
			sc(NR_close, fd, 0, 0, 0, 0);
			if (w == (long)(8 + 8 * nsite))
				sc(NR_renameat2, AT_FDCWD, (long)tmp, AT_FDCWD, (long)path, 0);
		}
	}
	n = patch(b);
	sayx("s31fp: scanned main text, patched ", n);
	sayx("s31fp: of which copied in place ", ncopied);
	sayx("s31fp: pages given their original frame colour ", ncoloured);
}
