/* aaprobe.c - phase 4 SMOOTH: print the coverage profile an implementation
 * gives GL_LINE_SMOOTH / GL_POINT_SMOOTH (white, SRC_ALPHA ONE over black,
 * read back with glReadPixels, green channel 0..63), for matching our
 * coverage model to Mesa's. Run under each GL (LD_LIBRARY_PATH) on Xvfb.
 * s31, MIT. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#define W 64
#define H 64
static unsigned short px[H][W];
static void grab(void)
{
	glReadPixels(0, 0, W, H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
}
static int g(int x, int y) { return (px[y][x] >> 5) & 63; }
static void clear(void) { glClear(GL_COLOR_BUFFER_BIT); }
static void line(float x0, float y0, float x1, float y1, float w)
{
	glLineWidth(w);
	glBegin(GL_LINES); glVertex2f(x0, y0); glVertex2f(x1, y1); glEnd();
}
static void col(const char *tag, int x, int y0, int y1)
{
	int y;
	printf("%s col x=%d y%d..%d:", tag, x, y0, y1);
	for (y = y0; y <= y1; y++) printf(" %2d", g(x, y));
	printf("\n");
}
static void row(const char *tag, int y, int x0, int x1)
{
	int x;
	printf("%s row y=%d x%d..%d:", tag, y, x0, x1);
	for (x = x0; x <= x1; x++) printf(" %2d", g(x, y));
	printf("\n");
}
static void box(const char *tag, int cx, int cy, int r)
{
	int x, y;
	printf("%s box %d,%d r%d:\n", tag, cx, cy, r);
	for (y = cy + r; y >= cy - r; y--) {
		printf("   ");
		for (x = cx - r; x <= cx + r; x++) printf(" %2d", g(x, y));
		printf("\n");
	}
}
int main(void)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	float ys[] = { 10.5f, 10.25f, 10.0f, 10.75f };
	float ws[] = { 1.0f, 1.5f, 2.0f, 3.5f };
	int i, j;

	if (!d) return 1;
	vi = glXChooseVisual(d, DefaultScreen(d), attr);
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth,
			    InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XMapWindow(d, win);
	ctx = glXCreateContext(d, vi, NULL, True);
	glXMakeCurrent(d, win, ctx);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, 0, H, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glClearColor(0, 0, 0, 1);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glColor4f(1, 1, 1, 1);
	glEnable(GL_LINE_SMOOTH);
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++) {
			char tag[64];
			clear();
			line(10, ys[i], 50, ys[i], ws[j]);
			grab();
			snprintf(tag, sizeof tag, "hline y=%.2f w=%.1f", ys[i], ws[j]);
			col(tag, 30, 6, 15);
			if (i == 0) { row(tag, 10, 6, 14); row(tag, 10, 46, 54); }
		}
	clear(); line(10, 10, 50, 50, 1); grab(); box("diag45 w1", 30, 30, 3);
	clear(); line(10, 10, 50, 30, 1); grab(); box("diag26 w1", 30, 20, 3);
	clear(); line(10, 10, 50, 50, 2.5f); grab(); box("diag45 w2.5", 30, 30, 3);
	clear(); line(20.2f, 20.5f, 20.7f, 20.5f, 1); grab(); box("short0.5", 20, 20, 2);
	clear(); line(10.5f, 10.5f, 10.5f, 50.5f, 1); grab(); row("vline x=10.5 w1", 30, 7, 14);
	glDisable(GL_LINE_SMOOTH);
	glEnable(GL_POINT_SMOOTH);
	{
		float sz[] = { 1, 2, 3, 4, 5, 8 };
		for (i = 0; i < 6; i++) {
			char tag[64];
			clear();
			glPointSize(sz[i]);
			glBegin(GL_POINTS); glVertex2f(30.5f, 30.5f); glVertex2f(10, 10); glEnd();
			grab();
			snprintf(tag, sizeof tag, "point %.0f centre 30.5", sz[i]);
			box(tag, 30, 30, 5);
			snprintf(tag, sizeof tag, "point %.0f centre 10.0", sz[i]);
			box(tag, 10, 10, 5);
		}
	}
	glXMakeCurrent(d, None, NULL);
	return 0;
}
