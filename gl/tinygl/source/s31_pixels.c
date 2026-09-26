/*
 * s31_pixels.c - client pixel unpacking (GL 1.3 section 3.6). s31, MIT.
 *
 * s31_unpack_setup() validates format/type and resolves the GL_UNPACK_*
 * state into a row pitch and origin; s31_unpack_row() then converts any
 * list of source columns of one row to RGBA8888. Texture upload uses it to
 * sample only the texels it keeps, so no full-size RGBA copy of the
 * application's image is ever made (a 1024x1024 upload would otherwise
 * need 4 MB of the board's 15 MB).
 *
 * Supported: formats RGB, RGBA, BGR, BGRA, RED, GREEN, BLUE, ALPHA,
 * LUMINANCE, LUMINANCE_ALPHA (+ INTENSITY-style internal formats are an
 * internal-format question, not this one); types UNSIGNED_BYTE, BYTE,
 * UNSIGNED_SHORT, SHORT, UNSIGNED_INT, INT, FLOAT, and the packed
 * UNSIGNED_SHORT_5_6_5, _4_4_4_4, _5_5_5_1, UNSIGNED_INT_8_8_8_8(_REV).
 * COLOR_INDEX, STENCIL_INDEX, DEPTH_COMPONENT and GL_BITMAP are not.
 */
#include "zgl.h"
#include "s31_pixels.h"

static int format_components(int format)
{
  switch (format) {
  case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA:
  case GL_LUMINANCE:
    return 1;
  case GL_LUMINANCE_ALPHA:
    return 2;
  case GL_RGB: case GL_BGR:
    return 3;
  case GL_RGBA: case GL_BGRA:
    return 4;
  default:
    return 0;
  }
}

/* 0 = ok, else the GL error */
int s31_unpack_setup(GLContext *c, S31Unpack *u, int width, int height,
                     int format, int type, const void *pixels)
{
  int comps = format_components(format);
  int elem, row_len, align;

  /* glPixelTransfer applies at execution to every source (GL 1.3 3.6.3),
     display lists' copies included */
  u->xs = c->xfer_active ? c->xfer_scale : NULL;
  u->xb = c->xfer_bias;
  u->fb = NULL;
  if (format == S31_PACKED_RGBA) {
    /* compiled into a display list (s31_unpack_copy): no pixel store */
    u->format = GL_RGBA; u->type = GL_UNSIGNED_BYTE;
    u->elem = 1; u->group = 4; u->swap = 0;
    u->pitch = 4 * width;
    u->base = pixels;
    u->width = width; u->height = height;
    return 0;
  }
  if (comps == 0) return GL_INVALID_ENUM;
  switch (type) {
  case GL_UNSIGNED_BYTE: case GL_BYTE: elem = 1; break;
  case GL_UNSIGNED_SHORT: case GL_SHORT: elem = 2; break;
  case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: elem = 4; break;
  case GL_UNSIGNED_SHORT_5_6_5:
    if (format != GL_RGB) return GL_INVALID_OPERATION;
    elem = 2; comps = 1; break;
  case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1:
    if (format != GL_RGBA && format != GL_BGRA) return GL_INVALID_OPERATION;
    elem = 2; comps = 1; break;
  case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    if (format != GL_RGBA && format != GL_BGRA) return GL_INVALID_OPERATION;
    elem = 4; comps = 1; break;
  default:
    return GL_INVALID_ENUM;
  }

  u->format = format;
  u->type = type;
  u->elem = elem;
  u->group = elem * comps;               /* bytes per pixel */
  u->swap = c->unpack_swap && elem > 1;
  row_len = c->unpack_row_length > 0 ? c->unpack_row_length : width;
  align = c->unpack_alignment;
  if (elem >= align)
    u->pitch = u->group * row_len;
  else
    u->pitch = ((u->group * row_len + align - 1) / align) * align;
  u->base = (const unsigned char *)pixels +
            c->unpack_skip_rows * u->pitch + c->unpack_skip_pixels * u->group;
  u->width = width;
  u->height = height;
  return 0;
}

