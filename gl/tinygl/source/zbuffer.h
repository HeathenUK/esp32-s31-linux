#ifndef _tgl_zbuffer_h_
#define _tgl_zbuffer_h_

/*
 * Z buffer
 */

#include "zfeatures.h"

#define ZB_Z_BITS 16

#define ZB_POINT_Z_FRAC_BITS 14

#define ZB_POINT_S_MIN ( (1<<13) )
#define ZB_POINT_S_MAX ( (1<<22)-(1<<13) )
#define ZB_POINT_T_MIN ( (1<<21) )
#define ZB_POINT_T_MAX ( (1<<30)-(1<<21) )

#ifdef S31GL_P4ARITH
#define ZB_POINT_RED_MIN ( (1<<10) )
#define ZB_POINT_RED_MAX ( (1<<16)-(1<<10) )
#define ZB_POINT_GREEN_MIN ( (1<<9) )
#define ZB_POINT_GREEN_MAX ( (1<<16)-(1<<9) )
#define ZB_POINT_BLUE_MIN ( (1<<10) )
#define ZB_POINT_BLUE_MAX ( (1<<16)-(1<<10) )
#else
/* s31 phase 5 P: a colour c in [0, 1] is 65280 c + 128, i.e. 256 (255 c +
   1/2), so its top 5 / 6 bits (RGB_TO_PIXEL, the smooth filler's packed
   fields) are round(255 c) truncated to 565 - Mesa's result on a 565
   buffer (gl/tests/glx_prec.c bands 1-3, 27). TinyGL's 1024 / 512 minimum
   rounded c straight to 5 / 6 bits, one level above Mesa for 29% of the
   values. The general path's 8.16 colour (raster_int.h c816) is
   (v - 128) 256 + 2^15 = 256 v: the same round(255 c) as before. The
   triangle fillers evaluate these planes with 8 more fraction bits
   (ztriangle.h ZTRI_CPLANE), so a plane's rounding cannot take a pixel
   below 0 or above 65535 (the 128 margin) */
#define ZB_POINT_RED_MIN ( 128 )
#define ZB_POINT_RED_MAX ( 65408 )
#define ZB_POINT_GREEN_MIN ( 128 )
#define ZB_POINT_GREEN_MAX ( 65408 )
#define ZB_POINT_BLUE_MIN ( 128 )
#define ZB_POINT_BLUE_MAX ( 65408 )
#endif

/* display modes */
#define ZB_MODE_5R6G5B  1  /* true color 16 bits */
#define ZB_MODE_INDEX   2  /* color index 8 bits */
#define ZB_MODE_RGBA    3  /* 32 bit rgba mode */
#define ZB_MODE_RGB24   4  /* 24 bit rgb mode */
#define ZB_NB_COLORS    225 /* number of colors for 8 bit display */

#if TGL_FEATURE_RENDER_BITS == 15

#define RGB_TO_PIXEL(r,g,b) \
  ((((r) >> 1) & 0x7c00) | (((g) >> 6) & 0x03e0) | ((b) >> 11))
typedef unsigned short PIXEL;
/* bytes per pixel */
#define PSZB 2 
/* bits per pixel = (1 << PSZH) */
#define PSZSH 4 

#elif TGL_FEATURE_RENDER_BITS == 16

/* 16 bit mode */
#define RGB_TO_PIXEL(r,g,b) \
  (((r) & 0xF800) | (((g) >> 5) & 0x07E0) | ((b) >> 11))
typedef unsigned short PIXEL;
#define PSZB 2 
#define PSZSH 4 

#elif TGL_FEATURE_RENDER_BITS == 24

#define RGB_TO_PIXEL(r,g,b) \
  ((((r) << 8) & 0xff0000) | ((g) & 0xff00) | ((b) >> 8))
typedef unsigned char PIXEL;
#define PSZB 3
#define PSZSH 5

#elif TGL_FEATURE_RENDER_BITS == 32

#define RGB_TO_PIXEL(r,g,b) \
  ((((r) << 8) & 0xff0000) | ((g) & 0xff00) | ((b) >> 8))
typedef unsigned int PIXEL;
#define PSZB 4
#define PSZSH 5

#else

#error Incorrect number of bits per pixel

#endif

