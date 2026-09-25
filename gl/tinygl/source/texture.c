/*
 * Texture Manager
 *
 * s31 changes (the rasteriser is unchanged: every texture is still stored
 * as one 256x256 RGB565 image, which the plan's F3/G13 replace):
 *  - glTexImage2D accepts every GL 1.3 format/type s31_pixels.c unpacks and
 *    honours the GL_UNPACK_* state, sampling the source straight into the
 *    256x256 image (no full-size copy); NULL pixels allocate a black image;
 *    mipmap levels > 0 are accepted and dropped; proxy targets answer
 *    glGetTexLevelParameteriv. It used to exit(1) for anything but
 *    RGB/UNSIGNED_BYTE, level 0.
 *  - glGenTextures reserves the names it returns (two calls used to return
 *    the same name); deleting texture 0 is ignored; a texture without an
 *    image draws untextured instead of dereferencing NULL (clip.c).
 *  - glTexEnv / glTexParameter / glPixelStore record every GL 1.3 value and
 *    raise GL errors instead of exiting.
 */

#include "zgl.h"
#include "s31_pixels.h"

#define TEX_SIZE 256            /* the rasteriser's only texture size */

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

  for(i=0;i<MAX_TEXTURE_LEVELS;i++) {
    im=&t->images[i];
    if (im->pixmap != NULL) gl_free(im->pixmap);
  }

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

  return t;
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

  if (target != GL_TEXTURE_2D) {
    if (target == GL_TEXTURE_1D || target == GL_TEXTURE_3D ||
        target == GL_TEXTURE_CUBE_MAP)
      gl_warn_once("glBindTexture(GL_TEXTURE_1D/3D/CUBE_MAP)");
    else
      gl_set_error(c, GL_INVALID_ENUM);
    return;
  }

  t=find_texture(c,texture);
  if (t==NULL) {
    t=alloc_texture(c,texture);
    if (t == NULL) { gl_set_error(c, GL_OUT_OF_MEMORY); return; }
  }
  c->current_texture=t;
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

/* internal formats that keep only alpha: RGB565 has nowhere to put it, so
   the texel is white (what GL_MODULATE of an alpha texture looks like on
   an opaque surface) */
static int alpha_only(int f)
{
  return f == GL_ALPHA || f == GL_ALPHA4 || f == GL_ALPHA8 ||
         f == GL_ALPHA12 || f == GL_ALPHA16;
}

void glopTexImage2D(GLContext *c,GLParam *p)
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
  GLImage *im;
  GLTexture *t;
  S31Unpack u;
  unsigned short *pix;
  int x,y,e,white;

  if (target != GL_TEXTURE_2D && target != GL_PROXY_TEXTURE_2D) {
    if (target == GL_TEXTURE_1D || target == GL_PROXY_TEXTURE_1D)
      gl_warn_once("glTexImage1D-style target in glTexImage2D");
    else
      gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (level < 0 || level >= MAX_TEXTURE_LEVELS || width < 0 || height < 0 ||
      (border != 0 && border != 1)) {
    gl_set_error(c, GL_INVALID_VALUE);
    return;
  }
  if (!valid_internal_format(components)) {
    gl_set_error(c, GL_INVALID_VALUE);
    return;
  }
  /* GL 1.x requires 2^n (+ 2*border) sizes. Not enforced: every size is
     resampled anyway, and Mesa (GL 2.x, non-power-of-two) accepts them, so
     stock apps written against Mesa do upload odd sizes. */
  e = s31_unpack_setup(c, &u, width, height, format, type, pixels);
  if (e) { gl_set_error(c, e); return; }

  if (target == GL_PROXY_TEXTURE_2D) {
    /* the proxy answers what a real upload would accept */
    if (width > TEX_SIZE || height > TEX_SIZE) {
      c->proxy_width = c->proxy_height = 0; c->proxy_format = 0;
    } else {
      c->proxy_width = width; c->proxy_height = height;
      c->proxy_format = components;
    }
    return;
  }
  /* Larger than GL_MAX_TEXTURE_SIZE: GL says GL_INVALID_VALUE. TinyGL
     always accepted and resampled, and GLU asks the proxy first anyway,
     so up to 2048 is still accepted (and sampled down). */
  if (width > 2048 || height > 2048) {
    gl_set_error(c, GL_INVALID_VALUE);
    return;
  }
  if (border) gl_warn_once("texture border (ignored)");

  t = c->current_texture;
  if (level > 0) {
    /* one image per texture; the base level is what is drawn */
    return;
  }
  t->width = width;
  t->height = height;
  t->internal_format = components;

  im=&t->images[0];
  if (im->pixmap == NULL) {
    im->pixmap = gl_malloc(TEX_SIZE * TEX_SIZE * sizeof(unsigned short));
    if (im->pixmap == NULL) {
      gl_set_error(c, GL_OUT_OF_MEMORY);
      t->width = t->height = 0;
      return;
    }
  }
  im->xsize = TEX_SIZE;
  im->ysize = TEX_SIZE;
  pix = im->pixmap;

  if (pixels == NULL || width == 0 || height == 0) {
    /* contents undefined by GL; black */
    memset(pix, 0, TEX_SIZE * TEX_SIZE * sizeof(unsigned short));
    return;
  }

  /* Nearest-neighbour resample from the application's image, as TinyGL's
     gl_resizeImageNoInterpolate did, but reading it in place. The image
     is flipped here exactly as before: row 0 of the source is t = 0. */
  white = alpha_only(components);
  for (y = 0; y < TEX_SIZE; y++) {
    int sy = (y * height) / TEX_SIZE;
    for (x = 0; x < TEX_SIZE; x++) {
      unsigned char rgba[4];
      int sx = (x * width) / TEX_SIZE;
      if (white) {
        *pix++ = 0xffff;
        continue;
      }
      s31_unpack_pixel(&u, sx, sy, rgba);
      *pix++ = ((rgba[0] & 0xF8) << 8) | ((rgba[1] & 0xFC) << 3) | (rgba[2] >> 3);
    }
  }
}

