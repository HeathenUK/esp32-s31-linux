/*
 * s31str.c - A1: PIE-safe string/memory routines for every process
 * (part of libs31fp.so, v3). S31STR=1 enables; default OFF until the board
 * test (board/v3-run.sh) passes.
 *
 * WHY. The shipped musl libc.so (build/buildroot/.../usr/lib/libc.so, md5
 * 2fac4511, stripped copy on the board) carries PIE - the S31's hart-1-only
 * 128-bit SIMD (esp.vld/esp.vst/esp.vcmp) - in exactly five functions
 * (objdump, rootfs/s31fp/v3/out/libc.dis):
 *   memchr, memrchr        PIE when n >= 64
 *   memcmp (bcmp)          PIE when n >= 64 and (a ^ b) & 15 == 0
 *   strcmp (strcoll)       PIE when (a ^ b) & 15 == 0 and the first 64
 *                          bytes are equal and not NUL
 *   memcpy                 PIE (__riscv32_xespv_memcpy64) when
 *                          (d ^ s) & 15 == 0 and n >= ((-d) & 15) + 64
 * and reaches them internally from memmove (non-overlapping -> memcpy),
 * strnlen (memchr(s, 0, n)), strrchr (memrchr(s, c, strlen + 1)), memmem
 * (memchr over the haystack), strstr (only for needles of 64+ bytes, via
 * twoway_strstr's memchr(.., l | 63) and memcmp) and strcoll (== strcmp).
 * Linux CPU1 is hart 0, the lent CPU, which has no PIE: a task there that
 * reaches one of them takes an illegal-instruction trap and the kernel moves
 * it to CPU0 (arch/riscv/kernel/esp32s31-ext.c), pinning it for good by
 * default. lvdesk/lentcpu.c fixed this for lvdesk alone; this is the same
 * idea for every dynamically linked process.
 *
 * WHAT. Each exported function below replaces libc's through ordinary
 * symbol interposition (the executable's and every library's calls through
 * the PLT land here; the executable's OWN definition, e.g. lvdesk's
 * lentcpu.c, still wins over ours). It decides in a few instructions,
 * mirroring libc's own entry tests above, whether libc's routine could reach
 * PIE for these arguments:
 *   - no  -> libc's own routine (dlsym RTLD_NEXT), unchanged;
 *   - eligible comparisons -> scalar on both CPUs: the board matrix found
 *            it faster, including early mismatches; no rseq lookup needed.
 *   - other eligible calls -> read this thread's CPU from its rseq area (one load; the kernel
 *            keeps it current): on CPU0 (hart 1, PIE) libc's own routine, so
 *            the 128-bit paths still serve; anywhere else the scalar
 *            routine below.
 * The functions are pure, and the scalar versions return exactly what
 * musl's do - memcmp/strcmp return the byte difference *a - *b of the first
 * mismatch as unsigned chars (musl's PIE code: lbu, lbu, sub), 0 if equal;
 * the search functions return the same pointer. Checked on QEMU against
 * reference definitions (strtest.c) and on silicon against libc's own PIE
 * routines on CPU0 (board test). The check and the call are not atomic: a
 * migration between them lands in the old trap-and-bounce, which is correct,
 * only rare.
 *
 * NOT COVERED: calls libc makes to itself are bound inside libc.so and
 * cannot be interposed - fgets/getdelim/getline/fgetln (memchr), printf's
 * %s and every other internal strnlen, sscanf's string reader (memchr),
 * fwrite/fputs on line-buffered streams (memrchr), strdup/strndup and
 * stdio buffering (memcpy), qsort, getpw*, getgr*, gethostby*, locale and
 * timezone code (strcmp/memcmp), crypt (memcmp). Those still trap on the
 * lent CPU, exactly as before. Static binaries (busybox) are not preloaded.
 *
 * rseq, per thread, without touching libc (musl registers none):
 *   - The rseq area is per-thread state found through a pthread key (no
 *     TLS: see struct tstate for why). A thread may only
 *     register it if we know it will be unregistered before its memory can
 *     go away: a DETACHED musl thread unmaps its own stack+TLS just before
 *     SYS_exit (__unmapself), and a return to user mode in that window with
 *     a registered area that is gone makes the kernel SIGSEGV the process
 *     (kernel/rseq.c force_sigsegv). So only two kinds of thread register:
 *       * the main thread (its state is static), marked by our constructor;
 *       * threads started through OUR pthread_create, whose start routine
 *         is wrapped: it pushes a pthread cleanup handler (musl's public
 *         _pthread_cleanup_push ABI) that unregisters, so return,
 *         pthread_exit and cancellation all unregister before musl frees
 *         anything; TSD destructors that run later see the thread marked
 *         dead and take the scalar side.
 *     Any other thread (created inside libc: thrd_create, SIGEV_THREAD
 *     timers, aio) never registers and simply always takes the scalar side
 *     for PIE-eligible calls - correct, only without PIE on CPU0.
 *   - Registration is lazy: the first PIE-eligible call in a thread.
 *   - If the program registers its own rseq area first, ours fails (EBUSY)
 *     and that thread is scalar-only. If WE register first, the program's
 *     own registration fails instead: no program on this board does (musl
 *     does not; lvdesk's lentcpu.c defines the functions itself, so ours
 *     are never called in lvdesk and never register there).
 *   - vfork/posix_spawn children share the parent's TLS but have no rseq
 *     (the kernel drops it on CLONE_VM); they run only libc internals before
 *     exec. fork() copies both the area and the registration.
 *   - One pthread key is used (of musl's 128).
 *
 * S31STR=1 enables the dispatch (default: off - every call goes straight
 * to libc's routine, one extra jump). S31STR=scalar forces the scalar side
 * for every PIE-eligible call on any CPU (the A/B arm). S31FP_DEBUG=1
 * prints the routing counters at exit.
 */
