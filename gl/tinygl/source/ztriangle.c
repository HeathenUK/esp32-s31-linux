#include <stdlib.h>
#include "zbuffer.h"
#include "zpipe.h"
#include "ztri.h"

/* s31: ztriangle_nt.c / ztriangle_nw.c include this file again with these
   redefined, to make the depth-test-off and depth-mask-off fillers without
   a per-pixel branch in the default ones */
#ifndef ZCMP
#define ZCMP(z,zpix) ((z) >= (zpix))
#endif
#ifndef ZWRITE
#define ZWRITE(d,v) ((d)=(v))
#endif
#ifndef ZFN
#define ZFN(n) n
#endif

void ZFN(ZB_fillTriangleFlat)(ZBuffer *zb,
			 ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
#if TGL_FEATURE_RENDER_BITS == 24
    unsigned char colorR, colorG, colorB;
#else
    int color;
#endif

#define INTERP_Z

#if TGL_FEATURE_RENDER_BITS == 24 

#define DRAW_INIT()				\
{						\
  colorR=p2->r>>8; \
  colorG=p2->g>>8; \
  colorB=p2->b>>8; \
}

#define PUT_PIXEL(_a)		\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      pp[3 * _a]=colorR;\
      pp[3 * _a + 1]=colorG;\
      pp[3 * _a + 2]=colorB;\
      ZWRITE(pz[_a],zz);				\
    }\
    z+=dzdx;					\
}

#else

#define DRAW_INIT()				\
{						\
  color=zb->flat_color;	/* s31: clip.c */	\
}
  
#define PUT_PIXEL(_a)				\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      pp[_a]=color;				\
      ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
}
#endif /* TGL_FEATURE_RENDER_BITS == 24 */

#include "ztriangle.h"
}

/*
 * Smooth filled triangle.
 * The code below is very tricky :)
 */

void ZFN(ZB_fillTriangleSmooth)(ZBuffer *zb,
			   ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
#if TGL_FEATURE_RENDER_BITS == 16
        int _drgbdx;
#endif

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v,n) (v / (1<<n))

#if TGL_FEATURE_RENDER_BITS == 24

#define DRAW_INIT() 				\
{						\
}

#define PUT_PIXEL(_a)				\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      pp[3 * _a]=or1 >> 8;\
      pp[3 * _a + 1]=og1 >> 8;\
      pp[3 * _a + 2]=ob1 >> 8;\
      ZWRITE(pz[_a],zz);				\
    }\
    z+=dzdx;					\
    og1+=dgdx;					\
    or1+=drdx;					\
    ob1+=dbdx;					\
}

#elif TGL_FEATURE_RENDER_BITS == 16

/* s31: the packed-colour shifts are done unsigned (negative deltas and
   r1 << 16 overflowed int: UBSan); same bits, same code */
#define DRAW_INIT() 				\
{						\
  _drgbdx=((unsigned int)SAR_RND_TO_ZERO(drdx,6) << 22) & 0xFFC00000;		\
  _drgbdx|=SAR_RND_TO_ZERO(dgdx,5) & 0x000007FF;		\
  _drgbdx|=((unsigned int)SAR_RND_TO_ZERO(dbdx,7) << 12) & 0x001FF000; 	\
}


#define PUT_PIXEL(_a)				\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      tmp=rgb & 0xF81F07E0;			\
      pp[_a]=tmp | (tmp >> 16);			\
      ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    rgb=(rgb+drgbdx) & ( ~ 0x00200800);		\
}

/* s31 (phase 3a): two pixels a turn, the odd one first, the end a
   pointer (see the generic line in ztriangle.h: spans are short, so the
   span's overhead is what costs); x2 is the last pixel */
#define DRAW_LINE()							   \
{									   \
  unsigned short *pz;							   \
  PIXEL *pp, *ppe;							   \
  unsigned int tmp,z,zz,rgb,drgbdx;					   \
  pp=pp1+x1;								   \
  ppe=pp1+x2+1;								   \
  pz=pz1+x1;								   \
  z=z1;									   \
  rgb=((unsigned int)r1 << 16) & 0xFFC00000;				   \
  rgb|=(g1 >> 5) & 0x000007FF;						   \
  rgb|=((unsigned int)b1 << 5) & 0x001FF000;				   \
  drgbdx=_drgbdx;							   \
  if (!((x2 - x1) & 1)) {						   \
    PUT_PIXEL(0);							   \
    pz+=1;								   \
    pp+=1;								   \
  }									   \
  while (pp != ppe) {							   \
    PUT_PIXEL(0);							   \
    PUT_PIXEL(1);							   \
    pz+=2;								   \
    pp+=2;								   \
  }									   \
}

