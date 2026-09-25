/* xgrab: XGetImage the root window (any TrueColor depth) -> binary PPM on
 * stdout, decoding pixels with the visual's masks. xwd on this rig's Xvfb
 * depth 16 writes a 24-bit image whose bytes do not match its own header. */
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
static int shift(unsigned long m) { int s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
int main(void)
{
	Display *d = XOpenDisplay(NULL);
	Window r; XWindowAttributes a; XImage *im; int x, y;
	if (!d) return 1;
	r = DefaultRootWindow(d);
	XGetWindowAttributes(d, r, &a);
	im = XGetImage(d, r, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
	if (!im) return 1;
	printf("P6\n%d %d\n255\n", a.width, a.height);
	for (y = 0; y < a.height; y++)
		for (x = 0; x < a.width; x++) {
			unsigned long p = XGetPixel(im, x, y);
			unsigned long m[3] = { im->red_mask, im->green_mask, im->blue_mask };
			int i;
			for (i = 0; i < 3; i++) {
				unsigned long v = (p & m[i]) >> shift(m[i]);
				unsigned long mx = m[i] >> shift(m[i]);
				putchar((int)(v * 255 / (mx ? mx : 1)));
			}
		}
	return 0;
}
