/*
 * gl_core.c - GL 1.0-1.3 entry points other than per-vertex attributes
 * (gl_attrib.c) and queries (gl_get.c). s31, MIT.
 *
 * Compiled against the standard GL/gl.h, so every signature here is the
 * Khronos one; each converts to float and forwards to TinyGL through
 * tgl_bridge.h. Names missing here are exported as stubs by gen_stubs.c
 * (mkstubs.py), which s31gl_get_proc() does NOT return.
 */
#include <string.h>
#include "s31_api.h"

/* ------------------------------------------------------------ helpers */

static void d16(const GLdouble *m, GLfloat *f)
{
	int i;
	for (i = 0; i < 16; i++)
		f[i] = s31_d2f(m[i]);
}

static void transpose(const GLfloat *m, GLfloat *t)
{
	int i, j;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			t[i * 4 + j] = m[j * 4 + i];
}

/* ------------------------------------------------------------- framebuffer */

void GLAPIENTRY glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	tgl_glClearColor(r, g, b, a);
}

void GLAPIENTRY glClear(GLbitfield mask)
{
	tgl_glClear((int)mask);
}

void GLAPIENTRY glClearDepth(GLclampd depth)
{
	tgl_glClearDepth(s31_d2f(depth));
}

/* no colour-index mode: GL_INDEX_CLEAR_VALUE / GL_INDEX_WRITEMASK only */
void GLAPIENTRY glClearIndex(GLfloat c) { (void)c; }
void GLAPIENTRY glIndexMask(GLuint mask) { (void)mask; }

/* no accumulation buffer: glClearAccum is state, glAccum is an error
   (GL 1.3 section 4.2.4: "if there is no accumulation buffer ...
   INVALID_OPERATION") */
void GLAPIENTRY glClearAccum(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	(void)r; (void)g; (void)b; (void)a;
}

void GLAPIENTRY glAccum(GLenum op, GLfloat value)
{
	(void)value;
	if (op != GL_ACCUM && op != GL_LOAD && op != GL_RETURN &&
	    op != GL_MULT && op != GL_ADD) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	S31_ERR(GL_INVALID_OPERATION);
}

void GLAPIENTRY glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	tgl_state_i(S31_ST_COLOR_MASK, r != 0, g != 0, b != 0, a != 0);
}

void GLAPIENTRY glDepthMask(GLboolean flag)
{
	tgl_state_i(S31_ST_DEPTH_MASK, flag != 0, 0, 0, 0);
}

void GLAPIENTRY glDepthFunc(GLenum func)
{
	tgl_state_i(S31_ST_DEPTH_FUNC, (int)func, 0, 0, 0);
}

void GLAPIENTRY glDepthRange(GLclampd n, GLclampd f)
{
	tgl_state_f(S31_ST_DEPTH_RANGE, s31_d2f(n), s31_d2f(f), 0, 0);
}

void GLAPIENTRY glAlphaFunc(GLenum func, GLclampf ref)
{
	tgl_state_alpha((int)func, ref);
}

void GLAPIENTRY glBlendFunc(GLenum s, GLenum d)
{
	tgl_state_i(S31_ST_BLEND_FUNC, (int)s, (int)d, 0, 0);
}

/* GL 1.2 imaging subset / GL 1.4 core: the GL_CONSTANT_* factors
   (zpipe.c factor()) */
void GLAPIENTRY glBlendColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	tgl_state_f(S31_ST_BLEND_COLOR, r, g, b, a);
}

/*
 * Phase 4 BLEND-EQ: glBlendEquation (GL 1.2 imaging subset, GL 1.4 core,
 * EXT_blend_minmax / EXT_blend_subtract), glBlendFuncSeparate (GL 1.4,
 * EXT_blend_func_separate) and glBlendEquationSeparate (GL 2.0,
 * EXT_blend_equation_separate). All five equations are drawn
 * (zpipe.c zp_out_fn). The separate ALPHA factors and the alpha equation
 * are recorded (glGet, glPushAttrib) and have no stored effect, exactly:
 * there is no destination alpha plane (GL_ALPHA_BITS 0), so the blended
 * alpha is never written, and the RGB result reads destination alpha as 1
 * whatever was blended (raster_sel.c folds GL_DST_ALPHA to GL_ONE).
 *
 * Plan 4.2 withheld these two names until SDL2 was built without its GL
 * render driver: SDL2's SDL_render_gl.c loads glBlendEquation and
 * glBlendFuncSeparate, and exporting them would have let "opengl" become
 * every SDL2 app's renderer. The SDL2 build is now GL contexts ON, the GL
 * render driver OFF (plan 4.2 option B), which removes that reason; SDL2's
 * own test/testgl2 loads the same function list (SDL_glfuncs.h) and
 * failed without them. mkstubs.py exports them and core_test checks they
 * are found.
 */
void GLAPIENTRY glBlendColorEXT(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	glBlendColor(r, g, b, a);
}

void GLAPIENTRY glBlendEquation(GLenum mode)
{
	tgl_state_i(S31_ST_BLEND_EQ, (int)mode, (int)mode, 0, 0);
}