static unsigned int rd16(const unsigned char *p, int swap)
{
  unsigned int v = p[0] | (p[1] << 8);
  return swap ? ((v >> 8) | ((v & 0xff) << 8)) : v;
}

static unsigned int rd32(const unsigned char *p, int swap)
{
  unsigned int v = p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
  if (swap)
    v = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
  return v;
}

/* one component as 0..255 */
static int comp8(const S31Unpack *u, const unsigned char *p)
{
  float f;
  switch (u->type) {
  case GL_UNSIGNED_BYTE: return p[0];
  case GL_BYTE: { int v = (signed char)p[0]; return v <= 0 ? 0 : (v * 255 + 63) / 127; }
  case GL_UNSIGNED_SHORT: return rd16(p, u->swap) >> 8;
  case GL_SHORT: { int v = (short)rd16(p, u->swap); return v <= 0 ? 0 : v >> 7; }
  case GL_UNSIGNED_INT: return rd32(p, u->swap) >> 24;
  case GL_INT: { int v = (int)rd32(p, u->swap); return v <= 0 ? 0 : v >> 23; }
  case GL_FLOAT: {
    union { unsigned int i; float f; } x;
    x.i = rd32(p, u->swap);
    f = x.f;
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 255;
    return (int)(f * 255.0f + 0.5f);
  }
  default: return 0;
  }
}

/* expand an n-bit field to 8 bits */
static inline int x8(unsigned int v, int bits)
{
  switch (bits) {
  case 1: return v ? 255 : 0;
  case 4: return v * 17;
  case 5: return (v << 3) | (v >> 2);
  case 6: return (v << 2) | (v >> 4);
  default: return v;
  }
}

/* pixel (x, y) of the image as r g b a (0..255) */
void s31_unpack_pixel(const S31Unpack *u, int x, int y, unsigned char *out)
{
  const unsigned char *p = u->base + y * u->pitch + x * u->group;
  int e = u->elem;
  int r = 0, g = 0, b = 0, a = 255;
  unsigned int v;

  switch (u->type) {
  case GL_UNSIGNED_SHORT_5_6_5:
    v = rd16(p, u->swap);
    r = x8(v >> 11, 5); g = x8((v >> 5) & 63, 6); b = x8(v & 31, 5);
    break;
  case GL_UNSIGNED_SHORT_4_4_4_4:
    v = rd16(p, u->swap);
    r = x8(v >> 12, 4); g = x8((v >> 8) & 15, 4); b = x8((v >> 4) & 15, 4);
    a = x8(v & 15, 4);
    if (u->format == GL_BGRA) { int t = r; r = b; b = t; }
    break;
  case GL_UNSIGNED_SHORT_5_5_5_1:
    v = rd16(p, u->swap);
    r = x8(v >> 11, 5); g = x8((v >> 6) & 31, 5); b = x8((v >> 1) & 31, 5);
    a = x8(v & 1, 1);
    if (u->format == GL_BGRA) { int t = r; r = b; b = t; }
    break;
  case GL_UNSIGNED_INT_8_8_8_8:
    v = rd32(p, u->swap);
    r = v >> 24; g = (v >> 16) & 255; b = (v >> 8) & 255; a = v & 255;
    if (u->format == GL_BGRA) { int t = r; r = b; b = t; }
    break;
  case GL_UNSIGNED_INT_8_8_8_8_REV:
    v = rd32(p, u->swap);
    r = v & 255; g = (v >> 8) & 255; b = (v >> 16) & 255; a = v >> 24;
    if (u->format == GL_BGRA) { int t = r; r = b; b = t; }
    break;
  default:
    switch (u->format) {
    case GL_RED: r = comp8(u, p); break;
    case GL_GREEN: g = comp8(u, p); break;
    case GL_BLUE: b = comp8(u, p); break;
    case GL_ALPHA: r = g = b = 0; a = comp8(u, p); break;
    case GL_LUMINANCE: r = g = b = comp8(u, p); break;
    case GL_LUMINANCE_ALPHA: r = g = b = comp8(u, p); a = comp8(u, p + e); break;
    case GL_RGB: r = comp8(u, p); g = comp8(u, p + e); b = comp8(u, p + 2 * e); break;
    case GL_BGR: b = comp8(u, p); g = comp8(u, p + e); r = comp8(u, p + 2 * e); break;
    case GL_RGBA:
      r = comp8(u, p); g = comp8(u, p + e); b = comp8(u, p + 2 * e);
      a = comp8(u, p + 3 * e);
      break;
    case GL_BGRA:
      b = comp8(u, p); g = comp8(u, p + e); r = comp8(u, p + 2 * e);
      a = comp8(u, p + 3 * e);
      break;
    }
  }
  out[0] = r; out[1] = g; out[2] = b; out[3] = a;
}

