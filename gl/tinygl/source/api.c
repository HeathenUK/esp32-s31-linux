#include "zgl.h"
#include "s31_pixels.h"
#include "s31_ramtext.h"
#include <stdio.h>

/* s31 (phase 3a G14): while executing (not compiling a list, not printing
   ops) the per-vertex ops run their glop directly: gl_add_op's context
   call, flag tests and op-table indirect call cost ~29 instructions per
   call (gl/bench prof.sh, teapot), per vertex and per normal in immediate
   mode. exec_flag is 0 only while compiling, so it needs no test. */
#define GL_RUN(name, p) do { \
    GLContext *c_ = gl_ctx; \
    if (!(c_->compile_flag | c_->print_flag)) glop##name(c_, p); \
    else gl_add_op(p); \
  } while (0)

/* glVertex */

void glVertex4f(float x,float y,float z,float w)
{
  GLParam p[5];

  GLContext *c=gl_ctx;
  /* s31 (phase 3a G14): executing: the vertex op itself (vertex.c);
     phase 4 L1: through vtx_run (s31_ramtext.h), which is NULL while
     compiling or printing */
  s31_vtx_fn run=c->vtx_run;
  if (run) {
    run(x,y,z,w,c);
    return;
  }
  p[0].op=OP_Vertex;
  p[1].f=x;
  p[2].f=y;
  p[3].f=z;
  p[4].f=w;
  gl_add_op(p);
}

void glVertex2f(float x,float y) 
{
  glVertex4f(x,y,0,1);
}

void glVertex3f(float x,float y,float z) 
{
  glVertex4f(x,y,z,1);
}

void glVertex3fv(float *v) 
{
  glVertex4f(v[0],v[1],v[2],1);
}

/* glNormal */

void glNormal3f(float x,float y,float z)
{
  GLParam p[4];

  GLContext *c=gl_ctx;
  /* s31 (phase 3a G14): executing: glopNormal's two lines, here */
  if (!(c->compile_flag | c->print_flag)) {
    c->current_normal.X=x;
    c->current_normal.Y=y;
    c->current_normal.Z=z;
    c->current_normal.W=0;
    return;
  }
  p[0].op=OP_Normal;
  p[1].f=x;
  p[2].f=y;
  p[3].f=z;
  gl_add_op(p);
}

void glNormal3fv(float *v) 
{
  glNormal3f(v[0],v[1],v[2]);
}

/* glColor */

/* s31: GL clamps colours to [0,1] before rasterising; TinyGL did not, and
   a negative or >1 component wrapped the integer colour. */
static inline float clamp01(float v)
{
  /* s31 (phase 3a G14): fmax.s/fmin.s, no branches; NaN clamps to 0 */
  return fminf(fmaxf(v, 0.0f), 1.0f);
}

void glColor4f(float r,float g,float b,float a)
{
  GLParam p[8];
  float rc=clamp01(r),gc=clamp01(g),bc=clamp01(b);
  GLContext *c=gl_ctx;

  /* s31 (phase 3a G14): executing: glopColor's stores, here */
  if (!(c->compile_flag | c->print_flag)) {
    c->current_color.X=r;
    c->current_color.Y=g;
    c->current_color.Z=b;
    c->current_color.W=a;
    c->longcurrent_color[0]=(unsigned int) (rc * (ZB_POINT_RED_MAX - ZB_POINT_RED_MIN) +
                                            ZB_POINT_RED_MIN);
    c->longcurrent_color[1]=(unsigned int) (gc * (ZB_POINT_GREEN_MAX - ZB_POINT_GREEN_MIN) +
                                            ZB_POINT_GREEN_MIN);
    c->longcurrent_color[2]=(unsigned int) (bc * (ZB_POINT_BLUE_MAX - ZB_POINT_BLUE_MIN) +
                                            ZB_POINT_BLUE_MIN);
    if (c->color_material_enabled)
      gl_color_material(c,r,g,b,a);
    return;
  }
  p[0].op=OP_Color;
  p[1].f=r;
  p[2].f=g;
  p[3].f=b;
  p[4].f=a;
  /* direct convertion to integer to go faster if no shading */
  p[5].ui = (unsigned int) (rc * (ZB_POINT_RED_MAX - ZB_POINT_RED_MIN) + 
                            ZB_POINT_RED_MIN);
  p[6].ui = (unsigned int) (gc * (ZB_POINT_GREEN_MAX - ZB_POINT_GREEN_MIN) + 
                            ZB_POINT_GREEN_MIN);
  p[7].ui = (unsigned int) (bc * (ZB_POINT_BLUE_MAX - ZB_POINT_BLUE_MIN) + 
                            ZB_POINT_BLUE_MIN);
  gl_add_op(p);
}

