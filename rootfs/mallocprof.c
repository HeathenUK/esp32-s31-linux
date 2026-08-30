// SPDX-License-Identifier: GPL-2.0-only
/*
 * Where do a client's bytes actually go?
 *
 * xcalc's resident set is 592 kB, of which 352 kB is one anonymous mapping -
 * the malloc arena. "352 kB of heap" is not an account of anything; it is the
 * absence of one. Every previous attempt to shrink this stack has started by
 * guessing which allocation was the big one, and the guess has been wrong at
 * least three times (the Xrm database was 159 kB, the widget records 620 kB,
 * and both were found only after being tripped over).
 *
 * So: interpose the allocator and keep a ledger. Live bytes per call site,
 * where the call site is the return address of whoever called malloc, printed
 * as library+offset so the host toolchain can name it:
 *
 *     riscv32-esp-linux-musl-addr2line -fe images/libX11.so.6.4.0 0x1234
 *
 * Usage:
 *     LD_PRELOAD=/root/mallocprof.so x11run xcalc
 *     kill -USR2 $(pidof xcalc)      # dump now, without killing it
 *     cat /tmp/mallocprof.txt
 *
 * The dump also happens at exit. The profiler's own tables are static and
 * reported in the header, so its cost never lands in the numbers it prints.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NSLOT	16384		/* live-pointer hash, open addressing */
#define NSITE	512

struct slot {
	void *p;
	uint32_t size;
	uint16_t site;
};

struct site {
	uintptr_t pc;
	size_t live, total, peak;
	uint32_t nlive, ncalls;
};

static struct slot slots[NSLOT];
static struct site sites[NSITE];
static int nsite;
static size_t live_bytes, peak_bytes, total_bytes;
static unsigned long ncalls, nmissed;

static void *(*real_malloc)(size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);

/*
 * dlsym() may allocate, and we are what allocates. Serve those first calls
 * from a static arena so the bootstrap cannot recurse.
 */
static char boot[8192];
static size_t bootused;
static int resolving;

static int is_boot(void *p)
{
	return (char *)p >= boot && (char *)p < boot + sizeof(boot);
}

static void resolve(void)
{
	if (real_malloc || resolving)
		return;
	resolving = 1;
	real_malloc = dlsym(RTLD_NEXT, "malloc");
	real_realloc = dlsym(RTLD_NEXT, "realloc");
	real_free = dlsym(RTLD_NEXT, "free");
	resolving = 0;
}

static void *boot_alloc(size_t n)
{
	void *p;

	n = (n + 15) & ~(size_t)15;
	if (bootused + n > sizeof(boot))
		return NULL;
	p = boot + bootused;
	bootused += n;
	return p;
}

/* ------------------------------------------------------------- ledger */

static unsigned hash_ptr(void *p)
{
	uintptr_t v = (uintptr_t)p;

	v = (v >> 4) * 2654435761u;
	return (unsigned)(v & (NSLOT - 1));
}

static int site_of(uintptr_t pc)
{
	int i;

	for (i = 0; i < nsite; i++)
		if (sites[i].pc == pc)
			return i;
	if (nsite == NSITE)
		return NSITE - 1;	/* last slot is the overflow bucket */
	sites[nsite].pc = pc;
	return nsite++;
}

static void note_alloc(void *p, size_t n, uintptr_t pc)
{
	unsigned h = hash_ptr(p);
	int tries = 0, s;

	while (slots[h].p) {
		if (++tries > 64) {	/* table full: count it, do not lie */
			nmissed++;
			return;
		}
		h = (h + 1) & (NSLOT - 1);
	}
	s = site_of(pc);
	slots[h].p = p;
	slots[h].size = (uint32_t)n;
	slots[h].site = (uint16_t)s;

	sites[s].live += n;
	sites[s].total += n;
	sites[s].nlive++;
	sites[s].ncalls++;
	if (sites[s].live > sites[s].peak)
		sites[s].peak = sites[s].live;

	live_bytes += n;
	total_bytes += n;
	ncalls++;
	if (live_bytes > peak_bytes)
		peak_bytes = live_bytes;
}