/* 5/6-bit to 8-bit, rounded (v * 255 / 31, v * 255 / 63) */
const unsigned char s31_c5to8[32] = {
  0, 8, 16, 25, 33, 41, 49, 58, 66, 74, 82, 90, 99, 107, 115, 123,
  132, 140, 148, 156, 165, 173, 181, 189, 197, 206, 214, 222, 230, 239, 247, 255 };
const unsigned char s31_c6to8[64] = {
  0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 45, 49, 53, 57, 61,
  65, 69, 73, 77, 81, 85, 89, 93, 97, 101, 105, 109, 113, 117, 121, 125,
  130, 134, 138, 142, 146, 150, 154, 158, 162, 166, 170, 174, 178, 182, 186, 190,
  194, 198, 202, 206, 210, 215, 219, 223, 227, 231, 235, 239, 243, 247, 251, 255 };

void s31_unpack_fb(GLContext *c, S31Unpack *u, const unsigned short *buf,
                   int bw, int bh, int linesize, int x, int y, int w, int h)
{
  memset(u, 0, sizeof(*u));
  u->format = S31_FB_565;
  u->type = GL_UNSIGNED_SHORT_5_6_5;
  u->group = 2; u->elem = 2;
  u->width = w; u->height = h;
  u->fb = buf; u->fbw = bw; u->fbh = bh; u->fbls = linesize;
  u->fbx = x; u->fby = y;
  u->xs = c->xfer_active ? c->xfer_scale : NULL;
  u->xb = c->xfer_bias;
}

static void unpack_fb_row(const S31Unpack *u, int x, int y, int n, unsigned char *out)
{
  int wy = u->fby + y, wx = u->fbx + x, i;
  const unsigned short *row;
  if (wy < 0 || wy >= u->fbh) {
    for (i = 0; i < n; i++, out += 4) { out[0] = out[1] = out[2] = 0; out[3] = 255; }
    return;
  }
  row = (const unsigned short *)((const char *)u->fb + (u->fbh - 1 - wy) * u->fbls);
  for (i = 0; i < n; i++, out += 4, wx++) {
    unsigned int v = wx >= 0 && wx < u->fbw ? row[wx] : 0;
    out[0] = s31_c5to8[v >> 11]; out[1] = s31_c6to8[(v >> 5) & 63];
    out[2] = s31_c5to8[v & 31]; out[3] = 255;
  }
}

/* glPixelTransfer scale and bias on RGBA8888 (plan F7) */
static void xfer_row(const S31Unpack *u, unsigned char *out, int n)
{
  int i, k;
  for (i = 0; i < n; i++, out += 4)
    for (k = 0; k < 4; k++) {
      float v = (float)out[k] * (1.0f / 255.0f) * u->xs[k] + u->xb[k];
      out[k] = (unsigned char)(v <= 0.0f ? 0 : (v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f)));
    }
}

static void unpack_raw(const S31Unpack *u, int x, int y, int n, unsigned char *out);

/* n pixels of row y as RGBA8888, the pixel transfer applied */
void s31_unpack_row(const S31Unpack *u, int x, int y, int n, unsigned char *out)
{
  if (u->fb) unpack_fb_row(u, x, y, n, out);
  else unpack_raw(u, x, y, n, out);
  if (u->xs) xfer_row(u, out, n);
}

/* UNSIGNED_BYTE (almost every upload, TyrQuake's per-frame lightmap
   glTexSubImage2D among them) walks the row directly, anything else goes
   through s31_unpack_pixel */
