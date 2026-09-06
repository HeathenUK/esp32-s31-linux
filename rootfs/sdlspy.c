/* Why does prboom present black frames? An LD_PRELOAD shim for SDL 1.2.
 *
 * prboom is off-the-shelf and must not be patched, and its own log only
 * reports the flags it REQUESTED (0x60000000 = SDL_DOUBLEBUF|SDL_HWPALETTE),
 * not what SDL_SetVideoMode actually returned. The distinction is the whole
 * question, because prboom caches the framebuffer pointer exactly once:
 *
 *     if (!SDL_MUSTLOCK(screen)) { screens[0].data = screen->pixels; }
 *
 * and never re-reads it. If SDL hands back a genuinely double-buffered
 * surface, SDL_Flip swaps ->pixels underneath that cached pointer and every
 * frame Doom draws lands in a buffer that is never shown - which looks
 * exactly like "the renderer produces black".
 *
 * So report, per flip: where ->pixels points, whether it MOVED since the last
 * flip, and how many bytes in it are non-zero. That separates three cases
 * that all present as a black window:
 *
 *   pixels moves            -> the cached-pointer/double-buffer trap
 *   pixels static, all zero -> Doom really is not drawing
 *   pixels static, non-zero -> we send content and it dies below SDL
 *
 * Build:  ./docker/build.sh 'cd /src && sh rootfs/build-sdlspy.sh'
 * Use:    LD_PRELOAD=/root/doom/sdlspy.so prboom ...
 */
#define _GNU_SOURCE
#include <SDL/SDL.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *out;

static void spy_open(void)
{
	if (!out) {
		const char *p = getenv("SDLSPY_LOG");

		out = fopen(p ? p : "/root/doom/sdlspy.log", "w");
		if (!out)
			out = stderr;
		setvbuf(out, NULL, _IONBF, 0);
	}
}

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
	static SDL_Surface *(*real)(int, int, int, Uint32);
	SDL_Surface *s;

	if (!real)
		real = dlsym(RTLD_NEXT, "SDL_SetVideoMode");
	s = real(w, h, bpp, flags);
	spy_open();
	fprintf(out, "SPY SetVideoMode(%d,%d,%d,0x%x) -> %p\n",
		w, h, bpp, (unsigned)flags, (void *)s);
	if (s) {
		int mustlock = SDL_MUSTLOCK(s);

		fprintf(out, "SPY   returned flags=0x%08x%s%s%s%s\n",
			(unsigned)s->flags,
			(s->flags & SDL_HWSURFACE) ? " HWSURFACE" : " SWSURFACE",
			(s->flags & SDL_DOUBLEBUF) ? " DOUBLEBUF" : "",
			(s->flags & SDL_HWPALETTE) ? " HWPALETTE" : "",
			(s->flags & SDL_FULLSCREEN) ? " FULLSCREEN" : "");
		fprintf(out, "SPY   %dx%d pitch=%d bpp=%d masks=%08x/%08x/%08x offset=%d\n",
			s->w, s->h, s->pitch, s->format->BitsPerPixel,
			(unsigned)s->format->Rmask, (unsigned)s->format->Gmask,
			(unsigned)s->format->Bmask, s->offset);
		fprintf(out, "SPY   pixels=%p  SDL_MUSTLOCK=%d  (prboom caches this pointer iff MUSTLOCK==0)\n",
			s->pixels, mustlock);
	}
	return s;
}

int SDL_Flip(SDL_Surface *s)
{
	static int (*real)(SDL_Surface *);
	static void *last_pixels;
	static int n, moved;
	int rc;

	if (!real)
		real = dlsym(RTLD_NEXT, "SDL_Flip");
	spy_open();

	if (s && s->pixels) {
		long bytes = (long)s->h * s->pitch;
		int show = (n < 12 || (n % 20) == 0);

		if (last_pixels && s->pixels != last_pixels)
			moved++;
		if (show) {
			/*
			 * Count non-zero bytes ONLY when reporting. This scan
			 * walks the whole framebuffer a byte at a time, and on
			 * a board with 88 MB/s of memory bandwidth doing it on
			 * every flip costs more than the frame does - it would
			 * be measuring the instrument, not Doom.
			 */
			const unsigned char *p = s->pixels;
			long i, nz = 0;

			for (i = 0; i < bytes; i++)
				if (p[i]) nz++;
			fprintf(out, "SPY flip %-5d pixels=%p %s nonzero=%ld/%ld\n",
				n, s->pixels,
				(last_pixels && s->pixels != last_pixels) ? "MOVED" : "same ",
				nz, bytes);
		}
		if (n == 400)
			fprintf(out, "SPY  after %d flips, pixels moved %d times\n", n, moved);
		last_pixels = s->pixels;
		n++;
	}
	rc = real(s);
	return rc;
}

/*
 * prboom's ONLY present call is SDL_Flip (i_video.c:410), yet the shim sees
 * ~31 fps of 320x200 PutImage while SDL_Flip is never intercepted. So some
 * other path is pushing the framebuffer at X. Wrap the two SDL surface-update
 * entry points and XPutImage itself, and record who calls it: the return
 * address lands in whichever library actually drives the repaint.
 */
