/*
 * s31_tex8.c - phase 5: how texture levels are stored. s31, MIT.
 *
 * Phase 4 stored every texel as RGB565, plus an A8 plane for the alpha
 * classes: 2 or 3 bytes a texel, 5 or 6 bits a channel. On the board the
 * textures are the largest thing libGL holds (QuakeSpasm 0.96.3: 4.9 MB
 * after its load phase, of 15.4 MB of RAM), and the 5-bit channels were
 * the darkest part of its lightmaps (artifacts/gl/glquake/dark/DARKNESS.md
 * defect A). A level is now stored as one of (GLTexture.st, the same for
 * every stored level of a texture):
 *
 *   TGL_ST_L8   grey texels (r = g = b): one byte of grey a texel, and its
 *               alpha by GLTexture.amode - none (255), a bit a texel (0 or
 *               255: a lightmap block's unused area), an A8 plane, the grey
 *               itself (INTENSITY), or the grey byte IS the alpha (ALPHA).
 *               LUMINANCE, LUMINANCE_ALPHA, INTENSITY, ALPHA, and any RGB
 *               or RGBA image whose texels are all grey. Exact, 1 to 2 B.
 *   TGL_ST_P8   at most 256 distinct texels: a byte index into the level's
 *               RGBA8 palette (GLTexPal, in the level's block). Quake's
 *               textures are 8-bit palette images expanded to RGBA. Exact,
 *               1 B a texel + 4-6 B an entry.
 *   TGL_ST_565  anything else: RGB565 rounded (phase 5 P), + A8 unless the
 *               texture is RGBA-class and every alpha is 255 (GLTexture.a1:
 *               no plane, alpha reads 255).
 *   TGL_ST_W32  S31GL_TEX8=2: the reference - P8 and L8 levels stored as
 *               RGBA8 words, every decision (the kinds, the palette's
 *               overflow) made as for P8 / L8 (GLTexture.stref). The fast
 *               paths never take it, so rendering a trace both ways gates
 *               the P8 / L8 fetches and fused fillers against the general
 *               path on unpacked texels (gl/tests/run-qsr-ab.sh).
 *
 * S31GL_TEX8=0 stores everything as phase 4 did (and -DS31GL_P4ARITH makes
 * that the default): the compat build for the phase-4 hash checks.
 *
 * What the sampling side reads (zpipe.h ZLevel): a P8 or L8 texel is an
 * RGBA8 word (r | g << 8 | b << 16 | a << 24), the texel GL defines for
 * the base format - RGB alpha 255, LUMINANCE (L, L, L, 255), ALPHA (255,
 * 255, 255, A) as the 565 path's white plane, INTENSITY (I, I, I, I).
 *
 * Kinds follow the texture: the first level stored picks one; a later
 * level or a glTexSubImage that the kind cannot hold (a colour texel in
 * an L8 texture, a 257th colour in a P8 level after its unused entries
 * are dropped) converts every stored level to 565 first - exactly, from
 * the 8-bit texels. An L8 texture's alpha mode only grows (none, bits,
 * A8), every level with it.
 *
 * KNOWN DEVIATION (phase 5 review S3, measured, not fixed): GL fixes a
 * level's internal resolution at glTexImage time; here a glTexSubImage2D
 * or glCopyTexSubImage2D that an L8/P8 texture cannot hold converts it to
 * RGB565, which requantises texels the call did not touch (by at most one
 * 565 step: a 16x16 grey ramp plus one colour texel changed 74 of 255
 * untouched texels, worst 9; a 257th colour into a 256-colour P8 image
 * 217 of 511, worst 9; Mesa 0 - artifacts/gl/phase5/review-spec
 * glx_review.c conv_one / conv_p8), and GL_TEXTURE_RED_SIZE then reads 5
 * instead of 8 (it reports what is stored). Within logcmp's +-16, so no
 * gate sees it; on the board it brings DARKNESS defect A back for any
 * lightmap block that once receives a coloured texel, and doubles that
 * texture's bytes. The exact alternative is to convert to the W32
 * reference layout (S31GL_TEX8=2's TGL_ST_W32) instead of 565: 4 B a
 * texel, so an owner decision against the memory bar.
 *
 * No double (F without D). Upload is not a per-pixel path, but QuakeSpasm
 * updates lightmaps every frame: grey data into an L8 level is a store,
 * no palette search.
 */
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "zgl.h"
#include "s31_pixels.h"
#include "s31_tex8.h"
#include "zpipe_int.h"

int gl_tex8_knob(void)
{
#ifdef S31GL_P4ARITH
  const int def = 0;
#else
  const int def = 1;
#endif
  const char *e = getenv("S31GL_TEX8");
  int v = e ? atoi(e) : def;
  return v < 0 || v > 2 ? def : v;
}

/* (texture.c's, moved here: plan F3) base format class of an internal format */
int gl_tex_class(int f, int *lum)
{
  *lum = 0;
  switch (f) {
  case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: case GL_ALPHA12: case GL_ALPHA16:
    return TGL_TEXF_ALPHA;
  case 1: case GL_LUMINANCE: case GL_LUMINANCE4: case GL_LUMINANCE8:
  case GL_LUMINANCE12: case GL_LUMINANCE16:
    *lum = 1;
    return TGL_TEXF_RGB;
  case 2: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE6_ALPHA2:
  case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE12_ALPHA4:
  case GL_LUMINANCE12_ALPHA12: case GL_LUMINANCE16_ALPHA16:
    *lum = 1;
    return TGL_TEXF_RGBA;
  case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8:
  case GL_INTENSITY12: case GL_INTENSITY16:
    *lum = 1;
    return TGL_TEXF_INTENSITY;
  case 3: case GL_R3_G3_B2: case GL_RGB: case GL_RGB4: case GL_RGB5: case GL_RGB8:
  case GL_RGB10: case GL_RGB12: case GL_RGB16:
    return TGL_TEXF_RGB;
  default:
    return TGL_TEXF_RGBA;
  }
}

/* PACK(l, l, l) of every grey value: tier 1's table for an L8 texture
   (raster_sel.c tier1_t8; in flash, not RAM) */
