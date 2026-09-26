/*
 * lentcpu.c - the string functions lvdesk calls, without PIE.
 *
 * WHY (2026-09-26, the glxgears fullscreen frame dips). musl's libc.so on
 * this board carries PIE - the S31's hart-1-only SIMD - in strcmp, memcmp,
 * memchr and memrchr, AND in memcpy: from 64 bytes up (plus the alignment
 * lead-in) memcpy calls a 128-bit esp.vld/esp.vst copier that sits after
 * strcmp in libc.so (objdump: memcpy+0x4e jal strcmp+0xd8). Found with the
 * #394 kernel's esp32s31_pie_log: lvdesk and glxgears both trapped at
 * libc+0x107b0 called from libc+0x5894a, a 64-byte memcpy.
 *
 * Linux CPU1 is hart 0, the CPU FreeRTOS lends, and it has no PIE: a task there that reaches one of
 * them takes an illegal-instruction trap and the kernel moves it to CPU0
 * (arch/riscv/kernel/esp32s31-ext.c, esp32s31_pie_bounce), where the stock
 * scheduler leaves it until a balance or a wake-up happens to move it back.
 *
 * lvdesk runs on CPU1 beside a fullscreen client on CPU0. One stray PIE
 * call put the desktop on the client's CPU; the client was pushed to CPU1, hit
 * PIE there itself (libGL's glBegin 64-byte memcpy + memcmp, 4 a frame) and bounced back,
 * and the pair ping-ponged for 1-3 s - the "dip clusters": 9-10
 * involuntary switches per client frame instead of 1.3, frames of 45-110 ms.
 * Measured with rootfs/swapstamp.so and the kernel's bounce counter: long
 * frames carried 0.86-1.18 bounces each, normal frames 0.001-0.005.
 *
 * Defining the functions in the executable makes every call through a PLT
 * - lvdesk's own and libasound's - land here instead of in libc. Calls libc
 * makes internally (fgets and sscanf -> memchr, strstr -> memchr, strrchr ->
 * memrchr, printf's %s -> strnlen -> memchr) are bound inside libc.so and
 * cannot be redirected, so the periodic paths do not use those entry
 * points at all (lvdesk.c sysinfo_update, fsg_read_majflt and friends);
 * strstr, strrchr and strnlen are defined here for the same reason.
 *
 * DISPATCH (owner's direction, 2026-09-26: the best of both). Each call
 * reads the CPU the thread is on from its rseq area - one load, the kernel
 * keeps it current (CONFIG_RSEQ; musl does not register one, so this file
 * does, per thread, on first use) - and on Linux CPU0 (hart 1, which has
 * PIE) calls libc's own routine, found once with dlsym(RTLD_NEXT), so the
 * 128-bit copier and compares still serve lvdesk there. Anywhere else, or
 * when rseq or dlsym is unavailable, the scalar loop below runs. The check
 * is not atomic with the call: a migration in the few cycles between them
 * lands in the old trap-and-bounce, which is correct, only rare now.
 * LVDESK_LENTCPU=scalar forces the scalar side everywhere (the A/B arm);
 * the stock libc arm is simply the previous binary.
 *
 * The compare/search functions are byte loops: lvdesk calls them a few
 * times a second (counted with rootfs/nopie.so). memcpy/memmove are word
 * loops, eight words a turn when source and destination share their
 * alignment - they carry the windowed fast-present row copies. The
 * loop-pattern pass is off so GCC cannot turn a loop back into a call to
 * itself.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LC __attribute__((optimize("no-tree-loop-distribute-patterns")))

static LC int sc_strcmp(const char *a, const char *b)
{
	const unsigned char *p = (const unsigned char *)a;
	const unsigned char *q = (const unsigned char *)b;

	while (*p && *p == *q)
		p++, q++;
	return *p - *q;
}

static LC int sc_memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;

	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

static LC void *sc_memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

static LC void *sc_memrchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s + n;

	while (n--)
		if (*--p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

static LC size_t sc_strnlen(const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n && s[i]; i++)
		;
	return i;
}

static LC char *sc_strrchr(const char *s, int c)
{
	const char *r = NULL;

	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

static LC char *sc_strstr(const char *h, const char *n)
{
	size_t i;

	if (!*n)
		return (char *)h;
	for (; *h; h++) {
		for (i = 0; n[i] && h[i] == n[i]; i++)
			;
		if (!n[i])
			return (char *)h;
	}
	return NULL;
}

static LC void *sc_memcpy(void *restrict dst, const void *restrict src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
		uint32_t *dw;
		const uint32_t *sw;

		while (n && ((uintptr_t)d & 3)) {
			*d++ = *s++;
			n--;
		}
		dw = (uint32_t *)d;
		sw = (const uint32_t *)s;
		for (; n >= 32; n -= 32, dw += 8, sw += 8) {
			uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			uint32_t f = sw[4], g = sw[5], h = sw[6], i = sw[7];

			dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
			dw[4] = f; dw[5] = g; dw[6] = h; dw[7] = i;
		}
		for (; n >= 4; n -= 4)
			*dw++ = *sw++;
		d = (unsigned char *)dw;
		s = (const unsigned char *)sw;
	} else if ((((uintptr_t)d ^ (uintptr_t)s) & 1) == 0) {
		uint16_t *dh;
		const uint16_t *sh;

		if (n && ((uintptr_t)d & 1)) {
			*d++ = *s++;
			n--;
		}
		dh = (uint16_t *)d;
		sh = (const uint16_t *)s;
		for (; n >= 2; n -= 2)
			*dh++ = *sh++;
		d = (unsigned char *)dh;
		s = (const unsigned char *)sh;
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

/* musl's memmove calls its memcpy internally (bound inside libc, so PIE) */
static LC void *sc_memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d == s || !n)
		return dst;
	if (d < s || d >= s + n)
		return sc_memcpy(dst, src, n);
	d += n;
	s += n;
	if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
		while (n && ((uintptr_t)d & 3)) {
			*--d = *--s;
			n--;
		}
		for (; n >= 4; n -= 4) {
			d -= 4;
			s -= 4;
			*(uint32_t *)d = *(const uint32_t *)s;
		}
	}
	while (n--)
		*--d = *--s;
	return dst;
}

