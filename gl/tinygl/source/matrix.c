#include "zgl.h"
#include "s31_fmath.h"

void gl_print_matrix( const float *m)
{
   int i;

   for (i=0;i<4;i++) {
      fprintf(stderr,"%f %f %f %f\n", m[i], m[4+i], m[8+i], m[12+i] );
   }
}

static inline void gl_matrix_update(GLContext *c)
{
  /* s31: was "= (matrix_mode <= 1)", which CLEARED a pending modelview or
     projection update when the texture matrix changed next, and never
     re-evaluated apply_texture_matrix */
  c->matrix_model_projection_updated=1;
  /* s31 (phase 3a G14): vertex.c re-reads the projection's shape */
  if (c->matrix_mode == 1)
    c->xf_dirty |= 2;
}


void glopMatrixMode(GLContext *c,GLParam *p)
{
  int mode=p[1].i;
  switch(mode) {
  case GL_MODELVIEW:
    c->matrix_mode=0;
    break;
  case GL_PROJECTION:
    c->matrix_mode=1;
    break;
  case GL_TEXTURE:
    /* s31 (phase 5 O1): the active unit's stack, [3] for unit 1 */
    c->matrix_mode=2 + c->active_tex;
    break;
  default:
    gl_set_error(c, GL_INVALID_ENUM);
    break;
  }
}

void glopLoadMatrix(GLContext *c,GLParam *p)
{
  M4 *m;
  int i;
  
  GLParam *q;

  m=c->matrix_stack_ptr[c->matrix_mode];
  q=p+1;

  for(i=0;i<4;i++) {
    m->m[0][i]=q[0].f;
    m->m[1][i]=q[1].f;
    m->m[2][i]=q[2].f;
    m->m[3][i]=q[3].f;
    q+=4;
  }

  gl_matrix_update(c);
}

void glopLoadIdentity(GLContext *c,GLParam *p)
{

  gl_M4_Id(c->matrix_stack_ptr[c->matrix_mode]);

  gl_matrix_update(c);
}

void glopMultMatrix(GLContext *c,GLParam *p)
{
  M4 m;
  int i;

  GLParam *q;
  q=p+1;

  for(i=0;i<4;i++) {
    m.m[0][i]=q[0].f;
    m.m[1][i]=q[1].f;
    m.m[2][i]=q[2].f;
    m.m[3][i]=q[3].f;
    q+=4;
  }

  gl_M4_MulLeft(c->matrix_stack_ptr[c->matrix_mode],&m);

  gl_matrix_update(c);
}


void glopPushMatrix(GLContext *c,GLParam *p)
{
  int n=c->matrix_mode;
  M4 *m;

  if ((c->matrix_stack_ptr[n] - c->matrix_stack[n] + 1)
      >= c->matrix_stack_depth_max[n]) {
    gl_set_error(c, GL_STACK_OVERFLOW);
    return;
  }

  m=++c->matrix_stack_ptr[n];
  
  gl_M4_Move(&m[0],&m[-1]);

  gl_matrix_update(c);
}

void glopPopMatrix(GLContext *c,GLParam *p)
{
  int n=c->matrix_mode;

  if (c->matrix_stack_ptr[n] <= c->matrix_stack[n]) {
    gl_set_error(c, GL_STACK_UNDERFLOW);
    return;
  }
  c->matrix_stack_ptr[n]--;
  gl_matrix_update(c);
}


void glopRotate(GLContext *c,GLParam *p)
{
  M4 m;
  float u[3];
  float sint, cost;
  int dir_code;

  /* s31 (phase 3a G01): sin and cos of the angle in degrees, in float
     (s31_fmath.c). It was angle * M_PI / 180.0 in double, then libm sin
     and cos in double (musl's sinf/cosf compute in double as well): about
     20 soft-double calls per glRotate, reduced in radians after rounding
     the angle to float */
  s31_sincos_deg(p[1].f, &sint, &cost);
  u[0]=p[2].f;
  u[1]=p[3].f;
  u[2]=p[4].f;

  /* simple case detection */
  dir_code = ((u[0] != 0)<<2) | ((u[1] != 0)<<1) | (u[2] != 0);

  switch(dir_code) {
  case 0:
    gl_M4_Id(&m);
    break;
  case 4:
    gl_M4_RotateSC(&m,u[0] < 0 ? -sint : sint,cost,0);
    break;
  case 2:
    gl_M4_RotateSC(&m,u[1] < 0 ? -sint : sint,cost,1);
    break;
  case 1:
    gl_M4_RotateSC(&m,u[2] < 0 ? -sint : sint,cost,2);
    break;
  default:
    {
      /* normalize vector */
      float len = u[0]*u[0]+u[1]*u[1]+u[2]*u[2];
      if (len == 0.0f) return;
      len = 1.0f / sqrtf(len);
      u[0] *= len;
      u[1] *= len;
      u[2] *= len;

      /* fill in the values */
      m.m[3][0]=m.m[3][1]=m.m[3][2]=
        m.m[0][3]=m.m[1][3]=m.m[2][3]=0.0f;
      m.m[3][3]=1.0f;

      /* do the math. s31: m is row-major (m[row][col], v' = m v); upstream
         filled it column-major, so a rotation about any axis but x, y or z
         turned by -angle (found against Mesa by gl/tests/glx_pixels.c
         page 4, the same transposition as glopOrtho's) */
      m.m[0][0]=u[0]*u[0]+cost*(1-u[0]*u[0]);
      m.m[0][1]=u[0]*u[1]*(1-cost)-u[2]*sint;
      m.m[0][2]=u[2]*u[0]*(1-cost)+u[1]*sint;
      m.m[1][0]=u[0]*u[1]*(1-cost)+u[2]*sint;
      m.m[1][1]=u[1]*u[1]+cost*(1-u[1]*u[1]);
      m.m[1][2]=u[1]*u[2]*(1-cost)-u[0]*sint;
      m.m[2][0]=u[2]*u[0]*(1-cost)-u[1]*sint;
      m.m[2][1]=u[1]*u[2]*(1-cost)+u[0]*sint;
      m.m[2][2]=u[2]*u[2]+cost*(1-u[2]*u[2]);
    }
  }

  gl_M4_MulLeft(c->matrix_stack_ptr[c->matrix_mode],&m);

  gl_matrix_update(c);
}

