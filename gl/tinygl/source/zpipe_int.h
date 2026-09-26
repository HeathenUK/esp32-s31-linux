/*
 * zpipe_int.h - the fragment arithmetic of the general path (zpipe.c),
 * shared with the fused fillers (zpipe_fused.c, phase 5 O1): one
 * definition, so a fused filler computes every pixel exactly as the
 * stages it replaces. s31, MIT.
 *
 * Phase 5 P (precision, gl/tests/glx_prec.c against Mesa llvmpipe on the
 * 565 visual, 48 bands of 256 values each; the model is in
 * artifacts/gl/phase5/prec/MODEL.txt): Mesa computes the texture
 * environment and the combiner in float and rounds to 8 bits once, blends
 * in 8-bit fixed point with each product rounded, and truncates the 8-bit
 * result to 565. So here:
 *   MUL8(x, y)        round(x y / 255): texenv products, blend products
 *   MULS8(x, y, sh)   clamp(round(x y 2^sh / 255)): a combiner MODULATE with
 *                     its scale (rounded once, after the scale)
 *   MIX8(a, wa, b, wb) round((a wa + b wb) / 255), wa + wb = 255: DECAL,
 *                     texenv BLEND, fog, INTERPOLATE (rounded once)
 *   MIXS8(...)        the same times 2^sh, clamped (INTERPOLATE with a scale)
 *   ADDS8(x, y, sh)   clamp(round((x + y - 127.5) 2^sh)): ADD_SIGNED
 * TinyGL's MUL8 was (x (y + 1)) >> 8, a floor; -DS31GL_P4ARITH (a compat
 * build for the phase-4 hash checks) restores it and the old sums.
 */
#ifndef ZPIPE_INT_H
#define ZPIPE_INT_H

static inline int clamp255(int v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

#ifdef S31GL_P4ARITH

#define MUL8(x, y) (((x) * ((y) + 1)) >> 8)
#define MULS8(x, y, sh) clamp255(MUL8(x, y) << (sh))
#define MIX8(a, wa, b, wb) (MUL8(a, wa) + MUL8(b, wb))
#define MIXS8(a, wa, b, wb, sh) clamp255((MUL8(a, wa) + MUL8(b, wb)) << (sh))
#define ADDS8(x, y, sh) clamp255(((x) + (y) - 128) * (1 << (sh)))

#else

/* round(n / 255) for 0 <= n <= 65662 (exhaustively checked; every use is a
   product or a sum of products of 8-bit values that adds to at most 65025,
   or a scaled one clamped at 255 first) */
static inline int div255r(unsigned int n)
{
  n += 128;
  return (int)((n + (n >> 8)) >> 8);
}
#define MUL8(x, y) div255r((unsigned int)((x) * (y)))
/* n = x y 2^sh can reach 4 x 65025: past 65662 the result is above 255
   anyway, and div255r is monotonic, so clamping its value is exact */
static inline int muls8(unsigned int n)
{
  int v = div255r(n < 65663u ? n : 65663u);
  return v > 255 ? 255 : v;
}
#define MULS8(x, y, sh) muls8((unsigned int)((x) * (y)) << (sh))
#define MIX8(a, wa, b, wb) div255r((unsigned int)((a) * (wa) + (b) * (wb)))
#define MIXS8(a, wa, b, wb, sh) muls8((unsigned int)((a) * (wa) + (b) * (wb)) << (sh))
/* (x + y - 127.5) 2^sh rounded half up: ((2 (x + y) - 255) 2^sh + 1) >> 1 */
#define ADDS8(x, y, sh) clamp255((((2 * ((x) + (y)) - 255) * (1 << (sh))) + 1) >> 1)

#endif

/* RGB565 -> 8 bits, replicating the high bits (31 -> 255, 63 -> 255) */
#define UNPACK(t, R, G, B) do { unsigned int t_ = (t); \
    (R) = ((t_ >> 8) & 0xf8) | (t_ >> 13); \
    (G) = ((t_ >> 3) & 0xfc) | ((t_ >> 9) & 3); \
    (B) = ((t_ << 3) & 0xf8) | ((t_ >> 2) & 7); } while (0)
/* 8 bits -> RGB565, truncating as RGB_TO_PIXEL does (and as Mesa does on
   a 565 buffer: band 4 of glx_prec) */
#define PACK(R, G, B) ((PIXEL)((((R) & 0xf8) << 8) | (((G) & 0xfc) << 3) | ((B) >> 3)))

#endif
