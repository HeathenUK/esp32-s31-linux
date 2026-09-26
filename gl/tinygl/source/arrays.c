#include "zgl.h"
#include "s31_float.h"
#include <stdio.h>

/*
 * Vertex arrays.
 *
 * s31 rewrite of TinyGL's arrays.c (erysdren's DrawElements/DrawArrays
 * kept in shape). What changed and why:
 *  - strides are in BYTES and 0 means tightly packed, as GL defines them;
 *    TinyGL added the stride to the element size as a count of floats;
 *  - every GL 1.1 type is fetched (colour as UNSIGNED_BYTE is the common
 *    case, GL_DOUBLE is GLtron's), normalised where GL says so; TinyGL
 *    asserted GL_FLOAT and read anything else as floats;
 *  - the normal array wrote Z twice (Z = 0), and a colour from an array
 *    left the integer flat-shading colour as stack garbage;
 *  - client state (enable/disable, the pointers) is executed at once and
 *    never compiled into a display list; glDrawArrays/glDrawElements/
 *    glArrayElement inside glNewList compile the vertices they reference,
 *    not the application's pointers (GL 1.3 section 5.4);
 *  - the edge-flag array exists.
 */

#define VERTEX_ARRAY   0x0001
#define COLOR_ARRAY    0x0002
#define NORMAL_ARRAY   0x0004
#define TEXCOORD_ARRAY 0x0008
#define EDGEFLAG_ARRAY 0x0010
#define INDEX_ARRAY    0x0020
/* phase 5 O1: texture unit 1's coordinate array (glClientActiveTexture;
   s31_mtex.c tc_swap exchanges it with TEXCOORD_ARRAY for the queries) */
#define TEXCOORD1_ARRAY 0x0040

static int type_size(int type)
{
  switch (type) {
  case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
  case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
  case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
  case GL_DOUBLE: return 8;
  default: return 0;
  }
}

/* n components of type at p into out[]; norm: map integers to [-1,1]/[0,1]
   as GL does for colours and normals */
static void fetch(const void *p, int type, int n, int norm, float *out)
{
  int i;
  switch (type) {
  case GL_FLOAT:
    for (i = 0; i < n; i++) out[i] = ((const float *)p)[i];
    break;
  case GL_DOUBLE:
    for (i = 0; i < n; i++) out[i] = s31_d2f(((const double *)p)[i]);
    break;
  case GL_UNSIGNED_BYTE:
    for (i = 0; i < n; i++) {
      unsigned int v = ((const unsigned char *)p)[i];
      out[i] = norm ? v * (1.0f / 255.0f) : (float)v;
    }
    break;
  case GL_BYTE:
    for (i = 0; i < n; i++) {
      int v = ((const signed char *)p)[i];
      out[i] = norm ? (2 * v + 1) * (1.0f / 255.0f) : (float)v;
    }
    break;
  case GL_UNSIGNED_SHORT:
    for (i = 0; i < n; i++) {
      unsigned int v = ((const unsigned short *)p)[i];
      out[i] = norm ? v * (1.0f / 65535.0f) : (float)v;
    }
    break;
  case GL_SHORT:
    for (i = 0; i < n; i++) {
      int v = ((const short *)p)[i];
      out[i] = norm ? (2 * v + 1) * (1.0f / 65535.0f) : (float)v;
    }
    break;
  case GL_UNSIGNED_INT:
    for (i = 0; i < n; i++) {
      unsigned int v = ((const unsigned int *)p)[i];
      out[i] = norm ? (float)(v >> 8) * (1.0f / 16777215.0f) : (float)v;
    }
    break;
  case GL_INT:
    for (i = 0; i < n; i++) {
      int v = ((const int *)p)[i];
      out[i] = norm ? (float)(v >> 7) * (1.0f / 16777215.0f) : (float)v;
    }
    break;
  default:
    for (i = 0; i < n; i++) out[i] = 0.0f;
  }
}

#define ELEM(base, bstride, idx) ((const char *)(base) + (idx) * (bstride))

