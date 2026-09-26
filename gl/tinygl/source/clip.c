#include "zgl.h"
#include "ztri.h"
#include "s31_ttv.h"

/* fill triangle profile */
/* #define PROFILE */

#define CLIP_XMIN   (1<<0)
#define CLIP_XMAX   (1<<1)
#define CLIP_YMIN   (1<<2)
#define CLIP_YMAX   (1<<3)
#define CLIP_ZMIN   (1<<4)
#define CLIP_ZMAX   (1<<5)

/* s31 (phase 6 V1): the body is s31_ttv.h gl_ttv, inlined into the
   vertex path (vertex.c); this copy serves the clipper (updateTmp) */
void gl_transform_to_viewport(GLContext *c,GLVertex *v)
{
  gl_ttv(c,v);
}


static void gl_add_select1(GLContext *c,int z1,int z2,int z3)
{
  unsigned int min,max;
  min=max=z1;
  if (z2<min) min=z2;
  if (z3<min) min=z3;
  if (z2>max) max=z2;
  if (z3>max) max=z3;

  gl_add_select(c,0xffffffff-min,0xffffffff-max);
}

/* point */

/* s31: a size-1 point of tier 1 (ZB_plot writes without a test): only
   inside the box - a point on the right or bottom edge of the viewport is
   one pixel past it */
static void zb_plot_in(GLContext *c,ZBufferPoint *p)
{
  const int *b=c->pipe.box;
  /* phase 3a G03 (s31_zepoch.c): stored depth is plus the epoch's base */
  if (p->z < c->zb->zguard) zep_materialise(c);
  if (c->pipe.bact) zdb_grow(c,p->x,p->y,p->x+1,p->y+1);   /* dirty box */
  if (p->x >= b[0] && p->x < b[2] && p->y >= b[1] && p->y < b[3]) {
    unsigned int off = zep_prim(c,(unsigned int)p->z);
    if (off) {
      ZBufferPoint q=*p;
      q.z += (int)off;
      c->zb_plot(c->zb,&q);
    } else {
      c->zb_plot(c->zb,p);
    }
  }
}

void gl_draw_point(GLContext *c,GLVertex *p0)
{
  S31T_SYNC(c);                  /* phase 6 (s31_thr.h) */
  if (p0->clip_code == 0) {
    if (c->render_mode == GL_SELECT) {
      gl_add_select(c,p0->zp.z,p0->zp.z);
    } else if (c->raster_gen_points) {
      if (!c->raster_skip) gl_general_point(c,p0);   /* s31 */
    } else {
      zb_plot_in(c,&p0->zp);   /* s31: raster.c, for the depth state */
    }
  }
}

/* line */

static inline void interpolate(GLVertex *q,GLVertex *p0,GLVertex *p1,float t)
{
  q->pc.X=p0->pc.X+(p1->pc.X-p0->pc.X)*t;
  q->pc.Y=p0->pc.Y+(p1->pc.Y-p0->pc.Y)*t;
  q->pc.Z=p0->pc.Z+(p1->pc.Z-p0->pc.Z)*t;
  q->pc.W=p0->pc.W+(p1->pc.W-p0->pc.W)*t;

  q->color.v[0]=p0->color.v[0] + (p1->color.v[0]-p0->color.v[0])*t;
  q->color.v[1]=p0->color.v[1] + (p1->color.v[1]-p0->color.v[1])*t;
  q->color.v[2]=p0->color.v[2] + (p1->color.v[2]-p0->color.v[2])*t;
  /* s31: alpha, fog and texture coordinates of a clipped line end */
  q->color.v[3]=p0->color.v[3] + (p1->color.v[3]-p0->color.v[3])*t;
  q->fog=p0->fog + (p1->fog-p0->fog)*t;
  q->tex_coord.X=p0->tex_coord.X + (p1->tex_coord.X-p0->tex_coord.X)*t;
  q->tex_coord.Y=p0->tex_coord.Y + (p1->tex_coord.Y-p0->tex_coord.Y)*t;
  q->tex_coord.W=p0->tex_coord.W + (p1->tex_coord.W-p0->tex_coord.W)*t;
  q->spec.X=p0->spec.X + (p1->spec.X-p0->spec.X)*t;
  q->spec.Y=p0->spec.Y + (p1->spec.Y-p0->spec.Y)*t;
  q->spec.Z=p0->spec.Z + (p1->spec.Z-p0->spec.Z)*t;
  /* s31 (phase 5 O1): texture unit 1's (read only while it is on) */
  q->tex_coord1.X=p0->tex_coord1.X + (p1->tex_coord1.X-p0->tex_coord1.X)*t;
  q->tex_coord1.Y=p0->tex_coord1.Y + (p1->tex_coord1.Y-p0->tex_coord1.Y)*t;
  q->tex_coord1.W=p0->tex_coord1.W + (p1->tex_coord1.W-p0->tex_coord1.W)*t;
}