/* ------------------------------------------------------------ dispatch */
#include <dlfcn.h>
#include <linux/rseq.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PIE_CPU		0	/* Linux CPU0 is hart 1, the one with PIE */
#define RSEQ_SIG	0x53053053

static __thread struct rseq lc_rs __attribute__((aligned(32)));
static __thread int lc_rs_state;	/* 0 untried, 1 registered, 2 failed */
static int lc_off;			/* LVDESK_LENTCPU=scalar */

static int (*lc_strcmp)(const char *, const char *);
static int (*lc_memcmp)(const void *, const void *, size_t);
static void *(*lc_memchr)(const void *, int, size_t);
static void *(*lc_memrchr)(const void *, int, size_t);
static size_t (*lc_strnlen)(const char *, size_t);
static char *(*lc_strrchr)(const char *, int);
static char *(*lc_strstr)(const char *, const char *);
static void *(*lc_memcpy)(void *, const void *, size_t);
static void *(*lc_memmove)(void *, const void *, size_t);

__attribute__((constructor)) static void lc_init(void)
{
	const char *e = getenv("LVDESK_LENTCPU");

	if (e && !sc_strcmp(e, "scalar")) {
		lc_off = 1;
		return;
	}
	lc_strcmp = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "strcmp");
	lc_memcmp = (int (*)(const void *, const void *, size_t))dlsym(RTLD_NEXT, "memcmp");
	lc_memchr = (void *(*)(const void *, int, size_t))dlsym(RTLD_NEXT, "memchr");
	lc_memrchr = (void *(*)(const void *, int, size_t))dlsym(RTLD_NEXT, "memrchr");
	lc_strnlen = (size_t (*)(const char *, size_t))dlsym(RTLD_NEXT, "strnlen");
	lc_strrchr = (char *(*)(const char *, int))dlsym(RTLD_NEXT, "strrchr");
	lc_strstr = (char *(*)(const char *, const char *))dlsym(RTLD_NEXT, "strstr");
	lc_memcpy = (void *(*)(void *, const void *, size_t))dlsym(RTLD_NEXT, "memcpy");
	lc_memmove = (void *(*)(void *, const void *, size_t))dlsym(RTLD_NEXT, "memmove");
}

static void lc_rs_register(void)
{
	lc_rs.cpu_id_start = 0;
	lc_rs.cpu_id = (uint32_t)RSEQ_CPU_ID_UNINITIALIZED;
	lc_rs_state = syscall(__NR_rseq, &lc_rs, sizeof(lc_rs), 0, RSEQ_SIG) == 0
		      ? 1 : 2;
	{
		static int said;

		/* once per process; no %s (printf's strnlen is libc's own) */
		if (!said++)
			fprintf(stderr, "lvdesk: lentcpu rseq=%d cpu=%d libc=%d\n",
				lc_rs_state == 1, (int)lc_rs.cpu_id,
				lc_memcpy != NULL);
	}
}

/* on the PIE CPU right now (and libc's routine known)? */
static inline int lc_pie(const void *fn)
{
	if (__builtin_expect(lc_rs_state != 1, 0)) {
		if (lc_off || lc_rs_state)
			return 0;
		lc_rs_register();
		if (lc_rs_state != 1)
			return 0;
	}
	return fn && *(volatile uint32_t *)&lc_rs.cpu_id == PIE_CPU;
}

int strcmp(const char *a, const char *b)
{
	return lc_pie(lc_strcmp) ? lc_strcmp(a, b) : sc_strcmp(a, b);
}

int memcmp(const void *a, const void *b, size_t n)
{
	return lc_pie(lc_memcmp) ? lc_memcmp(a, b, n) : sc_memcmp(a, b, n);
}

void *memchr(const void *p, int c, size_t n)
{
	return lc_pie(lc_memchr) ? lc_memchr(p, c, n) : sc_memchr(p, c, n);
}

void *memrchr(const void *p, int c, size_t n)
{
	return lc_pie(lc_memrchr) ? lc_memrchr(p, c, n) : sc_memrchr(p, c, n);
}

size_t strnlen(const char *p, size_t n)
{
	return lc_pie(lc_strnlen) ? lc_strnlen(p, n) : sc_strnlen(p, n);
}

char *strrchr(const char *p, int c)
{
	return lc_pie(lc_strrchr) ? lc_strrchr(p, c) : sc_strrchr(p, c);
}

char *strstr(const char *h, const char *n)
{
	return lc_pie(lc_strstr) ? lc_strstr(h, n) : sc_strstr(h, n);
}

/* below 64 bytes libc's memcpy never reaches its PIE copier: take it
 * without asking where we are */
void *memcpy(void *restrict d, const void *restrict p, size_t n)
{
	if (n < 64 && lc_memcpy)
		return lc_memcpy(d, p, n);
	return lc_pie(lc_memcpy) ? lc_memcpy(d, p, n) : sc_memcpy(d, p, n);
}

void *memmove(void *d, const void *p, size_t n)
{
	return lc_pie(lc_memmove) ? lc_memmove(d, p, n) : sc_memmove(d, p, n);
}
