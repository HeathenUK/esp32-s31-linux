/*
 * sdlkeys - what does an SDL 1.2 client actually receive?
 *
 * Opens a 320x200 window exactly as prboom does (8 bpp, software surface,
 * input grabbed) and logs every key event with its SDL symbol for <secs>
 * seconds. The question it answers is "did Ctrl/Alt/Space reach the GAME",
 * which neither keylog (blind under lvdesk's EVIOCGRAB) nor lvdesk's own log
 * (it records drops, not deliveries) can. Build:
 *   ./docker/build.sh 'cd /src && sh rootfs/build-sdlkeys.sh'
 * Use:  sdlkeys [secs] [grab 0|1]  > /root/sdlkeys.txt
 */
#include <SDL/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	int grab = argc > 2 ? atoi(argv[2]) : 1;
	Uint32 end;
	SDL_Event e;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (SDL_Init(SDL_INIT_VIDEO) < 0 ||
	    !SDL_SetVideoMode(320, 200, 8, SDL_SWSURFACE)) {
		printf("sdlkeys: %s\n", SDL_GetError());
		return 1;
	}
	SDL_WM_SetCaption("sdlkeys", NULL);
	SDL_ShowCursor(0);
	if (grab)
		SDL_WM_GrabInput(SDL_GRAB_ON);
	printf("sdlkeys: ready (grab %d)\n", grab);
	end = SDL_GetTicks() + (Uint32)secs * 1000;
	while (SDL_GetTicks() < end) {
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
				printf("%s sym=%d (%s) scancode=%d mod=0x%x\n",
				       e.type == SDL_KEYDOWN ? "DOWN" : "UP  ",
				       e.key.keysym.sym,
				       SDL_GetKeyName(e.key.keysym.sym),
				       e.key.keysym.scancode, e.key.keysym.mod);
			else if (e.type == SDL_ACTIVEEVENT)
				printf("ACTIVE gain=%d state=0x%x\n",
				       e.active.gain, e.active.state);
		}
		SDL_Delay(10);
	}
	SDL_Quit();
	return 0;
}
