/*
 * libs31fp.so - make an UNMODIFIED program's double arithmetic fast.
 *
 * This core has hardware single-precision float and no double, so every
 * `double` operation in every program is a call into libgcc's soft-float
 * routines - which the linker copied INTO the program (and into each shared
 * library that needs them), where no LD_PRELOAD symbol can reach them. They
 * are slow: __muldf3 is 723 ns here against 24 ns for a float multiply, and
 * OpenTyrian's FM synthesiser spends half its audio thread in it.
 *
 * The user's rule is absolute: no rebuilding, relinking or reconfiguring of
 * client software. So this library does it from underneath, at load time:
 * because every program was built with the same toolchain, the routines are
 * byte-identical everywhere. The constructor walks the loaded objects, finds
 * each routine by its first bytes (sigs.h, generated from the toolchain's own
 * libgcc.a by mksig.py), and overwrites its entry with a jump to the fast
 * version in s31fp.c. The file on disk is never touched: the write lands in a
 * private copy-on-write page of this process only.
 *
 * Safe by construction: a routine that does not match byte-for-byte is left
 * alone, so a program built with some other compiler simply runs as before.
 * S31FP=0 disables everything; S31FP_DEBUG=1 reports what was patched.
 * Objects dlopen()ed later are not scanned.
 *
 * Preloaded for every desktop client by lvdesk and for shells by
 * /etc/profile.d - the user configures nothing.
 */
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "sigs.h"

double s31fp_muldf3(double, double);
double s31fp_adddf3(double, double);
double s31fp_subdf3(double, double);
double s31fp_floatsidf(int);
double s31fp_floatunsidf(unsigned);
int s31fp_fixdfsi(double);

static const struct { const char *name; void *fn; } repl[] = {
	{ "__muldf3", (void *)s31fp_muldf3 },
	{ "__adddf3", (void *)s31fp_adddf3 },
	{ "__subdf3", (void *)s31fp_subdf3 },
	{ "__floatsidf", (void *)s31fp_floatsidf },
	{ "__floatunsidf", (void *)s31fp_floatunsidf },
	{ "__fixdfsi", (void *)s31fp_fixdfsi },
};

static int dbg, npatched;

static void *repl_for(const char *name)
{
	unsigned i;

	for (i = 0; i < sizeof(repl) / sizeof(repl[0]); i++)
		if (!strcmp(repl[i].name, name))
			return repl[i].fn;
	return NULL;
}

/* auipc t0, hi ; jalr x0, lo(t0) - t0 is caller-saved and these are ordinary
 * functions, so it is free at their entry. */
static int patch(uint8_t *at, void *to, const char *name, const char *obj)
{
	long page = sysconf(_SC_PAGESIZE);
	uintptr_t a = (uintptr_t)at, start = a & ~(uintptr_t)(page - 1);
	uintptr_t end = (a + 8 + page - 1) & ~(uintptr_t)(page - 1);
	int32_t rel = (int32_t)((uintptr_t)to - a);
	uint32_t hi = ((uint32_t)rel + 0x800u) & 0xFFFFF000u;
	uint32_t lo = ((uint32_t)rel - hi) & 0xFFFu;
	uint32_t insn[2] = {
		0x00000297u | hi,			/* auipc t0, hi */
		0x00028067u | (lo << 20),		/* jalr x0, lo(t0) */
	};

	if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE) < 0) {
		if (dbg)
			perror("s31fp: mprotect");
		return -1;
	}
	memcpy(at, insn, sizeof(insn));
	mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
	syscall(SYS_riscv_flush_icache, (void *)a, (void *)(a + 8), 0);
	npatched++;
	if (dbg)
		fprintf(stderr, "s31fp: %s at %p in %s -> %p\n", name,
			(void *)at, obj && *obj ? obj : "(main)", to);
	return 0;
}

static int scan(struct dl_phdr_info *info, size_t size, void *data)
{
	unsigned p, s;

	(void)size; (void)data;
	if (info->dlpi_name && strstr(info->dlpi_name, "libs31fp"))
		return 0;			/* never patch ourselves */
	for (p = 0; p < info->dlpi_phnum; p++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[p];
		uint8_t *base, *end;

		if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
			continue;
		base = (uint8_t *)(info->dlpi_addr + ph->p_vaddr);
		end = base + ph->p_memsz;
		for (s = 0; s < sizeof(s31fp_sigs) / sizeof(s31fp_sigs[0]); s++) {
			const struct s31fp_sig *g = &s31fp_sigs[s];
			void *to = repl_for(g->name);
			uint8_t *q = base;

			if (!to || (size_t)(end - base) < g->len)
				continue;
			while ((q = memmem(q, (size_t)(end - q), g->bytes,
					   g->len)) != NULL) {
				/* RVC code is 2-byte aligned; anything else
				 * is data that happens to look alike. */
				if (!((uintptr_t)q & 1))
					patch(q, to, g->name, info->dlpi_name);
				q += g->len;
			}
		}
	}
	return 0;
}

__attribute__((constructor)) static void s31fp_init(void)
{
	const char *e = getenv("S31FP");

	if (e && e[0] == '0')
		return;
	dbg = getenv("S31FP_DEBUG") != NULL;
	dl_iterate_phdr(scan, NULL);
	if (dbg)
		fprintf(stderr, "s31fp: %d routine(s) redirected\n", npatched);
}
