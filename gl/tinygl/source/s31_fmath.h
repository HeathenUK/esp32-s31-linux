/*
 * s31_fmath.h - float-only elementary functions for the GL core. s31, MIT.
 *
 * The S31 hart has F but not D, so every double operation is a libgcc
 * soft-float call, and musl's float functions do not avoid that: sinf,
 * cosf, powf and expf all compute in double internally (assessment 3.2:
 * __sindf has 8 __muldf3, powf 14 __muldf3 and 10 __adddf3; expf uses
 * double_t throughout). These are single-precision throughout, with
 * explicit fmaf (one fmadd.s here) so the result is the same on every
 * target with a fused multiply-add. Accuracy against a double reference
 * is measured by gl/tests/fmath_test.c (the numbers are in its header and
 * in artifacts/gl/phase3a/LEVERS.md).
 */
#ifndef S31_FMATH_H
#define S31_FMATH_H

/* sin and cos of an angle in DEGREES (glRotate, GL_SPOT_CUTOFF): reduced
   exactly by multiples of 90 degrees, so multiples of 90 give exact 0 and
   +-1 and the error does not grow with the angle */
void s31_sincos_deg(float deg, float *s, float *c);
/* x^y for x >= 0 (lighting: specular table, spot exponent); 0 for x <= 0
   unless y == 0. Relative error up to about 1.2e-5 for y <= 128 (the float
   log2's error times y: 1.15e-5 = 174 ulp at x = 0.70, y = 127.5, the worst
   of 2^28 random pairs; review 3a R5) - far below an 8-bit colour step */
float s31_powf(float x, float y);
/* e^x (fog factors) */
float s31_expf(float x);

#endif
