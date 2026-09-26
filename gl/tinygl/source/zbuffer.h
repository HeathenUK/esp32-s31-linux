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

#define ZB_POINT_RED_MIN ( (1<<10) )
#define ZB_POINT_RED_MAX ( (1<<16)-(1<<10) )
#define ZB_POINT_GREEN_MIN ( (1<<9) )
#define ZB_POINT_GREEN_MAX ( (1<<16)-(1<<9) )
#define ZB_POINT_BLUE_MIN ( (1<<10) )
#define ZB_POINT_BLUE_MAX ( (1<<16)-(1<<10) )

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
} ZBuffer;

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
/* s31: ztriangle_lt.c, GL_LESS (strict) with depth writes */
void ZB_fillTriangleFlat_lt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleSmooth_lt(ZBuffer *zb, ZBufferPoint *p1,ZBufferPoint *p2,ZBufferPoint *p3);
void ZB_fillTriangleMappingPerspective_lt(ZBuffer *zb, ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2);
/* s31: zline.c, GL_LESS lines and points */
void ZB_line_z_lt(ZBuffer *zb, ZBufferPoint *p1, ZBufferPoint *p2);
void ZB_plot_lt(ZBuffer *zb, ZBufferPoint *p);
/* s31: glClear inside a rectangle (scissor), with colour and depth masks */
void ZB_clear_rect(ZBuffer *zb, int x0, int y0, int x1, int y1,
                   int clear_z, int z, int clear_color, int color, int cmask);

typedef void (*ZB_fillTriangleFunc)(ZBuffer  *,
	    ZBufferPoint *,ZBufferPoint *,ZBufferPoint *);

/* memory.c */
void gl_free(void *p);
void *gl_malloc(int size);
void *gl_zalloc(int size);

#endif /* _tgl_zbuffer_h_ */