static void unpack_raw(const S31Unpack *u, int x, int y, int n, unsigned char *out)
{
  const unsigned char *p = u->base + y * u->pitch + x * u->group;
  int i;

  if (u->type == GL_UNSIGNED_BYTE) {
    switch (u->format) {
    case GL_RGBA:
      memcpy(out, p, (size_t)n * 4);
      return;
    case GL_RGB:
      for (i = 0; i < n; i++, p += 3, out += 4) {
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
      }
      return;
    case GL_BGRA:
      for (i = 0; i < n; i++, p += 4, out += 4) {
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
      }
      return;
    case GL_LUMINANCE:
      for (i = 0; i < n; i++, p++, out += 4) {
        out[0] = out[1] = out[2] = p[0]; out[3] = 255;
      }
      return;
    case GL_LUMINANCE_ALPHA:
      for (i = 0; i < n; i++, p += 2, out += 4) {
        out[0] = out[1] = out[2] = p[0]; out[3] = p[1];
      }
      return;
    case GL_ALPHA:
      for (i = 0; i < n; i++, p++, out += 4) {
        out[0] = out[1] = out[2] = 0; out[3] = p[0];
      }
      return;
    default:
      break;
    }
  }
  for (i = 0; i < n; i++, out += 4)
    s31_unpack_pixel(u, x + i, y, out);
}

void *s31_unpack_copy(GLContext *c, int width, int height, int format,
                      int type, const void *pixels)
{
  S31Unpack u;
  unsigned char *buf;
  int y;

  if (pixels == NULL || width <= 0 || height <= 0 ||
      width > 4096 || height > 4096)
    return NULL;
  if (s31_unpack_setup(c, &u, width, height, format, type, pixels))
    return NULL;
  u.xs = NULL;          /* the transfer applies when the list executes */
  buf = gl_malloc(S31_BLOCK_HDR + width * height * 4);
  if (buf == NULL) return NULL;
  for (y = 0; y < height; y++)
    s31_unpack_row(&u, 0, y, width, buf + S31_BLOCK_HDR + y * width * 4);
  return buf;
}

/* ------------------------------------------------------------ packing */

static int pack_components(int format)
{
  switch (format) {
  case GL_DEPTH_COMPONENT:
    return 1;
  default:
    return format_components(format);
  }
}

int s31_pack_setup(GLContext *c, S31Pack *k, int width, int height,
                   int format, int type, void *pixels)
{
  int comps = pack_components(format), elem, row_len, align;

  (void)height;
  if (comps == 0) return GL_INVALID_ENUM;
  switch (type) {
  case GL_UNSIGNED_BYTE: case GL_BYTE: elem = 1; break;
  case GL_UNSIGNED_SHORT: case GL_SHORT: elem = 2; break;
  case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: elem = 4; break;
  case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV:
    if (format != GL_RGB) return GL_INVALID_OPERATION;
    elem = 1; comps = 1; break;
  case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
    if (format != GL_RGB) return GL_INVALID_OPERATION;
    elem = 2; comps = 1; break;
  case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
  case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
    if (format != GL_RGBA && format != GL_BGRA) return GL_INVALID_OPERATION;
    elem = 2; comps = 1; break;
  case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
  case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
    if (format != GL_RGBA && format != GL_BGRA) return GL_INVALID_OPERATION;
    elem = 4; comps = 1; break;
  default:
    return GL_INVALID_ENUM;
  }
  if (format == GL_DEPTH_COMPONENT && comps != 1) return GL_INVALID_OPERATION;
  k->format = format; k->type = type; k->elem = elem;
  k->comps = format_components(format) ? format_components(format) : 1;
  k->group = elem * comps;
  k->swap = c->pack_swap && elem > 1;
  row_len = c->pack_row_length > 0 ? c->pack_row_length : width;
  align = c->pack_alignment;
  if (elem >= align)
    k->pitch = k->group * row_len;
  else
    k->pitch = ((k->group * row_len + align - 1) / align) * align;
  k->base = (unsigned char *)pixels + c->pack_skip_rows * k->pitch +
            c->pack_skip_pixels * k->group;
  return 0;
}

