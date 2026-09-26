/* tear.c - validation client (not shipped): every frame is ONE solid colour
 * that changes per frame; a torn present shows two colours in one frame.
 *   tear [seconds] [w] [h]    windowed at +100+60 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <GL/gl.h>
#include <GL/glx.h>

int main(int argc, char **argv)
{
	int slow = getenv("TEAR_SLOW") != NULL, fs = getenv("TEAR_FS") != NULL, left = 0;
	int secs = argc > 1 ? atoi(argv[1]) : 10, w = argc > 2 ? atoi(argv[2]) : 300, h = argc > 3 ? atoi(argv[3]) : 300;
	int att[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes a;
	Window win;
	GLXContext ctx;
	struct timespec t0, t;
	unsigned long n = 0;
	static const float pal[6][3] = { {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {0,1,1}, {1,0,1} };

	if (!d) return 1;
	vi = glXChooseVisual(d, DefaultScreen(d), att);
	if (!vi) return 2;
	a.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	a.event_mask = StructureNotifyMask;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 100, 60, w, h, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &a);
	XStoreName(d, win, "tear");
	if (fs) {	/* EWMH fullscreen before map, as glxgears -fullscreen */
		Atom st = XInternAtom(d, "_NET_WM_STATE", False);
		Atom fa = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
		XChangeProperty(d, win, st, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fa, 1);
	}
	XMapWindow(d, win);
	ctx = glXCreateContext(d, vi, NULL, True);
	glXMakeCurrent(d, win, ctx);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		const float *c = pal[n % 6];
		while (XPending(d)) { XEvent e; XNextEvent(d, &e); }
		if (slow) {
			/* black, then the colour band by band from the top: a
			 * frame shown mid-render has black or two colours */
			int k;

			glClearColor(0, 0, 0, 1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glColor3f(c[0], c[1], c[2]);
			for (k = 0; k < 60; k++) {
				float y0 = 1.0f - k * (2.0f / 60), y1 = y0 - 2.0f / 60;
				int rep;

				for (rep = 0; rep < 3; rep++) {
					glBegin(GL_QUADS);
					glVertex2f(-1, y0); glVertex2f(1, y0);
					glVertex2f(1, y1); glVertex2f(-1, y1);
					glEnd();
				}
			}
		} else {
			glClearColor(c[0], c[1], c[2], 1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}
		glXSwapBuffers(d, win);
		n++;
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (fs && !left && t.tv_sec - t0.tv_sec >= secs / 2) {
			/* leave fullscreen the EWMH way, keeping the size */
			XEvent e = { 0 };
			e.xclient.type = ClientMessage;
			e.xclient.window = win;
			e.xclient.message_type = XInternAtom(d, "_NET_WM_STATE", False);
			e.xclient.format = 32;
			e.xclient.data.l[0] = 0;	/* _NET_WM_STATE_REMOVE */
			e.xclient.data.l[1] = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
			XSendEvent(d, RootWindow(d, vi->screen), False,
				   SubstructureRedirectMask | SubstructureNotifyMask, &e);
			XFlush(d);
			left = 1;
			printf("tear: left fullscreen at frame %lu\n", n);
			fflush(stdout);
		}
		if (t.tv_sec - t0.tv_sec >= secs) break;
	}
	printf("tear: %lu frames in %d s\n", n, secs);
	return 0;
}
