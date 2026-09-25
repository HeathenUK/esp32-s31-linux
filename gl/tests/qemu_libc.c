/*
 * qemu_libc.c - scalar versions of the musl routines the S31 toolchain
 * vectorises with the ESP PIE extension (esp.vld, esp.vcmp ...), which
 * qemu-riscv32 cannot execute. Linked ONLY into the *.qemu test builds, so
 * the RV32 objects of libGL can be exercised on the host; the board builds
 * keep the real (vectorised) libc. s31, MIT.
 */
#include <stddef.h>

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

void *__memrchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s + n;
	while (n--)
		if (*--p == (unsigned char)c)
			return (void *)p;
	return NULL;
}
void *memrchr(const void *s, int c, size_t n) __attribute__((alias("__memrchr")));

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

int strcmp(const char *a, const char *b)
{
	for (; *a && *a == *b; a++, b++)
		;
	return *(const unsigned char *)a - *(const unsigned char *)b;
}

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	if (p < q) {
		while (n--)
			*p++ = *q++;
	} else {
		p += n;
		q += n;
		while (n--)
			*--p = *--q;
	}
	return d;
}
