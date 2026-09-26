/* rampt: isolate the precision of QuakeSpasm's no-combiner world path.
 * 256x256 window, 8 horizontal bands of 32 rows. Every band:
 *   pass 1: a 256x1 GL_RGBA texture, texel x = (x, x, x, 255), GL_REPLACE
 *           (R_DrawTextureChains_TextureOnly)
 *   pass 2: a lightmap texture, internal format 4 (= lightmap_bytes, as
 *           r_brush.c), filled by glTexSubImage2D with the band's flat
 *           value L, glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR) with depth
 *           writes off (r_world.c case 3). Band 0 skips pass 2 (texture only).
 * Expected colour: min(255, 2 * x * L / 255). Draws, swaps (the glref
 * capture shim grabs the swap), loops. Build: gcc rampt.c -lGL -lX11 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#ifndef LMV
#define LMV 16, 32, 48, 64, 96, 128, 255
#endif
static const int LM[8] = { 0, LMV };
int main(void)
{
    Display *d = XOpenDisplay(NULL);
    int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1,
                   GLX_BLUE_SIZE, 1, GLX_DEPTH_SIZE, 1, None };
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    Window w;
    GLXContext cx;
    GLuint tex[2];
    unsigned char ramp[256 * 4], lm[16 * 16 * 4];
    int i, b, frame;
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    vi = glXChooseVisual(d, DefaultScreen(d), attr);
    if (!vi) { fprintf(stderr, "no visual\n"); return 1; }
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    w = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 256, 256, 0, vi->depth,
                      InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(d, w);
    for (;;) { XEvent e; XNextEvent(d, &e); if (e.type == MapNotify) break; }
    cx = glXCreateContext(d, vi, NULL, True);
    glXMakeCurrent(d, w, cx);
    printf("GL_VENDOR %s\n", (const char *)glGetString(GL_VENDOR));
    for (i = 0; i < 256; i++) { ramp[4*i] = ramp[4*i+1] = ramp[4*i+2] = (unsigned char)i; ramp[4*i+3] = 255; }
    glGenTextures(2, tex);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    memset(lm, 0, sizeof lm);
    glTexImage2D(GL_TEXTURE_2D, 0, 4, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, lm);
    glViewport(0, 0, 256, 256);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 256, 0, 256, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    for (frame = 0; ; frame++) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (b = 0; b < 8; b++) {
            float y0 = 256 - 32 * (b + 1), y1 = y0 + 32;
            glDisable(GL_BLEND); glDepthMask(GL_TRUE);
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, y0);
            glTexCoord2f(1, 0); glVertex2f(256, y0);
            glTexCoord2f(1, 1); glVertex2f(256, y1);
            glTexCoord2f(0, 1); glVertex2f(0, y1);
            glEnd();
            if (b == 0) continue;
            for (i = 0; i < 16 * 16; i++) { lm[4*i] = lm[4*i+1] = lm[4*i+2] = (unsigned char)LM[b]; lm[4*i+3] = 255; }
            glBindTexture(GL_TEXTURE_2D, tex[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, lm);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, y0);
            glTexCoord2f(1, 0); glVertex2f(256, y0);
            glTexCoord2f(1, 1); glVertex2f(256, y1);
            glTexCoord2f(0, 1); glVertex2f(0, y1);
            glEnd();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_BLEND);
        }
        glXSwapBuffers(d, w);
    }
}
