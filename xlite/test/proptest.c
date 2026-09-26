// SPDX-License-Identifier: GPL-2.0-only
/*
 * proptest.c - format-32 properties and the ICCCM hints round trip (review
 * X1). Written through the Xlib API, read back through it and as raw
 * CARD32 by a second, stock-Xlib process (xprop), and printed; run-props.sh
 * runs it under stock Xlib and under the host (LP64) xlite and diffs the
 * two. On LP64, long is 8 bytes while the wire's CARD32 is 4, which is the
 * case the board (ILP32) never exercises.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	Window w, w2;
	XSizeHints h, g;
	XWMHints wh, *gh;
	Atom prot[2], type;
	long sup = 0, raw[3] = { 7, -2, 0x12345678 };
	int fmt, i;
	unsigned long n, after;
	unsigned char *data;
	char *av[] = { "prog", "-x" };
	char cmd[512];

	if (!d) { fprintf(stderr, "no display\n"); return 2; }
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 50, 40, 0, 0, 0);
	w2 = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 50, 40, 0, 0, 0);
	memset(&h, 0, sizeof h);
	h.flags = USPosition | USSize | PMinSize | PMaxSize | PResizeInc | PAspect |
		  PBaseSize | PWinGravity;
	h.x = 11; h.y = 22; h.width = 333; h.height = 44;
	h.min_width = 5; h.min_height = 6; h.max_width = 900; h.max_height = 800;
	h.width_inc = 3; h.height_inc = 4;
	h.min_aspect.x = 1; h.min_aspect.y = 2; h.max_aspect.x = 3; h.max_aspect.y = 1;
	h.base_width = 7; h.base_height = 8; h.win_gravity = 5;
	XSetStandardProperties(d, w, "the name", "icon", None, av, 2, &h);
	XSetWMNormalHints(d, w2, &h);
	memset(&wh, 0, sizeof wh);
	wh.flags = InputHint | StateHint | IconPositionHint | WindowGroupHint;
	wh.input = True; wh.initial_state = IconicState; wh.icon_x = 12; wh.icon_y = 34;
	wh.window_group = w2;
	XSetWMHints(d, w, &wh);
	XSetTransientForHint(d, w, w2);
	prot[0] = XInternAtom(d, "WM_DELETE_WINDOW", False);
	prot[1] = XInternAtom(d, "WM_TAKE_FOCUS", False);
	XSetWMProtocols(d, w, prot, 2);
	XChangeProperty(d, w, XInternAtom(d, "S31_RAW", False), XA_INTEGER, 32,
			PropModeReplace, (unsigned char *)raw, 3);
	XSync(d, False);

	memset(&g, 0, sizeof g);
	i = XGetWMNormalHints(d, w2, &g, &sup);
	printf("normal hints: st=%d flags=0x%lx pos=%d,%d size=%dx%d min=%dx%d max=%dx%d inc=%d,%d aspect=%d/%d..%d/%d base=%dx%d gravity=%d\n",
	       i, g.flags, g.x, g.y, g.width, g.height, g.min_width, g.min_height,
	       g.max_width, g.max_height, g.width_inc, g.height_inc, g.min_aspect.x,
	       g.min_aspect.y, g.max_aspect.x, g.max_aspect.y, g.base_width,
	       g.base_height, g.win_gravity);
	gh = XGetWMHints(d, w);
	if (gh) {
		printf("wm hints: flags=0x%lx input=%d state=%d icon=%d,%d group_is_w2=%d\n",
		       gh->flags, gh->input, gh->initial_state, gh->icon_x, gh->icon_y,
		       gh->window_group == w2);
		XFree(gh);
	} else {
		printf("wm hints: NULL\n");
	}
	if (XGetWindowProperty(d, w, XInternAtom(d, "S31_RAW", False), 0, 10, False,
			       XA_INTEGER, &type, &fmt, &n, &after, &data) == Success && data) {
		long *l = (long *)data;
		printf("raw format-32 read back: fmt=%d n=%lu %ld %ld %ld\n", fmt, n, l[0], l[1], l[2]);
		XFree(data);
	}
	fflush(stdout);
	/* the wire bytes, as a stock client sees them */
	snprintf(cmd, sizeof cmd,
		 "env -u LD_LIBRARY_PATH xprop -id 0x%lx WM_NORMAL_HINTS WM_HINTS WM_TRANSIENT_FOR WM_PROTOCOLS S31_RAW | sed -e 's/0x[0-9a-f]*/ID/g'",
		 w);
	if (system(cmd) != 0) printf("xprop failed\n");
	snprintf(cmd, sizeof cmd, "env -u LD_LIBRARY_PATH xprop -id 0x%lx WM_NORMAL_HINTS", w2);
	if (system(cmd) != 0) printf("xprop failed\n");
	XCloseDisplay(d);
	return 0;
}
