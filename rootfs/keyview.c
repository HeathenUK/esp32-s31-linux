/*
 * keyview - print every key event an X client actually receives.
 *
 * xev is not on this board, and the question it answers is the one that
 * matters when keys go missing: does the loss happen BELOW X (keyboard,
 * receiver, HID, evdev, lvdesk's reader) or ABOVE it (our terminal widget)?
 *
 * This is deliberately the dumbest possible X client. It opens one window,
 * asks for KeyPress and KeyRelease, and prints every event with a running
 * count and the server's own timestamp. Nothing else happens in the loop, so
 * it cannot be blamed for missing anything: if a key you pressed is absent
 * here, it never reached an X client at all.
 *
 * The GAP column is the millisecond delta between consecutive events by the
 * SERVER's clock. Bursty loss shows up as normal gaps with keys simply
 * absent; a stall shows up as one huge gap. Those are different faults and
 * the number distinguishes them.
 *
 *   keyview            run until killed
 *   keyview 30         run for 30 seconds, then print a summary
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 0;
	unsigned long press = 0, release = 0, other = 0;
	Time first = 0, last = 0;
	struct timespec t0;
	Display *d;
	Window w;
	int scr;

	setvbuf(stdout, NULL, _IOLBF, 0);

	d = XOpenDisplay(NULL);
	if (!d) {
		fprintf(stderr, "keyview: cannot open display\n");
		return 1;
	}
	scr = DefaultScreen(d);
	w = XCreateSimpleWindow(d, RootWindow(d, scr), 40, 40, 360, 160, 1,
				BlackPixel(d, scr), WhitePixel(d, scr));
	XSelectInput(d, w, KeyPressMask | KeyReleaseMask | ExposureMask |
			   StructureNotifyMask);
	XStoreName(d, w, "keyview");
	XMapWindow(d, w);
	XFlush(d);

	printf("keyview: window up. Press keys. Every event is printed.\n");
	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		XEvent e;

		if (secs) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec - t0.tv_sec >= secs)
				break;
		}
		/*
		 * Blocking wait. No timeout, no polling, no work between
		 * events - so any gap seen here is the server's, not ours.
		 */
		XNextEvent(d, &e);
		switch (e.type) {
		case KeyPress:
		case KeyRelease: {
			KeySym ks = XLookupKeysym(&e.xkey, 0);
			const char *nm = XKeysymToString(ks);
			Time t = e.xkey.time;
			long gap = last ? (long)(t - last) : 0;

			if (!first)
				first = t;
			last = t;
			if (e.type == KeyPress)
				press++;
			else
				release++;
			printf("%-8s keycode=%-3u keysym=%-12s state=0x%x "
			       "t=%lu gap=%ldms  [press=%lu release=%lu]\n",
			       e.type == KeyPress ? "PRESS" : "RELEASE",
			       e.xkey.keycode, nm ? nm : "?",
			       e.xkey.state, (unsigned long)t, gap,
			       press, release);
			break;
		}
		default:
			other++;
			break;
		}
	}

	printf("keyview: SUMMARY presses=%lu releases=%lu other_events=%lu "
	       "span=%lums\n", press, release, other,
	       (unsigned long)(last - first));
	if (press != release)
		printf("keyview: WARNING %ld unmatched press/release - a lost "
		       "release is what makes a key repeat for ever\n",
		       (long)press - (long)release);
	XCloseDisplay(d);
	return 0;
}
