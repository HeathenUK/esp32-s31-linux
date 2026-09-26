/*
 * s31_float.h - double to float without libgcc. s31, MIT.
 *
 * The S31 hart has F but not D: every double operation, including the
 * narrowing conversion, is a libgcc soft-float call. The GL double entry
 * points (glVertex3d, glRotated, GL_DOUBLE arrays, ...) only need the
 * conversion, and done on the bits it is a dozen integer instructions.
 * Bit-exact with (float)d under round-to-nearest-even for every input;
 * zeros, denormal results, overflow, inf and NaN fall back to the compiler.
 */
#ifndef S31_FLOAT_H
#define S31_FLOAT_H

#include <string.h>

static inline float s31_d2f_bits(double d)
{
  unsigned long long b;
  unsigned int hi, lo, e, m, rest, r;
  float f;

  memcpy(&b, &d, 8);
  hi = (unsigned int)(b >> 32);
  lo = (unsigned int)b;
  e = (hi >> 20) & 0x7ff;
  /* float normal exponents 1..254 are double exponents 897..1150 */
  if (e - 897u <= 1150u - 897u) {
    m = ((hi & 0xfffff) << 3) | (lo >> 29);
    rest = lo & 0x1fffffff;
    r = (hi & 0x80000000u) | ((e - 896u) << 23) | m;
    /* round to nearest, ties to even; a carry into the exponent is right,
       including the step to infinity */
    if (rest > 0x10000000u || (rest == 0x10000000u && (m & 1))) r++;
    memcpy(&f, &r, 4);
    return f;
  }
  if (((hi & 0x7fffffffu) | lo) == 0) {       /* +0 / -0 */
    r = hi & 0x80000000u;
    memcpy(&f, &r, 4);
    return f;
  }
  return (float)d;
}

/* s31 (phase 3a G01): the widening conversions the glGet*dv queries
   make, on the bits as well: exact for every input (float and int are
   subsets of double); zero, denormals, inf and NaN fall back */
static inline double s31_f2d_bits(float f)
{
  unsigned int b, e;
  unsigned long long r;
  double d;

  memcpy(&b, &f, 4);
  e = (b >> 23) & 0xff;
  if (e - 1u < 254u) {
    r = ((unsigned long long)((b & 0x80000000u) | ((e + 896u) << 20) |
                              ((b & 0x7fffffu) >> 3)) << 32) |
        ((unsigned long long)(b & 7u) << 29);
    memcpy(&d, &r, 8);
    return d;
  }
  return (double)f;
}

static inline double s31_i2d_bits(int i)
{
  unsigned int u, s, lz;
  unsigned long long m, r;
  double d;

  if (i == 0) return 0.0;
  s = i < 0 ? 0x80000000u : 0;
  u = i < 0 ? 0u - (unsigned int)i : (unsigned int)i;
  lz = (unsigned int)__builtin_clz(u);
  m = (unsigned long long)u << (lz + 21);      /* leading 1 at bit 52 */
  r = ((unsigned long long)(s | ((1054u - lz) << 20)) << 32) |
      (m & 0xfffffffffffffull);
  memcpy(&d, &r, 8);
  return d;
}

#if defined(__riscv) && (!defined(__riscv_flen) || __riscv_flen < 64)
#define s31_d2f(d) s31_d2f_bits(d)
#define s31_f2d(f) s31_f2d_bits(f)
#define s31_i2d(i) s31_i2d_bits(i)
#else
#define s31_d2f(d) ((float)(d))
#define s31_f2d(f) ((double)(f))
#define s31_i2d(i) ((double)(i))
#endif

#endif /* S31_FLOAT_H */