void GLAPIENTRY glBlendEquationEXT(GLenum mode)
{
	glBlendEquation(mode);
}

void GLAPIENTRY glBlendEquationSeparate(GLenum rgb, GLenum alpha)
{
	tgl_state_i(S31_ST_BLEND_EQ, (int)rgb, (int)alpha, 0, 0);
}

void GLAPIENTRY glBlendEquationSeparateEXT(GLenum rgb, GLenum alpha)
{
	glBlendEquationSeparate(rgb, alpha);
}

void GLAPIENTRY glBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da)
{
	tgl_state_i(S31_ST_BLEND_FUNC_SEP, (int)srgb, (int)drgb, (int)sa, (int)da);
}

void GLAPIENTRY glBlendFuncSeparateEXT(GLenum srgb, GLenum drgb, GLenum sa, GLenum da)
{
	glBlendFuncSeparate(srgb, drgb, sa, da);
}

void GLAPIENTRY glLogicOp(GLenum op)
{
	tgl_state_i(S31_ST_LOGIC_OP, (int)op, 0, 0, 0);
}

void GLAPIENTRY glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	tgl_state_i(S31_ST_SCISSOR, x, y, w, h);
}

static int valid_buffer(GLenum b)
{
	switch (b) {
	case GL_NONE: case GL_FRONT_LEFT: case GL_FRONT_RIGHT: case GL_BACK_LEFT:
	case GL_BACK_RIGHT: case GL_FRONT: case GL_BACK: case GL_LEFT:
	case GL_RIGHT: case GL_FRONT_AND_BACK: case GL_AUX0: case GL_AUX1:
	case GL_AUX2: case GL_AUX3:
		return 1;
	default:
		return 0;
	}
}

void GLAPIENTRY glDrawBuffer(GLenum mode)
{
	if (!valid_buffer(mode)) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	/* there is one colour buffer; drawing always goes to it */
	tgl_state_i(S31_ST_DRAW_BUFFER, (int)mode, 0, 0, 0);
}

void GLAPIENTRY glReadBuffer(GLenum mode)
{
	if (!valid_buffer(mode) || mode == GL_NONE) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_state_i(S31_ST_READ_BUFFER, (int)mode, 0, 0, 0);
}

/* phase 4 F8-STENCIL: honoured when the context has a stencil buffer
   (GLX_STENCIL_SIZE 8: gl/tinygl/source/zpipe.c zp_stencil_fn); with zero
   stencil bits the stencil test always passes and nothing is modified, so
   not running it is exact (GL 1.3 section 4.1.5) */
void GLAPIENTRY glStencilFunc(GLenum func, GLint ref, GLuint mask)
{
	tgl_state_i(S31_ST_STENCIL_FUNC, (int)func, ref, (int)mask, 0);
}

void GLAPIENTRY glStencilMask(GLuint mask)
{
	tgl_state_i(S31_ST_STENCIL_MASK, (int)mask, 0, 0, 0);
}

static int valid_stencil_op(GLenum op)
{
	/* GL 1.4 (EXT_stencil_wrap) adds the wrapping pair */
	return op == GL_KEEP || op == GL_ZERO || op == GL_REPLACE ||
	       op == GL_INCR || op == GL_DECR || op == GL_INVERT ||
	       op == GL_INCR_WRAP || op == GL_DECR_WRAP;
}

void GLAPIENTRY glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
	if (!valid_stencil_op(fail) || !valid_stencil_op(zfail) ||
	    !valid_stencil_op(zpass)) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_state_i(S31_ST_STENCIL_OP, (int)fail, (int)zfail, (int)zpass, 0);
}

void GLAPIENTRY glClearStencil(GLint s)
{
	tgl_state_i(S31_ST_STENCIL_CLEAR, s, 0, 0, 0);
}

/* ------------------------------------------------------------ rasterisation */

void GLAPIENTRY glCullFace(GLenum mode) { tgl_glCullFace((int)mode); }
void GLAPIENTRY glFrontFace(GLenum mode) { tgl_glFrontFace((int)mode); }
void GLAPIENTRY glShadeModel(GLenum mode) { tgl_glShadeModel((int)mode); }
void GLAPIENTRY glPolygonMode(GLenum face, GLenum mode)
{
	tgl_glPolygonMode((int)face, (int)mode);
}
void GLAPIENTRY glPolygonOffset(GLfloat factor, GLfloat units)
{
	tgl_glPolygonOffset(factor, units);
}
void GLAPIENTRY glPointSize(GLfloat size)
{
	tgl_state_f(S31_ST_POINT_SIZE, size, 0, 0, 0);
}
void GLAPIENTRY glLineWidth(GLfloat width)
{
	tgl_state_f(S31_ST_LINE_WIDTH, width, 0, 0, 0);
}
void GLAPIENTRY glLineStipple(GLint factor, GLushort pattern)
{
	tgl_state_i(S31_ST_LINE_STIPPLE, factor, pattern, 0, 0);
}