static void wr16(unsigned char *p, unsigned int v, int swap)
{
  if (swap) v = ((v >> 8) & 0xff) | ((v & 0xff) << 8);
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

static void wr32(unsigned char *p, unsigned int v, int swap)
{
  if (swap)
    v = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
  p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* [0,1] to an n-bit unsigned field, rounded (GL 1.3 table 2.8) */
static unsigned int unorm(float v, int bits)
{
  float m = (float)((1u << bits) - 1);
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return (1u << bits) - 1;
  return (unsigned int)(v * m + 0.5f);
}

static void put_comp(const S31Pack *k, unsigned char *p, float v)
{
  union { float f; unsigned int i; } x;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  switch (k->type) {
  case GL_UNSIGNED_BYTE: p[0] = (unsigned char)(v * 255.0f + 0.5f); break;
  case GL_BYTE: p[0] = (unsigned char)(signed char)(int)(v * 127.0f + 0.5f); break;
  case GL_UNSIGNED_SHORT: wr16(p, (unsigned int)(v * 65535.0f + 0.5f), k->swap); break;
  case GL_SHORT: wr16(p, (unsigned int)(int)(v * 32767.0f + 0.5f), k->swap); break;
  case GL_UNSIGNED_INT:
    /* v * (2^32 - 1): float has 24 bits, so 1.0 is caught exactly */
    wr32(p, v >= 1.0f ? 0xffffffffu : (unsigned int)(v * 4294967296.0f), k->swap);
    break;
  case GL_INT:
    wr32(p, v >= 1.0f ? 0x7fffffffu : (unsigned int)(v * 2147483648.0f), k->swap);
    break;
  default:                      /* GL_FLOAT */
    x.f = v;
    wr32(p, x.i, k->swap);
    break;
  }
}

/* v: n pixels of float RGBA (or n depths for GL_DEPTH_COMPONENT) */
void s31_pack_span(const S31Pack *k, int x, int y, int n, const float *v)
{
  unsigned char *p = k->base + y * k->pitch + x * k->group;
  int i, e = k->elem;

  if (k->format == GL_DEPTH_COMPONENT) {
    for (i = 0; i < n; i++, p += k->group) put_comp(k, p, v[i]);
    return;
  }
  for (i = 0; i < n; i++, v += 4, p += k->group) {
    float r = v[0], g = v[1], b = v[2], a = v[3], l;
    unsigned int w;
    switch (k->type) {
    case GL_UNSIGNED_BYTE_3_3_2:
      p[0] = (unsigned char)((unorm(r, 3) << 5) | (unorm(g, 3) << 2) | unorm(b, 2));
      continue;
    case GL_UNSIGNED_BYTE_2_3_3_REV:
      p[0] = (unsigned char)((unorm(b, 2) << 6) | (unorm(g, 3) << 3) | unorm(r, 3));
      continue;
    case GL_UNSIGNED_SHORT_5_6_5:
      wr16(p, (unorm(r, 5) << 11) | (unorm(g, 6) << 5) | unorm(b, 5), k->swap);
      continue;
    case GL_UNSIGNED_SHORT_5_6_5_REV:
      wr16(p, (unorm(b, 5) << 11) | (unorm(g, 6) << 5) | unorm(r, 5), k->swap);
      continue;
    default:
      break;
    }
    if (k->format == GL_BGRA) { float t = r; r = b; b = t; }
    switch (k->type) {
    case GL_UNSIGNED_SHORT_4_4_4_4:
      wr16(p, (unorm(r, 4) << 12) | (unorm(g, 4) << 8) | (unorm(b, 4) << 4) | unorm(a, 4), k->swap);
      continue;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:
      wr16(p, (unorm(a, 4) << 12) | (unorm(b, 4) << 8) | (unorm(g, 4) << 4) | unorm(r, 4), k->swap);
      continue;
    case GL_UNSIGNED_SHORT_5_5_5_1:
      wr16(p, (unorm(r, 5) << 11) | (unorm(g, 5) << 6) | (unorm(b, 5) << 1) | unorm(a, 1), k->swap);
      continue;
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
      wr16(p, (unorm(a, 1) << 15) | (unorm(b, 5) << 10) | (unorm(g, 5) << 5) | unorm(r, 5), k->swap);
      continue;
    case GL_UNSIGNED_INT_8_8_8_8:
      w = (unorm(r, 8) << 24) | (unorm(g, 8) << 16) | (unorm(b, 8) << 8) | unorm(a, 8);
      wr32(p, w, k->swap);
      continue;
    case GL_UNSIGNED_INT_8_8_8_8_REV:
      w = (unorm(a, 8) << 24) | (unorm(b, 8) << 16) | (unorm(g, 8) << 8) | unorm(r, 8);
      wr32(p, w, k->swap);
      continue;
    case GL_UNSIGNED_INT_10_10_10_2:
      w = (unorm(r, 10) << 22) | (unorm(g, 10) << 12) | (unorm(b, 10) << 2) | unorm(a, 2);
      wr32(p, w, k->swap);
      continue;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
      w = (unorm(a, 2) << 30) | (unorm(b, 10) << 20) | (unorm(g, 10) << 10) | unorm(r, 10);
      wr32(p, w, k->swap);
      continue;
    default:
      break;
    }
    if (k->format == GL_BGRA) { float t = r; r = b; b = t; }
    /* GL 1.3 4.3.2: L = R + G + B, clamped */
    l = r + g + b;
    switch (k->format) {
    case GL_RED: put_comp(k, p, r); break;
    case GL_GREEN: put_comp(k, p, g); break;
    case GL_BLUE: put_comp(k, p, b); break;
    case GL_ALPHA: put_comp(k, p, a); break;
    case GL_LUMINANCE: put_comp(k, p, l); break;
    case GL_LUMINANCE_ALPHA: put_comp(k, p, l); put_comp(k, p + e, a); break;
    case GL_RGB:
      put_comp(k, p, r); put_comp(k, p + e, g); put_comp(k, p + 2 * e, b); break;
    case GL_BGR:
      put_comp(k, p, b); put_comp(k, p + e, g); put_comp(k, p + 2 * e, r); break;
    case GL_RGBA:
      put_comp(k, p, r); put_comp(k, p + e, g); put_comp(k, p + 2 * e, b);
      put_comp(k, p + 3 * e, a); break;
    case GL_BGRA:
      put_comp(k, p, b); put_comp(k, p + e, g); put_comp(k, p + 2 * e, r);
      put_comp(k, p + 3 * e, a); break;
    }
  }
}

/* ------------------------------------------------------------ bitmaps */

/* GL 1.3 3.6.4 for GL_BITMAP: rows of ceil(l / 8) bytes rounded up to
   GL_UNPACK_ALIGNMENT, SKIP_PIXELS in bits, LSB_FIRST; SWAP_BYTES has no
   effect on bitmaps */
void s31_bits_setup(GLContext *c, S31Bits *b, int width, const void *bits)
{
  int l = c->unpack_row_length > 0 ? c->unpack_row_length : width;
  int a = c->unpack_alignment;
  int bytes = (l + 7) / 8;
  b->pitch = ((bytes + a - 1) / a) * a;
  b->skip = c->unpack_skip_pixels;
  b->lsb = c->unpack_lsb;
  b->base = (const unsigned char *)bits + c->unpack_skip_rows * b->pitch;
}

void *s31_bits_copy(GLContext *c, int width, int height, const void *bits)
{
  S31Bits b;
  unsigned char *buf, *d;
  int x, y, pitch = (width + 7) / 8;

  if (bits == NULL || width <= 0 || height <= 0 || width > 4096 || height > 4096)
    return NULL;
  s31_bits_setup(c, &b, width, bits);
  buf = gl_malloc(S31_BLOCK_HDR + pitch * height);
  if (buf == NULL) return NULL;
  d = buf + S31_BLOCK_HDR;
  memset(d, 0, pitch * height);
  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      if (s31_bit(&b, x, y)) d[y * pitch + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
  return buf;
}
