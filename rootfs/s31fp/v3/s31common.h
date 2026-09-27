/*
 * s31common.h - shared by the v3 parts of libs31fp.so (s31str.c, s31clk.c).
 * Freestanding like preload3.c: raw system calls, environ read by hand.
 * The only libc imports are named where they are used (dlsym, malloc, free,
 * _pthread_cleanup_push/_pop, pthread_key_create/getspecific/setspecific),
 * all resolved once at load (-z now). The library has NO TLS segment on
 * purpose (s31str.c, struct tstate).
 */
#ifndef S31COMMON_H
#define S31COMMON_H
#include <stddef.h>
#include <stdint.h>

#define S31_HID __attribute__((visibility("hidden")))
#define S31_EXP __attribute__((visibility("default")))
#define S31_LIKELY(x) __builtin_expect(!!(x), 1)
#define S31_UNLIKELY(x) __builtin_expect(!!(x), 0)

extern char **environ;
extern void *dlsym(void *, const char *);
#define S31_RTLD_NEXT ((void *)-1)	/* musl <dlfcn.h> */

static inline long s31_sc(long n, long a, long b, long c, long d, long e)
{
	register long a7 __asm__("a7") = n, a0 __asm__("a0") = a, a1 __asm__("a1") = b,
		a2 __asm__("a2") = c, a3 __asm__("a3") = d, a4 __asm__("a4") = e;
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4) : "memory");
	return a0;
}
#define S31_NR_openat 56
#define S31_NR_close 57
#define S31_NR_read 63
#define S31_NR_write 64
#define S31_NR_sched_yield 124
#define S31_NR_rseq 293
#define S31_NR_clock_gettime64 403

static inline const char *s31_env(const char *k)
{
	for (char **e = environ; e && *e; e++) {
		const char *p = *e, *q = k;
		while (*q && *p == *q) p++, q++;
		if (!*q && *p == '=') return p + 1;
	}
	return 0;
}

/* stderr, no libc: "tag name=value ..." lines for S31FP_DEBUG */
static inline void s31_puts(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	s31_sc(S31_NR_write, 2, (long)s, (long)n, 0, 0);
}
static inline char *s31_utoa(char *p, uint32_t v)
{
	char t[12]; int n = 0;
	do t[n++] = (char)('0' + v % 10); while (v /= 10);
	while (n) *p++ = t[--n];
	return p;
}
#endif