typedef struct {
    int xsize,ysize;
    int linesize; /* line size, in bytes */
    int mode;
    
    unsigned short *zbuf;
    PIXEL *pbuf;
    int frame_buffer_allocated;
    
    int nb_colors;
    unsigned char *dctable;
    int *ctable;
    PIXEL *current_texture;
    /* s31 phase 5: a tier-1 texture of 8-bit texels (s31_tex8.c): 
       current_texture is its index / grey plane, tex_pal the PACK of each
       index (the palette's, or the grey ramp's), tex_shift the byte shift */
    const unsigned short *tex_pal;
    int zbuf_ext;  /* s31: zbuf belongs to the caller (the GLX drawable), never freed here */

    /* s31: the bound texture's native size (plan F3). The texel of
       fixed-point (s, t) is at byte offset
         ((t & tex_tmask) | (s & tex_smask)) >> tex_shift
       from current_texture: s holds the column at bits [F, F+ws), t the
       row at bits [F+ws, F+ws+hs) (gl_transform_to_viewport scales them
       so), which is the same three operations per pixel as TinyGL's fixed
       256x256 layout; the masks give GL_REPEAT. See texture.c. */
    int tex_smask, tex_tmask, tex_shift;
    /* s31: one GL_REPEAT period of s and of t in that fixed point (a power
       of two): the textured filler adds whole periods to a triangle with a
       negative coordinate, so its int conversion floors (ztriangle.h) */
    int tex_speriod, tex_tperiod;
    /* s31: the colour of the next flat-shaded triangle (clip.c) */
    int flat_color;
    struct ZPipe *pipe;       /* s31: the general fragment path (zpipe.h) */

    /* s31 (phase 3a G03): depth epochs (s31_zepoch.c). zst is the state
       at the tail of the depth memory (ZB_DEPTH_TAIL), shared by every
       context bound to it; the rest is this context's copy, synchronised
       from it (zser). Vertex depth (zp.z) is always TinyGL's plain value;
       the rasterisers store it plus zoff */
    struct ZDepthState *zst;
    unsigned int zser;        /* zst->serial the fields below follow */
    unsigned int zoff;        /* the epoch's base << 14: added to every stored depth */
    unsigned int ztop;        /* the highest plain zp.z that still fits above zoff */
    unsigned int *zmaxp;      /* &zst->zmax while depth is written, else zmax_none */
    unsigned int zmax_none;
    int zguard;               /* 0, or: a GL_LESS primitive with a plain zp.z
                                 below it must see stale pixels as 1.0 */

    /* s31 (phase 3a): dirty boxes (s31_zepoch.c "dirty boxes"). With a
       retained colour buffer (the caller never writes it: GLX), a full
       clear writes only the box drawn into since the last full clear to the
       same value. Boxes are x0 y0 x1 y1, rows from the top, x1/y1 past the
       end; the one being drawn is ZPipe.db */
    int retained;
    struct ZColourSlot {      /* colour, per buffer (GLX ping-pongs two) */
        PIXEL *buf;
        int box[4], valid;    /* drawn into since its last full clear ... */
        unsigned int val, ser;/* ... to val, at bind serial ser */
    } cs[2];
    int cur;                  /* cs[] of pbuf, or -1 */
    int zdb_skip;             /* frames left without recording (s31_zepoch.c) */
    int dzbox[4], dzvalid;    /* depth: drawn into since the last real clear */
    unsigned int dzval, dzser;
    unsigned short *dzbuf;

    /* s31 (phase 4 F8): the 8-bit stencil buffer, xsize * ysize bytes
       (row y at y * xsize, as zbuf), followed by ZB_STENCIL_TAIL bytes of
       ZStencilState; NULL when the context has no stencil bits. The
       caller's (the GLX drawable's, sbuf_ext) or a private one allocated
       with the depth buffer. sst is its state, in that tail */
    unsigned char *sbuf;
    int sbuf_ext;
    struct ZStencilState *sst;
} ZBuffer;

/* s31 (phase 4 F8): the stencil buffer's state, kept after its values so
   every context bound to one buffer (GLX: the drawable's) shares it. The
   dirty range: since the last full clear to val, only bytes [lo, hi) may
   differ from val (a stencil write through the stages grows it, clear.c
   clamps it), so the next full clear to the same value writes only those.
   valid 0: nothing is known (the first full clear writes everything). */
typedef struct ZStencilState {
    unsigned int magic;       /* ZST_MAGIC once the core owns the state */
    int lo, hi;
    unsigned char val, valid, pad[2];
} ZStencilState;
#define ZB_STENCIL_TAIL 20
#define ZB_STENCIL_STATE(sbuf, npix) \
  ((ZStencilState *)(((unsigned long)((unsigned char *)(sbuf) + (npix)) + 3) & ~3ul))
#define ZST_MAGIC 0x53543331u     /* "ST31" */
#define ZST_ZERO 0x5354305au      /* "Z0ST": the caller states the buffer is all 0
                                     (s31gl_stencil_zeroed; review 4 R3-stencil) */

/* s31 (phase 3a G03): depth epochs. A full glClear of depth to 1.0 does
   not write the buffer when the values drawn since the last real clear
   leave room above them: the next epoch stores every depth plus a base
   above the highest value written (zmax), so everything left from before
   compares as farther than anything drawn now - it reads as the clear
   value. Details and the exact rules: s31_zepoch.c. The state lives after
   the depth values, so a depth buffer shared by several contexts (GLX: the
   drawable's) carries it; all zero is "plain 16-bit mapping, nothing
   stale", which is what calloc gives. */