void GLAPIENTRY glEnable(GLenum cap) { tgl_glEnable((int)cap); }
void GLAPIENTRY glDisable(GLenum cap) { tgl_glDisable((int)cap); }

static int valid_hint_mode(GLenum m)
{
	return m == GL_FASTEST || m == GL_NICEST || m == GL_DONT_CARE;
}

void GLAPIENTRY glHint(GLenum target, GLenum mode)
{
	switch (target) {
	case GL_PERSPECTIVE_CORRECTION_HINT: case GL_POINT_SMOOTH_HINT:
	case GL_LINE_SMOOTH_HINT: case GL_POLYGON_SMOOTH_HINT: case GL_FOG_HINT:
	case GL_TEXTURE_COMPRESSION_HINT: case GL_GENERATE_MIPMAP_HINT:
		break;
	default:
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	if (!valid_hint_mode(mode)) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glHint((int)target, (int)mode);
}

/* ------------------------------------------------------------ primitives */

void GLAPIENTRY glBegin(GLenum mode) { tgl_glBegin((int)mode); }
void GLAPIENTRY glEnd(void) { tgl_glEnd(); }

/* ------------------------------------------------------------ transforms */

void GLAPIENTRY glMatrixMode(GLenum mode) { tgl_glMatrixMode((int)mode); }
void GLAPIENTRY glLoadIdentity(void) { tgl_glLoadIdentity(); }
void GLAPIENTRY glPushMatrix(void) { tgl_glPushMatrix(); }
void GLAPIENTRY glPopMatrix(void) { tgl_glPopMatrix(); }

void GLAPIENTRY glLoadMatrixf(const GLfloat *m) { tgl_glLoadMatrixf(m); }
void GLAPIENTRY glMultMatrixf(const GLfloat *m) { tgl_glMultMatrixf(m); }

void GLAPIENTRY glLoadMatrixd(const GLdouble *m)
{
	GLfloat f[16];
	d16(m, f);
	tgl_glLoadMatrixf(f);
}

void GLAPIENTRY glMultMatrixd(const GLdouble *m)
{
	GLfloat f[16];
	d16(m, f);
	tgl_glMultMatrixf(f);
}

void GLAPIENTRY glLoadTransposeMatrixf(const GLfloat m[16])
{
	GLfloat t[16];
	transpose(m, t);
	tgl_glLoadMatrixf(t);
}

void GLAPIENTRY glMultTransposeMatrixf(const GLfloat m[16])
{
	GLfloat t[16];
	transpose(m, t);
	tgl_glMultMatrixf(t);
}

void GLAPIENTRY glLoadTransposeMatrixd(const GLdouble m[16])
{
	GLfloat f[16], t[16];
	d16(m, f);
	transpose(f, t);
	tgl_glLoadMatrixf(t);
}

void GLAPIENTRY glMultTransposeMatrixd(const GLdouble m[16])
{
	GLfloat f[16], t[16];
	d16(m, f);
	transpose(f, t);
	tgl_glMultMatrixf(t);
}

void GLAPIENTRY glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
	tgl_glRotatef(a, x, y, z);
}
void GLAPIENTRY glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z)
{
	tgl_glRotatef(s31_d2f(a), s31_d2f(x), s31_d2f(y), s31_d2f(z));
}
void GLAPIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
	tgl_glTranslatef(x, y, z);
}
void GLAPIENTRY glTranslated(GLdouble x, GLdouble y, GLdouble z)
{
	tgl_glTranslatef(s31_d2f(x), s31_d2f(y), s31_d2f(z));
}
void GLAPIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z)
{
	tgl_glScalef(x, y, z);
}
void GLAPIENTRY glScaled(GLdouble x, GLdouble y, GLdouble z)
{
	tgl_glScalef(s31_d2f(x), s31_d2f(y), s31_d2f(z));
}

void GLAPIENTRY glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
			  GLdouble n, GLdouble f)
{
	tgl_glFrustum(s31_d2f(l), s31_d2f(r), s31_d2f(b), s31_d2f(t),
		      s31_d2f(n), s31_d2f(f));
}

void GLAPIENTRY glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
			GLdouble n, GLdouble f)
{
	tgl_glOrtho(s31_d2f(l), s31_d2f(r), s31_d2f(b), s31_d2f(t),
		    s31_d2f(n), s31_d2f(f));
}

void GLAPIENTRY glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	if (w < 0 || h < 0) {
		S31_ERR(GL_INVALID_VALUE);
		return;
	}
	/* the GLX layer checks the window size here and may rebind */
	s31_hook_viewport(x, y, w, h);
	tgl_glViewport(x, y, w, h);
}

/* ------------------------------------------------------------ display lists */

GLboolean GLAPIENTRY glIsList(GLuint list) { return tgl_glIsList(list) != 0; }
void GLAPIENTRY glDeleteLists(GLuint list, GLsizei range)
{
	tgl_glDeleteLists(list, range);
}
GLuint GLAPIENTRY glGenLists(GLsizei range) { return tgl_glGenLists(range); }
void GLAPIENTRY glNewList(GLuint list, GLenum mode) { tgl_glNewList(list, (int)mode); }
void GLAPIENTRY glEndList(void) { tgl_glEndList(); }
void GLAPIENTRY glCallList(GLuint list) { tgl_glCallList(list); }

