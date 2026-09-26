/* glx_raster.c - the rasteriser features of plan F3-F6 against Mesa,
 * through tools/glref (gl/tests/run-raster.sh runs every page under both
 * and compares them):
 *     glx_raster PAGE [frames]
 * One 320x240 window, a grid of 8x6 cells of 40x40. Pages:
 *   1 texture environments x base formats (F4, F3 formats)
 *   2 blend factor pairs and colour masks (F5)
 *   3 depth functions, depth mask, alpha functions (F5)
 *   4 fog, scissor, polygon offset, depth range (F6)
 *   5 wide lines and points, lines/points through the fragment ops (F6)
 *   6 texture uploads: every GL_UNPACK_* parameter, glTexSubImage2D,
 *     wrap modes, sizes, display lists, GLU mipmaps (F3)
 * Page 6 also prints the glGet answers it depends on to stdout, so the two
 * logs can be compared too. Links -lGL -lGLU -lX11 only, so
 * LD_LIBRARY_PATH picks the implementation. s31, MIT.
 */
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
#define C 40

static int page = 1;

static void cell(int col, int row)
{
	glViewport(col * C, H - (row + 1) * C, C, C);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* the whole cell, untextured */
static void bg(float r, float g, float b)
{
	glColor3f(r, g, b);
	glBegin(GL_QUADS);
	glVertex2f(0, 0); glVertex2f(1, 0); glVertex2f(1, 1); glVertex2f(0, 1);
	glEnd();
}

/* two vertical stripes, to blend over */
static void stripes(void)
{
	glColor3f(0.9f, 0.6f, 0.1f);
	glBegin(GL_QUADS);
	glVertex2f(0, 0); glVertex2f(.5f, 0); glVertex2f(.5f, 1); glVertex2f(0, 1);
	glColor3f(0.1f, 0.3f, 0.8f);
	glVertex2f(.5f, 0); glVertex2f(1, 0); glVertex2f(1, 1); glVertex2f(.5f, 1);
	glEnd();
}

/* a quad with a colour and alpha per corner, texcoords 0..1. The corner
   values are affine in x and y (c11 = c10 + c01 - c00), so the result does
   not depend on how a quad is split into triangles, which GL leaves to the
   implementation (TinyGL: 012 023, Mesa: 013 123) */
static void cquad(float x0, float y0, float x1, float y1, float a)
{
	glBegin(GL_QUADS);
	glColor4f(.1f, .1f, .6f, a * .2f); glTexCoord2f(0, 0); glVertex2f(x0, y0);
	glColor4f(.9f, .1f, .3f, a * .5f); glTexCoord2f(1, 0); glVertex2f(x1, y0);
	glColor4f(.9f, .9f, 0, a * .9f); glTexCoord2f(1, 1); glVertex2f(x1, y1);
	glColor4f(.1f, .9f, .3f, a * .6f); glTexCoord2f(0, 1); glVertex2f(x0, y1);
	glEnd();
}

static void wquad(float x0, float y0, float x1, float y1, float s0, float t0,
		  float s1, float t1)
{
	glBegin(GL_QUADS);
	glTexCoord2f(s0, t0); glVertex2f(x0, y0);
	glTexCoord2f(s1, t0); glVertex2f(x1, y0);
	glTexCoord2f(s1, t1); glVertex2f(x1, y1);
	glTexCoord2f(s0, t1); glVertex2f(x0, y1);
	glEnd();
}

/* a 4x4 RGBA texel pattern with varied colour and alpha */
static void pattern(unsigned char *p, int w, int h)
{
	int x, y;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			unsigned char *q = p + (y * w + x) * 4;
			q[0] = (unsigned char)(x * 255 / (w - 1 ? w - 1 : 1));
			q[1] = (unsigned char)(y * 255 / (h - 1 ? h - 1 : 1));
			q[2] = (unsigned char)(255 - (x + y) * 30);
			q[3] = (unsigned char)(((x + y) & 1) ? 255 : 90 + x * 20);
		}
}

static GLuint tex_fmt(GLint internal)
{
	unsigned char p[4 * 4 * 4];
	GLuint t;
	pattern(p, 4, 4);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, internal, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, p);
	return t;
}

/* ---------------------------------------------------------------- page 1 */

