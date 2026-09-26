/*
 * gl_pixels.c - the GL 1.3 entry points of plan F7: the raster position
 * (glRasterPos*, glWindowPos* and their ARB/MESA names), glBitmap,
 * glDrawPixels, glReadPixels, glCopyPixels, glPixelZoom, glPixelTransfer,
 * glCopyTexImage/SubImage 1D and 2D, glTexImage1D/glTexSubImage1D,
 * glGetTexImage, glTexGen / glGetTexGen, glClipPlane / glGetClipPlane,
 * glPolygonStipple / glGetPolygonStipple. s31, MIT.
 *
 * Thin conversions to float (s31_d2f for doubles, never a libgcc double
 * op on the way in) forwarding to the TinyGL side through tgl_bridge.h:
 * gl/tinygl/source/s31_xform.c and s31_draw.c.
 */
#include "s31_api.h"

/* ------------------------------------------------------------ raster position */

#define RP(x, y, z, w) tgl_raster_pos((GLfloat)(x), (GLfloat)(y), (GLfloat)(z), (GLfloat)(w), 0)
#define RPD(x, y, z, w) tgl_raster_pos(s31_d2f(x), s31_d2f(y), s31_d2f(z), s31_d2f(w), 0)

void GLAPIENTRY glRasterPos2s(GLshort x, GLshort y) { RP(x, y, 0, 1); }
void GLAPIENTRY glRasterPos2i(GLint x, GLint y) { RP(x, y, 0, 1); }
void GLAPIENTRY glRasterPos2f(GLfloat x, GLfloat y) { RP(x, y, 0, 1); }
void GLAPIENTRY glRasterPos2d(GLdouble x, GLdouble y) { RPD(x, y, 0.0, 1.0); }
void GLAPIENTRY glRasterPos3s(GLshort x, GLshort y, GLshort z) { RP(x, y, z, 1); }
void GLAPIENTRY glRasterPos3i(GLint x, GLint y, GLint z) { RP(x, y, z, 1); }
void GLAPIENTRY glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { RP(x, y, z, 1); }
void GLAPIENTRY glRasterPos3d(GLdouble x, GLdouble y, GLdouble z) { RPD(x, y, z, 1.0); }
void GLAPIENTRY glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w) { RP(x, y, z, w); }
void GLAPIENTRY glRasterPos4i(GLint x, GLint y, GLint z, GLint w) { RP(x, y, z, w); }
void GLAPIENTRY glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { RP(x, y, z, w); }
void GLAPIENTRY glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { RPD(x, y, z, w); }
void GLAPIENTRY glRasterPos2sv(const GLshort *v) { RP(v[0], v[1], 0, 1); }
void GLAPIENTRY glRasterPos2iv(const GLint *v) { RP(v[0], v[1], 0, 1); }
void GLAPIENTRY glRasterPos2fv(const GLfloat *v) { RP(v[0], v[1], 0, 1); }
void GLAPIENTRY glRasterPos2dv(const GLdouble *v) { RPD(v[0], v[1], 0.0, 1.0); }
void GLAPIENTRY glRasterPos3sv(const GLshort *v) { RP(v[0], v[1], v[2], 1); }
void GLAPIENTRY glRasterPos3iv(const GLint *v) { RP(v[0], v[1], v[2], 1); }
void GLAPIENTRY glRasterPos3fv(const GLfloat *v) { RP(v[0], v[1], v[2], 1); }
void GLAPIENTRY glRasterPos3dv(const GLdouble *v) { RPD(v[0], v[1], v[2], 1.0); }
void GLAPIENTRY glRasterPos4sv(const GLshort *v) { RP(v[0], v[1], v[2], v[3]); }
void GLAPIENTRY glRasterPos4iv(const GLint *v) { RP(v[0], v[1], v[2], v[3]); }
void GLAPIENTRY glRasterPos4fv(const GLfloat *v) { RP(v[0], v[1], v[2], v[3]); }
void GLAPIENTRY glRasterPos4dv(const GLdouble *v) { RPD(v[0], v[1], v[2], v[3]); }

