/* Ground-truth check (run on Mesa and on ours): glViewport before the first
 * draw must survive. Prints the two pixels. */
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>
int main(void)
{
	Display *d = XOpenDisplay(NULL);
	int a[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
	XVisualInfo *vi = glXChooseVisual(d, 0, a);
	XSetWindowAttributes wa; Window w; GLXContext c; XEvent e; XImage *im;
	memset(&wa, 0, sizeof wa);
	wa.colormap = XCreateColormap(d, RootWindow(d, 0), vi->visual, AllocNone);
	wa.event_mask = StructureNotifyMask;
	w = XCreateWindow(d, RootWindow(d, 0), 0, 0, 64, 48, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &wa);
	XMapWindow(d, w);
	do XNextEvent(d, &e); while (e.type != MapNotify);
	c = glXCreateContext(d, vi, NULL, True);
	glXMakeCurrent(d, w, c);
	glViewport(0, 0, 32, 24);
	glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS); glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1); glEnd();
	glXSwapBuffers(d, w); glFinish(); XSync(d, False);
	im = XGetImage(d, w, 0, 0, 64, 48, AllPlanes, ZPixmap);
	printf("in(8,40)=0x%04lx out(50,8)=0x%04lx\n", XGetPixel(im, 8, 40), XGetPixel(im, 50, 8));
	return 0;
}