static void page_texenv(void)
{
	static const GLint fmts[8] = { GL_RGB, GL_RGBA, GL_LUMINANCE,
		GL_LUMINANCE_ALPHA, GL_ALPHA, GL_INTENSITY, 3, 2 };
	static const GLenum modes[5] = { GL_REPLACE, GL_MODULATE, GL_DECAL, GL_BLEND, GL_ADD };
	static const float envc[4] = { .1f, .8f, .9f, .6f };
	GLuint t[8];
	int i, m;

	for (i = 0; i < 8; i++)
		t[i] = tex_fmt(fmts[i]);
	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envc);
	for (m = 0; m < 6; m++) {
		for (i = 0; i < 8; i++) {
			GLenum mode = m < 5 ? modes[m] : GL_MODULATE;
			int base_rgb = fmts[i] == GL_RGB || fmts[i] == GL_RGBA || fmts[i] == 3;
			if (mode == GL_DECAL && !base_rgb)
				continue;         /* undefined for the other formats */
			cell(i, m);
			glDisable(GL_TEXTURE_2D);
			if (m == 5) stripes(); else bg(.25f, .25f, .25f);
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, t[i]);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			cquad(.1f, .1f, .9f, .9f, 1);
			glDisable(GL_BLEND);
		}
	}
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(8, t);
}

/* ---------------------------------------------------------------- page 2 */

static void page_blend(void)
{
	static const GLenum pairs[16][2] = {
		{ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA }, { GL_SRC_ALPHA, GL_ONE },
		{ GL_ONE, GL_ONE }, { GL_ZERO, GL_SRC_COLOR },
		{ GL_DST_COLOR, GL_ZERO }, { GL_DST_COLOR, GL_SRC_COLOR },
		{ GL_ONE, GL_ZERO }, { GL_ZERO, GL_ONE },
		{ GL_ONE_MINUS_DST_COLOR, GL_ONE }, { GL_SRC_ALPHA_SATURATE, GL_ONE },
		{ GL_DST_ALPHA, GL_ZERO }, { GL_ONE_MINUS_DST_ALPHA, GL_ONE },
		{ GL_ONE, GL_ONE_MINUS_SRC_COLOR }, { GL_ZERO, GL_ONE_MINUS_SRC_ALPHA },
		{ GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA }, { GL_DST_COLOR, GL_ONE },
	};
	int i;

	for (i = 0; i < 16; i++) {
		cell(i & 7, i >> 3);
		stripes();
		glEnable(GL_BLEND);
		glBlendFunc(pairs[i][0], pairs[i][1]);
		cquad(.1f, .1f, .9f, .9f, 1);
		glDisable(GL_BLEND);
	}
	/* colour masks: every RGB combination, drawn over stripes */
	for (i = 0; i < 8; i++) {
		cell(i, 2);
		stripes();
		glColorMask(i & 1, (i >> 1) & 1, (i >> 2) & 1, GL_TRUE);
		cquad(.1f, .1f, .9f, .9f, 1);
		glColorMask(1, 1, 1, 1);
	}
	/* masks with blending, and glClear under a mask */
	for (i = 0; i < 8; i++) {
		cell(i, 3);
		stripes();
		glColorMask(i & 1, (i >> 1) & 1, (i >> 2) & 1, GL_TRUE);
		if (i < 4) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			cquad(.1f, .1f, .9f, .9f, 1);
			glDisable(GL_BLEND);
		} else {
			glEnable(GL_SCISSOR_TEST);
			glScissor(i * C + 8, H - 4 * C + 8, 24, 24);
			glClearColor(1, 1, 1, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
		}
		glColorMask(1, 1, 1, 1);
	}
	/* flat shading with blending; smooth alpha on a triangle fan */
	for (i = 0; i < 4; i++) {
		cell(i, 4);
		stripes();
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, i & 1 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		glShadeModel(i & 2 ? GL_FLAT : GL_SMOOTH);
		cquad(.1f, .1f, .9f, .9f, 1);
		glShadeModel(GL_SMOOTH);
		glDisable(GL_BLEND);
	}
	/* lit, translucent material (lighting alpha = diffuse alpha) */
	for (i = 4; i < 8; i++) {
		float dif[4] = { .9f, .5f, .2f, .25f * (i - 3) };
		float pos[4] = { 0, 0, 1, 0 };
		cell(i, 4);
		stripes();
		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT0);
		glLightfv(GL_LIGHT0, GL_POSITION, pos);
		glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, dif);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glNormal3f(0, 0, 1);
		glBegin(GL_TRIANGLES);
		glVertex2f(.1f, .1f); glVertex2f(.9f, .1f); glVertex2f(.5f, .9f);
		glEnd();
		glDisable(GL_BLEND);
		glDisable(GL_LIGHTING);
	}
	/* depth + blend: a translucent quad in front of and behind an opaque one */
	for (i = 0; i < 8; i++) {
		cell(i, 5);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glColor3f(.2f, .6f, .2f);
		glBegin(GL_QUADS);
		glVertex3f(0, 0, 0); glVertex3f(.6f, 0, 0); glVertex3f(.6f, 1, 0); glVertex3f(0, 1, 0);
		glEnd();
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask(i & 1 ? GL_FALSE : GL_TRUE);
		glColor4f(.9f, .1f, .1f, .5f);
		glBegin(GL_QUADS);
		{
			float z = i & 2 ? -.5f : .5f;
			glVertex3f(.3f, .2f, z); glVertex3f(1, .2f, z); glVertex3f(1, .8f, z); glVertex3f(.3f, .8f, z);
		}
		glEnd();
		glDisable(GL_BLEND);
		glDepthMask(GL_TRUE);
		/* reveals whether the translucent quad wrote depth */
		if (i & 4) {
			glColor3f(.2f, .2f, .9f);
			glBegin(GL_QUADS);
			glVertex3f(.5f, 0, 0); glVertex3f(.9f, 0, 0); glVertex3f(.9f, .5f, 0); glVertex3f(.5f, .5f, 0);
			glEnd();
		}
		glDisable(GL_DEPTH_TEST);
	}
}