/*
 * Line Clipping 
 */

/* Line Clipping algorithm from 'Computer Graphics', Principles and
   Practice */
static inline int ClipLine1(float denom,float num,float *tmin,float *tmax)
{
  float t;
	 
  if (denom>0) {
    t=num/denom;
    if (t>*tmax) return 0;
    if (t>*tmin) *tmin=t;
  } else if (denom<0) {
    t=num/denom;
    if (t<*tmin) return 0;
    if (t<*tmax) *tmax=t;
  } else if (num>0) return 0;
  return 1;
}

/* s31 (plan F7): the user clip planes, in clip space (s31_xform.c):
   d(t) = d1 + t (d2 - d1) >= 0 */
static int clip_line_user(GLContext *c,GLVertex *p1,GLVertex *p2,
                          float *tmin,float *tmax)
{
  int i;
  for (i = 0; i < 6; i++) {
    const float *q = c->clip_plane_clip[i].v;
    float d1, d2;
    if (!(c->clip_plane_mask & (1 << i))) continue;
    d1 = q[0]*p1->pc.X + q[1]*p1->pc.Y + q[2]*p1->pc.Z + q[3]*p1->pc.W;
    d2 = q[0]*p2->pc.X + q[1]*p2->pc.Y + q[2]*p2->pc.Z + q[3]*p2->pc.W;
    if (!ClipLine1(d2 - d1, -d1, tmin, tmax)) return 0;
  }
  return 1;
}

/* s31: out of line: 9 call sites in vertex.c, and code size is time in XIP */
void gl_set_provoking_flat(GLContext *c, GLVertex *v)
{
  ZBufferPoint t;
  gl_zp_color(&t, &v->color);
  c->flat_r = t.r; c->flat_g = t.g; c->flat_b = t.b;
  c->flat_vtx = v;   /* s31: its alpha, for the general path (raster.c) */
  c->flat_ok = 0;    /* phase 6 V5: set_flat once per provoking vertex */
}

/* s31: GL_FLAT lines take the provoking vertex's colour (gl_set_provoking);
   TinyGL interpolated whenever the two ends differed */
