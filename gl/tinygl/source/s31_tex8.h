/* s31_tex8.h - phase 5 texture storage (s31_tex8.c). s31, MIT. */
#ifndef S31_TEX8_H
#define S31_TEX8_H

#include "zgl.h"
#include "s31_pixels.h"

/* the base format class of an internal format (TGL_TEXF_*); *lum: a
   luminance-like format (the colour is its first component) */
int gl_tex_class(int ifmt, int *lum);

/* Store level `level` of t: ws x hs (log2), internal format ifmt, from u
   (the image's pixel (sx + i, sy + j) is texel (i, j)) or black (u NULL).
   The level's kind follows t's other stored levels and the image
   (s31_tex8.c: P8, L8 or 565). 0: no memory (GL_OUT_OF_MEMORY set, the
   level is not stored) */
int gl_tex8_image(GLContext *c, GLTexture *t, int level, int ws, int hs, int ifmt,
                  const S31Unpack *u, int sx, int sy);
/* texels [x0, x0 + w) x [y0, y0 + h) of stored level `level` from u at
   (sx, sy). 0: no memory */
int gl_tex8_sub(GLContext *c, GLTexture *t, int level, const S31Unpack *u,
                int sx, int sy, int x0, int y0, int w, int h);
/* free level `level`'s block (level 0: images[0]; the fields stay) */
void gl_tex8_free_level(GLTexture *t, int level);

/* one texel of a stored level as RGBA8 (r | g << 8 | b << 16 | a << 24):
   glGetTexImage */
unsigned int gl_tex8_texel(const GLTexture *t, int level, int idx);

/* the stored texels' bits per channel (glGetTexLevelParameter): 8 for P8,
   L8 and W32, else 0 */
int gl_tex8_bits(const GLTexture *t, int level);

/* S31GL_TEX8 (GLContext.tex8), read at glInit */
int gl_tex8_knob(void);

/* the word of a grey value l with alpha a */
#define T8_GREY(l, a) ((unsigned int)(l) * 0x010101u | (unsigned int)(a) << 24)

#endif
