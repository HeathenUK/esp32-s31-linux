/*
 * icsim.c - an I-cache model for the gl/bench images (phase 6 tier 6):
 * every instruction fetch of the counted frames, one 64-byte line at a
 * time, through the hart-1 L1 I-cache geometry as MEASURED on the board
 * (artifacts/gl/phase6/tier6/icgeo.c: 32 kB, 2-way, 64-byte lines, 16 kB a
 * way, physically indexed - set bits 12-13 come from the page frame), LRU.
 * One hart, only this image's code, no kernel, no other process: an A/B
 * instrument for layouts of one library, not a board number.
 *
 *   qemu-system-riscv32 ... -plugin icsim.dylib,on=0x..,off=0x..[,kb=32,ways=2]
 *     on / off   q_on / q_off (q_replay.c); each q_on starts a counted frame
 *     lo=A,hi=B  model only fetches from [A,B) (e.g. the library's text);
 *                the rest is not fetched through the model at all
 *     pages=1,seed=S  every 4 kB code page gets a random frame (first
 *                touch, without replacement) - the board's page cache
 *     seq=LO:HI  with pages=1: the pages of [LO,HI) get frames whose colour
 *                follows the virtual page (RAMTEXT=2's colour-chosen copy)
 *     dump=FILE  per line: address, instructions, refills, frames it ran in,
 *                fetches (entries into the line from another line)
 * Output (QEMU log): "icsim frames F insns I fetches L refills R uniq U"
 * (U = sum over frames of the distinct lines each frame executed).
 * s31, MIT.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LB 20			/* per-line arrays cover 64 MB of address space */
#define LM ((1u << LB) - 1)
static unsigned nsets, ways = 2;
static uint32_t *tag, *age, tick;
static uint64_t a_on, a_off, lo, hi = ~0ull;
static int on, pages;
static uint32_t frame;
static uint64_t n_ins, n_fetch, n_ref, n_uniq;
static uint32_t *l_ins, *l_ref, *l_stamp, *l_frames, *l_vis;
static char *dump;
static uint64_t rs;
static uint32_t *fmap, *pool, npool = 1u << 15, pnext;
static uint32_t ubase = ~0u;
static uint32_t seq_lo, seq_hi;	/* seq=LO:HI - these pages get frames of colour vpage & 3 */	/* the line-address bits above LM, from the first counted fetch */

static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return (uint32_t)(rs >> 16);
}
static uint32_t phys(uint32_t line)
{
	uint32_t vp = (line >> 6) & ((1u << 20) - 1);
	if (!pages) return line;
	if (!fmap[vp]) {
		if (pnext == npool) { fprintf(stderr, "icsim: out of frames\n"); exit(1); }
		if (vp >= seq_lo && vp < seq_hi) {
			uint32_t j;
			for (j = pnext; j < npool && (pool[j] & 3) != (vp & 3); j++)
				;
			if (j < npool) { uint32_t t = pool[j]; pool[j] = pool[pnext]; pool[pnext] = t; }
		}
		fmap[vp] = pool[pnext++] + 1;
	}
	return (fmap[vp] - 1) << 6 | (line & 63);
}

static void fetch(uint32_t line)
{
	uint32_t pl = phys(line);
	unsigned s = pl & (nsets - 1), i, v = 0;
	uint32_t *t = tag + s * ways, *ag = age + s * ways;
	for (i = 0; i < ways; i++)
		if (t[i] == pl + 1) { ag[i] = ++tick; return; }
	for (i = 1; i < ways; i++)
		if (t[i] == 0 || (t[v] != 0 && ag[i] < ag[v])) v = i;
	if (t[0] == 0) v = 0;
	if (on) { n_ref++; l_ref[line & LM]++; }
	t[v] = pl + 1;
	ag[v] = ++tick;
}

struct tbi { uint32_t n; uint32_t w[]; };	/* n pairs: line, insns */

static void tb_exec(unsigned int vcpu, void *u)
{
	struct tbi *b = u;
	for (uint32_t i = 0; i < b->n; i++) {
		uint32_t line = b->w[2 * i], k = b->w[2 * i + 1];
		fetch(line);
		if (on) {
			uint32_t x = line & LM;
			if (ubase == ~0u) ubase = line & ~LM;
			n_ins += k; n_fetch++;
			l_ins[x] += k; l_vis[x]++;
			if (l_stamp[x] != frame) { l_stamp[x] = frame; l_frames[x]++; n_uniq++; }
		}
	}
}

