/* every translated block: pc, instructions, executions (inline counter) */
#include <glib.h>
#include <stdio.h>
#include <qemu-plugin.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
typedef struct { uint64_t pc; unsigned n; uint64_t c; } B;
static GHashTable *h; static GMutex m;
static void bump(unsigned int cpu, void *u) { ((B *)u)->c++; }
static void tb(qemu_plugin_id_t id, struct qemu_plugin_tb *t) {
	uint64_t pc = qemu_plugin_tb_vaddr(t);
	g_mutex_lock(&m);
	B *b = g_hash_table_lookup(h, &pc);
	if (!b) { b = g_new0(B, 1); b->pc = pc; b->n = qemu_plugin_tb_n_insns(t); g_hash_table_insert(h, &b->pc, b); }
	g_mutex_unlock(&m);
	qemu_plugin_register_vcpu_tb_exec_cb(t, bump, QEMU_PLUGIN_CB_NO_REGS, b);
}
static void fin(qemu_plugin_id_t id, void *p) {
	FILE *f = fopen(g_getenv("BBDUMP") ? g_getenv("BBDUMP") : "bbdump.txt", "w");
	GHashTableIter it; gpointer k, v; g_hash_table_iter_init(&it, h);
	while (g_hash_table_iter_next(&it, &k, &v)) { B *b = v; if (b->c) fprintf(f, "%llx %u %llu\n", (unsigned long long)b->pc, b->n, (unsigned long long)b->c); }
	fclose(f);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *i, int argc, char **argv) {
	h = g_hash_table_new(g_int64_hash, g_int64_equal);
	qemu_plugin_register_vcpu_tb_trans_cb(id, tb); qemu_plugin_register_atexit_cb(id, fin, NULL); return 0;
}
