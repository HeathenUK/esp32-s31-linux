/*
 * s31fp - fast software double arithmetic for a core with F and no D.
 *
 * WHY. rv32imafc here has a real single-precision FPU and no double; every
 * `double` operation is a libgcc call. Measured 2026-09-20 on the board:
 * __muldf3 705 ns, __adddf3 420 ns, against 25 ns for a hardware float
 * multiply. libgcc's soft-fp is written for generality (exception flags,
 * rounding modes, any word size). OpenTyrian's FM synthesiser is double
 * throughout and spent half its audio thread in __muldf3 - over 100% of the
 * core at 44.1 kHz, so its sound could never play.
 *
 * These are round-to-nearest-even, no exception flags: exactly what the C
 * environment on this target provides anyway (there is no fenv for double).
 * Algorithms follow compiler-rt's fp_mul_impl/fp_add_impl. Verified
 * bit-for-bit against hardware IEEE double with test.c (host build).
 *
 * Objects on the link line win over libgcc.a members, so a program picks
 * these up just by linking s31fp.o / libs31fp.a first; no source changes.
 */
#include <stdint.h>

typedef uint64_t rep_t;
typedef union { double f; rep_t i; } du;

#define SIGN	0x8000000000000000ULL
#define IMPL	0x0010000000000000ULL
#define SIGM	0x000FFFFFFFFFFFFFULL
#define ABSM	0x7FFFFFFFFFFFFFFFULL
#define INFR	0x7FF0000000000000ULL
#define QNAN	0x0008000000000000ULL

#ifdef S31FP_TEST
#define FN(n) s31_##n
#elif defined(S31FP_PRELOAD)
#define FN(n) s31fp_##n		/* reached by a patched jump, not by name */
#else
#define FN(n) __##n
#endif

static inline int clz64(rep_t x)
{
	uint32_t hi = (uint32_t)(x >> 32);

	return hi ? __builtin_clz(hi) : 32 + __builtin_clz((uint32_t)x);
}

static inline int normalize(rep_t *sig)
{
	int shift = clz64(*sig) - 11;

	*sig <<= shift;
	return 1 - shift;
}

#ifdef S31FP_ASM_MUL
#define MULNAME s31fp_muldf3_c	/* the asm fast path tail-calls this */
#else
#define MULNAME FN(muldf3)
#endif
double MULNAME(double x, double y)
{
	du ua = { x }, ub = { y }, r;
	rep_t a = ua.i, b = ub.i;
	unsigned ae = (unsigned)(a >> 52) & 0x7FF, be = (unsigned)(b >> 52) & 0x7FF;
	rep_t sign = (a ^ b) & SIGN;
	rep_t as = a & SIGM, bs = b & SIGM, hi, lo;
	int scale = 0, pe;

	if (ae - 1U >= 0x7FEU || be - 1U >= 0x7FEU) {
		rep_t aa = a & ABSM, ba = b & ABSM;

		if (aa > INFR) { r.i = a | QNAN; return r.f; }
		if (ba > INFR) { r.i = b | QNAN; return r.f; }
		if (aa == INFR) { r.i = ba ? (aa | sign) : (INFR | QNAN); return r.f; }
		if (ba == INFR) { r.i = aa ? (ba | sign) : (INFR | QNAN); return r.f; }
		if (!aa || !ba) { r.i = sign; return r.f; }
		if (aa < IMPL) scale += normalize(&as);
		if (ba < IMPL) scale += normalize(&bs);
	}
	as |= IMPL;
	bs = (bs | IMPL) << 11;
	{	/* 64x64 -> 128 from four 32x32 products */
		uint32_t a1 = (uint32_t)(as >> 32), a0 = (uint32_t)as;
		uint32_t b1 = (uint32_t)(bs >> 32), b0 = (uint32_t)bs;
		rep_t p00 = (rep_t)a0 * b0, p01 = (rep_t)a0 * b1;
		rep_t p10 = (rep_t)a1 * b0, p11 = (rep_t)a1 * b1;
		rep_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;

		lo = (mid << 32) | (uint32_t)p00;
		hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
	}
	pe = (int)ae + (int)be - 1023 + scale;
	if (hi & IMPL)
		pe++;
	else {
		hi = (hi << 1) | (lo >> 63);
		lo <<= 1;
	}
	if (pe >= 0x7FF) { r.i = INFR | sign; return r.f; }
	if (pe <= 0) {
		unsigned shift = 1U - (unsigned)pe;
		int sticky;

		if (shift >= 64) { r.i = sign; return r.f; }
		sticky = (lo << (64 - shift)) != 0;
		lo = (hi << (64 - shift)) | (lo >> shift) | (rep_t)sticky;
		hi >>= shift;
	} else {
		hi &= SIGM;
		hi |= (rep_t)pe << 52;
	}
	hi |= sign;
	if (lo > SIGN) hi++;
	else if (lo == SIGN) hi += hi & 1;
	r.i = hi;
	return r.f;
}

