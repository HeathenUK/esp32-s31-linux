/*
 * ringhammer - provoke a lost doorbell in the XLITE-RING transport, fast.
 *
 * The bug it exists for (2026-09-20): the ring reader cleared `sig` before
 * draining its eventfd, so a writer ringing between those two steps had its
 * bell swallowed and its NEXT message was never announced. sdlbench hit it
 * once in 25-115 three-second runs. This hits the window directly: bursts of
 * one-way requests, each flushed on its own (each flush is a bell, landing
 * while the server is still acknowledging the last), then one round trip that
 * can only complete if every bell was honoured. alarm() turns a hang into a
 * result: exit 3 and the iteration it died on.
 *
 *   ringhammer [iterations=20000] [burst=8]      exit 0 = no wake-up lost
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-ringhammer.sh'
 */
#include <X11/Xlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile long iter;

static void hung(int sig)
{
	char b[96];
	int n = snprintf(b, sizeof b, "ringhammer: HUNG at iteration %ld - a doorbell was lost\n", iter);

	(void)sig;
	if (write(1, b, n) < 0) { }
	_exit(3);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 20000;
	int burst = argc > 2 ? atoi(argv[2]) : 8, k;
	Display *d = XOpenDisplay(NULL);
	struct timespec a, b;

	if (!d) { printf("ringhammer: no display\n"); return 2; }
	signal(SIGALRM, hung);
	clock_gettime(CLOCK_MONOTONIC, &a);
	for (iter = 0; iter < iters; iter++) {
		alarm(4);
		for (k = 0; k < burst; k++) {
			XNoOp(d);
			XFlush(d);		/* one bell per request */
		}
		XSync(d, False);		/* needs every one of them honoured */
	}
	alarm(0);
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("ringhammer: %ld iterations x %d flushes + 1 round trip, no wake-up lost, %.1f s\n",
	       iters, burst, (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
	XCloseDisplay(d);
	return 0;
}
