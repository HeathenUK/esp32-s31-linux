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
 * Phase 4: with the stencil test, the depth stage passes everything and a
 * stencil stage runs the depth test, the stencil test and ops and the
 * depth write (first in the list, or after the alpha test when one may
 * kill a fragment - GL's order); a coverage stage (smooth lines and
 * points) sits between fog and the alpha test; blending has five
 * equations.
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
  /* phase 4 F-PERSP (s31_tfilter.c): r/w, g/w, b/w, a/w (8.16 over fz =
     1/w), stepped only for a triangle whose colour is interpolated
     perspective-correctly (ZP_N_PC) */
  float rq, gq, bq, aq;
  float drqdx, dgqdx, dbqdx, daqdx;
  /* phase 4 F-LIN (review 4 R1): the y steps of s/w, t/w and 1/w, for the
     mipmap level of each 8-pixel block (zp_run_lod); set only for a
     triangle whose level is chosen per block */
  float dszdy, dtzdy, dfzdy;
  float pcr[4], pcgy[4];   /* their planes (the triangle filler's, per span) */
  /* phase 4 SMOOTH (raster.c gl_aa_line / gl_aa_point, zpipe.c zv_cover):
     the coverage coordinates of the span's first pixel centre and their
     step per pixel - a line: distance across (cva) and along (cvb) from its
     middle; a point: x (cva, step 1) and y (cvb, constant) from its centre.
     cvpp is the span's first pixel, so a chunk finds its own offset (the
     runner steps nothing new). Set only by the smooth rasterisers */
  float cva, cvda, cvb, cvdb;
  PIXEL *cvpp;
  /* the general triangle filler's runner for this triangle's spans:
     zp_run, or zp_run_lod when zpx_tri chose the level per block */
  void (*run)(ZBuffer *zb, struct ZSpan *s);
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
  /* phase 4 (s31_tfilter.c): the per-triangle choices - texture filter
     and mipmap level, perspective-correct colour. xact: ZPX_* bits, 0 when
     no triangle of the batch needs a choice (the fillers then do nothing
     new); x: the context's ZPipeX (at the end of GLContext, off the hot
     2 kB) */
  int xact;
  struct ZPipeX *x;
} ZPipe;

/* ZPipe.xact */
enum { ZPX_TEX = 1,      /* the texel stage is chosen per triangle (LOD) */
       ZPX_PC = 2 };     /* colour may be perspective-corrected per triangle */

/* phase 4 (s31_tfilter.c): one stored mipmap level as the texel stages
   read it. The fixed-point s, t of the whole texture (clip.c) address it
   by shifts: column (s >> shs) & wm, row (t >> sht) & hm, index
   (row << ws) | column */
typedef struct ZLevel {
  const PIXEL *pix;
  const unsigned char *alpha;          /* A8 plane or NULL */
  int shs, sht, ws;
  int wm, hm;                          /* W-1, H-1 of the level */
} ZLevel;

#define ZPX_MAXLEV 11                  /* MAX_TEXTURE_LEVELS (zgl.h) */