/* the attribute values element idx supplies, as immediate-mode ops would */
static void array_element_params(GLContext *c, int idx,
                                 GLParam *col, GLParam *nor, GLParam *tex,
                                 GLParam *edge, GLParam *ver)
{
  int states = c->client_states;
  float v[4];

  if (states & COLOR_ARRAY) {
    int size = c->color_array_size;
    float r, g, b;
    v[3] = 1.0f;
    fetch(ELEM(c->color_array, c->color_array_bstride, idx),
          c->color_array_type, size, 1, v);
    col[0].op = OP_Color;
    col[1].f = v[0]; col[2].f = v[1]; col[3].f = v[2]; col[4].f = v[3];
    r = v[0] < 0 ? 0 : (v[0] > 1 ? 1 : v[0]);
    g = v[1] < 0 ? 0 : (v[1] > 1 ? 1 : v[1]);
    b = v[2] < 0 ? 0 : (v[2] > 1 ? 1 : v[2]);
    col[5].ui = (unsigned int)(r * (ZB_POINT_RED_MAX - ZB_POINT_RED_MIN) + ZB_POINT_RED_MIN);
    col[6].ui = (unsigned int)(g * (ZB_POINT_GREEN_MAX - ZB_POINT_GREEN_MIN) + ZB_POINT_GREEN_MIN);
    col[7].ui = (unsigned int)(b * (ZB_POINT_BLUE_MAX - ZB_POINT_BLUE_MIN) + ZB_POINT_BLUE_MIN);
  }
  if (states & NORMAL_ARRAY) {
    fetch(ELEM(c->normal_array, c->normal_array_bstride, idx),
          c->normal_array_type, 3, 1, v);
    nor[0].op = OP_Normal;
    nor[1].f = v[0]; nor[2].f = v[1]; nor[3].f = v[2];
  }
  if (states & TEXCOORD_ARRAY) {
    int size = c->texcoord_array_size;
    v[1] = 0.0f; v[2] = 0.0f; v[3] = 1.0f;
    fetch(ELEM(c->texcoord_array, c->texcoord_array_bstride, idx),
          c->texcoord_array_type, size, 0, v);
    tex[0].op = OP_TexCoord;
    tex[1].f = v[0]; tex[2].f = v[1]; tex[3].f = v[2]; tex[4].f = v[3];
  }
  if (states & EDGEFLAG_ARRAY) {
    edge[0].op = OP_EdgeFlag;
    edge[1].i = *(const unsigned char *)ELEM(c->edge_flag_array,
                                             c->edge_flag_array_stride ?
                                             c->edge_flag_array_stride : 1, idx);
  }
  if (states & VERTEX_ARRAY) {
    int size = c->vertex_array_size;
    v[2] = 0.0f; v[3] = 1.0f;
    fetch(ELEM(c->vertex_array, c->vertex_array_bstride, idx),
          c->vertex_array_type, size, 0, v);
    ver[0].op = OP_Vertex;
    ver[1].f = v[0]; ver[2].f = v[1]; ver[3].f = v[2]; ver[4].f = v[3];
  }
}

/* phase 5 O1: texture unit 1's array element (glClientActiveTexture),
   as a glMultiTexCoord op; out of line, so an element without it costs
   one test of the state word it already holds */