void GLAPIENTRY glListBase(GLuint base)
{
	tgl_state_i(S31_ST_LIST_BASE, (int)base, 0, 0, 0);
}

void GLAPIENTRY glCallLists(GLsizei n, GLenum type, const GLvoid *lists)
{
	const unsigned char *b = lists;
	GLint base;
	int iv[16], kind;
	float fv[16];
	GLsizei i;

	if (n < 0) {
		S31_ERR(GL_INVALID_VALUE);
		return;
	}
	base = tgl_get(GL_LIST_BASE, iv, fv, &kind) == 1 ? iv[0] : 0;
	for (i = 0; i < n; i++) {
		GLuint id;
		switch (type) {
		case GL_BYTE: id = (GLuint)(GLint)((const GLbyte *)lists)[i]; break;
		case GL_UNSIGNED_BYTE: id = b[i]; break;
		case GL_SHORT: id = (GLuint)(GLint)((const GLshort *)lists)[i]; break;
		case GL_UNSIGNED_SHORT: id = ((const GLushort *)lists)[i]; break;
		case GL_INT: id = (GLuint)((const GLint *)lists)[i]; break;
		case GL_UNSIGNED_INT: id = ((const GLuint *)lists)[i]; break;
		case GL_FLOAT: id = (GLuint)((const GLfloat *)lists)[i]; break;
		case GL_2_BYTES: id = (b[2 * i] << 8) | b[2 * i + 1]; break;
		case GL_3_BYTES:
			id = (b[3 * i] << 16) | (b[3 * i + 1] << 8) | b[3 * i + 2];
			break;
		case GL_4_BYTES:
			id = ((GLuint)b[4 * i] << 24) | (b[4 * i + 1] << 16) |
			     (b[4 * i + 2] << 8) | b[4 * i + 3];
			break;
		default:
			S31_ERR(GL_INVALID_ENUM);
			return;
		}
		tgl_glCallList(id + base);
	}
}

/* ------------------------------------------------------------ selection */

GLint GLAPIENTRY glRenderMode(GLenum mode) { return tgl_glRenderMode((int)mode); }
void GLAPIENTRY glSelectBuffer(GLsizei size, GLuint *buffer)
{
	tgl_glSelectBuffer(size, buffer);
}
void GLAPIENTRY glInitNames(void) { tgl_glInitNames(); }
void GLAPIENTRY glLoadName(GLuint name) { tgl_glLoadName(name); }
void GLAPIENTRY glPushName(GLuint name) { tgl_glPushName(name); }
void GLAPIENTRY glPopName(void) { tgl_glPopName(); }

/* ------------------------------------------------------------ lighting */

static int light_n(GLenum pname)
{
	switch (pname) {
	case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION:
		return 4;
	case GL_SPOT_DIRECTION:
		return 3;
	default:
		return 1;
	}
}

void GLAPIENTRY glLightf(GLenum light, GLenum pname, GLfloat param)
{
	if (light_n(pname) != 1) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glLightf((int)light, (int)pname, param);
}

void GLAPIENTRY glLighti(GLenum light, GLenum pname, GLint param)
{
	glLightf(light, pname, (GLfloat)param);
}

void GLAPIENTRY glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{
	tgl_glLightfv((int)light, (int)pname, (float *)params);
}

void GLAPIENTRY glLightiv(GLenum light, GLenum pname, const GLint *params)
{
	GLfloat f[4] = { 0, 0, 0, 0 };
	int i, n = light_n(pname);
	/* colours map like colours; positions and directions do not */
	int color = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
	for (i = 0; i < n; i++)
		f[i] = color ? (2.0f * (GLfloat)params[i] + 1.0f) * (1.0f / 4294967295.0f)
			     : (GLfloat)params[i];
	tgl_glLightfv((int)light, (int)pname, f);
}

void GLAPIENTRY glLightModelf(GLenum pname, GLfloat param)
{
	GLfloat f[4] = { param, 0, 0, 0 };
	if (pname == GL_LIGHT_MODEL_AMBIENT) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glLightModelfv((int)pname, f);
}

void GLAPIENTRY glLightModeli(GLenum pname, GLint param)
{
	glLightModelf(pname, (GLfloat)param);
}

void GLAPIENTRY glLightModelfv(GLenum pname, const GLfloat *params)
{
	tgl_glLightModelfv((int)pname, (float *)params);
}

void GLAPIENTRY glLightModeliv(GLenum pname, const GLint *params)
{
	GLfloat f[4] = { 0, 0, 0, 0 };
	int i;
	if (pname == GL_LIGHT_MODEL_AMBIENT) {
		for (i = 0; i < 4; i++)
			f[i] = (2.0f * (GLfloat)params[i] + 1.0f) * (1.0f / 4294967295.0f);
	} else {
		f[0] = (GLfloat)params[0];
	}
	tgl_glLightModelfv((int)pname, f);
}

