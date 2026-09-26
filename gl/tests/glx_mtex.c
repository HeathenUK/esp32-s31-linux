/* glx_mtex.c - phase 5 O1: GL_ARB_multitexture (2 units),
 * GL_ARB_texture_env_combine and GL_ARB_texture_env_add against Mesa,
 * through tools/glref (gl/tests/run-mtex.sh runs every page under both and
 * compares the frames and the logs):
 *     glx_mtex PAGE [frames]
 * One 320x240 window, 8 x 6 cells of 40 x 40. Pages:
 *   1 GL_COMBINE on unit 0, the RGB part: every function, every source
 *     with every operand, the scales, every base format
 *   2 the alpha part the same way (made visible by blending over stripes
 *     and by the alpha test), ALPHA_SCALE
 *   3 unit 1: every texture environment on every base format over a
 *     MODULATE'd unit 0; COMBINE on unit 1 reading PRIMARY_COLOR,
 *     PREVIOUS and CONSTANT; each unit alone; QuakeSpasm 0.96.3's world
 *     and alias "case 1" setups (r_world.c, r_alias.c), nearest and linear
 *   4 unit 1's coordinates and state: its texture matrix, texgen, vertex
 *     arrays through glClientActiveTexture (glDrawArrays, glDrawElements,
 *     glArrayElement), display lists, glPushAttrib / glPushClientAttrib,
 *     clipping, lines and points, glBitmap and glDrawPixels (the raster
 *     texcoords), filters and mipmaps, wraps, a 1D texture,
 *     glCopyTexSubImage2D into unit 1's binding
 *   5 glBitmap textured by both units from the raster position (a page
 *     with no vertex arrays: see page_pixels)
 * Every page prints "query" / "error" lines of the state it sets (only
 * values GL defines, so they match Mesa's whatever its unit count).
 * Texel and constant colours are exact in RGB565, so a scale of 4 does not
 * magnify a storage rounding past the harness tolerance. Links -lGL -lX11
 * only, so LD_LIBRARY_PATH picks the implementation. s31, MIT. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 320
#define H 240
#define C 40

static int page = 1, first;
static GLuint tex[16];
enum { T_RGB, T_RGBA, T_A, T_L, T_LA, T_I, T_LM, T_FB, T_BIG, T_1D, T_MIP, T_COPY, T_N };

static void err(const char *what)
{
	GLenum e = glGetError();
	if (first) printf("error %s: 0x%x\n", what, e);
}

/* 5- and 6-bit values expanded as RGB565 stores them: exact after the
   round trip through the texture store */
static unsigned char e5(int k) { k &= 31; return (unsigned char)((k << 3) | (k >> 2)); }
static unsigned char e6(int k) { k &= 63; return (unsigned char)((k << 2) | (k >> 4)); }

static void mktex(int id, GLenum ifmt, GLenum fmt, int w, int h, int kind)
{
	unsigned char buf[64 * 64 * 4];
	int i, j, n = fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			unsigned char *p = buf + (j * w + i) * n;
			int k = (i * 5 + j * 3 + kind) & 31;
			unsigned char r = e5(i * 4 + kind), g = e6(j * 8 + 3 * kind), b = e5((i ^ j) * 4 + 7);
			unsigned char a = (unsigned char)(((i + j) * 255) / (w + h - 2));
			switch (fmt) {
			case GL_RGBA: p[0] = r; p[1] = g; p[2] = b; p[3] = a; break;
			case GL_RGB: p[0] = r; p[1] = g; p[2] = b; break;
			case GL_LUMINANCE_ALPHA: p[0] = e5(k); p[1] = a; break;
			case GL_ALPHA: p[0] = a; break;
			default: p[0] = e5(k); break;          /* LUMINANCE, INTENSITY */
			}
		}
	glBindTexture(GL_TEXTURE_2D, tex[id]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, buf);
}

