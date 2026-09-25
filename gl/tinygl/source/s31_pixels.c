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