static int material_n(GLenum pname)
{
	switch (pname) {
	case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_EMISSION:
	case GL_AMBIENT_AND_DIFFUSE:
		return 4;
	case GL_SHININESS:
		return 1;
	case GL_COLOR_INDEXES:
		return 3;
	default:
		return 0;
	}
}

void GLAPIENTRY glMaterialf(GLenum face, GLenum pname, GLfloat param)
{
	if (pname != GL_SHININESS) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glMaterialf((int)face, (int)pname, param);
}

void GLAPIENTRY glMateriali(GLenum face, GLenum pname, GLint param)
{
	glMaterialf(face, pname, (GLfloat)param);
}

void GLAPIENTRY glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)
{
	if (material_n(pname) == 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glMaterialfv((int)face, (int)pname, (float *)params);
}

void GLAPIENTRY glMaterialiv(GLenum face, GLenum pname, const GLint *params)
{
	GLfloat f[4] = { 0, 0, 0, 0 };
	int i, n = material_n(pname);
	if (n == 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		f[i] = n == 4 ? (2.0f * (GLfloat)params[i] + 1.0f) * (1.0f / 4294967295.0f)
			      : (GLfloat)params[i];
	tgl_glMaterialfv((int)face, (int)pname, f);
}

void GLAPIENTRY glColorMaterial(GLenum face, GLenum mode)
{
	if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
	    (mode != GL_EMISSION && mode != GL_AMBIENT && mode != GL_DIFFUSE &&
	     mode != GL_SPECULAR && mode != GL_AMBIENT_AND_DIFFUSE)) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glColorMaterial((int)face, (int)mode);
}

/* ------------------------------------------------------------ fog (state) */

static void fog(GLenum pname, const GLfloat *v)
{
	switch (pname) {
	case GL_FOG_MODE:
		tgl_state_i(S31_ST_FOG_MODE, (int)v[0], 0, 0, 0);
		break;
	case GL_FOG_DENSITY:
		tgl_state_f(S31_ST_FOG_DENSITY, v[0], 0, 0, 0);
		break;
	case GL_FOG_START:
		tgl_state_f(S31_ST_FOG_START, v[0], 0, 0, 0);
		break;
	case GL_FOG_END:
		tgl_state_f(S31_ST_FOG_END, v[0], 0, 0, 0);
		break;
	case GL_FOG_INDEX:
		tgl_state_f(S31_ST_FOG_INDEX, v[0], 0, 0, 0);
		break;
	case GL_FOG_COLOR:
		tgl_state_f(S31_ST_FOG_COLOR, v[0], v[1], v[2], v[3]);
		break;
	case GL_FOG_COORD_SRC:
		break;	/* GL 1.4; accepted */
	default:
		S31_ERR(GL_INVALID_ENUM);
	}
}

void GLAPIENTRY glFogf(GLenum pname, GLfloat param)
{
	GLfloat v[4] = { param, 0, 0, 0 };
	if (pname == GL_FOG_COLOR) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	fog(pname, v);
}

void GLAPIENTRY glFogi(GLenum pname, GLint param)
{
	glFogf(pname, (GLfloat)param);
}

void GLAPIENTRY glFogfv(GLenum pname, const GLfloat *params)
{
	fog(pname, params);
}

void GLAPIENTRY glFogiv(GLenum pname, const GLint *params)
{
	GLfloat v[4] = { 0, 0, 0, 0 };
	int i;
	if (pname == GL_FOG_COLOR) {
		for (i = 0; i < 4; i++)
			v[i] = (2.0f * (GLfloat)params[i] + 1.0f) * (1.0f / 4294967295.0f);
	} else {
		v[0] = (GLfloat)params[0];
	}
	fog(pname, v);
}

/* ------------------------------------------------------------ textures */

void GLAPIENTRY glGenTextures(GLsizei n, GLuint *textures)
{
	tgl_glGenTextures(n, textures);
}

void GLAPIENTRY glDeleteTextures(GLsizei n, const GLuint *textures)
{
	tgl_glDeleteTextures(n, textures);
}

void GLAPIENTRY glBindTexture(GLenum target, GLuint texture)
{
	tgl_glBindTexture((int)target, (int)texture);
}

GLboolean GLAPIENTRY glIsTexture(GLuint texture)
{
	return tgl_is_texture(texture) != 0;
}

/* every texture is resident (there is no other memory) */
GLboolean GLAPIENTRY glAreTexturesResident(GLsizei n, const GLuint *textures,
					   GLboolean *residences)
{
	GLsizei i;
	if (n < 0) {
		S31_ERR(GL_INVALID_VALUE);
		return GL_FALSE;
	}
	for (i = 0; i < n; i++) {
		if (textures[i] == 0 || !tgl_is_texture(textures[i])) {
			S31_ERR(GL_INVALID_VALUE);
			return GL_FALSE;
		}
	}
	(void)residences;	/* GL: untouched when all are resident */
	return GL_TRUE;
}

/* priorities are a hint (GL 1.3 section 3.8.10) */
void GLAPIENTRY glPrioritizeTextures(GLsizei n, const GLuint *textures,
				     const GLclampf *priorities)
{
	(void)textures; (void)priorities;
	if (n < 0)
		S31_ERR(GL_INVALID_VALUE);
}

void GLAPIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat,
			     GLsizei width, GLsizei height, GLint border,
			     GLenum format, GLenum type, const GLvoid *pixels)
{
	tgl_glTexImage2D((int)target, level, internalformat, width, height,
			 border, (int)format, (int)type, (void *)pixels);
}