#include "s31common.h"

#define LC __attribute__((optimize("no-tree-loop-distribute-patterns")))
typedef uint32_t __attribute__((may_alias)) w32;
typedef uint16_t __attribute__((may_alias)) w16;
#define ONES 0x01010101u
#define HIGHS 0x80808080u
#define HASZERO(x) (((x) - ONES) & ~(x) & HIGHS)

/* ---------------------------------------------------------------- scalar */
/* Pure C, rv32imc + Zbb at most (whatever -march allows), never PIE. Reads
 * stay inside the objects except for aligned words that contain a byte the
 * function is allowed to read (strcmp/strlen), which cannot cross a page. */

static LC size_t sc_strlen(const char *s)
{
	const char *a = s;
	for (; (uintptr_t)s & 3; s++)
		if (!*s) return (size_t)(s - a);
	const w32 *w = (const w32 *)s;
	while (!HASZERO(*w)) w++;
	for (s = (const char *)w; *s; s++);
	return (size_t)(s - a);
}

static LC void *sc_memchr(const void *src, int c, size_t n)
{
	const unsigned char *s = src;
	c = (unsigned char)c;
	for (; ((uintptr_t)s & 3) && n; s++, n--)
		if (*s == c) return (void *)s;
	if (n >= 4) {
		uint32_t k = ONES * (uint32_t)c;
		const w32 *w = (const w32 *)s;
		for (; n >= 4; w++, n -= 4) {
			uint32_t x = *w ^ k;
			if (HASZERO(x)) break;
		}
		s = (const unsigned char *)w;
	}
	for (; n; s++, n--)
		if (*s == c) return (void *)s;
	return 0;
}

static LC void *sc_memrchr(const void *src, int c, size_t n)
{
	const unsigned char *s = (const unsigned char *)src + n;
	c = (unsigned char)c;
	for (; ((uintptr_t)s & 3) && n; n--)
		if (*--s == c) return (void *)s;
	if (n >= 4) {
		uint32_t k = ONES * (uint32_t)c;
		for (; n >= 4; n -= 4) {
			uint32_t x = *(const w32 *)(s - 4) ^ k;
			if (HASZERO(x)) break;
			s -= 4;
		}
	}
	for (; n; n--)
		if (*--s == c) return (void *)s;
	return 0;
}

static LC int sc_memcmp(const void *va, const void *vb, size_t n)
{
	const unsigned char *a = va, *b = vb;
	if (!(((uintptr_t)a ^ (uintptr_t)b) & 3)) {
		for (; ((uintptr_t)a & 3) && n; a++, b++, n--)
			if (*a != *b) return *a - *b;
		for (; n >= 8; a += 8, b += 8, n -= 8)
			if (((const w32 *)a)[0] != ((const w32 *)b)[0] ||
			    ((const w32 *)a)[1] != ((const w32 *)b)[1]) break;
		for (; n >= 4 && *(const w32 *)a == *(const w32 *)b; a += 4, b += 4, n -= 4);
	}
	for (; n; a++, b++, n--)
		if (*a != *b) return *a - *b;
	return 0;
}