/* GL 1.4 / ARB_window_pos / MESA_window_pos: window coordinates */
#define WP(x, y, z, w) tgl_raster_pos((GLfloat)(x), (GLfloat)(y), (GLfloat)(z), (GLfloat)(w), 1)
#define WPD(x, y, z, w) tgl_raster_pos(s31_d2f(x), s31_d2f(y), s31_d2f(z), s31_d2f(w), 1)

void GLAPIENTRY glWindowPos2s(GLshort x, GLshort y) { WP(x, y, 0, 1); }
void GLAPIENTRY glWindowPos2i(GLint x, GLint y) { WP(x, y, 0, 1); }
void GLAPIENTRY glWindowPos2f(GLfloat x, GLfloat y) { WP(x, y, 0, 1); }
void GLAPIENTRY glWindowPos2d(GLdouble x, GLdouble y) { WPD(x, y, 0.0, 1.0); }
void GLAPIENTRY glWindowPos3s(GLshort x, GLshort y, GLshort z) { WP(x, y, z, 1); }
void GLAPIENTRY glWindowPos3i(GLint x, GLint y, GLint z) { WP(x, y, z, 1); }
void GLAPIENTRY glWindowPos3f(GLfloat x, GLfloat y, GLfloat z) { WP(x, y, z, 1); }
void GLAPIENTRY glWindowPos3d(GLdouble x, GLdouble y, GLdouble z) { WPD(x, y, z, 1.0); }
void GLAPIENTRY glWindowPos2sv(const GLshort *v) { WP(v[0], v[1], 0, 1); }
void GLAPIENTRY glWindowPos2iv(const GLint *v) { WP(v[0], v[1], 0, 1); }
void GLAPIENTRY glWindowPos2fv(const GLfloat *v) { WP(v[0], v[1], 0, 1); }
void GLAPIENTRY glWindowPos2dv(const GLdouble *v) { WPD(v[0], v[1], 0.0, 1.0); }
void GLAPIENTRY glWindowPos3sv(const GLshort *v) { WP(v[0], v[1], v[2], 1); }
void GLAPIENTRY glWindowPos3iv(const GLint *v) { WP(v[0], v[1], v[2], 1); }
void GLAPIENTRY glWindowPos3fv(const GLfloat *v) { WP(v[0], v[1], v[2], 1); }
void GLAPIENTRY glWindowPos3dv(const GLdouble *v) { WPD(v[0], v[1], v[2], 1.0); }
void GLAPIENTRY glWindowPos4sMESA(GLshort x, GLshort y, GLshort z, GLshort w) { WP(x, y, z, w); }
void GLAPIENTRY glWindowPos4iMESA(GLint x, GLint y, GLint z, GLint w) { WP(x, y, z, w); }
void GLAPIENTRY glWindowPos4fMESA(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { WP(x, y, z, w); }
void GLAPIENTRY glWindowPos4dMESA(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { WPD(x, y, z, w); }
void GLAPIENTRY glWindowPos4svMESA(const GLshort *v) { WP(v[0], v[1], v[2], v[3]); }
void GLAPIENTRY glWindowPos4ivMESA(const GLint *v) { WP(v[0], v[1], v[2], v[3]); }
void GLAPIENTRY glWindowPos4fvMESA(const GLfloat *v) { WP(v[0], v[1], v[2], v[3]); }
void GLAPIENTRY glWindowPos4dvMESA(const GLdouble *v) { WPD(v[0], v[1], v[2], v[3]); }

#define WPALIAS(sfx, args) \
	void GLAPIENTRY glWindowPos##sfx##ARB args __attribute__((alias("glWindowPos" #sfx))); \
	void GLAPIENTRY glWindowPos##sfx##MESA args __attribute__((alias("glWindowPos" #sfx)));
WPALIAS(2s, (GLshort x, GLshort y))
WPALIAS(2i, (GLint x, GLint y))
WPALIAS(2f, (GLfloat x, GLfloat y))
WPALIAS(2d, (GLdouble x, GLdouble y))
WPALIAS(3s, (GLshort x, GLshort y, GLshort z))
WPALIAS(3i, (GLint x, GLint y, GLint z))
WPALIAS(3f, (GLfloat x, GLfloat y, GLfloat z))
WPALIAS(3d, (GLdouble x, GLdouble y, GLdouble z))
WPALIAS(2sv, (const GLshort *v))
WPALIAS(2iv, (const GLint *v))
WPALIAS(2fv, (const GLfloat *v))
WPALIAS(2dv, (const GLdouble *v))
WPALIAS(3sv, (const GLshort *v))
WPALIAS(3iv, (const GLint *v))
WPALIAS(3fv, (const GLfloat *v))
WPALIAS(3dv, (const GLdouble *v))

/* ------------------------------------------------------------ pixel rectangles */

void GLAPIENTRY glBitmap(GLsizei width, GLsizei height, GLfloat xorig,
			 GLfloat yorig, GLfloat xmove, GLfloat ymove,
			 const GLubyte *bitmap)
{
	tgl_bitmap(width, height, xorig, yorig, xmove, ymove, bitmap);
}

void GLAPIENTRY glDrawPixels(GLsizei width, GLsizei height, GLenum format,
			     GLenum type, const GLvoid *pixels)
{
	tgl_draw_pixels(width, height, (int)format, (int)type, pixels);
}

void GLAPIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
			     GLenum format, GLenum type, GLvoid *pixels)
{
	tgl_read_pixels(x, y, width, height, (int)format, (int)type, pixels);
}

void GLAPIENTRY glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height,
			     GLenum type)
{
	tgl_copy_pixels(x, y, width, height, (int)type);
}

