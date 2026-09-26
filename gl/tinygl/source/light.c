#include "zgl.h"
#include "msghandling.h"
#include "s31_fmath.h"

void glopMaterial(GLContext *c,GLParam *p)
{
  int mode=p[1].i;
  int type=p[2].i;
  int i;
  GLMaterial *m;

  /* s31 (phase 3a G02): only emission and ambient enter the products */
  if (type == GL_EMISSION || type == GL_AMBIENT || type == GL_AMBIENT_AND_DIFFUSE)
    c->light_dirty = 1;
  if (mode == GL_FRONT_AND_BACK) {
    /* s31: TinyGL wrote GL_FRONT into p[1] here, which rewrote the op inside
       a display list, so replays of it only ever set the front material */
    GLParam q[7];
    for (i = 0; i < 7; i++) q[i] = p[i];
    q[1].i=GL_FRONT;
    glopMaterial(c,q);
    mode=GL_BACK;
  }
  if (mode == GL_FRONT) m=&c->materials[0];
  else m=&c->materials[1];

  switch(type) {
  case GL_EMISSION:
    for(i=0;i<4;i++)
      m->emission.v[i]=p[3 + i].f;
    break;
  case GL_AMBIENT:
    for(i=0;i<4;i++)
      m->ambient.v[i]=p[3 + i].f;
    break;
  case GL_DIFFUSE:
    for(i=0;i<4;i++)
      m->diffuse.v[i]=p[3 + i].f;
    break;
  case GL_SPECULAR:
    for(i=0;i<4;i++)
      m->specular.v[i]=p[3 + i].f;
    m->do_specular = m->specular.v[0] != 0.0f || m->specular.v[1] != 0.0f ||
                     m->specular.v[2] != 0.0f;
    break;
  case GL_SHININESS:
    m->shininess=p[3].f;
    m->shininess_i = (m->shininess/128.0f)*SPECULAR_BUFFER_RESOLUTION;
    break;
  case GL_AMBIENT_AND_DIFFUSE:
    for(i=0;i<4;i++)
      m->diffuse.v[i]=p[3 + i].f;
    for(i=0;i<4;i++)
      m->ambient.v[i]=p[3 + i].f;
    break;
  case GL_COLOR_INDEXES:
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    break;
  }
}

/* s31 (phase 3a G02): glColor under GL_COLOR_MATERIAL - once per vertex
   in such apps - writes the tracked material fields directly; it built a
   glMaterial op and went through glopMaterial's recursion and switch */
void gl_color_material(GLContext *c, float r, float g, float b, float a)
{
  int mode = c->current_color_material_mode;
  int type = c->current_color_material_type;
  int s, s0 = mode == GL_BACK ? 1 : 0, s1 = mode == GL_FRONT ? 0 : 1;
  V4 v;

  v.v[0] = r; v.v[1] = g; v.v[2] = b; v.v[3] = a;
  for (s = s0; s <= s1; s++) {
    GLMaterial *m = &c->materials[s];
    switch (type) {
    case GL_AMBIENT_AND_DIFFUSE: m->ambient = v; m->diffuse = v; break;
    case GL_DIFFUSE: m->diffuse = v; continue;
    case GL_AMBIENT: m->ambient = v; break;
    case GL_EMISSION: m->emission = v; break;
    default: {                                 /* GL_SPECULAR, or invalid */
      GLParam q[7];
      int i;
      q[0].op = OP_Material;
      q[1].i = s ? GL_BACK : GL_FRONT;
      q[2].i = type;
      for (i = 0; i < 4; i++) q[3 + i].f = v.v[i];
      glopMaterial(c, q);
      continue;
    }
    }
    c->light_dirty = 1;
  }
}

void glopColorMaterial(GLContext *c,GLParam *p)
{
  int mode=p[1].i;
  int type=p[2].i;

  c->current_color_material_mode=mode;
  c->current_color_material_type=type;
}

