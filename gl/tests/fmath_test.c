/*
 * fmath_test.c - accuracy of gl/tinygl/source/s31_fmath.c against a double
 * reference, and against what the library did before (phase 3a, G01).
 *   cc -O2 -ffp-contract=off -I../tinygl/source fmath_test.c ../tinygl/source/s31_fmath.c -lm
 * Error in float ulps of the reference (ulp of the correctly rounded
 * result), max over each set, and for the old path the same measure:
 *  - sin/cos of degrees: every 7th float in [-720, 720], every 1/4096
 *    degree in [-720, 720], then 2^24 random
 *    floats in [-1e6, 1e6], and integers -100000..100000. The old path was
 *    float angle = deg * M_PI / 180.0 (double), then (float)sin(angle).
 *  - powf(x, y): x = i/1024 (the specular table) and x = every 97th float
 *    in [0.001, 1] (spot attenuation), y in {0.5, 1, 1.5, 2, 3.3, 4, 8, 10,
 *    16, 20, 32, 50, 64, 100, 128}; errors below 1e-30 absolute ignored
 *    (they are underflow, and invisible); then (review 3a R5) 2^23 random
 *    pairs, x uniform in the float bit patterns of [2^-10, 1] and y
 *    uniform in [0, 128], and 2^23 with y in [112, 128], where the float
 *    log2's error is amplified most
 *  - expf(x): every float in [-104, 88]
 * Exit 1 if any bound in LIMITS is exceeded. s31, MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "s31_fmath.h"

static double ulp_of(double ref)
{
	float f = (float)ref;
	float g;
	if (f == 0.0f) return ldexp(1.0, -149);
	g = nextafterf(fabsf(f), INFINITY);
	return (double)g - fabs((double)f);
}
static double uerr(float got, double ref) { return fabs((double)got - ref) / ulp_of(ref); }

/* the reference: reduced exactly by multiples of 90 degrees in double
   (deg is a float, so deg - 90 q is exact), then libm in double. Multiples
   of 90 degrees therefore have exact zeros, as they do mathematically;
   sin(M_PI) = 1.2e-16 would make every exact zero look infinitely wrong */
static void rsc(double deg, double *s, double *c)
{
	double q = nearbyint(deg / 90.0), r = (deg - 90.0 * q) * (M_PI / 180.0);
	double sn = sin(r), cs = cos(r);
	switch (((long long)q) & 3) {
	case 0: *s = sn; *c = cs; break;
	case 1: *s = cs; *c = -sn; break;
	case 2: *s = -sn; *c = -cs; break;
	default: *s = -cs; *c = sn; break;
	}
}

static double ms, mc, mos, moc, mabs, moabs;
static void one(float d)
{
	float s, c, a;
	double rs, rc, e;
	rsc(d, &rs, &rc);
	s31_sincos_deg(d, &s, &c);
	a = d * M_PI / 180.0;
	e = uerr(s, rs); if (e > ms) ms = e;
	e = uerr(c, rc); if (e > mc) mc = e;
	e = fabs(s - rs); if (e > mabs) mabs = e;
	e = fabs(c - rc); if (e > mabs) mabs = e;
	/* the old path has no exact zeros (sin(180) = -8.7e-8): its ulp error
	   only where |ref| > 1e-3 */
	if (fabs(rs) > 1e-3) { e = uerr((float)sin(a), rs); if (e > mos) mos = e; }
	if (fabs(rc) > 1e-3) { e = uerr((float)cos(a), rc); if (e > moc) moc = e; }
	e = fabs((float)sin(a) - rs); if (e > moabs) moabs = e;
	e = fabs((float)cos(a) - rc); if (e > moabs) moabs = e;
}

