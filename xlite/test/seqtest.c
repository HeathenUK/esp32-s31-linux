/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * seqtest - xlite's sequence-number and reply-error behaviour, checked
 * against the answers stock Xlib gives on the same server.
 *
 *   1. last_request_read (what LastKnownRequestProcessed reads) is advanced
 *      by XSync, by every event and by every error read off the wire, with
 *      the 16-bit wire sequence widened to the full serial - including
 *      across the 65,536 wrap. Events and errors carry that full serial.
 *   2. A round trip whose request is answered with an X error instead of a
 *      reply returns failure after the handler has seen the error, rather
 *      than waiting for ever; errors for EARLIER requests met while waiting
 *      are handed to the handler and the wait goes on.
 *
 * Every check runs against stock libX11 too (run-host.sh), so the test's
 * expectations are Xlib's, not ours. A SIGALRM watchdog turns a hang into a
 * FAIL line and exit 3.
 *
 *   DISPLAY=:95 ./seqtest
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int nfail, npass;
static const char *stage = "start";

#define CHECK(cond, ...) do { \
	if (cond) { npass++; printf("PASS "); } \
	else { nfail++; printf("FAIL "); } \
	printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
} while (0)

/* ---- error recorder ---- */
#define MAXERR 16
static int nerr;
static unsigned long err_serial[MAXERR];
static int err_code[MAXERR];

static int on_error(Display *d, XErrorEvent *e)
{
	(void)d;
	if (nerr < MAXERR) {
		err_serial[nerr] = e->serial;
		err_code[nerr] = e->error_code;
	}
	nerr++;
	return 0;
}

static void on_alarm(int sig)
{
	(void)sig;
	/* async-signal-safe enough for a test: write + _exit */
	char b[160];
	int n = snprintf(b, sizeof(b), "FAIL watchdog: hung in '%s'\n", stage);

	write(1, b, n);
	_exit(3);
}

static Bool is_prop(Display *d, XEvent *e, XPointer a)
{
	(void)d;
	return e->type == PropertyNotify && e->xproperty.window == *(Window *)a;
}

/* The serial the next request will get. */
static unsigned long next(Display *d) { return NextRequest(d); }

/* Pump without a round trip until the handler has seen `want` errors. */
static int wait_errors(Display *d, int want)
{
	int i;

	for (i = 0; i < 400 && nerr < want; i++) {
		XPending(d);
		if (nerr < want)
			usleep(5000);
	}
	return nerr >= want;
}

/* A ChangeProperty whose PropertyNotify is read with no round trip after
 * it; `extra` no-reply requests follow it before anything is read, so the
 * wire serial must be widened against a pub.request that has moved on. */
static void event_check(Display *d, Window w, Atom a, int extra, const char *tag)
{
	XEvent ev;
	unsigned long s;
	int i;

	stage = tag;
	s = next(d);
	XChangeProperty(d, w, a, XA_STRING, 8, PropModeReplace,
			(const unsigned char *)"x", 1);
	for (i = 0; i < extra; i++)
		XNoOp(d);
	XFlush(d);
	XIfEvent(d, &ev, is_prop, (XPointer)&w);
	CHECK(ev.xany.serial == s,
	      "%s: PropertyNotify serial %lu == its request's %lu", tag,
	      ev.xany.serial, s);
	CHECK(LastKnownRequestProcessed(d) >= s,
	      "%s: LastKnownRequestProcessed %lu >= %lu after the event alone",
	      tag, LastKnownRequestProcessed(d), s);
}

static void error_check(Display *d, const char *tag)
{
	unsigned long s;
	int base = nerr;

	stage = tag;
	s = next(d);
	XMapWindow(d, (Window)0x7fffff00);	/* no reply; BadWindow */
	XFlush(d);
	CHECK(wait_errors(d, base + 1), "%s: BadWindow delivered without a round trip", tag);
	if (nerr > base) {
		CHECK(err_serial[base] == s && err_code[base] == BadWindow,
		      "%s: error serial %lu == request %lu, code %d", tag,
		      err_serial[base], s, err_code[base]);
		CHECK(LastKnownRequestProcessed(d) >= s,
		      "%s: LastKnownRequestProcessed %lu >= %lu after the error alone",
		      tag, LastKnownRequestProcessed(d), s);
	}
}

static void advance_to(Display *d, unsigned long target)
{
	while (next(d) < target) {
		XNoOp(d);
		if ((next(d) & 0x3FFF) == 0)
			XSync(d, False);	/* keep < 65536 in flight */
	}
}

