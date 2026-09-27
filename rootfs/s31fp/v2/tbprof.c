/* QEMU TCG plugin: executed-instruction count per guest PC, dumped at exit
 * as "pc count" lines (out=<file>). Attribute to functions with nm. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
typedef struct { uint64_t pc; uint64_t cnt; } Ins;
static GHashTable *h;
static const char *out = "tbprof.txt";
static void exec_cb(unsigned int vcpu, void *u) { ((Ins *)u)->cnt++; }
static void trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
	size_t n = qemu_plugin_tb_n_insns(tb);
	for (size_t i = 0; i < n; i++) {
		struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
		uint64_t pc = qemu_plugin_insn_vaddr(in);
		Ins *e = g_hash_table_lookup(h, &pc);
		if (!e) { e = g_new0(Ins, 1); e->pc = pc; g_hash_table_insert(h, &e->pc, e); }
		qemu_plugin_register_vcpu_insn_exec_cb(in, exec_cb, QEMU_PLUGIN_CB_NO_REGS, e);
	}
}
static void fin(qemu_plugin_id_t id, void *p)
{
	FILE *f = fopen(out, "w");
	GHashTableIter it; gpointer k, v;
	g_hash_table_iter_init(&it, h);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Ins *e = v;
		if (e->cnt) fprintf(f, "%llx %llu\n", (unsigned long long)e->pc, (unsigned long long)e->cnt);
	}
	fclose(f);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		if (!strncmp(argv[i], "out=", 4)) out = argv[i] + 4;
	h = g_hash_table_new(g_int64_hash, g_int64_equal);
	qemu_plugin_register_vcpu_tb_trans_cb(id, trans);
	qemu_plugin_register_atexit_cb(id, fin, NULL);
	return 0;
}