static LC int sc_strcmp(const char *sa, const char *sb)
{
	const unsigned char *a = (const unsigned char *)sa, *b = (const unsigned char *)sb;
	if (!(((uintptr_t)a ^ (uintptr_t)b) & 3)) {
		for (; (uintptr_t)a & 3; a++, b++)
			if (*a != *b || !*a) return *a - *b;
		for (;;) {
			uint32_t x = *(const w32 *)a, y = *(const w32 *)b;
			if (x != y || HASZERO(x)) break;
			a += 4, b += 4;
		}
	}
	for (; *a == *b && *a; a++, b++);
	return *a - *b;
}

static LC void *sc_memcpy(void *restrict dst, const void *restrict src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
		for (; n && ((uintptr_t)d & 3); n--) *d++ = *s++;
		w32 *dw = (w32 *)d;
		const w32 *sw = (const w32 *)s;
		for (; n >= 32; n -= 32, dw += 8, sw += 8) {
			uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			uint32_t f = sw[4], g = sw[5], h = sw[6], i = sw[7];
			dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
			dw[4] = f; dw[5] = g; dw[6] = h; dw[7] = i;
		}
		for (; n >= 4; n -= 4) *dw++ = *sw++;
		d = (unsigned char *)dw;
		s = (const unsigned char *)sw;
	} else if ((((uintptr_t)d ^ (uintptr_t)s) & 1) == 0) {
		if (n && ((uintptr_t)d & 1)) { *d++ = *s++; n--; }
		w16 *dh = (w16 *)d;
		const w16 *sh = (const w16 *)s;
		for (; n >= 2; n -= 2) *dh++ = *sh++;
		d = (unsigned char *)dh;
		s = (const unsigned char *)sh;
	}
	while (n--) *d++ = *s++;
	return dst;
}

#include "sc_memmem.h"

/* first occurrence (= musl's strstr result); only used on the lent CPU
 * for needles of 64+ bytes */
static LC char *sc_strstr(const char *h, const char *n)
{
	size_t nl = sc_strlen(n);
	if (!nl) return (char *)h;
	return sc_memmem(h, sc_strlen(h), n, nl);
}

/* -------------------------------------------------------------- dispatch */
#define PIE_CPU 0		/* Linux CPU0 is hart 1, the one with PIE */
#define RSEQ_SIG 0x53053053
struct s31_rseq {
	uint32_t cpu_id_start, cpu_id;
	uint64_t rseq_cs;
	uint32_t flags, node_id, mm_cid, pad;
} __attribute__((aligned(32)));

/*
 * Per-thread state WITHOUT TLS. An earlier build kept the rseq area in
 * initial-exec TLS; adding a TLS segment to a preloaded library changes
 * musl's libc.tls_size, and on this riscv32 musl that decides whether a new
 * thread's stack pointer is 16-byte aligned: its __clone does
 * `addi a1,a1,-16` with no `andi -16`, and pthread_create only word-aligns.
 * With our 72-byte segment, threads started with sp = 8 mod 16 and printf of
 * a 64-bit value in a thread printed the wrong words (QEMU, out/dbgraw.c);
 * 68 bytes, 4 bytes and no preload happened to be fine. So the library has
 * no TLS: the area is found through a pthread key (musl's getspecific is
 * one tp-relative load behind a call), and only the calls that could reach
 * PIE pay for it. The same musl behaviour can bite any program whose
 * libraries' TLS happens to sum to the wrong size - noted in REPORT.txt.
 */
struct tstate {
	struct s31_rseq rs;	/* first: the allocation is aligned to 32 */
	/* 1 may register, 2 registered, 3 failed/unregistered/dead,
	 * 4 registering (a signal handler that lands here meanwhile takes the
	 * scalar side). A thread with no state (not ours) never registers. */
	int st;
};
extern int pthread_key_create(unsigned *, void (*)(void *));
extern void *pthread_getspecific(unsigned);
extern int pthread_setspecific(unsigned, const void *);
static unsigned g_key;
static int g_key_ok;
static struct tstate g_main_ts;	/* the main thread's: never freed (aligned by its type) */