static void textures(void)
{
	unsigned char buf[16 * 16 * 4];
	int i, j, l;
	glGenTextures(T_N, tex);
	mktex(T_RGB, GL_RGB, GL_RGB, 8, 8, 0);
	mktex(T_RGBA, GL_RGBA, GL_RGBA, 8, 8, 1);
	mktex(T_A, GL_ALPHA, GL_ALPHA, 8, 8, 2);
	mktex(T_L, GL_LUMINANCE, GL_LUMINANCE, 8, 8, 3);
	mktex(T_LA, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, 8, 8, 4);
	mktex(T_I, GL_INTENSITY, GL_LUMINANCE, 8, 8, 5);
	/* a lightmap: 4 x 4, uploaded as QuakeSpasm does (internal format 4,
	   GL_RGBA, alpha 255), then a glTexSubImage2D */
	for (j = 0; j < 4; j++)
		for (i = 0; i < 4; i++) {
			unsigned char *p = buf + (j * 4 + i) * 4;
			p[0] = e5(8 + i * 5 + j); p[1] = e6(16 + j * 10 + i * 2); p[2] = e5(6 + (i + j) * 3);
			p[3] = 255;
		}
	glBindTexture(GL_TEXTURE_2D, tex[T_LM]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, 4, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	/* a fullbright mask: black except a few texels (alpha 255) */
	for (j = 0; j < 8; j++)
		for (i = 0; i < 8; i++) {
			unsigned char *p = buf + (j * 8 + i) * 4;
			int on = ((i * 3 + j * 5) % 7) == 0;
			p[0] = on ? e5(31) : 0; p[1] = on ? e6(40) : 0; p[2] = on ? e5(9) : 0;
			p[3] = 255;
		}
	glBindTexture(GL_TEXTURE_2D, tex[T_FB]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	mktex(T_BIG, GL_RGB, GL_RGB, 16, 16, 6);
	/* 1D */
	for (i = 0; i < 8; i++) {
		buf[i * 3] = e5(i * 4); buf[i * 3 + 1] = e6(63 - i * 8); buf[i * 3 + 2] = e5(20);
	}
	glBindTexture(GL_TEXTURE_1D, tex[T_1D]);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGB, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, buf);
	/* a full mipmap chain, a colour per level */
	glBindTexture(GL_TEXTURE_2D, tex[T_MIP]);
	for (l = 0; l <= 4; l++) {
		int s = 16 >> l;
		for (i = 0; i < s * s; i++) {
			buf[i * 3] = e5(l * 7 + (i & 3)); buf[i * 3 + 1] = e6(60 - l * 12);
			buf[i * 3 + 2] = e5(31 - l * 6 + ((i >> 2) & 1) * 3);
		}
		glTexImage2D(GL_TEXTURE_2D, l, GL_RGB, s, s, 0, GL_RGB, GL_UNSIGNED_BYTE, buf);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	mktex(T_COPY, GL_RGB, GL_RGB, 16, 16, 9);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* window coordinates everywhere: pixel (x, y) from the bottom left */
static void win2d(void)
{
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

#define CX(col) ((col) * C)
#define CY(row) ((row) * C)

static void rect(float x0, float y0, float x1, float y1, float r, float g, float b)
{
	glColor3f(r, g, b);
	glBegin(GL_QUADS);
	glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
	glEnd();
}

/* the background of a cell: two stripes, to see blending */
static void stripes(int col, int row)
{
	rect(CX(col), CY(row), CX(col) + 20, CY(row) + C, .9f, .6f, .1f);
	rect(CX(col) + 20, CY(row), CX(col) + C, CY(row) + C, .1f, .3f, .8f);
}

/* the corners' primary colours: exact 8-bit values */
static const float pc[4][4] = {
	{ 1.0f, 128 / 255.f, 51 / 255.f, 77 / 255.f },
	{ 51 / 255.f, 1.0f, 128 / 255.f, 1.0f },
	{ 128 / 255.f, 51 / 255.f, 1.0f, 204 / 255.f },
	{ 204 / 255.f, 204 / 255.f, 204 / 255.f, 153 / 255.f },
};

/* a cell's quad, texcoords 0..1 on both units (unit 1's scaled by s1),
   the corners' colours */
static void cellq(int col, int row, float s1)
{
	float x0 = CX(col) + 2, y0 = CY(row) + 2, x1 = CX(col) + C - 2, y1 = CY(row) + C - 2;
	glBegin(GL_QUADS);
	glColor4fv(pc[0]); glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0);
	glVertex2f(x0, y0);
	glColor4fv(pc[1]); glMultiTexCoord2f(GL_TEXTURE0, 1, 0); glMultiTexCoord2f(GL_TEXTURE1, s1, 0);
	glVertex2f(x1, y0);
	glColor4fv(pc[2]); glMultiTexCoord2f(GL_TEXTURE0, 1, 1); glMultiTexCoord2f(GL_TEXTURE1, s1, s1);
	glVertex2f(x1, y1);
	glColor4fv(pc[3]); glMultiTexCoord2f(GL_TEXTURE0, 0, 1); glMultiTexCoord2f(GL_TEXTURE1, 0, s1);
	glVertex2f(x0, y1);
	glEnd();
}

static const float kcol[4] = { 51 / 255.f, 153 / 255.f, 204 / 255.f, 102 / 255.f };

/* the combiner of the active unit: func, three (source, operand) pairs of
   the part (0 RGB, 1 alpha), scale */
static void comb(int part, GLenum f, GLenum s0, GLenum o0, GLenum s1, GLenum o1,
		 GLenum s2, GLenum o2, float scale)
{
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	if (part == 0) {
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, f);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, s0);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, o0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, s1);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, o1);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, s2);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, o2);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, scale);
	} else {
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, f);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, s0);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, o0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, s1);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, o1);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_ALPHA, s2);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA, o2);
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, scale);
	}
}

/* the GL 1.3 defaults of the active unit's environment */
static void env_reset(void)
{
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	comb(0, GL_MODULATE, GL_TEXTURE, GL_SRC_COLOR, GL_PREVIOUS, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	comb(1, GL_MODULATE, GL_TEXTURE, GL_SRC_ALPHA, GL_PREVIOUS, GL_SRC_ALPHA,
	     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, kcol);
}

static void unit(int u, GLuint t)
{
	glActiveTexture(GL_TEXTURE0 + u);
	if (t) {
		glBindTexture(GL_TEXTURE_2D, t);
		glEnable(GL_TEXTURE_2D);
	} else {
		glDisable(GL_TEXTURE_2D);
	}
}

static void units_off(void)
{
	glActiveTexture(GL_TEXTURE1);
	env_reset();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_1D);
	glActiveTexture(GL_TEXTURE0);
	env_reset();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_TEXTURE_1D);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
}

