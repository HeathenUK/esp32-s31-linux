/* glx_p4.c - phase 4 feature test for our libGL, compared against Mesa
 * through tools/glref (gl/tests/run-p4apps.sh RUN p4):
 *     tools/glref/run.sh mesa p4 2 /src/gl/out-host/glx_p4
 *     tools/glref/run.sh ours p4 2 /src/gl/out-host/glx_p4
 * One 320x240 window with an 8-bit stencil buffer, a grid of 8x6 cells of
 * 40x40, each exercising one rule:
 *   rows 0-1: F8-STENCIL - every op, write and value masks, depth fail /
 *             pass, the late path after the alpha test and polygon stipple,
 *             lines and points, glDrawPixels / glCopyPixels of stencil,
 *             glBitmap under the stencil test, scissored stencil clears
 *   row 2:    BLEND-EQ - every equation, separate factors, constant colour
 *   row 3:    SMOOTH lines (widths 1, 2, 3.5; blended and not)
 *   row 4:    SMOOTH points
 *   row 5:    combinations
 * Stencil values are made visible by drawing a full cell per value with
 * glStencilFunc(GL_EQUAL, v) in a colour per value. Links -lGL -lX11 only,
 * so LD_LIBRARY_PATH picks the implementation. s31, MIT. */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define W 320
#define H 240
#define C 40

static void cell(int col, int row)
{
	glViewport(col * C, H - (row + 1) * C, C, C);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* the scissor box of a cell, for per-cell clears */
static void cell_scissor(int col, int row)
{
	glEnable(GL_SCISSOR_TEST);
	glScissor(col * C, H - (row + 1) * C, C, C);
}

static void quad(float x0, float y0, float x1, float y1)
{
	glBegin(GL_QUADS);
	glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
	glEnd();
}

static void quadz(float x0, float y0, float x1, float y1, float z)
{
	glBegin(GL_QUADS);
	glVertex3f(x0, y0, z); glVertex3f(x1, y0, z); glVertex3f(x1, y1, z); glVertex3f(x0, y1, z);
	glEnd();
}

static void tri(void)
{
	glBegin(GL_TRIANGLES);
	glVertex2f(.1f, .1f); glVertex2f(.9f, .15f); glVertex2f(.45f, .9f);
	glEnd();
}

static const float pal[8][3] = {
	{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0},
	{0, 1, 1}, {1, 0, 1}, {1, 1, 1}, {1, .5f, 0},
};

/* make stencil values visible: a full-cell quad for each value in vals,
   in pal[i], where the stencil equals it */
static void show(const int *vals, int n)
{
	int i;
	glEnable(GL_STENCIL_TEST);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glStencilMask(0xff);
	for (i = 0; i < n; i++) {
		glStencilFunc(GL_EQUAL, vals[i], 0xff);
		glColor3fv(pal[i & 7]);
		quad(0, 0, 1, 1);
	}
	glDisable(GL_STENCIL_TEST);
}

/* write only the stencil */
static void swrite_begin(GLenum func, int ref, GLenum sfail, GLenum zfail, GLenum zpass)
{
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(func, ref, 0xff);
	glStencilOp(sfail, zfail, zpass);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
}

static void swrite_end(void)
{
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xff);
}