typedef struct ZPipeX {
  /* the stages gl_build_pipe placed (index in ZPipe.st, -1: none) */
  int slot_tex, slot_col;
  int need0;                           /* ZPipe.need without per-triangle bits */
  ZStageFn col_affine;                 /* the batch's colour stage */
  /* the texel stage of the batch without a choice, and the level-0
     texture it reads (pixel paths and lines restore these) */
  ZStageFn tex_base;
  const PIXEL *tex0;
  const unsigned char *talpha0;
  /* texture filtering (TF_* kinds): magnification and minification, the
     lambda at which they switch (GL 1.3 3.8.8 c), the highest level */
  int kmag, kmin, nlev;
  float lod_c;
  float lod_ks, lod_kt;                /* fixed point -> level-0 texels, squared */
  float lod_min, lod_max;              /* GL_TEXTURE_MIN_LOD / MAX_LOD */
  /* the level choice the texel slot holds now (zpx_code's code, -1: none
     yet): zp_run_lod re-places the stage only when a block's differs */
  int lod_cur;
  ZLevel lvl[ZPX_MAXLEV];              /* the stored chain, lvl[0] = level 0 */
  /* the filtered stages for the texture's wraps and format: nearest in a
     level > 0, bilinear, trilinear */
  ZStageFn f_near, f_bil, f_tri, f_ntri;
  int rep_s, rep_t;                    /* -1: GL_REPEAT on that axis, else 0 */
  /* this chunk's level(s), and the trilinear weight of cur1 (0..32) for
     each of its 8-pixel blocks */
  const ZLevel *cur0, *cur1;
  unsigned char twb[ZP_CHUNK / 8];
  int lod_gen;                         /* MIN_LOD / MAX_LOD are not GL's defaults */
  /* filtered texels of the chunk: the texenv stages read them through
     ZPipe.tex / talpha with idx[i] = i */
  PIXEL ftex[ZP_CHUNK];
  unsigned char falpha[ZP_CHUNK];
  /* perspective colour: selected when the colour error of screen-affine
     interpolation could reach pc_min 8.16 units */
  float pc_min;
  int pc_cur;                          /* the colour slot holds the corrected stage */
  /* phase 4 BLEND-EQ: the blend equation zp_out_fn was chosen for
     (GL_FUNC_ADD when blending is off), and GL_BLEND_COLOR in 8 bits for
     the GL_CONSTANT_* factors */
  int beq;
  unsigned char bcol[4];
  /* phase 4 F8-STENCIL (raster_sel.c zs_build): per stored stencil value
     s, the value each outcome leaves - bits 0-7 stencil test failed
     (GL_STENCIL_FAIL op), 8-15 depth failed (PASS_DEPTH_FAIL), 16-23 both
     passed (PASS_DEPTH_PASS), with the write mask applied - and bit 24 set
     when the stencil test passes. One load per fragment replaces the
     function, the reference, both masks and three ops, so the stencil
     stage tests no GL state per pixel. st_key: the state it was built for.
     st_zfn: the read-only depth test the stencil stage runs first. The
     1 kB table is allocated at the first stencil batch (review 4 R6-size:
     it sat in every context, stencil or not) */
  unsigned int *stab;
  int st_key[7], st_valid;
  ZDepthFn st_zfn;
  /* the stencil value of a span's pixel is at the same offset from st_sb
     as its depth is from st_zb: the buffers' own (gl_build_pipe), or a
     line's gathered copies while it runs them (raster.c). So no span
     producer sets, and no runner steps, a stencil pointer */
  const unsigned short *st_zb;
  unsigned char *st_sb;
  /* phase 4 SMOOTH: what the coverage stage multiplies alpha by. cov_kind
     0 (a triangle in a smooth-line batch: the stage does nothing), 1 a
     smooth line, 2 a smooth point, set by the smooth rasterisers for their
     primitive; the line's half width + 1/2 and half length + 1/2 and its
     length cap (min(1, length)), the point's radius + 1/2 */
  int cov_kind;
  float cv_hw, cv_hl, cv_lcap, cv_rr;
} ZPipeX;

/* phase 4: the spread of the bit patterns of three q = 1/w (> 0). The
   float bits grow with the value, piecewise linear with slope 2^23 / 2^e
   on [2^e, 2^(e+1)), so q to r q moves them by at least 2^23 (r - 1) / r.
   zpx_qspread: can the ratio exceed 1.0648 (ZPX_PC_PRE, perspective
   colour)? 2^23 (r - 1) / r = 2^18.96 there; 0x7C000 (2^18.93) is still
   conservative (review 4 R2-smooth: the 2^18 first used passed ratios from
   1.03, sending many triangles to fill_pq_slow for nothing; measured
   hash-identical on every bench frame). zpx_qspread_lod: can it exceed
   ~1.03 (2^18)? - a triangle below that takes one mipmap level for its
   whole area (lambda then varies by less than 2 log2 1.03 = 0.09 across
   it), above it one per 8-pixel block (review 4 R1). The one test the
   fillers make per triangle before anything else */
static inline unsigned int zpx_qbits(float qa, float qb, float qc)
{
  union { float f; unsigned int u; } a, b, d;
  unsigned int mx, mn;
  a.f = qa; b.f = qb; d.f = qc;
  mx = a.u > b.u ? a.u : b.u;
  mn = a.u < b.u ? a.u : b.u;
  mx = d.u > mx ? d.u : mx;
  mn = d.u < mn ? d.u : mn;
  return mx - mn;
}
static inline int zpx_qspread(float qa, float qb, float qc)
{
  return zpx_qbits(qa, qb, qc) >= 0x7C000u;
}
static inline int zpx_qspread_lod(float qa, float qb, float qc)
{
  return zpx_qbits(qa, qb, qc) >= 0x40000u;
}

/* perspective colour (s31_tfilter.c): a triangle is corrected when
   crange * (qmax - qmin) > ZPX_PC_MIN * (qmax + qmin), i.e. when the
   screen-affine error crange (qmax - qmin) / (2 (qmax + qmin)) can exceed
   E = 4 8-bit levels (4 x 65536 in 8.16): one step of RGB565's green, half
   of red's and blue's - below it the corrected colour would change a pixel
   by at most one stored step, and only at a quantisation boundary. With
   crange at most 255 levels, only q ratios above 1.0648 can qualify
   ((qmax - qmin) * ZPX_PC_PRE > qmax + qmin, ZPX_PC_PRE = 255 / (2 * 4)) */