static const GLenum srcs[4] = { GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS };
static const GLenum ops[4] = { GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
static const GLenum funcs[6] = { GL_REPLACE, GL_MODULATE, GL_ADD, GL_ADD_SIGNED, GL_INTERPOLATE, GL_SUBTRACT };

/* ------------------------------------------------------------ page 1 */

static void page_rgb(void)
{
	int i, j, col, row;
	static const GLuint fmts[6] = { T_RGB, T_RGBA, T_A, T_L, T_LA, T_I };
	/* row 0: every function, the default sources; two scales */
	for (i = 0; i < 6; i++) {
		unit(0, tex[T_RGBA]);
		comb(0, funcs[i], GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
		     GL_TEXTURE, GL_SRC_ALPHA, 1.0f);
		cellq(i, 0, 1);
	}
	unit(0, tex[T_RGB]);
	comb(0, GL_MODULATE, GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
	cellq(6, 0, 1);
	comb(0, GL_MODULATE, GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 4.0f);
	cellq(7, 0, 1);
	/* rows 1-2: REPLACE of every source with every operand */
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			col = (i * 4 + j) % 8; row = 1 + (i * 4 + j) / 8;
			unit(0, tex[T_RGBA]);
			comb(0, GL_REPLACE, srcs[i], ops[j], GL_PREVIOUS, GL_SRC_COLOR,
			     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
			cellq(col, row, 1);
		}
	/* row 3: MODULATE, the texture by every source and operand as arg1 */
	for (i = 0; i < 8; i++) {
		unit(0, tex[T_RGBA]);
		comb(0, GL_MODULATE, GL_TEXTURE, GL_SRC_COLOR, srcs[1 + (i >> 2)], ops[i & 3],
		     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
		cellq(i, 3, 1);
	}
	/* row 4: INTERPOLATE with every arg2 source / operand, ADD_SIGNED
	   and SUBTRACT with the operands swapped around */
	for (i = 0; i < 4; i++) {
		unit(0, tex[T_RGBA]);
		comb(0, GL_INTERPOLATE, GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
		     i < 2 ? GL_TEXTURE : GL_PRIMARY_COLOR, ops[(i & 1) + 2], 1.0f);
		cellq(i, 4, 1);
	}
	unit(0, tex[T_RGB]);
	comb(0, GL_INTERPOLATE, GL_CONSTANT, GL_SRC_COLOR, GL_TEXTURE, GL_SRC_COLOR,
	     GL_PRIMARY_COLOR, GL_ONE_MINUS_SRC_COLOR, 1.0f);
	cellq(4, 4, 1);
	comb(0, GL_ADD_SIGNED, GL_TEXTURE, GL_ONE_MINUS_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
	cellq(5, 4, 1);
	comb(0, GL_SUBTRACT, GL_PRIMARY_COLOR, GL_SRC_COLOR, GL_TEXTURE, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
	cellq(6, 4, 1);
	comb(0, GL_SUBTRACT, GL_CONSTANT, GL_ONE_MINUS_SRC_ALPHA, GL_PRIMARY_COLOR, GL_SRC_ALPHA,
	     GL_CONSTANT, GL_SRC_ALPHA, 4.0f);
	cellq(7, 4, 1);
	/* row 5: every base format as the texture source, REPLACE and ADD */
	for (i = 0; i < 6; i++) {
		unit(0, tex[fmts[i]]);
		comb(0, i & 1 ? GL_ADD : GL_REPLACE, GL_TEXTURE, i < 4 ? GL_SRC_COLOR : GL_SRC_ALPHA,
		     GL_PRIMARY_COLOR, GL_SRC_COLOR, GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
		cellq(i, 5, 1);
	}
	unit(0, tex[T_A]);
	comb(0, GL_MODULATE, GL_TEXTURE, GL_ONE_MINUS_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	cellq(6, 5, 1);
	unit(0, tex[T_I]);
	comb(0, GL_INTERPOLATE, GL_TEXTURE, GL_SRC_COLOR, GL_CONSTANT, GL_SRC_COLOR,
	     GL_TEXTURE, GL_SRC_ALPHA, 2.0f);
	cellq(7, 5, 1);
	units_off();
}

/* ------------------------------------------------------------ page 2 */

/* the combiner's alpha, made visible: RGB is the constant colour, blended
   by the fragment's alpha over the cell's stripes */
static void alpha_cell(int col, int row, GLuint t, GLenum f, GLenum s0, GLenum o0,
		       GLenum s1, GLenum o1, GLenum s2, GLenum o2, float scale)
{
	stripes(col, row);
	unit(0, t);
	comb(0, GL_REPLACE, GL_CONSTANT, GL_SRC_COLOR, GL_PREVIOUS, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	comb(1, f, s0, o0, s1, o1, s2, o2, scale);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	cellq(col, row, 1);
	glDisable(GL_BLEND);
}

static void page_alpha(void)
{
	int i, j;
	static const GLuint fmts[6] = { T_RGB, T_RGBA, T_A, T_L, T_LA, T_I };
	for (i = 0; i < 6; i++)
		alpha_cell(i, 0, tex[T_RGBA], funcs[i], GL_TEXTURE, GL_SRC_ALPHA,
			   GL_PRIMARY_COLOR, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	alpha_cell(6, 0, tex[T_RGBA], GL_MODULATE, GL_TEXTURE, GL_SRC_ALPHA,
		   GL_PRIMARY_COLOR, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
	alpha_cell(7, 0, tex[T_RGBA], GL_MODULATE, GL_TEXTURE, GL_SRC_ALPHA,
		   GL_PRIMARY_COLOR, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 4.0f);
	/* rows 1: REPLACE of every source, both operands */
	for (i = 0; i < 4; i++)
		for (j = 0; j < 2; j++)
			alpha_cell(i * 2 + j, 1, tex[T_RGBA], GL_REPLACE, srcs[i], ops[2 + j],
				   GL_PREVIOUS, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	/* row 2: the other functions with one-minus operands */
	for (i = 0; i < 8; i++)
		alpha_cell(i, 2, tex[T_RGBA], funcs[1 + i % 5], GL_TEXTURE, ops[2 + (i & 1)],
			   srcs[1 + (i >> 1) % 3], ops[3 - (i & 1)], GL_PRIMARY_COLOR,
			   ops[2 + ((i >> 2) & 1)], i == 7 ? 2.0f : 1.0f);
	/* row 3: every base format */
	for (i = 0; i < 6; i++)
		alpha_cell(i, 3, tex[fmts[i]], i & 1 ? GL_MODULATE : GL_REPLACE, GL_TEXTURE,
			   GL_SRC_ALPHA, GL_PRIMARY_COLOR, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	alpha_cell(6, 3, tex[T_LA], GL_ADD_SIGNED, GL_TEXTURE, GL_SRC_ALPHA,
		   GL_CONSTANT, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
	alpha_cell(7, 3, tex[T_I], GL_SUBTRACT, GL_PRIMARY_COLOR, GL_SRC_ALPHA,
		   GL_TEXTURE, GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 4.0f);
	/* row 4: the alpha test on the combiner's alpha; the RGB part uses the
	   alpha as a colour (operand SRC_ALPHA of PREVIOUS after unit 0) */
	for (i = 0; i < 4; i++) {
		stripes(i, 4);
		unit(0, tex[T_RGBA]);
		comb(0, GL_MODULATE, GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR, GL_SRC_COLOR,
		     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
		comb(1, i & 1 ? GL_ADD : GL_MODULATE, GL_TEXTURE, GL_SRC_ALPHA,
		     GL_PRIMARY_COLOR, i & 2 ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA,
		     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.4f);
		cellq(i, 4, 1);
		glDisable(GL_ALPHA_TEST);
	}
	for (i = 4; i < 8; i++) {
		unit(0, tex[T_RGBA]);
		comb(0, GL_REPLACE, GL_TEXTURE, i & 1 ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA,
		     GL_PRIMARY_COLOR, GL_SRC_COLOR, GL_CONSTANT, GL_SRC_ALPHA, i >= 6 ? 2.0f : 1.0f);
		comb(1, GL_REPLACE, GL_CONSTANT, GL_SRC_ALPHA, GL_PRIMARY_COLOR, GL_SRC_ALPHA,
		     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
		cellq(i, 4, 1);
	}
	/* row 5: the alpha from unit 1 over unit 0's */
	for (i = 0; i < 8; i++) {
		stripes(i, 5);
		unit(0, tex[T_RGBA]);
		env_reset();
		unit(1, tex[i & 1 ? T_LA : T_A]);
		comb(0, GL_REPLACE, GL_PREVIOUS, GL_SRC_COLOR, GL_TEXTURE, GL_SRC_COLOR,
		     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
		comb(1, funcs[i % 6], GL_TEXTURE, GL_SRC_ALPHA, GL_PREVIOUS, ops[2 + ((i >> 1) & 1)],
		     GL_PRIMARY_COLOR, GL_SRC_ALPHA, 1.0f);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		cellq(i, 5, 1);
		glDisable(GL_BLEND);
		env_reset();
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
	}
	units_off();
}

/* ------------------------------------------------------------ page 3 */

static void filters(GLuint t, GLenum f)
{
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

/* QuakeSpasm 0.96.3 r_world.c "case 1": unit 0 the texture (REPLACE),
   unit 1 the lightmap, COMBINE_RGB MODULATE (PREVIOUS, TEXTURE), RGB_SCALE
   2; depth LEQUAL, written */
static void qs_world(int col, int row, GLenum filt)
{
	filters(tex[T_BIG], filt);
	filters(tex[T_LM], filt);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_PREVIOUS_EXT);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_TEXTURE);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 2.0f);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glBindTexture(GL_TEXTURE_2D, tex[T_BIG]);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tex[T_LM]);
	/* glBegin(GL_POLYGON) with glMultiTexCoord2fARB per unit, as
	   R_DrawTextureChains_Multitexture draws it */
	glBegin(GL_POLYGON);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0, 0); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, .1f, .2f);
	glVertex3f(CX(col) + 1, CY(row) + 1, 0);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 2, 0); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, .9f, .1f);
	glVertex3f(CX(col) + C - 1, CY(row) + 3, 0);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 2, 2); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, .8f, .9f);
	glVertex3f(CX(col) + C - 3, CY(row) + C - 1, 0);
	glMultiTexCoord2fARB(GL_TEXTURE0_ARB, 0, 2); glMultiTexCoord2fARB(GL_TEXTURE1_ARB, .2f, .7f);
	glVertex3f(CX(col) + 2, CY(row) + C - 2, 0);
	glEnd();
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glDisable(GL_DEPTH_TEST);
	filters(tex[T_BIG], GL_NEAREST);
	filters(tex[T_LM], GL_NEAREST);
}

/* r_alias.c "case 1": unit 0 COMBINE_RGB MODULATE (TEXTURE, PRIMARY_COLOR)
   x 2, unit 1 the fullbright mask with GL_ADD, blending on with
   SRC_ALPHA / ONE_MINUS_SRC_ALPHA, smooth vertex colours */
static void qs_alias(int col, int row, GLenum filt)
{
	stripes(col, row);
	filters(tex[T_RGBA], filt);
	filters(tex[T_FB], filt);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex[T_RGBA]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_EXT);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_EXT, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_EXT, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_EXT, GL_PRIMARY_COLOR_EXT);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 2.0f);
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex[T_FB]);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	/* strips of triangles with glColor per vertex (GL_DrawAliasFrame) */
	glBegin(GL_TRIANGLE_STRIP);
	glColor4f(.8f, .6f, .4f, 1); glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0);
	glVertex2f(CX(col) + 3, CY(row) + 3);
	glColor4f(.3f, .5f, .7f, .8f); glMultiTexCoord2f(GL_TEXTURE0, 1, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0);
	glVertex2f(CX(col) + C - 3, CY(row) + 5);
	glColor4f(.5f, .9f, .2f, .9f); glMultiTexCoord2f(GL_TEXTURE0, 0, 1); glMultiTexCoord2f(GL_TEXTURE1, 0, 1);
	glVertex2f(CX(col) + 4, CY(row) + C - 4);
	glColor4f(.9f, .2f, .6f, .6f); glMultiTexCoord2f(GL_TEXTURE0, 1, 1); glMultiTexCoord2f(GL_TEXTURE1, 1, 1);
	glVertex2f(CX(col) + C - 2, CY(row) + C - 3);
	glEnd();
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glActiveTexture(GL_TEXTURE0);
	glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE_EXT, 1.0f);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	filters(tex[T_RGBA], GL_NEAREST);
	filters(tex[T_FB], GL_NEAREST);
}

static void page_unit1(void)
{
	static const GLenum modes[5] = { GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND, GL_ADD };
	static const GLuint fmts[6] = { T_RGB, T_RGBA, T_A, T_L, T_LA, T_I };
	int m, f, n = 0, i;
	GLint v;
	GLfloat fv[16];
	/* 30 cells: every mode on every format, unit 0 MODULATE'd */
	for (m = 0; m < 5; m++)
		for (f = 0; f < 6; f++, n++) {
			int col = n % 8, row = n / 8;
			if (m == 2 && fmts[f] != T_RGB && fmts[f] != T_RGBA) {
				/* DECAL is undefined for the other formats */
				n--;
				continue;
			}
			stripes(col, row);
			unit(0, tex[T_BIG]);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			unit(1, tex[fmts[f]]);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, modes[m]);
			glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, kcol);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			cellq(col, row, 1);
			glDisable(GL_BLEND);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			glDisable(GL_TEXTURE_2D);
			glActiveTexture(GL_TEXTURE0);
		}
	/* n = 26: COMBINE on unit 1 reading PRIMARY_COLOR after unit 0 replaced
	   the colour, PREVIOUS's alpha, CONSTANT */
	for (i = 0; i < 4; i++, n++) {
		unit(0, tex[T_RGB]);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		unit(1, tex[T_LM]);
		comb(0, i < 2 ? GL_MODULATE : GL_INTERPOLATE, i & 1 ? GL_PRIMARY_COLOR : GL_PREVIOUS,
		     GL_SRC_COLOR, i & 1 ? GL_TEXTURE : GL_PRIMARY_COLOR, GL_SRC_COLOR,
		     i == 3 ? GL_CONSTANT : GL_PRIMARY_COLOR, GL_SRC_ALPHA, i == 2 ? 2.0f : 1.0f);
		cellq(n % 8, n / 8, .5f);
		env_reset();
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
	/* n = 30: unit 1 alone (its previous is the primary colour), unit 0
	   COMBINE alone */
	units_off();
	unit(1, tex[T_RGBA]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	cellq(n % 8, n / 8, 1); n++;
	comb(0, GL_ADD_SIGNED, GL_PREVIOUS, GL_SRC_COLOR, GL_TEXTURE, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 1.0f);
	cellq(n % 8, n / 8, 1); n++;
	units_off();
	unit(0, tex[T_RGB]);
	comb(0, GL_INTERPOLATE, GL_TEXTURE, GL_SRC_COLOR, GL_CONSTANT, GL_SRC_COLOR,
	     GL_PRIMARY_COLOR, GL_SRC_COLOR, 1.0f);
	cellq(n % 8, n / 8, 1); n++;
	units_off();
	/* n = 33: QuakeSpasm */
	qs_world(n % 8, n / 8, GL_NEAREST); n++;
	qs_world(n % 8, n / 8, GL_LINEAR); n++;
	qs_alias(n % 8, n / 8, GL_NEAREST); n++;
	qs_alias(n % 8, n / 8, GL_LINEAR); n++;
	/* the world's lightmap pass under trilinear world textures */
	glBindTexture(GL_TEXTURE_2D, tex[T_MIP]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	filters(tex[T_LM], GL_LINEAR);
	unit(0, tex[T_MIP]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	unit(1, tex[T_LM]);
	comb(0, GL_MODULATE, GL_PREVIOUS, GL_SRC_COLOR, GL_TEXTURE, GL_SRC_COLOR,
	     GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
	glBegin(GL_QUADS);
	glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0);
	glVertex2f(CX(n % 8), CY(n / 8));
	glMultiTexCoord2f(GL_TEXTURE0, 4, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0);
	glVertex2f(CX(n % 8) + C, CY(n / 8));
	glMultiTexCoord2f(GL_TEXTURE0, 4, 4); glMultiTexCoord2f(GL_TEXTURE1, 1, 1);
	glVertex2f(CX(n % 8) + C, CY(n / 8) + C);
	glMultiTexCoord2f(GL_TEXTURE0, 0, 4); glMultiTexCoord2f(GL_TEXTURE1, 0, 1);
	glVertex2f(CX(n % 8), CY(n / 8) + C);
	glEnd();
	n++;
	filters(tex[T_LM], GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, tex[T_MIP]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	units_off();
	/* n = 38: unit 1's texture matrix (and unit 0's untouched), with a
	   clamped wrap in n = 39 */
	unit(0, tex[T_RGB]);
	unit(1, tex[T_LM]);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glTranslatef(.5f, .5f, 0);
	glRotatef(30, 0, 0, 1);
	glScalef(2, 1.5f, 1);
	glMatrixMode(GL_MODELVIEW);
	cellq(n % 8, n / 8, 1); n++;
	if (first) {
		glMatrixMode(GL_TEXTURE);
		glGetFloatv(GL_TEXTURE_MATRIX, fv);
		printf("query u1 texmatrix %.4f %.4f %.4f %.4f\n", fv[0], fv[1], fv[4], fv[12]);
		glPushMatrix();
		glGetIntegerv(GL_TEXTURE_STACK_DEPTH, &v); printf("query u1 stack %d\n", v);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_STACK_DEPTH, &v); printf("query u0 stack %d\n", v);
		glGetFloatv(GL_TEXTURE_MATRIX, fv);
		printf("query u0 texmatrix %.4f %.4f %.4f %.4f\n", fv[0], fv[1], fv[4], fv[12]);
		glActiveTexture(GL_TEXTURE1);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
	}
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glActiveTexture(GL_TEXTURE1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity(); glTranslatef(-.5f, -.5f, 0); glScalef(2, 2, 1);
	glMatrixMode(GL_MODELVIEW);
	cellq(n % 8, n / 8, 1); n++;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	units_off();
	/* the unit-1 queries */
	if (first) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, tex[T_LM]);
		comb(0, GL_INTERPOLATE, GL_CONSTANT, GL_ONE_MINUS_SRC_ALPHA, GL_PRIMARY_COLOR,
		     GL_SRC_ALPHA, GL_PREVIOUS, GL_ONE_MINUS_SRC_COLOR, 4.0f);
		comb(1, GL_SUBTRACT, GL_PREVIOUS, GL_ONE_MINUS_SRC_ALPHA, GL_TEXTURE,
		     GL_SRC_ALPHA, GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &v); printf("query u1 combine_rgb 0x%x\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE0_RGB, &v); printf("query u1 src0_rgb 0x%x\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_RGB, &v); printf("query u1 op0_rgb 0x%x\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND2_RGB, &v); printf("query u1 op2_rgb 0x%x\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, &v); printf("query u1 combine_alpha 0x%x\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, &v); printf("query u1 op0_alpha 0x%x\n", v);
		glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, fv); printf("query u1 rgb_scale %g\n", fv[0]);
		glGetTexEnvfv(GL_TEXTURE_ENV, GL_ALPHA_SCALE, fv); printf("query u1 alpha_scale %g\n", fv[0]);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_RGB_SCALE, &v); printf("query u1 rgb_scale_i %d\n", v);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &v); printf("query u1 binding %d\n", v == (GLint)tex[T_LM]);
		glActiveTexture(GL_TEXTURE0);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_COMBINE_RGB, &v); printf("query u0 combine_rgb 0x%x\n", v);
		glGetTexEnvfv(GL_TEXTURE_ENV, GL_RGB_SCALE, fv); printf("query u0 rgb_scale %g\n", fv[0]);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &v); printf("query u0 binding %d\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_SOURCE2_ALPHA, &v); printf("query u0 src2_alpha 0x%x\n", v);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_OPERAND1_RGB, &v); printf("query u0 op1_rgb 0x%x\n", v);
		/* errors GL defines the same way for any unit count */
		glGetError();
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 3.0f); err("rgb_scale 3");
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 0.5f); err("alpha_scale 0.5");
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_BLEND); err("combine_rgb blend");
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_COLOR); err("op0_alpha src_color");
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE); err("mode combine");
		glGetIntegerv(GL_ACTIVE_TEXTURE, &v); printf("query active 0x%x\n", v);
		glBegin(GL_POINTS); glActiveTexture(GL_TEXTURE1); glEnd(); err("active in begin");
		glGetIntegerv(GL_ACTIVE_TEXTURE, &v); printf("query active after 0x%x\n", v);
		glActiveTexture(GL_TEXTURE1); env_reset(); glActiveTexture(GL_TEXTURE0); env_reset();
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
}