static void gl_zb_line(GLContext *c,GLVertex *va,GLVertex *vb)
{
  ZBufferPoint fa,fb,*a=&va->zp,*b=&vb->zp;
  /* phase 3a G03 (s31_zepoch.c): a line that can reach the depth epoch's
     farthest step under GL_LESS */
  unsigned int off;
  if ((a->z < b->z ? a->z : b->z) < c->zb->zguard) zep_materialise(c);
  if (c->pipe.bact) {
    /* the dirty box (s31_zepoch.c): the ends, and the width either side */
    int w=c->line_w;
    zdb_grow(c,(a->x < b->x ? a->x : b->x)-w,(a->y < b->y ? a->y : b->y)-w,
             (a->x > b->x ? a->x : b->x)+w+1,(a->y > b->y ? a->y : b->y)+w+1);
  }
  off=zep_prim(c,(unsigned int)(a->z > b->z ? a->z : b->z));
  if (c->raster_gen_lines) {
    /* s31: blending, fog, depth func/mask, texture, width ... */
    if (!c->raster_skip)
      gl_general_line(c,va,vb,c->current_shade_model != GL_SMOOTH);
    return;
  }
  {
    /* s31: TinyGL's line writes every pixel between the ends without a
       test: an end one past the box (on the right or bottom clip plane,
       clip.c gl_transform_to_viewport) is moved onto its last pixel */
    const int *bx=c->pipe.box;
    int out=a->x < bx[0] || a->x >= bx[2] || a->y < bx[1] || a->y >= bx[3] ||
            b->x < bx[0] || b->x >= bx[2] || b->y < bx[1] || b->y >= bx[3];
    if (out || off || c->current_shade_model != GL_SMOOTH) {
      fa=*a; fb=*b;
      a=&fa; b=&fb;
      fa.z+=(int)off; fb.z+=(int)off;   /* the epoch's base (s31_zepoch.c) */
    }
    if (out) {
      fa.x=fa.x < bx[0] ? bx[0] : (fa.x >= bx[2] ? bx[2]-1 : fa.x);
      fa.y=fa.y < bx[1] ? bx[1] : (fa.y >= bx[3] ? bx[3]-1 : fa.y);
      fb.x=fb.x < bx[0] ? bx[0] : (fb.x >= bx[2] ? bx[2]-1 : fb.x);
      fb.y=fb.y < bx[1] ? bx[1] : (fb.y >= bx[3] ? bx[3]-1 : fb.y);
    }
  }
  if (c->current_shade_model != GL_SMOOTH) {
    fa.r=fb.r=c->flat_r; fa.g=fb.g=c->flat_g; fa.b=fb.b=c->flat_b;
  }
  c->zb_line(c->zb,a,b);   /* s31: raster.c, for the depth state */
}

void gl_draw_line(GLContext *c,GLVertex *p1,GLVertex *p2)
{
  float dx,dy,dz,dw,x1,y1,z1,w1;
  float tmin,tmax;
  GLVertex q1,q2;

  S31T_SYNC(c);                  /* phase 6 (s31_thr.h) */
  int cc1,cc2;
  
  cc1=p1->clip_code;
  cc2=p2->clip_code;

  if ( (cc1 | cc2) == 0) {
    if (c->render_mode == GL_SELECT) {
      gl_add_select1(c,p1->zp.z,p2->zp.z,p2->zp.z);
    } else {
      gl_zb_line(c,p1,p2);
    }
  } else if ( (cc1&cc2) != 0 ) {
    return;
  } else {
    dx=p2->pc.X-p1->pc.X;
    dy=p2->pc.Y-p1->pc.Y;
    dz=p2->pc.Z-p1->pc.Z;
    dw=p2->pc.W-p1->pc.W;
    x1=p1->pc.X;
    y1=p1->pc.Y;
    z1=p1->pc.Z;
    w1=p1->pc.W;
    
    tmin=0;
    tmax=1;
    if (ClipLine1(dx+dw,-x1-w1,&tmin,&tmax) &&
        ClipLine1(-dx+dw,x1-w1,&tmin,&tmax) &&
        ClipLine1(dy+dw,-y1-w1,&tmin,&tmax) &&
        ClipLine1(-dy+dw,y1-w1,&tmin,&tmax) &&
        ClipLine1(dz+dw,-z1-w1,&tmin,&tmax) && 
        ClipLine1(-dz+dw,z1-w1,&tmin,&tmax) &&
        (c->clip_plane_mask == 0 || clip_line_user(c,p1,p2,&tmin,&tmax))) {

      interpolate(&q1,p1,p2,tmin);
      interpolate(&q2,p1,p2,tmax);
      gl_transform_to_viewport(c,&q1);
      gl_transform_to_viewport(c,&q2);
      /* s31: without lighting gl_transform_to_viewport takes the CURRENT
         colour, which is not this interpolated vertex's */
      if (!c->lighting_enabled) {
        gl_zp_color(&q1.zp,&q1.color);
        gl_zp_color(&q2.zp,&q2.color);
      }
      if (c->render_mode == GL_SELECT)
        gl_add_select1(c,q1.zp.z,q2.zp.z,q2.zp.z);
      else
        gl_zb_line(c,&q1,&q2);
    }
  }
}

	 
/* triangle */