static void clear_cell_stencil(int col, int row, int v)
{
	cell_scissor(col, row);
	glClearStencil(v);
	glClear(GL_STENCIL_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	glClearStencil(0);
}

static void fan(int n, float r)
{
	int i;
	glBegin(GL_LINES);
	for (i = 0; i < n; i++) {
		float a = (float)i * 3.14159265f / (float)n + 0.13f;
		glColor4f(pal[i & 7][0], pal[i & 7][1], pal[i & 7][2], 1);
		glVertex2f(.5f - r * cosf(a), .5f - r * sinf(a));
		glVertex2f(.5f + r * cosf(a), .5f + r * sinf(a));
	}
	glEnd();
}

static void stencil_rows(void)
{
	int i;
	static const int v123[] = { 1, 2, 3 };
	static const int v1[] = { 1 };

	/* 0,0: REPLACE a triangle's shape, show it */
	cell(0, 0);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	tri();
	swrite_end();
	show(v1, 1);

	/* 1,0: INCR, three overlapping quads -> 1, 2, 3 */
	cell(1, 0);
	swrite_begin(GL_ALWAYS, 0, GL_KEEP, GL_KEEP, GL_INCR);
	quad(.05f, .05f, .6f, .6f); quad(.3f, .3f, .95f, .95f); quad(.2f, .45f, .8f, .7f);
	swrite_end();
	show(v123, 3);

	/* 2,0: from 2: DECR, INVERT (253), and a DECR at 0 stays 0 */
	clear_cell_stencil(2, 0, 2);
	cell(2, 0);
	swrite_begin(GL_ALWAYS, 0, GL_KEEP, GL_KEEP, GL_DECR);
	quad(.05f, .05f, .5f, .95f);
	swrite_end();
	swrite_begin(GL_ALWAYS, 0, GL_KEEP, GL_KEEP, GL_INVERT);
	quad(.5f, .05f, .95f, .5f);
	swrite_end();
	{ static const int v[] = { 1, 253, 2 }; show(v, 3); }

	/* 3,0: depth fail vs pass: a near quad writes depth; a far quad
	   does KEEP / INCR (zfail) / REPLACE 5 (zpass) */
	cell(3, 0);
	glEnable(GL_DEPTH_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	quadz(.1f, .1f, .6f, .6f, -.5f);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	swrite_begin(GL_ALWAYS, 5, GL_KEEP, GL_INCR, GL_REPLACE);
	quadz(.3f, .3f, .9f, .9f, .5f);
	swrite_end();
	glDisable(GL_DEPTH_TEST);
	{ static const int v[] = { 1, 5 }; show(v, 2); }

	/* 4,0: write mask 0x0f: REPLACE 0xff stores 0x0f */
	cell(4, 0);
	glStencilMask(0x0f);
	swrite_begin(GL_ALWAYS, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	glStencilMask(0x0f);
	quad(.2f, .2f, .8f, .8f);
	swrite_end();
	{ static const int v[] = { 0x0f, 0xff }; show(v, 2); }

	/* 5,0: value mask: store 0x13, pass EQUAL 0x03 under mask 0x0f */
	cell(5, 0);
	swrite_begin(GL_ALWAYS, 0x13, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(.1f, .1f, .7f, .9f);
	swrite_end();
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_EQUAL, 0x03, 0x0f);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glColor3f(1, .5f, 0);
	quad(0, 0, 1, 1);
	glStencilFunc(GL_EQUAL, 0x03, 0xff);   /* not with all bits */
	glColor3f(1, 0, 0);
	quad(0, 0, 1, 1);
	glDisable(GL_STENCIL_TEST);

	/* 6,0: glDrawPixels(GL_STENCIL_INDEX) of a checker, then show */
	cell(6, 0);
	{
		static GLubyte img[24 * 24];
		int x, y;
		for (y = 0; y < 24; y++)
			for (x = 0; x < 24; x++)
				img[y * 24 + x] = (GLubyte)((((x >> 2) ^ (y >> 2)) & 1) ? 1 : 2);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glRasterPos2f(.2f, .2f);
		glDrawPixels(24, 24, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, img);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	}
	{ static const int v[] = { 1, 2 }; show(v, 2); }

	/* 7,0: the late path: an alpha test (GREATER 0.5) on an alpha
	   gradient decides which fragments REPLACE the stencil */
	cell(7, 0);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.5f);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	glBegin(GL_QUADS);
	glColor4f(1, 1, 1, 0); glVertex2f(.05f, .1f);
	glColor4f(1, 1, 1, 1); glVertex2f(.95f, .1f);
	glColor4f(1, 1, 1, 1); glVertex2f(.95f, .9f);
	glColor4f(1, 1, 1, 0); glVertex2f(.05f, .9f);
	glEnd();
	swrite_end();
	glDisable(GL_ALPHA_TEST);
	show(v1, 1);

	/* 0,1: polygon stipple (late path) writes the stencil */
	cell(0, 1);
	{
		GLubyte pat[128];
		for (i = 0; i < 128; i++) pat[i] = (GLubyte)((i / 4) & 2 ? 0xcc : 0x33);
		glPolygonStipple(pat);
	}
	glEnable(GL_POLYGON_STIPPLE);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(.1f, .1f, .9f, .9f);
	swrite_end();
	glDisable(GL_POLYGON_STIPPLE);
	show(v1, 1);

	/* 1,1: lines (width 1 and 3) write the stencil */
	cell(1, 1);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	glBegin(GL_LINES);
	glVertex2f(.1f, .2f); glVertex2f(.9f, .6f);
	glEnd();
	glLineWidth(3);
	glBegin(GL_LINES);
	glVertex2f(.2f, .9f); glVertex2f(.6f, .1f);
	glEnd();
	glLineWidth(1);
	swrite_end();
	show(v1, 1);

	/* 2,1: points (size 1 and 4) write the stencil */
	cell(2, 1);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	glBegin(GL_POINTS);
	for (i = 0; i < 6; i++) glVertex2f(.1f + .15f * i, .2f);
	glEnd();
	glPointSize(4);
	glBegin(GL_POINTS);
	for (i = 0; i < 4; i++) glVertex2f(.15f + .2f * i, .7f);
	glEnd();
	glPointSize(1);
	swrite_end();
	show(v1, 1);

	/* 3,1: glCopyPixels(GL_STENCIL) of cell (0,0) into this cell */
	cell(3, 1);
	glRasterPos2f(0, 0);
	glCopyPixels(0, H - C, C, C, GL_STENCIL);
	show(v1, 1);

	/* 4,1: the other functions, on a 1-2-3 staircase */
	cell(4, 1);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(0, 0, .33f, 1);
	glStencilFunc(GL_ALWAYS, 2, 0xff);
	quad(.33f, 0, .66f, 1);
	glStencilFunc(GL_ALWAYS, 3, 0xff);
	quad(.66f, 0, 1, 1);
	swrite_end();
	glEnable(GL_STENCIL_TEST);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glStencilFunc(GL_LESS, 1, 0xff);       /* 1 < s: 2, 3 */
	glColor3f(1, 0, 0); quad(0, .66f, 1, 1);
	glStencilFunc(GL_GEQUAL, 2, 0xff);     /* 2 >= s: 1, 2 */
	glColor3f(0, 1, 0); quad(0, .33f, 1, .66f);
	glStencilFunc(GL_NOTEQUAL, 2, 0xff);   /* 1, 3 */
	glColor3f(0, 0, 1); quad(0, 0, 1, .33f);
	glDisable(GL_STENCIL_TEST);

	/* 5,1: INCR_WRAP from 255 -> 0, DECR_WRAP from 0 -> 255 */
	clear_cell_stencil(5, 1, 255);
	cell(5, 1);
	swrite_begin(GL_ALWAYS, 0, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
	quad(.1f, .1f, .5f, .9f);
	swrite_end();
	swrite_begin(GL_ALWAYS, 0, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
	quad(.3f, .3f, .9f, .7f);
	swrite_end();
	{ static const int v[] = { 0, 255, 254 }; show(v, 3); }

	/* 6,1: depth GL_NEVER: every fragment takes the depth-fail op */
	cell(6, 1);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_NEVER);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_REPLACE, GL_KEEP);
	tri();
	swrite_end();
	glDepthFunc(GL_LESS);
	glDisable(GL_DEPTH_TEST);
	show(v1, 1);

	/* 7,1: glBitmap under the stencil test (the pixel path) */
	cell(7, 1);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(0, 0, .5f, 1);
	swrite_end();
	{
		static GLubyte bits[32 * 4];
		for (i = 0; i < 128; i++) bits[i] = (GLubyte)(i & 4 ? 0xf0 : 0x0f);
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_EQUAL, 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glColor3f(1, 1, 0);
		glRasterPos2f(.1f, .1f);
		glBitmap(32, 32, 0, 0, 0, 0, bits);
		glDisable(GL_STENCIL_TEST);
	}
}

static void blend_row(void)
{
	int i;
	/* 0,2: ADD ONE,ONE */
	cell(0, 2);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE);
	for (i = 0; i < 3; i++) {
		glColor3f(i == 0 ? .8f : 0, i == 1 ? .8f : 0, i == 2 ? .8f : 0);
		quad(.1f + .2f * i, .1f + .15f * i, .6f + .2f * i, .6f + .15f * i);
	}
	/* 1,2: SUBTRACT on white */
	cell(1, 2);
	glDisable(GL_BLEND);
	glColor3f(1, 1, 1); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_SUBTRACT);
	glBlendFunc(GL_ONE, GL_ONE);
	glColor3f(.5f, .5f, .5f); quad(.1f, .1f, .7f, .7f);    /* src - dst: 0 */
	glBlendFunc(GL_ONE, GL_ZERO);
	glColor3f(.25f, .75f, .5f); quad(.4f, .4f, .9f, .9f);  /* src */
	/* 2,2: REVERSE_SUBTRACT on grey */
	cell(2, 2);
	glDisable(GL_BLEND);
	glColor3f(.7f, .7f, .7f); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
	glBlendFunc(GL_ONE, GL_ONE);
	glColor3f(.6f, .2f, 0); quad(.1f, .1f, .7f, .7f);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glColor4f(0, 1, 1, .5f); quad(.4f, .3f, .9f, .9f);
	/* 3,2: MIN of a gradient over grey */
	cell(3, 2);
	glDisable(GL_BLEND);
	glColor3f(.5f, .5f, .5f); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendEquation(GL_MIN);
	glBegin(GL_QUADS);
	glColor3f(0, 0, 1); glVertex2f(.1f, .1f); glColor3f(1, 0, 0); glVertex2f(.9f, .1f);
	glColor3f(1, 1, 0); glVertex2f(.9f, .9f); glColor3f(0, 1, 1); glVertex2f(.1f, .9f);
	glEnd();
	/* 4,2: MAX */
	cell(4, 2);
	glDisable(GL_BLEND);
	glColor3f(.5f, .5f, .5f); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendEquation(GL_MAX);
	glBegin(GL_QUADS);
	glColor3f(0, 0, 1); glVertex2f(.1f, .1f); glColor3f(1, 0, 0); glVertex2f(.9f, .1f);
	glColor3f(1, 1, 0); glVertex2f(.9f, .9f); glColor3f(0, 1, 1); glVertex2f(.1f, .9f);
	glEnd();
	/* 5,2: separate factors: RGB SRC_ALPHA/1-SRC_ALPHA, alpha ONE/ZERO */
	cell(5, 2);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	glColor4f(1, 0, 0, .5f); quad(.05f, .05f, .7f, .7f);
	glColor4f(0, 0, 1, .3f); quad(.3f, .3f, .95f, .95f);
	/* 6,2: constant colour factors */
	cell(6, 2);
	glDisable(GL_BLEND);
	glColor3f(0, .5f, 1); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendColor(1, .5f, .25f, .75f);
	glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_ALPHA);
	glColor3f(1, 1, 1); quad(.1f, .1f, .9f, .6f);
	glBlendFunc(GL_CONSTANT_ALPHA, GL_ZERO);
	glColor3f(1, .5f, 0); quad(.1f, .65f, .9f, .9f);
	/* 7,2: MAX on lines and points (the general line path) */
	cell(7, 2);
	glDisable(GL_BLEND);
	glColor3f(.3f, .3f, .3f); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendEquation(GL_MAX);
	glLineWidth(2);
	fan(6, .4f);
	glLineWidth(1);
	glPointSize(3);
	glBegin(GL_POINTS);
	for (i = 0; i < 5; i++) { glColor3fv(pal[i]); glVertex2f(.1f + .2f * i, .1f); }
	glEnd();
	glPointSize(1);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ZERO);
	glBlendColor(0, 0, 0, 0);
	glDisable(GL_BLEND);
}

