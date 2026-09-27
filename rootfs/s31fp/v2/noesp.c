/* QEMU-only: plain versions of the libc routines that this toolchain's musl
 * implements with Espressif PIE vector instructions QEMU cannot execute. */
#include <stddef.h>
void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return 0;
}
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) a++, b++;
	return *(const unsigned char *)a - *(const unsigned char *)b;
}
void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *q = d; const unsigned char *p = s;
	while (n--) *q++ = *p++;
	return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}
