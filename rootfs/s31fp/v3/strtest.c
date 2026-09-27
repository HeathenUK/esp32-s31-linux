/*
 * strtest - A1 correctness: the exported string routines (whatever the
 * dynamic linker binds: libs31fp.so's when preloaded) against plain
 * reference definitions, on random inputs built to hit every size and
 * alignment class, including the PIE-eligible ones (>= 64 bytes, equal
 * 16-byte alignment, long equal prefixes).
 *
 *   strtest check N [threads] [hammer]   exported functions vs reference
 *   strtest libc N                       libc's OWN functions (dlsym
 *                                        RTLD_NEXT past the preload) vs the
 *                                        reference - on the board, run
 *                                        pinned to CPU0 so libc's PIE paths
 *                                        run: it proves the reference IS
 *                                        libc's semantics
 *   strtest bench SIZE N                 ns per call, exported memcpy/memchr/
 *                                        memcmp/strcmp (the A1 on/off cost)
 * threads > 1 runs the check in that many threads (joinable ones, then a
 * detached batch, some leaving by pthread_exit and some cancelled); hammer=1
 * adds a thread that flips every worker's affinity between CPU0 and CPU1
 * every millisecond (the board's forced-migration test).
 * Prints RESULT lines; exit 0 only with 0 mismatches.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#define LC __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

/* ---- references: the C standard's definitions, byte by byte ---- */
static LC void *r_memchr(const void *s, int c, size_t n)
{ const unsigned char *p = s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return 0; }
static LC void *r_memrchr(const void *s, int c, size_t n)
{ const unsigned char *p = (const unsigned char *)s + n; while (n--) if (*--p == (unsigned char)c) return (void *)p; return 0; }
static LC int r_memcmp(const void *a, const void *b, size_t n)
{ const unsigned char *p = a, *q = b; for (; n; n--, p++, q++) if (*p != *q) return *p - *q; return 0; }
static LC int r_strcmp(const char *a, const char *b)
{ const unsigned char *p = (const void *)a, *q = (const void *)b; while (*p && *p == *q) p++, q++; return *p - *q; }
static LC size_t r_strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
static LC char *r_strrchr(const char *s, int c)
{ const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
static LC char *r_strstr(const char *h, const char *n)
{ size_t i; if (!*n) return (char *)h; for (; *h; h++) { for (i = 0; n[i] && h[i] == n[i]; i++); if (!n[i]) return (char *)h; } return 0; }
static LC void *r_memmem(const void *h, size_t k, const void *n, size_t l)
{ const unsigned char *p = h; size_t i; if (!l) return (void *)h; if (k < l) return 0;
  for (i = 0; i + l <= k; i++) if (!r_memcmp(p + i, n, l)) return (void *)(p + i);
  return 0; }

/* the functions under test, called through pointers so nothing is inlined */
struct fns {
	void *(*memcpy)(void *, const void *, size_t);
	void *(*memmove)(void *, const void *, size_t);
	void *(*memchr)(const void *, int, size_t);
	void *(*memrchr)(const void *, int, size_t);
	int (*memcmp)(const void *, const void *, size_t);
	int (*bcmp)(const void *, const void *, size_t);
	int (*strcmp)(const char *, const char *);
	int (*strcoll)(const char *, const char *);
	size_t (*strnlen)(const char *, size_t);
	char *(*strrchr)(const char *, int);
	char *(*strstr)(const char *, const char *);
	void *(*memmem)(const void *, size_t, const void *, size_t);
};
static struct fns F;
static volatile struct fns *FP = &F;

static uint32_t rnd(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static size_t rsize(uint32_t *s)
{
	uint32_t r = rnd(s) % 16;
	if (r < 5) return rnd(s) % 16;
	if (r < 9) return 48 + rnd(s) % 40;	/* the 64 boundary */
	if (r < 14) return rnd(s) % 320;
	return rnd(s) % 2100;
}
static unsigned char rbyte(uint32_t *s)
{	/* a small alphabet so matches and equal prefixes happen; 0x80/0xff for sign bugs */
	static const unsigned char al[] = { 'a', 'b', 'c', 0x80, 0xff, 0x7f, 'a', 'a' };
	return al[rnd(s) & 7];
}

#define BUF 4608
struct ctx { uint32_t seed; unsigned long n, bad, calls; int id, done, cancel_ready, cancel_sent; };

/* No %s anywhere: printf's %s is libc's internal strnlen -> memchr, whose PIE
 * path QEMU cannot run (and which a lent-CPU run would trap on). */
static void out(const char *s) { write(1, s, strlen(s)); }
static void fail(struct ctx *c, const char *what, size_t n, int ao, int bo)
{
	char b[96];
	if (c->bad++ >= 20) return;
	out("MISMATCH ");
	out(what);
	snprintf(b, sizeof b, " n=%zu align %d/%d (thread %d)\n", n, ao, bo, c->id);
	out(b);
}

static void one(struct ctx *c, unsigned char *A, unsigned char *B, unsigned char *D, unsigned char *E)
{
	uint32_t *s = &c->seed;
	size_t n = rsize(s), i;
	int ao = rnd(s) % 32, bo = (rnd(s) & 1) ? ao : (int)(rnd(s) % 32);	/* equal alignment half the time */
	unsigned char *a = A + ao, *b = B + bo;
	int ch = (rnd(s) & 3) ? rbyte(s) : (int)(rnd(s) & 0x3ff);	/* sometimes with high bits */

	for (i = 0; i < n + 64; i++) a[i] = rbyte(s);
	for (i = 0; i < n + 64; i++) b[i] = a[i];
	if (n && (rnd(s) & 1)) b[rnd(s) % n] ^= (unsigned char)(1 + rnd(s) % 255);	/* one difference */

	/* memcpy / memmove with guard bytes */
	{
		size_t m = n > BUF - 200 ? BUF - 200 : n;
		int d0 = rnd(s) % 32;
		for (i = 0; i < m + 96; i++) D[i] = E[i] = 0x5a;
		void *r1 = FP->memcpy(D + d0, a, m);
		for (i = 0; i < m; i++) E[d0 + i] = a[i];
		if (r1 != D + d0 || r_memcmp(D, E, m + 96)) fail(c, "memcpy", m, d0, ao);
		/* memmove: overlapping both directions and disjoint, within D */
		for (i = 0; i < m + 96; i++) D[i] = E[i] = (unsigned char)(i * 7 + 1);
		int so = rnd(s) % 64, doff = rnd(s) % 64;
		r1 = FP->memmove(D + doff, D + so, m);
		{
			unsigned char *t = malloc(m + 1);
			for (i = 0; i < m; i++) t[i] = E[so + i];
			for (i = 0; i < m; i++) E[doff + i] = t[i];
			free(t);
		}
		if (r1 != D + doff || r_memcmp(D, E, m + 96)) fail(c, "memmove", m, doff, so);
		c->calls += 2;
	}
	/* comparisons */
	if (FP->memcmp(a, b, n) != r_memcmp(a, b, n)) fail(c, "memcmp", n, ao, bo);
	if (FP->bcmp(a, b, n) != r_memcmp(a, b, n)) fail(c, "bcmp", n, ao, bo);
	/* searches */
	if (FP->memchr(a, ch, n) != r_memchr(a, ch, n)) fail(c, "memchr", n, ao, ch);
	if (FP->memrchr(a, ch, n) != r_memrchr(a, ch, n)) fail(c, "memrchr", n, ao, ch);
	/* strings: NUL at n (and b's NUL at n or at the difference) */
	{
		unsigned char sa = a[n], sb = b[n];
		a[n] = 0; b[n] = 0;
		for (i = 0; i < n; i++) { if (!a[i]) a[i] = 'z'; if (!b[i]) b[i] = 'z'; }
		if ((rnd(s) & 7) == 0 && n) b[rnd(s) % n] = 0;	/* b shorter */
		if (FP->strcmp((char *)a, (char *)b) != r_strcmp((char *)a, (char *)b)) fail(c, "strcmp", n, ao, bo);
		if (FP->strcoll((char *)a, (char *)b) != r_strcmp((char *)a, (char *)b)) fail(c, "strcoll", n, ao, bo);
		size_t lim = (rnd(s) & 1) ? n + 10 : rnd(s) % (n + 1);
		if (FP->strnlen((char *)a, lim) != r_strnlen((char *)a, lim)) fail(c, "strnlen", lim, ao, 0);
		int cc = (rnd(s) & 15) ? ch : 0;
		if (FP->strrchr((char *)a, cc) != r_strrchr((char *)a, cc)) fail(c, "strrchr", n, ao, cc);
		/* needle: a substring of a (present) or a mutated one, lengths up to 100 */
		{
			char nd[128];
			size_t nl = rnd(s) % 101, st;
			if (nl > n) nl = n;
			st = n > nl ? rnd(s) % (n - nl + 1) : 0;
			for (i = 0; i < nl; i++) nd[i] = (char)a[st + i];
			nd[nl] = 0;
			if (nl && (rnd(s) & 3) == 0) nd[rnd(s) % nl] = 'q';
			if (FP->strstr((char *)a, nd) != r_strstr((char *)a, nd)) fail(c, "strstr", n, (int)nl, ao);
			if (FP->memmem(a, n, nd, nl) != r_memmem(a, n, nd, nl)) fail(c, "memmem", n, (int)nl, ao);
		}
		a[n] = sa; b[n] = sb;
	}
	c->calls += 11;
}

static int g_libc;
static void bind(void)
{
	void *h = g_libc ? RTLD_NEXT : RTLD_DEFAULT;
#define B(x) F.x = (__typeof__(F.x))dlsym(h, #x)
	if (g_libc) {
		/* RTLD_NEXT from the executable = the first definition after it:
		 * the preload's. libc's own: open it and ask it directly. */
		h = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD);
		if (!h) h = dlopen("/lib/ld-musl-riscv32-sf.so.1", RTLD_NOW | RTLD_NOLOAD);
	}
	B(memcpy); B(memmove); B(memchr); B(memrchr); B(memcmp); B(bcmp); B(strcmp);
	B(strcoll); B(strnlen); B(strrchr); B(strstr); B(memmem);
}

/* End-of-mapping inputs: a vector/word overread must fault rather than hide
 * in spare heap capacity. Run through the same exported/reference binding. */
static int page_edges(void)
{
	char *a = mmap(0, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	char *b = mmap(0, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED || b == MAP_FAILED) return 1;
	if (mprotect(a+4096, 4096, PROT_NONE) || mprotect(b+4096, 4096, PROT_NONE)) return 1;
	for (size_t n = 0; n < 160; n++) {
		char *x = a+4095-n, *y = b+4095-n;
		for (size_t i = 0; i < n; i++) x[i] = y[i] = 'a';
		x[n] = y[n] = 0;
		if (F.memcpy(y, x, n+1) != y || F.memcmp(x,y,n+1) || F.strcmp(x,y) ||
		    F.memchr(x,0,n+1) != x+n || F.memrchr(x,0,n+1) != x+n ||
		    F.strnlen(x,n+1) != n || F.strrchr(x,0) != x+n ||
		    F.strstr(x,y) != x || F.memmem(x,n,y,n) != x) return 1;
	}
	munmap(a,8192); munmap(b,8192);
	return 0;
}

static unsigned long g_iters;
static pthread_mutex_t done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;
struct worker_storage { struct ctx *ctx; void *a, *b, *d, *e; };
static void worker_done(void *arg)
{
	struct worker_storage *w = arg;
	free(w->a); free(w->b); free(w->d); free(w->e);
	pthread_mutex_lock(&done_mu);
	w->ctx->done = 1;
	pthread_cond_broadcast(&done_cv);
	pthread_mutex_unlock(&done_mu);
}
static void *worker(void *p)
{
	struct ctx *c = p;
	int cancel_target = c->id >= 1000 && c->id % 3 == 2;
	/* Detached storage must survive the ENTIRE pthread_cancel call. musl
	 * publishes the cancel flag before pthread_kill takes the target's lock;
	 * an enabled target can otherwise exit/unmap underneath that lock. */
	if (cancel_target) pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, 0);
	unsigned char *A = malloc(BUF), *B = malloc(BUF), *D = malloc(BUF), *E = malloc(BUF);
	struct worker_storage w = { c, A, B, D, E };
	pthread_cleanup_push(worker_done, &w);
	if (!A || !B || !D || !E) { c->bad++; pthread_exit(0); }
	for (unsigned long i = 0; i < g_iters; i++) {
		one(c, A, B, D, E);
		c->n++;
		if (c->id >= 1000 && c->id % 3 == 1 && i == g_iters / 2) pthread_exit(0);	/* leave early */
	}
	if (cancel_target) {
		pthread_mutex_lock(&done_mu);
		c->cancel_ready = 1;
		pthread_cond_broadcast(&done_cv);
		while (!c->cancel_sent) pthread_cond_wait(&done_cv, &done_mu);
		pthread_mutex_unlock(&done_mu);
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0);
		pthread_testcancel();
		c->bad++; /* A pending cancellation must run the cleanup handler. */
	}
	pthread_cleanup_pop(1);
	return 0;
}