#define ZPX_PC_MIN (8.0f * 65536.0f)
#define ZPX_PC_PRE 31.875f
/* long spans on tier 1 (s31_tfilter.c gl_draw_triangle_fill_pq): a smooth
   triangle at least ZPX_LONG_W pixels wide whose colour range is at least
   ZPX_LONG_C zp units (1/64 of full scale: below it the stepping error
   cannot reach a stored step) is re-anchored every 8 pixels */
#define ZPX_LONG_W 64
#define ZPX_LONG_C 1024

/* texture filter kinds (ZPipeX.kmag / kmin) */
enum { TF_NEAREST0,   /* nearest, level 0: the batch's own texidx stage */
       TF_LINEAR0,    /* bilinear, level 0 */
       TF_NMN,        /* NEAREST_MIPMAP_NEAREST */
       TF_LMN,        /* LINEAR_MIPMAP_NEAREST */
       TF_NML,        /* NEAREST_MIPMAP_LINEAR */
       TF_LML };      /* LINEAR_MIPMAP_LINEAR */

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
       ZP_N_SPEC = 32, ZP_N_PC = 64 };

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
/* phase 4 F8-STENCIL: the stencil test and its ops, with the depth test
   (x->st_zfn) run inside it and the depth write fused (dwrite). late 0:
   the first stage (nothing before it can kill a fragment); late 1: after
   the alpha test / polygon stipple, only on the fragments they left. rw 0:
   every op KEEP or a zero write mask - the buffer is only read. The depth
   stage of the batch is then zp_depth_fn(ZP_DEPTH_NONE, 0) (all alive) */
ZStageFn zp_stencil_fn(int late, int rw, int dwrite);
ZDepthFn zp_depth_never_m(void);    /* GL_NEVER that clears the mask */
/* phase 4 SMOOTH: alpha times coverage (GL 1.3 3.11), after fog, before
   the alpha test; present only in a batch with smooth lines or points */
ZStageFn zp_cover_fn(void);
ZStageFn zp_stipple_fn(void);       /* GL_POLYGON_STIPPLE (reads ZPipe.stip_on) */
/* the final stage: blend (GL factors, dst alpha folded; equation eq,
   phase 4) + colour mask */
ZStageFn zp_out_fn(int sfactor, int dfactor, int cmask, int eq);

/* process one span through zb->pipe */
void zp_run(ZBuffer *zb, ZSpan *s);

/* phase 4 (s31_tfilter.c) */
struct GLContext;
struct GLTexture;
struct ZTri;
/* gl_build_pipe's part: the filter kinds, the level chain, xact */
void zpx_build(struct GLContext *c, const struct GLTexture *t, int flat);
/* the general filler, per triangle (xact != 0), before its planes: pick
   the texel stage and level(s) from the triangle's lambda, and whether its
   colour is perspective-corrected (sets ZPipe.need) */
/* returns 1 when the triangle's mipmap level is chosen per 8-pixel block
   (its 1/w differ: zpx_qspread_lod): the filler then runs its spans with
   zp_run_lod, which reads ZSpan.dszdy, dtzdy, dfzdy */
int zpx_tri(ZPipe *p, const struct ZTri *T, const ZVtxG *v0, const ZVtxG *v1,
            const ZVtxG *v2);
/* zp_run for such a triangle: each 8-pixel block's lambda (at its centre,
   from the span's s/w, t/w, 1/w and their x and y steps) picks the texel
   stage and level(s); consecutive blocks with the same choice form one
   chunk (up to ZP_CHUNK), so the stages still run per chunk */
void zp_run_lod(ZBuffer *zb, ZSpan *s);
/* lines and points: the batch's base choices (level 0, affine colour) */
void zpx_reset(ZPipe *p);
/* the texel stages of phase 4, for s31_draw.c's pixel paths to drop */
int zpx_is_tex_stage(ZStageFn f);
/* the texel stage for kind k at the batch's wraps (TF_LINEAR0 ...) */
ZStageFn zpx_stage(int kind, int repeat, int alpha);
ZStageFn zpx_color_pc(void);

/* ztriangle_gen.c: the general triangle filler (GL's pixel-centre rule,
   top-left fill convention, so shared edges are drawn once) */
void ZB_fillTriangleGeneral(ZBuffer *zb, const ZVtxG *v0, const ZVtxG *v1,
                            const ZVtxG *v2, int textured);

#endif