#else

#define DRAW_INIT() 				\
{						\
}

#define PUT_PIXEL(_a)				\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);\
      ZWRITE(pz[_a],zz);				\
    }\
    z+=dzdx;					\
    og1+=dgdx;					\
    or1+=drdx;					\
    ob1+=dbdx;					\
}

#endif /* TGL_FEATURE_RENDER_BITS */

#include "ztriangle.h"
}

#if TGL_FEATURE_RENDER_BITS == 16 && !defined(ZTRI_NO_PERSP)
/*
 * s31 (phase 4 F-PERSP, s31_tfilter.c): the smooth filler with the colour
 * interpolated perspective-correctly - r/w, g/w, b/w over 1/w divided every
 * NB_INTERP pixels (the textured filler's subdivision) and stepped
 * linearly in between, into the smooth filler's packed colour and its
 * PUT_PIXEL. Only triangles whose w values and colours differ enough reach
 * it (gl_draw_triangle_fill_pq); every other smooth triangle keeps
 * ZB_fillTriangleSmooth. The colour at a pixel lies between the vertices'
 * (up to float rounding), so the packed fields keep their guard bits.
 */
void ZFN(ZB_fillTriangleSmoothPersp)(ZBuffer *zb,
                                     ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
    float ndqdx, ndrqdx, ndgqdx, ndbqdx;

#define INTERP_Z
#define INTERP_PRGB

#define NB_INTERP 8

#define DRAW_INIT()				\
{						\
  ndqdx=NB_INTERP * dqdx;			\
  ndrqdx=NB_INTERP * drqdx;			\
  ndgqdx=NB_INTERP * dgqdx;			\
  ndbqdx=NB_INTERP * dbqdx;			\
}

#define PUT_PIXEL(_a)				\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      tmp=rgb & 0xF81F07E0;			\
      pp[_a]=tmp | (tmp >> 16);			\
      ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    rgb=(rgb+drgbdx) & ( ~ 0x00200800);		\
}

/* the packed colour at the segment's first pixel and its per-pixel step
   towards the colour at pixel m further on (the chord: exact at both ends,
   so the error of the linear steps between is an eighth of what stepping
   the start's derivative gives - 3 steps of green at the far end of an
   8-pixel segment with w 1 -> 4, filt_test) */
#define PC_SEGMENT(m)						      \
{								      \
  float e_=1.0f / (fz + (float)(m) * dqdx);			      \
  float r1_=(rq + (float)(m) * drqdx) * e_;			      \
  float g1_=(gq + (float)(m) * dgqdx) * e_;			      \
  float b1_=(bq + (float)(m) * dbqdx) * e_;			      \
  float k_=(m) > 0 ? 1.0f / (float)(m) : 0.0f;			      \
  int ri_=(int)r0, gi_=(int)g0, bi_=(int)b0;			      \
  int dr_=(int)((r1_ - r0) * k_);				      \
  int dg_=(int)((g1_ - g0) * k_);				      \
  int db_=(int)((b1_ - b0) * k_);				      \
  rgb=((unsigned int)ri_ << 16) & 0xFFC00000;			      \
  rgb|=(gi_ >> 5) & 0x000007FF;					      \
  rgb|=((unsigned int)bi_ << 5) & 0x001FF000;			      \
  drgbdx=((unsigned int)SAR_RND_TO_ZERO(dr_,6) << 22) & 0xFFC00000;   \
  drgbdx|=SAR_RND_TO_ZERO(dg_,5) & 0x000007FF;			      \
  drgbdx|=((unsigned int)SAR_RND_TO_ZERO(db_,7) << 12) & 0x001FF000;  \
  r0=r1_; g0=g1_; b0=b1_;					      \
}

