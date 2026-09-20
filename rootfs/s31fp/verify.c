/* Board-side: s31fp routines vs libgcc (the trusted reference on this target).
 * usage: s31fp-verify [millions]   exit 0 = identical on every operand pair */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
double s31fp_muldf3(double, double), s31fp_adddf3(double, double), s31fp_subdf3(double, double);
static uint64_t s = 88172645463325252ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double mk(void)
{
	uint64_t r = rnd(), u = r; unsigned k = rnd() % 20; double d;
	if (k == 0) u = r & 0x800FFFFFFFFFFFFFULL;
	else if (k == 1) u = (r & 0x8000000000000000ULL) | 0x7FF0000000000000ULL;
	else if (k == 2) u = r | 0x7FF0000000000000ULL;
	else if (k == 3) u = r & 0x8000000000000000ULL;
	else if (k == 4) u = (r & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(1023 + (int)(rnd() % 8) - 4) << 52);
	else if (k == 5) u = (r & 0x8000000000000001ULL) | 0x3FF0000000000000ULL;
	else if (k == 6) u = (r & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(rnd() % 64) << 52);
	else if (k == 7) u = (r & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(2046 - rnd() % 64) << 52);
	else if (k == 8) u = (r & 0xFFFFFFFF00000000ULL & ~0x7FF0000000000000ULL) | ((uint64_t)(1000 + rnd() % 60) << 52); /* low word 0 */
	else if (k == 9) { d = (double)(int32_t)r; return d; }			/* an int */
	else if (k == 10) { d = (double)(float)(int32_t)r / 65536.0f; return d; } /* a float */
	else if (k == 11) u = (r | 0x000FFFFFFFFFFFFFULL) & ~0x7FF0000000000000ULL | ((uint64_t)(1023 + rnd() % 3) << 52); /* all ones: carries */
	else if (k < 16) u = (r & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(1023 - 40 + rnd() % 80) << 52);
	memcpy(&d, &u, 8); return d;
}
static int same(double a, double b) { return (a != a && b != b) || bits(a) == bits(b); }
int main(int argc, char **argv)
{
	long n = (argc > 1 ? atol(argv[1]) : 3) * 1000000L, i, bad = 0;
	for (i = 0; i < n; i++) {
		volatile double a = mk(), b = mk();
		double m = a * b, ad = a + b, sb = a - b;	/* libgcc */
		if (!same(s31fp_muldf3(a, b), m) && bad++ < 8)
			printf("MUL %016llx %016llx got %016llx want %016llx\n", (unsigned long long)bits(a), (unsigned long long)bits(b), (unsigned long long)bits(s31fp_muldf3(a, b)), (unsigned long long)bits(m));
		if (!same(s31fp_adddf3(a, b), ad) && bad++ < 8) printf("ADD mismatch\n");
		if (!same(s31fp_subdf3(a, b), sb) && bad++ < 8) printf("SUB mismatch\n");
	}
	printf("s31fp-verify: %ld operand pairs x3 ops, %ld mismatches\n", n, bad);
	return bad != 0;
}
