/* s31_f2d_bits() and s31_i2d_bits() (phase 3a G01) against the compiler's
   (double)f and (double)i: every one of the 2^32 float bit patterns (NaN
   payloads compared as NaN), and every 1021st int plus the extremes.
   Host: cc -O2 -Igl/tinygl/source gl/tests/f2d_test.c. s31, MIT. */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "s31_float.h"
int main(void)
{
	unsigned long long bad = 0, n = 0;
	unsigned int b = 0;
	long long i;
	do {
		float f; double a, c;
		memcpy(&f, &b, 4);
		a = (double)f; c = s31_f2d_bits(f);
		if (f == f ? memcmp(&a, &c, 8) != 0 : c == c) { if (bad < 10) printf("f2d %08x\n", b); bad++; }
		n++;
	} while (++b != 0);
	for (i = INT_MIN; i <= INT_MAX; i += 1021) {
		double a = (double)(int)i, c = s31_i2d_bits((int)i);
		if (memcmp(&a, &c, 8)) { if (bad < 20) printf("i2d %lld\n", i); bad++; }
		n++;
	}
	{ int ex[] = { INT_MIN, INT_MAX, -1, 1, 0, 2, 3, 1 << 30, -(1 << 30) }; unsigned k;
	  for (k = 0; k < sizeof ex / sizeof *ex; k++) { double a = ex[k], c = s31_i2d_bits(ex[k]);
	    if (memcmp(&a, &c, 8)) { printf("i2d %d\n", ex[k]); bad++; } n++; } }
	printf("f2d/i2d: %llu tested, %llu mismatches\n", n, bad);
	return bad != 0;
}
