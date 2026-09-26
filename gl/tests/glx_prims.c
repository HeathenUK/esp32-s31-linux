/* glx_prims.c - rasterisation-rule regression test for our libGL, compared
 * against Mesa through tools/glref (run.sh captures swap N):
 *     tools/glref/run.sh mesa prims 2 /src/gl/out-host/glx_prims
 *     tools/glref/run.sh ours prims 2 /src/gl/out-host/glx_prims
 * One 320x240 window, a grid of 8x6 cells of 40x40, each exercising one rule:
 *   row 0: culling / front face (CCW and CW triangles, GL_BACK, GL_FRONT, CW)
 *   row 1: GL_FLAT provoking vertex for every primitive type
 *   row 2: clipped primitives with per-vertex colour, smooth and flat
 *   row 3: lines and points (flat/smooth), line loop closure
 *   row 4: polygon mode LINE/POINT, glShadeModel inside display lists
 *   row 5: depth test and depth mask
 * Links -lGL -lX11 only, so LD_LIBRARY_PATH picks the implementation.
 * s31, MIT. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

#define W 320
#define H 240
#define C 40

static void cell(int col, int row)
{
	/* cell (col,row) counted from the top-left, as GL units 0..1 */
	glViewport(col * C, H - (row + 1) * C, C, C);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void tri_ccw(float r, float g, float b)
{
	glColor3f(r, g, b);
	glBegin(GL_TRIANGLES);
	glVertex2f(0.1f, 0.1f); glVertex2f(0.9f, 0.1f); glVertex2f(0.5f, 0.9f);
	glEnd();
}

static void tri_cw(float r, float g, float b)
{
	glColor3f(r, g, b);
	glBegin(GL_TRIANGLES);
	glVertex2f(0.1f, 0.1f); glVertex2f(0.5f, 0.9f); glVertex2f(0.9f, 0.1f);
	glEnd();
}

static const float pal[8][3] = {
	{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0},
	{0, 1, 1}, {1, 0, 1}, {1, 1, 1}, {1, .5f, 0},
};
static void col(int i) { glColor3fv(pal[i & 7]); }