static int g_on;		/* S31STR=1 */
static int g_scalar;		/* S31STR=scalar: never libc for eligible calls */
static int g_dbg;
static unsigned g_nscalar, g_npie, g_nreg, g_nregfail;
/* Diagnostics are opt-in: no shared counter writes on the normal hot path. */
static inline void count(unsigned *p)
{
	if (S31_UNLIKELY(g_dbg)) __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
}

#define DECL(ret, name, args) static ret (*l_##name) args;
DECL(void *, memcpy, (void *, const void *, size_t))
DECL(void *, memmove, (void *, const void *, size_t))
DECL(void *, memchr, (const void *, int, size_t))
DECL(void *, memrchr, (const void *, int, size_t))
DECL(int, memcmp, (const void *, const void *, size_t))
DECL(int, bcmp, (const void *, const void *, size_t))
DECL(int, strcmp, (const char *, const char *))
DECL(int, strcoll, (const char *, const char *))
DECL(size_t, strnlen, (const char *, size_t))
DECL(char *, strrchr, (const char *, int))
DECL(char *, strstr, (const char *, const char *))
DECL(void *, memmem, (const void *, size_t, const void *, size_t))
DECL(int, pthread_create, (void *, const void *, void *(*)(void *), void *))

static void rs_register(struct tstate *t)
{
	t->st = 4;
	/* Do not rely on the kernel to clear stale allocator contents. */
	t->rs.rseq_cs = 0;
	t->rs.flags = t->rs.node_id = t->rs.mm_cid = t->rs.pad = 0;
	t->rs.cpu_id_start = 0;
	t->rs.cpu_id = (uint32_t)-1;	/* RSEQ_CPU_ID_UNINITIALIZED */
	if (s31_sc(S31_NR_rseq, (long)&t->rs, sizeof(t->rs), 0, RSEQ_SIG, 0) == 0) {
		count(&g_nreg);
		t->st = 2;
	} else {
		count(&g_nregfail);
		t->st = 3;
	}
}

static int rs_unregister(struct tstate *t)
{
	long rc = 0;
	if (t->st == 2)
		rc = s31_sc(S31_NR_rseq, (long)&t->rs, sizeof(t->rs), 1 /* UNREGISTER */, RSEQ_SIG, 0);
	t->st = 3;
	return rc == 0;
}

/* A PIE-eligible call: may libc's own routine run here? */
static inline int use_libc(const void *fn)
{
	struct tstate *t;
	if (S31_UNLIKELY(!fn || g_scalar || !g_key_ok)) goto scalar;
	t = pthread_getspecific(g_key);
	if (S31_UNLIKELY(!t)) goto scalar;
	if (S31_LIKELY(t->st == 2)) {
		if (*(volatile uint32_t *)&t->rs.cpu_id == PIE_CPU) {
			count(&g_npie);
			return 1;
		}
		goto scalar;
	}
	if (t->st == 1) {
		rs_register(t);
		if (t->st == 2 && *(volatile uint32_t *)&t->rs.cpu_id == PIE_CPU) {
			count(&g_npie);
			return 1;
		}
	}
scalar:
	count(&g_nscalar);
	return 0;
}

S31_EXP void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
	int elig = !(((uintptr_t)d ^ (uintptr_t)s) & 15) && n >= ((-(uintptr_t)d) & 15) + 64;
	if (l_memcpy && (!elig || !g_on || use_libc(l_memcpy)))
		return l_memcpy(d, s, n);
	return sc_memcpy(d, s, n);
}

/* musl: if (d == s) return d; if (s - d - n <= -2n) return memcpy(d, s, n);
 * the overlapping paths are word loops without PIE */
S31_EXP void *memmove(void *d, const void *s, size_t n)
{
	uintptr_t ud = (uintptr_t)d, us = (uintptr_t)s;
	int elig = ud != us && us - ud - n <= -(2 * n) &&
		   !((ud ^ us) & 15) && n >= ((-ud) & 15) + 64;
	if (l_memmove && (!elig || !g_on || use_libc(l_memmove)))
		return l_memmove(d, s, n);
	if (!elig && ud == us) return d;
	if (elig) return sc_memcpy(d, s, n);
	/* no libc yet (before the constructor): a correct overlapping move */
	{
		unsigned char *dd = d;
		const unsigned char *ss = s;
		if (ud < us) while (n--) *dd++ = *ss++;
		else while (n--) dd[n] = ss[n];
	}
	return d;
}