/*
 * Clipping
 */

/* We clip the segment [a,b] against the 6 planes of the normal volume.
 * We compute the point 'c' of intersection and the value of the parameter 't'
 * of the intersection if x=a+t(b-a). 
 */
	 
#define clip_func(name,sign,dir,dir1,dir2) \
static float name(V4 *c,V4 *a,V4 *b) \
{\
  float t,dX,dY,dZ,dW,den;\
  dX = (b->X - a->X);\
  dY = (b->Y - a->Y);\
  dZ = (b->Z - a->Z);\
  dW = (b->W - a->W);\
  den = -(sign d ## dir) + dW;\
  if (den == 0) t=0;\
  else t = ( sign a->dir - a->W) / den;\
  c->dir1 = a->dir1 + t * d ## dir1;\
  c->dir2 = a->dir2 + t * d ## dir2;\
  c->W = a->W + t * dW;\
  c->dir = sign c->W;\
  return t;\
}


clip_func(clip_xmin,-,X,Y,Z)

clip_func(clip_xmax,+,X,Y,Z)

clip_func(clip_ymin,-,Y,X,Z)

clip_func(clip_ymax,+,Y,X,Z)

clip_func(clip_zmin,-,Z,X,Y)

clip_func(clip_zmax,+,Z,X,Y)


float (*clip_proc[6])(V4 *,V4 *,V4 *)=  {
    clip_xmin,clip_xmax,
    clip_ymin,clip_ymax,
    clip_zmin,clip_zmax
};

/* s31 (plan F7): the intersection with user plane k (clip space) */
static float clip_user(GLContext *c,int k,V4 *out,V4 *a,V4 *b)
{
  const float *q = c->clip_plane_clip[k].v;
  float da = q[0]*a->X + q[1]*a->Y + q[2]*a->Z + q[3]*a->W;
  float db = q[0]*b->X + q[1]*b->Y + q[2]*b->Z + q[3]*b->W;
  float t = da != db ? da / (da - db) : 0.0f;
  int i;
  for (i = 0; i < 4; i++) out->v[i] = a->v[i] + t * (b->v[i] - a->v[i]);
  return t;
}

/* done: the user plane just clipped against, whose bit the new vertex
   must not carry (it lies on the plane; rounding could say outside) */
static inline void updateTmp(GLContext *c,
			     GLVertex *q,GLVertex *p0,GLVertex *p1,float t,int done)
{
  if (c->current_shade_model == GL_SMOOTH) {
    q->color.v[0]=p0->color.v[0] + (p1->color.v[0]-p0->color.v[0])*t;
    q->color.v[1]=p0->color.v[1] + (p1->color.v[1]-p0->color.v[1])*t;
    q->color.v[2]=p0->color.v[2] + (p1->color.v[2]-p0->color.v[2])*t;
  } else {
    q->color.v[0]=p0->color.v[0];
    q->color.v[1]=p0->color.v[1];
    q->color.v[2]=p0->color.v[2];
  }
  if (c->lighting_enabled && c->light_model_two_side) {
    /* s31: the back colours too (gl_draw_triangle_twoside) */
    int k;
    for (k = 0; k < 4; k++)
      q->color_back.v[k]=p0->color_back.v[k] + (p1->color_back.v[k]-p0->color_back.v[k])*t;
    q->spec_back.X=p0->spec_back.X + (p1->spec_back.X-p0->spec_back.X)*t;
    q->spec_back.Y=p0->spec_back.Y + (p1->spec_back.Y-p0->spec_back.Y)*t;
    q->spec_back.Z=p0->spec_back.Z + (p1->spec_back.Z-p0->spec_back.Z)*t;
  }
  if (c->raster_need_attr) {
    /* s31: alpha (flat or not: GL_FLAT takes the provoking vertex's in
       the filler) and the fog factor */
    q->color.v[3]=p0->color.v[3] + (p1->color.v[3]-p0->color.v[3])*t;
    q->fog=p0->fog + (p1->fog-p0->fog)*t;
    if (c->raster_sepspec) {
      q->spec.X=p0->spec.X + (p1->spec.X-p0->spec.X)*t;
      q->spec.Y=p0->spec.Y + (p1->spec.Y-p0->spec.Y)*t;
      q->spec.Z=p0->spec.Z + (p1->spec.Z-p0->spec.Z)*t;
    }
  }

  if (c->texture_2d_enabled) {
    q->tex_coord.X=p0->tex_coord.X + (p1->tex_coord.X-p0->tex_coord.X)*t;
    q->tex_coord.Y=p0->tex_coord.Y + (p1->tex_coord.Y-p0->tex_coord.Y)*t;
    /* s31: q too (gl_transform_to_viewport divides by it) */
    q->tex_coord.W=p0->tex_coord.W + (p1->tex_coord.W-p0->tex_coord.W)*t;
  }
  if (c->vtx_extra & 4) {
    /* s31 (phase 5 O1): texture unit 1's, likewise (vtx_extra bit 2 is
       tu1_on, and near the context pointer) */
    q->tex_coord1.X=p0->tex_coord1.X + (p1->tex_coord1.X-p0->tex_coord1.X)*t;
    q->tex_coord1.Y=p0->tex_coord1.Y + (p1->tex_coord1.Y-p0->tex_coord1.Y)*t;
    q->tex_coord1.W=p0->tex_coord1.W + (p1->tex_coord1.W-p0->tex_coord1.W)*t;
  }

  q->clip_code=gl_clipcode(q->pc.X,q->pc.Y,q->pc.Z,q->pc.W);
  if (c->clip_plane_mask)
    q->clip_code |= gl_user_clipcode(c,&q->pc) & ~done;
  if (q->clip_code==0) {
    const int *b=c->pipe.box;
    gl_transform_to_viewport(c,q);
    /* s31: a vertex the clipper made lies on a plane of the clip volume,
       which maps to the edge of pipe.box, up to the rounding of the
       intersection - which grows with the far vertex's magnitude. The
       triangle fillers (ztri.h) rely on every vertex being inside the box,
       so clamp it there: exact for the vertices the clipper passed whole,
       and only these can stray */
    q->zp.fx=fminf(fmaxf(q->zp.fx,(float)b[0]),(float)b[2]);
    q->zp.fy=fminf(fmaxf(q->zp.fy,(float)b[1]),(float)b[3]);
    /* s31: without lighting gl_transform_to_viewport takes the CURRENT
       colour (the last glColor), not this clipped vertex's */
    if (!c->lighting_enabled)
      gl_zp_color(&q->zp,&q->color);
  }
}

static void gl_draw_triangle_clip(GLContext *c,
                                  GLVertex *p0,GLVertex *p1,GLVertex *p2,int clip_bit);

/* s31: GL_LIGHT_MODEL_TWO_SIDE (GL 1.3 2.13.1; review G5). light.c lit
   every vertex twice, front and back; a back-facing triangle is drawn with
   the back colours, swapped in for this triangle only (strips share
   vertices), including the flat colour of the provoking vertex. Only
   back-facing triangles with two-sided lighting on come here: front faces
   and every other state pay nothing. */
static void gl_draw_triangle_twoside(GLContext *c,
                                     GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
  GLVertex *v[3]={p0,p1,p2};
  GLVertex *fv=c->current_shade_model != GL_SMOOTH ? c->flat_vtx : NULL;
  V4 col[3], fcol;
  V3 sp[3], fsp;
  int zc[3][3], i, fr=c->flat_r, fg=c->flat_g, fb=c->flat_b;
  int fown = fv != NULL && fv != p0 && fv != p1 && fv != p2;

  for (i=0;i<3;i++) {
    col[i]=v[i]->color; sp[i]=v[i]->spec;
    zc[i][0]=v[i]->zp.r; zc[i][1]=v[i]->zp.g; zc[i][2]=v[i]->zp.b;
    v[i]->color=v[i]->color_back; v[i]->spec=v[i]->spec_back;
    gl_zp_color(&v[i]->zp,&v[i]->color);
  }
  if (fown) {
    /* a clipped piece: the provoking vertex is the original one */
    fcol=fv->color; fsp=fv->spec;
    fv->color=fv->color_back; fv->spec=fv->spec_back;
  }
  if (fv) {
    ZBufferPoint t;
    gl_zp_color(&t,&fv->color);
    c->flat_r=t.r; c->flat_g=t.g; c->flat_b=t.b;
    c->flat_ok=0;   /* phase 6 V5 */
  }
  c->draw_triangle_back(c,p0,p1,p2);
  if (fown) { fv->color=fcol; fv->spec=fsp; }
  c->flat_r=fr; c->flat_g=fg; c->flat_b=fb;
  c->flat_ok=0;   /* phase 6 V5: the front colours again */
  for (i=0;i<3;i++) {
    v[i]->color=col[i]; v[i]->spec=sp[i];
    v[i]->zp.r=zc[i][0]; v[i]->zp.g=zc[i][1]; v[i]->zp.b=zc[i][2];
  }
}

void gl_draw_triangle(GLContext *c,
                      GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
  int co,c_and,cc[3],front;
  float norm;
  
  cc[0]=p0->clip_code;
  cc[1]=p1->clip_code;
  cc[2]=p2->clip_code;
  
  co=cc[0] | cc[1] | cc[2];

  /* we handle the non clipped case here to go faster */
  if (co==0) {
    
      /* s31: from the window position the fillers scan-convert (ztri.h):
         the snapped one called a sub-pixel triangle degenerate and dropped
         it, a hole in a fine mesh, although it may cover a pixel centre */
      norm=(p1->zp.fx-p0->zp.fx)*(p2->zp.fy-p0->zp.fy)-
        (p2->zp.fx-p0->zp.fx)*(p1->zp.fy-p0->zp.fy);
      
      if (norm == 0) return;

      front = norm < 0.0;
      front = front ^ c->current_front_face;
  
      /* back face culling */
      if (c->cull_face_enabled) {
        /* most used case first */
        if (c->current_cull_face == GL_BACK) {
          if (front == 0) return;
          c->draw_triangle_front(c,p0,p1,p2);
        } else if (c->current_cull_face == GL_FRONT) {
          if (front != 0) return;
          if (c->lighting_enabled && c->light_model_two_side)
            gl_draw_triangle_twoside(c,p0,p1,p2);
          else
            c->draw_triangle_back(c,p0,p1,p2);
        } else {
          return;
        }
      } else {
        /* no culling */
        if (front) {
          c->draw_triangle_front(c,p0,p1,p2);
        } else if (c->lighting_enabled && c->light_model_two_side) {
          gl_draw_triangle_twoside(c,p0,p1,p2);
        } else {
          c->draw_triangle_back(c,p0,p1,p2);
        }
      }
  } else {
    c_and=cc[0] & cc[1] & cc[2];
    if (c_and==0) {
      gl_draw_triangle_clip(c,p0,p1,p2,0);
    }
  }
}

#define CLIP_AT(o,a,b) (clip_bit < 6 ? clip_proc[clip_bit](o,a,b) : \
                         clip_user(c,clip_bit - TGL_CLIP_USER_SHIFT,o,a,b))