void glColor4fv(float *v)
{
  glColor4f(v[0],v[1],v[2],v[3]);
}

void glColor3f(float x,float y,float z) 
{
  glColor4f(x,y,z,1);
}

void glColor3fv(float *v) 
{
  glColor4f(v[0],v[1],v[2],1);
}

void glColor3ub(GLubyte x, GLubyte y, GLubyte z)
{
  glColor4f((float)x/255.0f, (float)y/255.0f, (float)z/255.0f, 1);
}

void glColor3ubv(GLubyte *v)
{
  glColor4f((float)v[0]/255.0f, (float)v[1]/255.0f, (float)v[2]/255.0f, 1);
}

void glColor4ub(GLubyte x, GLubyte y, GLubyte z, GLubyte w)
{
  glColor4f((float)x/255.0f, (float)y/255.0f, (float)z/255.0f, (float)w/255.0f);
}

void glColor4ubv(GLubyte *v)
{
  glColor4f((float)v[0]/255.0f, (float)v[1]/255.0f, (float)v[2]/255.0f, (float)v[3]/255.0f);
}

/* TexCoord */

void glTexCoord4f(float s,float t,float r,float q)
{
  GLParam p[5];

  GLContext *c=gl_ctx;
  /* s31 (phase 3a G14): executing: glopTexCoord's stores, here */
  if (!(c->compile_flag | c->print_flag)) {
    c->current_tex_coord.X=s;
    c->current_tex_coord.Y=t;
    c->current_tex_coord.Z=r;
    c->current_tex_coord.W=q;
    return;
  }
  p[0].op=OP_TexCoord;
  p[1].f=s;
  p[2].f=t;
  p[3].f=r;
  p[4].f=q;
  gl_add_op(p);
}

void glTexCoord2f(float s,float t)
{
  glTexCoord4f(s,t,0,1);
}

void glTexCoord2fv(float *v)
{
  glTexCoord4f(v[0],v[1],0,1);
}

void glEdgeFlag(int flag)
{
  GLParam p[2];

  p[0].op=OP_EdgeFlag;
  p[1].i=flag;

  GL_RUN(EdgeFlag, p);
}

/* misc */

void glShadeModel(int mode)
{
  GLParam p[2];

  if (mode != GL_FLAT && mode != GL_SMOOTH) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_ShadeModel;
  p[1].i=mode;

  gl_add_op(p);
}

void glCullFace(int mode)
{
  GLParam p[2];

  if (mode != GL_BACK && mode != GL_FRONT && mode != GL_FRONT_AND_BACK) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_CullFace;
  p[1].i=mode;

  gl_add_op(p);
}

void glFrontFace(int mode)
{
  GLParam p[2];

  if (mode != GL_CCW && mode != GL_CW) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  mode = (mode != GL_CCW);

  p[0].op=OP_FrontFace;
  p[1].i=mode;

  gl_add_op(p);
}

void glPolygonMode(int face,int mode)
{
  GLParam p[3];

  if ((face != GL_BACK && face != GL_FRONT && face != GL_FRONT_AND_BACK) ||
      (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL)) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_PolygonMode;
  p[1].i=face;
  p[2].i=mode;

  gl_add_op(p);
}


/* glEnable / glDisable */