/* ---------------------------------------------------------------- page 3 */

static void bars(void)
{
	/* nearer, equal, farther than the grey background at z = 0 */
	static const float z[3] = { .5f, 0, -.5f };   /* glOrtho: +z is nearer */
	int k;
	for (k = 0; k < 3; k++) {
		float y = .1f + k * .3f;
		glColor3f(k == 0, k == 1, k == 2);
		glBegin(GL_QUADS);
		glVertex3f(.1f, y, z[k]); glVertex3f(.9f, y, z[k]);
		glVertex3f(.9f, y + .2f, z[k]); glVertex3f(.1f, y + .2f, z[k]);
		glEnd();
	}
}

static void page_depth(void)
{
	static const GLenum f[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
		GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
	int i;

	glEnable(GL_DEPTH_TEST);
	for (i = 0; i < 16; i++) {
		cell(i & 7, i >> 3);
		glDepthFunc(GL_ALWAYS);
		glColor3f(.4f, .4f, .4f);
		glBegin(GL_QUADS);
		glVertex3f(0, 0, 0); glVertex3f(1, 0, 0); glVertex3f(1, 1, 0); glVertex3f(0, 1, 0);
		glEnd();
		glDepthFunc(f[i & 7]);
		if (i >= 8) glDepthMask(GL_FALSE);
		bars();
		glDepthMask(GL_TRUE);
		/* a yellow quad at z = 0.25 with LESS shows the depth left behind */
		glDepthFunc(GL_LESS);
		glColor3f(1, 1, 0);
		glBegin(GL_QUADS);
		glVertex3f(.45f, 0, .25f); glVertex3f(.55f, 0, .25f);
		glVertex3f(.55f, 1, .25f); glVertex3f(.45f, 1, .25f);
		glEnd();
	}
	/* smooth depth: two crossing slanted quads, per function */
	for (i = 0; i < 8; i++) {
		cell(i, 2);
		glDepthFunc(GL_ALWAYS);
		glColor3f(.2f, .7f, .2f);
		glBegin(GL_QUADS);
		glVertex3f(0, 0, -.8f); glVertex3f(1, 0, .8f); glVertex3f(1, 1, .8f); glVertex3f(0, 1, -.8f);
		glEnd();
		glDepthFunc(f[i]);
		glColor3f(.8f, .2f, .6f);
		glBegin(GL_QUADS);
		glVertex3f(0, .2f, .8f); glVertex3f(1, .2f, -.8f); glVertex3f(1, .8f, -.8f); glVertex3f(0, .8f, .8f);
		glEnd();
	}
	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	/* alpha test: every function, reference 0.5, over a horizontal alpha ramp */
	for (i = 0; i < 16; i++) {
		cell(i & 7, 3 + (i >> 3));
		bg(.25f, .25f, .25f);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(f[i & 7], i < 8 ? .5f : .25f);
		glBegin(GL_QUADS);
		glColor4f(.9f, .8f, .1f, 0); glVertex2f(.05f, .1f);
		glColor4f(.1f, .8f, .9f, 1); glVertex2f(.95f, .1f);
		glColor4f(.1f, .8f, .9f, 1); glVertex2f(.95f, .9f);
		glColor4f(.9f, .8f, .1f, 0); glVertex2f(.05f, .9f);
		glEnd();
		glDisable(GL_ALPHA_TEST);
	}
	/* alpha test on a texture's alpha, with depth: the killed fragments
	   must not write depth (a later quad shows through the holes) */
	for (i = 0; i < 8; i++) {
		GLuint t = tex_fmt(i & 1 ? GL_RGBA : GL_LUMINANCE_ALPHA);
		cell(i, 5);
		bg(.1f, .1f, .3f);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, i & 2 ? GL_REPLACE : GL_MODULATE);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(i & 4 ? GL_GREATER : GL_LESS, .5f);
		glColor4f(1, 1, 1, 1);
		glBegin(GL_QUADS);
		glTexCoord2f(0, 0); glVertex3f(.1f, .1f, .5f);
		glTexCoord2f(1, 0); glVertex3f(.9f, .1f, .5f);
		glTexCoord2f(1, 1); glVertex3f(.9f, .9f, .5f);
		glTexCoord2f(0, 1); glVertex3f(.1f, .9f, .5f);
		glEnd();
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_TEXTURE_2D);
		glColor3f(.9f, .3f, .1f);
		glBegin(GL_QUADS);
		glVertex3f(0, .3f, 0); glVertex3f(1, .3f, 0); glVertex3f(1, .7f, 0); glVertex3f(0, .7f, 0);
		glEnd();
		glDisable(GL_DEPTH_TEST);
		glDeleteTextures(1, &t);
	}
}