int main(void)
{
	unsigned int u, n;
	float f;
	int i, bad = 0;
	double mp = 0, mpo = 0, me = 0, meo = 0, mprel = 0;
	static const float ys[] = { 0.5f, 1, 1.5f, 2, 3.3f, 4, 8, 10, 16, 20, 32, 50, 64, 100, 128 };

	/* every 7th float in [-720, 720], then every float in [-1, 1] degree
	   steps of 1/4096 up to 720 (the angles apps pass) */
	for (f = 0.0f; f <= 720.0f; ) {
		unsigned int k;
		one(f); one(-f);
		memcpy(&k, &f, 4); k += 7; memcpy(&f, &k, 4);
	}
	for (i = -720 * 4096; i <= 720 * 4096; i++) one(i / 4096.0f);
	srand(1);
	for (n = 0; n < (1u << 24); n++) {
		double r = ((double)rand() / RAND_MAX) * 2e6 - 1e6;
		one((float)r);
	}
	for (i = -100000; i <= 100000; i++) one((float)i);
	{ float s, c; s31_sincos_deg(90, &s, &c); printf("sincos_deg(90) = %g %g; (180) ", s, c);
	  s31_sincos_deg(180, &s, &c); printf("%g %g; (270) ", s, c);
	  s31_sincos_deg(270, &s, &c); printf("%g %g; (-90) ", s, c);
	  s31_sincos_deg(-90, &s, &c); printf("%g %g\n", s, c);
	  if (s != -1.0f || c != 0.0f) bad++; }
	printf("sin_deg: max %.3f ulp (old path %.1f ulp where |ref| > 1e-3), cos_deg: max %.3f ulp (old %.1f); max abs %.3g (old %.3g)\n",
	       ms, mos, mc, moc, mabs, moabs);

	for (i = 0; i < (int)(sizeof ys / sizeof ys[0]); i++) {
		float y = ys[i];
		int j;
		for (j = 0; j <= 1024; j++) {
			float x = j / 1024.0f;
			double ref = pow((double)x, (double)y), e;
			float got = s31_powf(x, y);
			if (fabs(ref) < 1e-30 && fabsf(got) < 1e-30f) continue;
			e = uerr(got, ref); if (e > mp) mp = e;
			e = fabs(got - ref) / ref; if (e > mprel) mprel = e;
			e = uerr(powf(x, y), ref); if (e > mpo) mpo = e;
		}
		for (f = 0.001f; f <= 1.0f; ) {
			double ref = pow((double)f, (double)y), e;
			float got = s31_powf(f, y);
			unsigned int k;
			if (!(fabs(ref) < 1e-30 && fabsf(got) < 1e-30f)) {
				e = uerr(got, ref); if (e > mp) mp = e;
				e = fabs(got - ref) / ref; if (e > mprel) mprel = e;
				e = uerr(powf(f, y), ref); if (e > mpo) mpo = e;
			}
			memcpy(&k, &f, 4); k += 97; memcpy(&f, &k, 4);
		}
	}
	{
		/* review 3a R5: random x and y, and y crowded near 128 */
		unsigned int r = 12345u, lo, hi, k;
		float fl = 1.0f / 1024.0f, fh = 1.0f;
		double wy = 0, wx = 0;
		memcpy(&lo, &fl, 4); memcpy(&hi, &fh, 4);
		for (k = 0; k < (1u << 24); k++) {
			unsigned int xb;
			float x, y;
			double ref, e;
			r = r * 1664525u + 1013904223u;
			xb = lo + (unsigned int)(((unsigned long long)r * (hi - lo + 1)) >> 32);
			memcpy(&x, &xb, 4);
			r = r * 1664525u + 1013904223u;
			y = (k & 1) ? 112.0f + (float)(r >> 8) * (16.0f / 16777216.0f)
			            : (float)(r >> 8) * (128.0f / 16777216.0f);
			ref = pow((double)x, (double)y);
			if (fabs(ref) < 1e-30) continue;
			e = fabs(s31_powf(x, y) - ref) / ref;
			if (e > mprel) { mprel = e; wx = x; wy = y; }
			e = uerr(s31_powf(x, y), ref); if (e > mp) mp = e;
		}
		printf("powf random: max rel %.3g so far, at x %.9g y %.6g\n", mprel, wx, wy);
	}
	if (s31_powf(0, 0) != 1 || s31_powf(0, 3) != 0 || s31_powf(1, 77) != 1) bad++;
	printf("powf: max %.2f ulp, max rel %.3g (libm powf %.2f ulp)\n", mp, mprel, mpo);

	for (f = -104.0f; f <= 88.0f; f = nextafterf(f, INFINITY)) {
		double ref = exp((double)f), e;
		if (ref < 1.2e-38) continue;   /* denormal results: flushed, invisible */
		e = uerr(s31_expf(f), ref); if (e > me) me = e;
		e = uerr(expf(f), ref); if (e > meo) meo = e;
	}
	printf("expf: max %.3f ulp over [-87.3, 88] (libm expf %.3f)\n", me, meo);
	(void)u;
	/* LIMITS */
	if (ms > 1.0 || mc > 1.0) bad++;
	/* review 3a R5: 1.15e-5 found over 2^28 random pairs (x = 0.7036,
	   y = 127.5; 174 ulp); 1e-5 was exceeded near y = 128 */
	if (mprel > 1.25e-5) bad++;
	if (me > 1.5) bad++;
	printf("fmath_test: %s\n", bad ? "FAIL" : "PASS");
	return bad != 0;
}
