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
	char b[256];
	snprintf(b, sizeof(b), "cachesim sets %u ways %u loads %" PRIu64 " stores %" PRIu64
		 " refills %" PRIu64 " writebacks %" PRIu64 "\n", nsets, ways, n_ld, n_st, n_ref, n_wb);
	qemu_plugin_outs(b);
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