__attribute__((noinline))
static void array_tex1(GLContext *c, int idx, GLParam *tex1)
{
  const GLTexClient *a = &c->tc1;
  float v[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  fetch(ELEM(a->array, a->bstride, idx), a->type, a->size, 0, v);
  tex1[0].op = OP_MultiTexCoord;
  tex1[1].i = 1;
  tex1[2].f = v[0]; tex1[3].f = v[1]; tex1[4].f = v[2]; tex1[5].f = v[3];
}

__attribute__((noinline))
static void array_tex1_run(GLContext *c, int idx)
{
  GLParam tex1[6];
  array_tex1(c, idx, tex1);
  glopMultiTexCoord(c, tex1);
}

void
glopArrayElement(GLContext *c, GLParam *param)
{
  GLParam col[8], nor[4], tex[5], edge[2], ver[5];
  int states = c->client_states;

  array_element_params(c, param[1].i, col, nor, tex, edge, ver);
  if (states & COLOR_ARRAY) glopColor(c, col);
  if (states & NORMAL_ARRAY) glopNormal(c, nor);
  if (states & TEXCOORD_ARRAY) glopTexCoord(c, tex);
  if (states & TEXCOORD1_ARRAY) array_tex1_run(c, param[1].i);
  if (states & EDGEFLAG_ARRAY) glopEdgeFlag(c, edge);
  if (states & VERTEX_ARRAY) glopVertex(c, ver);
}

/* inside glNewList: the element's values become ordinary ops */
static void compile_element(GLContext *c, int idx)
{
  GLParam col[8], nor[4], tex[5], edge[2], ver[5], tex1[6];
  int states = c->client_states;

  array_element_params(c, idx, col, nor, tex, edge, ver);
  if (states & COLOR_ARRAY) gl_add_op(col);
  if (states & NORMAL_ARRAY) gl_add_op(nor);
  if (states & TEXCOORD_ARRAY) gl_add_op(tex);
  if (states & TEXCOORD1_ARRAY) {
    array_tex1(c, idx, tex1);
    gl_add_op(tex1);
  }
  if (states & EDGEFLAG_ARRAY) gl_add_op(edge);
  if (states & VERTEX_ARRAY) gl_add_op(ver);
}

void
glArrayElement(GLint i)
{
  GLContext *c = gl_get_context();
  GLParam p[2];

  if (c->compile_flag) {
    compile_element(c, i);
    return;
  }
  p[0].op = OP_ArrayElement;
  p[1].i = i;
  glopArrayElement(c, p);
}

static int client_bit(int array)
{
  switch (array) {
  case GL_VERTEX_ARRAY: return VERTEX_ARRAY;
  case GL_NORMAL_ARRAY: return NORMAL_ARRAY;
  case GL_COLOR_ARRAY: return COLOR_ARRAY;
  case GL_TEXTURE_COORD_ARRAY: return TEXCOORD_ARRAY;
  case GL_EDGE_FLAG_ARRAY: return EDGEFLAG_ARRAY;
  case GL_INDEX_ARRAY: return INDEX_ARRAY;   /* no colour-index mode */
  default: return 0;
  }
}

void
glopEnableClientState(GLContext *c, GLParam *p)
{
  c->client_states |= p[1].i;
}

void
glEnableClientState(GLenum array)
{
  GLContext *c = gl_get_context();
  int bit = client_bit(array);
  if (!bit) { gl_set_error(c, GL_INVALID_ENUM); return; }
  /* phase 5 O1: the texture-coordinate array of the client-active unit */
  if (bit == TEXCOORD_ARRAY && c->client_tex) bit = TEXCOORD1_ARRAY;
  c->client_states |= bit;
}

void
glopDisableClientState(GLContext *c, GLParam *p)
{
  c->client_states &= ~p[1].i;   /* s31 (phase 3a): was &= (never reached:
                                    glDisableClientState sets state directly) */
}

void
glDisableClientState(GLenum array)
{
  GLContext *c = gl_get_context();
  int bit = client_bit(array);
  if (!bit) { gl_set_error(c, GL_INVALID_ENUM); return; }
  if (bit == TEXCOORD_ARRAY && c->client_tex) bit = TEXCOORD1_ARRAY;
  c->client_states &= ~bit;
}

int s31_client_state(GLContext *c, int array)
{
  int bit = client_bit(array);
  return bit ? (c->client_states & bit) != 0 : -1;
}

/* the ops are kept for the op table; the entry points set state directly */
void glopVertexPointer(GLContext *c, GLParam *p) { (void)c; (void)p; }
void glopColorPointer(GLContext *c, GLParam *p) { (void)c; (void)p; }
void glopNormalPointer(GLContext *c, GLParam *p) { (void)c; (void)p; }
void glopTexCoordPointer(GLContext *c, GLParam *p) { (void)c; (void)p; }

static int pointer_args(GLContext *c, int size, int minsize, int maxsize,
                        int type, int stride)
{
  if (size < minsize || size > maxsize || stride < 0) {
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  if (type_size(type) == 0) {
    gl_set_error(c, GL_INVALID_ENUM);
    return 0;
  }
  return 1;
}

void
glVertexPointer(GLint size, GLenum type, GLsizei stride,
                const GLvoid *pointer)
{
  GLContext *c = gl_get_context();
  if (!pointer_args(c, size, 2, 4, type, stride)) return;
  if (type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT ||
      type == GL_UNSIGNED_INT) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  c->vertex_array_size = size;
  c->vertex_array_type = type;
  c->vertex_array_stride = stride;
  c->vertex_array_bstride = stride ? stride : size * type_size(type);
  c->vertex_array = (void *)pointer;
}

void
glColorPointer(GLint size, GLenum type, GLsizei stride,
               const GLvoid *pointer)
{
  GLContext *c = gl_get_context();
  if (!pointer_args(c, size, 3, 4, type, stride)) return;
  c->color_array_size = size;
  c->color_array_type = type;
  c->color_array_stride = stride;
  c->color_array_bstride = stride ? stride : size * type_size(type);
  c->color_array = (void *)pointer;
}

void
glNormalPointer(GLenum type, GLsizei stride,
                const GLvoid *pointer)
{
  GLContext *c = gl_get_context();
  if (!pointer_args(c, 3, 3, 3, type, stride)) return;
  if (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT ||
      type == GL_UNSIGNED_INT) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  c->normal_array_type = type;
  c->normal_array_stride = stride;
  c->normal_array_bstride = stride ? stride : 3 * type_size(type);
  c->normal_array = (void *)pointer;
}

void
glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
                  const GLvoid *pointer)
{
  GLContext *c = gl_get_context();
  if (!pointer_args(c, size, 1, 4, type, stride)) return;
  if (type == GL_BYTE || type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT ||
      type == GL_UNSIGNED_INT) {
    gl_set_error(c, GL_INVALID_ENUM);
    return;
  }
  if (c->client_tex) {
    /* phase 5 O1: the client-active unit's */
    GLTexClient *a = &c->tc1;
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->bstride = stride ? stride : size * type_size(type);
    a->array = (void *)pointer;
    return;
  }
  c->texcoord_array_size = size;
  c->texcoord_array_type = type;
  c->texcoord_array_stride = stride;
  c->texcoord_array_bstride = stride ? stride : size * type_size(type);
  c->texcoord_array = (void *)pointer;
}

void tgl_edge_flag_pointer(int stride, const void *pointer)
{
  GLContext *c = gl_get_context();
  if (stride < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
  c->edge_flag_array_stride = stride;
  c->edge_flag_array = (void *)pointer;
}

static int index_at(const void *indices, int type, int i)
{
  switch (type) {
  case GL_UNSIGNED_BYTE: return ((const GLubyte *)indices)[i];
  case GL_UNSIGNED_SHORT: return ((const GLushort *)indices)[i];
  default: return (int)((const GLuint *)indices)[i];
  }
}

/* s31 (phase 3a G14): the vertex cache's storage, at the first use */
static int vcache_ready(GLContext *c)
{
  char *b;
  if (c->vc) return 1;
  b = gl_zalloc(TGL_VCACHE * (sizeof(GLVertex) + sizeof(int) + sizeof(unsigned int)));
  if (!b) return 0;                /* no cache: the uncached path */
  c->vc = (GLVertex *)b;
  c->vc_idx = (int *)(b + TGL_VCACHE * sizeof(GLVertex));
  c->vc_tag = (unsigned int *)(c->vc_idx + TGL_VCACHE);
  c->vc_gen = 0;
  return 1;
}

/*
 * erysdren
 */

void glopDrawElements(GLContext *c, GLParam *p)
{
	/* variables */
	GLenum mode = p[1].i;
	GLsizei count = p[2].i;
	GLenum type = p[3].i;
	GLvoid *indices = p[4].p;
	int i;
	GLParam param_ptr[2];
	GLParam param_mode[2];

	/* setup params */
	param_ptr[0].op = OP_ArrayElement;

	param_mode[0].op = OP_Begin;
	param_mode[1].i = mode;

	glopBegin(c, param_mode);
	/* s31 (phase 3a G14): a post-transform vertex cache. An indexed mesh
	   names each vertex several times (a grid ~6), and TinyGL fetched,
	   transformed, lit and projected it every time. Within one call
	   nothing can change the result, so the processed vertex is kept,
	   direct-mapped by its index (TGL_VCACHE entries: a grid row of up to
	   TGL_VCACHE - 1 vertices is still there for the next row), and a hit
	   copies it into the primitive. Only with a vertex array (else no
	   vertex is emitted) and a live primitive. The current colour, normal
	   and texture coordinate after the call are those of the last miss,
	   not the last index: GL 1.3 2.8 leaves them undefined. */
	if ((c->client_states & VERTEX_ARRAY) && c->begin_type != TGL_BEGIN_DISCARD &&
	    count > 3 && vcache_ready(c)) {
		unsigned int gen = ++c->vc_gen;
		/* phase 5 O1: with texture unit 1 on, the copies take its
		   coordinates too (the vertex's last field) */
		void (*vi)(GLContext *, GLParam *, const GLVertex *, GLVertex *) =
			c->tu1_on ? gl_vertex_indexed_mt : gl_vertex_indexed;
		for (i = 0; i < count; i++) {
			int idx = index_at(indices, type, i);
			int s = idx & (TGL_VCACHE - 1);
			if (c->vc_tag[s] == gen && c->vc_idx[s] == idx) {
				vi(c, NULL, &c->vc[s], NULL);
			} else {
				GLParam col[8], nor[4], tex[5], edge[2], ver[5];
				int states = c->client_states;
				array_element_params(c, idx, col, nor, tex, edge, ver);
				if (states & COLOR_ARRAY) glopColor(c, col);
				if (states & NORMAL_ARRAY) glopNormal(c, nor);
				if (states & TEXCOORD_ARRAY) glopTexCoord(c, tex);
				if (states & TEXCOORD1_ARRAY) array_tex1_run(c, idx);
				if (states & EDGEFLAG_ARRAY) glopEdgeFlag(c, edge);
				c->vc_tag[s] = gen;
				c->vc_idx[s] = idx;
				vi(c, ver, NULL, &c->vc[s]);
			}
		}
	} else {
		for (i = 0; i < count; i++)
		{
			param_ptr[1].i = index_at(indices, type, i);
			glopArrayElement(c, param_ptr);
		}
	}
	glopEnd(c, NULL);
}

static int draw_args(GLContext *c, int mode, int count)
{
  if (mode < GL_POINTS || mode > GL_POLYGON) {
    gl_set_error(c, GL_INVALID_ENUM);
    return 0;
  }
  if (count < 0) {
    gl_set_error(c, GL_INVALID_VALUE);
    return 0;
  }
  if (c->in_begin) {
    gl_set_error(c, GL_INVALID_OPERATION);
    return 0;
  }
  return 1;
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type,
				const GLvoid *indices)
{
	GLContext *c = gl_get_context();
	GLParam p[5];
	int i;

	if (!draw_args(c, mode, count)) return;
	if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT &&
	    type != GL_UNSIGNED_INT) {
		gl_set_error(c, GL_INVALID_ENUM);
		return;
	}
	if (!(c->client_states & VERTEX_ARRAY) || count == 0) return;

	if (c->compile_flag) {
		p[0].op = OP_Begin; p[1].i = mode; gl_add_op(p);
		for (i = 0; i < count; i++)
			compile_element(c, index_at(indices, type, i));
		p[0].op = OP_End; gl_add_op(p);
		return;
	}
	p[0].op = OP_DrawElements;
	p[1].i = mode;
	p[2].i = count;
	p[3].i = type;
	p[4].p = (void *)indices;
	glopDrawElements(c, p);
}

/* probably a hack */
/* mesa's version of this function is way more complex */
void glopDrawArrays(GLContext *c, GLParam *p)
{
	/* variables */
	int i;
	GLenum mode = p[1].i;
	GLint first = p[2].i;
	GLsizei count = p[3].i;
	GLParam param_element[2];
	GLParam param_begin[2];

	param_begin[0].op = OP_Begin;
	param_begin[1].i = mode;

	param_element[0].op = OP_ArrayElement;

	/* begin */
	glopBegin(c, param_begin);

	/* do elements */
	for (i = 0; i < count; i++)
	{
		param_element[1].i = first + i;
		glopArrayElement(c, param_element);
	}

	/* end */
	glopEnd(c, NULL);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	GLContext *c = gl_get_context();
	GLParam p[4];
	int i;

	if (!draw_args(c, mode, count)) return;
	if (first < 0) { gl_set_error(c, GL_INVALID_VALUE); return; }
	if (!(c->client_states & VERTEX_ARRAY) || count == 0) return;

	if (c->compile_flag) {
		p[0].op = OP_Begin; p[1].i = mode; gl_add_op(p);
		for (i = 0; i < count; i++)
			compile_element(c, first + i);
		p[0].op = OP_End; gl_add_op(p);
		return;
	}
	p[0].op = OP_DrawArrays;
	p[1].i = mode;
	p[2].i = first;
	p[3].i = count;
	glopDrawArrays(c, p);
}