S31_EXP void *memchr(const void *p, int c, size_t n)
{
	if (l_memchr && (n < 64 || !g_on || use_libc(l_memchr)))
		return l_memchr(p, c, n);
	return sc_memchr(p, c, n);
}

S31_EXP void *memrchr(const void *p, int c, size_t n)
{
	if (l_memrchr && (n < 64 || !g_on || use_libc(l_memrchr)))
		return l_memrchr(p, c, n);
	return sc_memrchr(p, c, n);
}

S31_EXP int memcmp(const void *a, const void *b, size_t n)
{
	int elig = n >= 64 && !(((uintptr_t)a ^ (uintptr_t)b) & 15);
	/* Board comparison matrix: scalar wins equal, first- and last-byte
	 * mismatch cases on CPU0 as well. Avoid both PIE and the rseq lookup. */
	if (l_memcmp && (!elig || !g_on))
		return l_memcmp(a, b, n);
	if (g_on) count(&g_nscalar);
	return sc_memcmp(a, b, n);
}

S31_EXP int bcmp(const void *a, const void *b, size_t n)
{
	int elig = n >= 64 && !(((uintptr_t)a ^ (uintptr_t)b) & 15);
	if (l_bcmp && (!elig || !g_on))
		return l_bcmp(a, b, n);
	if (g_on) count(&g_nscalar);
	return sc_memcmp(a, b, n);
}

/* eligible = same 16-byte alignment (libc then reaches PIE only after 64
 * equal non-NUL bytes; the cheap test is enough, both sides are exact) */
S31_EXP int strcmp(const char *a, const char *b)
{
	int elig = !(((uintptr_t)a ^ (uintptr_t)b) & 15);
	if (l_strcmp && (!elig || !g_on))
		return l_strcmp(a, b);
	if (g_on) count(&g_nscalar);
	return sc_strcmp(a, b);
}

/* musl: strcoll(l, r) = __strcoll_l(l, r, loc) = strcmp(l, r) */
S31_EXP int strcoll(const char *a, const char *b)
{
	int elig = !(((uintptr_t)a ^ (uintptr_t)b) & 15);
	if (l_strcoll && (!elig || !g_on))
		return l_strcoll(a, b);
	if (g_on) count(&g_nscalar);
	return sc_strcmp(a, b);
}

/* musl: p = memchr(s, 0, n); return p ? p - s : n */
S31_EXP size_t strnlen(const char *s, size_t n)
{
	const char *p;
	if (l_strnlen && (n < 64 || !g_on || use_libc(l_strnlen)))
		return l_strnlen(s, n);
	p = sc_memchr(s, 0, n);
	return p ? (size_t)(p - s) : n;
}

/* musl: memrchr(s, c, strlen(s) + 1) */
S31_EXP char *strrchr(const char *s, int c)
{
	size_t n;
	if (l_strrchr && (!g_on || use_libc(l_strrchr)))
		return l_strrchr(s, c);
	n = sc_strlen(s) + 1;
	if (n < 64 && l_memrchr)
		return l_memrchr(s, c, n);
	return sc_memrchr(s, c, n);
}

/* musl reaches PIE from strstr only for needles of 64+ bytes */
S31_EXP char *strstr(const char *h, const char *n)
{
	if (l_strstr) {
		size_t i;
		if (!g_on) return l_strstr(h, n);
		for (i = 0; i < 64 && n[i]; i++);
		if (i < 64 || use_libc(l_strstr)) return l_strstr(h, n);
	}
	return sc_strstr(h, n);
}

/* musl: if (!l) return h; if (k < l) return 0; memchr(h, *n, k) ... */
S31_EXP void *memmem(const void *h, size_t k, const void *n, size_t l)
{
	int elig = l && k >= l && k >= 64;
	if (l_memmem && (!elig || !g_on || use_libc(l_memmem)))
		return l_memmem(h, k, n, l);
	if (!l) return (void *)h;
	if (k < l) return 0;
	return sc_memmem(h, k, n, l);
}

/* ------------------------------------------------------- thread wrapper */
struct s31_ptcb { void (*f)(void *); void *x; struct s31_ptcb *next; };	/* musl struct __ptcb */
extern void _pthread_cleanup_push(struct s31_ptcb *, void (*)(void *), void *);
extern void _pthread_cleanup_pop(struct s31_ptcb *, int);
extern void *malloc(size_t);
extern void free(void *);
struct s31_start { void *(*fn)(void *); void *arg; };