void GLAPIENTRY glPixelZoom(GLfloat xfactor, GLfloat yfactor)
{
	tgl_state_f(S31_ST_PIXEL_ZOOM, xfactor, yfactor, 0, 0);
}

static int transfer_pname(GLenum p)
{
	switch (p) {
	case GL_MAP_COLOR: case GL_MAP_STENCIL: case GL_INDEX_SHIFT:
	case GL_INDEX_OFFSET: case GL_RED_SCALE: case GL_RED_BIAS:
	case GL_GREEN_SCALE: case GL_GREEN_BIAS: case GL_BLUE_SCALE:
	case GL_BLUE_BIAS: case GL_ALPHA_SCALE: case GL_ALPHA_BIAS:
	case GL_DEPTH_SCALE: case GL_DEPTH_BIAS:
		return 1;
	default:
		return 0;
	}
}

void GLAPIENTRY glPixelTransferf(GLenum pname, GLfloat param)
{
	if (!transfer_pname(pname)) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_state_pf(S31_ST_PIXEL_TRANSFER, (int)pname, param);
}

void GLAPIENTRY glPixelTransferi(GLenum pname, GLint param)
{
	glPixelTransferf(pname, (GLfloat)param);
}

/* ------------------------------------------------------------ textures */

void GLAPIENTRY glTexImage1D(GLenum target, GLint level, GLint internalformat,
			     GLsizei width, GLint border, GLenum format,
			     GLenum type, const GLvoid *pixels)
{
	if (target != GL_TEXTURE_1D && target != GL_PROXY_TEXTURE_1D) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glTexImage2D((int)target, level, internalformat, width, 1, border,
			 (int)format, (int)type, (void *)pixels);
}

void GLAPIENTRY glTexSubImage1D(GLenum target, GLint level, GLint xoffset,
				GLsizei width, GLenum format, GLenum type,
				const GLvoid *pixels)
{
	if (target != GL_TEXTURE_1D) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_glTexSubImage2D((int)target, level, xoffset, 0, width, 1,
			    (int)format, (int)type, pixels);
}

void GLAPIENTRY glCopyTexImage2D(GLenum target, GLint level,
				 GLenum internalformat, GLint x, GLint y,
				 GLsizei width, GLsizei height, GLint border)
{
	tgl_copy_tex((int)target, level, (int)internalformat, x, y, width,
		     height, border, 0, 0, 0);
}

void GLAPIENTRY glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset,
				    GLint yoffset, GLint x, GLint y,
				    GLsizei width, GLsizei height)
{
	tgl_copy_tex((int)target, level, 0, x, y, width, height, 0, xoffset,
		     yoffset, 1);
}

void GLAPIENTRY glCopyTexImage1D(GLenum target, GLint level,
				 GLenum internalformat, GLint x, GLint y,
				 GLsizei width, GLint border)
{
	if (target != GL_TEXTURE_1D) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_copy_tex((int)target, level, (int)internalformat, x, y, width, 1,
		     border, 0, 0, 0);
}

