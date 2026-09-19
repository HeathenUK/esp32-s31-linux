#include "sdlbench-common.h"
#include <SDL.h>
#include "sdlbench-runtime.h"
static SDL_Window *win;
static SDL_Renderer *renderer;
static SDL_Texture *texture,*target,*yuv;
static SDL_Surface *indexed,*argb,*window_surface;
static Uint32 *pixels;
static Uint8 *planes,*source_frames;
static int tw=320,th=200;
static SDL_Surface *lock_surface;
static int convert(int i) { (void)i; return SDL_BlitSurface(indexed,NULL,argb,NULL); }
static int lock_upload(int i) {
    void *p; int pitch;
    if(SDL_LockTexture(texture,NULL,&p,&pitch)) return -1;
    for(int y=0;y<th;y++) memcpy((char*)p+y*pitch,pixels+y*tw,tw*4);
    ((Uint32*)p)[0]=0xff000000|((i*797)&0xffffff);
    SDL_UnlockTexture(texture); return 0;
}
static int index_lock(int i) {
    void *p; int pitch;
    if(SDL_LockTexture(texture,NULL,&p,&pitch)) return -1;
    /* Stack description, matching a normal SDL surface blit into locked pixels. */
    SDL_Surface *s=lock_surface;
    s->pixels=p; s->pitch=pitch;
    for(int y=0;y<th;y++) memcpy((Uint8*)indexed->pixels+y*indexed->pitch,source_frames+((i&1)*th+y)*tw,tw);
    int rc=SDL_BlitSurface(indexed,NULL,s,NULL);
    SDL_UnlockTexture(texture); return rc;
}
static int update(int i) { pixels[0]=0xff000000|((i*797)&0xffffff); return SDL_UpdateTexture(texture,NULL,pixels,tw*4); }
static int clear(int i) {
    if(SDL_SetRenderDrawColor(renderer,i&255,(i*3)&255,90,255)) return -1;
    if(SDL_RenderClear(renderer)) return -1;
    return SDL_RenderFlush(renderer);
}
static int copy(int i) {
    SDL_Rect d={i&1,0,tw,th}; if(SDL_RenderCopy(renderer,texture,NULL,&d)) return -1;
    return SDL_RenderFlush(renderer);
}
static int scaled(int i) { (void)i; if(SDL_RenderCopy(renderer,texture,NULL,NULL)) return -1;
    return SDL_RenderFlush(renderer); }