static double addsub(rep_t a, rep_t b)
{
	du r;
	rep_t aa = a & ABSM, ba = b & ABSM, as, bs, rsign;
	int ae, be, sub;
	unsigned align;

	if (aa - 1ULL >= INFR - 1ULL || ba - 1ULL >= INFR - 1ULL) {
		if (aa > INFR) { r.i = a | QNAN; return r.f; }
		if (ba > INFR) { r.i = b | QNAN; return r.f; }
		if (aa == INFR) {
			r.i = ((a ^ b) == SIGN) ? (INFR | QNAN) : a;
			return r.f;
		}
		if (ba == INFR) { r.i = b; return r.f; }
		if (!aa) { r.i = ba ? b : (a & b); return r.f; }
		if (!ba) { r.i = a; return r.f; }
	}
	if (ba > aa) { rep_t t = a; a = b; b = t; }
	ae = (int)(a >> 52) & 0x7FF; be = (int)(b >> 52) & 0x7FF;
	as = a & SIGM; bs = b & SIGM;
	if (!ae) ae = normalize(&as);
	if (!be) be = normalize(&bs);
	rsign = a & SIGN;
	sub = ((a ^ b) & SIGN) != 0;
	as = (as | IMPL) << 3;
	bs = (bs | IMPL) << 3;
	align = (unsigned)(ae - be);
	if (align) {
		if (align < 64) {
			int sticky = (bs << (64 - align)) != 0;

			bs = (bs >> align) | (rep_t)sticky;
		} else
			bs = 1;
	}
	if (sub) {
		as -= bs;
		if (!as) { r.i = 0; return r.f; }
		if (as < (IMPL << 3)) {
			int shift = clz64(as) - clz64(IMPL << 3);

			as <<= shift;
			ae -= shift;
		}
	} else {
		as += bs;
		if (as & (IMPL << 4)) {
			int sticky = (int)(as & 1);

			as = (as >> 1) | (rep_t)sticky;
			ae++;
		}
	}
	if (ae >= 0x7FF) { r.i = INFR | rsign; return r.f; }
	if (ae <= 0) {
		int shift = 1 - ae;
		int sticky = (as << (64 - shift)) != 0;

		as = (as >> shift) | (rep_t)sticky;
		ae = 0;
	}
	{
		int round = (int)(as & 7);
		rep_t res = (as >> 3) & SIGM;

		res |= (rep_t)ae << 52;
		res |= rsign;
		if (round > 4) res++;
		else if (round == 4) res += res & 1;
		r.i = res;
	}
	return r.f;
}

double FN(adddf3)(double x, double y)
{
	du a = { x }, b = { y };

	return addsub(a.i, b.i);
}

double FN(subdf3)(double x, double y)
{
	du a = { x }, b = { y };

	return addsub(a.i, b.i ^ SIGN);
}