typedef struct ZDepthState {
    unsigned int serial;      /* bumped at every change of base/stale */
    unsigned int zmax;        /* highest stored depth << 14 since the last real clear */
    unsigned short base;      /* the epoch's lowest stored value (0: plain) */
    unsigned char stale;      /* pixels from an earlier epoch may be left */
    unsigned char backoff;    /* full clears left that stay real (s31_zepoch.c) */
    unsigned int magic;       /* ZEP_MAGIC (the top 24 bits) once the core
                                 has taken the state over (zep_attach):
                                 anything else - the caller's zeroes, or old
                                 depth values where a resized buffer's tail
                                 now falls - is not trusted, and the first
                                 full clear is real. The low byte: how many
                                 demotions/materialisations in a row (the
                                 backoff doubles with each) */
} ZDepthState;
/* bytes to allocate after the w*h depth values: the state, 4-aligned */
#define ZB_DEPTH_TAIL 20
#define ZB_DEPTH_STATE(zbuf, npix) \
  ((ZDepthState *)(((unsigned long)((unsigned short *)(zbuf) + (npix)) + 3) & ~3ul))

typedef struct {
  int x,y,z;     /* integer coordinates in the zbuffer */
  int s,t;       /* coordinates for the mapping */
  int r,g,b;     /* color indexes */

  /* s31: the triangle fillers' vertex (ztri.h): GL window coordinates,
     rows from the top, pixel centres at +1/2 - not TinyGL's snapped x, y,
     which lines and points still use - and q = 1/w for the perspective
     division (gl_transform_to_viewport computes all three once per vertex;
     review P1/G1, P2) */
  float fx,fy,q;
} ZBufferPoint;



/* zbuffer.c */

ZBuffer *ZB_open(int xsize,int ysize,int mode,
		 int nb_colors,
		 unsigned char *color_indexes,
		 int *color_table,
		 void *frame_buffer);


void ZB_close(ZBuffer *zb);

void ZB_resize(ZBuffer *zb,void *frame_buffer,int xsize,int ysize);
void ZB_clear(ZBuffer *zb,int clear_z,int z,
	      int clear_color,int r,int g,int b);
/* linesize is in BYTES */
void ZB_copyFrameBuffer(ZBuffer *zb,void *buf,int linesize);

/* zdither.c */

void ZB_initDither(ZBuffer *zb,int nb_colors,
		   unsigned char *color_indexes,int *color_table);
void ZB_closeDither(ZBuffer *zb);
void ZB_ditherFrameBuffer(ZBuffer *zb,unsigned char *dest,
			  int linesize);

/* zline.c */

void ZB_plot(ZBuffer *zb,ZBufferPoint *p);
void ZB_line(ZBuffer *zb,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_line_z(ZBuffer * zb, ZBufferPoint * p1, ZBufferPoint * p2);

/* ztriangle.c */

void ZB_setTexture(ZBuffer *zb, PIXEL *texture);

void ZB_fillTriangleFlat(ZBuffer *zb,
		 ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);

void ZB_fillTriangleSmooth(ZBuffer *zb,
		   ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);

void ZB_fillTriangleMappingPerspective(ZBuffer *zb,
                    ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);


/* s31: ztriangle_nt.c (no depth test) and ztriangle_nw.c (no depth write) */
void ZB_fillTriangleFlat_nt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmooth_nt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleMappingPerspective_nt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleFlat_nw(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmooth_nw(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleMappingPerspective_nw(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_plot_nz(ZBuffer *zb,ZBufferPoint *p);
/* s31 (phase 4 F-PERSP): the smooth filler with perspective-correct colour,
   for depth LEQUAL and LESS (the other depth states take the general path) */
void ZB_fillTriangleSmoothPersp(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmoothPersp_lt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
/* (phase 4) the smooth filler re-anchored every 8 pixels (long spans, equal
   w), for LEQUAL, LESS and no depth test */
void ZB_fillTriangleSmoothLong(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmoothLong_nt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmoothLong_lt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
/* s31: ztriangle_lt.c, GL_LESS (strict) with depth writes */
void ZB_fillTriangleFlat_lt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmooth_lt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleMappingPerspective_lt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
/* s31 phase 5: the same for a texture of 8-bit texels (ZBuffer.tex_pal) */
void ZB_fillTriangleMappingPerspective8(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective8_nt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective8_nw(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective8_lt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective32(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective32_nt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective32_nw(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
void ZB_fillTriangleMappingPerspective32_lt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
/* PACK(l, l, l) of every grey value (s31_tex8.c) */
extern const unsigned short s31_grey565[256];
/* s31: zline.c, GL_LESS lines and points */
void ZB_line_z_lt(ZBuffer *zb, ZBufferPoint *p1, ZBufferPoint *p2);
void ZB_plot_lt(ZBuffer *zb, ZBufferPoint *p);
/* s31: glClear inside a rectangle (scissor), with colour and depth masks */
void ZB_clear_rect(ZBuffer *zb, int x0, int y0, int x1, int y1,
                   int clear_z, int z, int clear_color, int color, int cmask);
/* s31: the colour fill of glClear (and any 16-bit fill): n pixels of
   val from p, any alignment */
void ZB_fill16(unsigned short *p, unsigned int val, int n);

typedef void (*ZB_fillTriangleFunc)(ZBuffer  *,
	    ZBufferPoint *,ZBufferPoint *,ZBufferPoint *);

/* memory.c */
void gl_free(void *p);
void *gl_malloc(int size);
void *gl_zalloc(int size);

#endif /* _tgl_zbuffer_h_ */
