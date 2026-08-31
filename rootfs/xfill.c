// SPDX-License-Identifier: GPL-2.0-only
/*
 * How long does one full-screen repaint actually take?
 *
 * Every earlier attempt to measure this measured something else. Frame rate
 * from mousebench is swamped by warm-up - consecutive identical runs on the
 * same boot ranged from 0 to 36 fps - and timing a loop of `xsetroot -solid`
 * measures process startup, since each iteration forks, execs, opens a display
 * and tears it down again. That loop reported 236 ms per iteration on a server
 * whose driver commit path costs about 5 ms.
 *
 * This connects once and then does nothing but damage, so what it reports is
 * the server's repaint cost and the driver's commit cost, and nothing else.
 * XSync after each fill makes the round trip complete before the clock is read,
 * so a fast client cannot run ahead of a slow server and flatter the result.
 *
 * Usage: xfill [iterations] [w h]   - w/h default to the whole screen, and
 *                                     a smaller rect exercises the driver's
 *                                     CPU-vs-PPA size threshold.
 *        xfill -s [iterations]       - XSync-only round trips: no drawing at
 *                                     all, so the number is the server's wake
 *                                     and reply latency and nothing else.
 *        xfill -w [iterations] [w h] - create a window and fill THAT. Against
 *                                     xshim the root is not a drawable (fills
 *                                     on it error out and cost only the round
 *                                     trip), so -w is the mode that actually
 *                                     exercises the fill and damage path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <X11/Xlib.h>

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	int sync_only = argc > 1 && !strcmp(argv[1], "-s");
	int windowed = argc > 1 && !strcmp(argv[1], "-w");
	int argoff = (sync_only || windowed) ? 1 : 0;
	int n = argc > 1 + argoff ? atoi(argv[1 + argoff]) : 30;
	int rw = argc > 3 + argoff ? atoi(argv[2 + argoff]) : 0;
	int rh = argc > 3 + argoff ? atoi(argv[3 + argoff]) : 0;
	Display *dpy;
	Window root, target;
	XWindowAttributes wa;
	GC gc;
	double *ms, total = 0;
	int i;

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot open display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);
	XGetWindowAttributes(dpy, root, &wa);
	if (wa.width <= 0 || wa.width > 4096) {	/* stubbed Screen fields */
		wa.width = 800;
		wa.height = 480;
	}
	if (rw <= 0 || rw > wa.width)
		rw = wa.width;
	if (rh <= 0 || rh > wa.height)
		rh = wa.height;
	target = root;
	if (windowed) {
		target = XCreateSimpleWindow(dpy, root, 0, 0, rw, rh, 0, 0, 0);
		XMapWindow(dpy, target);
		XSync(dpy, False);
	}
	gc = XCreateGC(dpy, target, 0, NULL);

	ms = calloc(n, sizeof(*ms));
	if (!ms)
		return 1;

	/* One warm-up fill: the first touches paths nothing else has. */
	XSetForeground(dpy, gc, 0x123456);
	if (!sync_only)
		XFillRectangle(dpy, target, gc, 0, 0, rw, rh);
	XSync(dpy, False);

	for (i = 0; i < n; i++) {
		double t0 = now_ms();

		/* Alternate the colour so the server cannot elide the fill. */
		if (!sync_only) {
			XSetForeground(dpy, gc,
				       (i & 1) ? 0x4682b4 : 0x191970);
			XFillRectangle(dpy, target, gc, 0, 0, rw, rh);
		}
		XSync(dpy, False);
		ms[i] = now_ms() - t0;
		total += ms[i];
	}

	qsort(ms, n, sizeof(*ms), cmp);
	if (sync_only)
		printf("%d XSync round trips, nothing drawn\n", n);
	else
		printf("%s %dx%d, %d repaints of %dx%d (%d bytes)\n",
		       windowed ? "window" : "screen",
		       wa.width, wa.height, n, rw, rh, rw * rh * 2);
	printf("  median %.1f ms   min %.1f   max %.1f   mean %.1f\n",
	       ms[n / 2], ms[0], ms[n - 1], total / n);
	printf("  implies %.1f fps sustained\n", 1000.0 / (total / n));

	XFreeGC(dpy, gc);
	XCloseDisplay(dpy);
	free(ms);
	return 0;
}