void GLAPIENTRY glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
				GLint yoffset, GLsizei width, GLsizei height,
				GLenum format, GLenum type, const GLvoid *pixels)
{
	tgl_glTexSubImage2D((int)target, level, xoffset, yoffset, width, height,
			    (int)format, (int)type, pixels);
}

/* phase 5 O1: GL_RGB_SCALE and GL_ALPHA_SCALE are floats (1.0, 2.0 or
   4.0), whichever form sets them: the value goes to the core as given */
static int env_scale(GLenum pname)
{
	return pname == GL_RGB_SCALE || pname == GL_ALPHA_SCALE;
}

void GLAPIENTRY glTexEnvi(GLenum target, GLenum pname, GLint param)
{
	if (pname == GL_TEXTURE_ENV_COLOR) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	if (env_scale(pname)) {
		tgl_tex_envf((int)target, (int)pname, (GLfloat)param);
		return;
	}
	tgl_glTexEnvi((int)target, (int)pname, param);
}

void GLAPIENTRY glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
	if (env_scale(pname)) {
		tgl_tex_envf((int)target, (int)pname, param);
		return;
	}
	glTexEnvi(target, pname, (GLint)param);
}

void GLAPIENTRY glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
	if (pname == GL_TEXTURE_ENV_COLOR) {
		if (target != GL_TEXTURE_ENV) {
			S31_ERR(GL_INVALID_ENUM);
			return;
		}
		tgl_state_f(S31_ST_TEXENV_COLOR, params[0], params[1], params[2],
			    params[3]);
		return;
	}
	glTexEnvf(target, pname, params[0]);
}

void GLAPIENTRY glTexEnviv(GLenum target, GLenum pname, const GLint *params)
{
	if (pname == GL_TEXTURE_ENV_COLOR) {
		GLfloat f[4];
		int i;
		for (i = 0; i < 4; i++)
			f[i] = (2.0f * (GLfloat)params[i] + 1.0f) * (1.0f / 4294967295.0f);
		glTexEnvfv(target, pname, f);
		return;
	}
	glTexEnvi(target, pname, params[0]);
}

static int tex_param_float(GLenum pname)
{
	return pname == GL_TEXTURE_PRIORITY || pname == GL_TEXTURE_MIN_LOD ||
	       pname == GL_TEXTURE_MAX_LOD;
}

void GLAPIENTRY glTexParameteri(GLenum target, GLenum pname, GLint param)
{
	if (pname == GL_TEXTURE_BORDER_COLOR) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	if (tex_param_float(pname)) {
		GLfloat f = (GLfloat)param;
		tgl_tex_parameterf((int)target, (int)pname, &f, 1);
		return;
	}
	tgl_glTexParameteri((int)target, (int)pname, param);
}

void GLAPIENTRY glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
	if (pname == GL_TEXTURE_BORDER_COLOR) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	if (tex_param_float(pname)) {
		tgl_tex_parameterf((int)target, (int)pname, &param, 1);
		return;
	}
	tgl_glTexParameteri((int)target, (int)pname, (GLint)param);
}

void GLAPIENTRY glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
	if (pname == GL_TEXTURE_BORDER_COLOR) {
		tgl_tex_parameterf((int)target, (int)pname, params, 4);
		return;
	}
	glTexParameterf(target, pname, params[0]);
}

void GLAPIENTRY glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
	if (pname == GL_TEXTURE_BORDER_COLOR) {
		GLfloat f[4];
		int i;
		for (i = 0; i < 4; i++)
			f[i] = (2.0f * (GLfloat)params[i] + 1.0f) * (1.0f / 4294967295.0f);
		tgl_tex_parameterf((int)target, (int)pname, f, 4);
		return;
	}
	glTexParameteri(target, pname, params[0]);
}

void GLAPIENTRY glPixelStorei(GLenum pname, GLint param)
{
	tgl_glPixelStorei((int)pname, param);
}

void GLAPIENTRY glPixelStoref(GLenum pname, GLfloat param)
{
	/* GL: booleans are param != 0, integers are rounded */
	tgl_glPixelStorei((int)pname, (GLint)(param < 0 ? param - 0.5f : param + 0.5f));
}

/* phase 5 O1: two texture units (GL_MAX_TEXTURE_UNITS = 2; one under
   S31GL_MTEX=0), tinygl/source/s31_mtex.c */
void GLAPIENTRY glActiveTexture(GLenum texture)
{
	tgl_active_texture((int)texture);
}

void GLAPIENTRY glClientActiveTexture(GLenum texture)
{
	tgl_client_active_texture((int)texture);
}

