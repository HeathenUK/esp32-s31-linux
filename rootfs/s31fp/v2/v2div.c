/*
 * s31v2_divdf3 - binary64 divide, bit-exact to this target's libgcc
 * (results, canonical NaNs, fflags, frm), same split as v2.S: normal operands
 * with a normal result under RNE are done here; everything else - zeros
 * (except 0/finite), subnormals, inf, NaN, x/0, results leaving the normal
 * range, frm != RNE - is libgcc's own routine, linked in renamed.
 *
 * Algorithm (Q = mx/my in [1,2), T = floor(Q * 2^53), 54 bits):
 *   seed   R0 = 2^31 / b from the hardware fdiv.s (b = my's top 24 bits),
 *          |rel err| < 1.5 * 2^-23; fflags saved/restored around it so the
 *          FPU's own NX never leaks
 *   q0     = (mx >> 22) * R0 >> 31            ~ Q * 2^30, |err| < ~2^8.6
 *   E1     = mx*2^30 - q0*my   (exact, mod 2^64; |E1| < 2^61.7)
 *   T1     = q0*2^23 + (E1>>31)*R0 >> 29      |err| < ~2^9.3
 *   E2     = mx*2^53 - T1*my   (exact, mod 2^64; |E2| < 2^62.3)
 *   T2     = T1 + (E2>>32)*R0 >> 51           |err| <= 2
 *   final  remainder check: rem = mx*2^53 - T*my, step T until 0 <= rem < my;
 *          round bit = T & 1, sticky = rem != 0 - correctly rounded by
 *          construction, independent of how good the estimate was.
 * The "mod 2^64" products need only the low halves: 3 multiplies each.
 */
#include <stdint.h>

typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t u32;

u64 s31lg_divdf3(u64, u64);

#ifndef S31V2_FENV
#define S31V2_FENV 1
#endif
#ifndef S31V2_FFLAGS
#define S31V2_FFLAGS S31V2_FENV
#endif
#define SIGN	0x8000000000000000ULL
#define IMPL	0x0010000000000000ULL
#define SIGM	0x000FFFFFFFFFFFFFULL

static inline u64 lo64(u64 a, u64 b)	/* low 64 bits of a*b */
{
	return a * b;
}

u64 s31v2_divdf3(u64 x, u64 y)
{
	u32 xh = (u32)(x >> 32), yh = (u32)(y >> 32);
	unsigned ex = (xh >> 20) & 0x7ff, ey = (yh >> 20) & 0x7ff;
	u64 sign = (x ^ y) & SIGN, mx, my, T, rem, res;
	s64 E;
	u32 R0, q0, fl;
	int e;

#if S31V2_FENV
	u32 fcsr;	/* one CSR read: frm (7:5) and the accrued flags (4:0), see v2.S */

	__asm__ volatile("frcsr %0" : "=r"(fcsr));
	if (fcsr >> 5)
		return s31lg_divdf3(x, y);
#endif
	if (ex - 1U >= 0x7feU || ey - 1U >= 0x7feU) {
		/* +-0 / finite nonzero (normal or subnormal) = signed zero, exact */
		if (!(x << 1) && ey != 0x7ff && (y << 1))
			return sign;
		return s31lg_divdf3(x, y);
	}
	mx = (x & SIGM) | IMPL;
	my = (y & SIGM) | IMPL;
	e = (int)ex - (int)ey + 1023;
	if (mx < my) {
		mx <<= 1;
		e--;
	}
	if ((unsigned)(e - 1) >= 0x7fdU)	/* keep the rounding carry below 0x7ff */
		return s31lg_divdf3(x, y);
	if (!(y & SIGM))			/* y = +-2^k: exact */
		return sign | ((u64)e << 52) | (x & SIGM);

	{
		u32 yb = 0x3f800000u | ((u32)(my >> 29) & 0x7fffffu);
		u32 two31 = 0x4f000000u;	/* 2^31f */

		__asm__ volatile(
			"frflags %0\n\t"
			"fmv.w.x ft0, %2\n\t"
			"fmv.w.x ft1, %3\n\t"
			"fdiv.s  ft0, ft1, ft0, rne\n\t"
			"fcvt.wu.s %1, ft0, rtz\n\t"
			"fsflags %0"
			: "=&r"(fl), "=&r"(R0) : "r"(yb), "r"(two31) : "ft0", "ft1");
	}
	q0 = (u32)(((u64)(u32)(mx >> 22) * R0) >> 31);
	E = (s64)((mx << 30) - lo64(q0, my));
	T = ((u64)q0 << 23) + (u64)(((s64)(int32_t)(E >> 31) * (s64)R0) >> 29);
	E = (s64)((mx << 53) - lo64(T, my));
	T += (u64)(((s64)(int32_t)(E >> 32) * (s64)R0) >> 51);
	rem = (mx << 53) - lo64(T, my);	/* signed, small */
	while ((s64)rem < 0) {
		T--;
		rem += my;
	}
	while (rem >= my) {
		T++;
		rem -= my;
	}
	{
		u64 round = T & 1, mant = T >> 1;
		u64 inc = round & ((rem != 0) | (mant & 1));

		res = (sign | ((u64)(e - 1) << 52)) + mant + inc;
#if S31V2_FFLAGS
		if ((round | rem) && !(fcsr & 1))
			__asm__ volatile("csrsi fflags, 1");
#endif
	}
	return res;
}
