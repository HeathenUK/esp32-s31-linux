// SPDX-License-Identifier: GPL-2.0-only
/*
 * Does XPutImage work?
 *
 * Nothing in the current program set draws an image, so the whole path -
 * XCreateImage, XPutImage, the shim's PutImage handler and the shared-pixmap
 * fast route - had no client to prove it. It was also, until now, a hole:
 * XPutImage was a stub in xlite and the shim ACCEPTED opcode 72 and threw it
 * away, so an off-the-shelf application drew nothing and was told nothing.
 *
 * Draws two gradients: the left half through a PIXMAP, which takes the shared
 * path and never puts a pixel on the socket, and the right half straight to
 * the WINDOW, which cannot be shared and therefore exercises the wire request.
 * If either half is missing or torn, that half's path is broken.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 320
#define H 200

static void fill(uint16_t *p, int w, int h, int phase)
{
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int r = (x * 31) / (w - 1), g = (y * 63) / (h - 1);

			p[y * w + x] = phase ? (uint16_t)((r << 11) | (g << 5))
					     : (uint16_t)((g << 5) | (31 - r));
		}
}

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	Window win;
	Pixmap pm;
	GC gc;
	XImage *a, *b;
	uint16_t *pa, *pb;

	if (!d)
		return 1;
	win = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, W * 2, H, 0,
				  0, 0x000000);
	XStoreName(d, win, "ximgtest");
	XSelectInput(d, win, ExposureMask);
	XMapWindow(d, win);
	gc = XCreateGC(d, win, 0, NULL);
	pm = XCreatePixmap(d, win, W, H, 16);

	pa = malloc(W * H * 2);
	pb = malloc(W * H * 2);
	fill(pa, W, H, 0);
	fill(pb, W, H, 1);
	a = XCreateImage(d, NULL, 16, ZPixmap, 0, (char *)pa, W, H, 32, W * 2);
	b = XCreateImage(d, NULL, 16, ZPixmap, 0, (char *)pb, W, H, 32, W * 2);

	for (;;) {
		XEvent e;

		XNextEvent(d, &e);
		if (e.type != Expose)
			continue;
		XPutImage(d, pm, gc, a, 0, 0, 0, 0, W, H);   /* shared path */
		XCopyArea(d, pm, win, gc, 0, 0, W, H, 0, 0);
		XPutImage(d, win, gc, b, 0, 0, W, 0, W, H);  /* wire path */
		XFlush(d);
	}
	return 0;
}