void glopLight(GLContext *c,GLParam *p)
{
  int light=p[1].i;
  int type=p[2].i;
  V4 v;
  GLLight *l;
  int i;
  
  if (light < GL_LIGHT0 || light >= GL_LIGHT0+MAX_LIGHTS) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }

  l=&c->lights[light-GL_LIGHT0];
  c->light_dirty = 1;   /* s31 (phase 3a G02) */

  for(i=0;i<4;i++) v.v[i]=p[3+i].f;

  switch(type) {
  case GL_AMBIENT:
    l->ambient=v;
    break;
  case GL_DIFFUSE:
    l->diffuse=v;
    break;
  case GL_SPECULAR:
    l->specular=v;
    l->has_specular = v.v[0] != 0.0f || v.v[1] != 0.0f || v.v[2] != 0.0f;
    break;
  case GL_POSITION:
    {
      V4 pos;
      gl_M4_MulV4(&pos,c->matrix_stack_ptr[0],&v);

      l->position=pos;

      if (l->position.v[3] == 0) {
        l->norm_position.X=pos.X;
        l->norm_position.Y=pos.Y;
        l->norm_position.Z=pos.Z;
        
        gl_V3_Norm(&l->norm_position);
      }
    }
    break;
  case GL_SPOT_DIRECTION:
    /* s31 (phase 4): GL 1.3 2.13.1 - the direction is transformed by the
       upper-left 3x3 of the modelview when specified, as the position is
       by the whole matrix; TinyGL stored it untransformed, so a spot set
       after gluLookAt pointed somewhere else (mesa-demos teapot: the light
       pool on the floor was missing, the teapot lit by ambient alone) */
    {
      const M4 *m = c->matrix_stack_ptr[0];
      for(i=0;i<3;i++) {
        float d = m->m[i][0]*v.v[0] + m->m[i][1]*v.v[1] + m->m[i][2]*v.v[2];
        l->spot_direction.v[i]=d;
        l->norm_spot_direction.v[i]=d;
      }
    }
    gl_V3_Norm(&l->norm_spot_direction);
    break;
  case GL_SPOT_EXPONENT:
    l->spot_exponent=v.v[0];
    break;
  case GL_SPOT_CUTOFF:
    {
      float a=v.v[0];
      if (!(a == 180 || (a>=0 && a<=90))) {
        gl_set_error(c, GL_INVALID_VALUE);
        return;
      }
      l->spot_cutoff=a;
      /* s31 (phase 3a G01): float, from degrees (was cos in double) */
      if (a != 180) {
        float s;
        s31_sincos_deg(a, &s, &l->cos_spot_cutoff);
      }
    }
    break;
  case GL_CONSTANT_ATTENUATION:
    l->attenuation[0]=v.v[0];
    break;
  case GL_LINEAR_ATTENUATION:
    l->attenuation[1]=v.v[0];
    break;
  case GL_QUADRATIC_ATTENUATION:
    l->attenuation[2]=v.v[0];
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    break;
  }
}
  

void glopLightModel(GLContext *c,GLParam *p)
{
  int pname=p[1].i;
  int i;

  c->light_dirty = 1;   /* s31 (phase 3a G02) */
  switch(pname) {
  case GL_LIGHT_MODEL_AMBIENT:
    for(i=0;i<4;i++) 
      c->ambient_light_model.v[i]=p[2 + i].f;
    break;
  case GL_LIGHT_MODEL_LOCAL_VIEWER:
    c->local_light_model=(int)p[2].f;
    break;
  case GL_LIGHT_MODEL_TWO_SIDE:
    c->light_model_two_side = (int)p[2].f;
    break;
  case GL_LIGHT_MODEL_COLOR_CONTROL:
    /* s31: honoured (gl_shade_vertex, zpipe.c) */
    if ((int)p[2].f != GL_SINGLE_COLOR && (int)p[2].f != GL_SEPARATE_SPECULAR_COLOR) {
      gl_set_error(c, GL_INVALID_ENUM);
      break;
    }
    c->color_control = (int)p[2].f;
    c->raster_dirty = 1;
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    break;
  }
}


/* s31 (phase 3a G02): fmax.s and fmin.s, no branches (4 colour channels
   of every lit vertex). The same value for every number; a NaN now clamps
   to min instead of passing through, and -0 becomes +0 */
static inline float clampf(float a,float min,float max)
{
  return fminf(fmaxf(a,min),max);
}

void gl_enable_disable_light(GLContext *c,int light,int v)
{
  GLLight *l=&c->lights[light];
  c->light_dirty = 1;   /* s31 (phase 3a G02) */
  if (v && !l->enabled) {
    l->enabled=1;
    l->next=c->first_light;
    c->first_light=l;
    l->prev=NULL;
  } else if (!v && l->enabled) {
    l->enabled=0;
    if (l->prev == NULL) c->first_light=l->next;
    else l->prev->next=l->next;
    if (l->next != NULL) l->next->prev=l->prev;
  }
}

