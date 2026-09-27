/*
 * cachesim.c - a D-cache model for the gl/bench images (phase 6 tier 3):
 * every load and store of the counted frames through the S31's L1 D-cache
 * geometry as documented (docs/perf-ideas-sweep.md: 64 kB, 64-byte lines,
 * 2-way, write-back; write-allocate with a line fetch on a store miss is
 * ASSUMED), LRU. What it counts is PSRAM traffic: line refills (reads) and
 * dirty evictions (write-backs), 64 bytes each. One hart, no instruction
 * fetches (the I-cache is separate), no other process - a model of the
 * library's own traffic, for A/B arms of one image, not a board number.
 *
 *   qemu-system-riscv32 ... -plugin cachesim.dylib,on=0x..,off=0x..[,kb=64,ways=2]
 *     on / off  q_on / q_off (q_replay.c), as pcprof.c
 * Output (QEMU log): "cachesim loads L stores S refills R writebacks W"
 * over the counted window (the cache state carries across windows).
 *
 * Phase 6 tier 4, the board's physical placement: the S31 cache is indexed
 * by the physical (PSRAM) address and a way is 32 kB, so set bits 12-14 come
 * from the page frame Linux picked, not from the virtual address the
 * library sees.
 *     pages=1,seed=S[,col=ADDR:LEN ...]
 * gives every 4 kB page a random frame (first touch, without replacement),
 * except each col= range (up to 4), which stays contiguous from a random
 * page of its own - a CMA dumb buffer, as the zero-copy colour buffers are.
 * fix=VPAGE:C forces a page's frame colour (an oracle arm); pagestat=1|2|3
 * prints the pages (and lines) with the most refills, or with =3 the most
 * accesses.
 * s31, MIT.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LSH 6
static unsigned nsets = 512, ways = 2;
static uint32_t *tag;         /* nsets * ways: line address + 1, 0 empty */
static uint8_t *dirty;
static uint32_t *age;
static uint32_t tick;
static uint64_t a_on, a_off;
static int on;
static uint64_t n_ld, n_st, n_ref, n_wb;

static int pages, pagestat;
static uint32_t *pref;		/* pagestat=1: refills per virtual page */
static uint32_t cur_vp, cur_line;
static uint32_t *lref;		/* pagestat=2: refills per line too */
static uint64_t rs;
static uint32_t *fmap, *pool, npool = 1u << 15, pnext;
static uint32_t col_a[4], col_n[4], col_base[4];
static int ncol;
/* fix=VPAGE:COLOUR (up to 16): that page gets a frame of the given colour
   (bits 12-14) - an oracle arm for "the hot pages never collide" */
static uint32_t fix_p[16], fix_c[16];
static int nfix;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return (uint32_t)(rs >> 16);
}
static uint32_t new_frame(void)
{
	if (pnext == npool) {
		fprintf(stderr, "cachesim: out of frames\n");
		exit(1);
	}
	return pool[pnext++];
}
/* line address -> the line's physical line address */
static uint32_t phys(uint32_t line)
{
	uint32_t vp = line >> 6;
	int i;
	for (i = 0; i < ncol; i++)
		if (vp - col_a[i] < col_n[i])
			return (col_base[i] + (vp - col_a[i])) << 6 | (line & 63);
	if (!fmap[vp]) {
		for (i = 0; i < nfix; i++)
			if (fix_p[i] == vp) {
				uint32_t j;
				for (j = pnext; j < npool && (pool[j] & 7) != fix_c[i]; j++)
					;
				if (j < npool) {
					uint32_t t = pool[j];
					pool[j] = pool[pnext];
					pool[pnext] = t;
				}
				break;
			}
		fmap[vp] = new_frame() + 1;
	}
	return (fmap[vp] - 1) << 6 | (line & 63);
}

static void access_line(uint32_t line, int st)
{
	unsigned s = line & (nsets - 1), i, v = 0;
	uint32_t *t = tag + s * ways, *ag = age + s * ways;
	uint8_t *d = dirty + s * ways;
	for (i = 0; i < ways; i++)
		if (t[i] == line + 1) {
			ag[i] = ++tick;
			if (st) d[i] = 1;
			return;
		}
	for (i = 1; i < ways; i++)
		if (t[i] == 0 || (t[v] != 0 && ag[i] < ag[v]))
			v = i;
	if (t[0] == 0) v = 0;
	if (on) {
		n_ref++;
		if (t[v] && d[v]) n_wb++;
		if (pref && pagestat < 3) pref[cur_vp]++;
		if (lref && pagestat < 3) lref[cur_line & ((1u << 26) - 1)]++;
	}
	t[v] = line + 1;
	d[v] = (uint8_t)st;
	ag[v] = ++tick;
}

static void mem_cb(unsigned int vcpu, qemu_plugin_meminfo_t info, uint64_t va, void *u)
{
	int st = qemu_plugin_mem_is_store(info);
	unsigned sz = 1u << qemu_plugin_mem_size_shift(info);
	uint32_t a = (uint32_t)va, l0 = a >> LSH, l1 = (a + sz - 1) >> LSH;
	if (on) {
		if (st) n_st++; else n_ld++;
	}
	cur_vp = l0 >> 6 & ((1u << 20) - 1);
	cur_line = l0;
	if (pagestat == 3 && on) {
		pref[cur_vp]++;
		lref[l0 & ((1u << 26) - 1)]++;
	}
	if (pages) {
		access_line(phys(l0), st);
		if (l1 != l0) access_line(phys(l1), st);
		return;
	}
	access_line(l0, st);
	if (l1 != l0) access_line(l1, st);
}