#define DRAW_LINE()				\
{						\
  unsigned short *pz;				\
  PIXEL *pp;					\
  unsigned int tmp,z,zz,rgb,drgbdx;		\
  int n;					\
  float fz=q1, rq=rq1, gq=gq1, bq=bq1;		\
  float s_=1.0f / fz, r0=rq * s_, g0=gq * s_, b0=bq * s_;	\
  n=x2-x1;					\
  pp=pp1+x1;					\
  pz=pz1+x1;					\
  z=z1;						\
  while (n>=NB_INTERP) {			\
    PC_SEGMENT(NB_INTERP);			\
    PUT_PIXEL(0);				\
    PUT_PIXEL(1);				\
    PUT_PIXEL(2);				\
    PUT_PIXEL(3);				\
    PUT_PIXEL(4);				\
    PUT_PIXEL(5);				\
    PUT_PIXEL(6);				\
    PUT_PIXEL(7);				\
    pz+=NB_INTERP;				\
    pp+=NB_INTERP;				\
    n-=NB_INTERP;				\
    fz+=ndqdx; rq+=ndrqdx; gq+=ndgqdx; bq+=ndbqdx;	\
  }						\
  /* the last 1 to 8 pixels: the chord to the last one */	\
  PC_SEGMENT(n);				\
  while (n>=0) {				\
    PUT_PIXEL(0);				\
    pz+=1;					\
    pp+=1;					\
    n-=1;					\
  }						\
}

#include "ztriangle.h"
#undef PC_SEGMENT
#undef NB_INTERP
}
#endif

#if TGL_FEATURE_RENDER_BITS == 16 && !defined(ZTRI_NO_LONG)
/*
 * s31 (phase 4, s31_tfilter.c): the smooth filler for long spans of an
 * affine (equal-w) triangle. TinyGL's packed colour steps with the gradient
 * truncated (1/32 of a stored step of red and green, 1/16 of blue), which
 * accumulates along a span; here the packed colour is re-made every
 * NB_INTERP pixels from the exact integer planes (r1 + k drdx ...), so the
 * error stays below 8 truncated steps. PUT_PIXEL is the smooth filler's.
 */
