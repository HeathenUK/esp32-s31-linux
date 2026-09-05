/*
 * What frame rate can a 320x200 software renderer actually get onto this
 * panel? Doom's inner loop is "write every pixel of a small buffer, then blit
 * it", so this measures that shape without porting anything.
 *
 *   blitbench [seconds] [w] [h]
 *
 * Two arms, because they answer different questions:
 *   blit only  - the ceiling the display path imposes, buffer untouched
 *   fill+blit  - with a full-buffer write first, which is what a renderer
 *                that touches every pixel actually costs
 *
 * CPU is taken from CLOCK_PROCESS_CPUTIME_ID, not wall time: on one core a
 * client blocked on the server reads as slow when it is merely waiting.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double cpu(void)
{
	struct timespec t;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 5;
	int w = argc > 2 ? atoi(argv[2]) : 320;
	int h = argc > 3 ? atoi(argv[3]) : 200;
	Display *dpy = XOpenDisplay(NULL);
	Window win;
	GC gc;
	XImage *im;
	unsigned short *buf;
	int scr, arm;

	if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
	scr = DefaultScreen(dpy);
	win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 40, 40, w, h, 0,
				  BlackPixel(dpy, scr), BlackPixel(dpy, scr));
	XStoreName(dpy, win, "blitbench");
	XMapWindow(dpy, win);
	XFlush(dpy);
	gc = XCreateGC(dpy, win, 0, NULL);
	buf = malloc((size_t)w * h * 2);
	if (!buf) { fprintf(stderr, "no memory\n"); return 1; }
	memset(buf, 0, (size_t)w * h * 2);
	im = XCreateImage(dpy, DefaultVisual(dpy, scr), 16, ZPixmap, 0,
			  (char *)buf, w, h, 16, w * 2);
	if (!im) { fprintf(stderr, "no image\n"); return 1; }

	printf("blitbench %dx%d, %d s per arm\n", w, h, secs);
	for (arm = 0; arm < 2; arm++) {
		double t0 = now(), c0 = cpu(), t1;
		long frames = 0;

		while ((t1 = now()) - t0 < secs) {
			if (arm) {			/* fill+blit */
				unsigned short v = (unsigned short)frames;
				int i, n = w * h;

				for (i = 0; i < n; i++)
					buf[i] = (unsigned short)(v + i);
			}
			XPutImage(dpy, win, gc, im, 0, 0, 0, 0, w, h);
			XFlush(dpy);
			frames++;
		}
		printf("  %-10s %6.1f fps   %5.1f%% cpu   %.2f ms/frame cpu\n",
		       arm ? "fill+blit" : "blit only",
		       frames / (t1 - t0),
		       100.0 * (cpu() - c0) / (t1 - t0),
		       1000.0 * (cpu() - c0) / frames);
	}
	return 0;
}