void glEnable(int cap)
{
  GLParam p[3];

  if (s31_cap_index(cap) < 0) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_EnableDisable;
  p[1].i=cap;
  p[2].i=1;

  gl_add_op(p);
}

void glDisable(int cap)
{
  GLParam p[3];

  if (s31_cap_index(cap) < 0) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_EnableDisable;
  p[1].i=cap;
  p[2].i=0;

  gl_add_op(p);
}

/* glBegin / glEnd */

void glBegin(int mode)
{
  GLParam p[2];

  p[0].op=OP_Begin;
  p[1].i=mode;

  GL_RUN(Begin, p);
}

void glEnd(void)
{
  GLParam p[1];

  p[0].op=OP_End;

  GL_RUN(End, p);
}

/* matrix */

void glMatrixMode(int mode)
{
  GLParam p[2];

  if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_MatrixMode;
  p[1].i=mode;

  gl_add_op(p);
}

void glLoadMatrixf(const float *m)
{
  GLParam p[17];
  int i;

  p[0].op=OP_LoadMatrix;
  for(i=0;i<16;i++) p[i+1].f=m[i];

  gl_add_op(p);
}

void glLoadIdentity(void)
{
  GLParam p[1];

  p[0].op=OP_LoadIdentity;

  gl_add_op(p);
}

void glMultMatrixf(const float *m)
{
  GLParam p[17];
  int i;

  p[0].op=OP_MultMatrix;
  for(i=0;i<16;i++) p[i+1].f=m[i];

  gl_add_op(p);
}

void glPushMatrix(void)
{
  GLParam p[1];

  p[0].op=OP_PushMatrix;

  gl_add_op(p);
}

void glPopMatrix(void)
{
  GLParam p[1];

  p[0].op=OP_PopMatrix;

  gl_add_op(p);
}

void glRotatef(float angle,float x,float y,float z)
{
  GLParam p[5];

  p[0].op=OP_Rotate;
  p[1].f=angle;
  p[2].f=x;
  p[3].f=y;
  p[4].f=z;

  gl_add_op(p);
}

void glTranslatef(float x,float y,float z)
{
  GLParam p[4];

  p[0].op=OP_Translate;
  p[1].f=x;
  p[2].f=y;
  p[3].f=z;

  gl_add_op(p);
}

void glScalef(float x,float y,float z)
{
  GLParam p[4];

  p[0].op=OP_Scale;
  p[1].f=x;
  p[2].f=y;
  p[3].f=z;

  gl_add_op(p);
}


void glViewport(int x,int y,int width,int height)
{
  GLParam p[5];

  if (width < 0 || height < 0) {
    gl_set_error(gl_get_context(), GL_INVALID_VALUE);
    return;
  }

  p[0].op=OP_Viewport;
  p[1].i=x;
  p[2].i=y;
  p[3].i=width;
  p[4].i=height;

  gl_add_op(p);
}

void glFrustum(float left,float right,float bottom,float top,
               float nearv,float farv)
{
  GLParam p[7];

  p[0].op=OP_Frustum;
  p[1].f=left;
  p[2].f=right;
  p[3].f=bottom;
  p[4].f=top;
  p[5].f=nearv;
  p[6].f=farv;

  gl_add_op(p);
}

void glOrtho(float left, float right, float bottom, float top,
	float nearv, float farv)
{
	GLParam p[7];

	p[0].op = OP_Ortho;
	p[1].f = left;
	p[2].f = right;
	p[3].f = bottom;
	p[4].f = top;
	p[5].f = nearv;
	p[6].f = farv;

	gl_add_op(p);
}

/* lightening */

void glMaterialfv(int mode,int type,float *v)
{
  GLParam p[7];
  int i,n;

  if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_Material;
  p[1].i=mode;
  p[2].i=type;
  n=4;
  if (type == GL_SHININESS) n=1;
  else if (type == GL_COLOR_INDEXES) n=3;
  /* s31: read only as many floats as the pname has (the app's array may be
     exactly that long) */
  for(i=0;i<n;i++) p[3+i].f=v[i];
  for(i=n;i<4;i++) p[3+i].f=0;

  gl_add_op(p);
}

