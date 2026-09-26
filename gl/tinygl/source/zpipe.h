/*
 * zpipe.h - the general fragment path (plan F3-F6). s31, MIT.
 *
 * TinyGL's fillers (ztriangle.c) fuse one fixed fragment recipe into the
 * scanline loop. They stay exactly as they were for every frame that uses
 * none of the added features. Anything else - blending, alpha test, fog,
 * texture environments other than the texel itself (REPLACE/DECAL of an
 * opaque texture, MODULATE by white), alpha textures, CLAMP wrap, colour
 * masks, depth functions other than LESS/LEQUAL (and LESS without the
 * depth write), separate specular, depth mask / texture / width on lines
 * and points - is drawn here.
 *
 * Specialised, not branched. At glBegin (raster.c gl_update_raster) the
 * state selects one function per stage; a span is then processed in
 * chunks of ZP_CHUNK pixels by that fixed list of stages, each a tight
 * loop with no per-pixel test of GL state. A disabled feature has no
 * stage at all, so it costs nothing:
 *
 *   depth test  -> colour (flat/smooth) -> texel index (wrap) -> texenv
 *   -> fog -> alpha test -> depth write -> blend / colour mask / store
 *
 * The depth test runs first (read only) so a chunk that is wholly hidden
 * stops there; the depth write runs after the alpha test, as GL orders
 * them. Fragment colours are 8-bit per channel; colours are interpolated
 * in 8.16 fixed point (ZP_C1 = 1.0), multiplied as (x * (y + 1)) >> 8,
 * which is exact for 0 and 255 (so MODULATE by white is the texel), and
 * stored to RGB565 by truncation as TinyGL's RGB_TO_PIXEL does.
 */
#ifndef ZPIPE_H
#define ZPIPE_H

#include "zbuffer.h"

#define ZP_CHUNK 32
#define ZP_CSHIFT 16                   /* colour fixed point: 8.16 */
#define ZP_C1 (255 << ZP_CSHIFT)       /* 1.0 */

/* one span of a triangle (or one pixel of a line / row of a point) */
typedef struct ZSpan {
  PIXEL *pp;               /* first pixel */
  unsigned short *pz;      /* its depth */
  int n;                   /* pixels, >= 1 */
  unsigned int z; int dzdx;            /* depth, zp scale */
  int r, g, b, a;                      /* 8.16 */
  int drdx, dgdx, dbdx, dadx;
  int sr, sg, sb, dsrdx, dsgdx, dsbdx; /* secondary colour, 8.16 */
  /* perspective: s/w, t/w, fog/w (fog factor 0..255) over fz = 1/w, as the
     general filler interpolates them; a line or point pixel has fz = 1 */
  float sz, tz, fq, fz;
  float dszdx, dtzdx, dfqdx, dfzdx;
} ZSpan;

/* the chunk being processed */
typedef struct ZFrag {
  int n;
  unsigned char m[ZP_CHUNK];           /* 1: fragment alive */
  unsigned short zz[ZP_CHUNK];         /* its depth, for the write */
  unsigned char r[ZP_CHUNK], g[ZP_CHUNK], b[ZP_CHUNK], a[ZP_CHUNK];
  unsigned int idx[ZP_CHUNK];          /* texel index */
} ZFrag;

struct ZPipe;
typedef int (*ZDepthFn)(const ZSpan *s, ZFrag *f);         /* returns alive */
typedef void (*ZStageFn)(const struct ZPipe *p, const ZSpan *s, ZFrag *f);

#define ZP_MAX_STAGES 10   /* the 8 stage kinds + NULL, with room */

typedef struct ZPipe {
  ZDepthFn depth;
  ZStageFn st[ZP_MAX_STAGES];          /* NULL-terminated */
  /* texture */
  const PIXEL *tex;
  const unsigned char *talpha;
  int ws, hs, fbits;                   /* log2 size, fraction bits of s/t */
  unsigned int smask, tmask;           /* repeat masks (as ZBuffer's) */
  int wmax, hmax;                      /* W-1, H-1 (clamp) */
  unsigned char envc[4];               /* GL_TEXTURE_ENV_COLOR, 8-bit */
  /* fog colour, alpha reference, flat colour */
  unsigned char fogc[3];
  int aref;                            /* per function: raster_sel.c */
  unsigned char flat[4];
  unsigned char flatspec[3];           /* flat secondary colour */
  /* blending: factors after dst-alpha folding, colour write mask */
  int sfactor, dfactor;
  unsigned short cmask;
  /* triangles never write outside this: viewport, scissor and buffer
     (x0 y0 x1 y1, rows from the top) */
  int box[4];
  /* phase 3a G03 (s31_zepoch.c), for ztri_setup: the depth epoch's base
     << 14, added to every triangle's depth plane, and the highest plain
     vertex depth a triangle may have without zep_tri_check (a new zmax, or
     a depth that does not fit above zoff); ctx is the GLContext */
  unsigned int zoff, zchk;
  unsigned int zguard;  /* ZBuffer.zguard's copy: a triangle below it first
                           materialises the epoch (GL_LESS, stale pixels) */
  int zact;             /* zchk != ~0 or zoff != 0: ztri_zepoch has work */
  void *zctx;
  /* phase 3a dirty boxes (s31_zepoch.c): what has been drawn into since
     the last full clear (x0 y0 x1 y1), recorded while bact */
  int db[4];
  int bact;
  /* the rows a triangle may cover without zdb_tri (ztri_rows): db[1] and
     db[3] with rows-only boxes; with x boxes (S31GL_DIRTYBOX=2) an empty
     range, so every triangle takes zdb_tri */
  int dbc[2];
  int st_spec;                         /* the secondary colour is interpolated */
  /* the state gl_update_raster decided on, for gl_build_pipe */
  int dsel, afunc, nocolor, clamp_s, clamp_t;
  /* the span attributes the chosen stages read: the filler evaluates and
     zp_run steps only these (ZP_N_*) */
  int need;
  /* polygon stipple (plan F7): the pattern (row y%32, bit x%32), the
     buffer it is placed in, and whether the primitive is a polygon
     (lines and points are not stippled by it) */
  const unsigned int *stip;
  ZBuffer *zb;
  int stip_on;
} ZPipe;

