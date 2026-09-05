/* What does SDL2 actually see through the shim?
 *
 * chocolate-doom dies with "Couldn't find matching render driver", which is
 * SDL_CreateRenderer's message for "no entry in render_drivers[] has all the
 * flags you asked for". That one string cannot distinguish an EMPTY driver
 * table (a build-configuration problem) from a table whose single software
 * entry lacks one requested flag (a display-mode problem), and the two have
 * nothing in common as fixes. It also cannot explain the 160 MB allocation
 * the kernel refused just before it.
 *
 * So ask SDL2 directly, in the order chocolate-doom asks: driver table first,
 * then the display mode that decides whether it requests PRESENTVSYNC, then
 * the window, then the two SDL_CreateRenderer calls it actually makes.
 *
 * Build:
 *   ./docker/build.sh 'cd /src && sh rootfs/build-sdl2probe.sh'
 */
#include <SDL.h>
#include <stdio.h>

static void flags_str(Uint32 f, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s%s",
		 (f & SDL_RENDERER_SOFTWARE)      ? "SOFTWARE "      : "",
		 (f & SDL_RENDERER_ACCELERATED)   ? "ACCELERATED "   : "",
		 (f & SDL_RENDERER_PRESENTVSYNC)  ? "PRESENTVSYNC "  : "",
		 (f & SDL_RENDERER_TARGETTEXTURE) ? "TARGETTEXTURE " : "");
}

int main(void)
{
	SDL_DisplayMode mode;
	SDL_Window *win;
	SDL_Renderer *r;
	SDL_Surface *surf;
	char buf[128];
	int n, i, w = 0, h = 0;

	setvbuf(stdout, NULL, _IONBF, 0);	/* the board dies mid-run; do not buffer */

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		printf("P SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	printf("P video driver = %s\n", SDL_GetCurrentVideoDriver());

	n = SDL_GetNumRenderDrivers();
	printf("P render drivers = %d\n", n);
	for (i = 0; i < n; i++) {
		SDL_RendererInfo info;

		if (SDL_GetRenderDriverInfo(i, &info) == 0) {
			flags_str(info.flags, buf, sizeof buf);
			printf("P   [%d] %-10s flags=%08x %s\n",
			       i, info.name, info.flags, buf);
		} else {
			printf("P   [%d] GetRenderDriverInfo failed: %s\n", i, SDL_GetError());
		}
	}

	if (SDL_GetCurrentDisplayMode(0, &mode) == 0)
		printf("P display mode = %dx%d refresh=%d format=%s\n",
		       mode.w, mode.h, mode.refresh_rate,
		       SDL_GetPixelFormatName(mode.format));
	else
		printf("P GetCurrentDisplayMode failed: %s\n", SDL_GetError());

	win = SDL_CreateWindow("sdl2probe", SDL_WINDOWPOS_UNDEFINED,
			       SDL_WINDOWPOS_UNDEFINED, 320, 200, 0);
	if (!win) {
		printf("P CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	SDL_GetWindowSize(win, &w, &h);
	printf("P window size = %dx%d\n", w, h);

	/* exactly what chocolate-doom asks for, in order */
	r = SDL_CreateRenderer(win, -1, SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC);
	printf("P CreateRenderer(TARGETTEXTURE|PRESENTVSYNC) = %s (%s)\n",
	       r ? "ok" : "NULL", r ? "" : SDL_GetError());
	if (!r) {
		r = SDL_CreateRenderer(win, -1, SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_SOFTWARE);
		printf("P CreateRenderer(TARGETTEXTURE|SOFTWARE) = %s (%s)\n",
		       r ? "ok" : "NULL", r ? "" : SDL_GetError());
	}
	if (!r) {
		r = SDL_CreateRenderer(win, -1, 0);
		printf("P CreateRenderer(0) = %s (%s)\n",
		       r ? "ok" : "NULL", r ? "" : SDL_GetError());
	}

	surf = SDL_GetWindowSurface(win);
	printf("P GetWindowSurface = %s (%s)\n",
	       surf ? "ok" : "NULL", surf ? "" : SDL_GetError());
	if (surf)
		printf("P   surface %dx%d pitch=%d fmt=%s\n", surf->w, surf->h,
		       surf->pitch, SDL_GetPixelFormatName(surf->format->format));

	/*
	 * Does the clock advance? chocolate-doom's TryRunTics only leaves its
	 * NetUpdate/I_Sleep loop when I_GetTime() moves, and I_GetTime is
	 * SDL_GetTicks scaled to 35 Hz. A frozen clock there is an infinite
	 * loop that polls X on every pass - which is exactly what the board
	 * shows: 96% of a core, all of it in recvmsg, and not one PutImage.
	 */
	{
		Uint32 t0 = SDL_GetTicks(), t1;
		Uint64 p0 = SDL_GetPerformanceCounter();

		SDL_Delay(300);
		t1 = SDL_GetTicks();
		printf("P GetTicks %u -> %u (delta %u, expected ~300)\n",
		       (unsigned)t0, (unsigned)t1, (unsigned)(t1 - t0));
		printf("P perf counter delta = %llu freq = %llu\n",
		       (unsigned long long)(SDL_GetPerformanceCounter() - p0),
		       (unsigned long long)SDL_GetPerformanceFrequency());
	}

	if (r) {
		SDL_RendererInfo info;

		if (SDL_GetRendererInfo(r, &info) == 0) {
			flags_str(info.flags, buf, sizeof buf);
			printf("P chosen renderer = %s (%s)\n", info.name, buf);
		}
		SDL_SetRenderDrawColor(r, 255, 0, 0, 255);
		SDL_RenderClear(r);
		SDL_RenderPresent(r);
		printf("P painted red and presented\n");
		SDL_Delay(3000);
	}
	/*
	 * Does the event queue ever DRAIN?
	 *
	 * SDL2's X11_PumpEvents is "while (X11_Pending(display)) dispatch()".
	 * If XPending never returns 0 that loop never exits, SDL_PollEvent
	 * never returns, and a caller that polls once per frame - which is
	 * every game - stops dead without drawing anything, burning a core
	 * in recvmsg. That is exactly what chocolate-doom does here, and a
	 * probe that only creates a window and paints cannot see it.
	 */
	{
		SDL_Event ev;
		int count = 0;
		Uint32 stop = SDL_GetTicks() + 2000;
		int types[8] = { 0 };

		while (SDL_GetTicks() < stop) {
			while (SDL_PollEvent(&ev)) {
				count++;
				if (count < 100000)
					types[count % 8] = ev.type;
				if (count > 200000)
					break;
			}
			if (count > 200000)
				break;
		}
		printf("P polled %d events in 2 s%s\n", count,
		       count > 200000 ? "  <-- QUEUE NEVER DRAINS" : "");
		printf("P last types: %d %d %d %d\n",
		       types[0], types[1], types[2], types[3]);
	}

	SDL_Quit();
	printf("P done\n");
	return 0;
}