const unsigned short s31_grey565[256] = {
  0x0000, 0x0000, 0x0000, 0x0000, 0x0020, 0x0020, 0x0020, 0x0020,
  0x0841, 0x0841, 0x0841, 0x0841, 0x0861, 0x0861, 0x0861, 0x0861,
  0x1082, 0x1082, 0x1082, 0x1082, 0x10a2, 0x10a2, 0x10a2, 0x10a2,
  0x18c3, 0x18c3, 0x18c3, 0x18c3, 0x18e3, 0x18e3, 0x18e3, 0x18e3,
  0x2104, 0x2104, 0x2104, 0x2104, 0x2124, 0x2124, 0x2124, 0x2124,
  0x2945, 0x2945, 0x2945, 0x2945, 0x2965, 0x2965, 0x2965, 0x2965,
  0x3186, 0x3186, 0x3186, 0x3186, 0x31a6, 0x31a6, 0x31a6, 0x31a6,
  0x39c7, 0x39c7, 0x39c7, 0x39c7, 0x39e7, 0x39e7, 0x39e7, 0x39e7,
  0x4208, 0x4208, 0x4208, 0x4208, 0x4228, 0x4228, 0x4228, 0x4228,
  0x4a49, 0x4a49, 0x4a49, 0x4a49, 0x4a69, 0x4a69, 0x4a69, 0x4a69,
  0x528a, 0x528a, 0x528a, 0x528a, 0x52aa, 0x52aa, 0x52aa, 0x52aa,
  0x5acb, 0x5acb, 0x5acb, 0x5acb, 0x5aeb, 0x5aeb, 0x5aeb, 0x5aeb,
  0x630c, 0x630c, 0x630c, 0x630c, 0x632c, 0x632c, 0x632c, 0x632c,
  0x6b4d, 0x6b4d, 0x6b4d, 0x6b4d, 0x6b6d, 0x6b6d, 0x6b6d, 0x6b6d,
  0x738e, 0x738e, 0x738e, 0x738e, 0x73ae, 0x73ae, 0x73ae, 0x73ae,
  0x7bcf, 0x7bcf, 0x7bcf, 0x7bcf, 0x7bef, 0x7bef, 0x7bef, 0x7bef,
  0x8410, 0x8410, 0x8410, 0x8410, 0x8430, 0x8430, 0x8430, 0x8430,
  0x8c51, 0x8c51, 0x8c51, 0x8c51, 0x8c71, 0x8c71, 0x8c71, 0x8c71,
  0x9492, 0x9492, 0x9492, 0x9492, 0x94b2, 0x94b2, 0x94b2, 0x94b2,
  0x9cd3, 0x9cd3, 0x9cd3, 0x9cd3, 0x9cf3, 0x9cf3, 0x9cf3, 0x9cf3,
  0xa514, 0xa514, 0xa514, 0xa514, 0xa534, 0xa534, 0xa534, 0xa534,
  0xad55, 0xad55, 0xad55, 0xad55, 0xad75, 0xad75, 0xad75, 0xad75,
  0xb596, 0xb596, 0xb596, 0xb596, 0xb5b6, 0xb5b6, 0xb5b6, 0xb5b6,
  0xbdd7, 0xbdd7, 0xbdd7, 0xbdd7, 0xbdf7, 0xbdf7, 0xbdf7, 0xbdf7,
  0xc618, 0xc618, 0xc618, 0xc618, 0xc638, 0xc638, 0xc638, 0xc638,
  0xce59, 0xce59, 0xce59, 0xce59, 0xce79, 0xce79, 0xce79, 0xce79,
  0xd69a, 0xd69a, 0xd69a, 0xd69a, 0xd6ba, 0xd6ba, 0xd6ba, 0xd6ba,
  0xdedb, 0xdedb, 0xdedb, 0xdedb, 0xdefb, 0xdefb, 0xdefb, 0xdefb,
  0xe71c, 0xe71c, 0xe71c, 0xe71c, 0xe73c, 0xe73c, 0xe73c, 0xe73c,
  0xef5d, 0xef5d, 0xef5d, 0xef5d, 0xef7d, 0xef7d, 0xef7d, 0xef7d,
  0xf79e, 0xf79e, 0xf79e, 0xf79e, 0xf7be, 0xf7be, 0xf7be, 0xf7be,
  0xffdf, 0xffdf, 0xffdf, 0xffdf, 0xffff, 0xffff, 0xffff, 0xffff
};

/* ------------------------------------------------------------ RGB565 */

/* Store rows [y0, y0+h) x [x0, x0+w) of a 565 level from an unpacked
   source whose pixel (sx, sy) lands at (x0, y0). Conversion per class:
   RGB565 rounded (phase 5 P; truncated as TinyGL did under S31GL_P4ARITH);
   luminance and intensity take R (GL 1.3 table 3.15); alpha classes also
   fill the A8 plane, when there is one (al NULL: an a1 texture).
   One loop per class, the class chosen once per row (review P7: the class
   and luminance tests ran per texel, and this file is -Os, so GCC did not
   unswitch them; TyrQuake re-uploads lightmaps every frame). Built -O2. */
#ifdef S31GL_P4ARITH
#define T565(r, g, b) ((unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#else
/* phase 5 P: rounded to the nearest 5 / 6-bit level, as Mesa converts 8-bit
   values into a 565 format (_mesa_unorm_to_unorm: (x 31 + 127) / 255,
   (x 63 + 127) / 255, here as one multiply each - exact for all 256
   values). A value expanded from 565 (glCopyTexImage from the colour
   buffer: s31_c5to8) comes back unchanged */
#define T5R(x) (((unsigned int)(x) * 249u + 1014u) >> 11)
#define T6R(x) (((unsigned int)(x) * 253u + 505u) >> 10)
#define T565(r, g, b) ((unsigned short)((T5R(r) << 11) | (T6R(g) << 5) | T5R(b)))
/* (measured on the QuakeSpasm trace against Mesa, 80 frames: rounded, the
   view is 1.011 of Mesa's luminance with 71.3% of pixels exact; truncated,
   0.918 and 53.3% - artifacts/gl/phase5/p5c/qs-exp-f8{,t}) */
#endif
__attribute__((optimize("O2")))
static void st565_rows(unsigned short *pix, unsigned char *al, int ws, int ifmt,
                       const S31Unpack *u, int sx, int sy, int x0, int y0, int w, int h)
{
  unsigned char row[4 * TGL_TEX_MAX];
  int TW = 1 << ws, x, y, lum;
  int cls = gl_tex_class(ifmt, &lum);

  /* the common uploads straight from the client's bytes, without the
     RGBA8888 row in between: UNSIGNED_BYTE RGB / RGBA / LUMINANCE into
     the matching stored class, no pixel transfer, not the colour buffer */
  int direct = u->fb == NULL && u->xs == NULL && u->type == GL_UNSIGNED_BYTE &&
    ((u->format == GL_RGB && cls == TGL_TEXF_RGB && !lum) ||
     (u->format == GL_RGBA && (cls == TGL_TEXF_RGBA || cls == TGL_TEXF_RGB) && !lum) ||
     (u->format == GL_LUMINANCE && cls == TGL_TEXF_RGB && lum));

  /* (fix 2, bench P2) the colour buffer into an RGB or RGBA texture, no
     pixel transfer: its 565 values are the texels (the expansion
     s31_c5to8 / c6to8 and T565 are inverse, in both arithmetics; alpha
     255, black outside the buffer as unpack_fb_row reads it).
     glCopyTexSubImage2D was +15% over phase 4 through the 8-bit row */
  int fbd = u->fb != NULL && u->xs == NULL && !lum &&
            (cls == TGL_TEXF_RGB || cls == TGL_TEXF_RGBA);

  for (y = 0; y < h; y++) {
    unsigned short *d = pix + (y0 + y) * TW + x0;
    unsigned char *da = al ? al + (y0 + y) * TW + x0 : NULL;
    const unsigned char *q = row;
    if (fbd) {
      int wy = u->fby + sy + y, wx = u->fbx + sx;
      const unsigned short *fr = (const unsigned short *)
        ((const char *)u->fb + (u->fbh - 1 - wy) * u->fbls);
      if (wy < 0 || wy >= u->fbh)
        for (x = 0; x < w; x++) d[x] = 0;
      else if (wx >= 0 && wx + w <= u->fbw)
        for (x = 0; x < w; x++) d[x] = fr[wx + x];
      else
        for (x = 0; x < w; x++) d[x] = wx + x >= 0 && wx + x < u->fbw ? fr[wx + x] : 0;
      if (da) for (x = 0; x < w; x++) da[x] = 255;
      continue;
    }
    if (direct) {
      const unsigned char *p = u->base + (sy + y) * u->pitch + sx * u->group;
      switch (u->format) {
      case GL_RGB:
        for (x = 0; x < w; x++, p += 3) d[x] = T565(p[0], p[1], p[2]);
        break;
      case GL_RGBA:                    /* (no plane: RGB, or an a1 texture) */
        if (da == NULL)
          for (x = 0; x < w; x++, p += 4) d[x] = T565(p[0], p[1], p[2]);
        else
          for (x = 0; x < w; x++, p += 4) { d[x] = T565(p[0], p[1], p[2]); da[x] = p[3]; }
        break;
      default:                         /* GL_LUMINANCE */
        for (x = 0; x < w; x++) d[x] = T565(p[x], p[x], p[x]);
        break;
      }
      continue;
    }
    s31_unpack_row(u, sx, sy + y, w, row);
    switch (cls) {
    case TGL_TEXF_ALPHA:
      for (x = 0; x < w; x++, q += 4) { d[x] = 0xffff; da[x] = q[3]; }
      break;
    case TGL_TEXF_INTENSITY:           /* always luminance-like: R */
      for (x = 0; x < w; x++, q += 4) { d[x] = T565(q[0], q[0], q[0]); da[x] = q[0]; }
      break;
    case TGL_TEXF_RGBA:
      if (da == NULL) {                /* an a1 texture: no plane */
        if (lum)
          for (x = 0; x < w; x++, q += 4) d[x] = T565(q[0], q[0], q[0]);
        else
          for (x = 0; x < w; x++, q += 4) d[x] = T565(q[0], q[1], q[2]);
      } else if (lum)
        for (x = 0; x < w; x++, q += 4) { d[x] = T565(q[0], q[0], q[0]); da[x] = q[3]; }
      else
        for (x = 0; x < w; x++, q += 4) { d[x] = T565(q[0], q[1], q[2]); da[x] = q[3]; }
      break;
    default:                           /* TGL_TEXF_RGB */
      if (lum)
        for (x = 0; x < w; x++, q += 4) d[x] = T565(q[0], q[0], q[0]);
      else
        for (x = 0; x < w; x++, q += 4) d[x] = T565(q[0], q[1], q[2]);
      break;
    }
  }
}