/* s31 (phase 3a G02): the per-vertex terms that depend on no vertex -
   emission + ambient * scene ambient per material side, and each enabled
   light's ambient * material ambient - once per state change instead of
   once per vertex (and per light). The same expressions as the per-vertex
   code had, so the result is bit-identical. */
static void gl_light_products(GLContext *c)
{
  GLLight *l;
  int s, ns = c->light_model_two_side ? 2 : 1;   /* the back only when lit */
  for (s = 0; s < ns; s++) {
    const GLMaterial *m = &c->materials[s];
    c->light_base[s].v[0]=m->emission.v[0]+m->ambient.v[0]*c->ambient_light_model.v[0];
    c->light_base[s].v[1]=m->emission.v[1]+m->ambient.v[1]*c->ambient_light_model.v[1];
    c->light_base[s].v[2]=m->emission.v[2]+m->ambient.v[2]*c->ambient_light_model.v[2];
    for (l = c->first_light; l != NULL; l = l->next) {
      l->amb_prod[s].v[0]=l->ambient.v[0] * m->ambient.v[0];
      l->amb_prod[s].v[1]=l->ambient.v[1] * m->ambient.v[1];
      l->amb_prod[s].v[2]=l->ambient.v[2] * m->ambient.v[2];
    }
  }
  c->light_dirty = 0;
}

/* non optimized lightening model */
/* s31: one side's colour: material m, normal n, into *col (and *scol, the
   secondary colour, with GL_SEPARATE_SPECULAR_COLOR). gl_shade_vertex
   calls it once for the front, and with GL_LIGHT_MODEL_TWO_SIDE once more
   for the back with the back material and -n (GL 1.3 2.13.1; review G5:
   TinyGL lit both sides with |n.l| and the front material) */