void glMaterialf(int mode,int type,float v)
{
  GLParam p[7];
  int i;

  p[0].op=OP_Material;
  p[1].i=mode;
  p[2].i=type;
  p[3].f=v;
  for(i=0;i<3;i++) p[4+i].f=0;

  gl_add_op(p);
}

void glColorMaterial(int mode,int type)
{
  GLParam p[3];

  p[0].op=OP_ColorMaterial;
  p[1].i=mode;
  p[2].i=type;

  gl_add_op(p);
}

void glLightfv(int light,int type,float *v)
{
  GLParam p[7];
  int i,n;

  if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }
  switch (type) {
  case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION:
    n = 4; break;
  case GL_SPOT_DIRECTION:
    n = 3; break;
  case GL_SPOT_EXPONENT: case GL_SPOT_CUTOFF: case GL_CONSTANT_ATTENUATION:
  case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
    n = 1; break;
  default:
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }
  p[0].op=OP_Light;
  p[1].i=light;
  p[2].i=type;
  for(i=0;i<4;i++) p[3+i].f= i < n ? v[i] : 0.0f;

  gl_add_op(p);
}


void glLightf(int light,int type,float v)
{
  GLParam p[7];
  int i;

  if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS) {
    gl_set_error(gl_get_context(), GL_INVALID_ENUM);
    return;
  }

  p[0].op=OP_Light;
  p[1].i=light;
  p[2].i=type;
  p[3].f=v;
  for(i=0;i<3;i++) p[4+i].f=0;

  gl_add_op(p);
}

void glLightModeli(int pname,int param)
{
  GLParam p[6];
  int i;

  p[0].op=OP_LightModel;
  p[1].i=pname;
  p[2].f=(float)param;
  for(i=0;i<3;i++) p[3+i].f=0;

  gl_add_op(p);
}

void glLightModelfv(int pname,float *param)
{
  GLParam p[6];
  int i,n=(pname == GL_LIGHT_MODEL_AMBIENT) ? 4 : 1;

  p[0].op=OP_LightModel;
  p[1].i=pname;
  for(i=0;i<4;i++) p[2+i].f= i < n ? param[i] : 0.0f;

  gl_add_op(p);
}

/* clear */

void glClear(int mask)
{
  GLParam p[2];

  if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
               GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) {
    gl_set_error(gl_get_context(), GL_INVALID_VALUE);
    return;
  }
  if (gl_get_context()->in_begin) {
    gl_set_error(gl_get_context(), GL_INVALID_OPERATION);
    return;
  }

  p[0].op=OP_Clear;
  p[1].i=mask;

  gl_add_op(p);
}

void glClearColor(float r,float g,float b,float a)
{
  GLParam p[5];

  p[0].op=OP_ClearColor;
  p[1].f=r;
  p[2].f=g;
  p[3].f=b;
  p[4].f=a;

  gl_add_op(p);
}

void glClearDepth(float depth)
{
  GLParam p[2];

  p[0].op=OP_ClearDepth;
  p[1].f=depth;

  gl_add_op(p);
}


/* textures */

/* s31: inside glNewList the pixels are unpacked NOW, with the pixel store
   state of this moment, and the list keeps the copy (GL 1.3 5.4): the
   application may free or reuse its buffer after the call */
static void *list_pixels(int width, int height, int *format, int *type, void *pixels)
{
  GLContext *c = gl_get_context();
  void *blk;
  if (!c->compile_flag || pixels == NULL) return pixels;
  blk = s31_unpack_copy(c, width, height, *format, *type, pixels);
  if (blk == NULL) return pixels;       /* the error is raised at execution */
  gl_list_own(c, blk);
  *format = S31_PACKED_RGBA;
  *type = GL_UNSIGNED_BYTE;
  return (char *)blk + S31_BLOCK_HDR;
}