void ZFN(ZB_fillTriangleSmoothLong)(ZBuffer *zb,
                                    ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
    int _drgbdx, ndrdx, ndgdx, ndbdx;

#define INTERP_Z
#define INTERP_RGB

#define NB_INTERP 8

#define DRAW_INIT() 				\
{						\
  _drgbdx=((unsigned int)SAR_RND_TO_ZERO(drdx,6) << 22) & 0xFFC00000;		\
  _drgbdx|=SAR_RND_TO_ZERO(dgdx,5) & 0x000007FF;		\
  _drgbdx|=((unsigned int)SAR_RND_TO_ZERO(dbdx,7) << 12) & 0x001FF000; 	\
  ndrdx=NB_INTERP * drdx; ndgdx=NB_INTERP * dgdx; ndbdx=NB_INTERP * dbdx;	\
}

#define PUT_PIXEL(_a)				\
{						\
    zz=z >> ZB_POINT_Z_FRAC_BITS;		\
    if (ZCMP(zz,pz[_a])) {				\
      tmp=rgb & 0xF81F07E0;			\
      pp[_a]=tmp | (tmp >> 16);			\
      ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    rgb=(rgb+drgbdx) & ( ~ 0x00200800);		\
}

#define LONG_PACK()						\
{								\
  rgb=((unsigned int)r_ << 16) & 0xFFC00000;			\
  rgb|=(g_ >> 5) & 0x000007FF;					\
  rgb|=((unsigned int)b_ << 5) & 0x001FF000;			\
}

#define DRAW_LINE()				\
{						\
  unsigned short *pz;				\
  PIXEL *pp;					\
  unsigned int tmp,z,zz,rgb,drgbdx=_drgbdx;	\
  int n, r_=r1, g_=g1, b_=b1;			\
  n=x2-x1;					\
  pp=pp1+x1;					\
  pz=pz1+x1;					\
  z=z1;						\
  while (n>=(NB_INTERP-1)) {			\
    LONG_PACK();				\
    PUT_PIXEL(0);				\
    PUT_PIXEL(1);				\
    PUT_PIXEL(2);				\
    PUT_PIXEL(3);				\
    PUT_PIXEL(4);				\
    PUT_PIXEL(5);				\
    PUT_PIXEL(6);				\
    PUT_PIXEL(7);				\
    pz+=NB_INTERP;				\
    pp+=NB_INTERP;				\
    n-=NB_INTERP;				\
    r_+=ndrdx; g_+=ndgdx; b_+=ndbdx;		\
  }						\
  LONG_PACK();					\
  while (n>=0) {				\
    PUT_PIXEL(0);				\
    pz+=1;					\
    pp+=1;					\
    n-=1;					\
  }						\
}

#include "ztriangle.h"
#undef LONG_PACK
#undef NB_INTERP
}
#endif

#ifndef ZTRI_VARIANT
void ZB_setTexture(ZBuffer *zb,PIXEL *texture)
{
    zb->current_texture=texture;
}
#endif

/*
 * Texture mapping with perspective correction.
 * We use the gradient method to make less divisions.
 * TODO: pipeline the division
 */
#if 1

void ZFN(ZB_fillTriangleMappingPerspective)(ZBuffer *zb,
                            ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
    PIXEL *texture;
    float fdzdx,fndzdx,ndszdx,ndtzdx;
    /* s31: the texture's own size - masks and shift instead of TinyGL's
       fixed 256x256 constants, the same operations per pixel (zbuffer.h) */
    unsigned int tsmask, ttmask, tshift;

/* s31: divide by q = 1/w, not by window z. Window z is affine in 1/w
   with a constant term, so TinyGL's s*z/z was close to an affine mapping
   on distant surfaces; s*q/q is GL's perspective-correct one (3.8). */
#define INTERP_Z
#define INTERP_STZ

#define NB_INTERP 8

#define DRAW_INIT()				\
{						\
  texture=zb->current_texture;\
  tsmask=zb->tex_smask; ttmask=zb->tex_tmask; tshift=zb->tex_shift;\
  fdzdx=dqdx;\
  fndzdx=NB_INTERP * fdzdx;\
  ndszdx=NB_INTERP * dszdx;\
  ndtzdx=NB_INTERP * dtzdx;\
}


#if TGL_FEATURE_RENDER_BITS == 24

#define PUT_PIXEL(_a)				\
{						\
   unsigned char *ptr;\
   zz=z >> ZB_POINT_Z_FRAC_BITS;		\
     if (ZCMP(zz,pz[_a])) {				\
       ptr = texture + (((t & 0x3FC00000) | (s & 0x003FC000)) >> 14) * 3;\
       pp[3 * _a]= ptr[0];\
       pp[3 * _a + 1]= ptr[1];\
       pp[3 * _a + 2]= ptr[2];\
       ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    s+=dsdx;					\
    t+=dtdx;					\
}

#else

#define PUT_PIXEL(_a)				\
{						\
   zz=z >> ZB_POINT_Z_FRAC_BITS;		\
     if (ZCMP(zz,pz[_a])) {				\
       pp[_a]=*(PIXEL *)((char *)texture+ \
               (((t & ttmask) | (s & tsmask)) >> tshift));\
       ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    s+=dsdx;					\
    t+=dtdx;					\
}

#endif

#define DRAW_LINE()				\
{						\
  register unsigned short *pz;		\
  register PIXEL *pp;		\
  register unsigned int s,t,z,zz;	\
  register int n,dsdx,dtdx;		\
  float sz,tz,fz,zinv; \
  n=x2-x1;                             \
  fz=q1;\
  zinv=1.0f / fz;\
  pp=(PIXEL *)((char *)pp1 + x1 * PSZB); \
  pz=pz1+x1;					\
  z=z1;						\
  sz=sz1;\
  tz=tz1;\
  while (n>=(NB_INTERP-1)) {						   \
    {\
      float ss,tt;\
      ss=(sz * zinv);\
      tt=(tz * zinv);\
      s=(int) ss;\
      t=(int) tt;\
      dsdx= (int)( (dszdx - ss*fdzdx)*zinv );\
      dtdx= (int)( (dtzdx - tt*fdzdx)*zinv );\
      fz+=fndzdx;\
      zinv=1.0f / fz;\
    }\
    PUT_PIXEL(0);							   \
    PUT_PIXEL(1);							   \
    PUT_PIXEL(2);							   \
    PUT_PIXEL(3);							   \
    PUT_PIXEL(4);							   \
    PUT_PIXEL(5);							   \
    PUT_PIXEL(6);							   \
    PUT_PIXEL(7);							   \
    pz+=NB_INTERP;							   \
    pp=(PIXEL *)((char *)pp + NB_INTERP * PSZB);\
    n-=NB_INTERP;							   \
    sz+=ndszdx;\
    tz+=ndtzdx;\
  }									   \
    {\
      float ss,tt;\
      ss=(sz * zinv);\
      tt=(tz * zinv);\
      s=(int) ss;\
      t=(int) tt;\
      dsdx= (int)( (dszdx - ss*fdzdx)*zinv );\
      dtdx= (int)( (dtzdx - tt*fdzdx)*zinv );\
    }\
  while (n>=0) {							   \
    PUT_PIXEL(0);							   \
    pz+=1;								   \
    pp=(PIXEL *)((char *)pp + PSZB);\
    n-=1;								   \
  }									   \
}
  
#include "ztriangle.h"
}

#endif