/* phase 3a G03 (s31_zepoch.c): a triangle whose highest plain depth m is
   above zchk - a new zmax, or a depth that does not fit above the epoch's
   base (the buffer goes back to the plain mapping first). m = ~0: a sliver
   whose depth gradient saturated (its depths are meaningless) */
void zep_tri_check(ZPipe *p, unsigned int m);
/* a triangle whose depth gradient saturated (review 3a R4): bounds, or
   failing that demotes, from its rows */
struct ZTri;
void zep_tri_sliver(ZPipe *p, const struct ZTri *T);
void zep_tri_far(ZPipe *p);
/* phase 3a dirty boxes: a triangle's rows (or, S31GL_DIRTYBOX=2, its
   box) grow the box being drawn */
/* (ya, yb: the triangle's rows; x0, dx1, dx2: its top vertex's window x
   and the other two's offsets from it, for S31GL_DIRTYBOX=2 - scalars, not
   the ZTri: passing &T from the fillers cost them a callee-saved register) */
void zdb_tri(ZPipe *p, int ya, int yb, float x0, float dx1, float dx2);

enum { ZP_N_Z = 1, ZP_N_RGBA = 2, ZP_N_Q = 4, ZP_N_F = 8, ZP_N_ST = 16,
       ZP_N_SPEC = 32 };

/* a vertex of the general filler: GL window coordinates (pixel centres at
   +0.5, rows from the top), TinyGL's integer depth, colour and alpha in 8.16, the
   fog factor (0..255) times 1/w, and s/w, t/w (the texture's fixed point)
   and q = 1/w */
typedef struct {
  float x, y;
  int z;                    /* the same integer depth tier 1 reads (ztri.h) */
  float r, g, b, a, f;
  float s, t, q;
  float sr, sg, sb;         /* GL_SEPARATE_SPECULAR_COLOR, 8.16 */
  int si, ti;               /* s, t unprojected (zp.s, zp.t): the REPEAT shift */
} ZVtxG;

/* stage selectors, used by raster.c (zpipe.c holds the stages) */
enum { ZP_DEPTH_NONE, ZP_DEPTH_NEVER, ZP_DEPTH_LESS, ZP_DEPTH_EQUAL,
       ZP_DEPTH_LEQUAL, ZP_DEPTH_GREATER, ZP_DEPTH_NOTEQUAL,
       ZP_DEPTH_GEQUAL, ZP_DEPTH_ALWAYS };                /* GL order */
ZDepthFn zp_depth_fn(int zp_depth, int write);   /* write: test + write fused */
ZStageFn zp_color_fn(int flat);
ZStageFn zp_texidx_fn(int clamp_s, int clamp_t);
/* texenv ops: the (mode, base format) pairs GL 1.3 table 3.22 needs */
enum { ZP_TE_REPLACE_RGB, ZP_TE_REPLACE_RGBA, ZP_TE_MOD_RGB, ZP_TE_MOD_RGBA,
       ZP_TE_DECAL_RGBA, ZP_TE_BLEND_RGB, ZP_TE_BLEND_RGBA, ZP_TE_BLEND_I,
       ZP_TE_ALPHA_REPLACE, ZP_TE_ALPHA_MOD, ZP_TE_ADD_RGB, ZP_TE_ADD_RGBA,
       ZP_TE_ADD_I, ZP_TE_N };
ZStageFn zp_texenv_fn(int op);
ZStageFn zp_fog_fn(void);
ZStageFn zp_spec_fn(int flat);         /* add the secondary colour */
ZStageFn zp_alpha_fn(int gl_func);     /* GL_LESS ... (not NEVER/ALWAYS) */
ZStageFn zp_zwrite_fn(void);
ZStageFn zp_stipple_fn(void);       /* GL_POLYGON_STIPPLE (reads ZPipe.stip_on) */
/* the final stage: blend (GL factors, dst alpha folded) + colour mask */
ZStageFn zp_out_fn(int sfactor, int dfactor, int cmask);

/* process one span through zb->pipe */
void zp_run(ZBuffer *zb, ZSpan *s);

/* ztriangle_gen.c: the general triangle filler (GL's pixel-centre rule,
   top-left fill convention, so shared edges are drawn once) */
void ZB_fillTriangleGeneral(ZBuffer *zb, const ZVtxG *v0, const ZVtxG *v1,
                            const ZVtxG *v2, int textured);

#endif