/* ------------------------------------------------------------ levels */

/* a stored level's fields, level 0 (GLTexture) and the others (GLMipLevel)
   alike */
typedef struct {
  void **pix;
  unsigned char **alpha;
  GLTexPal **pal;              /* the level's palette pointer (into its block) */
  int ws, hs, cls;
} Lv;

static int lv_get(GLTexture *t, int level, Lv *v)
{
  if (level == 0) {
    v->pix = &t->images[0].pixmap;
    v->alpha = &t->alpha;
    v->pal = &t->pal0;
    v->ws = t->ws; v->hs = t->hs; v->cls = t->fmt;
  } else {
    GLMipLevel *m;
    if (t->mip == NULL) {
      v->pix = NULL; v->alpha = NULL; v->pal = NULL;
      v->ws = v->hs = v->cls = 0;
      return 0;
    }
    m = &t->mip->l[level];
    v->pix = &m->pix;
    v->alpha = &m->alpha;
    v->pal = &m->pal;
    v->ws = m->ws; v->hs = m->hs; v->cls = m->cls;
  }
  return *v->pix != NULL;
}

/* the other stored levels of t besides `level` */
static int t8_others(GLTexture *t, int level)
{
  int l;
  Lv v;
  for (l = 0; l < MAX_TEXTURE_LEVELS; l++)
    if (l != level && lv_get(t, l, &v)) return 1;
  return 0;
}

void gl_tex8_free_level(GLTexture *t, int level)
{
  Lv v;
  if (!lv_get(t, level, &v)) {
    if (v.pal) *v.pal = NULL;
    return;
  }
  gl_free(*v.pix);
  *v.pix = NULL;
  *v.alpha = NULL;
  *v.pal = NULL;
}

/* the kind the decisions follow (a W32 reference: the kind it stands for) */
static int t8_vkind(const GLTexture *t)
{
  return t->st == TGL_ST_W32 ? t->stref : t->st;
}

/* where a P8 / W32 level's palette header starts in its block: after the
   texels, aligned for its pointers (4 on the RV32 target, 8 on a 64-bit
   host) */
#define T8_PALOFF(st, n) \
  ((((st) == TGL_ST_P8 ? (n) : (n) * 4) + (int)sizeof(void *) - 1) & ~((int)sizeof(void *) - 1))

/* bytes of a level's block */
static int t8_bytes(int st, int n, int cap, int amode, int level0, int cls, int a1)
{
  switch (st) {
  case TGL_ST_P8:
    return T8_PALOFF(st, n) + (int)sizeof(GLTexPal) + cap * 4 + (level0 ? cap * 2 : 0);
  case TGL_ST_L8:
    return n + (amode == TGL_AM_BITS ? (n + 7) >> 3 : amode == TGL_AM_A8 ? n : 0);
  case TGL_ST_W32:
    return T8_PALOFF(st, n) + (cap ? (int)sizeof(GLTexPal) + cap * 4 : 0);
  default:
    return n * 2 + (cls != TGL_TEXF_RGB && !a1 ? n : 0);
  }
}

/* one texel of a stored P8 / L8 / W32 level as its RGBA8 word */
static inline unsigned int t8_get(const GLTexture *t, const void *pix, const unsigned char *al,
                                  const GLTexPal *pal, int n, int k)
{
  (void)n;
  switch (t->st) {
  case TGL_ST_P8:
    return pal->w[((const unsigned char *)pix)[k]];
  case TGL_ST_W32:
    return ((const unsigned int *)pix)[k];
  default: {                                   /* TGL_ST_L8 */
    unsigned int l = ((const unsigned char *)pix)[k];
    switch (t->amode) {
    case TGL_AM_ONE: return T8_GREY(l, 255);
    case TGL_AM_BITS: return T8_GREY(l, (al[k >> 3] >> (k & 7) & 1) * 255u);
    case TGL_AM_A8: return T8_GREY(l, al[k]);
    case TGL_AM_I: return T8_GREY(l, l);
    default: return 0x00ffffffu | l << 24;     /* TGL_AM_ALPHA: the byte is the alpha */
    }
  }
  }
}

unsigned int gl_tex8_texel(const GLTexture *t, int level, int idx)
{
  Lv v;
  if (!lv_get((GLTexture *)t, level, &v) || t->st == TGL_ST_565) return 0;
  return t8_get(t, *v.pix, *v.alpha, *v.pal, 1 << (v.ws + v.hs), idx);
}

int gl_tex8_bits(const GLTexture *t, int level)
{
  Lv v;
  return lv_get((GLTexture *)t, level, &v) && t->st != TGL_ST_565 ? 8 : 0;
}

/* ------------------------------------------------------------ scanning */

/* the texel words of a source rectangle: the distinct ones (up to 256, in
   first-seen order, hashed), and what every texel has in common */
typedef struct {
  unsigned int pal[256];
  unsigned short h[512];       /* index + 1, 0: free */
  int n;                       /* entries; 257: more than 256 */
  int grey, a255, abin;        /* r == g == b; alpha 255; alpha 0 or 255 */
  int want;                    /* collect the palette */
} T8Scan;

static void t8_scan_init(T8Scan *s, int want)
{
  /* only the palette search reads the hash (t8_idx, want != 0): the
     per-frame L8 lightmap updates skip clearing it (and a 1 kB libc
     memset, O7) */
  if (want) memset(s->h, 0, sizeof s->h);
  s->n = 0;
  s->grey = s->a255 = s->abin = 1;
  s->want = want;
}