static void draw(void)
{
	int i;
	glClearColor(0.2f, 0.2f, 0.2f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);

	/* row 0: culling */
	cell(0, 0); tri_ccw(1, 0, 0);
	cell(1, 0); tri_cw(0, 1, 0);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	cell(2, 0); tri_ccw(1, 0, 0);    /* drawn */
	cell(3, 0); tri_cw(0, 1, 0);     /* culled */
	glCullFace(GL_FRONT);
	cell(4, 0); tri_ccw(1, 0, 0);    /* culled */
	cell(5, 0); tri_cw(0, 1, 0);     /* drawn */
	glCullFace(GL_BACK);
	glFrontFace(GL_CW);
	cell(6, 0); tri_ccw(1, 0, 0);    /* culled */
	cell(7, 0); tri_cw(0, 1, 0);     /* drawn */
	glFrontFace(GL_CCW);
	glDisable(GL_CULL_FACE);

	/* row 1: flat provoking vertex */
	glShadeModel(GL_FLAT);
	cell(0, 1);
	glBegin(GL_TRIANGLES);
	col(0); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f); col(2); glVertex2f(.5f, .9f);
	glEnd();
	cell(1, 1);
	glBegin(GL_TRIANGLE_STRIP);
	for (i = 0; i < 6; i++) {
		col(i); glVertex2f(.1f + .16f * i, (i & 1) ? .9f : .1f);
	}
	glEnd();
	cell(2, 1);
	glBegin(GL_TRIANGLE_FAN);
	col(0); glVertex2f(.5f, .1f);
	for (i = 0; i < 5; i++) {
		col(i + 1); glVertex2f(.1f + .2f * i, .9f - (i == 0 || i == 4 ? .5f : 0));
	}
	glEnd();
	cell(3, 1);
	glBegin(GL_QUADS);
	col(0); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f);
	col(2); glVertex2f(.9f, .45f); col(3); glVertex2f(.1f, .45f);
	col(4); glVertex2f(.1f, .55f); col(5); glVertex2f(.9f, .55f);
	col(6); glVertex2f(.9f, .9f); col(7); glVertex2f(.1f, .9f);
	glEnd();
	cell(4, 1);
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < 4; i++) {
		col(2 * i); glVertex2f(.1f + .26f * i, .1f);
		col(2 * i + 1); glVertex2f(.1f + .26f * i, .9f);
	}
	glEnd();
	cell(5, 1);
	glBegin(GL_POLYGON);
	col(3); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f);
	col(2); glVertex2f(.9f, .9f); col(0); glVertex2f(.1f, .9f);
	glEnd();
	cell(6, 1);   /* a cull-enabled flat quad strip, as bounce's ball */
	glEnable(GL_CULL_FACE);
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < 4; i++) {
		col(2 * i); glVertex2f(.1f + .26f * i, .1f);
		col(2 * i + 1); glVertex2f(.1f + .26f * i, .9f);
	}
	glEnd();
	cell(7, 1);   /* the same strip, reversed winding */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < 4; i++) {
		col(2 * i); glVertex2f(.1f + .26f * i, .9f);
		col(2 * i + 1); glVertex2f(.1f + .26f * i, .1f);
	}
	glEnd();
	glDisable(GL_CULL_FACE);

	/* row 2: clipped, per-vertex colour */
	glShadeModel(GL_SMOOTH);
	cell(0, 2);
	glBegin(GL_TRIANGLES);
	col(0); glVertex2f(-.5f, .1f); col(1); glVertex2f(.9f, .1f); col(2); glVertex2f(.5f, 1.6f);
	glEnd();
	glColor3f(1, 1, 1);   /* the current colour must not leak into clipped vertices */
	cell(1, 2);
	glBegin(GL_TRIANGLES);
	col(0); glVertex2f(-.5f, -.5f); col(1); glVertex2f(1.5f, .2f); col(2); glVertex2f(.3f, 1.5f);
	glEnd();
	glShadeModel(GL_FLAT);
	cell(2, 2);
	glBegin(GL_TRIANGLES);
	col(0); glVertex2f(-.5f, .1f); col(1); glVertex2f(.9f, .1f); col(2); glVertex2f(.5f, 1.6f);
	glEnd();
	cell(3, 2);
	glBegin(GL_QUADS);
	col(0); glVertex2f(-.5f, -.5f); col(1); glVertex2f(1.5f, -.5f);
	col(4); glVertex2f(1.5f, .6f); col(5); glVertex2f(-.5f, .6f);
	glEnd();
	glShadeModel(GL_SMOOTH);
	cell(4, 2);
	glBegin(GL_LINES);
	col(0); glVertex2f(-.5f, .2f); col(2); glVertex2f(1.5f, .8f);
	col(1); glVertex2f(.5f, -.5f); col(3); glVertex2f(.5f, 1.5f);
	glEnd();
	glShadeModel(GL_FLAT);
	cell(5, 2);
	glBegin(GL_LINES);
	col(0); glVertex2f(-.5f, .2f); col(2); glVertex2f(1.5f, .8f);
	col(1); glVertex2f(.5f, -.5f); col(3); glVertex2f(.5f, 1.5f);
	glEnd();
	glShadeModel(GL_SMOOTH);
	cell(6, 2);   /* clipped against the near plane */
	glBegin(GL_TRIANGLES);
	col(0); glVertex3f(.1f, .1f, -2); col(1); glVertex3f(.9f, .1f, 0); col(2); glVertex3f(.5f, .9f, 2);
	glEnd();
	cell(7, 2);
	glBegin(GL_TRIANGLE_STRIP);
	for (i = 0; i < 6; i++) {
		col(i); glVertex2f(-.3f + .32f * i, (i & 1) ? 1.3f : -.3f);
	}
	glEnd();

	/* row 3: lines and points */
	cell(0, 3);
	glShadeModel(GL_FLAT);
	glBegin(GL_LINE_STRIP);
	for (i = 0; i < 5; i++) {
		col(i); glVertex2f(.1f + .2f * i, (i & 1) ? .8f : .2f);
	}
	glEnd();
	cell(1, 3);
	glBegin(GL_LINE_LOOP);
	col(0); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f);
	col(2); glVertex2f(.9f, .9f); col(3); glVertex2f(.1f, .9f);
	glEnd();
	glShadeModel(GL_SMOOTH);
	cell(2, 3);
	glBegin(GL_LINE_LOOP);
	col(0); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f);
	col(2); glVertex2f(.9f, .9f); col(3); glVertex2f(.1f, .9f);
	glEnd();
	cell(3, 3);
	glBegin(GL_POINTS);
	for (i = 0; i < 8; i++) {
		col(i); glVertex2f(.1f + .1f * i, .5f);
	}
	glEnd();
	cell(4, 3);   /* integer-aligned lines: which row a pixel-edge line lands on */
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, C, 0, C, -1, 1);
	glColor3f(0, 1, 1);
	glBegin(GL_LINES);
	for (i = 5; i < C; i += 10) {
		glVertex2i(i, 2); glVertex2i(i, C - 2);
		glVertex2i(2, i); glVertex2i(C - 2, i);
	}
	glEnd();
	cell(5, 3);   /* integer-aligned rectangle: pixel coverage of a 2D quad */
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, C, 0, C, -1, 1);
	glColor3f(1, 1, 0);
	glBegin(GL_QUADS);
	glVertex2i(10, 10); glVertex2i(30, 10); glVertex2i(30, 30); glVertex2i(10, 30);
	glEnd();
	cell(6, 3);   /* the same with a y-down ortho, as 2D games set it up */
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, C, C, 0, -1, 1);
	glColor3f(1, 0, 1);
	glBegin(GL_QUADS);
	glVertex2i(5, 5); glVertex2i(20, 5); glVertex2i(20, 20); glVertex2i(5, 20);
	glEnd();
	glColor3f(0, 1, 0);
	glBegin(GL_QUADS);
	glVertex2i(20, 20); glVertex2i(35, 20); glVertex2i(35, 35); glVertex2i(20, 35);
	glEnd();
	cell(7, 3);   /* a full-cell quad: must cover every pixel of the cell */
	glColor3f(0, 0, 1);
	glBegin(GL_QUADS);
	glVertex2f(0, 0); glVertex2f(1, 0); glVertex2f(1, 1); glVertex2f(0, 1);
	glEnd();

	/* row 4: polygon modes, lists */
	cell(0, 4);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	glBegin(GL_TRIANGLES);
	col(0); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f); col(2); glVertex2f(.5f, .9f);
	glEnd();
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	{
		GLuint l = glGenLists(1);
		glNewList(l, GL_COMPILE);
		glShadeModel(GL_FLAT);
		glBegin(GL_QUAD_STRIP);
		for (i = 0; i < 4; i++) {
			col(2 * i + 1); glVertex2f(.1f + .26f * i, .1f);
			col(2 * i); glVertex2f(.1f + .26f * i, .9f);
		}
		glEnd();
		glShadeModel(GL_SMOOTH);
		glEndList();
		cell(1, 4);
		glCallList(l);
		cell(2, 4);
		glRotatef(90, 0, 0, 1);
		glTranslatef(0, -1, 0);
		glCallList(l);
		glDeleteLists(l, 1);
	}
	cell(3, 4);
	glShadeModel(GL_SMOOTH);
	glBegin(GL_QUADS);
	col(0); glVertex2f(.1f, .1f); col(1); glVertex2f(.9f, .1f);
	col(2); glVertex2f(.9f, .9f); col(3); glVertex2f(.1f, .9f);
	glEnd();

	/* row 5: depth */
	glEnable(GL_DEPTH_TEST);
	cell(0, 5);
	glColor3f(1, 0, 0);
	glBegin(GL_TRIANGLES);
	glVertex3f(.1f, .1f, -.5f); glVertex3f(.9f, .1f, -.5f); glVertex3f(.5f, .9f, -.5f);
	glEnd();
	glColor3f(0, 1, 0);
	glBegin(GL_TRIANGLES);   /* behind: z = +0.5 is farther with glOrtho near=-1 */
	glVertex3f(.1f, .9f, .5f); glVertex3f(.9f, .9f, .5f); glVertex3f(.5f, .1f, .5f);
	glEnd();
	cell(1, 5);
	glColor3f(0, 1, 0);
	glBegin(GL_TRIANGLES);
	glVertex3f(.1f, .9f, .5f); glVertex3f(.9f, .9f, .5f); glVertex3f(.5f, .1f, .5f);
	glEnd();
	glColor3f(1, 0, 0);
	glBegin(GL_TRIANGLES);   /* in front, drawn second */
	glVertex3f(.1f, .1f, -.5f); glVertex3f(.9f, .1f, -.5f); glVertex3f(.5f, .9f, -.5f);
	glEnd();
	cell(2, 5);
	glDepthMask(GL_FALSE);
	glColor3f(1, 0, 0);
	glBegin(GL_TRIANGLES);
	glVertex3f(.1f, .1f, -.5f); glVertex3f(.9f, .1f, -.5f); glVertex3f(.5f, .9f, -.5f);
	glEnd();
	glDepthMask(GL_TRUE);
	glColor3f(0, 1, 0);
	glBegin(GL_TRIANGLES);   /* the red one wrote no depth: green covers it */
	glVertex3f(.1f, .9f, .5f); glVertex3f(.9f, .9f, .5f); glVertex3f(.5f, .1f, .5f);
	glEnd();
	glDisable(GL_DEPTH_TEST);
}

int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None };
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
	for (f = 0; f < frames; f++) {
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
