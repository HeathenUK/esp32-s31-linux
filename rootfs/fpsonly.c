/* Frame counter for SDL 1.2, and NOTHING else.
 *
 * rootfs/sdlspy.c wraps SDL_GetTicks, SDL_PollEvent and SDL_Delay, all of
 * which Doom calls many times per frame; measuring frame rate through it
 * measures the instrument as much as the game. This wraps exactly one
 * function, once per presented frame, and writes a count and a monotonic
 * timestamp so the rate can be computed from any two samples.
 *
 * Build: ./docker/build.sh 'cd /src && sh rootfs/build-fpsonly.sh'
 * Use:   LD_PRELOAD=/root/doom/fpsonly.so prboom ...
 */
#define _GNU_SOURCE
#include <SDL/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int SDL_Flip(SDL_Surface *s)
{
	static int (*real)(SDL_Surface *);
	static FILE *out;
	static long n;
	struct timespec ts;

	if (!real) {
		real = dlsym(RTLD_NEXT, "SDL_Flip");
		out = fopen(getenv("FPS_LOG") ? getenv("FPS_LOG")
					      : "/root/doom/fps.txt", "w");
		if (out)
			setvbuf(out, NULL, _IONBF, 0);
	}
	n++;
	if (out && (n % 10) == 0) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		fprintf(out, "%ld %ld.%03ld\n", n, (long)ts.tv_sec,
			ts.tv_nsec / 1000000);
	}
	return real(s);
}