/* ---------------------------------------------------------------- page 4 */

static void persp_cell(int col, int row)
{
	glViewport(col * C, H - (row + 1) * C, C, C);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1, 1, -1, 1, 1, 20);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* a floor receding from z = -1 to z = -19, one colour: Gouraud colour is
   interpolated linearly in screen space (TinyGL's, documented), which
   would hide what these cells test - the fog */
static void floor_quad(void)
{
	glBegin(GL_QUADS);
	glColor3f(.9f, .4f, .2f);
	glTexCoord2f(0, 0); glVertex3f(-1, -1, -1.2f);
	glTexCoord2f(2, 0); glVertex3f(1, -1, -1.2f);
	glTexCoord2f(2, 3); glVertex3f(1, -1, -19);
	glTexCoord2f(0, 3); glVertex3f(-1, -1, -19);
	glEnd();
	glBegin(GL_TRIANGLES);
	glColor3f(.9f, .9f, .9f);
	glTexCoord2f(0, 0); glVertex3f(-.8f, -.5f, -3);
	glTexCoord2f(1, 0); glVertex3f(.8f, -.5f, -8);
	glTexCoord2f(.5f, 1); glVertex3f(0, 1.5f, -14);
	glEnd();
}

static void page_fog(void)
{
	static const float fogc[4] = { .6f, .7f, .8f, 1 };
	static const GLenum modes[3] = { GL_LINEAR, GL_EXP, GL_EXP2 };
	GLuint t = tex_fmt(GL_RGB);
	int i;

	glFogfv(GL_FOG_COLOR, fogc);
	for (i = 0; i < 16; i++) {
		int m = i % 3;
		persp_cell(i & 7, i >> 3);
		glClearColor(.1f, .1f, .1f, 1);
		glEnable(GL_SCISSOR_TEST);
		glScissor((i & 7) * C, H - ((i >> 3) + 1) * C, C, C);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_FOG);
		glFogi(GL_FOG_MODE, modes[m]);
		glFogf(GL_FOG_START, i < 8 ? 2.0f : 0.5f);
		glFogf(GL_FOG_END, i < 8 ? 15.0f : 8.0f);
		glFogf(GL_FOG_DENSITY, i < 8 ? .15f : .3f);
		glShadeModel(i & 4 ? GL_FLAT : GL_SMOOTH);
		if (i & 8) {
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, t);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, i & 1 ? GL_MODULATE : GL_REPLACE);
		}
		floor_quad();
		glDisable(GL_TEXTURE_2D);
		glDisable(GL_FOG);
		glDisable(GL_DEPTH_TEST);
		glShadeModel(GL_SMOOTH);
	}
	/* scissor: triangles, lines and clears inside boxes of varied size */
	for (i = 0; i < 8; i++) {
		cell(i, 2);
		bg(.2f, .2f, .2f);
		glEnable(GL_SCISSOR_TEST);
		glScissor(i * C + 3 + i, H - 3 * C + 5, 30 - 2 * i, 20 + i);
		if (i & 1) {
			glClearColor(.3f, .8f, .3f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		cquad(-.5f, -.5f, 1.5f, 1.2f, 1);
		glColor3f(1, 1, 1);
		glBegin(GL_LINES);
		glVertex2f(0, 0); glVertex2f(1, 1);
		glVertex2f(0, 1); glVertex2f(1, 0);
		glEnd();
		glDisable(GL_SCISSOR_TEST);
	}
	/* polygon offset: a coplanar decal pulled forward, and pushed back */
	for (i = 0; i < 8; i++) {
		persp_cell(i, 3);
		glEnable(GL_SCISSOR_TEST);
		glScissor(i * C, H - 4 * C, C, C);
		glClearColor(.1f, .1f, .1f, 1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		/* a slanted plane, so the slope term (factor) matters too */
		glTranslatef(0, 0, -3);
		glRotatef(50, 1, 0, 0);
		glColor3f(.2f, .5f, .9f);
		glBegin(GL_QUADS);
		glVertex3f(-1.2f, -1.2f, 0); glVertex3f(1.2f, -1.2f, 0);
		glVertex3f(1.2f, 1.2f, 0); glVertex3f(-1.2f, 1.2f, 0);
		glEnd();
		/* the coplanar decal: pulled forward (negative) shows, pushed back
		   (positive) hides; factor alone, units alone, both */
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(i & 1 ? 0.0f : (i & 4 ? 1.5f : -1.5f),
				i & 2 ? (i & 4 ? 3.0f : -3.0f) : 0.0f);
		glColor3f(.9f, .7f, .1f);
		glBegin(GL_TRIANGLES);
		glVertex3f(-.8f, -.8f, 0); glVertex3f(.8f, -.8f, 0); glVertex3f(0, .8f, 0);
		glEnd();
		glDisable(GL_POLYGON_OFFSET_FILL);
		glDisable(GL_DEPTH_TEST);
	}
	/* depth range: the same two quads with the near one's range pushed back */
	for (i = 0; i < 8; i++) {
		cell(i, 4);
		glEnable(GL_SCISSOR_TEST);
		glScissor(i * C, H - 5 * C, C, C);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		glEnable(GL_DEPTH_TEST);
		glColor3f(.8f, .3f, .3f);
		glBegin(GL_QUADS);
		glVertex3f(0, 0, 0); glVertex3f(.7f, 0, 0); glVertex3f(.7f, .7f, 0); glVertex3f(0, .7f, 0);
		glEnd();
		glDepthRange(i * .125f, i & 1 ? 1.0f : .5f + i * .06f);
		glColor3f(.3f, .8f, .3f);
		glBegin(GL_QUADS);
		glVertex3f(.3f, .3f, .5f); glVertex3f(1, .3f, .5f); glVertex3f(1, 1, .5f); glVertex3f(.3f, 1, .5f);
		glEnd();
		glDepthRange(0, 1);
		glDisable(GL_DEPTH_TEST);
	}
	/* fogged lines and points */
	for (i = 0; i < 8; i++) {
		int k;
		persp_cell(i, 5);
		glEnable(GL_FOG);
		glFogi(GL_FOG_MODE, modes[i % 3]);
		glFogf(GL_FOG_START, 1.0f);
		glFogf(GL_FOG_END, 12.0f);
		glFogf(GL_FOG_DENSITY, .2f);
		glColor3f(1, .3f, .1f);
		glBegin(i & 4 ? GL_POINTS : GL_LINES);
		for (k = 0; k < 8; k++) {
			glVertex3f(-1 + k * .25f, -1, -1.5f);
			glVertex3f(-1 + k * .25f, 1, -1.5f - k * 2);
		}
		glEnd();
		glDisable(GL_FOG);
	}
	glDeleteTextures(1, &t);
}

/* ---------------------------------------------------------------- page 5 */

static void page_lines(void)
{
	int i, k;
	for (i = 0; i < 8; i++) {
		cell(i, 0);
		glLineWidth((float)(i + 1));
		glBegin(GL_LINES);
		glColor3f(1, 0, 0); glVertex2f(.1f, .2f);
		glColor3f(0, 0, 1); glVertex2f(.9f, .35f);
		glColor3f(0, 1, 0); glVertex2f(.3f, .5f);
		glColor3f(1, 1, 0); glVertex2f(.4f, .95f);
		glEnd();
		glLineWidth(1);
	}
	for (i = 0; i < 8; i++) {
		cell(i, 1);
		glPointSize((float)(i + 1));
		glBegin(GL_POINTS);
		for (k = 0; k < 4; k++) {
			glColor3f(k & 1, (k >> 1) & 1, 1);
			glVertex2f(.2f + .2f * k, .3f + .15f * k);
		}
		glEnd();
		glPointSize(1);
	}
	/* blended wide lines and points over stripes */
	for (i = 0; i < 8; i++) {
		cell(i, 2);
		stripes();
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, i & 1 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
		glLineWidth(i & 2 ? 3.0f : 1.0f);
		glPointSize(i & 2 ? 4.0f : 1.0f);
		glShadeModel(i & 4 ? GL_FLAT : GL_SMOOTH);
		glBegin(GL_LINE_LOOP);
		glColor4f(1, 1, 1, .8f); glVertex2f(.15f, .15f);
		glColor4f(1, 0, 1, .3f); glVertex2f(.85f, .2f);
		glColor4f(0, 1, 1, .6f); glVertex2f(.5f, .85f);
		glEnd();
		glBegin(GL_POINTS);
		glColor4f(1, 1, 1, .5f); glVertex2f(.5f, .45f);
		glEnd();
		glShadeModel(GL_SMOOTH);
		glLineWidth(1);
		glPointSize(1);
		glDisable(GL_BLEND);
	}
	/* depth mask on lines: a masked line leaves no depth behind */
	for (i = 0; i < 8; i++) {
		cell(i, 3);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(i & 4 ? GL_LEQUAL : GL_LESS);
		glDepthMask(i & 1 ? GL_FALSE : GL_TRUE);
		glLineWidth(i & 2 ? 4.0f : 2.0f);
		glColor3f(1, .5f, 0);
		glBegin(GL_LINES);
		glVertex3f(.1f, .5f, .5f); glVertex3f(.9f, .5f, .5f);
		glEnd();
		glLineWidth(1);
		glDepthMask(GL_TRUE);
		glColor3f(.2f, .4f, 1);
		glBegin(GL_QUADS);
		glVertex3f(.3f, .2f, 0); glVertex3f(.7f, .2f, 0); glVertex3f(.7f, .8f, 0); glVertex3f(.3f, .8f, 0);
		glEnd();
		glDisable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
	}
	/* polygon mode lines and points, with width */
	for (i = 0; i < 8; i++) {
		cell(i, 4);
		glPolygonMode(GL_FRONT_AND_BACK, i & 1 ? GL_POINT : GL_LINE);
		glLineWidth(i & 2 ? 3.0f : 1.0f);
		glPointSize(i & 2 ? 3.0f : 1.0f);
		if (i & 4) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		cquad(.15f, .15f, .85f, .85f, 1);
		glDisable(GL_BLEND);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glLineWidth(1);
		glPointSize(1);
	}
	/* textured lines and points */
	{
		GLuint t = tex_fmt(GL_RGB);
		for (i = 0; i < 8; i++) {
			cell(i, 5);
			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, t);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, i & 1 ? GL_MODULATE : GL_REPLACE);
			glLineWidth(i & 2 ? 2.0f : 1.0f);
			glPointSize(i & 2 ? 3.0f : 1.0f);
			glColor3f(1, 1, 1);
			glBegin(i & 4 ? GL_POINTS : GL_LINES);
			for (k = 0; k < 4; k++) {
				glTexCoord2f(k * .25f + .1f, 0); glVertex2f(.1f + k * .25f, .1f);
				glTexCoord2f(k * .25f + .1f, 1); glVertex2f(.1f + k * .2f, .9f);
			}
			glEnd();
			glLineWidth(1);
			glPointSize(1);
			glDisable(GL_TEXTURE_2D);
		}
		glDeleteTextures(1, &t);
	}
}

