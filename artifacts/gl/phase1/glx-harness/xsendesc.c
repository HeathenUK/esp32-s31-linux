/* xsendesc NAME: send an Escape KeyPress to the top-level window titled NAME
 * (stock glxgears exits on it), to exercise the app's clean teardown. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
int main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	Window r, p, *kids; unsigned n, i; int sent = 0;
	if (!d || argc < 2) return 2;
	XQueryTree(d, DefaultRootWindow(d), &r, &p, &kids, &n);
	/* pass 0: by title; pass 1 (nothing matched, e.g. a libX11 that
	 * dropped the title): every top-level window */
	for (int pass = 0; pass < 2 && !sent; pass++)
	for (i = 0; i < n; i++) {
		char *name = NULL;
		int hit = XFetchName(d, kids[i], &name) && name && !strcmp(name, argv[1]);
		if (hit || pass == 1) {
			XEvent e; memset(&e, 0, sizeof e);
			e.xkey.type = KeyPress; e.xkey.window = kids[i]; e.xkey.root = r;
			e.xkey.display = d; e.xkey.same_screen = True;
			/* XSENDESC_KEYCODE: xlite reads wire keycodes as its
			 * own codes (XLW_ESCAPE = 4), not the server's map */
			e.xkey.keycode = getenv("XSENDESC_KEYCODE") ?
				atoi(getenv("XSENDESC_KEYCODE")) :
				XKeysymToKeycode(d, XK_Escape);
			XSendEvent(d, kids[i], True, KeyPressMask, &e);
			sent++;
		}
		if (name) XFree(name);
	}
	XSync(d, False);
	printf("xsendesc: sent to %d window(s)\n", sent);
	return sent ? 0 : 1;
}
