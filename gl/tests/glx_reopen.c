/* glx_reopen.c - an app that closes its display and opens a new one, the
 * way SDL re-initialises video, five times over: window, context, draw,
 * swap, teardown, XCloseDisplay. Exercises GLX records that outlive a
 * Display (xlite's XESetCloseDisplay is a no-op). Exit 0 = every round drew
 * and read back red. s31, MIT. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>

int main(void)
{
	int round, bad = 0;
	for (round = 0; round < 5; round++) {
		int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None };
		Display *d = XOpenDisplay(NULL);
		XVisualInfo *vi;
		XSetWindowAttributes swa;
		Window w;
		GLXContext c;
		XImage *img;
		XEvent e;
		unsigned long px;

		if (!d) { printf("round %d: no display\n", round); return 1; }
		vi = glXChooseVisual(d, DefaultScreen(d), attr);
		if (!vi) { printf("round %d: no visual\n", round); return 1; }
		swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
		swa.border_pixel = 0;
		swa.event_mask = StructureNotifyMask;
		w = XCreateWindow(d, RootWindow(d, vi->screen), 10 * round, 0, 64, 48, 0,
				  vi->depth, InputOutput, vi->visual,
				  CWColormap | CWBorderPixel | CWEventMask, &swa);
		XMapWindow(d, w);
		do XNextEvent(d, &e); while (e.type != MapNotify);
		c = glXCreateContext(d, vi, NULL, True);
		if (!glXMakeCurrent(d, w, c)) { printf("round %d: MakeCurrent\n", round); return 1; }
		glClearColor(1, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, w);
		glXWaitGL();
		XSync(d, False);
		img = XGetImage(d, w, 32, 24, 1, 1, AllPlanes, ZPixmap);
		px = img ? XGetPixel(img, 0, 0) : 0;
		if (img) XDestroyImage(img);
		printf("round %d: pixel 0x%04lx %s\n", round, px, px == 0xf800 ? "ok" : "BAD");
		bad += px != 0xf800;
		glXMakeCurrent(d, None, NULL);
		glXDestroyContext(d, c);
		XDestroyWindow(d, w);
		XFree(vi);
		XCloseDisplay(d);
	}
	return bad != 0;
}
