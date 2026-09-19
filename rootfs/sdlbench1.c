#include "sdlbench-common.h"
#include <SDL/SDL.h>
#include "sdlbench-runtime.h"
static SDL_Surface *screen,*indexed,*rgb16,*rgb32,*sprite;
static SDL_Color palette[256];
static Uint32 colors[256];
static int depth=8;
static Uint8 *source_frames;
static int sprites=64;
static void pattern(SDL_Surface *s) {
    SDL_LockSurface(s);
    for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++)
        ((Uint8*)s->pixels)[y*s->pitch+x]=(x*7+y*13)&255;
    SDL_UnlockSurface(s);
}
static int convert16(int i) { (void)i; return SDL_BlitSurface(indexed,NULL,rgb16,NULL); }
static int convert32(int i) { (void)i; return SDL_BlitSurface(indexed,NULL,rgb32,NULL); }
static int fill(int i) { return SDL_FillRect(rgb16,NULL,colors[i&255]); }
static int copy(int i) { (void)i; return SDL_BlitSurface(rgb16,NULL,screen,NULL); }
static int alpha(int i) { SDL_Rect r={i%(width-32),i%(height-32),32,32}; return SDL_BlitSurface(sprite,NULL,rgb16,&r); }
static int full(int i) {
    if(SDL_FillRect(screen,NULL,depth==8?(i&255):colors[i&255])) return -1;
    SDL_UpdateRect(screen,0,0,0,0); return pump(i);
}
static int dirty(int i) {
    SDL_Rect r={(i*7)%(width-32),(i*11)%(height-32),32,32};
    if(SDL_FillRect(screen,&r,depth==8?(i&255):colors[i&255])) return -1;
    SDL_UpdateRects(screen,1,&r); return pump(i);
}
static int flip(int i) {
    if(SDL_FillRect(screen,NULL,depth==8?(i&255):colors[i&255])) return -1;
    if(SDL_Flip(screen)) return -1;
    return pump(i);
}
static int palette_only(int i) {
    SDL_Color c={(Uint8)i,(Uint8)(255-i),(Uint8)(i*3),0};
    SDL_SetPalette(screen,SDL_LOGPAL|SDL_PHYSPAL,&c,i&255,1);
    /* Deliberately no UpdateRect: tests palette-only invalidation contract. */
    return pump(i);
}
static int buffer_copy(int i) {
    for(int y=0;y<height;y++) memcpy((Uint8*)indexed->pixels+y*indexed->pitch,source_frames+((i&1)*height+y)*width,width);
    return 0;
}
static int indexed_frame(int i) {
    uint64_t a=now_ns(); buffer_copy(i); uint64_t b=now_ns();
    if(SDL_BlitSurface(indexed,NULL,screen,NULL)) return -1;
    uint64_t c=now_ns(); SDL_UpdateRect(screen,0,0,0,0); uint64_t d=now_ns();
    int rc=pump(i); uint64_t e=now_ns();
    stage_add(0,"source_copy",b-a); stage_add(1,"surface_blit",c-b);
    stage_add(2,"update_return",d-c); stage_add(3,"event_pump",e-d);
    return rc;
}
/* Synthetic indexed renderer, NOT Tyrian code: scrolling plus transparent
 * 16x16 sprites exercises CPU work outside SDL as well as presentation. */
static int sprite_work(int i) {
    buffer_copy(i);
    Uint8 *p=indexed->pixels;
    for(int n=0;n<sprites;n++) {
        int dx=(n*37+i*3)%(width-16),dy=(n*23+i*5)%(height-16);
        for(int y=0;y<16;y++) for(int x=0;x<16;x++) {
            unsigned c=(x*7+y*11+n)&31;
            if(c>7) p[(dy+y)*indexed->pitch+dx+x]=(c+n+i)&255;
        }
    }
    return 0;
}
static int sprite_frame(int i) {
    uint64_t a=now_ns(); sprite_work(i); uint64_t b=now_ns();
    if(SDL_BlitSurface(indexed,NULL,screen,NULL)) return -1;
    uint64_t c=now_ns(); if(SDL_Flip(screen)) return -1; uint64_t d=now_ns();
    int rc=pump(i); uint64_t e=now_ns();
    stage_add(0,"synthetic_sprite_cpu",b-a); stage_add(1,"surface_blit",c-b);
    stage_add(2,"flip_return",d-c); stage_add(3,"event_pump",e-d);
    return rc;
}
/* Models the unmodified application's own palette-to-display loop. This is
 * deliberately distinct from SDL_BlitSurface, which a shim could intercept. */
