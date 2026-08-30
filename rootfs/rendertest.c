// SPDX-License-Identifier: GPL-2.0-only
/*
 * Does RENDER work?
 *
 * "could not find XRender visual format" is a client's whole diagnosis of a
 * five-step handshake - QueryExtension, QueryVersion, QueryPictFormats,
 * parse, then match a visual - and it prints the same line whichever step
 * failed. This walks the steps and names the one that broke, against whatever
 * libX11/libXrender the library path points at.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <stdio.h>

int main(void)
{
	Display *d = XOpenDisplay(NULL);
	int ev = -1, er = -1, maj = -1, min = -1;
	XRenderPictFormat *f;
	Visual *v;
	XftFont *font;

	if (!d) { printf("no display\n"); return 1; }
	printf("XRenderQueryExtension: %d (event %d error %d)\n",
	       XRenderQueryExtension(d, &ev, &er), ev, er);
	printf("XRenderQueryVersion:   %d -> %d.%d\n",
	       XRenderQueryVersion(d, &maj, &min), maj, min);

	v = DefaultVisual(d, DefaultScreen(d));
	printf("DefaultVisual %p id 0x%lx depth %d\n", (void *)v,
	       (unsigned long)v->visualid, DefaultDepth(d, DefaultScreen(d)));

	f = XRenderFindVisualFormat(d, v);
	printf("FindVisualFormat(default): %p\n", (void *)f);
	if (f)
		printf("  id 0x%lx depth %d rgb %d/%d %d/%d %d/%d a %d/%d\n",
		       (unsigned long)f->id, f->depth,
		       f->direct.red, f->direct.redMask,
		       f->direct.green, f->direct.greenMask,
		       f->direct.blue, f->direct.blueMask,
		       f->direct.alpha, f->direct.alphaMask);

	f = XRenderFindStandardFormat(d, PictStandardA8);
	printf("FindStandardFormat(A8):    %p\n", (void *)f);
	f = XRenderFindStandardFormat(d, PictStandardARGB32);
	printf("FindStandardFormat(ARGB32):%p\n", (void *)f);

	font = XftFontOpenName(d, DefaultScreen(d), "");
	printf("XftFontOpenName(\"\"):       %p\n", (void *)font);
	font = XftFontOpenName(d, DefaultScreen(d), "Sans-12");
	printf("XftFontOpenName(Sans-12):  %p\n", (void *)font);
	if (font)
		printf("  ascent %d descent %d height %d\n",
		       font->ascent, font->descent, font->height);
	return 0;
}