int main(void)
{
	Display *d;
	Window w;
	Atom a;
	unsigned long s;
	Window root, *kids = NULL, par;
	unsigned int nk, gw, gh, bw, dep;
	int gx, gy, fmt, st;
	Atom type;
	unsigned long n, after;
	unsigned char *data = NULL;

	signal(SIGALRM, on_alarm);
	alarm(60);
	d = XOpenDisplay(NULL);
	if (!d) {
		printf("FAIL cannot open display\n");
		return 2;
	}
	XSetErrorHandler(on_error);
	w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 64, 48, 0, 0, 0);
	XSelectInput(d, w, PropertyChangeMask);
	a = XInternAtom(d, "SEQTEST", False);

	/* ---- 1. last_request_read ---- */
	stage = "sync";
	XSync(d, False);
	CHECK(LastKnownRequestProcessed(d) == next(d) - 1,
	      "sync: LastKnownRequestProcessed %lu == last request %lu",
	      LastKnownRequestProcessed(d), next(d) - 1);

	event_check(d, w, a, 0, "event");
	event_check(d, w, a, 5, "event+5");
	error_check(d, "error");

	/* The same across the 16-bit wrap: the event is numbered 0x1fffe on
	 * the client, 0xfffe on the wire, and read after the client has sent
	 * past 0x20000. */
	stage = "advance";
	advance_to(d, 0x1FFF0);
	XSync(d, False);
	advance_to(d, 0x1FFFE);
	event_check(d, w, a, 5, "wrap-event");
	CHECK(next(d) > 0x20000, "wrap: NextRequest %lu is past 0x20000", next(d));
	error_check(d, "wrap-error");
	stage = "wrap-sync";
	XSync(d, False);
	CHECK(LastKnownRequestProcessed(d) == next(d) - 1,
	      "wrap-sync: LastKnownRequestProcessed %lu == last request %lu",
	      LastKnownRequestProcessed(d), next(d) - 1);

	/* ---- 2. a round trip answered with an error ---- */
	nerr = 0;
	stage = "GetGeometry(bad)";
	s = next(d);
	st = XGetGeometry(d, (Drawable)0x7fffff00, &root, &gx, &gy, &gw, &gh,
			  &bw, &dep);
	CHECK(st == 0, "reply-error: XGetGeometry(bad) returns %d (0 = failed)", st);
	CHECK(nerr == 1 && err_code[0] == BadDrawable && err_serial[0] == s,
	      "reply-error: handler saw %d error(s), code %d serial %lu (want 1, %d, %lu)",
	      nerr, nerr ? err_code[0] : -1, nerr ? err_serial[0] : 0,
	      BadDrawable, s);

	stage = "GetWindowProperty(bad)";
	s = next(d);
	st = XGetWindowProperty(d, (Window)0x7fffff00, XA_WM_NAME, 0, 16, False,
				AnyPropertyType, &type, &fmt, &n, &after, &data);
	CHECK(st != Success && nerr == 2 && err_code[1] == BadWindow &&
	      err_serial[1] == s,
	      "reply-error: XGetWindowProperty(bad) -> %d, handler code %d serial %lu",
	      st, nerr > 1 ? err_code[1] : -1, nerr > 1 ? err_serial[1] : 0);

	stage = "QueryTree(bad)";
	st = XQueryTree(d, (Window)0x7fffff00, &root, &par, &kids, &nk);
	CHECK(st == 0 && nerr == 3, "reply-error: XQueryTree(bad) returns %d, %d errors",
	      st, nerr);

	/* An error for an EARLIER request met while waiting for a reply: it
	 * goes to the handler and the wait continues to the real answer. */
	stage = "earlier error, then GetGeometry(good)";
	s = next(d);
	XMapWindow(d, (Window)0x7fffff00);
	st = XGetGeometry(d, w, &root, &gx, &gy, &gw, &gh, &bw, &dep);
	CHECK(st == 1 && gw == 64 && gh == 48,
	      "earlier-error: XGetGeometry(good) %d -> %ux%u after an in-flight error",
	      st, gw, gh);
	CHECK(nerr == 4 && err_serial[3] == s && err_code[3] == BadWindow,
	      "earlier-error: handler saw it once (errors %d, serial %lu == %lu)",
	      nerr, nerr > 3 ? err_serial[3] : 0, s);

	/* The stream is still in step. */
	stage = "after";
	CHECK(XInternAtom(d, "WM_NAME", True) == XA_WM_NAME,
	      "after: InternAtom(WM_NAME) is still answered correctly");
	XSync(d, False);
	CHECK(LastKnownRequestProcessed(d) == next(d) - 1 && nerr == 4,
	      "after: sync caught up (%lu == %lu), no stray errors (%d)",
	      LastKnownRequestProcessed(d), next(d) - 1, nerr);

	XDestroyWindow(d, w);
	XCloseDisplay(d);
	if (nfail)
		printf("SEQTEST FAIL: %d of %d checks failed\n", nfail, nfail + npass);
	else
		printf("SEQTEST ALL PASS: %d checks\n", npass);
	return nfail ? 1 : 0;
}