static int application_expand(int i) {
    (void)i;
    if(SDL_LockSurface(screen)) return -1;
    for(int y=0;y<height;y++) {
        const Uint8 *src=(Uint8*)indexed->pixels+y*indexed->pitch;
        Uint8 *dst=(Uint8*)screen->pixels+y*screen->pitch;
        if(screen->format->BytesPerPixel==1) memcpy(dst,src,width);
        else if(screen->format->BytesPerPixel==2)
            for(int x=0;x<width;x++) ((Uint16*)dst)[x]=colors[src[x]];
        else if(screen->format->BytesPerPixel==4)
            for(int x=0;x<width;x++) ((Uint32*)dst)[x]=colors[src[x]];
        else { SDL_UnlockSurface(screen); return -1; }
    }
    SDL_UnlockSurface(screen); return 0;
}
static int tyrian_frame(int i) {
    uint64_t a=now_ns(); sprite_work(i); uint64_t b=now_ns();
    if(application_expand(i)) return -1;
    uint64_t c=now_ns(); if(SDL_Flip(screen)) return -1; uint64_t d=now_ns();
    int rc=pump(i); uint64_t e=now_ns();
    stage_add(0,"synthetic_sprite_cpu",b-a); stage_add(1,"application_palette_loop",c-b);
    stage_add(2,"flip_return",d-c); stage_add(3,"event_pump",e-d);
    return rc;
}
/* A Doom-style direct video-surface producer. Synthetic writes are timed
 * separately; no intermediate SDL_BlitSurface is charged to presentation. */
