#include <stdlib.h>
#include "zbuffer.h"

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
  color=RGB_TO_PIXEL(p2->r,p2->g,p2->b);	\
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

#define DRAW_LINE()							   \
{									   \
  register unsigned short *pz;					   \
  register PIXEL *pp;					   \
  register unsigned int tmp,z,zz,rgb,drgbdx;				   \
  register int n;							   \
  n=(x2 >> 16) - x1;							   \
  pp=pp1+x1;								   \
  pz=pz1+x1;								   \
  z=z1;									   \
  rgb=((unsigned int)r1 << 16) & 0xFFC00000;						   \
  rgb|=(g1 >> 5) & 0x000007FF;						   \
  rgb|=((unsigned int)b1 << 5) & 0x001FF000;						   \
  drgbdx=_drgbdx;							   \
  while (n>=3) {							   \
    PUT_PIXEL(0);							   \
    PUT_PIXEL(1);							   \
    PUT_PIXEL(2);							   \
    PUT_PIXEL(3);							   \
    pz+=4;								   \
    pp+=4;								   \
    n-=4;								   \
  }									   \
  while (n>=0) {							   \
    PUT_PIXEL(0);							   \
    pz+=1;								   \
    pp+=1;								   \
    n-=1;								   \
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

#ifndef ZTRI_VARIANT
void ZB_setTexture(ZBuffer *zb,PIXEL *texture)
{
    zb->current_texture=texture;
}
#endif

void ZFN(ZB_fillTriangleMapping)(ZBuffer *zb,
			    ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
    PIXEL *texture;

#define INTERP_Z
#define INTERP_ST

#define DRAW_INIT()				\
{						\
  texture=zb->current_texture;			\
}

#if TGL_FEATURE_RENDER_BITS == 24

#define PUT_PIXEL(_a)				\
{						\
   unsigned char *ptr;\
   zz=z >> ZB_POINT_Z_FRAC_BITS;		\
     if (ZCMP(zz,pz[_a])) {				\
       ptr = texture + (((t & 0x3FC00000) | s) >> 14) * 3; \
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
       pp[_a]=texture[((t & 0x3FC00000) | s) >> 14];	\
       ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    s+=dsdx;					\
    t+=dtdx;					\
}

#endif

#include "ztriangle.h"
}

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

#define INTERP_Z
#define INTERP_STZ

#define NB_INTERP 8

#define DRAW_INIT()				\
{						\
  texture=zb->current_texture;\
  fdzdx=(float)dzdx;\
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
               (((t & 0x3FC00000) | (s & 0x003FC000)) >> (17 - PSZSH)));\
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
  n=(x2>>16)-x1;                             \
  fz=(float)z1;\
  zinv=1.0 / fz;\
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
      zinv=1.0 / fz;\
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

#if 0

/* slow but exact version (only there for reference, incorrect for 24
   bits) */

void ZFN(ZB_fillTriangleMappingPerspective)(ZBuffer *zb,
                            ZBufferPoint *p0,ZBufferPoint *p1,ZBufferPoint *p2)
{
    PIXEL *texture;

#define INTERP_Z
#define INTERP_STZ

#define DRAW_INIT()				\
{						\
  texture=zb->current_texture;			\
}

#define PUT_PIXEL(_a)				\
{						\
   float zinv; \
   int s,t; \
   zz=z >> ZB_POINT_Z_FRAC_BITS;		\
     if (ZCMP(zz,pz[_a])) {				\
       zinv= 1.0 / (float) z; \
       s= (int) (sz * zinv); \
       t= (int) (tz * zinv); \
       pp[_a]=texture[((t & 0x3FC00000) | s) >> 14];	\
       ZWRITE(pz[_a],zz);				\
    }						\
    z+=dzdx;					\
    sz+=dszdx;					\
    tz+=dtzdx;					\
}

#include "ztriangle.h"
}


#endif