static int present(int i) {
    if(clear(i)||scaled(i)) return -1;
    SDL_RenderPresent(renderer); return pump(i);
}
static int frame(int i) {
    uint64_t a=now_ns(); if(index_lock(i)) return -1; uint64_t b=now_ns();
    if(clear(i)||scaled(i)) return -1;
    uint64_t c=now_ns(); SDL_RenderPresent(renderer); uint64_t d=now_ns();
    int rc=pump(i); uint64_t e=now_ns();
    stage_add(0,"source_copy_index_convert_upload",b-a); stage_add(1,"clear_copy_flush",c-b);
    stage_add(2,"present_return",d-c); stage_add(3,"event_pump",e-d);
    return rc;
}
static int two_pass(int i) {
    if(!target) return -1;
    if(index_lock(i) || SDL_SetRenderTarget(renderer,target)) return -1;
    if(SDL_RenderCopy(renderer,texture,NULL,NULL)) return -1;
    if(SDL_SetRenderTarget(renderer,NULL) || clear(i)) return -1;
    if(SDL_RenderCopy(renderer,target,NULL,NULL)) return -1;
    SDL_RenderPresent(renderer); return pump(i);
}
static int surface_frame(int i) {
    for(int y=0;y<th;y++) memcpy((Uint8*)indexed->pixels+y*indexed->pitch,source_frames+((i&1)*th+y)*tw,tw);
    if(SDL_BlitSurface(indexed,NULL,window_surface,NULL)) return -1;
    if(SDL_UpdateWindowSurface(win)) return -1;
    return pump(i);
}
static int surface_dirty(int i) {
    SDL_Rect r={(i*7)%(width-32),(i*11)%(height-32),32,32};
    if(SDL_FillRect(window_surface,&r,SDL_MapRGB(window_surface->format,i&255,128,90))) return -1;
    if(SDL_UpdateWindowSurfaceRects(win,&r,1)) return -1;
    return pump(i);
}
static int yuv_frame(int i) {
    planes[0]=32+(i%180);
    if(SDL_UpdateYUVTexture(yuv,NULL,planes,tw,planes+tw*th,tw/2,planes+tw*th*5/4,tw/2)) return -1;
    if(SDL_RenderCopy(renderer,yuv,NULL,NULL)) return -1;
    SDL_RenderPresent(renderer); return pump(i);
}
int main(int argc,char **argv) {
    parse_args(argc,argv); metadata(2); futex_probe();
    if(SDL_Init(SDL_INIT_VIDEO|(timers_enabled?SDL_INIT_TIMER:0))) { error_record("init",SDL_GetError()); return 1; }
    SDL_version v; SDL_GetVersion(&v);
    printf("{\"type\":\"sdl_version\",\"version\":\"%d.%d.%d\",\"driver\":\"%s\"}\n",v.major,v.minor,v.patch,SDL_GetCurrentVideoDriver());
    for(int n=0;n<SDL_GetNumRenderDrivers();n++) {
        SDL_RendererInfo info;
        if(!SDL_GetRenderDriverInfo(n,&info)) printf("{\"type\":\"renderer_available\",\"name\":\"%s\",\"flags\":%u}\n",info.name,info.flags);
    }
    win=SDL_CreateWindow("SDL2 baseline",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,width,height,fullscreen?SDL_WINDOW_FULLSCREEN:0);
    if(!win) { error_record("window",SDL_GetError()); return 1; }
    SDL_GetWindowSize(win,&width,&height);
    indexed=SDL_CreateRGBSurfaceWithFormat(0,tw,th,8,SDL_PIXELFORMAT_INDEX8);
    argb=SDL_CreateRGBSurfaceWithFormat(0,tw,th,32,SDL_PIXELFORMAT_ARGB8888);
    pixels=malloc(tw*th*4);
    source_frames=malloc(tw*th*2);
    if(!indexed||!argb||!pixels||!source_frames) { error_record("alloc",SDL_GetError()); return 1; }
    SDL_Color pal[256];
    for(int i=0;i<256;i++) pal[i]=(SDL_Color){i,255-i,(i*3)&255,255};
    SDL_SetPaletteColors(indexed->format->palette,pal,0,256);
    for(int y=0;y<th;y++) for(int x=0;x<tw;x++) {
        ((Uint8*)indexed->pixels)[y*indexed->pitch+x]=(x*7+y*13)&255;
        pixels[y*tw+x]=0xff000000|((x*53+y*179)&0xffffff);
    }
    for(int f=0;f<2;f++) for(int y=0;y<th;y++) for(int x=0;x<tw;x++)
        source_frames[(f*th+y)*tw+x]=(x*7+y*13+f*53)&255;
    convert(0); Uint32 p; memcpy(&p,argb->pixels,4);
    check_record("indexed_to_argb8888",p==SDL_MapRGBA(argb->format,0,255,0,255));
    start_audio(); thread_probe(); ready();
    bench("idle_wait","sleep_20ms",idle_wait);
    bench("timer_overhead","cpu",timer_only);
    bench("event_pump","api_return",pump);
    bench("index8_to_argb8888","cpu",convert);
    if(getenv("SDLBENCH_SURFACE")) {
        window_surface=SDL_GetWindowSurface(win);
        if(!window_surface) { error_record("window_surface",SDL_GetError()); return 1; }
        printf("{\"type\":\"surface\",\"width\":%d,\"height\":%d,\"pitch\":%d,\"format\":\"%s\"}\n",window_surface->w,window_surface->h,window_surface->pitch,SDL_GetPixelFormatName(window_surface->format->format));
        bench("surface_frame","convert_submit_pump_not_scanout",surface_frame);
        bench("surface_dirty32","submit_and_pump_not_scanout",surface_dirty);
        hold_picture();
    } else {
        renderer=SDL_CreateRenderer(win,-1,SDL_RENDERER_SOFTWARE|SDL_RENDERER_TARGETTEXTURE);
        if(!renderer) { error_record("renderer",SDL_GetError()); return 1; }
        SDL_RendererInfo info; SDL_GetRendererInfo(renderer,&info);
        int rw,rh; SDL_GetRendererOutputSize(renderer,&rw,&rh);
        printf("{\"type\":\"renderer\",\"name\":\"%s\",\"flags\":%u,\"width\":%d,\"height\":%d}\n",info.name,info.flags,rw,rh);
        texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,tw,th);
        if(!texture) { error_record("texture",SDL_GetError()); return 1; }
        SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
        lock_surface=SDL_CreateRGBSurfaceWithFormatFrom(pixels,tw,th,32,tw*4,SDL_PIXELFORMAT_ARGB8888);
        if(!lock_surface) { error_record("lock_surface",SDL_GetError()); return 1; }
        update(0);
        /* Exact CPU renderer readback, outside all timed loops. */
        SDL_SetRenderDrawColor(renderer,17,83,149,255); SDL_RenderClear(renderer);
        SDL_Rect pixel={0,0,1,1}; Uint32 out=0;
        int rc=SDL_RenderReadPixels(renderer,&pixel,SDL_PIXELFORMAT_ARGB8888,&out,4);
        printf("{\"type\":\"readback\",\"rc\":%d,\"argb\":%u}\n",rc,out);
        /* Software renderer may use RGB565 window storage; allow its
         * quantization, not arbitrary differences or a black image. */
        check_record("renderer_clear_readback",rc==0 && abs((int)((out>>16)&255)-17)<=7 && abs((int)((out>>8)&255)-83)<=3 && abs((int)(out&255)-149)<=7);
        bench("lock_texture_copy","cpu_upload",lock_upload);
        bench("index8_lock_texture","cpu_convert_upload",index_lock);
        bench("update_texture","api_return",update);
        bench("render_clear","api_return",clear);
        bench("render_copy_native","api_return",copy);
        SDL_SetTextureScaleMode(texture,SDL_ScaleModeNearest);
        bench("render_scale_nearest","api_return",scaled);
        SDL_SetTextureScaleMode(texture,SDL_ScaleModeLinear);
        bench("render_scale_linear","api_return",scaled);
        SDL_SetTextureScaleMode(texture,SDL_ScaleModeNearest);
        SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND); SDL_SetTextureAlphaMod(texture,128);
        bench("render_alpha","api_return",scaled);
        SDL_SetTextureColorMod(texture,128,192,64);
        bench("render_color_mod","api_return",scaled);
        SDL_SetTextureColorMod(texture,255,255,255); SDL_SetTextureAlphaMod(texture,255); SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
        bench("render_present","clear_copy_submit_pump_not_scanout",present);
        bench("indexed_frame","convert_clear_copy_submit_pump_not_scanout",frame);
        if(!only||wanted("target_two_pass")) {
            target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,tw*2,th*2);
            if(target) {
                SDL_SetTextureBlendMode(target,SDL_BLENDMODE_NONE);
                SDL_SetTextureScaleMode(target,SDL_ScaleModeLinear);
                bench("target_two_pass","convert_nearest2x_linear_submit_pump_not_scanout",two_pass);
                SDL_DestroyTexture(target);
            } else error_record("target_texture",SDL_GetError());
        }
        if(!only||wanted("yuv420_frame")) {
            yuv=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_IYUV,SDL_TEXTUREACCESS_STREAMING,tw,th);
            planes=malloc(tw*th*3/2);
            if(yuv&&planes) {
                memset(planes,100,tw*th); memset(planes+tw*th,128,tw*th/2);
                bench("yuv420_frame","update_convert_submit_pump_not_scanout",yuv_frame);
            } else error_record("yuv_texture",SDL_GetError());
            free(planes); if(yuv) SDL_DestroyTexture(yuv);
        }
        hold_picture(); SDL_FreeSurface(lock_surface); SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer);
    }
    SDL_Delay(200);
    free(source_frames); free(pixels); SDL_FreeSurface(argb); SDL_FreeSurface(indexed); SDL_DestroyWindow(win);
    finish(); return failures?1:0;
}