static void gl_draw_triangle_clip(GLContext *c,
                                  GLVertex *p0,GLVertex *p1,GLVertex *p2,int clip_bit)
{
  int co,c_and,co1,cc[3],edge_flag_tmp,clip_mask,nbits,done;
  GLVertex tmp1,tmp2,*q[3];
  float tt;
  
  cc[0]=p0->clip_code;
  cc[1]=p1->clip_code;
  cc[2]=p2->clip_code;
  
  co=cc[0] | cc[1] | cc[2];
  if (co == 0) {
    gl_draw_triangle(c,p0,p1,p2);
  } else {
    c_and=cc[0] & cc[1] & cc[2];
    /* the triangle is completely outside */
    if (c_and!=0) return;

    /* find the next direction to clip; s31: bits 6.. are the user
       planes (plan F7) */
    nbits = c->clip_plane_mask ? TGL_CLIP_USER_SHIFT + 6 : 6;
    while (clip_bit < nbits && (co & (1 << clip_bit)) == 0)  {
      clip_bit++;
    }

    /* this test can be true only in case of rounding errors */
    if (clip_bit == nbits) {
#if 0
      printf("Error:\n");
      printf("%f %f %f %f\n",p0->pc.X,p0->pc.Y,p0->pc.Z,p0->pc.W);
      printf("%f %f %f %f\n",p1->pc.X,p1->pc.Y,p1->pc.Z,p1->pc.W);
      printf("%f %f %f %f\n",p2->pc.X,p2->pc.Y,p2->pc.Z,p2->pc.W);
#endif
      return;
    }
  
    clip_mask = 1 << clip_bit;
    co1=(cc[0] ^ cc[1] ^ cc[2]) & clip_mask;
    done = clip_bit >= TGL_CLIP_USER_SHIFT ? clip_mask : 0;
    
    if (co1)  { 
      /* one point outside */

      if (cc[0] & clip_mask) { q[0]=p0; q[1]=p1; q[2]=p2; }
      else if (cc[1] & clip_mask) { q[0]=p1; q[1]=p2; q[2]=p0; }
      else { q[0]=p2; q[1]=p0; q[2]=p1; }
      
      tt=CLIP_AT(&tmp1.pc,&q[0]->pc,&q[1]->pc);
      updateTmp(c,&tmp1,q[0],q[1],tt,done);

      tt=CLIP_AT(&tmp2.pc,&q[0]->pc,&q[2]->pc);
      updateTmp(c,&tmp2,q[0],q[2],tt,done);

      tmp1.edge_flag=q[0]->edge_flag;
      edge_flag_tmp=q[2]->edge_flag;
      q[2]->edge_flag=0;
      gl_draw_triangle_clip(c,&tmp1,q[1],q[2],clip_bit+1);

      tmp2.edge_flag=1;
      tmp1.edge_flag=0;
      q[2]->edge_flag=edge_flag_tmp;
      gl_draw_triangle_clip(c,&tmp2,&tmp1,q[2],clip_bit+1);
    } else {
      /* two points outside */

      if ((cc[0] & clip_mask)==0) { q[0]=p0; q[1]=p1; q[2]=p2; }
      else if ((cc[1] & clip_mask)==0) { q[0]=p1; q[1]=p2; q[2]=p0; } 
      else { q[0]=p2; q[1]=p0; q[2]=p1; }
      
      tt=CLIP_AT(&tmp1.pc,&q[0]->pc,&q[1]->pc);
      updateTmp(c,&tmp1,q[0],q[1],tt,done);

      tt=CLIP_AT(&tmp2.pc,&q[0]->pc,&q[2]->pc);
      updateTmp(c,&tmp2,q[0],q[2],tt,done);
      
      tmp1.edge_flag=1;
      tmp2.edge_flag=q[2]->edge_flag;
      gl_draw_triangle_clip(c,q[0],&tmp1,&tmp2,clip_bit+1);
    }
  }
}