static inline unsigned int t8_hash(unsigned int w)
{
  return (w * 0x9E3779B1u) >> 23;
}

/* the index of w, added when new; -1 once there are more than 256 */
static int t8_idx(T8Scan *s, unsigned int w)
{
  unsigned int k = t8_hash(w), e;
  while ((e = s->h[k]) != 0) {
    if (s->pal[e - 1] == w) return (int)e - 1;
    k = (k + 1) & 511;
  }
  if (s->n >= 256) { s->n = 257; return -1; }
  s->pal[s->n] = w;
  s->h[k] = (unsigned short)++s->n;
  return s->n - 1;
}

/* a palette's entries into the hash, in order */
static void t8_scan_load(T8Scan *s, const unsigned int *w, int n)
{
  int i;
  for (i = 0; i < n; i++) t8_idx(s, w[i]);
}

/* the texel of source pixel q (RGBA8888) for class cls */
static inline unsigned int t8_word(int cls, int lum, const unsigned char *q)
{
  switch (cls) {
  case TGL_TEXF_ALPHA: return 0x00ffffffu | (unsigned int)q[3] << 24;
  case TGL_TEXF_INTENSITY: return T8_GREY(q[0], q[0]);
  case TGL_TEXF_RGB:
    if (lum) return T8_GREY(q[0], 255);
    return q[0] | (unsigned int)q[1] << 8 | (unsigned int)q[2] << 16 | 0xff000000u;
  default:
    if (lum) return T8_GREY(q[0], q[3]);
    return q[0] | (unsigned int)q[1] << 8 | (unsigned int)q[2] << 16 | (unsigned int)q[3] << 24;
  }
}

/* n texel words of source row sy from column sx */
__attribute__((optimize("O2")))
static void t8_row(const S31Unpack *u, int cls, int lum, int sx, int sy, int n,
                   unsigned int *out)
{
  unsigned char row[4 * TGL_TEX_MAX];
  const unsigned char *q = row;
  int x;
  if (u->fb == NULL && u->xs == NULL && u->type == GL_UNSIGNED_BYTE && u->format == GL_RGBA &&
      !lum && (cls == TGL_TEXF_RGBA || cls == TGL_TEXF_RGB)) {
    /* QuakeSpasm's every upload: the client's words as they are (an RGB
       class: alpha 255) */
    const unsigned char *src = u->base + sy * u->pitch + sx * 4;
    /* O7: a word loop when the client row is aligned (per-frame lightmap
       rows are 64 B and up: libc memcpy would take the ESP PIE path) */
    if (((unsigned long)src & 3) == 0)
      s31_wcopy(out, src, n);
    else
      memcpy(out, src, (size_t)n * 4);
    if (cls == TGL_TEXF_RGB)
      for (x = 0; x < n; x++) out[x] |= 0xff000000u;
    return;
  }
  s31_unpack_row(u, sx, sy, n, row);
  for (x = 0; x < n; x++, q += 4) out[x] = t8_word(cls, lum, q);
}

/* the statistics of n words, as bits no word may set: grey (bits 0-15 of
   w ^ w >> 8), alpha 255 (the AND's alpha), alpha 0 or 255 (bits 25-31 of
   w + 2^24: the alpha + 1 is 0 or 1). Only what is still open is looked
   at: a colour texture stops testing grey at its first colour, and alpha
   0 / 255 need be tested only once an alpha other than 255 has been seen
   (every word before it passes) */
__attribute__((optimize("O2")))
static void t8_stats(T8Scan *s, const unsigned int *w, int n)
{
  unsigned int acc = 0;
  int x;
  if (s->grey && s->a255) {                    /* a lightmap: one loop */
    unsigned int aand = ~0u;
    for (x = 0; x < n; x++) { acc |= w[x] ^ (w[x] >> 8); aand &= w[x]; }
    s->grey = (acc & 0xffffu) == 0;
    if ((aand >> 24) == 255u) return;
    s->a255 = 0;
    goto bin;
  }
  if (s->grey) {
    for (x = 0; x < n; x++) acc |= w[x] ^ (w[x] >> 8);
    s->grey = (acc & 0xffffu) == 0;
  }
  if (s->a255) {
    acc = ~0u;
    for (x = 0; x < n; x++) acc &= w[x];
    if ((acc >> 24) == 255u) return;
    s->a255 = 0;
  }
bin:
  if (s->abin) {
    acc = 0;
    for (x = 0; x < n; x++) acc |= (w[x] + 0x01000000u) & 0xfe000000u;
    s->abin = acc == 0;
  }
}

/* fold the words of one row into s; with ix, each texel's index into the
   palette too (while it holds: when s->n reaches 257 the rest is not
   written, and the caller does not use ix) */
__attribute__((optimize("O2")))
static void t8_fold(T8Scan *s, const unsigned int *w, int n, unsigned char *ix)
{
  int x;
  t8_stats(s, w, n);
  if (s->want && s->n <= 256) {
    unsigned int last = ~w[0];
    int li = 0;
    for (x = 0; x < n && s->n <= 256; x++) {
      if (w[x] != last) {                 /* runs: one search */
        last = w[x];
        li = t8_idx(s, last);
      }
      if (ix) ix[x] = (unsigned char)li;
    }
  }
}

/* scan a source rectangle into s; with ix (row stride ixs), each texel's
   palette index too - valid only while s->n stays <= 256 */
static void t8_scan_src(T8Scan *s, const S31Unpack *u, int cls, int lum, int sx, int sy,
                        int w, int h, unsigned char *ix, int ixs)
{
  unsigned int row[TGL_TEX_MAX];
  int y;
  for (y = 0; y < h; y++) {
    t8_row(u, cls, lum, sx, sy + y, w, row);
    t8_fold(s, row, w, ix ? ix + y * ixs : NULL);
  }
}

/* an L8 level's rectangle from u in one pass: the grey (or, AM_ALPHA, the
   alpha) bytes and the current alpha plane written while s gathers the
   statistics. What it wrote is right only if s->grey comes out true (or
   the class is a luminance / alpha / intensity one) and the alpha mode
   need not change: otherwise the caller rewrites the rectangle */