static int doom_frame(int i) {
    uint64_t a=now_ns();
    if(SDL_LockSurface(screen)) return -1;
    for(int y=0;y<height;y++) {
        const Uint8 *src=source_frames+((i&1)*height+y)*width;
        Uint8 *dst=(Uint8*)screen->pixels+y*screen->pitch;
        if(screen->format->BytesPerPixel==1) memcpy(dst,src,width);
        else if(screen->format->BytesPerPixel==2)
            for(int x=0;x<width;x++) ((Uint16*)dst)[x]=colors[src[x]];
        else if(screen->format->BytesPerPixel==4)
            for(int x=0;x<width;x++) ((Uint32*)dst)[x]=colors[src[x]];
        else { SDL_UnlockSurface(screen); return -1; }
    }
    SDL_UnlockSurface(screen);
    uint64_t b=now_ns(); if(SDL_Flip(screen)) return -1; uint64_t c=now_ns();
    int rc=pump(i); uint64_t d=now_ns();
    stage_add(0,"synthetic_frame_write",b-a); stage_add(1,"flip_return",c-b);
    stage_add(2,"event_pump",d-c); return rc;
}
int main(int argc,char **argv) {
    const char *d=getenv("SDLBENCH_DEPTH"); if(d) depth=atoi(d);
    if(depth!=8 && depth!=16 && depth!=32) return 2;
    parse_args(argc,argv); metadata(1); futex_probe();
    if(SDL_Init(SDL_INIT_VIDEO|(timers_enabled?SDL_INIT_TIMER:0))) { error_record("init",SDL_GetError()); return 1; }
    const SDL_version *v=SDL_Linked_Version(); char drv[64]; SDL_VideoDriverName(drv,sizeof drv);
    printf("{\"type\":\"sdl_version\",\"version\":\"%d.%d.%d\",\"driver\":\"%s\"}\n",v->major,v->minor,v->patch,drv);
    screen=SDL_SetVideoMode(width,height,depth,SDL_SWSURFACE|(fullscreen?SDL_FULLSCREEN:0));
    if(!screen) { error_record("video_mode",SDL_GetError()); return 1; }
    printf("{\"type\":\"surface\",\"requested_depth\":%d,\"width\":%d,\"height\":%d,\"bpp\":%u,\"pitch\":%u,\"flags\":%u}\n",depth,screen->w,screen->h,screen->format->BitsPerPixel,screen->pitch,screen->flags);
    width=screen->w; height=screen->h;
    indexed=SDL_CreateRGBSurface(SDL_SWSURFACE,width,height,8,0,0,0,0);
    rgb16=SDL_CreateRGBSurface(SDL_SWSURFACE,width,height,16,0xf800,0x07e0,0x001f,0);
    rgb32=SDL_CreateRGBSurface(SDL_SWSURFACE,width,height,32,0xff0000,0xff00,0xff,0);
    sprite=SDL_CreateRGBSurface(SDL_SWSURFACE,32,32,32,0xff0000,0xff00,0xff,0xff000000);
    if(!indexed||!rgb16||!rgb32||!sprite) { error_record("surface_alloc",SDL_GetError()); return 1; }
    for(int i=0;i<256;i++) {
        palette[i]=(SDL_Color){i,255-i,(i*3)&255,0};
        colors[i]=SDL_MapRGB(screen->format,i,255-i,(i*3)&255);
    }
    SDL_SetColors(indexed,palette,0,256);
    if(depth==8) SDL_SetPalette(screen,SDL_LOGPAL|SDL_PHYSPAL,palette,0,256);
    pattern(indexed);
    source_frames=malloc(width*height*2);
    if(!source_frames) return 1;
    for(int f=0;f<2;f++) for(int y=0;y<height;y++) for(int x=0;x<width;x++)
        source_frames[(f*height+y)*width+x]=(x*7+y*13+f*53)&255;
    const char *sp=getenv("SDLBENCH_SPRITES"); if(sp) sprites=atoi(sp);
    if(sprites<0||sprites>512) return 2;
    printf("{\"type\":\"profile\",\"sprites\":%d,\"sprite_size\":16,\"source\":\"synthetic_not_game_code\"}\n",sprites);
    convert16(0); convert32(0);
    Uint16 p16; Uint32 p32; memcpy(&p16,rgb16->pixels,2); memcpy(&p32,rgb32->pixels,4);
    check_record("indexed_to_rgb565",p16==SDL_MapRGB(rgb16->format,0,255,0));
    check_record("indexed_to_xrgb8888",p32==SDL_MapRGB(rgb32->format,0,255,0));
    SDL_FillRect(sprite,NULL,SDL_MapRGBA(sprite->format,255,0,0,128));
    SDL_SetAlpha(sprite,SDL_SRCALPHA,128);
    start_audio(); thread_probe(); ready();
    bench("idle_wait","sleep_20ms",idle_wait);
    bench("timer_overhead","cpu",timer_only);
    bench("event_pump","api_return",pump);
    bench("index8_to_rgb565","cpu",convert16);
    bench("index8_to_xrgb8888","cpu",convert32);
    bench("fill_rgb565","cpu",fill);
    bench("blit_to_screen","cpu_no_present",copy);
    bench("alpha32_sprite","cpu",alpha);
    SDL_SetAlpha(sprite,0,255); SDL_SetColorKey(sprite,SDL_SRCCOLORKEY,SDL_MapRGB(sprite->format,0,0,0));
    bench("colorkey_sprite","cpu",alpha);
    bench("present_full","submit_and_pump_not_scanout",full);
    bench("present_dirty32","submit_and_pump_not_scanout",dirty);
    bench("flip_full","submit_and_pump_not_scanout",flip);
    bench("doom_frame","direct_surface_write_flip_pump_not_scanout",doom_frame);
    bench("application_expand", "application_cpu",application_expand);
    bench("tyrian_frame", "synthetic_cpu_application_expand_flip_pump_not_scanout",tyrian_frame);
    bench("buffer_copy", "cpu",buffer_copy);
    bench("sprite_cpu", "synthetic_cpu",sprite_work);
    bench("sprite_frame", "synthetic_cpu_convert_flip_pump_not_scanout",sprite_frame);
    bench("indexed_frame","convert_submit_pump_not_scanout",indexed_frame);
    if(depth==8) { SDL_BlitSurface(indexed,NULL,screen,NULL); SDL_UpdateRect(screen,0,0,0,0); bench("palette_only","submit_and_pump_not_scanout",palette_only); }
    hold_picture(); SDL_Delay(200);
    free(source_frames); SDL_FreeSurface(sprite); SDL_FreeSurface(rgb32); SDL_FreeSurface(rgb16); SDL_FreeSurface(indexed);
    finish(); return failures?1:0;
}
