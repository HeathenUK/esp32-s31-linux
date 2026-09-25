#include "zgl.h"

/* fill triangle profile */
/* #define PROFILE */

#define CLIP_XMIN   (1<<0)
#define CLIP_XMAX   (1<<1)
#define CLIP_YMIN   (1<<2)
#define CLIP_YMAX   (1<<3)
#define CLIP_ZMIN   (1<<4)
#define CLIP_ZMAX   (1<<5)

void gl_transform_to_viewport(GLContext *c,GLVertex *v)
{
  float winv;

  /* coordinates */
  winv=1.0/v->pc.W;
  v->zp.x= (int) ( v->pc.X * winv * c->viewport.scale.X 
                   + c->viewport.trans.X );
  v->zp.y= (int) ( v->pc.Y * winv * c->viewport.scale.Y 
                   + c->viewport.trans.Y );
  v->zp.z= (int) ( v->pc.Z * winv * c->viewport.scale.Z 
                   + c->viewport.trans.Z );
  /* color */
  if (c->lighting_enabled) {
      v->zp.r=(int)(v->color.v[0] * (ZB_POINT_RED_MAX - ZB_POINT_RED_MIN) 
                    + ZB_POINT_RED_MIN);
      v->zp.g=(int)(v->color.v[1] * (ZB_POINT_GREEN_MAX - ZB_POINT_GREEN_MIN) 
                    + ZB_POINT_GREEN_MIN);
      v->zp.b=(int)(v->color.v[2] * (ZB_POINT_BLUE_MAX - ZB_POINT_BLUE_MIN) 
                    + ZB_POINT_BLUE_MIN);
  } else {
      /* no need to convert to integer if no lighting : take current color */
      v->zp.r = c->longcurrent_color[0];
      v->zp.g = c->longcurrent_color[1];
      v->zp.b = c->longcurrent_color[2];
  }
  
  /* texture */

  if (c->texture_2d_enabled) {
    v->zp.s=(int)(v->tex_coord.X * (ZB_POINT_S_MAX - ZB_POINT_S_MIN) 
                  + ZB_POINT_S_MIN);
    v->zp.t=(int)(v->tex_coord.Y * (ZB_POINT_T_MAX - ZB_POINT_T_MIN) 
                  + ZB_POINT_T_MIN);
  }
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

void gl_draw_point(GLContext *c,GLVertex *p0)
{
  if (p0->clip_code == 0) {
    if (c->render_mode == GL_SELECT) {
      gl_add_select(c,p0->zp.z,p0->zp.z);
    } else if (c->depth_test) {
      ZB_plot(c->zb,&p0->zp);
    } else {
      ZB_plot_nz(c->zb,&p0->zp);   /* s31 */
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

/* s31: out of line: 9 call sites in vertex.c, and code size is time in XIP */
void gl_set_provoking_flat(GLContext *c, GLVertex *v)
{
  ZBufferPoint t;
  gl_zp_color(&t, &v->color);
  c->flat_r = t.r; c->flat_g = t.g; c->flat_b = t.b;
}

/* s31: GL_FLAT lines take the provoking vertex's colour (gl_set_provoking);
   TinyGL interpolated whenever the two ends differed */
static void gl_zb_line(GLContext *c,ZBufferPoint *a,ZBufferPoint *b)
{
  ZBufferPoint fa,fb;
  if (c->current_shade_model != GL_SMOOTH) {
    fa=*a; fb=*b;
    fa.r=fb.r=c->flat_r; fa.g=fb.g=c->flat_g; fa.b=fb.b=c->flat_b;
    a=&fa; b=&fb;
  }
  if (c->depth_test)
    ZB_line_z(c->zb,a,b);
  else
    ZB_line(c->zb,a,b);
}

void gl_draw_line(GLContext *c,GLVertex *p1,GLVertex *p2)
{
  float dx,dy,dz,dw,x1,y1,z1,w1;
  float tmin,tmax;
  GLVertex q1,q2;
  int cc1,cc2;
  
  cc1=p1->clip_code;
  cc2=p2->clip_code;

  if ( (cc1 | cc2) == 0) {
    if (c->render_mode == GL_SELECT) {
      gl_add_select1(c,p1->zp.z,p2->zp.z,p2->zp.z);
    } else {
      gl_zb_line(c,&p1->zp,&p2->zp);
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
        ClipLine1(-dz+dw,z1-w1,&tmin,&tmax)) {

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
        gl_zb_line(c,&q1.zp,&q2.zp);
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

static inline void updateTmp(GLContext *c,
			     GLVertex *q,GLVertex *p0,GLVertex *p1,float t)
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

  if (c->texture_2d_enabled) {
    q->tex_coord.X=p0->tex_coord.X + (p1->tex_coord.X-p0->tex_coord.X)*t;
    q->tex_coord.Y=p0->tex_coord.Y + (p1->tex_coord.Y-p0->tex_coord.Y)*t;
  }

  q->clip_code=gl_clipcode(q->pc.X,q->pc.Y,q->pc.Z,q->pc.W);
  if (q->clip_code==0) {
    gl_transform_to_viewport(c,q);
    /* s31: without lighting gl_transform_to_viewport takes the CURRENT
       colour (the last glColor), not this clipped vertex's */
    if (!c->lighting_enabled)
      gl_zp_color(&q->zp,&q->color);
  }
}

static void gl_draw_triangle_clip(GLContext *c,
                                  GLVertex *p0,GLVertex *p1,GLVertex *p2,int clip_bit);

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
    
      norm=(float)(p1->zp.x-p0->zp.x)*(float)(p2->zp.y-p0->zp.y)-
        (float)(p2->zp.x-p0->zp.x)*(float)(p1->zp.y-p0->zp.y);
      
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
          c->draw_triangle_back(c,p0,p1,p2);
        } else {
          return;
        }
      } else {
        /* no culling */
        if (front) {
          c->draw_triangle_front(c,p0,p1,p2);
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

static void gl_draw_triangle_clip(GLContext *c,
                                  GLVertex *p0,GLVertex *p1,GLVertex *p2,int clip_bit)
{
  int co,c_and,co1,cc[3],edge_flag_tmp,clip_mask;
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

    /* find the next direction to clip */
    while (clip_bit < 6 && (co & (1 << clip_bit)) == 0)  {
      clip_bit++;
    }

    /* this test can be true only in case of rounding errors */
    if (clip_bit == 6) {
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
    
    if (co1)  { 
      /* one point outside */

      if (cc[0] & clip_mask) { q[0]=p0; q[1]=p1; q[2]=p2; }
      else if (cc[1] & clip_mask) { q[0]=p1; q[1]=p2; q[2]=p0; }
      else { q[0]=p2; q[1]=p0; q[2]=p1; }
      
      tt=clip_proc[clip_bit](&tmp1.pc,&q[0]->pc,&q[1]->pc);
      updateTmp(c,&tmp1,q[0],q[1],tt);

      tt=clip_proc[clip_bit](&tmp2.pc,&q[0]->pc,&q[2]->pc);
      updateTmp(c,&tmp2,q[0],q[2],tt);

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
      
      tt=clip_proc[clip_bit](&tmp1.pc,&q[0]->pc,&q[1]->pc);
      updateTmp(c,&tmp1,q[0],q[1],tt);

      tt=clip_proc[clip_bit](&tmp2.pc,&q[0]->pc,&q[2]->pc);
      updateTmp(c,&tmp2,q[0],q[2],tt);
      
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
  /* s31: depth test off / depth mask off select their own fillers
     (ztriangle_nt.c, ztriangle_nw.c); TinyGL always tested and wrote Z */
  int zv = !c->depth_test ? 1 : (!c->depth_mask ? 2 : 0);
  if (c->texture_2d_enabled && c->current_texture->images[0].pixmap) {
#ifdef PROFILE
    count_triangles_textured++;
#endif
    ZB_setTexture(c->zb,c->current_texture->images[0].pixmap);
    if (zv == 0) ZB_fillTriangleMappingPerspective(c->zb,&p0->zp,&p1->zp,&p2->zp);
    else if (zv == 1) ZB_fillTriangleMappingPerspective_nt(c->zb,&p0->zp,&p1->zp,&p2->zp);
    else ZB_fillTriangleMappingPerspective_nw(c->zb,&p0->zp,&p1->zp,&p2->zp);
  } else if (c->current_shade_model == GL_SMOOTH) {
    if (zv == 0) ZB_fillTriangleSmooth(c->zb,&p0->zp,&p1->zp,&p2->zp);
    else if (zv == 1) ZB_fillTriangleSmooth_nt(c->zb,&p0->zp,&p1->zp,&p2->zp);
    else ZB_fillTriangleSmooth_nw(c->zb,&p0->zp,&p1->zp,&p2->zp);
  } else {
    /* s31: the flat fillers colour from whichever vertex their y sort
       leaves in p2. Give all three the provoking vertex's colour
       (gl_set_provoking) for this triangle only: the vertices may be
       shared with the next triangle of a strip. */
    ZBufferPoint *v[3]={&p0->zp,&p1->zp,&p2->zp};
    int sv[3][3],i;
    for (i=0;i<3;i++) {
      sv[i][0]=v[i]->r; sv[i][1]=v[i]->g; sv[i][2]=v[i]->b;
      v[i]->r=c->flat_r; v[i]->g=c->flat_g; v[i]->b=c->flat_b;
    }
    if (zv == 0) ZB_fillTriangleFlat(c->zb,v[0],v[1],v[2]);
    else if (zv == 1) ZB_fillTriangleFlat_nt(c->zb,v[0],v[1],v[2]);
    else ZB_fillTriangleFlat_nw(c->zb,v[0],v[1],v[2]);
    for (i=0;i<3;i++) {
      v[i]->r=sv[i][0]; v[i]->g=sv[i][1]; v[i]->b=sv[i][2];
    }
  }
}

/* Render a clipped triangle in line mode */  

void gl_draw_triangle_line(GLContext *c,
                           GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
    if (p0->edge_flag) gl_zb_line(c,&p0->zp,&p1->zp);
    if (p1->edge_flag) gl_zb_line(c,&p1->zp,&p2->zp);
    if (p2->edge_flag) gl_zb_line(c,&p2->zp,&p0->zp);
}



/* Render a clipped triangle in point mode */
void gl_draw_triangle_point(GLContext *c,
                            GLVertex *p0,GLVertex *p1,GLVertex *p2)
{
  void (*plot)(ZBuffer *,ZBufferPoint *) = c->depth_test ? ZB_plot : ZB_plot_nz;
  if (p0->edge_flag) plot(c->zb,&p0->zp);
  if (p1->edge_flag) plot(c->zb,&p1->zp);
  if (p2->edge_flag) plot(c->zb,&p2->zp);
}