/* ------------------------------------------------------------ page 4 */

static const float av[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };   /* a unit square */

static void cellbox(int col, int row)
{
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(CX(col) + 2, CY(row) + 2, 0);
	glScalef(C - 4, C - 4, 1);
}

static void page_state(void)
{
	float tc1[8], tc0[8];
	GLuint list;
	int i;
	GLint v;
	GLfloat fv[16];
	static const GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };

	for (i = 0; i < 8; i++) { tc0[i] = av[i] * 2.0f; tc1[i] = av[i] * .75f + .1f; }
	/* 0: unit 1's coordinates scaled (its texture matrix is page 3's:
	   Mesa's glBitmap keeps using a texture matrix unit 1 once had) */
	unit(0, tex[T_RGB]);
	unit(1, tex[T_LM]);
	glActiveTexture(GL_TEXTURE0);
	cellq(0, 0, 2.5f);
	/* 1-3: texgen on unit 1: object linear, eye linear, sphere map */
	for (i = 0; i < 3; i++) {
		static const float ps[4] = { .02f, 0, 0, .1f }, pt[4] = { 0, .03f, .01f, 0 };
		GLenum mode = i == 0 ? GL_OBJECT_LINEAR : i == 1 ? GL_EYE_LINEAR : GL_SPHERE_MAP;
		glActiveTexture(GL_TEXTURE1);
		glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, mode);
		glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, mode);
		if (i < 2) {
			glTexGenfv(GL_S, i ? GL_EYE_PLANE : GL_OBJECT_PLANE, ps);
			glTexGenfv(GL_T, i ? GL_EYE_PLANE : GL_OBJECT_PLANE, pt);
		}
		glEnable(GL_TEXTURE_GEN_S);
		glEnable(GL_TEXTURE_GEN_T);
		glNormal3f(.3f, .4f, .866f);
		cellq(1 + i, 0, 1);
		glDisable(GL_TEXTURE_GEN_S);
		glDisable(GL_TEXTURE_GEN_T);
		if (first && i == 2) {
			glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, &v); printf("query u1 texgen %x\n", v);
			printf("query u1 gen_s on %d\n", glIsEnabled(GL_TEXTURE_GEN_S));
			glActiveTexture(GL_TEXTURE0);
			glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, &v); printf("query u0 texgen %x\n", v);
			glActiveTexture(GL_TEXTURE1);
		}
	}
	glActiveTexture(GL_TEXTURE0);
	/* 4-6: vertex arrays with unit 1's through glClientActiveTexture */
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, av);
	glClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, 0, tc0);
	glClientActiveTexture(GL_TEXTURE1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, 0, tc1);
	if (first) {
		void *pp;
		printf("query u1 array %d\n", glIsEnabled(GL_TEXTURE_COORD_ARRAY));
		glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &pp); printf("query u1 ptr %d\n", pp == (void *)tc1);
		glGetIntegerv(GL_TEXTURE_COORD_ARRAY_SIZE, &v); printf("query u1 tcsize %d\n", v);
		glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &v); printf("query client active 0x%x\n", v);
	}
	glClientActiveTexture(GL_TEXTURE0);
	cellbox(4, 0);
	glDrawArrays(GL_QUADS, 0, 4);
	cellbox(5, 0);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
	cellbox(6, 0);
	glBegin(GL_QUADS);
	for (i = 0; i < 4; i++) glArrayElement(i);
	glEnd();
	/* 7: a display list of unit selection, coordinates and environments */
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tex[T_RGBA]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
	glDrawArrays(GL_QUADS, 0, 4);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glBindTexture(GL_TEXTURE_2D, tex[T_LM]);
	glActiveTexture(GL_TEXTURE0);
	glEndList();
	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	cellbox(7, 0);
	glCallList(list);
	glDeleteLists(list, 1);
	glLoadIdentity();
	if (first) {
		glGetIntegerv(GL_ACTIVE_TEXTURE, &v); printf("query active after list 0x%x\n", v);
		glActiveTexture(GL_TEXTURE1);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &v); printf("query u1 binding after list %d\n", v == (GLint)tex[T_LM]);
		glActiveTexture(GL_TEXTURE0);
	}
	/* row 1, 0: a list with glMultiTexCoord inside glBegin/glEnd */
	list = glGenLists(1);
	glNewList(list, GL_COMPILE);
	glBegin(GL_TRIANGLES);
	glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glMultiTexCoord4f(GL_TEXTURE0, 0, 0, 0, 1); glVertex2f(CX(0) + 2, CY(1) + 2);
	glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glMultiTexCoord4f(GL_TEXTURE0, 2, 0, 0, 1); glVertex2f(CX(0) + 38, CY(1) + 4);
	glMultiTexCoord2f(GL_TEXTURE1, .5f, 1); glMultiTexCoord4f(GL_TEXTURE0, 1, 2, 0, 1); glVertex2f(CX(0) + 20, CY(1) + 38);
	glEnd();
	glEndList();
	glCallList(list);
	glDeleteLists(list, 1);
	/* 1: glPushAttrib(GL_TEXTURE_BIT | GL_ENABLE_BIT) restores both units */
	glPushAttrib(GL_TEXTURE_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT);
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex[T_A]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glMultiTexCoord2f(GL_TEXTURE1, .7f, .2f);
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_TEXTURE_2D);
	glPopAttrib();
	if (first) {
		glGetIntegerv(GL_ACTIVE_TEXTURE, &v); printf("query active after pop 0x%x\n", v);
		glActiveTexture(GL_TEXTURE1);
		printf("query u1 enabled after pop %d\n", glIsEnabled(GL_TEXTURE_2D));
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &v); printf("query u1 binding after pop %d\n", v == (GLint)tex[T_LM]);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &v); printf("query u1 mode after pop 0x%x\n", v);
		glGetFloatv(GL_CURRENT_TEXTURE_COORDS, fv); printf("query u1 tc after pop %.3f %.3f\n", fv[0], fv[1]);
		glActiveTexture(GL_TEXTURE0);
		printf("query u0 enabled after pop %d\n", glIsEnabled(GL_TEXTURE_2D));
	}
	cellq(1, 1, 1);
	/* 2: glPushClientAttrib restores unit 1's array */
	glClientActiveTexture(GL_TEXTURE1);
	glTexCoordPointer(2, GL_FLOAT, 0, tc1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(3, GL_FLOAT, 4, tc0);
	glClientActiveTexture(GL_TEXTURE0);
	glPopClientAttrib();
	if (first) {
		void *pp;
		glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &v); printf("query client active after pop 0x%x\n", v);
		printf("query u1 array after pop %d\n", glIsEnabled(GL_TEXTURE_COORD_ARRAY));
		glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &pp); printf("query u1 ptr after pop %d\n", pp == (void *)tc1);
		glClientActiveTexture(GL_TEXTURE0);
		printf("query u0 array after pop %d\n", glIsEnabled(GL_TEXTURE_COORD_ARRAY));
	}
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, av);
	cellbox(2, 1);
	glDrawArrays(GL_QUADS, 0, 4);
	glLoadIdentity();
	glDisableClientState(GL_VERTEX_ARRAY);
	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);
	/* 3: clipped by the viewport: a triangle reaching far outside it */
	glViewport(CX(3), CY(1), C, C);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);
	glBegin(GL_TRIANGLES);
	glMultiTexCoord2f(GL_TEXTURE0, -1, -1); glMultiTexCoord2f(GL_TEXTURE1, -2, -1); glVertex2f(-1, -1);
	glMultiTexCoord2f(GL_TEXTURE0, 3, 0); glMultiTexCoord2f(GL_TEXTURE1, 4, 0); glVertex2f(3, -.2f);
	glMultiTexCoord2f(GL_TEXTURE0, 0, 3); glMultiTexCoord2f(GL_TEXTURE1, .5f, 5); glVertex2f(.2f, 3);
	glEnd();
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	win2d();
	/* 4: lines and points, unit 1 textured */
	glLineWidth(3);
	glBegin(GL_LINES);
	for (i = 0; i < 4; i++) {
		glMultiTexCoord2f(GL_TEXTURE1, 0, i * .25f); glMultiTexCoord2f(GL_TEXTURE0, 0, 0);
		glVertex2f(CX(4) + 3, CY(1) + 5 + i * 9);
		glMultiTexCoord2f(GL_TEXTURE1, 1, i * .25f + .1f); glMultiTexCoord2f(GL_TEXTURE0, 1, 1);
		glVertex2f(CX(4) + 37, CY(1) + 8 + i * 9);
	}
	glEnd();
	glLineWidth(1);
	glPointSize(4);
	glBegin(GL_POINTS);
	for (i = 0; i < 9; i++) {
		glMultiTexCoord2f(GL_TEXTURE1, (i % 3) * .33f, (i / 3) * .33f);
		glMultiTexCoord2f(GL_TEXTURE0, (i % 3) * .3f, (i / 3) * .3f);
		glVertex2f(CX(5) + 8 + (i % 3) * 12, CY(1) + 8 + (i / 3) * 12);
	}
	glEnd();
	glPointSize(1);
	/* row 2: unit 1's filters and wraps over an untextured unit 0 */
	unit(0, 0);
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	filters(tex[T_LM], GL_LINEAR);
	cellq(0, 2, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	cellq(1, 2, 2.5f);
	filters(tex[T_LM], GL_NEAREST);
	cellq(2, 2, 2.5f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	/* 3-4: a mipmapped texture on unit 1, a receding quad */
	glBindTexture(GL_TEXTURE_2D, tex[T_MIP]);
	for (i = 0; i < 2; i++) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				i ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
		glViewport(CX(3 + i), CY(2), C, C);
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glFrustum(-.1f, .1f, -.1f, .1f, .1f, 10);
		glBegin(GL_QUADS);
		glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glVertex3f(-1, -.3f, -.3f);
		glMultiTexCoord2f(GL_TEXTURE1, 4, 0); glVertex3f(1, -.3f, -.3f);
		glMultiTexCoord2f(GL_TEXTURE1, 4, 16); glVertex3f(1, -.3f, -8);
		glMultiTexCoord2f(GL_TEXTURE1, 0, 16); glVertex3f(-1, -.3f, -8);
		glEnd();
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	win2d();
	/* 5: a 1D texture on unit 1 */
	glDisable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_1D, tex[T_1D]);
	glEnable(GL_TEXTURE_1D);
	cellq(5, 2, 1);
	glDisable(GL_TEXTURE_1D);
	/* 6: glCopyTexSubImage2D into unit 1's binding, then drawn */
	glBindTexture(GL_TEXTURE_2D, tex[T_COPY]);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CX(0) + 4, CY(0) + 4, 16, 16);
	glEnable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE0);
	unit(0, tex[T_RGB]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glActiveTexture(GL_TEXTURE1);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glActiveTexture(GL_TEXTURE0);
	cellq(6, 2, 1);
	/* 7: flat shading and fog with both units */
	glShadeModel(GL_FLAT);
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0);
	glFogf(GL_FOG_END, 2);
	glFogfv(GL_FOG_COLOR, kcol);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, tex[T_LM]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glActiveTexture(GL_TEXTURE0);
	cellq(7, 2, 1);
	glDisable(GL_FOG);
	glShadeModel(GL_SMOOTH);
	units_off();
	/* the current texture coordinates of each unit */
	if (first) {
		glMultiTexCoord4f(GL_TEXTURE1, .1f, .2f, .3f, .4f);
		glMultiTexCoord3f(GL_TEXTURE0, .5f, .6f, .7f);
		glActiveTexture(GL_TEXTURE1);
		glGetFloatv(GL_CURRENT_TEXTURE_COORDS, fv);
		printf("query u1 current %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
		glActiveTexture(GL_TEXTURE0);
		glGetFloatv(GL_CURRENT_TEXTURE_COORDS, fv);
		printf("query u0 current %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
	}
}

/* ------------------------------------------------------------ page 5 */

/* the pixel paths with both units. A page of its own, with no vertex array
   and no texture matrix on unit 1 in it: after either (even in an earlier
   frame), Mesa 25's glBitmap / glDrawPixels texture with other coordinates
   than the raster position's - its own glGet of them is right (Mesa bugs,
   measured: artifacts/gl/phase5/probe/bmprobe.c ARR=1, mtexdbg.c) */
static void pix_cells(int col, int row)
{
	GLfloat fv[4];
	int i;
	/* glBitmap and glDrawPixels, textured on both units from the raster
	   position's texcoords */
	{
		static const unsigned char bits[32] = {
			0xff, 0xff, 0xc3, 0xc3, 0xa5, 0xa5, 0x99, 0x99, 0x99, 0x99, 0xa5, 0xa5,
			0xc3, 0xc3, 0xff, 0xff, 0xff, 0xff, 0x81, 0x81, 0x81, 0x81, 0xff, 0xff,
			0xf0, 0xf0, 0x0f, 0x0f, 0xf0, 0xf0, 0x0f, 0x0f };
		unsigned char img[16 * 16 * 3];
		glMultiTexCoord2f(GL_TEXTURE1, .6f, .3f);
		glMultiTexCoord2f(GL_TEXTURE0, .2f, .8f);
		glColor3f(1, 1, 1);
		glRasterPos2i(CX(col) + 12, CY(row) + 12);
		if (first) {
			glActiveTexture(GL_TEXTURE1);
			glGetFloatv(GL_CURRENT_RASTER_TEXTURE_COORDS, fv);
			printf("query u1 raster tc %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
			glGetFloatv(GL_CURRENT_TEXTURE_COORDS, fv);
			printf("query u1 tc %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
			glActiveTexture(GL_TEXTURE0);
			glGetFloatv(GL_CURRENT_RASTER_TEXTURE_COORDS, fv);
			printf("query u0 raster tc %.3f %.3f %.3f %.3f\n", fv[0], fv[1], fv[2], fv[3]);
		}
		if (first) {
			int u;
			for (u = 0; u < 2; u++) {
				GLint b1, b2, md, mf;
				glActiveTexture(GL_TEXTURE0 + u);
				glGetIntegerv(GL_TEXTURE_BINDING_2D, &b2);
				glGetIntegerv(GL_TEXTURE_BINDING_1D, &b1);
				glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &md);
				glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mf);
				printf("query bitmap u%d en2d %d en1d %d b2 %d b1 %d mode 0x%x minf 0x%x\n", u,
				       glIsEnabled(GL_TEXTURE_2D), glIsEnabled(GL_TEXTURE_1D),
				       b2 ? (int)(b2 - tex[0]) : -1, b1 ? (int)(b1 - tex[0]) : -1, md, mf);
			}
			glActiveTexture(GL_TEXTURE0);
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glBitmap(16, 16, 0, 0, 0, 0, bits);
		/* Mesa's bitmap cache draws a glBitmap later, with the state of
		   then (as glx_pixels.c notes): flushed here, so the reference is
		   GL's */
		glFlush();
		if (first) {
			unsigned char px[3];
			glReadPixels(CX(col) + 15, CY(row) + 15, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
			printf("read bitmap %d %d %d\n", px[0], px[1], px[2]);
		}
		/* (glDrawPixels is not drawn here: Mesa 25 textures its fragments
		   at texcoord (0, 0) on every unit, not the raster position's -
		   measured, the same probe - so its textured image is no
		   reference; gl/tests/core_test.c test_mtex checks ours exactly) */
		(void)img; (void)i;
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	}
}

static void page_pixels(void)
{
	int k;
	/* both units MODULATE; unit 1 alone; unit 1 COMBINE with the primary
	   (raster) colour; unit 0 COMBINE and unit 1 ADD */
	for (k = 0; k < 4; k++) {
		unit(0, k == 1 ? 0 : tex[T_RGB]);
		if (k == 3)
			comb(0, GL_MODULATE, GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR,
			     GL_SRC_COLOR, GL_CONSTANT, GL_SRC_ALPHA, 2.0f);
		unit(1, tex[k == 3 ? T_FB : T_LM]);
		if (k == 2)
			comb(0, GL_INTERPOLATE, GL_TEXTURE, GL_SRC_COLOR, GL_PRIMARY_COLOR,
			     GL_SRC_COLOR, GL_PREVIOUS, GL_SRC_COLOR, 1.0f);
		if (k == 3)
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
		glActiveTexture(GL_TEXTURE0);
		pix_cells(2 * k, 1);
		units_off();
	}
}

static void draw(void)
{
	win2d();
	glClearColor(.25f, .25f, .25f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);
	units_off();
	switch (page) {
	case 1: page_rgb(); break;
	case 2: page_alpha(); break;
	case 3: page_unit1(); break;
	case 4: page_state(); break;
	default: page_pixels(); break;
	}
	err("page");
}

int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	int frames, f;
	const char *ext;

	page = argc > 1 ? atoi(argv[1]) : 1;
	frames = argc > 2 ? atoi(argv[2]) : 4;
	if (!d) { fprintf(stderr, "no display\n"); return 1; }
	vi = glXChooseVisual(d, DefaultScreen(d), attr);
	if (!vi) { fprintf(stderr, "no visual\n"); return 1; }
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	swa.event_mask = StructureNotifyMask;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth,
			    InputOutput, vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
	XMapWindow(d, win);
	for (;;) {
		XEvent e;
		XNextEvent(d, &e);
		if (e.type == MapNotify) break;
	}
	ctx = glXCreateContext(d, vi, NULL, True);
	XFree(vi);
	glXMakeCurrent(d, win, ctx);
	ext = (const char *)glGetString(GL_EXTENSIONS);
	printf("query ext multitexture %d combine %d env_add %d\n",
	       ext && strstr(ext, "GL_ARB_multitexture") != NULL,
	       ext && strstr(ext, "GL_ARB_texture_env_combine") != NULL,
	       ext && strstr(ext, "GL_ARB_texture_env_add") != NULL);
	{
		GLint n = 0;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &n);
		printf("query units>=2 %d\n", n >= 2);
	}
	textures();
	for (f = 0; f < frames; f++) {
		first = f == 0;
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
