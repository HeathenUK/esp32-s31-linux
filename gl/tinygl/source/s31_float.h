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

#if defined(__riscv) && (!defined(__riscv_flen) || __riscv_flen < 64)
#define s31_d2f(d) s31_d2f_bits(d)
#else
#define s31_d2f(d) ((float)(d))
#endif

#endif /* S31_FLOAT_H */
