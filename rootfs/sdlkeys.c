/* What SDL 1.2 reports for keys, before and after it grabs input and hides
 * the cursor - the state a game is in. prboom stopped reacting to keys once
 * grabbed while libX11 was demonstrably decoding them. */
#include <SDL/SDL.h>
#include <stdio.h>
#include <time.h>
int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20, grab_at = argc > 2 ? atoi(argv[2]) : 6;
	SDL_Surface *s; SDL_Event e; int grabbed = 0; Uint32 t0;
	if (SDL_Init(SDL_INIT_VIDEO) < 0) { printf("init: %s\n", SDL_GetError()); return 1; }
	s = SDL_SetVideoMode(320, 200, 8, SDL_HWPALETTE);
	if (!s) { printf("mode: %s\n", SDL_GetError()); return 1; }
	SDL_WM_SetCaption("sdlkeys", "sdlkeys");
	t0 = SDL_GetTicks();
	while ((int)(SDL_GetTicks() - t0) < secs * 1000) {
		if (!grabbed && (int)(SDL_GetTicks() - t0) > grab_at * 1000) {
			SDL_ShowCursor(SDL_DISABLE);
			SDL_WM_GrabInput(SDL_GRAB_ON);
			grabbed = 1;
			printf("t=%u GRAB on (state %x)\n", SDL_GetTicks() - t0, SDL_GetAppState());
			fflush(stdout);
		}
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
				printf("t=%u key %s sym=%d scancode=%u\n", SDL_GetTicks() - t0,
				       e.type == SDL_KEYDOWN ? "down" : "up  ", e.key.keysym.sym, e.key.keysym.scancode);
			else if (e.type == SDL_MOUSEMOTION)
				printf("t=%u motion xrel=%d yrel=%d (%d,%d)\n", SDL_GetTicks() - t0, e.motion.xrel, e.motion.yrel, e.motion.x, e.motion.y);
			else if (e.type == SDL_ACTIVEEVENT)
				printf("t=%u active gain=%d state=%x\n", SDL_GetTicks() - t0, e.active.gain, e.active.state);
			fflush(stdout);
		}
		SDL_Delay(10);
	}
	printf("done\n");
	SDL_Quit();
	return 0;
}