void GLAPIENTRY glActiveTextureARB(GLenum texture)
	__attribute__((alias("glActiveTexture")));
void GLAPIENTRY glClientActiveTextureARB(GLenum texture)
	__attribute__((alias("glClientActiveTexture")));

/* no multisample buffers: the coverage state has no effect */
void GLAPIENTRY glSampleCoverage(GLclampf value, GLboolean invert)
{
	(void)value; (void)invert;
}

/* ------------------------------------------------------------ arrays */

void GLAPIENTRY glEnableClientState(GLenum array) { tgl_glEnableClientState((int)array); }
void GLAPIENTRY glDisableClientState(GLenum array) { tgl_glDisableClientState((int)array); }

void GLAPIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride,
				const GLvoid *ptr)
{
	tgl_glVertexPointer(size, (int)type, stride, ptr);
}

void GLAPIENTRY glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
	tgl_glNormalPointer((int)type, stride, ptr);
}

void GLAPIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride,
			       const GLvoid *ptr)
{
	tgl_glColorPointer(size, (int)type, stride, ptr);
}

void GLAPIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride,
				  const GLvoid *ptr)
{
	tgl_glTexCoordPointer(size, (int)type, stride, ptr);
}

void GLAPIENTRY glEdgeFlagPointer(GLsizei stride, const GLvoid *ptr)
{
	tgl_edge_flag_pointer(stride, ptr);
}

/* colour-index arrays have no effect in RGBA mode */
void GLAPIENTRY glIndexPointer(GLenum type, GLsizei stride, const GLvoid *ptr)
{
	(void)ptr;
	if (type != GL_UNSIGNED_BYTE && type != GL_SHORT && type != GL_INT &&
	    type != GL_FLOAT && type != GL_DOUBLE) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	if (stride < 0)
		S31_ERR(GL_INVALID_VALUE);
}

void GLAPIENTRY glArrayElement(GLint i) { tgl_glArrayElement(i); }

void GLAPIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	tgl_glDrawArrays((int)mode, first, count);
}

void GLAPIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type,
			       const GLvoid *indices)
{
	tgl_glDrawElements((int)mode, count, (int)type, indices);
}

void GLAPIENTRY glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
				    GLsizei count, GLenum type,
				    const GLvoid *indices)
{
	if (end < start) {
		S31_ERR(GL_INVALID_VALUE);
		return;
	}
	tgl_glDrawElements((int)mode, count, (int)type, indices);
}