/* ---------------------------------------------------------------- page 6 */

static void q(GLenum target, int level, const char *what)
{
	GLint w = -1, h = -1, f = -1, r = -1, a = -1;
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_HEIGHT, &h);
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT, &f);
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_RED_SIZE, &r);
	glGetTexLevelParameteriv(target, level, GL_TEXTURE_ALPHA_SIZE, &a);
	/* sizes differ by implementation; only "has it / has it not" is compared */
	printf("query %s level %d: %dx%d fmt 0x%x red %s alpha %s\n", what, level, w, h, f,
	       r > 0 ? "yes" : "no", a > 0 ? "yes" : "no");
}

static void err(const char *what)
{
	printf("error after %s: 0x%x\n", what, glGetError());
}

static GLuint tex_new(void)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return t;
}

static void draw_tex(int col, int row, GLuint t, float s0, float t0, float s1, float t1)
{
	cell(col, row);
	bg(.2f, .2f, .2f);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor3f(1, 1, 1);
	wquad(.05f, .05f, .95f, .95f, s0, t0, s1, t1);
	glDisable(GL_TEXTURE_2D);
}

static void page_textures(int first)
{
	static unsigned char big[64 * 64 * 4], sub[16 * 16 * 4];
	unsigned short s565[8 * 8];
	GLuint t[24], list;
	int i, x, y, n = 0;

	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) {
			unsigned char *p = big + (y * 64 + x) * 4;
			p[0] = (unsigned char)(x * 4); p[1] = (unsigned char)(y * 4);
			p[2] = (unsigned char)(((x >> 3) ^ (y >> 3)) & 1 ? 255 : 0);
			p[3] = (unsigned char)(255 - x * 2);
		}
	for (i = 0; i < 16 * 16; i++) {
		sub[i * 4 + 0] = 255; sub[i * 4 + 1] = (unsigned char)(i * 3);
		sub[i * 4 + 2] = 0; sub[i * 4 + 3] = 255;
	}
	/* row 0: sizes and shapes, and wrap modes */
	t[n] = tex_new();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	if (first) { err("64x64 RGBA"); q(GL_TEXTURE_2D, 0, "64x64"); }
	draw_tex(0, 0, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	draw_tex(1, 0, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, big + 4 * 200);
	draw_tex(2, 0, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	/* (magnified: a minified nearest texture aliases, differently in any
	   two correct renderers) */
	draw_tex(3, 0, t[n++], 0, 0, 1, .25f);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, big + 4 * 64 * 20);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	draw_tex(4, 0, t[n], -1.3f, -.7f, 2.2f, 1.9f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	draw_tex(5, 0, t[n], -1.3f, -.7f, 2.2f, 1.9f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	draw_tex(6, 0, t[n], -1.3f, -.7f, 2.2f, 1.9f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	draw_tex(7, 0, t[n++], -1.3f, -.7f, 2.2f, 1.9f);

	/* row 1: every GL_UNPACK_* parameter */
	t[n] = tex_new();
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 20);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 12);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	draw_tex(0, 1, t[n++], 0, 0, 1, 1);
	{
		/* RGB bytes, 5-byte rows padded to ALIGNMENT 8 */
		static unsigned char rgb[8 * 8];
		for (i = 0; i < 64; i++) rgb[i] = (unsigned char)(i * 37);
		t[n] = tex_new();
		glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
		draw_tex(1, 1, t[n++], 0, 0, 1, 1);
		t[n] = tex_new();
		glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 4, 8, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, rgb + 1);
		draw_tex(2, 1, t[n++], 0, 0, 1, 1);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	}
	for (i = 0; i < 64; i++)
		s565[i] = (unsigned short)(((i * 4) << 11) | ((63 - i) << 5) | (i & 31));
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s565);
	draw_tex(3, 1, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 8, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s565);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	draw_tex(4, 1, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_BGRA, GL_UNSIGNED_BYTE, big);
	draw_tex(5, 1, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, 4, 8, 8, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, big);
	draw_tex(6, 1, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	{
		static float fl[4 * 4 * 3];
		for (i = 0; i < 48; i++) fl[i] = (i % 7) / 6.0f;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_FLOAT, fl);
	}
	draw_tex(7, 1, t[n++], 0, 0, 1, 1);

	/* row 2: glTexSubImage2D, with UNPACK_ROW_LENGTH as TyrQuake's lightmaps */
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 8, 4, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, sub);
	if (first) err("glTexSubImage2D");
	draw_tex(0, 2, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, big + 4 * (64 * 16 + 16));
	glTexSubImage2D(GL_TEXTURE_2D, 0, 20, 20, 12, 12, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	draw_tex(1, 2, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 4, 4, 8, 8, GL_LUMINANCE, GL_UNSIGNED_BYTE, sub);
	draw_tex(2, 2, t[n++], 0, 0, 1, 1);
	/* errors: outside the level, an unspecified level */
	glTexSubImage2D(GL_TEXTURE_2D, 0, 10, 10, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, sub);
	if (first) err("glTexSubImage2D outside the image (INVALID_VALUE)");
	glTexSubImage2D(GL_TEXTURE_2D, 3, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sub);
	if (first) err("glTexSubImage2D of a missing level (INVALID_OPERATION)");
	/* a texture compiled into a display list keeps its pixels */
	{
		static unsigned char tmp[8 * 8 * 4];
		memcpy(tmp, big, sizeof tmp);
		t[n] = tex_new();
		list = glGenLists(1);
		glNewList(list, GL_COMPILE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 2, 2, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, sub);
		glEndList();
		memset(tmp, 0, sizeof tmp);
		memset(sub, 0x40, 64);
		glCallList(list);
		draw_tex(3, 2, t[n++], 0, 0, 1, 1);
		glDeleteLists(list, 1);
	}
	/* mipmap completeness: default MIN filter with one level is untextured;
	   all levels given is textured; nearest mipmap filters use level 0 here */
	t[n] = tex_new();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	draw_tex(4, 2, t[n++], 0, 0, 1, 1);
	t[n] = tex_new();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	for (i = 0; i <= 3; i++)
		glTexImage2D(GL_TEXTURE_2D, i, GL_RGB, 8 >> i, 8 >> i, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	if (first) { q(GL_TEXTURE_2D, 1, "mip 8x8"); q(GL_TEXTURE_2D, 3, "mip 8x8"); q(GL_TEXTURE_2D, 4, "mip 8x8"); }
	draw_tex(5, 2, t[n++], 0, 0, 1, 1);

	/* row 3: GLU (the real libGLU): gluBuild2DMipmaps of a 100x60 image
	   scales it to a power of two through our proxy answers */
	{
		static unsigned char odd[100 * 60 * 3];
		for (y = 0; y < 60; y++)
			for (x = 0; x < 100; x++) {
				unsigned char *p = odd + (y * 100 + x) * 3;
				p[0] = (unsigned char)(x * 2); p[1] = (unsigned char)(y * 4);
				p[2] = (unsigned char)((x / 10 + y / 10) & 1 ? 200 : 40);
			}
		t[n] = tex_new();
		i = gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 100, 60, GL_RGB, GL_UNSIGNED_BYTE, odd);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		if (first) { printf("gluBuild2DMipmaps -> %d\n", i); q(GL_TEXTURE_2D, 0, "glu 100x60"); }
		draw_tex(0, 3, t[n++], 0, 0, .25f, .5f);
		t[n] = tex_new();
		i = gluBuild2DMipmaps(GL_TEXTURE_2D, GL_LUMINANCE_ALPHA, 100, 60, GL_RGB, GL_UNSIGNED_BYTE, odd);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		draw_tex(1, 3, t[n++], 0, 0, .25f, .5f);
	}
	/* sizes GL 1.1 refuses, and the proxies */
	if (first) {
		GLuint z = tex_new();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 512, 512, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		err("512x512 glTexImage2D");
		glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGB, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		q(GL_PROXY_TEXTURE_2D, 0, "proxy 256");
		glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA, 128, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		q(GL_PROXY_TEXTURE_2D, 0, "proxy 128x32");
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 16, 16, 0, GL_ALPHA, GL_UNSIGNED_BYTE, big);
		q(GL_TEXTURE_2D, 0, "alpha 16");
		glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 16, 16, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, big);
		q(GL_TEXTURE_2D, 0, "luminance 16");
		glDeleteTextures(1, &z);
		err("the queries");
	}
	/* row 3 cont.: 1:1 texel mapping in 2D (a 32x32 texture on 32x32 pixels) */
	t[n] = tex_new();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, big);
	glViewport(2 * C, H - 4 * C, 40, 40);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 40, 0, 40, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, t[n++]);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	wquad(4, 4, 36, 36, 0, 0, 1, 1);
	glDisable(GL_TEXTURE_2D);
	/* rows 4-5: a tiling textured floor in perspective, MODULATE by a
	   smooth colour, REPEAT far beyond [0,1] (terrain / tunnel style) */
	for (i = 0; i < 4; i++) {
		glViewport(i * 80, 0, 80, 80);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glFrustum(-1, 1, -1, 1, 1, 60);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, t[i < 2 ? 4 : 8]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, i & 1 ? GL_MODULATE : GL_REPLACE);
		glBegin(GL_QUADS);
		glColor3f(i & 2 ? 1 : .6f, 1, .7f);
		glTexCoord2f(-20, 3); glVertex3f(-10, -1, -1.5f);
		glTexCoord2f(-14, 3); glVertex3f(10, -1, -1.5f);
		glTexCoord2f(-14, 7); glVertex3f(10, -1, -12);
		glTexCoord2f(-20, 7); glVertex3f(-10, -1, -12);
		glEnd();
		glDisable(GL_TEXTURE_2D);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glDeleteTextures(n, t);
}

static void draw(int first)
{
	glClearColor(0.1f, 0.1f, 0.1f, 1);
	glClearDepth(1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);
	switch (page) {
	case 1: page_texenv(); break;
	case 2: page_blend(); break;
	case 3: page_depth(); break;
	case 4: page_fog(); break;
	case 5: page_lines(); break;
	default: page_textures(first); break;
	}
	if (first) err("the frame");
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
	for (f = 0; f < frames; f++) {
		draw(f == 0);
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