static void on_cb(unsigned int vcpu, void *u) { on = 1; frame++; }
static void off_cb(unsigned int vcpu, void *u) { on = 0; }

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
	size_t i, n = qemu_plugin_tb_n_insns(tb);
	struct tbi *b = g_malloc0(sizeof(*b) + 8 * (n + 1));
	uint32_t last = ~0u;
	for (i = 0; i < n; i++) {
		struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
		uint64_t va = qemu_plugin_insn_vaddr(insn);
		if (a_on && va == a_on)
			qemu_plugin_register_vcpu_insn_exec_cb(insn, on_cb, QEMU_PLUGIN_CB_NO_REGS, NULL);
		if (a_off && va == a_off)
			qemu_plugin_register_vcpu_insn_exec_cb(insn, off_cb, QEMU_PLUGIN_CB_NO_REGS, NULL);
		if (va < lo || va >= hi) continue;
		/* an instruction that straddles a line boundary fetches both */
		uint32_t l0 = (uint32_t)va >> 6, l1 = (uint32_t)(va + qemu_plugin_insn_size(insn) - 1) >> 6;
		for (uint32_t l = l0; l <= l1; l++) {
			if (l != last) { b->w[2 * b->n] = l; b->w[2 * b->n + 1] = 0; b->n++; last = l;
				if (b->n > n) b = g_realloc(b, sizeof(*b) + 8 * (b->n + n + 1)); }
			if (l == l0) b->w[2 * (b->n - 1) + 1]++;
		}
	}
	/* the callback runs at TB entry: an early exit (trap) overcounts, rare */
	qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS, b);
}

static void at_exit(qemu_plugin_id_t id, void *u)
{
	char b[512];
	snprintf(b, sizeof(b), "icsim kb %u ways %u pages %d frames %u insns %" PRIu64 " fetches %" PRIu64
		 " refills %" PRIu64 " uniq %" PRIu64 "\n", nsets * ways * 64 / 1024, ways, pages, frame,
		 n_ins, n_fetch, n_ref, n_uniq);
	qemu_plugin_outs(b);
	if (dump) {
		FILE *f = fopen(dump, "w");
		for (uint32_t x = 0; x <= LM; x++)
			if (l_ins[x] || l_ref[x])
				fprintf(f, "%08x %u %u %u %u\n", (ubase | x) << 6, l_ins[x], l_ref[x], l_frames[x], l_vis[x]);
		fclose(f);
	}
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
					   int argc, char **argv)
{
	unsigned kb = 32;
	for (int i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "on=", 3)) a_on = strtoull(argv[i] + 3, NULL, 0);
		else if (!strncmp(argv[i], "off=", 4)) a_off = strtoull(argv[i] + 4, NULL, 0);
		else if (!strncmp(argv[i], "lo=", 3)) lo = strtoull(argv[i] + 3, NULL, 0);
		else if (!strncmp(argv[i], "hi=", 3)) hi = strtoull(argv[i] + 3, NULL, 0);
		else if (!strncmp(argv[i], "kb=", 3)) kb = (unsigned)strtoul(argv[i] + 3, NULL, 0);
		else if (!strncmp(argv[i], "ways=", 5)) ways = (unsigned)strtoul(argv[i] + 5, NULL, 0);
		else if (!strncmp(argv[i], "pages=", 6)) pages = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "seed=", 5)) rs = strtoull(argv[i] + 5, NULL, 0);
		else if (!strncmp(argv[i], "dump=", 5)) dump = g_strdup(argv[i] + 5);
		else if (!strncmp(argv[i], "seq=", 4)) {
			char *e;
			seq_lo = ((uint32_t)strtoul(argv[i] + 4, &e, 0) >> 12) & ((1u << 20) - 1);
			seq_hi = *e == ':' ? (((uint32_t)strtoul(e + 1, NULL, 0) + 4095) >> 12) & ((1u << 20) - 1) : seq_lo;
		}
	}
	if (pages) {
		rs = rs * 0x9E3779B97F4A7C15ull + 0x1234567;
		for (int i = 0; i < 8; i++) rnd();
		pool = malloc(npool * sizeof(*pool));
		fmap = calloc(1u << 20, sizeof(*fmap));
		for (uint32_t j = 0; j < npool; j++) pool[j] = j;
		for (uint32_t j = npool - 1; j > 0; j--) {
			uint32_t k = rnd() % (j + 1), t = pool[j];
			pool[j] = pool[k]; pool[k] = t;
		}
	}
	nsets = kb * 1024 / 64 / ways;
	tag = calloc((size_t)nsets * ways, sizeof(*tag));
	age = calloc((size_t)nsets * ways, sizeof(*age));
	l_ins = calloc(1u << LB, 4); l_ref = calloc(1u << LB, 4);
	l_stamp = calloc(1u << LB, 4); l_frames = calloc(1u << LB, 4); l_vis = calloc(1u << LB, 4);
	on = a_on ? 0 : 1;
	qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
	qemu_plugin_register_atexit_cb(id, at_exit, NULL);
	return 0;
}
