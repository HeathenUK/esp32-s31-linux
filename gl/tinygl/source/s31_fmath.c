/*
 * s31_fmath.c - float-only sin/cos (degrees), pow and exp. s31, MIT.
 * See s31_fmath.h. The polynomial coefficients are near-minimax fits
 * (mpmath chebyfit, rounded to float) on the reduced ranges below.
 */
#include <math.h>
#include <string.h>
#include "s31_fmath.h"

static inline float fbits(unsigned int u) { float f; memcpy(&f, &u, 4); return f; }
static inline unsigned int ubits(float f) { unsigned int u; memcpy(&u, &f, 4); return u; }

/* sin x = x + x z g(z), cos x = 1 - z/2 + z^2 h(z), z = x^2, |x| <= pi/4 + 0.02 */
#define S1 -0.166666641831398f
#define S2 0.008332686498761177f
#define S3 -0.0001957490894710645f
#define C1 0.0416666641831398f
#define C2 -0.0013888240791857243f
#define C3 2.4534931071684696e-05f
/* pi/180 = D_HI + D_LO */
#define D_HI 0.017453292f
#define D_LO 1.351996e-10f

void s31_sincos_deg(float deg, float *s, float *c)
{
  float r, x, xl, t, z, zl, hz, w, e, sn, cs;
  int q;

  if (!(fabsf(deg) < 8388608.0f)) {
    if (!(fabsf(deg) <= 3.4028235e38f)) {   /* inf or NaN */
      *s = *c = deg - deg;
      return;
    }
    deg = fmodf(deg, 360.0f);               /* exact; rare */
  }
  /* nearest multiple of 90. q * 90 is an integer below 2^24, and
     deg - q * 90 is exact: both are multiples of ulp(deg) <= 1/2 */
  q = (int)(deg * (1.0f / 90.0f) + (deg < 0.0f ? -0.5f : 0.5f));
  r = deg - (float)q * 90.0f;
  /* x + xl = r pi/180 to about 2^-40 (the rounding of x alone costs up
     to 1/2 ulp of the result). sin(x + xl) = x + (xl + x z g(z)) and
     cos = 1 - z/2 + z^2 h(z), with 1 - z/2 compensated (its rounding
     error e is exact) and z's own rounding and xl folded into zl, so each
     result is rounded about once */
  t = r * D_LO;
  x = fmaf(r, D_HI, t);
  xl = fmaf(r, D_HI, -x) + t;
  z = x * x;
  zl = fmaf(x, x, -z) + 2.0f * x * xl;
  sn = x + fmaf(x * z, fmaf(fmaf(S3, z, S2), z, S1), xl);
  hz = 0.5f * z;
  w = 1.0f - hz;
  e = (1.0f - w) - hz;
  cs = w + fmaf(z * z, fmaf(fmaf(C3, z, C2), z, C1), fmaf(-0.5f, zl, e));
  switch (q & 3) {
  case 0: *s = sn;  *c = cs;  break;
  case 1: *s = cs;  *c = -sn; break;
  case 2: *s = -sn; *c = -cs; break;
  default: *s = -cs; *c = sn; break;
  }
}

/* e^r = 1 + r + r^2 q(r), |r| <= ln2/2 + 0.001 */
#define E1 0.5f
#define E2 0.16666576266288757f
#define E3 0.04166655242443085f
#define E4 0.008363345637917519f
#define E5 0.0013926392421126366f
static inline float k_exp(float r)
{
  float q = fmaf(fmaf(fmaf(fmaf(E5, r, E4), r, E3), r, E2), r, E1);
  return fmaf(r * r, q, r) + 1.0f;
}
/* p * 2^k for k in [-151, 128] */
static inline float scale2(float p, int k)
{
  if (k > 127) return p * 1.7014118e38f * 2.0f;   /* inf */
  if (k < -126) {
    if (k < -151) return 0.0f;
    p *= 5.9604645e-08f;                            /* 2^-24 */
    k += 24;
  }
  return p * fbits((unsigned int)(k + 127) << 23);
}

#define LOG2E 1.44269502f
#define LN2_HI 0.693145752f       /* 11 bits: k * LN2_HI is exact */
#define LN2_LO 1.42860677e-06f

float s31_expf(float x)
{
  float kf, r;
  int k;
  if (!(x < 88.8f)) return x != x ? x : fbits(0x7f800000u);   /* NaN, overflow */
  if (x < -104.0f) return 0.0f;
  kf = x * LOG2E;
  k = (int)(kf + (kf < 0.0f ? -0.5f : 0.5f));
  kf = (float)k;
  r = fmaf(-kf, LN2_HI, x);
  r = fmaf(-kf, LN2_LO, r);
  return scale2(k_exp(r), k);
}

/* log m = 2t + t^3 P(t^2), t = (m - 1)/(m + 1), m in [sqrt(1/2), sqrt(2)] */
#define L1 0.6666668653488159f
#define L2 0.39988550543785095f
#define L3 0.29590317606925964f

float s31_powf(float x, float y)
{
  unsigned int u;
  int e, k;
  float m, t, w, l2, v, f;

  if (y == 0.0f) return 1.0f;
  if (!(x > 0.0f)) return 0.0f;
  if (x == 1.0f) return 1.0f;
  u = ubits(x);
  if (u >= 0x7f800000u) return x;                 /* inf, NaN */
  e = (int)(u >> 23) - 127;
  if (e == -127) {                                /* denormal */
    u = ubits(x * 8388608.0f);
    e = (int)(u >> 23) - 127 - 23;
  }
  m = fbits((u & 0x7fffffu) | 0x3f800000u);       /* [1, 2) */
  if (m > 1.41421356f) { m *= 0.5f; e++; }
  t = (m - 1.0f) / (m + 1.0f);
  w = t * t;
  l2 = fmaf(fmaf(t * w, fmaf(fmaf(L3, w, L2), w, L1), 2.0f * t), LOG2E, (float)e);
  v = y * l2;                                     /* log2 of the result */
  if (!(v < 128.0f)) return v != v ? v : fbits(0x7f800000u);
  if (v < -151.0f) return 0.0f;
  k = (int)(v + (v < 0.0f ? -0.5f : 0.5f));
  f = v - (float)k;                               /* exact, |f| <= 1/2 */
  return scale2(k_exp(f * 0.693147182f), k);
}