static void smooth_rows(void)
{
	int i;
	/* row 3: GL_LINE_SMOOTH, additive as rRootage */
	glEnable(GL_LINE_SMOOTH);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	cell(0, 3); fan(8, .45f);
	cell(1, 3); glLineWidth(2); fan(6, .45f);
	cell(2, 3); glLineWidth(3.5f); fan(4, .4f);
	glLineWidth(1);
	cell(3, 3);
	glBegin(GL_LINE_LOOP);
	for (i = 0; i < 8; i++) {
		float a = (float)i * 3.14159265f / 4.0f;
		glColor4f(pal[i][0], pal[i][1], pal[i][2], .8f);
		glVertex2f(.5f + .4f * cosf(a), .5f + .4f * sinf(a));
	}
	glEnd();
	cell(4, 3);
	glDisable(GL_BLEND);
	glColor3f(.4f, .4f, .4f); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glLineWidth(1.5f);
	fan(6, .45f);
	glLineWidth(1);
	cell(5, 3);                          /* smooth without blending: aliased */
	glDisable(GL_BLEND);
	fan(6, .45f);
	cell(6, 3);                          /* with depth: half behind a quad */
	glEnable(GL_DEPTH_TEST);
	glColor3f(.2f, .2f, .6f);
	glDisable(GL_LINE_SMOOTH);
	quadz(.5f, 0, 1, 1, -.5f);
	glEnable(GL_LINE_SMOOTH);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glBegin(GL_LINES);
	for (i = 0; i < 5; i++) {
		glColor4f(1, 1, .5f, 1);
		glVertex3f(.05f, .1f + .18f * i, 0); glVertex3f(.95f, .15f + .15f * i, 0);
	}
	glEnd();
	glDisable(GL_DEPTH_TEST);
	cell(7, 3);                          /* short, axis-aligned */
	glBegin(GL_LINES);
	glColor4f(1, 1, 1, 1);
	glVertex2f(.1f, .2f); glVertex2f(.9f, .2f);
	glVertex2f(.2f, .1f); glVertex2f(.2f, .9f);
	glVertex2f(.5f, .5f); glVertex2f(.51f, .52f);
	glVertex2f(.7f, .7f); glVertex2f(.75f, .72f);
	glEnd();
	glDisable(GL_LINE_SMOOTH);

	/* row 4: GL_POINT_SMOOTH */
	glEnable(GL_POINT_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	cell(0, 4);
	for (i = 1; i <= 6; i++) {
		glPointSize((float)i);
		glBegin(GL_POINTS);
		glColor4fv(pal[i & 7]);
		glVertex2f(.1f + .14f * (i - 1), .3f);
		glVertex2f(.12f + .14f * (i - 1), .72f);
		glEnd();
	}
	cell(1, 4);
	glPointSize(8); glBegin(GL_POINTS); glColor3f(1, .5f, 0); glVertex2f(.3f, .3f); glEnd();
	glPointSize(12); glBegin(GL_POINTS); glColor3f(0, 1, 1); glVertex2f(.65f, .65f); glEnd();
	cell(2, 4);
	glPointSize(2.5f); glBegin(GL_POINTS); glColor3f(1, 1, 1); glVertex2f(.25f, .25f); glVertex2f(.75f, .25f); glEnd();
	glPointSize(3.5f); glBegin(GL_POINTS); glColor3f(1, 1, 0); glVertex2f(.25f, .75f); glVertex2f(.73f, .77f); glEnd();
	cell(3, 4);                          /* without blending: aliased */
	glDisable(GL_BLEND);
	glPointSize(5); glBegin(GL_POINTS); glColor3f(1, 0, 1); glVertex2f(.3f, .5f); glVertex2f(.7f, .5f); glEnd();
	cell(4, 4);
	glColor3f(.4f, .4f, .4f); quad(0, 0, 1, 1);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glPointSize(7); glBegin(GL_POINTS); glColor4f(1, 0, 0, 1); glVertex2f(.3f, .4f);
	glColor4f(0, 1, 0, .5f); glVertex2f(.6f, .6f); glEnd();
	glPointSize(1);
	glDisable(GL_POINT_SMOOTH);
	glDisable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ZERO);
}