static void on_cb(unsigned int vcpu, void *u) { on = 1; }
static void off_cb(unsigned int vcpu, void *u) { on = 0; }

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
	size_t i, n = qemu_plugin_tb_n_insns(tb);
	for (i = 0; i < n; i++) {
		struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
		uint64_t va = qemu_plugin_insn_vaddr(insn);
		if (a_on && va == a_on)
			qemu_plugin_register_vcpu_insn_exec_cb(insn, on_cb, QEMU_PLUGIN_CB_NO_REGS, NULL);
		if (a_off && va == a_off)
			qemu_plugin_register_vcpu_insn_exec_cb(insn, off_cb, QEMU_PLUGIN_CB_NO_REGS, NULL);
		qemu_plugin_register_vcpu_mem_cb(insn, mem_cb, QEMU_PLUGIN_CB_NO_REGS,
						 QEMU_PLUGIN_MEM_RW, NULL);
	}
}

static void at_exit(qemu_plugin_id_t id, void *u)
{
	char b[2048];
	snprintf(b, sizeof(b), "cachesim sets %u ways %u loads %" PRIu64 " stores %" PRIu64
		 " refills %" PRIu64 " writebacks %" PRIu64 "\n", nsets, ways, n_ld, n_st, n_ref, n_wb);
	qemu_plugin_outs(b);
	if (pref) {
		/* the 48 pages with the most refills: vpage, refills, the
		   frame's colour (set bits 12-14) */
		for (int k = 0; k < 48; k++) {
			uint32_t best = 0, bi = 0;
			for (uint32_t j = 0; j < (1u << 20); j++)
				if (pref[j] > best) { best = pref[j]; bi = j; }
			if (!best) break;
			snprintf(b, sizeof(b), "cachesim page 0x%05x refills %u colour %d\n", bi, best,
				 pages ? (int)(phys(bi << 6) >> 6 & 7) : (int)(bi & 7));
			qemu_plugin_outs(b);
			pref[bi] = 0;
			if (lref && k < 16) {
				char *o = b;
				o += snprintf(o, 64, "cachesim lines 0x%05x:", bi);
				for (int m = 0; m < 64; m++)
					if (lref[bi << 6 | m] * 50 > best)
						o += snprintf(o, 24, " %03x:%u", m << 6, lref[bi << 6 | m]);
				snprintf(o, 4, "\n");
				qemu_plugin_outs(b);
			}
		}
	}
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
					   int argc, char **argv)
{
	int i;
	unsigned kb = 64;
	for (i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "on=", 3)) a_on = strtoull(argv[i] + 3, NULL, 0);
		else if (!strncmp(argv[i], "off=", 4)) a_off = strtoull(argv[i] + 4, NULL, 0);
		else if (!strncmp(argv[i], "kb=", 3)) kb = (unsigned)strtoul(argv[i] + 3, NULL, 0);
		else if (!strncmp(argv[i], "ways=", 5)) ways = (unsigned)strtoul(argv[i] + 5, NULL, 0);
		else if (!strncmp(argv[i], "pages=", 6)) pages = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "pagestat=", 9)) pagestat = atoi(argv[i] + 9);
		else if (!strncmp(argv[i], "seed=", 5)) rs = strtoull(argv[i] + 5, NULL, 0);
		else if (!strncmp(argv[i], "fix=", 4) && nfix < 16) {
			char *e;
			fix_p[nfix] = (uint32_t)strtoul(argv[i] + 4, &e, 0);
			fix_c[nfix] = *e == ':' ? (uint32_t)strtoul(e + 1, NULL, 0) & 7 : 0;
			nfix++;
		}
		else if (!strncmp(argv[i], "col=", 4) && ncol < 4) {
			char *e;
			uint32_t a = (uint32_t)strtoul(argv[i] + 4, &e, 0);
			uint32_t n = *e == ':' ? (uint32_t)strtoul(e + 1, NULL, 0) : 0;
			col_a[ncol] = a >> 12;
			col_n[ncol] = ((a & 4095) + n + 4095) >> 12;
			ncol++;
		}
	}
	if (pagestat) pref = calloc(1u << 20, sizeof(*pref));
	if (pagestat > 1) lref = calloc(1u << 26, sizeof(*lref));
	if (pages) {
		uint32_t j, t;
		rs = rs * 0x9E3779B97F4A7C15ull + 0x1234567;
		for (i = 0; i < 8; i++) rnd();
		pool = malloc(npool * sizeof(*pool));
		fmap = calloc(1u << 20, sizeof(*fmap));
		for (j = 0; j < npool; j++) pool[j] = j;
		for (j = npool - 1; j > 0; j--) {
			uint32_t k = rnd() % (j + 1);
			t = pool[j]; pool[j] = pool[k]; pool[k] = t;
		}
		/* a colour buffer's run of frames, clear of the random pool's */
		for (i = 0; i < ncol; i++)
			col_base[i] = npool + 0x1000u * (uint32_t)(i + 1) + (rnd() & 0x7ff);
	}
	nsets = kb * 1024 / 64 / ways;
	tag = calloc((size_t)nsets * ways, sizeof(*tag));
	age = calloc((size_t)nsets * ways, sizeof(*age));
	dirty = calloc((size_t)nsets * ways, 1);
	on = a_on ? 0 : 1;
	qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
	qemu_plugin_register_atexit_cb(id, at_exit, NULL);
	return 0;
}