/* GL 1.3 section 2.8, table 2.5 */
void GLAPIENTRY glInterleavedArrays(GLenum format, GLsizei stride,
				    const GLvoid *pointer)
{
	/* et ec en: enables; st sc sv: sizes; tc: colour type; pc pn pv: offsets */
	int et = 0, ec = 0, en = 0, st = 0, sc = 0, sv = 0, tc = GL_FLOAT;
	int pc = 0, pn = 0, pv = 0, s;
	const int f = sizeof(GLfloat), c4ub = 4;	/* c = 4 * sizeof(ubyte) */
	const char *p = pointer;

	if (stride < 0) {
		S31_ERR(GL_INVALID_VALUE);
		return;
	}
	switch (format) {
	case GL_V2F: sv = 2; pv = 0; s = 2 * f; break;
	case GL_V3F: sv = 3; pv = 0; s = 3 * f; break;
	case GL_C4UB_V2F: ec = 1; sc = 4; tc = GL_UNSIGNED_BYTE; sv = 2;
		pc = 0; pv = c4ub; s = c4ub + 2 * f; break;
	case GL_C4UB_V3F: ec = 1; sc = 4; tc = GL_UNSIGNED_BYTE; sv = 3;
		pc = 0; pv = c4ub; s = c4ub + 3 * f; break;
	case GL_C3F_V3F: ec = 1; sc = 3; sv = 3; pc = 0; pv = 3 * f;
		s = 6 * f; break;
	case GL_N3F_V3F: en = 1; sv = 3; pn = 0; pv = 3 * f; s = 6 * f; break;
	case GL_C4F_N3F_V3F: ec = 1; en = 1; sc = 4; sv = 3; pc = 0;
		pn = 4 * f; pv = 7 * f; s = 10 * f; break;
	case GL_T2F_V3F: et = 1; st = 2; sv = 3; pv = 2 * f; s = 5 * f; break;
	case GL_T4F_V4F: et = 1; st = 4; sv = 4; pv = 4 * f; s = 8 * f; break;
	case GL_T2F_C4UB_V3F: et = 1; ec = 1; st = 2; sc = 4;
		tc = GL_UNSIGNED_BYTE; sv = 3; pc = 2 * f; pv = c4ub + 2 * f;
		s = c4ub + 5 * f; break;
	case GL_T2F_C3F_V3F: et = 1; ec = 1; st = 2; sc = 3; sv = 3;
		pc = 2 * f; pv = 5 * f; s = 8 * f; break;
	case GL_T2F_N3F_V3F: et = 1; en = 1; st = 2; sv = 3; pn = 2 * f;
		pv = 5 * f; s = 8 * f; break;
	case GL_T2F_C4F_N3F_V3F: et = 1; ec = 1; en = 1; st = 2; sc = 4;
		sv = 3; pc = 2 * f; pn = 6 * f; pv = 9 * f; s = 12 * f; break;
	case GL_T4F_C4F_N3F_V4F: et = 1; ec = 1; en = 1; st = 4; sc = 4;
		sv = 4; pc = 4 * f; pn = 8 * f; pv = 11 * f; s = 15 * f; break;
	default:
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	if (stride == 0)
		stride = s;

	tgl_glDisableClientState(GL_EDGE_FLAG_ARRAY);
	tgl_glDisableClientState(GL_INDEX_ARRAY);
	if (et) {
		tgl_glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		tgl_glTexCoordPointer(st, GL_FLOAT, stride, p);
	} else {
		tgl_glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	if (ec) {
		tgl_glEnableClientState(GL_COLOR_ARRAY);
		tgl_glColorPointer(sc, tc, stride, p + pc);
	} else {
		tgl_glDisableClientState(GL_COLOR_ARRAY);
	}
	if (en) {
		tgl_glEnableClientState(GL_NORMAL_ARRAY);
		tgl_glNormalPointer(GL_FLOAT, stride, p + pn);
	} else {
		tgl_glDisableClientState(GL_NORMAL_ARRAY);
	}
	tgl_glEnableClientState(GL_VERTEX_ARRAY);
	tgl_glVertexPointer(sv, GL_FLOAT, stride, p + pv);
}

/* ---- EXT aliases stock binaries built against Mesa's headers import ---- */

void GLAPIENTRY glBindTextureEXT(GLenum target, GLuint texture)
	__attribute__((alias("glBindTexture")));
void GLAPIENTRY glDeleteTexturesEXT(GLsizei n, const GLuint *textures)
	__attribute__((alias("glDeleteTextures")));
void GLAPIENTRY glGenTexturesEXT(GLsizei n, GLuint *textures)
	__attribute__((alias("glGenTextures")));
GLboolean GLAPIENTRY glIsTextureEXT(GLuint texture)
	__attribute__((alias("glIsTexture")));
GLboolean GLAPIENTRY glAreTexturesResidentEXT(GLsizei n, const GLuint *textures,
					      GLboolean *residences)
	__attribute__((alias("glAreTexturesResident")));
void GLAPIENTRY glPrioritizeTexturesEXT(GLsizei n, const GLuint *textures,
					const GLclampf *priorities)
	__attribute__((alias("glPrioritizeTextures")));
void GLAPIENTRY glArrayElementEXT(GLint i) __attribute__((alias("glArrayElement")));
void GLAPIENTRY glDrawArraysEXT(GLenum mode, GLint first, GLsizei count)
	__attribute__((alias("glDrawArrays")));
void GLAPIENTRY glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end,
				       GLsizei count, GLenum type,
				       const void *indices)
	__attribute__((alias("glDrawRangeElements")));

/* EXT_vertex_array's pointer calls carry a count, which GL 1.1 dropped */
void GLAPIENTRY glVertexPointerEXT(GLint size, GLenum type, GLsizei stride,
				   GLsizei count, const void *pointer)
{
	(void)count;
	glVertexPointer(size, type, stride, pointer);
}

void GLAPIENTRY glNormalPointerEXT(GLenum type, GLsizei stride, GLsizei count,
				   const void *pointer)
{
	(void)count;
	glNormalPointer(type, stride, pointer);
}

void GLAPIENTRY glColorPointerEXT(GLint size, GLenum type, GLsizei stride,
				  GLsizei count, const void *pointer)
{
	(void)count;
	glColorPointer(size, type, stride, pointer);
}

void GLAPIENTRY glTexCoordPointerEXT(GLint size, GLenum type, GLsizei stride,
				     GLsizei count, const void *pointer)
{
	(void)count;
	glTexCoordPointer(size, type, stride, pointer);
}

void GLAPIENTRY glEdgeFlagPointerEXT(GLsizei stride, GLsizei count,
				     const GLboolean *pointer)
{
	(void)count;
	glEdgeFlagPointer(stride, pointer);
}

void GLAPIENTRY glIndexPointerEXT(GLenum type, GLsizei stride, GLsizei count,
				  const void *pointer)
{
	(void)count;
	glIndexPointer(type, stride, pointer);
}

/* EXT_compiled_vertex_array: locking is a hint; nothing is cached */
void GLAPIENTRY glLockArraysEXT(GLint first, GLsizei count)
{
	if (first < 0 || count <= 0)
		S31_ERR(GL_INVALID_VALUE);
}

void GLAPIENTRY glUnlockArraysEXT(void)
{
}

void GLAPIENTRY glPolygonOffsetEXT(GLfloat factor, GLfloat bias)
{
	/* EXT_polygon_offset's bias is in depth units, GL 1.1's units are
	   in minimum resolvable steps; 16-bit depth makes one step 1/65535 */
	tgl_glPolygonOffset(factor, bias * 65535.0f);
}
