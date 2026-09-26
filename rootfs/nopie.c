/* nopie - which calls put a process through the lent CPU's PIE trap.
 *
 * Diagnostic LD_PRELOAD (a measuring instrument; it never ships as a fix).
 * musl's libc.so carries PIE (the S31's hart-1-only SIMD extension) in
 * exactly four functions: strcmp, memcmp, memchr, memrchr
 * (docs: s31-asymmetric-harts-keep-pie). hart 0 - Linux CPU1, the CPU
 * FreeRTOS lends - has no PIE, so a task on CPU1 that calls one of them
 * takes an illegal-instruction trap and the kernel migrates it to CPU0
 * (arch/riscv/kernel/esp32s31-ext.c, esp32s31_pie_bounce). This preload
 * replaces the four with plain C, so calls that go through the PLT (the
 * executable's own and every other library's) no longer trap, and counts
 * each by caller. Calls made INSIDE libc (fgets -> memchr, strrchr ->
 * __memrchr, ...) are bound within libc and still trap: whatever bounce
 * rate is left with this loaded is theirs.
 *
 * It also counts, by caller, the libc entry points whose musl
 * implementation reaches one of the four internally (so still traps):
 * fgets/getline/getdelim (memchr), strnlen and every printf with %s
 * (strnlen -> memchr), sscanf (its string reader uses memchr), strstr
 * (memchr), strrchr (__memrchr), fwrite/fputs/puts on a line-buffered
 * stream (memrchr). They are only counted; the real function still runs.
 *
 * SIGUSR2 dumps "func caller count" to NOPIE_OUT (default /root/gq/nopie.txt)
 * with the process's r-xp mappings, so callers resolve against a symbol
 * table (scripts/board/h1s-report.py's maps logic, or by hand).
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-nopie.sh'
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOTS 256
static struct { unsigned long pc; unsigned int n; unsigned char f; } slot[SLOTS];
static unsigned int dropped;

static void note(int f, void *ra)
{
	unsigned long pc = (unsigned long)ra;
	unsigned int h = (unsigned int)((pc >> 1) * 2654435761u) % SLOTS, i;

	for (i = 0; i < SLOTS; i++, h = (h + 1) % SLOTS) {
		if (slot[h].pc == pc && slot[h].f == f) {
			slot[h].n++;
			return;
		}
		if (!slot[h].pc) {
			slot[h].pc = pc;
			slot[h].f = (unsigned char)f;
			slot[h].n = 1;
			return;
		}
	}
	dropped++;
}

static const char *fname[] = { "strcmp", "memcmp", "memchr", "memrchr",
	"fgets", "getdelim", "strnlen", "strstr", "strrchr", "sscanf",
	"vsscanf", "snprintf", "vsnprintf", "sprintf", "vsprintf", "printf",
	"fprintf", "vfprintf", "vprintf", "fputs", "puts", "fwrite" };

static void dump(int sig)
{
	char b[256];
	const char *o = getenv("NOPIE_OUT");
	int fd = open(o ? o : "/root/gq/nopie.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644), i, l, m;

	(void)sig;
	if (fd < 0)
		return;
	for (i = 0; i < SLOTS; i++)
		if (slot[i].pc) {
			l = snprintf(b, sizeof b, "%s %lx %u\n", fname[slot[i].f],
				     slot[i].pc, slot[i].n);
			write(fd, b, l);
		}
	l = snprintf(b, sizeof b, "dropped %u\n--- maps\n", dropped);
	write(fd, b, l);
	m = open("/proc/self/maps", O_RDONLY);
	if (m >= 0) {
		while ((l = read(m, b, sizeof b)) > 0)
			write(fd, b, l);
		close(m);
	}
	close(fd);
}

__attribute__((constructor)) static void init(void)
{
	signal(SIGUSR2, dump);
}

int strcmp(const char *a, const char *b)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;

	note(0, __builtin_return_address(0));
	while (*p && *p == *q)
		p++, q++;
	return *p - *q;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;

	note(1, __builtin_return_address(0));
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	note(2, __builtin_return_address(0));
	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

void *memrchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s + n;

	note(3, __builtin_return_address(0));
	while (n--)
		if (*--p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

#define REAL(ret, name, args) \
	static ret (*real_##name) args; \
	if (!real_##name) real_##name = (ret (*) args)dlsym(RTLD_NEXT, #name)

char *fgets(char *b, int n, FILE *f)
{
	REAL(char *, fgets, (char *, int, FILE *));
	note(4, __builtin_return_address(0));
	return real_fgets(b, n, f);
}

ssize_t getdelim(char **l, size_t *n, int d, FILE *f)
{
	REAL(ssize_t, getdelim, (char **, size_t *, int, FILE *));
	note(5, __builtin_return_address(0));
	return real_getdelim(l, n, d, f);
}

ssize_t getline(char **l, size_t *n, FILE *f)
{
	REAL(ssize_t, getdelim, (char **, size_t *, int, FILE *));
	note(5, __builtin_return_address(0));
	return real_getdelim(l, n, '\n', f);
}

size_t strnlen(const char *str, size_t n)
{
	size_t i;

	note(6, __builtin_return_address(0));
	for (i = 0; i < n && str[i]; i++)
		;
	return i;
}

char *strstr(const char *h, const char *n)
{
	REAL(char *, strstr, (const char *, const char *));
	note(7, __builtin_return_address(0));
	return real_strstr(h, n);
}

char *strrchr(const char *str, int c)
{
	REAL(char *, strrchr, (const char *, int));
	note(8, __builtin_return_address(0));
	return real_strrchr(str, c);
}

int vsscanf(const char *str, const char *fmt, va_list ap)
{
	REAL(int, vsscanf, (const char *, const char *, va_list));
	note(10, __builtin_return_address(0));
	return real_vsscanf(str, fmt, ap);
}

int sscanf(const char *str, const char *fmt, ...)
{
	va_list ap;
	int r;
	REAL(int, vsscanf, (const char *, const char *, va_list));

	note(9, __builtin_return_address(0));
	va_start(ap, fmt);
	r = real_vsscanf(str, fmt, ap);
	va_end(ap);
	return r;
}

int vsnprintf(char *b, size_t n, const char *fmt, va_list ap)
{
	REAL(int, vsnprintf, (char *, size_t, const char *, va_list));
	note(12, __builtin_return_address(0));
	return real_vsnprintf(b, n, fmt, ap);
}

int snprintf(char *b, size_t n, const char *fmt, ...)
{
	va_list ap;
	int r;
	REAL(int, vsnprintf, (char *, size_t, const char *, va_list));

	note(11, __builtin_return_address(0));
	va_start(ap, fmt);
	r = real_vsnprintf(b, n, fmt, ap);
	va_end(ap);
	return r;
}

int vsprintf(char *b, const char *fmt, va_list ap)
{
	REAL(int, vsprintf, (char *, const char *, va_list));
	note(14, __builtin_return_address(0));
	return real_vsprintf(b, fmt, ap);
}

int sprintf(char *b, const char *fmt, ...)
{
	va_list ap;
	int r;
	REAL(int, vsprintf, (char *, const char *, va_list));

	note(13, __builtin_return_address(0));
	va_start(ap, fmt);
	r = real_vsprintf(b, fmt, ap);
	va_end(ap);
	return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	REAL(int, vfprintf, (FILE *, const char *, va_list));
	note(17, __builtin_return_address(0));
	return real_vfprintf(f, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int r;
	REAL(int, vfprintf, (FILE *, const char *, va_list));

	note(16, __builtin_return_address(0));
	va_start(ap, fmt);
	r = real_vfprintf(f, fmt, ap);
	va_end(ap);
	return r;
}

int vprintf(const char *fmt, va_list ap)
{
	REAL(int, vfprintf, (FILE *, const char *, va_list));
	note(18, __builtin_return_address(0));
	return real_vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
	va_list ap;
	int r;
	REAL(int, vfprintf, (FILE *, const char *, va_list));

	note(15, __builtin_return_address(0));
	va_start(ap, fmt);
	r = real_vfprintf(stdout, fmt, ap);
	va_end(ap);
	return r;
}

int fputs(const char *str, FILE *f)
{
	REAL(int, fputs, (const char *, FILE *));
	note(19, __builtin_return_address(0));
	return real_fputs(str, f);
}

int puts(const char *str)
{
	REAL(int, puts, (const char *));
	note(20, __builtin_return_address(0));
	return real_puts(str);
}

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
	REAL(size_t, fwrite, (const void *, size_t, size_t, FILE *));
	note(21, __builtin_return_address(0));
	return real_fwrite(p, sz, n, f);
}