/* Returns the recorded size, or 0 if we never saw this pointer. */
static size_t note_free(void *p)
{
	unsigned h = hash_ptr(p);
	int tries = 0;

	while (slots[h].p != p) {
		if (!slots[h].p || ++tries > 64)
			return 0;
		h = (h + 1) & (NSLOT - 1);
	}
	{
		size_t n = slots[h].size;
		struct site *s = &sites[slots[h].site];

		s->live -= n;
		s->nlive--;
		live_bytes -= n;
		slots[h].p = NULL;
		/*
		 * Open addressing: clearing a slot breaks the probe chain of
		 * anything that collided past it, so reinsert the run.
		 */
		for (;;) {
			unsigned j = (h + 1) & (NSLOT - 1);
			struct slot t;

			if (!slots[j].p)
				break;
			t = slots[j];
			slots[j].p = NULL;
			{
				unsigned k = hash_ptr(t.p);

				while (slots[k].p)
					k = (k + 1) & (NSLOT - 1);
				slots[k] = t;
			}
			h = j;
		}
		return n;
	}
}

/* ---------------------------------------------------------------- dump */

struct mapent {
	uintptr_t lo, hi;
	char name[64];
};

static int load_maps(struct mapent *m, int max)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[256];
	int n = 0;

	if (!f)
		return 0;
	while (n < max && fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;
		char perm[8], path[192];

		path[0] = 0;
		if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %191s",
			   &lo, &hi, perm, path) < 4)
			continue;
		if (path[0] != '/')
			continue;
		m[n].lo = lo;
		m[n].hi = hi;
		snprintf(m[n].name, sizeof(m[n].name), "%s", path);
		n++;
	}
	fclose(f);
	return n;
}

static void dump(void)
{
	static struct mapent maps[64];
	int nmaps = load_maps(maps, 64);
	FILE *f = fopen("/tmp/mallocprof.txt", "w");
	int order[NSITE], i, j, n = nsite;

	if (!f)
		return;
	fprintf(f, "live %zu bytes in %u allocations, peak %zu, "
		"lifetime %zu over %lu calls\n",
		live_bytes, 0u, peak_bytes, total_bytes, ncalls);
	if (nmissed)
		fprintf(f, "WARNING: %lu allocations not tracked "
			"(hash table full) - the totals below are low\n",
			nmissed);
	fprintf(f, "profiler's own tables: %zu bytes static\n\n",
		sizeof(slots) + sizeof(sites));

	for (i = 0; i < n; i++)
		order[i] = i;
	for (i = 0; i < n; i++)		/* n <= 512; insertion sort is fine */
		for (j = i + 1; j < n; j++)
			if (sites[order[j]].live > sites[order[i]].live) {
				int t = order[i]; order[i] = order[j];
				order[j] = t;
			}

	fprintf(f, "%10s %7s %10s %8s  call site\n",
		"live", "count", "lifetime", "calls");
	for (i = 0; i < n; i++) {
		struct site *s = &sites[order[i]];
		const char *name = "?";
		uintptr_t off = s->pc;
		int k;

		if (!s->live && !s->total)
			continue;
		for (k = 0; k < nmaps; k++)
			if (s->pc >= maps[k].lo && s->pc < maps[k].hi) {
				name = maps[k].name;
				off = s->pc - maps[k].lo;
				break;
			}
		fprintf(f, "%10zu %7u %10zu %8u  %s+0x%lx\n",
			s->live, s->nlive, s->total, s->ncalls,
			name, (unsigned long)off);
	}
	fclose(f);
}

static void on_usr2(int sig)
{
	(void)sig;
	dump();
}

__attribute__((constructor))
static void init(void)
{
	resolve();
	signal(SIGUSR2, on_usr2);
	atexit(dump);
}

/* ------------------------------------------------------------ the calls */

void *malloc(size_t n)
{
	void *p;

	resolve();
	if (!real_malloc)
		return boot_alloc(n);
	p = real_malloc(n);
	if (p)
		note_alloc(p, n, (uintptr_t)__builtin_return_address(0));
	return p;
}

void *calloc(size_t a, size_t b)
{
	size_t n = a * b;
	void *p;

	resolve();
	if (!real_malloc) {
		p = boot_alloc(n);
		if (p)
			memset(p, 0, n);
		return p;
	}
	p = real_malloc(n);
	if (p) {
		memset(p, 0, n);
		note_alloc(p, n, (uintptr_t)__builtin_return_address(0));
	}
	return p;
}

void *realloc(void *old, size_t n)
{
	void *p;

	resolve();
	if (is_boot(old)) {
		p = real_malloc ? real_malloc(n) : boot_alloc(n);
		if (p && old)
			memcpy(p, old, n);
		if (p && real_malloc)
			note_alloc(p, n,
				   (uintptr_t)__builtin_return_address(0));
		return p;
	}
	if (old)
		note_free(old);
	p = real_realloc(old, n);
	if (p)
		note_alloc(p, n, (uintptr_t)__builtin_return_address(0));
	return p;
}

void free(void *p)
{
	if (!p || is_boot(p))
		return;
	resolve();
	note_free(p);
	real_free(p);
}
