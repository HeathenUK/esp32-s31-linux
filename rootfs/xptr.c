// SPDX-License-Identifier: GPL-2.0-only
/*
 * Where does the X server think the pointer is?
 *
 * "The cursor does not move" has at least two causes that look identical from
 * the outside: the server is not receiving motion, or it is receiving motion
 * and not redrawing the cursor. Every indirect probe conflates them - the
 * framebuffer is unchanged either way, and so is the driver's commit counter.
 *
 * XQueryPointer asks the server directly, so it separates them in one step. If
 * the coordinates move while the panel does not, the input path is fine and the
 * cursor is not being painted. If they do not move, input is the problem and
 * there is no point looking at rendering at all.
 *
 * Usage: xptr [samples] [ms_between]
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xlib.h>

int main(int argc, char **argv)
{
	int samples = argc > 1 ? atoi(argv[1]) : 20;
	int gap_ms = argc > 2 ? atoi(argv[2]) : 200;
	Display *dpy;
	Window root, child;
	int rx, ry, wx, wy, i;
	int first_x = -1, first_y = -1, moved = 0;
	unsigned int mask;

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot open display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);

	for (i = 0; i < samples; i++) {
		if (!XQueryPointer(dpy, root, &root, &child,
				   &rx, &ry, &wx, &wy, &mask)) {
			printf("  pointer is on another screen\n");
			continue;
		}
		if (first_x < 0) {
			first_x = rx;
			first_y = ry;
		} else if (rx != first_x || ry != first_y) {
			moved = 1;
		}
		printf("  %2d  x=%-4d y=%-4d buttons=0x%x\n", i + 1, rx, ry,
		       mask >> 8);
		fflush(stdout);
		usleep(gap_ms * 1000);
	}

	XCloseDisplay(dpy);
	printf("\npointer %s during sampling\n",
	       moved ? "MOVED - input works, so the cursor is not being painted"
		     : "DID NOT MOVE - the server is not acting on motion events");
	return 0;
}