void gl_draw_triangle_select(GLContext *c,
                             GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
  gl_add_select1(c,p0->zp.z,p1->zp.z,p2->zp.z);
}

#ifdef PROFILE
int count_triangles,count_triangles_textured,count_pixels;
#endif

void gl_draw_triangle_fill(GLContext *c,
                           GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
#ifdef PROFILE
  {
    int norm;
    assert(p0->zp.x >= 0 && p0->zp.x < c->zb->xsize);
    assert(p0->zp.y >= 0 && p0->zp.y < c->zb->ysize);
    assert(p1->zp.x >= 0 && p1->zp.x < c->zb->xsize);
    assert(p1->zp.y >= 0 && p1->zp.y < c->zb->ysize);
    assert(p2->zp.x >= 0 && p2->zp.x < c->zb->xsize);
    assert(p2->zp.y >= 0 && p2->zp.y < c->zb->ysize);
    
    norm=(p1->zp.x-p0->zp.x)*(p2->zp.y-p0->zp.y)-
      (p2->zp.x-p0->zp.x)*(p1->zp.y-p0->zp.y);
    count_pixels+=abs(norm)/2;
    count_triangles++;
  }
#endif
    
  /* s31: an enabled texture without an image draws untextured (GL: an
     incomplete texture disables texturing), instead of reading NULL */
  /* s31: the fillers for the depth state - LEQUAL (TinyGL's), test off
     (ztriangle_nt.c), mask off (ztriangle_nw.c), strict LESS
     (ztriangle_lt.c) - are chosen once per glBegin (raster.c); TinyGL
     always tested >= and wrote Z */
  if (c->tex_active) {
    /* s31: the texture is complete and its environment is the texel
       (raster.c); zbuffer.h's masks carry its own size. The vertices go
       as they are: the filler samples pixel centres, so TinyGL's texture
       squeeze and its per-triangle copies are gone (review G2, P2) */
#ifdef PROFILE
    count_triangles_textured++;
#endif
    c->zb_map(c->zb,&p0->zp,&p1->zp,&p2->zp);
  } else if (c->current_shade_model == GL_SMOOTH) {
    c->zb_smooth(c->zb,&p0->zp,&p1->zp,&p2->zp);
  } else {
    /* s31: the provoking vertex's colour (gl_set_provoking), handed to the
       flat filler through the ZBuffer: TinyGL's took whichever vertex its
       y sort left last, and copying the colour into all three vertices
       (shared along a strip) cost 18 loads and stores a triangle */
    c->zb->flat_color=RGB_TO_PIXEL(c->flat_r,c->flat_g,c->flat_b);
    c->zb_flat(c->zb,&p0->zp,&p1->zp,&p2->zp);
  }
}

/* Render a clipped triangle in line mode */  

void gl_draw_triangle_line(GLContext *c,
                           GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
    if (p0->edge_flag) gl_zb_line(c,p0,p1);
    if (p1->edge_flag) gl_zb_line(c,p1,p2);
    if (p2->edge_flag) gl_zb_line(c,p2,p0);
}



/* Render a clipped triangle in point mode */
void gl_draw_triangle_point(GLContext *c,
                            GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
  if (c->raster_gen_points) {
    /* s31 */
    if (c->raster_skip) return;
    if (p0->edge_flag) gl_general_point(c,p0);
    if (p1->edge_flag) gl_general_point(c,p1);
    if (p2->edge_flag) gl_general_point(c,p2);
    return;
  }
  if (p0->edge_flag) zb_plot_in(c,&p0->zp);
  if (p1->edge_flag) zb_plot_in(c,&p1->zp);
  if (p2->edge_flag) zb_plot_in(c,&p2->zp);
}




