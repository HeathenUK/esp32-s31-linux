/*
 * gl_get.c - glGetString, glGetError, glIsEnabled and the glGet*v family.
 * s31, MIT.
 *
 * The values come from TinyGL (tgl_get, gl/tinygl/source/get.c); this file
 * only converts them to the four glGet types per GL 1.3 section 6.1.2.
 */
#include <limits.h>
#include "s31_api.h"

/* GL_RENDERER is exactly "Software Rasterizer": true, and SDL2's own
   sentinel for declining its GL texture framebuffer (plan section 4.2).
   GL_VERSION is honest: fixed-function GL 1.1 plus a few 1.2/1.3 pieces.
   Only extensions that really work are listed. */
static const char s_vendor[] = "s31";
static const char s_renderer[] = "Software Rasterizer";
static const char s_version[] = "1.1 s31-tinygl";
static const char s_extensions[] =
	"GL_EXT_bgra GL_EXT_texture_object GL_EXT_vertex_array";

const GLubyte *GLAPIENTRY glGetString(GLenum name)
{
	if (tgl_ctx_current() == NULL)
		return NULL;	/* as Mesa: no current context, no strings */
	if (tgl_in_begin()) {
		S31_ERR(GL_INVALID_OPERATION);
		return NULL;
	}
	switch (name) {
	case GL_VENDOR: return (const GLubyte *)s_vendor;
	case GL_RENDERER: return (const GLubyte *)s_renderer;
	case GL_VERSION: return (const GLubyte *)s_version;
	case GL_EXTENSIONS: return (const GLubyte *)s_extensions;
	default:
		S31_ERR(GL_INVALID_ENUM);
		return NULL;
	}
}

GLenum GLAPIENTRY glGetError(void)
{
	return (GLenum)tgl_get_error();
}

GLboolean GLAPIENTRY glIsEnabled(GLenum cap)
{
	int v = tgl_is_enabled((int)cap);
	if (v < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return GL_FALSE;
	}
	return v ? GL_TRUE : GL_FALSE;
}

/* colour-like values as integers: [-1,1] onto the full GLint range */
static GLint color_to_int(GLfloat f)
{
	if (f >= 1.0f)
		return INT_MAX;
	if (f <= -1.0f)
		return INT_MIN;
	return (GLint)(f * 2147483647.0f);
}

static int get(GLenum pname, int *iv, float *fv, int *kind)
{
	int n;
	if (tgl_ctx_current() == NULL)
		return -2;
	n = tgl_get((int)pname, iv, fv, kind);
	if (n < 0)
		S31_ERR(GL_INVALID_ENUM);
	return n;
}

void GLAPIENTRY glGetIntegerv(GLenum pname, GLint *params)
{
	int iv[16], kind, n, i;
	float fv[16];
	n = get(pname, iv, fv, &kind);
	for (i = 0; i < n; i++)
		params[i] = kind == TGL_GET_COLOR ? color_to_int(fv[i]) : iv[i];
}

void GLAPIENTRY glGetFloatv(GLenum pname, GLfloat *params)
{
	int iv[16], kind, n, i;
	float fv[16];
	n = get(pname, iv, fv, &kind);
	for (i = 0; i < n; i++)
		params[i] = kind == TGL_GET_INT ? (GLfloat)iv[i] : fv[i];
}

void GLAPIENTRY glGetDoublev(GLenum pname, GLdouble *params)
{
	int iv[16], kind, n, i;
	float fv[16];
	n = get(pname, iv, fv, &kind);
	for (i = 0; i < n; i++)
		params[i] = kind == TGL_GET_INT ? (GLdouble)iv[i] : (GLdouble)fv[i];
}

void GLAPIENTRY glGetBooleanv(GLenum pname, GLboolean *params)
{
	int iv[16], kind, n, i;
	float fv[16];
	n = get(pname, iv, fv, &kind);
	for (i = 0; i < n; i++)
		params[i] = (kind == TGL_GET_INT ? iv[i] != 0 : fv[i] != 0.0f)
			    ? GL_TRUE : GL_FALSE;
}

void GLAPIENTRY glGetPointerv(GLenum pname, GLvoid **params)
{
	void *p = tgl_get_pointer((int)pname);
	if (p == (void *)-1) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	*params = p;
}

void GLAPIENTRY glGetPointervEXT(GLenum pname, void **params)
	__attribute__((alias("glGetPointerv")));

/* ---- lights and materials ---- */

void GLAPIENTRY glGetLightfv(GLenum light, GLenum pname, GLfloat *params)
{
	float v[4];
	int i, n = tgl_get_light((int)light, (int)pname, v);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = v[i];
}

void GLAPIENTRY glGetLightiv(GLenum light, GLenum pname, GLint *params)
{
	float v[4];
	int i, n = tgl_get_light((int)light, (int)pname, v);
	int color = pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR;
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = color ? color_to_int(v[i])
				  : (GLint)(v[i] < 0 ? v[i] - 0.5f : v[i] + 0.5f);
}

void GLAPIENTRY glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params)
{
	float v[4];
	int i, n = tgl_get_material((int)face, (int)pname, v);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = v[i];
}

void GLAPIENTRY glGetMaterialiv(GLenum face, GLenum pname, GLint *params)
{
	float v[4];
	int i, n = tgl_get_material((int)face, (int)pname, v);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = n == 4 ? color_to_int(v[i])
				   : (GLint)(v[i] < 0 ? v[i] - 0.5f : v[i] + 0.5f);
}

/* ---- texture queries ---- */

static void put(int n, const int *iv, const float *fv, int kind,
		GLint *ip, GLfloat *fp)
{
	int i;
	for (i = 0; i < n; i++) {
		if (ip)
			ip[i] = kind == TGL_GET_COLOR ? color_to_int(fv[i]) : iv[i];
		else
			fp[i] = kind == TGL_GET_INT ? (GLfloat)iv[i] : fv[i];
	}
}

void GLAPIENTRY glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params)
{
	int iv[4], kind, n;
	float fv[4];
	n = tgl_get_tex_env((int)target, (int)pname, iv, fv, &kind);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	put(n, iv, fv, kind, NULL, params);
}

void GLAPIENTRY glGetTexEnviv(GLenum target, GLenum pname, GLint *params)
{
	int iv[4], kind, n;
	float fv[4];
	n = tgl_get_tex_env((int)target, (int)pname, iv, fv, &kind);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	put(n, iv, fv, kind, params, NULL);
}

void GLAPIENTRY glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
	int iv[4], kind, n;
	float fv[4];
	n = tgl_get_tex_parameter((int)target, (int)pname, iv, fv, &kind);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	put(n, iv, fv, kind, NULL, params);
}

void GLAPIENTRY glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
	int iv[4], kind, n;
	float fv[4];
	n = tgl_get_tex_parameter((int)target, (int)pname, iv, fv, &kind);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	put(n, iv, fv, kind, params, NULL);
}

void GLAPIENTRY glGetTexLevelParameteriv(GLenum target, GLint level,
					 GLenum pname, GLint *params)
{
	int v, n = tgl_get_tex_level_parameter((int)target, level, (int)pname, &v);
	if (n == -2) {
		S31_ERR(GL_INVALID_VALUE);
		return;
	}
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	*params = v;
}

void GLAPIENTRY glGetTexLevelParameterfv(GLenum target, GLint level,
					 GLenum pname, GLfloat *params)
{
	GLint v;
	int n = tgl_get_tex_level_parameter((int)target, level, (int)pname, &v);
	if (n == -2) {
		S31_ERR(GL_INVALID_VALUE);
		return;
	}
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	*params = (GLfloat)v;
}