int tgl_get_tex_level_parameter(int target, int level, int pname, int *iv)
{
  GLContext *c=gl_get_context();
  int w, h, f;
  GLTexture *t = c->current_texture;

  if (level < 0 || level >= MAX_TEXTURE_LEVELS) return -2;
  if (target == GL_PROXY_TEXTURE_2D) {
    w = c->proxy_width; h = c->proxy_height; f = c->proxy_format;
  } else if (target == GL_TEXTURE_2D) {
    /* levels > 0 are accepted and not stored */
    w = level == 0 ? t->width : 0;
    h = level == 0 ? t->height : 0;
    f = t->internal_format;
  } else {
    return -1;
  }
  switch (pname) {
  case GL_TEXTURE_WIDTH: *iv = w; return 1;
  case GL_TEXTURE_HEIGHT: *iv = h; return 1;
  case GL_TEXTURE_BORDER: *iv = 0; return 1;
  case GL_TEXTURE_INTERNAL_FORMAT: *iv = f ? f : 1; return 1;
  case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_BLUE_SIZE: *iv = w ? 5 : 0; return 1;
  case GL_TEXTURE_GREEN_SIZE: *iv = w ? 6 : 0; return 1;
  case GL_TEXTURE_ALPHA_SIZE: case GL_TEXTURE_LUMINANCE_SIZE:
  case GL_TEXTURE_INTENSITY_SIZE: *iv = 0; return 1;
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
    return;
  }
  if (pname != GL_TEXTURE_ENV_MODE) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  switch (param) {
  case GL_DECAL:
  case GL_REPLACE:
    break;
  case GL_MODULATE:
  case GL_BLEND:
  case GL_ADD:
  case GL_COMBINE:
    gl_warn_once("glTexEnv(GL_TEXTURE_ENV_MODE other than GL_REPLACE/GL_DECAL)");
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  c->texenv_mode = param;
}

/* s31: records GL 1.3 texture parameters on the bound texture. p[3] is an
   int, p[4..7] a float vector (border colour, priority) */
void glopTexParameter(GLContext *c,GLParam *p)
{
  int target=p[1].i;
  int pname=p[2].i;
  int param=p[3].i;
  GLTexture *t=c->current_texture;

  if (target != GL_TEXTURE_2D) {
    if (target == GL_TEXTURE_1D || target == GL_TEXTURE_3D ||
        target == GL_TEXTURE_CUBE_MAP)
      return;
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }

  switch(pname) {
  case GL_TEXTURE_WRAP_S:
  case GL_TEXTURE_WRAP_T:
    if (param != GL_REPEAT && param != GL_CLAMP && param != GL_CLAMP_TO_EDGE) {
      gl_set_error(c, GL_INVALID_ENUM);
      return;
    }
    if (param != GL_REPEAT)
      gl_warn_once("GL_CLAMP/GL_CLAMP_TO_EDGE texture wrap (drawn as GL_REPEAT)");
    if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = param; else t->wrap_t = param;
    break;
  case GL_TEXTURE_MAG_FILTER:
    if (param != GL_NEAREST && param != GL_LINEAR) {
      gl_set_error(c, GL_INVALID_ENUM);
      return;
    }
    t->mag_filter = param;
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
    t->min_filter = param;
    break;
  case GL_TEXTURE_PRIORITY:
    t->priority = p[4].f < 0.0f ? 0.0f : (p[4].f > 1.0f ? 1.0f : p[4].f);
    break;
  case GL_TEXTURE_BORDER_COLOR:
  case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD:
  case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_MAX_LEVEL:
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
  GLTexture *t=c->current_texture;
  *kind = TGL_GET_INT;
  if (target != GL_TEXTURE_2D) return -1;
  switch (pname) {
  case GL_TEXTURE_MAG_FILTER: iv[0] = t->mag_filter; break;
  case GL_TEXTURE_MIN_FILTER: iv[0] = t->min_filter; break;
  case GL_TEXTURE_WRAP_S: iv[0] = t->wrap_s; break;
  case GL_TEXTURE_WRAP_T: iv[0] = t->wrap_t; break;
  case GL_TEXTURE_PRIORITY: fv[0] = t->priority; *kind = TGL_GET_FLOAT;
    iv[0] = (int)t->priority; return 1;
  case GL_TEXTURE_RESIDENT: iv[0] = 1; break;
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
