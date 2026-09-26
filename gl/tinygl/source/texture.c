/*
 * Texture Manager
 *
 * s31 changes:
 *  - (plan F3) every texture is stored at its own power-of-two size, up
 *    to GL_MAX_TEXTURE_SIZE 256: RGB565, plus an A8 plane only when the
 *    internal format has alpha (see tex_class; README.s31 has the
 *    RGB565+A8 versus ARGB4444 measurement). Non-power-of-two and larger
 *    sizes are GL_INVALID_VALUE, as GL 1.1 says (with one "libGL:"
 *    line). glTexImage2D/glTexSubImage2D accept every GL 1.3 format/type
 *    s31_pixels.c unpacks and honour every GL_UNPACK_* parameter; NULL
 *    pixels give a black image; levels > 0 are validated and recorded
 *    (completeness, glGetTexLevelParameter) but not stored - the MIPMAP
 *    filters sample level 0 (superseded by phase 4 F-LIN, below); proxy
 *    targets answer as a real upload would.
 *    TinyGL exit(1)ed for anything but RGB/UNSIGNED_BYTE, level 0.
 *  - glGenTextures reserves the names it returns (two calls used to return
 *    the same name); deleting texture 0 is ignored; a texture without an
 *    image draws untextured instead of dereferencing NULL (clip.c).
 *  - glTexEnv / glTexParameter / glPixelStore record every GL 1.3 value and
 *    raise GL errors instead of exiting.
 *  - (phase 4 F-LIN) levels > 0 are stored as the application uploads
 *    them (glTexImage, glTexSubImage, glCopyTex*), each its own block in
 *    GLTexture.mip, converted to that level's stored class; a texture that
 *    never gets a level > 0 pays one pointer. S31GL_MIPMAPS=0 stores none
 *    (the mipmap filters then sample level 0, as before). GL_LINEAR and
 *    the mipmap filters are drawn by s31_tfilter.c.
 */

#include "zgl.h"
#include "s31_pixels.h"

#define TEX_SIZE 256            /* GL_MAX_TEXTURE_SIZE (get.c) */

/* s31 (phase 4): the stored levels > 0 */
void gl_tex_free_mip(GLTexture *t)
{
  int l;
  if (t->mip == NULL) return;
  for (l = 1; l < MAX_TEXTURE_LEVELS; l++) gl_free(t->mip->l[l].pix);
  gl_free(t->mip);
  t->mip = NULL;
}

static GLTexture *find_texture(GLContext *c,int h)
{
  GLTexture *t;

  t=c->shared_state.texture_hash_table[h % TEXTURE_HASH_TABLE_SIZE];
  while (t!=NULL) {
    if (t->handle == h) return t;
    t=t->next;
  }
  return NULL;
}

static void free_texture(GLContext *c,int h)
{
  GLTexture *t,**ht;
  GLImage *im;
  int i;

  t=find_texture(c,h);
  if (t == NULL) return;
  if (t->prev==NULL) {
    ht=&c->shared_state.texture_hash_table
      [t->handle % TEXTURE_HASH_TABLE_SIZE];
    *ht=t->next;
  } else {
    t->prev->next=t->next;
  }
  if (t->next!=NULL) t->next->prev=t->prev;

  /* s31: the A8 plane shares level 0's block */
  for(i=0;i<TGL_STORED_LEVELS;i++) {
    im=&t->images[i];
    if (im->pixmap != NULL) gl_free(im->pixmap);
  }
  gl_tex_free_mip(t);

  gl_free(t);
}

GLTexture *alloc_texture(GLContext *c,int h)
{
  GLTexture *t,**ht;

  t=gl_zalloc(sizeof(GLTexture));
  if (t == NULL) return NULL;

  ht=&c->shared_state.texture_hash_table[h % TEXTURE_HASH_TABLE_SIZE];

  t->next=*ht;
  t->prev=NULL;
  if (t->next != NULL) t->next->prev=t;
  *ht=t;

  t->handle=h;
  /* GL 1.3 initial texture object state */
  t->min_filter=GL_NEAREST_MIPMAP_LINEAR;
  t->mag_filter=GL_LINEAR;
  t->wrap_s=GL_REPEAT;
  t->wrap_t=GL_REPEAT;
  t->priority=1.0f;
  t->internal_format=1;
  t->max_level=1000;              /* GL 1.2 defaults */
  t->min_lod=-1000.0f; t->max_lod=1000.0f;

  return t;
}


/* s31 (plan F7): a texture object outside the name table - the default
   GL_TEXTURE_1D object of a context (name 0, s31_state.c) */