__attribute__((optimize("O2")))
static void t8_l8_pass(T8Scan *s, const GLTexture *t, unsigned char *pix, unsigned char *al,
                       int ws, int cls, int lum, const S31Unpack *u, int sx, int sy,
                       int x0, int y0, int w, int h)
{
  unsigned int row[TGL_TEX_MAX];
  int TW = 1 << ws, x, y, sh = t->amode == TGL_AM_ALPHA ? 24 : 0;
  if (u->fb == NULL && u->xs == NULL && u->type == GL_UNSIGNED_BYTE &&
      u->format == GL_RGBA && !lum && (cls == TGL_TEXF_RGBA || cls == TGL_TEXF_RGB) &&
      (t->amode == TGL_AM_ONE || (t->amode == TGL_AM_BITS && cls == TGL_TEXF_RGBA)) &&
      ((uintptr_t)u->base & 3) == 0 && (u->pitch & 3) == 0) {
    /* a lightmap update (RGBA bytes, grey and opaque so far): the
       client's words read once - the statistics and the grey byte in one
       loop; t8_row's RGB class forces alpha 255 */
    const unsigned int a1 = cls == TGL_TEXF_RGB ? 0xff000000u : 0;
    for (y = 0; y < h; y++) {
      const unsigned int *q = (const unsigned int *)(u->base + (sy + y) * u->pitch) + sx;
      int k0 = (y0 + y) * TW + x0;
      unsigned char *d = pix + k0;
      unsigned int gacc = 0, aand = ~0u, abad = 0;
      if (t->amode == TGL_AM_BITS) {
        /* (QuakeSpasm's lightmap blocks: opaque texels, and alpha 0 in
           the area no surface has used yet - rows of both) */
        for (x = 0; x < w; x++) {
          unsigned int v = q[x];
          int k = k0 + x;
          gacc |= v ^ (v >> 8);
          aand &= v;
          abad |= (v + 0x01000000u) & 0xfe000000u;
          d[x] = (unsigned char)v;
          if (v >> 31) al[k >> 3] |= (unsigned char)(1u << (k & 7));
          else al[k >> 3] &= (unsigned char)~(1u << (k & 7));
        }
      } else {
        for (x = 0; x < w; x++) {
          unsigned int v = q[x];
          gacc |= v ^ (v >> 8);
          aand &= v;
          d[x] = (unsigned char)v;
        }
      }
      if (s->grey) s->grey = (gacc & 0xffffu) == 0;
      if (t->amode == TGL_AM_BITS) {
        if (((aand | a1) >> 24) != 255u) s->a255 = 0;
        if (abad) s->abin = 0;
      } else if ((!s->a255 || ((aand | a1) >> 24) != 255u) && s->abin) {
        /* an alpha other than 255 (rare: a lightmap is opaque): the row's
           statistics again, the general way, for alpha 0 / 255 */
        t8_row(u, cls, lum, sx, sy + y, w, row);
        t8_stats(s, row, w);
      }
    }
    return;
  }
  for (y = 0; y < h; y++) {
    int k0 = (y0 + y) * TW + x0;
    unsigned char *d = pix + k0;
    t8_row(u, cls, lum, sx, sy + y, w, row);
    t8_stats(s, row, w);
    for (x = 0; x < w; x++) d[x] = (unsigned char)(row[x] >> sh);
    if (t->amode == TGL_AM_A8) {
      for (x = 0; x < w; x++) al[k0 + x] = (unsigned char)(row[x] >> 24);
    } else if (t->amode == TGL_AM_BITS) {
      for (x = 0; x < w; x++) {
        int k = k0 + x;
        if (row[x] >> 31) al[k >> 3] |= (unsigned char)(1u << (k & 7));
        else al[k >> 3] &= (unsigned char)~(1u << (k & 7));
      }
    }
  }
}

/* index 0 over a rectangle of a P8 level whose indices a scan left
   unusable (a palette that overflowed): every stored index valid again */
static void t8_zero_rect(unsigned char *pix, int ws, int x0, int y0, int w, int h)
{
  int y;
  for (y = 0; y < h; y++) memset(pix + ((y0 + y) << ws) + x0, 0, (size_t)w);
}

/* the L8 alpha mode the texels of s need */
static int t8_amode(const T8Scan *s, int cls, int lum)
{
  if (cls == TGL_TEXF_ALPHA) return TGL_AM_ALPHA;
  if (cls == TGL_TEXF_INTENSITY) return TGL_AM_I;
  if (cls == TGL_TEXF_RGB) return TGL_AM_ONE;
  (void)lum;
  return s->a255 ? TGL_AM_ONE : (s->abin ? TGL_AM_BITS : TGL_AM_A8);
}

/* every entry of s's palette survives RGB565 unchanged: UNPACK(PACK(w))
   is w in r, g and b (0 and 255 always; texobj's red / green / white, a
   black undefined image) */
static int t8_pal565(const T8Scan *s)
{
  int k;
  for (k = 0; k < s->n; k++) {
    unsigned int w = s->pal[k], r = w & 255, g = (w >> 8) & 255, b = (w >> 16) & 255;
    if ((((r & 0xf8) | (r >> 5)) != r) | (((g & 0xfc) | (g >> 6)) != g) |
        (((b & 0xf8) | (b >> 5)) != b))
      return 0;
  }
  return 1;
}

/* the kind a level with texels s would have on its own. (fix 2: a colour
   RGB / RGBA image whose every colour is exact in 565 is stored as 565 -
   the texels read back the same, and tier 1 and the phase-4 stages fetch
   565 without the palette load: texobj was +10% over phase 4, bench P2.
   A grey one stays L8: a lightmap that starts black keeps its 8 bits) */
static int t8_kind(const GLContext *c, const T8Scan *s, int cls, int lum)
{
  if (c->tex8 == 0) return TGL_ST_565;
  if (cls == TGL_TEXF_ALPHA || cls == TGL_TEXF_INTENSITY || lum) return TGL_ST_L8;
  if (s->grey) return TGL_ST_L8;
  if (s->n <= 256 && t8_pal565(s)) return TGL_ST_565;
  return s->n <= 256 ? TGL_ST_P8 : TGL_ST_565;
}

/* ------------------------------------------------------------ writing */

static void pal_set(GLTexPal *p, const unsigned int *w, int n, int from, int level0)
{
  int i;
  for (i = from; i < n; i++) {
    unsigned int v = w[i];
    p->w[i] = v;
    if (level0 && p->p565) p->p565[i] = PACK(v & 255, (v >> 8) & 255, (v >> 16) & 255);
    if (((v ^ (v >> 8)) & 0xffff) != 0) p->grey = 0;
  }
  p->n = (unsigned short)n;
}

/* rows of a P8 / L8 / W32 level from source u; s holds the level's palette
   (P8 and a P8 reference) */
__attribute__((optimize("O2")))
static void t8_write(const GLTexture *t, void *pix, unsigned char *al, int ws, int cls,
                     int lum, T8Scan *s, const S31Unpack *u, int sx, int sy, int x0,
                     int y0, int w, int h)
{
  unsigned int row[TGL_TEX_MAX];
  int TW = 1 << ws, x, y;
  for (y = 0; y < h; y++) {
    int k0 = (y0 + y) * TW + x0;
    t8_row(u, cls, lum, sx, sy + y, w, row);
    switch (t->st) {
    case TGL_ST_P8: {
      unsigned char *d = (unsigned char *)pix + k0;
      unsigned int last = ~row[0];
      int li = 0;
      for (x = 0; x < w; x++) {
        if (row[x] != last) { last = row[x]; li = t8_idx(s, last); }
        d[x] = (unsigned char)li;
      }
      break;
    }
    case TGL_ST_W32:
      memcpy((unsigned int *)pix + k0, row, (size_t)w * 4);
      break;
    default: {                                 /* TGL_ST_L8 */
      unsigned char *d = (unsigned char *)pix + k0;
      if (t->amode == TGL_AM_ALPHA) {
        for (x = 0; x < w; x++) d[x] = (unsigned char)(row[x] >> 24);
        break;
      }
      for (x = 0; x < w; x++) d[x] = (unsigned char)row[x];
      if (t->amode == TGL_AM_A8) {
        for (x = 0; x < w; x++) al[k0 + x] = (unsigned char)(row[x] >> 24);
      } else if (t->amode == TGL_AM_BITS) {
        for (x = 0; x < w; x++) {
          int k = k0 + x;
          if (row[x] >> 31) al[k >> 3] |= (unsigned char)(1u << (k & 7));
          else al[k >> 3] &= (unsigned char)~(1u << (k & 7));
        }
      }
      break;
    }
    }
  }
  (void)cls;
}

/* (re)make level v's block for kind st (palette cap), or keep it (reuse:
   the same shape and layout - TyrQuake re-specifies its lightmaps every
   frame); fills in the plane pointers. 0: no memory (the level keeps its
   old block) */