void GLAPIENTRY glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset,
				    GLint x, GLint y, GLsizei width)
{
	if (target != GL_TEXTURE_1D) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_copy_tex((int)target, level, 0, x, y, width, 1, 0, xoffset, 0, 1);
}

void GLAPIENTRY glGetTexImage(GLenum target, GLint level, GLenum format,
			      GLenum type, GLvoid *pixels)
{
	tgl_get_tex_image((int)target, level, (int)format, (int)type, pixels);
}

/* ------------------------------------------------------------ texgen */

void GLAPIENTRY glTexGeni(GLenum coord, GLenum pname, GLint param)
{
	if (pname != GL_TEXTURE_GEN_MODE) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	tgl_tex_gen((int)coord, (int)pname, param, NULL);
}

void GLAPIENTRY glTexGenf(GLenum coord, GLenum pname, GLfloat param)
{
	glTexGeni(coord, pname, (GLint)param);
}

void GLAPIENTRY glTexGend(GLenum coord, GLenum pname, GLdouble param)
{
	glTexGeni(coord, pname, (GLint)s31_d2f(param));
}

void GLAPIENTRY glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params)
{
	if (pname == GL_TEXTURE_GEN_MODE) {
		glTexGeni(coord, pname, (GLint)params[0]);
		return;
	}
	tgl_tex_gen((int)coord, (int)pname, 0, params);
}

void GLAPIENTRY glTexGeniv(GLenum coord, GLenum pname, const GLint *params)
{
	GLfloat f[4];
	int i;
	if (pname == GL_TEXTURE_GEN_MODE) {
		glTexGeni(coord, pname, params[0]);
		return;
	}
	for (i = 0; i < 4; i++)
		f[i] = (GLfloat)params[i];
	tgl_tex_gen((int)coord, (int)pname, 0, f);
}

void GLAPIENTRY glTexGendv(GLenum coord, GLenum pname, const GLdouble *params)
{
	GLfloat f[4];
	int i;
	if (pname == GL_TEXTURE_GEN_MODE) {
		glTexGeni(coord, pname, (GLint)s31_d2f(params[0]));
		return;
	}
	for (i = 0; i < 4; i++)
		f[i] = s31_d2f(params[i]);
	tgl_tex_gen((int)coord, (int)pname, 0, f);
}

void GLAPIENTRY glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params)
{
	GLfloat v[4];
	int i, n = tgl_get_tex_gen((int)coord, (int)pname, v);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = v[i];
}

void GLAPIENTRY glGetTexGeniv(GLenum coord, GLenum pname, GLint *params)
{
	GLfloat v[4];
	int i, n = tgl_get_tex_gen((int)coord, (int)pname, v);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = (GLint)(v[i] < 0 ? v[i] - 0.5f : v[i] + 0.5f);
}

/* a query: the double conversion is not on any per-vertex path */
void GLAPIENTRY glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params)
{
	GLfloat v[4];
	int i, n = tgl_get_tex_gen((int)coord, (int)pname, v);
	if (n < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < n; i++)
		params[i] = s31_f2d(v[i]);
}

/* ------------------------------------------------------------ clip planes */

void GLAPIENTRY glClipPlane(GLenum plane, const GLdouble *equation)
{
	GLfloat f[4];
	int i;
	for (i = 0; i < 4; i++)
		f[i] = s31_d2f(equation[i]);
	tgl_clip_plane((int)plane, f);
}

void GLAPIENTRY glGetClipPlane(GLenum plane, GLdouble *equation)
{
	GLfloat f[4];
	int i;
	if (tgl_get_clip_plane((int)plane, f) < 0) {
		S31_ERR(GL_INVALID_ENUM);
		return;
	}
	for (i = 0; i < 4; i++)
		equation[i] = s31_f2d(f[i]);
}

/* ------------------------------------------------------------ stipple */

void GLAPIENTRY glPolygonStipple(const GLubyte *mask)
{
	tgl_polygon_stipple(mask);
}

void GLAPIENTRY glGetPolygonStipple(GLubyte *mask)
{
	tgl_get_polygon_stipple(mask);
}
