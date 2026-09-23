/*
 * sdl2keys - what does an SDL2 application see from the keyboard here?
 *
 * The SDL2 twin of sdlkeys.c. SDL2 does not decode keys per event like SDL 1.2
 * (XLookupString); it builds a keycode -> scancode table ONCE at init from
 * XGetKeyboardMapping / XkbKeycodeToKeysym and then indexes it by the wire
 * keycode of every KeyPress. xlite's wire keycodes are Latin-1 characters, so
 * whether that table comes out right is exactly what Chocolate Doom (SDL2)
 * depends on and prboom (SDL 1.2) does not. Prints, per event:
 *   t=<s> DOWN/UP scancode=<n> (<scancode name>) key=<sym> (<key name>) mod=<hex>
 * Run it with `uinject keytest` from the host; exit on Escape or after 20 s.
 */
#include <SDL2/SDL.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	SDL_Window *w;
	Uint32 t0;
	int secs = argc > 1 ? atoi(argv[1]) : 20;

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	w = SDL_CreateWindow("sdl2keys", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			     320, 200, 0);
	if (!w) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	SDL_ShowCursor(SDL_DISABLE);
	SDL_SetWindowGrab(w, SDL_TRUE);
	t0 = SDL_GetTicks();
	printf("sdl2keys: ready (SDL %d.%d.%d), %d s\n", SDL_MAJOR_VERSION,
	       SDL_MINOR_VERSION, SDL_PATCHLEVEL, secs);
	fflush(stdout);
	while (SDL_GetTicks() - t0 < (Uint32)secs * 1000) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
				printf("t=%.2f %s scancode=%d (%s) key=%d (%s) mod=0x%x%s\n",
				       (SDL_GetTicks() - t0) / 1000.0,
				       e.type == SDL_KEYDOWN ? "DOWN" : "UP  ",
				       e.key.keysym.scancode,
				       SDL_GetScancodeName(e.key.keysym.scancode),
				       e.key.keysym.sym,
				       SDL_GetKeyName(e.key.keysym.sym),
				       e.key.keysym.mod,
				       e.key.repeat ? " repeat" : "");
				fflush(stdout);
				if (e.key.keysym.sym == SDLK_ESCAPE)
					goto out;
			} else if (e.type == SDL_TEXTINPUT) {
				printf("t=%.2f TEXT '%s'\n", (SDL_GetTicks() - t0) / 1000.0, e.text.text);
				fflush(stdout);
			} else if (e.type == SDL_MOUSEMOTION) {
				printf("t=%.2f MOTION x=%d y=%d rel=%d,%d\n", (SDL_GetTicks() - t0) / 1000.0,
				       e.motion.x, e.motion.y, e.motion.xrel, e.motion.yrel);
				fflush(stdout);
			}
		}
		SDL_Delay(5);
	}
out:
	SDL_Quit();
	return 0;
}