GLTexture *alloc_texture_detached(void)
{
  GLTexture *t = gl_zalloc(sizeof(GLTexture));
  if (t == NULL) return NULL;
  t->min_filter=GL_NEAREST_MIPMAP_LINEAR;
  t->mag_filter=GL_LINEAR;
  t->wrap_s=GL_REPEAT;
  t->wrap_t=GL_REPEAT;
  t->priority=1.0f;
  t->internal_format=1;
  t->max_level=1000;              /* GL 1.2 defaults */
  t->min_lod=-1000.0f; t->max_lod=1000.0f;
  return t;
}

void free_texture_detached(GLTexture *t)
{
  if (t == NULL) return;
  gl_free(t->images[0].pixmap);
  gl_tex_free_mip(t);
  gl_free(t);
}

/* the object bound to a target (NULL: not a target this library has) */
GLTexture *gl_tex_target(GLContext *c, int target)
{
  if (target == GL_TEXTURE_2D) return c->current_texture;
  if (target == GL_TEXTURE_1D) return c->current_texture_1d;
  return NULL;
}

void glInitTextures(GLContext *c)
{
  /* textures */

  c->texture_2d_enabled=0;
  c->current_texture=find_texture(c,0);
}

void glGenTextures(int n, unsigned int *textures)
{
  GLContext *c=gl_get_context();
  int max,i;
  GLTexture *t;

  if (n < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  max=0;
  for(i=0;i<TEXTURE_HASH_TABLE_SIZE;i++) {
    t=c->shared_state.texture_hash_table[i];
    while (t!=NULL) {
      if (t->handle>max) max=t->handle;
      t=t->next;
    }

  }
  for(i=0;i<n;i++) {
    /* s31: reserve the name, so the next glGenTextures does not return it */
    if (alloc_texture(c,max+i+1) == NULL) {
      gl_set_error(c, GL_OUT_OF_MEMORY);
      textures[i]=0;
      continue;
    }
    textures[i]=max+i+1;
  }
}


void glDeleteTextures(int n, const unsigned int *textures)
{
  GLContext *c=gl_get_context();
  int i;
  GLTexture *t;

  if (n < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  for(i=0;i<n;i++) {
    if (textures[i] == 0) continue;
    t=find_texture(c,textures[i]);
    if (t!=NULL) {
      if (t==c->current_texture) {
	c->current_texture=find_texture(c,0);
      }
      if (t==c->current_texture_1d)
	c->current_texture_1d=c->tex1d_default;   /* s31 */
      c->raster_dirty=1;   /* s31: raster.c points at its pixels */
      free_texture(c,textures[i]);
    }
  }
}

int tgl_is_texture(unsigned int name)
{
  GLContext *c=gl_get_context();
  GLTexture *t;
  if (name == 0) return 0;
  t=find_texture(c,name);
  return t != NULL;
}


void glopBindTexture(GLContext *c,GLParam *p)
{
  int target=p[1].i;
  int texture=p[2].i;
  GLTexture *t;

  if (target != GL_TEXTURE_2D && target != GL_TEXTURE_1D) {
    if (target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP)
      gl_warn_once("glBindTexture(GL_TEXTURE_3D/CUBE_MAP)");
    else
      gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (target == GL_TEXTURE_1D && texture == 0) {
    /* s31 (plan F7): the 1D default object is the context's own */
    c->current_texture_1d = c->tex1d_default;
    c->raster_dirty = 1;
    return;
  }

  t=find_texture(c,texture);
  if (t==NULL) {
    t=alloc_texture(c,texture);
    if (t == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
  }
  if (target == GL_TEXTURE_1D) c->current_texture_1d=t;
  else c->current_texture=t;
  c->raster_dirty=1;
}

static int valid_internal_format(int f)
{
  switch (f) {
  case 1: case 2: case 3: case 4:
  case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: case GL_ALPHA12: case GL_ALPHA16:
  case GL_LUMINANCE: case GL_LUMINANCE4: case GL_LUMINANCE8:
  case GL_LUMINANCE12: case GL_LUMINANCE16:
  case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE6_ALPHA2:
  case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE12_ALPHA4:
  case GL_LUMINANCE12_ALPHA12: case GL_LUMINANCE16_ALPHA16:
  case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8:
  case GL_INTENSITY12: case GL_INTENSITY16:
  case GL_R3_G3_B2: case GL_RGB: case GL_RGB4: case GL_RGB5: case GL_RGB8:
  case GL_RGB10: case GL_RGB12: case GL_RGB16:
  case GL_RGBA: case GL_RGBA2: case GL_RGBA4: case GL_RGB5_A1: case GL_RGBA8:
  case GL_RGB10_A2: case GL_RGBA12: case GL_RGBA16:
    return 1;
  default:
    return 0;
  }
}

/* s31 (plan F3): base format class of an internal format */
static int tex_class(int f, int *lum)
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

/* log2 of a power of two, else -1 */
static int ilog2(int v)
{
  int l = 0;
  if (v <= 0 || (v & (v - 1))) return -1;
  while ((1 << l) < v) l++;
  return l;
}

/* internal formats 1-4 are the base formats by another name */
static int fmt_norm(int f)
{
  static const unsigned short base[5] = { 0, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_RGB, GL_RGBA };
  return f >= 1 && f <= 4 ? base[f] : f;
}

/* GL 1.3 3.8.10: a texture is complete when level 0 exists and, for a
   mipmap minification filter, every level down to 1x1 (GL 1.2: or
   GL_TEXTURE_MAX_LEVEL) was specified with the halved size and level 0's
   internal format. Incomplete = texturing off (clip.c). */
int gl_texture_complete(const GLTexture *t)
{
  int l, w, h, b = 2 * t->border, n, f0;
  if (t->images[0].pixmap == NULL || t->lw[0] == 0) return 0;
  if (t->min_filter == GL_NEAREST || t->min_filter == GL_LINEAR) return 1;
  w = t->lw[0] - b; h = t->lh[0] - b;
  n = t->ws > t->hs ? t->ws : t->hs;
  if (n > t->max_level) n = t->max_level;      /* GL 1.2 (review 4 R4) */
  f0 = fmt_norm(t->lfmt[0]);
  for (l = 1; l <= n; l++) {
    w = w > 1 ? w >> 1 : 1;
    h = h > 1 ? h >> 1 : 1;
    if (l >= MAX_TEXTURE_LEVELS || t->lw[l] - b != w || t->lh[l] - b != h)
      return 0;
    /* GL 1.3 3.8.10: the same internal format at every level (review 4
       R4: a level of another format was sampled, or level 0 drawn) */
    if (fmt_norm(t->lfmt[l]) != f0) return 0;
  }
  return 1;
}

/* Store rows [y0, y0+h) x [x0, x0+w) of level 0 from an unpacked source
   whose pixel (sx, sy) lands at (x0, y0). Conversion per class:
   RGB565 truncated as TinyGL did; luminance and intensity take R (GL 1.3
   table 3.15); alpha classes also fill the A8 plane.
   One loop per class, the class chosen once per row (review P7: the class
   and luminance tests ran per texel, and this file is -Os, so GCC did not
   unswitch them; TyrQuake re-uploads lightmaps every frame). Built -O2. */
#define T565(r, g, b) ((unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* (phase 4: any level - pix/al are the level's planes, ws its log2 width,
   ifmt its internal format) */
__attribute__((optimize("O2")))
static void tex_store_img(unsigned short *pix, unsigned char *al, int ws, int ifmt,
                          const S31Unpack *u, int sx, int sy,
                          int x0, int y0, int w, int h)
{
  unsigned char row[4 * 256];
  int TW = 1 << ws, x, y, lum;
  int cls = tex_class(ifmt, &lum);

  /* the common uploads straight from the client's bytes, without the
     RGBA8888 row in between: UNSIGNED_BYTE RGB / RGBA / LUMINANCE into
     the matching stored class, no pixel transfer, not the colour buffer */
  int direct = u->fb == NULL && u->xs == NULL && u->type == GL_UNSIGNED_BYTE &&
    ((u->format == GL_RGB && cls == TGL_TEXF_RGB && !lum) ||
     (u->format == GL_RGBA && cls == TGL_TEXF_RGBA && !lum) ||
     (u->format == GL_LUMINANCE && cls == TGL_TEXF_RGB && lum));

  for (y = 0; y < h; y++) {
    unsigned short *d = pix + (y0 + y) * TW + x0;
    unsigned char *da = al ? al + (y0 + y) * TW + x0 : NULL;
    const unsigned char *q = row;
    if (direct) {
      const unsigned char *p = u->base + (sy + y) * u->pitch + sx * u->group;
      switch (u->format) {
      case GL_RGB:
        for (x = 0; x < w; x++, p += 3) d[x] = T565(p[0], p[1], p[2]);
        break;
      case GL_RGBA:
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
      if (lum)
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

static void tex_store(GLTexture *t, const S31Unpack *u, int sx, int sy,
                      int x0, int y0, int w, int h)
{
  tex_store_img(t->images[0].pixmap, t->alpha, t->ws, t->internal_format,
                u, sx, sy, x0, y0, w, h);
}

/* s31 (phase 4 F-LIN): store level L > 0 (iw x ih, log2 ws x hs, internal
   format ifmt), from u when given. The block is reused when the level
   keeps its shape and class. Returns 0 when it could not be stored (no
   memory, or S31GL_MIPMAPS=0): the level is then only recorded, and the
   mipmap filters see an incomplete chain and sample level 0 */
static int tex_level_store(GLContext *c, GLTexture *t, int level, int iw, int ih,
                           int ws, int hs, int ifmt, int border, int vb,
                           const S31Unpack *u, int have_pixels)
{
  GLMipLevel *m;
  int lum, cls = tex_class(ifmt, &lum), has_alpha = cls != TGL_TEXF_RGB;
  int need = iw * ih * 2 + (has_alpha ? iw * ih : 0);

  if (!c->mip_store) return 0;
  if (t->mip == NULL) {
    t->mip = gl_zalloc(sizeof(GLMipChain));
    if (t->mip == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
  }
  m = &t->mip->l[level];
  if (iw == 0 || ih == 0) {
    gl_free(m->pix);
    m->pix = NULL; m->alpha = NULL;
    return 0;
  }
  if (m->pix == NULL || m->ws != ws || m->hs != hs || (m->alpha != NULL) != has_alpha) {
    gl_free(m->pix);
    m->pix = gl_malloc(need);
    m->alpha = NULL;
    if (m->pix == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
  }
  m->ws = (unsigned char)ws; m->hs = (unsigned char)hs;
  m->cls = (unsigned char)cls;
  m->alpha = has_alpha ? (unsigned char *)m->pix + iw * ih * 2 : NULL;
  if (!have_pixels) {
    memset(m->pix, 0, need);
    return 1;
  }
  tex_store_img(m->pix, m->alpha, ws, ifmt, u, border, vb, 0, 0, iw, ih);
  return 1;
}

/* s31: glTexImage2D and glTexImage1D (a W x 1 image, plan F7). src: the
   source already set up (glCopyTexImage: the colour buffer), or NULL for
   the op's own format/type/pixels */
int gl_tex_image_src(GLContext *c, GLParam *p, const S31Unpack *src)
{
  int target=p[1].i;
  int level=p[2].i;
  int components=p[3].i;
  int width=p[4].i;
  int height=p[5].i;
  int border=p[6].i;
  int format=p[7].i;
  int type=p[8].i;
  void *pixels=p[9].p;
  GLTexture *t;
  S31Unpack u;
  int e, ws, hs, iw, ih, lum, cls, need, has_alpha, TW, TH, maxl, is1d, proxy, vb;

  is1d = target == GL_TEXTURE_1D || target == GL_PROXY_TEXTURE_1D;
  proxy = target == GL_PROXY_TEXTURE_2D || target == GL_PROXY_TEXTURE_1D;
  if (!is1d && target != GL_TEXTURE_2D && target != GL_PROXY_TEXTURE_2D) {
    gl_set_error(c, GL_INVALID_ENUM);
    return 0;
  }
  /* GL 1.3 3.8.1: level <= log2(GL_MAX_TEXTURE_SIZE); sizes 2^n + 2*border */
  maxl = ilog2(TEX_SIZE);
  if (level < 0 || level > maxl || width < 0 || height < 0 ||
      (border != 0 && border != 1)) {
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  if (!valid_internal_format(components)) {
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  /* a 1D image is one row: the border is on its ends only */
  vb = is1d ? 0 : border;
  if (is1d) height = 1;
  iw = width - 2 * border;
  ih = height - 2 * vb;
  if (width == 0 || height == 0) iw = ih = 0;   /* the null texture */
  ws = ilog2(iw); hs = ilog2(ih);
  if ((iw != 0 && ws < 0) || (ih != 0 && hs < 0) || iw < 0 || ih < 0) {
    /* not a power of two: GL 1.x has no ARB_texture_non_power_of_two,
       and it is not advertised */
    gl_warn_once("non-power-of-two texture (GL_INVALID_VALUE, as GL 1.1 requires)");
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  /* (S31_PACKED_RGBA: compiled into a display list, api.c) */
  if (src) {
    u = *src;
  } else {
    e = s31_unpack_setup(c, &u, width, height, format, type, pixels);
    if (e) { gl_set_error(c, e); return 0; }
  }

  if (proxy) {
    /* the proxy answers what a real upload would accept: 0 when larger
       than level L allows, 2^(k - L) with 2^k = GL_MAX_TEXTURE_SIZE (GL 1.3
       3.8.1). GLU's gluBuild2DMipmaps probes level 1 with half its
       candidate size and halves until this says yes (closestFit,
       mipmap.c ~3433); review G3: level 1 at 256 was accepted, so GLU
       chose a 512 base that the real upload then refused, and returned
       success with no texture */
    c->proxy_level = level;
    if (iw > (TEX_SIZE >> level) || ih > (TEX_SIZE >> level)) {
      c->proxy_width = c->proxy_height = 0; c->proxy_format = 0;
      c->proxy_border = 0;
    } else {
      c->proxy_width = width; c->proxy_height = height;
      c->proxy_format = components;
      c->proxy_border = border;
    }
    return 0;
  }
  /* GL_MAX_TEXTURE_SIZE is 256, so level L is at most 256 >> L; larger is
     GL_INVALID_VALUE */
  if (iw > (TEX_SIZE >> level) || ih > (TEX_SIZE >> level)) {
    gl_warn_once("texture larger than GL_MAX_TEXTURE_SIZE 256 (GL_INVALID_VALUE)");
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }

  t = is1d ? c->current_texture_1d : c->current_texture;
  if (t == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return 0; }
  c->raster_dirty = 1;
  t->lw[level] = (unsigned short)width;
  t->lh[level] = (unsigned short)height;
  t->lfmt[level] = components;
  if (level > 0) {
    /* recorded for completeness and glGetTexLevelParameter, and (phase 4)
       stored for the mipmap filters */
    return tex_level_store(c, t, level, iw, ih, ws, hs, components, border, vb,
                           &u, pixels != NULL || src != NULL);
  }
  if (border) gl_warn_once("texture border (the border texels are dropped)");
  t->width = width;
  t->height = height;
  t->internal_format = components;
  t->border = border;

  if (iw == 0 || ih == 0) {
    /* the null texture: incomplete, texturing is off */
    gl_free(t->images[0].pixmap);
    t->images[0].pixmap = NULL;
    t->alpha = NULL;
    t->lw[0] = 0;
    return 0;
  }
  cls = tex_class(components, &lum);
  has_alpha = cls != TGL_TEXF_RGB;
  TW = iw; TH = ih;
  /* one block: RGB565 plane, then the A8 plane when the format has alpha
     (3 bytes a texel; opaque textures stay at 2). Reused when the shape
     is the same (TyrQuake re-uploads its lightmaps every frame) */
  need = TW * TH * 2 + (has_alpha ? TW * TH : 0);
  if (t->images[0].pixmap == NULL || t->ws != ws || t->hs != hs ||
      (t->alpha != NULL) != has_alpha) {
    gl_free(t->images[0].pixmap);
    t->images[0].pixmap = gl_malloc(need);
    if (t->images[0].pixmap == NULL) {
      gl_set_error(c, GL_OUT_OF_MEMORY);
      t->alpha = NULL;
      t->lw[0] = 0;
      t->width = t->height = 0;
      return 0;
    }
  }
  t->images[0].xsize = TW;
  t->images[0].ysize = TH;
  t->alpha = has_alpha ? (unsigned char *)t->images[0].pixmap + TW * TH * 2 : NULL;
  t->ws = ws; t->hs = hs;
  t->fmt = cls;
  /* fraction bits of the fixed-point s/t (clip.c): F + ws + hs <= 22 keeps
     9 bits of repeat headroom; 14 is TinyGL's for 256 wide */
  t->fbits = 22 - ws - hs < 14 ? 22 - ws - hs : 14;

  if (pixels == NULL && src == NULL) {
    /* contents undefined by GL; black (transparent) */
    memset(t->images[0].pixmap, 0, need);
    return 0;
  }
  /* row 0 of the source is t = 0; the border texels are skipped */
  tex_store(t, &u, border, vb, 0, 0, TW, TH);
  return 1;
}

void glopTexImage2D(GLContext *c,GLParam *p)
{
  gl_tex_image_src(c, p, NULL);
}

/* s31: glTexSubImage2D (GL 1.3 3.8.2), with every GL_UNPACK_* parameter
   (TyrQuake updates lightmaps through GL_UNPACK_ROW_LENGTH), and
   glTexSubImage1D / glCopyTexSubImage (plan F7) */
int gl_tex_subimage_src(GLContext *c, GLParam *p, const S31Unpack *src)
{
  int target=p[1].i;
  int level=p[2].i;
  int xoff=p[3].i;
  int yoff=p[4].i;
  int width=p[5].i;
  int height=p[6].i;
  int format=p[7].i;
  int type=p[8].i;
  void *pixels=p[9].p;
  GLTexture *t = gl_tex_target(c, target);
  S31Unpack u;
  int e, b, vb, x0, y0, sx, sy, w, h, TW, TH;

  if (t == NULL) {
    gl_set_error(c, GL_INVALID_ENUM);
    return 0;
  }
  if (level < 0 || level >= MAX_TEXTURE_LEVELS || width < 0 || height < 0) {
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  if (t->lw[level] == 0 && t->lh[level] == 0) {
    gl_set_error(c, GL_INVALID_OPERATION);   /* no such level */
    return 0;
  }
  b = t->border;
  vb = target == GL_TEXTURE_1D ? 0 : b;
  if (xoff < -b || yoff < -vb || xoff + width > t->lw[level] - b ||
      yoff + height > t->lh[level] - vb) {
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  if (src) {
    u = *src;
  } else {
    e = s31_unpack_setup(c, &u, width, height, format, type, pixels);
    if (e) { gl_set_error(c, e); return 0; }
  }
  if ((pixels == NULL && src == NULL) || width == 0 || height == 0)
    return 0;
  if (level > 0) {
    /* phase 4: into the stored level, if there is one */
    GLMipLevel *m = t->mip ? &t->mip->l[level] : NULL;
    if (m == NULL || m->pix == NULL) return 0;
    TW = 1 << m->ws; TH = 1 << m->hs;
    x0 = xoff; y0 = yoff; sx = 0; sy = 0; w = width; h = height;
    if (x0 < 0) { sx = -x0; w += x0; x0 = 0; }
    if (y0 < 0) { sy = -y0; h += y0; y0 = 0; }
    if (x0 + w > TW) w = TW - x0;
    if (y0 + h > TH) h = TH - y0;
    if (w <= 0 || h <= 0) return 0;
    c->raster_dirty = 1;
    tex_store_img(m->pix, m->alpha, m->ws, t->lfmt[level], &u, sx, sy, x0, y0, w, h);
    return 1;
  }
  if (t->images[0].pixmap == NULL)
    return 0;
  /* the part inside the stored (border-less) image */
  TW = 1 << t->ws; TH = 1 << t->hs;
  x0 = xoff; y0 = yoff; sx = 0; sy = 0; w = width; h = height;
  if (x0 < 0) { sx = -x0; w += x0; x0 = 0; }
  if (y0 < 0) { sy = -y0; h += y0; y0 = 0; }
  if (x0 + w > TW) w = TW - x0;
  if (y0 + h > TH) h = TH - y0;
  if (w <= 0 || h <= 0) return 0;
  c->raster_dirty = 1;
  tex_store(t, &u, sx, sy, x0, y0, w, h);
  return 1;
}

void glopTexSubImage2D(GLContext *c,GLParam *p)
{
  gl_tex_subimage_src(c, p, NULL);
}

int tgl_get_tex_level_parameter(int target, int level, int pname, int *iv)
{
  GLContext *c=gl_get_context();
  int w, h, f, cls, lum, rgb, al;
  GLTexture *t = c->current_texture;

  if (level < 0 || level >= MAX_TEXTURE_LEVELS) return -2;
  if (target == GL_PROXY_TEXTURE_2D || target == GL_PROXY_TEXTURE_1D) {
    /* the one proxy level last specified; any other level is empty */
    if (level == c->proxy_level) {
      w = c->proxy_width; h = c->proxy_height; f = c->proxy_format;
    } else {
      w = h = f = 0;
    }
  } else if (target == GL_TEXTURE_2D || target == GL_TEXTURE_1D) {
    t = gl_tex_target(c, target);
    /* every level as specified (levels > 0 are recorded, not stored) */
    w = t->lw[level]; h = t->lh[level];
    f = t->lfmt[level];
  } else {
    return -1;
  }
  if (w == 0 || h == 0) f = 0;
  cls = tex_class(f ? f : 1, &lum);
  /* what is stored: RGB565 (+ A8); luminance and intensity keep 5 bits */
  rgb = w && f && cls != TGL_TEXF_ALPHA && !lum;
  al = w && f && cls != TGL_TEXF_RGB;
  switch (pname) {
  case GL_TEXTURE_WIDTH: *iv = w; return 1;
  case GL_TEXTURE_HEIGHT: *iv = h; return 1;
  case GL_TEXTURE_BORDER:
    if (target == GL_PROXY_TEXTURE_2D || target == GL_PROXY_TEXTURE_1D)
      *iv = w ? c->proxy_border : 0;
    else
      *iv = w && level == 0 ? t->border : 0;
    return 1;
  case GL_TEXTURE_INTERNAL_FORMAT: *iv = f ? f : 1; return 1;
  case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_BLUE_SIZE: *iv = rgb ? 5 : 0; return 1;
  case GL_TEXTURE_GREEN_SIZE: *iv = rgb ? 6 : 0; return 1;
  case GL_TEXTURE_ALPHA_SIZE: *iv = al && cls != TGL_TEXF_INTENSITY ? 8 : 0; return 1;
  case GL_TEXTURE_LUMINANCE_SIZE: *iv = w && f && lum && cls != TGL_TEXF_INTENSITY ? 5 : 0; return 1;
  case GL_TEXTURE_INTENSITY_SIZE: *iv = w && f && cls == TGL_TEXF_INTENSITY ? 5 : 0; return 1;
  default: return -1;
  }
}


/* s31: records GL 1.3 texture environment state */
void glopTexEnv(GLContext *c,GLParam *p)
{
  int target=p[1].i;
  int pname=p[2].i;
  int param=p[3].i;

  if (target != GL_TEXTURE_ENV) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (pname == GL_TEXTURE_ENV_COLOR) {
    int i;
    for (i = 0; i < 4; i++) {
      float v = p[4 + i].f;
      c->texenv_color.v[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    c->raster_dirty = 1;
    return;
  }
  if (pname != GL_TEXTURE_ENV_MODE) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  switch (param) {
  case GL_DECAL:
  case GL_REPLACE:
  case GL_MODULATE:
  case GL_BLEND:
  case GL_ADD:
    break;              /* s31: all honoured (zpipe.c, plan F4) */
  case GL_COMBINE:
    gl_warn_once("glTexEnv(GL_COMBINE) (drawn as GL_MODULATE)");
    param = GL_MODULATE;
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  c->texenv_mode = param;
  c->raster_dirty = 1;
}

/* s31: records GL 1.3 texture parameters on the bound texture. p[3] is an
   int, p[4..7] a float vector (border colour, priority) */
void glopTexParameter(GLContext *c,GLParam *p)
{
  int target=p[1].i;
  int pname=p[2].i;
  int param=p[3].i;
  GLTexture *t=gl_tex_target(c,target);

  if (t == NULL) {
    if (target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP)
      return;
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  c->raster_dirty = 1;

  switch(pname) {
  case GL_TEXTURE_WRAP_S:
  case GL_TEXTURE_WRAP_T:
    if (param != GL_REPEAT && param != GL_CLAMP && param != GL_CLAMP_TO_EDGE) {
      gl_set_error(c, GL_INVALID_ENUM);
      return;
    }
    /* s31: honoured; with nearest sampling CLAMP and CLAMP_TO_EDGE pick
       the same texel (the border colour only enters a linear filter) */
    if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = param; else t->wrap_t = param;
    break;
  case GL_TEXTURE_MAG_FILTER:
    if (param != GL_NEAREST && param != GL_LINEAR) {
      gl_set_error(c, GL_INVALID_ENUM);
      return;
    }
    t->mag_filter = param;      /* phase 4: honoured (s31_tfilter.c) */
    break;
  case GL_TEXTURE_MIN_FILTER:
    switch (param) {
    case GL_NEAREST: case GL_LINEAR:
    case GL_NEAREST_MIPMAP_NEAREST: case GL_LINEAR_MIPMAP_NEAREST:
    case GL_NEAREST_MIPMAP_LINEAR: case GL_LINEAR_MIPMAP_LINEAR:
      break;
    default:
      gl_set_error(c, GL_INVALID_ENUM);
      return;
    }
    t->min_filter = param;      /* phase 4: honoured (s31_tfilter.c) */
    break;
  case GL_TEXTURE_PRIORITY:
    t->priority = p[4].f < 0.0f ? 0.0f : (p[4].f > 1.0f ? 1.0f : p[4].f);
    break;
  /* GL 1.2 (review 4 R4): recorded and used - they were accepted and
     ignored, while glGet raised INVALID_ENUM for them */
  case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_MAX_LEVEL:
    if (param < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
    if (param > 30000) param = 30000;
    if (pname == GL_TEXTURE_BASE_LEVEL) t->base_level = (short)param;
    else t->max_level = (short)param;
    break;
  case GL_TEXTURE_MIN_LOD: t->min_lod = p[4].f; break;
  case GL_TEXTURE_MAX_LOD: t->max_lod = p[4].f; break;
  case GL_TEXTURE_BORDER_COLOR:
  case GL_GENERATE_MIPMAP:
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
}

/* float-valued glTexParameter (priority, border colour, LODs) */
void tgl_tex_parameterf(int target, int pname, const float *v, int n)
{
  GLParam p[8];
  int i;
  p[0].op=OP_TexParameter;
  p[1].i=target;
  p[2].i=pname;
  p[3].i=(int)v[0];
  for (i=0;i<4;i++) p[4+i].f= i < n ? v[i] : 0.0f;
  gl_add_op(p);
}

int tgl_get_tex_parameter(int target, int pname, int *iv, float *fv, int *kind)
{
  GLContext *c=gl_get_context();
  GLTexture *t=gl_tex_target(c,target);
  *kind = TGL_GET_INT;
  if (t == NULL) return -1;
  switch (pname) {
  case GL_TEXTURE_MAG_FILTER: iv[0] = t->mag_filter; break;
  case GL_TEXTURE_MIN_FILTER: iv[0] = t->min_filter; break;
  case GL_TEXTURE_WRAP_S: iv[0] = t->wrap_s; break;
  case GL_TEXTURE_WRAP_T: iv[0] = t->wrap_t; break;
  case GL_TEXTURE_PRIORITY: fv[0] = t->priority; *kind = TGL_GET_FLOAT;
    iv[0] = (int)t->priority; return 1;
  case GL_TEXTURE_RESIDENT: iv[0] = 1; break;
  case GL_TEXTURE_BASE_LEVEL: iv[0] = t->base_level; break;
  case GL_TEXTURE_MAX_LEVEL: iv[0] = t->max_level; break;
  case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD:
    fv[0] = pname == GL_TEXTURE_MIN_LOD ? t->min_lod : t->max_lod;
    iv[0] = (int)fv[0]; *kind = TGL_GET_FLOAT;
    return 1;
  case GL_TEXTURE_BORDER_COLOR:
    fv[0] = fv[1] = fv[2] = fv[3] = 0.0f; iv[0] = iv[1] = iv[2] = iv[3] = 0;
    *kind = TGL_GET_COLOR; return 4;
  default: return -1;
  }
  fv[0] = (float)iv[0];
  return 1;
}

int tgl_get_tex_env(int target, int pname, int *iv, float *fv, int *kind)
{
  GLContext *c=gl_get_context();
  int i;
  if (target != GL_TEXTURE_ENV) return -1;
  if (pname == GL_TEXTURE_ENV_MODE) {
    iv[0] = c->texenv_mode; fv[0] = (float)c->texenv_mode;
    *kind = TGL_GET_INT;
    return 1;
  }
  if (pname == GL_TEXTURE_ENV_COLOR) {
    for (i = 0; i < 4; i++) { fv[i] = c->texenv_color.v[i]; iv[i] = 0; }
    *kind = TGL_GET_COLOR;
    return 4;
  }
  return -1;
}

void glopPixelStore(GLContext *c,GLParam *p)
{
  int pname=p[1].i;
  int param=p[2].i;

  switch (pname) {
  case GL_UNPACK_ALIGNMENT:
  case GL_PACK_ALIGNMENT:
    if (param != 1 && param != 2 && param != 4 && param != 8) {
      gl_set_error(c, GL_INVALID_VALUE);
      return;
    }
    break;
  case GL_UNPACK_SWAP_BYTES: case GL_UNPACK_LSB_FIRST:
  case GL_PACK_SWAP_BYTES: case GL_PACK_LSB_FIRST:
    param = param != 0;
    break;
  case GL_UNPACK_ROW_LENGTH: case GL_UNPACK_SKIP_ROWS: case GL_UNPACK_SKIP_PIXELS:
  case GL_UNPACK_IMAGE_HEIGHT: case GL_UNPACK_SKIP_IMAGES:
  case GL_PACK_ROW_LENGTH: case GL_PACK_SKIP_ROWS: case GL_PACK_SKIP_PIXELS:
  case GL_PACK_IMAGE_HEIGHT: case GL_PACK_SKIP_IMAGES:
    if (param < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  switch (pname) {
  case GL_UNPACK_ALIGNMENT: c->unpack_alignment = param; break;
  case GL_UNPACK_SWAP_BYTES: c->unpack_swap = param; break;
  case GL_UNPACK_LSB_FIRST: c->unpack_lsb = param; break;
  case GL_UNPACK_ROW_LENGTH: c->unpack_row_length = param; break;
  case GL_UNPACK_SKIP_ROWS: c->unpack_skip_rows = param; break;
  case GL_UNPACK_SKIP_PIXELS: c->unpack_skip_pixels = param; break;
  case GL_UNPACK_IMAGE_HEIGHT: c->unpack_image_height = param; break;
  case GL_UNPACK_SKIP_IMAGES: c->unpack_skip_images = param; break;
  case GL_PACK_ALIGNMENT: c->pack_alignment = param; break;
  case GL_PACK_SWAP_BYTES: c->pack_swap = param; break;
  case GL_PACK_LSB_FIRST: c->pack_lsb = param; break;
  case GL_PACK_ROW_LENGTH: c->pack_row_length = param; break;
  case GL_PACK_SKIP_ROWS: c->pack_skip_rows = param; break;
  case GL_PACK_SKIP_PIXELS: c->pack_skip_pixels = param; break;
  case GL_PACK_IMAGE_HEIGHT: c->pack_image_height = param; break;
  case GL_PACK_SKIP_IMAGES: c->pack_skip_images = param; break;
  }
}