/* the cleanup handler: runs on return, pthread_exit and cancellation,
 * before musl frees the thread's memory and before TSD destructors */
static void thr_done(void *raw)
{
	struct tstate *t = (struct tstate *)(((uintptr_t)raw + 31) & ~(uintptr_t)31);
	int released = rs_unregister(t);
	pthread_setspecific(g_key, 0);
	/* If unregister fails, retain the kernel-visible storage until process
	 * exit rather than hand it back to the allocator while still registered. */
	if (released) free(raw);
}

static void *thr_start(void *p)
{
	struct s31_start st;
	struct s31_ptcb cb;
	void *r, *raw;

	st.fn = ((struct s31_start *)p)->fn;
	st.arg = ((struct s31_start *)p)->arg;
	free(p);
	raw = malloc(sizeof(struct tstate) + 31);
	if (!raw || pthread_setspecific(g_key, (void *)(((uintptr_t)raw + 31) & ~(uintptr_t)31))) {
		free(raw);
		return st.fn(st.arg);	/* no state: this thread is scalar-only */
	}
	((struct tstate *)(((uintptr_t)raw + 31) & ~(uintptr_t)31))->st = 1;
	_pthread_cleanup_push(&cb, thr_done, raw);
	r = st.fn(st.arg);
	_pthread_cleanup_pop(&cb, 1);
	return r;
}

S31_EXP int pthread_create(void *th, const void *attr, void *(*fn)(void *), void *arg)
{
	struct s31_start *st;
	int r;

	if (S31_UNLIKELY(!l_pthread_create))
		l_pthread_create = (int (*)(void *, const void *, void *(*)(void *), void *))
			dlsym(S31_RTLD_NEXT, "pthread_create");
	if (!g_on || g_scalar || !g_key_ok || !(st = malloc(sizeof(*st))))
		return l_pthread_create(th, attr, fn, arg);
	st->fn = fn;
	st->arg = arg;
	r = l_pthread_create(th, attr, thr_start, st);
	if (r) free(st);
	return r;
}

/* ------------------------------------------------------------ lifecycle */
#define RES(name) l_##name = (__typeof__(l_##name))dlsym(S31_RTLD_NEXT, #name)

__attribute__((constructor)) static void s31str_init(void)
{
	const char *e = s31_env("S31STR");

	g_dbg = s31_env("S31FP_DEBUG") != 0;
	if (e && e[0] == '1') g_on = 1;
	if (e && e[0] == 's') g_on = g_scalar = 1;
	RES(pthread_create);
	RES(memmove); RES(memchr); RES(memrchr); RES(memcmp); RES(bcmp);
	RES(strcmp); RES(strcoll); RES(strnlen); RES(strrchr); RES(strstr); RES(memmem);
	RES(memcpy);	/* last: until now every memcpy took the scalar copy */
	if (g_on && !pthread_key_create(&g_key, 0)) {
		g_main_ts.st = 1;	/* the main thread: its state is static, never freed */
		g_key_ok = !pthread_setspecific(g_key, &g_main_ts);
	}
}

__attribute__((destructor)) static void s31str_fini(void)
{
	char b[160], *p = b;
	const char *t = "s31str: on=";

	if (!g_dbg) return;
	while (*t) *p++ = *t++;
	p = s31_utoa(p, (uint32_t)g_on + g_scalar);
	for (t = " eligible->libc(PIE cpu)="; *t; ) *p++ = *t++;
	p = s31_utoa(p, __atomic_load_n(&g_npie, __ATOMIC_RELAXED));
	for (t = " eligible->scalar="; *t; ) *p++ = *t++;
	p = s31_utoa(p, __atomic_load_n(&g_nscalar, __ATOMIC_RELAXED));
	for (t = " rseq_reg="; *t; ) *p++ = *t++;
	p = s31_utoa(p, __atomic_load_n(&g_nreg, __ATOMIC_RELAXED));
	for (t = " rseq_fail="; *t; ) *p++ = *t++;
	p = s31_utoa(p, __atomic_load_n(&g_nregfail, __ATOMIC_RELAXED));
	*p++ = '\n';
	*p = 0;
	s31_puts(b);
}