void SDL_UpdateRect(SDL_Surface *s, Sint32 x, Sint32 y, Uint32 w, Uint32 h)
{
	static void (*real)(SDL_Surface *, Sint32, Sint32, Uint32, Uint32);
	static int n;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_UpdateRect");
	spy_open();
	if (n < 5) fprintf(out, "SPY UpdateRect #%d %ux%u+%d+%d\n", n, (unsigned)w, (unsigned)h, (int)x, (int)y);
	n++;
	real(s, x, y, w, h);
}

void SDL_UpdateRects(SDL_Surface *s, int n_rects, SDL_Rect *rects)
{
	static void (*real)(SDL_Surface *, int, SDL_Rect *);
	static int n;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_UpdateRects");
	spy_open();
	if (n < 5) fprintf(out, "SPY UpdateRects #%d n=%d\n", n, n_rects);
	n++;
	real(s, n_rects, rects);
}

int XPutImage(Display *d, Drawable dr, GC gc, XImage *im,
	      int sx, int sy, int dx, int dy, unsigned int w, unsigned int h)
{
	static int (*real)(Display *, Drawable, GC, XImage *, int, int, int, int,
			   unsigned int, unsigned int);
	static int n;
	void *ra = __builtin_return_address(0);

	if (!real) real = dlsym(RTLD_NEXT, "XPutImage");
	spy_open();
	if (n < 6 || (n % 500) == 0) {
		Dl_info info;
		long nz = 0, bytes = (long)h * (im ? im->bytes_per_line : 0), i;
		const unsigned char *p = im ? (const unsigned char *)im->data : NULL;

		for (i = 0; p && i < bytes; i++)
			if (p[i]) nz++;
		if (dladdr(ra, &info) && info.dli_fname)
			fprintf(out, "SPY XPutImage #%d %ux%u nonzero=%ld/%ld caller=%s+%p\n",
				n, w, h, nz, bytes, info.dli_fname,
				(void *)((char *)ra - (char *)info.dli_fbase));
		else
			fprintf(out, "SPY XPutImage #%d %ux%u nonzero=%ld/%ld caller=%p\n",
				n, w, h, nz, bytes, ra);
	}
	n++;
	return real(d, dr, gc, im, sx, sy, dx, dy, w, h);
}

/*
 * prboom's whole tic loop hangs off this one function:
 *
 *   I_GetTime_RealTime()  ->  int t = SDL_GetTicks();
 *
 * and TryRunTics() spins calling NetUpdate() - which pumps X events, hence
 * the recvmsg/poll burn - until I_GetTime() advances. If SDL_GetTicks never
 * moves, prboom never gets a tic, never calls D_Display, never calls
 * SDL_Flip, and presents nothing while looking busy. SDL2's clock was
 * measured good (sdl2probe: 300 ms in 300 ms); SDL 1.2's is a DIFFERENT
 * implementation and had never been checked.
 */
Uint32 SDL_GetTicks(void)
{
	static Uint32 (*real)(void);
	static long n;
	static Uint32 first, last;
	Uint32 t;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_GetTicks");
	t = real();
	if (n == 0) { first = t; spy_open(); }
	if ((n % 50000) == 0)
		fprintf(out, "SPY GetTicks call %-8ld -> %u  (advanced %u ms since first)\n",
			n, (unsigned)t, (unsigned)(t - first));
	last = t;
	(void)last;
	n++;
	return t;
}

void SDL_Delay(Uint32 ms)
{
	static void (*real)(Uint32);
	static long n;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_Delay");
	if ((n % 2000) == 0) { spy_open(); fprintf(out, "SPY Delay #%ld %u ms\n", n, (unsigned)ms); }
	n++;
	real(ms);
}

/*
 * Stuck inside one call, or looping around it?
 *
 * prboom's D_DoomLoop calls I_GetTime() (-> SDL_GetTicks) on every pass, and
 * SDL_GetTicks was called EXACTLY ONCE in 100 s. So prboom is not looping -
 * it entered something and never came back. Log entry and exit separately:
 * an "enter" with no matching "exit" names the call it died in.
 */
int SDL_PollEvent(SDL_Event *ev)
{
	static int (*real)(SDL_Event *);
	static long n;
	int rc;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_PollEvent");
	spy_open();
	if (n < 3 || (n % 20000) == 0)
		fprintf(out, "SPY PollEvent enter #%ld\n", n);
	n++;
	rc = real(ev);
	if (n <= 3 || (n % 20000) == 1)
		fprintf(out, "SPY PollEvent exit  #%ld rc=%d\n", n - 1, rc);
	return rc;
}

void SDL_PumpEvents(void)
{
	static void (*real)(void);
	static long n;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_PumpEvents");
	spy_open();
	if (n < 3 || (n % 20000) == 0)
		fprintf(out, "SPY PumpEvents enter #%ld\n", n);
	n++;
	real();
	if (n <= 3 || (n % 20000) == 1)
		fprintf(out, "SPY PumpEvents exit  #%ld\n", n - 1);
}

int SDL_WaitEvent(SDL_Event *ev)
{
	static int (*real)(SDL_Event *);
	static long n;
	int rc;

	if (!real) real = dlsym(RTLD_NEXT, "SDL_WaitEvent");
	spy_open();
	fprintf(out, "SPY WaitEvent enter #%ld  <-- BLOCKS until an event arrives\n", n);
	n++;
	rc = real(ev);
	fprintf(out, "SPY WaitEvent exit  #%ld rc=%d\n", n - 1, rc);
	return rc;
}
