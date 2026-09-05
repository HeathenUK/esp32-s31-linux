/*
 * Minimal SDL 1.2 client: what does SDL think our display is, and can it put a
 * known colour on the screen?
 *
 * Written because prboom sent perfectly formed PutImage requests in which every
 * byte was zero. That is what an SDL surface whose colour masks are zero
 * produces: SDL_MapRGB returns 0 for every colour and the client renders a
 * correct all-black frame. This separates "SDL is misconfigured" from "the game
 * is at fault" without a 4 MB WAD in the way, and is the thing to reach for
 * with any future SDL program on this board.
 */
#include <SDL/SDL.h>
#include <stdio.h>

int main(void)
{
	SDL_Surface *s;
	const SDL_VideoInfo *vi;
	char drv[32] = "?";
	int i;

	/* Unbuffered: SDL_Quit() can take the process down a path that
	 * never flushes stdout, and a lost diagnostic looks like a
	 * crash. */
	setvbuf(stdout, NULL, _IONBF, 0);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("SDLT init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_VideoDriverName(drv, sizeof drv);
	vi = SDL_GetVideoInfo();
	printf("SDLT driver=%s\n", drv);
	if (vi && vi->vfmt)
		printf("SDLT desktop bpp=%d R=%08x G=%08x B=%08x\n",
		       vi->vfmt->BitsPerPixel, (unsigned)vi->vfmt->Rmask,
		       (unsigned)vi->vfmt->Gmask, (unsigned)vi->vfmt->Bmask);

	s = SDL_SetVideoMode(320, 200, 16, SDL_SWSURFACE);
	if (!s) {
		printf("SDLT SetVideoMode failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	printf("SDLT surface %dx%d bpp=%d pitch=%d R=%08x G=%08x B=%08x\n",
	       s->w, s->h, s->format->BitsPerPixel, s->pitch,
	       (unsigned)s->format->Rmask, (unsigned)s->format->Gmask,
	       (unsigned)s->format->Bmask);
	printf("SDLT MapRGB(255,0,0)=%08x (0 means every colour is black)\n",
	       (unsigned)SDL_MapRGB(s->format, 255, 0, 0));

	/* Three visible bands, so a photo of the panel is unambiguous. */
	for (i = 0; i < 12; i++) {
		SDL_Rect r = { 0, 0, 320, 66 };
		SDL_FillRect(s, &r, SDL_MapRGB(s->format, 255, 0, 0));
		r.y = 66;  SDL_FillRect(s, &r, SDL_MapRGB(s->format, 0, 255, 0));
		r.y = 132; SDL_FillRect(s, &r, SDL_MapRGB(s->format, 0, 0, 255));
		SDL_UpdateRect(s, 0, 0, 0, 0);
		SDL_Delay(500);
	}
	SDL_Quit();
	return 0;
}