static int g_stop;
static pthread_t g_tids[64];
static int g_ntids;
static void *hammer(void *u)
{
	(void)u;
	for (unsigned k = 0; !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); k++) {
		cpu_set_t m;
		CPU_ZERO(&m);
		CPU_SET(k & 1, &m);
		for (int i = 0; i < g_ntids; i++) pthread_setaffinity_np(g_tids[i], sizeof(m), &m);
		usleep(1000);
	}
	return 0;
}

static double now(void)
{
	struct timespec t;
	syscall(SYS_clock_gettime64, CLOCK_MONOTONIC, &t);	/* not the preload's */
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "check";
	if (!strcmp(mode, "bench") || !strcmp(mode, "cmpbench")) {
		size_t sz = argc > 2 ? strtoul(argv[2], 0, 0) : 256;
		unsigned long n = argc > 3 ? strtoul(argv[3], 0, 0) : 100000, i;
		unsigned char *a = aligned_alloc(16, sz + 64), *b = aligned_alloc(16, sz + 64);
		volatile uintptr_t sink = 0;
		bind();
		for (i = 0; i < sz; i++) a[i] = b[i] = 'x';
		a[sz] = b[sz] = 0;
		if (!strcmp(mode, "cmpbench")) {
			long where = argc > 4 ? strtol(argv[4], 0, 0) : -1;
			if (where >= 0 && (size_t)where < sz) b[where] = 'y';
		}
#define T(name, expr) { double t0 = now(); for (i = 0; i < n; i++) sink += (uintptr_t)(expr); \
			double elapsed = now() - t0; char ob[80]; out("BENCH " name); \
			snprintf(ob, sizeof ob, " size %zu: %.1f ns/call\n", sz, elapsed * 1e9 / n); out(ob); }
		if (strcmp(mode, "cmpbench")) {
		T("memcpy", FP->memcpy(a, b, sz));
		T("memchr", FP->memchr(a, 'y', sz));
		}
		T("memcmp", FP->memcmp(a, b, sz));
		T("strcmp", FP->strcmp((char *)a, (char *)b));
		if (strcmp(mode, "cmpbench")) { T("strnlen", FP->strnlen((char *)a, sz + 8)); }
		(void)sink;
		return 0;
	}
	g_libc = !strcmp(mode, "libc");
	g_iters = argc > 2 ? strtoul(argv[2], 0, 0) : 100000;
	int nt = argc > 3 ? atoi(argv[3]) : 1, ham = argc > 4 ? atoi(argv[4]) : 0;
	bind();
	if (!F.memcpy || !F.memmem) { out("RESULT bind failed\n"); return 2; }
	if (page_edges()) { out("RESULT page-edge failure\n"); return 1; }
	/* Repetitive long needles exercise the scalar two-way search, including
	 * misses that made the previous naive fallback quadratic. */
	{
		static char hay[32769], pat[4097];
		for (size_t i = 0; i < sizeof(hay)-1; i++) hay[i] = 'a';
		for (size_t i = 0; i < sizeof(pat)-1; i++) pat[i] = 'a';
		pat[4095] = 'b';
		if (F.memmem(hay, 32768, pat, 4096) || F.strstr(hay, pat)) return 1;
		hay[32767] = 'b';
		if (F.memmem(hay, 32768, pat, 4096) != hay + 28672 ||
		    F.strstr(hay, pat) != hay + 28672) return 1;
	}
	struct ctx cs[64] = { 0 };
	unsigned long tot = 0, bad = 0, calls = 0;
	if (nt <= 1) {
		cs[0].seed = 0x9e3779b9u;
		worker(&cs[0]);
		tot = cs[0].n; bad = cs[0].bad; calls = cs[0].calls;
	} else {
		pthread_t hm;
		if (nt > 32) nt = 32;
		for (int i = 0; i < nt; i++) {
			cs[i].seed = 0x12345u + 77777u * i; cs[i].id = i;
			if (pthread_create(&g_tids[i], 0, worker, &cs[i])) return 2;
		}
		g_ntids = nt;
		if (ham && pthread_create(&hm, 0, hammer, 0)) return 2;
		/* Stop the affinity writer BEFORE joining/freeing worker handles.
		 * Completed joinable workers retain their pthread storage until join. */
		pthread_mutex_lock(&done_mu);
		for (int i = 0; i < nt; i++)
			while (!cs[i].done) pthread_cond_wait(&done_cv, &done_mu);
		pthread_mutex_unlock(&done_mu);
		__atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
		if (ham) pthread_join(hm, 0);
		for (int i = 0; i < nt; i++) pthread_join(g_tids[i], 0);
		/* detached batch: return / pthread_exit / cancel */
		for (int i = nt; i < 2 * nt; i++) {
			pthread_t t; pthread_attr_t at;
			cs[i].seed = 0x777u + 99991u * i; cs[i].id = 1000 + i;
			pthread_attr_init(&at);
			pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
			if (pthread_create(&t, &at, worker, &cs[i])) return 2;
			pthread_attr_destroy(&at);
			if (cs[i].id % 3 == 2) {
				pthread_mutex_lock(&done_mu);
				while (!cs[i].cancel_ready) pthread_cond_wait(&done_cv, &done_mu);
				pthread_mutex_unlock(&done_mu);
				if (pthread_cancel(t)) return 2;
				pthread_mutex_lock(&done_mu);
				cs[i].cancel_sent = 1;
				pthread_cond_broadcast(&done_cv);
				pthread_mutex_unlock(&done_mu);
			}
		}
		/* Completion, not a guessed sleep: detached workers may be slower on
		 * the board, and reading live counters would race the workload. */
		pthread_mutex_lock(&done_mu);
		for (int i = nt; i < 2 * nt; i++)
			while (!cs[i].done) pthread_cond_wait(&done_cv, &done_mu);
		pthread_mutex_unlock(&done_mu);
		for (int i = 0; i < 2 * nt; i++) { tot += cs[i].n; bad += cs[i].bad; calls += cs[i].calls; }
	}
	{
		char ob[128];
		out(g_libc ? "RESULT libc-vs-reference: " : "RESULT exported-vs-reference: ");
		snprintf(ob, sizeof ob, "%lu cases, %lu calls, %lu mismatches (threads %d hammer %d)\n",
			 tot, calls, bad, nt, ham);
		out(ob);
	}
	return bad ? 1 : 0;
}