static inline __attribute__((always_inline))
void shade_side(GLContext *c, const GLVertex *v, int side, V3 n,
                V4 *col, V3 *scol)
{
  const GLMaterial *m = &c->materials[side];
  float R,G,B,A;
  float SR=0.0f,SG=0.0f,SB=0.0f;   /* s31: secondary colour (separate specular) */
  int sep = c->raster_sepspec;
  GLLight *l;
  V3 s,d;
  float dist,tmp,att,dot,dot_spot,dot_spec;

  R=c->light_base[side].v[0];
  G=c->light_base[side].v[1];
  B=c->light_base[side].v[2];
  A=clampf(m->diffuse.v[3],0,1);

  for(l=c->first_light;l!=NULL;l=l->next) {
    float lR,lB,lG;
    
    /* ambient. (A diffuse * material diffuse product kept the same way
       was measured -0.3% gears, -0.6% teapot, -1.0% two lights, but +0.7%
       under GL_COLOR_MATERIAL, where it must be recomputed per vertex, and
       it is not bit-exact (dot*(ld*md) vs (dot*ld)*md): rejected,
       artifacts/gl/phase3a/LEVERS.md G02d) */
    lR=l->amb_prod[side].v[0];
    lG=l->amb_prod[side].v[1];
    lB=l->amb_prod[side].v[2];

    if (l->position.v[3] == 0) {
      /* light at infinity */
      d.X=l->norm_position.v[0];
      d.Y=l->norm_position.v[1];
      d.Z=l->norm_position.v[2];
      att=1;
    } else {
      /* distance attenuation */
      d.X=l->position.v[0]-v->ec.v[0];
      d.Y=l->position.v[1]-v->ec.v[1];
      d.Z=l->position.v[2]-v->ec.v[2];
      dist=sqrtf(d.X*d.X+d.Y*d.Y+d.Z*d.Z);  /* s31: float (was a soft-double sqrt per vertex) */
      if (dist>1E-10f) {
        tmp=1/dist;
        d.X*=tmp;
        d.Y*=tmp;
        d.Z*=tmp;
      }
      att=1.0f/(l->attenuation[0]+dist*(l->attenuation[1]+
				     dist*l->attenuation[2]));
    }
    dot=d.X*n.X+d.Y*n.Y+d.Z*n.Z;
    if (dot>0) {
      /* diffuse light */
      lR+=dot * l->diffuse.v[0] * m->diffuse.v[0];
      lG+=dot * l->diffuse.v[1] * m->diffuse.v[1];
      lB+=dot * l->diffuse.v[2] * m->diffuse.v[2];

      /* spot light */
      if (l->spot_cutoff != 180) {
        dot_spot=-(d.X*l->norm_spot_direction.v[0]+
                   d.Y*l->norm_spot_direction.v[1]+
                   d.Z*l->norm_spot_direction.v[2]);
        if (dot_spot < l->cos_spot_cutoff) {
          /* no contribution */
          continue;
        } else {
          /* TODO: optimize */
          if (l->spot_exponent > 0) {
            /* s31 (phase 3a G01): musl's powf computes in double
               (14 __muldf3 call sites); this is float only */
            att=att*s31_powf(dot_spot,l->spot_exponent);
          }
        }
      }

      /* specular light. s31 (phase 3a G02): skipped when the material's or
         the light's specular colour is zero - the default material's is
         (0,0,0), so gears, glxgears and teapot computed it on every lit
         vertex and added 0. Bit-exact: every skipped term is x * 0 */
      if (m->do_specular && l->has_specular) {
      if (c->local_light_model) {
        V3 vcoord;
        vcoord.X=v->ec.X;
        vcoord.Y=v->ec.Y;
        vcoord.Z=v->ec.Z;
        gl_V3_Norm(&vcoord);
        /* s31: was vcoord.X for all three */
        s.X=d.X-vcoord.X;
        s.Y=d.Y-vcoord.Y;
        s.Z=d.Z-vcoord.Z;
      } else {
        s.X=d.X;
        s.Y=d.Y;
        s.Z=d.Z+1.0f;
      }
      dot_spec=n.X*s.X+n.Y*s.Y+n.Z*s.Z;
      if (dot_spec>0) {
        GLSpecBuf *specbuf;
        int idx;
        tmp=sqrtf(s.X*s.X+s.Y*s.Y+s.Z*s.Z);
        if (tmp > 1E-3f) {
          dot_spec=dot_spec / tmp;
        }
      
        /* TODO: optimize */
        /* testing specular buffer code */
        /* dot_spec= pow(dot_spec,m->shininess);*/
        /* s31 (phase 3a G02): the material's last table while it still
           holds this shininess (the list walk ran per vertex per light) */
        specbuf = m->specbuf;
        if (specbuf == NULL || specbuf->shininess_i != m->shininess_i)
          specbuf = ((GLMaterial *)m)->specbuf =
            specbuf_get_buffer(c, m->shininess_i, m->shininess);
        idx = (int)(dot_spec*SPECULAR_BUFFER_SIZE);
        if (idx > SPECULAR_BUFFER_SIZE) idx = SPECULAR_BUFFER_SIZE;
        dot_spec = specbuf->buf[idx];
        if (sep) {
          /* s31: GL_SEPARATE_SPECULAR_COLOR: added after texturing */
          SR+=att * dot_spec * l->specular.v[0] * m->specular.v[0];
          SG+=att * dot_spec * l->specular.v[1] * m->specular.v[1];
          SB+=att * dot_spec * l->specular.v[2] * m->specular.v[2];
        } else {
          lR+=dot_spec * l->specular.v[0] * m->specular.v[0];
          lG+=dot_spec * l->specular.v[1] * m->specular.v[1];
          lB+=dot_spec * l->specular.v[2] * m->specular.v[2];
        }
      }
      }   /* do_specular */
    }

    R+=att * lR;
    G+=att * lG;
    B+=att * lB;
  }

  col->v[0]=clampf(R,0,1);
  col->v[1]=clampf(G,0,1);
  col->v[2]=clampf(B,0,1);
  col->v[3]=A;
  if (sep) {
    /* the normal is not needed any more (zgl.h GLVertex) */
    scol->X=clampf(SR,0,1);
    scol->Y=clampf(SG,0,1);
    scol->Z=clampf(SB,0,1);
  }
}

void gl_shade_vertex(GLContext *c,GLVertex *v)
{
  V3 n = v->normal;               /* v->spec may overwrite it */
  if (c->light_dirty)
    gl_light_products(c);
  if (c->light_model_two_side) {
    /* stored after the front: color_back and spec_back share the object
       and eye coordinates' storage (zgl.h), which the front pass reads.
       Two inlined bodies here and one below: 2.4 kB of flash for the rare
       two-sided case, where one out-of-line body, or one inlined body in
       a loop over the sides, cost gears 1.2% (measured, gl/bench) */
    V3 nb, sb;
    V4 cb;
    nb.X = -n.X; nb.Y = -n.Y; nb.Z = -n.Z;
    shade_side(c, v, 1, nb, &cb, &sb);
    shade_side(c, v, 0, n, &v->color, &v->spec);
    v->color_back = cb;
    v->spec_back = sb;
    return;
  }
  shade_side(c, v, 0, n, &v->color, &v->spec);
}