void glopScale(GLContext *c,GLParam *p)
{
  float *m;
  float x=p[1].f,y=p[2].f,z=p[3].f;

  m=&c->matrix_stack_ptr[c->matrix_mode]->m[0][0];

  m[0] *= x;   m[1] *= y;   m[2]  *= z;
  m[4] *= x;   m[5] *= y;   m[6]  *= z;
  m[8] *= x;   m[9] *= y;   m[10] *= z;
  m[12] *= x;   m[13] *= y;   m[14] *= z;
  gl_matrix_update(c);
}

void glopTranslate(GLContext *c,GLParam *p)
{
  float *m;
  float x=p[1].f,y=p[2].f,z=p[3].f;

  m=&c->matrix_stack_ptr[c->matrix_mode]->m[0][0];

  m[3] = m[0] * x + m[1] * y + m[2]  * z + m[3];
  m[7] = m[4] * x + m[5] * y + m[6]  * z + m[7];
  m[11] = m[8] * x + m[9] * y + m[10] * z + m[11];
  m[15] = m[12] * x + m[13] * y + m[14] * z + m[15];

  gl_matrix_update(c);
}


void glopFrustum(GLContext *c,GLParam *p)
{
  float *r;
  M4 m;
  float left=p[1].f;
  float right=p[2].f;
  float bottom=p[3].f;
  float top=p[4].f;
  float nearv=p[5].f;
  float farp=p[6].f;
  float x,y,A,B,C,D;

  if (nearv <= 0 || farp <= 0 || left == right || bottom == top ||
      nearv == farp) {
    gl_set_error(c, GL_INVALID_VALUE);
    return;
  }

  x = (2.0f*nearv) / (right-left);
  y = (2.0f*nearv) / (top-bottom);
  A = (right+left) / (right-left);
  B = (top+bottom) / (top-bottom);
  C = -(farp+nearv) / ( farp-nearv);
  D = -(2.0f*farp*nearv) / (farp-nearv);

  r=&m.m[0][0];
  r[0]= x; r[1]=0; r[2]=A; r[3]=0;
  r[4]= 0; r[5]=y; r[6]=B; r[7]=0;
  r[8]= 0; r[9]=0; r[10]=C; r[11]=D;
  r[12]= 0; r[13]=0; r[14]=-1; r[15]=0;

  gl_M4_MulLeft(c->matrix_stack_ptr[c->matrix_mode],&m);

  gl_matrix_update(c);
}

/* thanks mesa */
void glopOrtho(GLContext *c,GLParam *p)
{
	float left = p[1].f;
	float right = p[2].f;
	float bottom = p[3].f;
	float top = p[4].f;
	float nearv = p[5].f;
	float farv = p[6].f;
	float x, y, z;
	float tx, ty, tz;
	float m[16];

	if (left == right || bottom == top || nearv == farv) {
		gl_set_error(c, GL_INVALID_VALUE);
		return;
	}

	x = 2.0f / (right-left);
	y = 2.0f / (top-bottom);
	z = -2.0f / (farv-nearv);
	tx = -(right+left) / (right-left);
	ty = -(top+bottom) / (top-bottom);
	tz = -(farv+nearv) / (farv-nearv);

	/* s31: M4 is row-major (m[row][col]); upstream copied Mesa's
	   column-major m[col*4+row], which transposed the matrix and put the
	   translation in the w row: every asymmetric glOrtho (the 2D
	   glOrtho(0,w,h,0) of every game) drew in the wrong place */
	#define M(row,col)  m[(row)*4+(col)]
		M(0,0) = x;     M(0,1) = 0.0F;  M(0,2) = 0.0F;  M(0,3) = tx;
		M(1,0) = 0.0F;  M(1,1) = y;     M(1,2) = 0.0F;  M(1,3) = ty;
		M(2,0) = 0.0F;  M(2,1) = 0.0F;  M(2,2) = z;     M(2,3) = tz;
		M(3,0) = 0.0F;  M(3,1) = 0.0F;  M(3,2) = 0.0F;  M(3,3) = 1.0F;
	#undef M

	gl_M4_MulLeft(c->matrix_stack_ptr[c->matrix_mode], (M4 *)m);
	gl_matrix_update(c);
}

