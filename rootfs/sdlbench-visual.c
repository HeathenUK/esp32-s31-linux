/* A separate visual oracle: three primary-color bands, white grid, yellow
 * moving square. Capture this OUTSIDE performance runs. Stock SDL only. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    int seconds=argc>1?atoi(argv[1]):60;
    int depth=argc>2?atoi(argv[2]):8;
    if(seconds<1||seconds>120) return 2;
    if(depth!=8&&depth!=16&&depth!=32) return 2;
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)) { puts(SDL_GetError()); return 1; }
#if SDL_MAJOR_VERSION == 2
    SDL_Window *w=SDL_CreateWindow("SDL2 visual oracle",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,320,200,0);
    if(!w) return 1;
    SDL_Renderer *r=SDL_CreateRenderer(w,-1,SDL_RENDERER_SOFTWARE);
    if(!r) return 1;
    SDL_Texture *t=SDL_CreateTexture(r,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,320,200);
    SDL_Surface *s=SDL_CreateRGBSurfaceWithFormat(0,320,200,8,SDL_PIXELFORMAT_INDEX8);
    SDL_Surface *locked=SDL_CreateRGBSurfaceWithFormat(0,320,200,32,SDL_PIXELFORMAT_ARGB8888);
    if(!t||!s||!locked) return 1;
    SDL_SetTextureBlendMode(t,SDL_BLENDMODE_NONE);
#else
    SDL_Surface *s=SDL_SetVideoMode(320,200,depth,SDL_SWSURFACE);
    if(!s) return 1;
    SDL_WM_SetCaption("SDL1 visual oracle",NULL);
#endif
    if(s->format->BitsPerPixel==8) {
        SDL_Color colors[6]={{0,0,0,255},{255,0,0,255},{0,255,0,255},
                             {0,0,255,255},{255,255,255,255},{255,255,0,255}};
#if SDL_MAJOR_VERSION == 2
        SDL_SetPaletteColors(s->format->palette,colors,0,6);
#else
        SDL_SetPalette(s,SDL_LOGPAL|SDL_PHYSPAL,colors,0,6);
#endif
    }
    for(int n=0;n<seconds*5;n++) {
        for(int band=0;band<3;band++) {
            SDL_Rect q={band*106,0,band==2?108:106,200};
            Uint8 red=band==0?255:0,green=band==1?255:0,blue=band==2?255:0;
            SDL_FillRect(s,&q,SDL_MapRGB(s->format,red,green,blue));
        }
        for(int y=0;y<200;y+=32) {
            SDL_Rect q={0,y,320,2};
            SDL_FillRect(s,&q,SDL_MapRGB(s->format,255,255,255));
        }
        SDL_Rect q={(n*13)%288,82,32,32};
        SDL_FillRect(s,&q,SDL_MapRGB(s->format,255,255,0));
#if SDL_MAJOR_VERSION == 2
        void *ptr; int pitch;
        if(SDL_LockTexture(t,NULL,&ptr,&pitch)) return 1;
        void *saved=locked->pixels; int saved_pitch=locked->pitch;
        locked->pixels=ptr; locked->pitch=pitch;
        int rc=SDL_BlitSurface(s,NULL,locked,NULL);
        locked->pixels=saved; locked->pitch=saved_pitch;
        SDL_UnlockTexture(t);
        if(rc) return 1;
        SDL_SetRenderDrawColor(r,0,0,0,255); SDL_RenderClear(r);
        if(SDL_RenderCopy(r,t,NULL,NULL)) return 1;
        SDL_RenderPresent(r);
#else
        if(SDL_Flip(s)) return 1;
#endif
        SDL_Event e; while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT) goto done;
        SDL_Delay(200);
    }
done:
#if SDL_MAJOR_VERSION == 2
    SDL_FreeSurface(locked); SDL_FreeSurface(s); SDL_DestroyTexture(t);
    SDL_DestroyRenderer(r); SDL_DestroyWindow(w);
#endif
    SDL_Quit(); puts("visual oracle completed"); return 0;
}