void glTexImage2D( int target, int level, int components,
                   int width, int height, int border,
                   int format, int type, void *pixels)
{
  GLParam p[10];

  pixels = list_pixels(width, height, &format, &type, pixels);

  p[0].op=OP_TexImage2D;
  p[1].i=target;
  p[2].i=level;
  p[3].i=components;
  p[4].i=width;
  p[5].i=height;
  p[6].i=border;
  p[7].i=format;
  p[8].i=type;
  p[9].p=pixels;

  gl_add_op(p);
}


void tgl_glTexSubImage2D(int target, int level, int xoffset, int yoffset,
                         int width, int height, int format, int type,
                         const void *pixels)
{
  GLParam p[10];
  void *px = list_pixels(width, height, &format, &type, (void *)pixels);

  p[0].op=OP_TexSubImage2D;
  p[1].i=target;
  p[2].i=level;
  p[3].i=xoffset;
  p[4].i=yoffset;
  p[5].i=width;
  p[6].i=height;
  p[7].i=format;
  p[8].i=type;
  p[9].p=px;

  gl_add_op(p);
}

void glBindTexture(int target,int texture)
{
  GLParam p[3];

  p[0].op=OP_BindTexture;
  p[1].i=target;
  p[2].i=texture;

  gl_add_op(p);
}

void glTexEnvi(int target,int pname,int param)
{
  GLParam p[8];
  
  p[0].op=OP_TexEnv;
  p[1].i=target;
  p[2].i=pname;
  p[3].i=param;
  p[4].f=0;
  p[5].f=0;
  p[6].f=0;
  p[7].f=0;

  gl_add_op(p);
}

void glTexParameteri(int target,int pname,int param)
{
  GLParam p[8];
  
  p[0].op=OP_TexParameter;
  p[1].i=target;
  p[2].i=pname;
  p[3].i=param;
  p[4].f=0;
  p[5].f=0;
  p[6].f=0;
  p[7].f=0;

  gl_add_op(p);
}

void glPixelStorei(int pname,int param)
{
  GLParam p[3];

  /* s31: pixel storage is client state: executed at once, never compiled
     into a display list (GL 1.3 section 5.4) */
  p[0].op=OP_PixelStore;
  p[1].i=pname;
  p[2].i=param;

  glopPixelStore(gl_get_context(),p);
}

/* selection */

void glInitNames(void)
{
  GLParam p[1];

  p[0].op=OP_InitNames;

  gl_add_op(p);
}

void glPushName(unsigned int name)
{
  GLParam p[2];

  p[0].op=OP_PushName;
  p[1].i=name;

  gl_add_op(p);
}

void glPopName(void)
{
  GLParam p[1];

  p[0].op=OP_PopName;

  gl_add_op(p);
}

void glLoadName(unsigned int name)
{
  GLParam p[2];

  p[0].op=OP_LoadName;
  p[1].i=name;

  gl_add_op(p);
}

void 
glPolygonOffset(GLfloat factor, GLfloat units)
{
  GLParam p[3];
  p[0].op = OP_PolygonOffset;
  p[1].f = factor;
  p[2].f = units;

  gl_add_op(p);
}

/* Special Functions */

void glCallList(unsigned int list)
{
  GLParam p[2];
  GLContext *c=gl_ctx;

  p[0].op=OP_CallList;
  p[1].i=list;

  /* phase 4 L1: executing, straight to glopCallList - the RAM copy when
     S31GL_RAMTEXT made one (s31_ramtext.h); gl_add_op otherwise */
  if (!(c->compile_flag | c->print_flag)) {
    s31_rt.call_list(c,p);
    return;
  }
  gl_add_op(p);
}

void glFlush(void)
{
  /* nothing to do: gl/api/context.c handles glFlush/glFinish */
}

void glHint(int target,int mode)
{
  GLParam p[3];

  p[0].op=OP_Hint;
  p[1].i=target;
  p[2].i=mode;

  gl_add_op(p);
}

/* Non standard functions */

void glDebug(int mode)
{
  GLContext *c=gl_get_context();
  c->print_flag=mode;
  gl_update_vtx_run(c);
}

