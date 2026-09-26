/* s31_pixels.h - client pixel unpacking; see s31_pixels.c. s31, MIT. */
#ifndef S31_PIXELS_H
#define S31_PIXELS_H

typedef struct {
  const unsigned char *base;   /* first pixel after SKIP_ROWS/SKIP_PIXELS */
  int pitch;                   /* bytes between rows (ROW_LENGTH, ALIGNMENT) */
  int group;                   /* bytes per pixel */
  int elem;                    /* bytes per component */
  int format, type, swap;
  int width, height;
  /* glPixelTransfer scale and bias (NULL: the identity), applied by
     s31_unpack_row to every source (plan F7) */
  const float *xs, *xb;
  /* S31_FB_565: the source is an RGB565 buffer (the colour buffer, or a
     copy of part of it): image pixel (i, j) is window (fbx + i, fby + j),
     buffer row fbh - 1 - (fby + j); outside the buffer reads black */
  const unsigned short *fb;
  int fbx, fby, fbw, fbh, fbls;
} S31Unpack;

int s31_unpack_setup(GLContext *c, S31Unpack *u, int width, int height,
                     int format, int type, const void *pixels);
void s31_unpack_pixel(const S31Unpack *u, int x, int y, unsigned char *rgba);
/* n pixels of row y from column x, as RGBA8888 */
void s31_unpack_row(const S31Unpack *u, int x, int y, int n, unsigned char *rgba);

/* Display lists keep pixel data unpacked at compile time (GL 1.3 5.4) in
   this format: tightly packed RGBA8888, no pixel-store state applies. */
#define S31_PACKED_RGBA 0x7ffe0001
/* the image as a display list keeps it: a gl_malloc'd block whose data
   (S31_PACKED_RGBA) starts S31_BLOCK_HDR bytes in, the header left for
   gl_list_own's chain; or NULL (then the op keeps the application's
   arguments and reports any error at execution) */
#define S31_BLOCK_HDR 8

/* a source in RGB565 (s31_unpack_fb) */
#define S31_FB_565 0x7ffe0002
void s31_unpack_fb(GLContext *c, S31Unpack *u, const unsigned short *buf,
                   int bw, int bh, int linesize, int x, int y, int w, int h);
/* 5/6-bit channel to 8 bits, rounded as v * 255 / 31 (or 63) */
extern const unsigned char s31_c5to8[32], s31_c6to8[64];

/* client pixel packing (GL 1.3 4.3.2, glReadPixels / glGetTexImage):
   s31_pack_setup validates and resolves GL_PACK_*; s31_pack_span stores
   n pixels of row y from column x, from float RGBA (or depth in [0], with
   format GL_DEPTH_COMPONENT) with the pixel transfer already applied */
typedef struct {
  unsigned char *base;
  int pitch, group, elem, comps, format, type, swap;
} S31Pack;
int s31_pack_setup(GLContext *c, S31Pack *k, int width, int height,
                   int format, int type, void *pixels);   /* 0 or the GL error */
void s31_pack_span(const S31Pack *k, int x, int y, int n, const float *v);

/* 1-bit images (glBitmap, glPolygonStipple): GL_UNPACK_* for GL_BITMAP */
typedef struct {
  const unsigned char *base;
  int pitch, skip, lsb;
} S31Bits;
void s31_bits_setup(GLContext *c, S31Bits *b, int width, const void *bits);
static inline int s31_bit(const S31Bits *b, int x, int y)
{
  int k = b->skip + x;
  unsigned int v = b->base[y * b->pitch + (k >> 3)];
  return b->lsb ? (v >> (k & 7)) & 1 : (v >> (7 - (k & 7))) & 1;
}
/* a display list's copy of a bitmap: rows of (w + 7) / 8 bytes, MSB
   first (the canonical form, no pixel store); NULL on failure */
void *s31_bits_copy(GLContext *c, int width, int height, const void *bits);

void *s31_unpack_copy(GLContext *c, int width, int height, int format,
                      int type, const void *pixels);

/* texture.c: glTexImage / glTexSubImage from an already set-up source
   (glCopyTexImage: the colour buffer); src NULL = the op's own pixels */
int gl_tex_image_src(GLContext *c, GLParam *p, const S31Unpack *src);
int gl_tex_subimage_src(GLContext *c, GLParam *p, const S31Unpack *src);

#endif