static void combo_row(void)
{
	static const int v1[] = { 1 };
	/* 0,5: stencil mask, then a blended quad only inside (reflect-like) */
	cell(0, 5);
	glColor3f(.5f, 0, 0); quad(0, 0, 1, 1);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	tri();
	swrite_end();
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(0, 1, 0, .5f); quad(0, 0, 1, 1);
	glDisable(GL_BLEND);
	glDisable(GL_STENCIL_TEST);
	/* 1,5: a scissored stencil clear inside a written area */
	cell(1, 5);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(0, 0, 1, 1);
	swrite_end();
	glEnable(GL_SCISSOR_TEST);
	glScissor(1 * C + 10, H - 6 * C + 10, 20, 15);
	glClear(GL_STENCIL_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	show(v1, 1);
	/* 2,5: a masked stencil clear (write mask 0x02) */
	cell(2, 5);
	swrite_begin(GL_ALWAYS, 3, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(.2f, .2f, .8f, .8f);
	swrite_end();
	cell_scissor(2, 5);
	glStencilMask(0x02);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	glStencilMask(0xff);
	glDisable(GL_SCISSOR_TEST);
	{ static const int v[] = { 1, 3 }; show(v, 2); }
	/* 3,5: stencil + fog + smooth-shaded triangle */
	cell(3, 5);
	swrite_begin(GL_ALWAYS, 1, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad(.1f, .1f, .6f, .9f);
	swrite_end();
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_NOTEQUAL, 1, 0xff);
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, -1); glFogf(GL_FOG_END, 1);
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0); glVertex3f(0, 0, .9f);
	glColor3f(0, 1, 0); glVertex3f(1, .2f, -.9f);
	glColor3f(0, 0, 1); glVertex3f(.5f, 1, 0);
	glEnd();
	glDisable(GL_FOG);
	glDisable(GL_STENCIL_TEST);
}

static void draw(void)
{
	glClearColor(0.1f, 0.1f, 0.1f, 1);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);
	stencil_rows();
	blend_row();
	smooth_rows();
	combo_row();
}

int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, GLX_STENCIL_SIZE, 8, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	int frames = argc > 1 ? atoi(argv[1]) : 10, f;

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
	{
		GLint sb = 0;
		glGetIntegerv(GL_STENCIL_BITS, &sb);
		fprintf(stderr, "glx_p4: GL_STENCIL_BITS %d\n", sb);
	}
	for (f = 0; f < frames; f++) {
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