static int lv_alloc(GLContext *c, GLTexture *t, int level, Lv *v, int st, int cap, int a1,
                    int reuse)
{
  int n = 1 << (v->ws + v->hs);
  unsigned char *b = *v->pix;
  GLTexPal *P;
  if (!reuse) {
    int need = t8_bytes(st, n, cap, t->amode, level == 0, v->cls, a1);
    b = gl_malloc(need);
    if (b == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
    gl_free(*v->pix);
    *v->pix = b;
  } else {
    cap = *v->pal ? (*v->pal)->cap : 0;
  }
  *v->alpha = NULL;
  *v->pal = NULL;
  switch (st) {
  case TGL_ST_P8:
  case TGL_ST_W32:
    if (st == TGL_ST_W32 && cap == 0) break;
    P = (GLTexPal *)(b + T8_PALOFF(st, n));
    memset(P, 0, sizeof *P);
    P->w = (unsigned int *)(P + 1);
    P->p565 = st == TGL_ST_P8 && level == 0 ? (unsigned short *)(P->w + cap) : NULL;
    P->cap = (unsigned short)cap;
    P->grey = 1;
    *v->pal = P;
    break;
  case TGL_ST_L8:
    if (t->amode == TGL_AM_BITS || t->amode == TGL_AM_A8) *v->alpha = b + n;
    if (t->amode == TGL_AM_BITS) memset(b + n, 0xff, (size_t)((n + 7) >> 3));
    break;
  default:
    if (v->cls != TGL_TEXF_RGB && !a1) *v->alpha = b + n * 2;
    break;
  }
  return 1;
}

/* the palette capacity for n entries of a level of `texels` texels: room
   to grow (a glTexSubImage) at a quarter of the texel bytes at most */
static int pal_cap(int n, int texels)
{
  int cap = n + (n >> 2) + 8;
  cap = (cap + 7) & ~7;
  if (cap > 256) cap = 256;
  if (cap * 4 > texels / 4 && cap > n) cap = n > 8 ? (n + 7) & ~7 : 8;
  if (cap < n) cap = n;
  return cap;
}

/* ------------------------------------------------------------ converting */

/* every stored level of t to 565 (exactly, from the 8-bit texels), with A8
   planes unless the class has none or (a1) every alpha is 255. 0: no
   memory (nothing changed) */
static int t8_to565(GLContext *c, GLTexture *t)
{
  void *nb[MAX_TEXTURE_LEVELS];
  int l, k, a1 = 1, ok = 1;
  Lv v;
  if (t->st == TGL_ST_565) return 1;
  /* a1: an RGBA-class texture whose texels are all opaque (phase 5 (d)) */
  for (l = 0; l < MAX_TEXTURE_LEVELS; l++) {
    int n;
    if (!lv_get(t, l, &v)) continue;
    n = 1 << (v.ws + v.hs);
    if (v.cls != TGL_TEXF_RGBA) { a1 = 0; break; }
    for (k = 0; k < n && a1; k++)
      a1 = t8_get(t, *v.pix, *v.alpha, *v.pal, n, k) >> 24 == 255;
  }
  if (t->st == TGL_ST_W32) a1 = 0;             /* the reference keeps the plane */
  for (l = 0; l < MAX_TEXTURE_LEVELS; l++) {
    nb[l] = NULL;
    if (!lv_get(t, l, &v)) continue;
    nb[l] = gl_malloc(t8_bytes(TGL_ST_565, 1 << (v.ws + v.hs), 0, 0, 0, v.cls, a1));
    if (nb[l] == NULL) ok = 0;
  }
  if (!ok) {
    for (l = 0; l < MAX_TEXTURE_LEVELS; l++) gl_free(nb[l]);
    gl_set_error(c, GL_OUT_OF_MEMORY);
    return 0;
  }
  for (l = 0; l < MAX_TEXTURE_LEVELS; l++) {
    int n;
    unsigned short *d;
    unsigned char *da;
    if (!lv_get(t, l, &v)) continue;
    n = 1 << (v.ws + v.hs);
    d = nb[l];
    da = v.cls != TGL_TEXF_RGB && !a1 ? (unsigned char *)nb[l] + n * 2 : NULL;
    for (k = 0; k < n; k++) {
      unsigned int w = t8_get(t, *v.pix, *v.alpha, *v.pal, n, k);
      unsigned int r = w & 255, g = (w >> 8) & 255, b = (w >> 16) & 255;
      d[k] = v.cls == TGL_TEXF_ALPHA ? 0xffff : T565(r, g, b);
      if (da) da[k] = (unsigned char)(w >> 24);
    }
    gl_free(*v.pix);
    *v.pix = nb[l];
    *v.alpha = da;
    *v.pal = NULL;
  }
  t->st = TGL_ST_565;
  t->stref = 0;
  t->amode = 0;
  t->a1 = (unsigned char)a1;
  c->raster_dirty = 1;
  return 1;
}

/* an a1 texture's levels get their A8 planes (all 255) */
static int t8_add_alpha(GLContext *c, GLTexture *t)
{
  int l;
  Lv v;
  if (!t->a1) return 1;
  for (l = 0; l < MAX_TEXTURE_LEVELS; l++) {
    int n;
    unsigned char *b;
    if (!lv_get(t, l, &v)) continue;
    n = 1 << (v.ws + v.hs);
    b = gl_malloc(n * 3);
    if (b == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
    memcpy(b, *v.pix, (size_t)n * 2);
    memset(b + n * 2, 255, (size_t)n);
    gl_free(*v.pix);
    *v.pix = b;
    *v.alpha = b + n * 2;
  }
  t->a1 = 0;
  c->raster_dirty = 1;
  return 1;
}

/* an L8 texture's alpha mode to am (a wider one), every level */
static int t8_amode_to(GLContext *c, GLTexture *t, int am)
{
  int l, k;
  Lv v;
  if (t->amode == am) return 1;
  /* the W32 reference keeps every alpha in its words: only the mode moves */
  for (l = 0; l < MAX_TEXTURE_LEVELS && t->st == TGL_ST_L8; l++) {
    int n, old = t->amode;
    unsigned char *b, *al;
    if (!lv_get(t, l, &v)) continue;
    n = 1 << (v.ws + v.hs);
    b = gl_malloc(t8_bytes(TGL_ST_L8, n, 0, am, 0, v.cls, 0));
    if (b == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
    memcpy(b, *v.pix, (size_t)n);
    al = b + n;
    if (am == TGL_AM_BITS) memset(al, 0xff, (size_t)((n + 7) >> 3));
    for (k = 0; k < n && am == TGL_AM_A8; k++) {
      unsigned int a = old == TGL_AM_BITS ? ((*v.alpha)[k >> 3] >> (k & 7) & 1) * 255u : 255u;
      al[k] = (unsigned char)a;
    }
    gl_free(*v.pix);
    *v.pix = b;
    *v.alpha = am == TGL_AM_ONE ? NULL : al;
  }
  t->amode = (unsigned char)am;
  c->raster_dirty = 1;
  return 1;
}

/* drop the entries of a P8 level (or a P8 reference's) that no texel
   outside [x0, x0 + w) x [y0, y0 + h) uses - the rectangle is about to
   be overwritten - renumbering the texels (a lightmap's old values). The
   reference rebuilds its palette from its words in first-seen order: the
   entries differ in order from P8's, never in number, so both make the
   same decisions */
static void t8_compact(GLTexture *t, Lv *v, int x0, int y0, int w, int h)
{
  int TW = 1 << v->ws, n = 1 << (v->ws + v->hs), k, m = 0, i;
  if (t->st == TGL_ST_W32) {
    static T8Scan r;
    const unsigned int *wd = *v->pix;
    t8_scan_init(&r, 1);
    for (k = 0; k < n; k++) {
      int x = k & (TW - 1), y = k >> v->ws;
      if (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h) continue;
      t8_idx(&r, wd[k]);
    }
    GLTexPal *P = *v->pal;
    for (i = 0; i < r.n && i < P->cap; i++) P->w[i] = r.pal[i];
    P->n = (unsigned short)(r.n < P->cap ? r.n : P->cap);
    return;
  }
  {
    unsigned char used[256], map[256];
    unsigned char *ix = *v->pix;
    GLTexPal *P = *v->pal;
    memset(used, 0, sizeof used);
    for (k = 0; k < n; k++) {
      int x = k & (TW - 1), y = k >> v->ws;
      if (x >= x0 && x < x0 + w && y >= y0 && y < y0 + h) continue;
      used[ix[k]] = 1;
    }
    for (i = 0; i < P->n; i++) {
      if (!used[i]) { map[i] = 0; continue; }
      map[i] = (unsigned char)m;
      P->w[m] = P->w[i];
      if (P->p565) P->p565[m] = P->p565[i];
      m++;
    }
    /* (a texel in the rectangle keeps any index: it is rewritten next) */
    for (k = 0; k < n; k++) ix[k] = map[ix[k]];
    P->n = (unsigned short)m;
  }
}

/* a P8 level's block with room for cap entries (the texels and the
   entries kept) */
static int t8_grow(GLContext *c, GLTexture *t, int level, Lv *v, int cap)
{
  int n = 1 << (v->ws + v->hs), l0 = level == 0 && t->st == TGL_ST_P8;
  GLTexPal old = **v->pal, *P;
  int tb = T8_PALOFF(t->st, n);
  unsigned char *b = gl_malloc(t8_bytes(t->st, n, cap, 0, l0, v->cls, 0));
  if (b == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
  memcpy(b, *v->pix, (size_t)(t->st == TGL_ST_P8 ? n : n * 4));
  P = (GLTexPal *)(b + tb);
  *P = old;
  P->w = (unsigned int *)(P + 1);
  memcpy(P->w, old.w, (size_t)old.n * 4);
  P->p565 = l0 ? (unsigned short *)(P->w + cap) : NULL;
  if (l0 && old.p565) memcpy(P->p565, old.p565, (size_t)old.n * 2);
  P->cap = (unsigned short)cap;
  gl_free(*v->pix);
  *v->pix = b;
  *v->pal = P;
  return 1;
}

/* ------------------------------------------------------------ the entry points */

int gl_tex8_image(GLContext *c, GLTexture *t, int level, int ws, int hs, int ifmt,
                  const S31Unpack *u, int sx, int sy)
{
  static T8Scan s;               /* (1.5 kB: not on a thread's stack) */
  int lum, cls = gl_tex_class(ifmt, &lum), n = 1 << (ws + hs), st, am = 0, cap = 0;
  int others, a1 = 0, vk, shape, pst, pam, pa1, pcap, r;
  Lv v;

  if (level > 0 && t->mip == NULL) {
    t->mip = gl_zalloc(sizeof(GLMipChain));
    if (t->mip == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
  }
  /* the level as it is (texture.c records level 0's new shape after this
     call), and what it was stored as */
  shape = lv_get(t, level, &v) && v.ws == ws && v.hs == hs && v.cls == cls;
  pst = t->st; pam = t->amode; pa1 = t->a1; pcap = *v.pal ? (*v.pal)->cap : 0;
  if (level > 0) {
    GLMipLevel *m = &t->mip->l[level];
    m->ws = (unsigned char)ws; m->hs = (unsigned char)hs; m->cls = (unsigned char)cls;
  }
  v.ws = ws; v.hs = hs; v.cls = cls;
  others = t8_others(t, level);

  if (c->tex8 == 0) {
    /* phase 4's storage, exactly: 565 (+ A8 by class), the block reused
       when its shape and class are the same, black for no pixels */
    int has_alpha = cls != TGL_TEXF_RGB, need = n * 2 + (has_alpha ? n : 0);
    void *b;
    if (t->st != TGL_ST_565 && others && !t8_to565(c, t)) return 0;
    if (!shape || pst != TGL_ST_565 || (*v.alpha != NULL) != has_alpha) {
      b = gl_malloc(need);
      if (b == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
      gl_free(*v.pix);
      *v.pix = b;
    }
    b = *v.pix;
    *v.pal = NULL;
    t->st = TGL_ST_565; t->stref = 0; t->a1 = 0; t->amode = 0; t->x565 = 0;
    *v.alpha = has_alpha ? (unsigned char *)b + n * 2 : NULL;
    if (u == NULL) memset(b, 0, (size_t)need);
    else st565_rows(b, *v.alpha, ws, ifmt, u, sx, sy, 0, 0, 1 << ws, 1 << hs);
    return 1;
  }
  if (!others) {
    t->st = TGL_ST_565; t->stref = 0; t->a1 = 0; t->amode = 0; t->x565 = 0;
  }

  /* what the image holds */
  t8_scan_init(&s, 1);
  if (u) {
    /* (two passes: an index plane written during the scan measured 270 ->
       237 M instructions for QuakeSpasm's load, but its transient n bytes
       raise the load's peak heap by 57 kB; memory is the constraint) */
    t8_scan_src(&s, u, cls, lum, sx, sy, 1 << ws, 1 << hs, NULL, 0);
  } else {
    /* black (GL leaves the contents undefined): transparent for the
       alpha classes */
    static const unsigned char z[4] = { 0, 0, 0, 0 };
    unsigned int w = t8_word(cls, lum, z);
    t8_fold(&s, &w, 1, NULL);
  }
  st = t8_kind(c, &s, cls, lum);
  if (!others && st == TGL_ST_565 && s.n <= 256) t->x565 = 1;   /* t8_kind's exact rule */
  if (st == TGL_ST_L8) am = t8_amode(&s, cls, lum);
  vk = others ? t8_vkind(t) : st;
  if (others && (vk != st || (st == TGL_ST_L8 && (am >= TGL_AM_I || t->amode >= TGL_AM_I) &&
                              am != t->amode))) {
    /* the level does not fit the texture's kind: 565, every level */
    if (!t8_to565(c, t)) { r = 0; goto out; }
    st = TGL_ST_565;
    shape = 0;
  }
  if (st == TGL_ST_565) {
    a1 = c->tex8 == 1 && cls == TGL_TEXF_RGBA && s.a255 && (!others || t->a1);
    if (others && t->a1 && !a1 && cls != TGL_TEXF_RGB && !t8_add_alpha(c, t)) { r = 0; goto out; }
    if (!others) t->a1 = (unsigned char)a1;
    t->st = TGL_ST_565;
    if (!lv_alloc(c, t, level, &v, TGL_ST_565, 0, t->a1,
                  shape && pst == TGL_ST_565 && pa1 == t->a1)) { r = 0; goto out; }
    if (u == NULL) memset(*v.pix, 0, (size_t)t8_bytes(TGL_ST_565, n, 0, 0, 0, cls, t->a1));
    else st565_rows(*v.pix, *v.alpha, ws, ifmt, u, sx, sy, 0, 0, 1 << ws, 1 << hs);
    { r = 1; goto out; }
  }
  if (st == TGL_ST_L8) {
    if (others && am < TGL_AM_I && t->amode < TGL_AM_I && am < t->amode) am = t->amode;
    if (others && am > t->amode && !t8_amode_to(c, t, am)) { r = 0; goto out; }
    t->amode = (unsigned char)am;
  }
  /* P8 or L8 (a W32 reference of either) */
  t->st = c->tex8 == 2 ? TGL_ST_W32 : (unsigned char)st;
  t->stref = (unsigned char)st;
  if (st == TGL_ST_P8) cap = pal_cap(s.n, n);
  if (!lv_alloc(c, t, level, &v, t->st, cap, 0,
                shape && pst == t->st && (st != TGL_ST_L8 || pam == am) &&
                (st != TGL_ST_P8 || pcap >= s.n))) { r = 0; goto out; }
  if (st == TGL_ST_P8) pal_set(*v.pal, s.pal, s.n, 0, level == 0 && t->st == TGL_ST_P8);
  c->raster_dirty = 1;
  if (u == NULL) {
    unsigned int w = s.pal[0];
    if (t->st == TGL_ST_P8) memset(*v.pix, 0, (size_t)n);
    else if (t->st == TGL_ST_W32) { int k; for (k = 0; k < n; k++) ((unsigned int *)*v.pix)[k] = w; }
    else {
      memset(*v.pix, t->amode == TGL_AM_ALPHA ? (int)(w >> 24) : (int)(w & 255), (size_t)n);
      if (t->amode == TGL_AM_A8) memset(*v.alpha, (int)(w >> 24), (size_t)n);
      else if (t->amode == TGL_AM_BITS) memset(*v.alpha, w >> 31 ? 0xff : 0, (size_t)((n + 7) >> 3));
    }
    { r = 1; goto out; }
  }
  t8_write(t, *v.pix, *v.alpha, ws, cls, lum, &s, u, sx, sy, 0, 0, 1 << ws, 1 << hs);
  r = 1;
out:
  return r;
}

int gl_tex8_sub(GLContext *c, GLTexture *t, int level, const S31Unpack *u,
                int sx, int sy, int x0, int y0, int w, int h)
{
  static T8Scan s;
  int lum, cls, vk, ifmt = t->lfmt[level];
  Lv v;

  if (!lv_get(t, level, &v)) return 0;
  cls = gl_tex_class(ifmt, &lum);
  if (u->fb != NULL && u->xs == NULL && !lum && t->st != TGL_ST_565 &&
      (cls == TGL_TEXF_RGB || cls == TGL_TEXF_RGBA) && x0 == 0 && y0 == 0 &&
      w == 1 << v.ws && h == 1 << v.hs && !t8_others(t, level)) {
    /* (fix 2, bench P2) glCopyTexSubImage2D from the colour buffer over the
       whole of a texture's only level (RGB / RGBA): the buffer is RGB565,
       so a 565 texture holds the copy exactly - no texel is left to
       requantise - and st565_rows stores it as it is, where every copy
       paid the statistics pass before (pix6 +38% over phase 4: an L8
       texture made with no pixels, black, stayed L8 while black was
       copied into it). A partial copy, or one into a mipmapped texture,
       keeps the kind rules below */
    if (!t8_to565(c, t)) return 0;
    lv_get(t, level, &v);
  }
  vk = t8_vkind(t);
  if (vk == TGL_ST_L8 && t->st == TGL_ST_L8) {
    /* one pass (the per-frame lightmap update): written while checked */
    int am;
    t8_scan_init(&s, 0);
    t8_l8_pass(&s, t, *v.pix, *v.alpha, v.ws, cls, lum, u, sx, sy, x0, y0, w, h);
    if (!s.grey && cls != TGL_TEXF_ALPHA && cls != TGL_TEXF_INTENSITY && !lum) {
      /* (the rectangle's bytes are rewritten below, as 565) */
      if (!t8_to565(c, t)) return 0;
      lv_get(t, level, &v);
    } else {
      am = t8_amode(&s, cls, lum);
      if (am < TGL_AM_I && am > t->amode) {
        if (!t8_amode_to(c, t, am)) return 0;
        lv_get(t, level, &v);
        t8_write(t, *v.pix, *v.alpha, v.ws, cls, lum, &s, u, sx, sy, x0, y0, w, h);
      }
      return 1;
    }
  } else if (vk == TGL_ST_L8) {                /* the W32 reference */
    int am;
    t8_scan_init(&s, 0);
    t8_scan_src(&s, u, cls, lum, sx, sy, w, h, NULL, 0);
    if (!s.grey && cls != TGL_TEXF_ALPHA && cls != TGL_TEXF_INTENSITY && !lum) {
      if (!t8_to565(c, t)) return 0;
      lv_get(t, level, &v);
    } else {
      am = t8_amode(&s, cls, lum);
      if (am < TGL_AM_I && am > t->amode && !t8_amode_to(c, t, am)) return 0;
      lv_get(t, level, &v);
      t8_write(t, *v.pix, *v.alpha, v.ws, cls, lum, &s, u, sx, sy, x0, y0, w, h);
      return 1;
    }
  } else if (vk == TGL_ST_P8) {
    /* P8: the indices go straight into the level as they are found (right
       once the palette holds); W32 (the reference): a second pass */
    int n0 = (*v.pal)->n, p8 = t->st == TGL_ST_P8;
    unsigned char *ix = p8 ? (unsigned char *)*v.pix + (y0 << v.ws) + x0 : NULL;
    t8_scan_init(&s, 1);
    t8_scan_load(&s, (*v.pal)->w, n0);
    t8_scan_src(&s, u, cls, lum, sx, sy, w, h, ix, 1 << v.ws);
    if (s.n > 256) {
      /* the entries no texel will use any more (a lightmap's old values) */
      if (p8) t8_zero_rect(*v.pix, v.ws, x0, y0, w, h);
      t8_compact(t, &v, x0, y0, w, h);
      n0 = (*v.pal)->n;
      t8_scan_init(&s, 1);
      t8_scan_load(&s, (*v.pal)->w, n0);
      t8_scan_src(&s, u, cls, lum, sx, sy, w, h, ix, 1 << v.ws);
    }
    if (s.n > 256) {
      if (p8) t8_zero_rect(*v.pix, v.ws, x0, y0, w, h);
      if (!t8_to565(c, t)) return 0;
      lv_get(t, level, &v);
    } else {
      if (s.n > (*v.pal)->cap) {
        int cap = pal_cap(s.n, 1 << (v.ws + v.hs));
        if (cap < s.n) cap = s.n;
        if (!t8_grow(c, t, level, &v, cap)) {
          if (p8) t8_zero_rect(*v.pix, v.ws, x0, y0, w, h);
          return 0;
        }
      }
      pal_set(*v.pal, s.pal, s.n, n0, level == 0 && t->st == TGL_ST_P8);
      c->raster_dirty = 1;
      if (!p8) t8_write(t, *v.pix, *v.alpha, v.ws, cls, lum, &s, u, sx, sy, x0, y0, w, h);
      return 1;
    }
  }
  /* 565 */
  if (t->a1 && cls != TGL_TEXF_RGB) {
    t8_scan_init(&s, 0);
    t8_scan_src(&s, u, cls, lum, sx, sy, w, h, NULL, 0);
    if (!s.a255) {
      if (!t8_add_alpha(c, t)) return 0;
      lv_get(t, level, &v);
    }
  }
  st565_rows(*v.pix, *v.alpha, v.ws, ifmt, u, sx, sy, x0, y0, w, h);
  return 1;
}
