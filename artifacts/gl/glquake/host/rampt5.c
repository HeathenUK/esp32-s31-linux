/* rampt5: the lightmap pass with bilinear magnification, as QuakeSpasm
 * draws it. 256x256 window, 8 bands. Every band: pass 1 a flat 16x16 world
 * texture of value T = 8*(x/16)+3 per 16-px column group (16 greys, GL_REPLACE,
 * NEAREST); pass 2 a 256x256 internal-format-4 lightmap (QuakeSpasm's
 * LMBLOCK, uploaded by glTexImage2D then glTexSubImage2D), constant value L
 * for the band, GL_LINEAR, magnified 64x (fractional sample positions
 * everywhere), glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR).
 * Bands L = 20 39 55 71 100 135 199 255. Build: gcc rampt5.c -lGL -lX11 */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
static const int LM[8] = { 20, 39, 55, 71, 100, 135, 199, 255 };
static unsigned char lm[256 * 256 * 4], tx[16 * 16 * 4];
int main(void)
{
    Display *d = XOpenDisplay(NULL);
    int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1,
                   GLX_BLUE_SIZE, 1, GLX_DEPTH_SIZE, 1, None };
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    Window w;
    GLXContext cx;
    GLuint t[16], l[8];
    int i, b, k;
    if (!d) return 1;
    vi = glXChooseVisual(d, DefaultScreen(d), attr);
    if (!vi) return 1;
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask;
    w = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 256, 256, 0, vi->depth,
                      InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(d, w);
    for (;;) { XEvent e; XNextEvent(d, &e); if (e.type == MapNotify) break; }
    cx = glXCreateContext(d, vi, NULL, True);
    glXMakeCurrent(d, w, cx);
    glGenTextures(16, t);
    for (k = 0; k < 16; k++) {
        for (i = 0; i < 16 * 16; i++) { tx[4*i] = tx[4*i+1] = tx[4*i+2] = (unsigned char)(8 * k + 3 + 64); tx[4*i+3] = 255; }
        glBindTexture(GL_TEXTURE_2D, t[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, 3, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx);
    }
    glGenTextures(8, l);
    for (b = 0; b < 8; b++) {
        memset(lm, 0, sizeof lm);
        glBindTexture(GL_TEXTURE_2D, l[b]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, 4, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, lm);
        for (i = 0; i < 256 * 256; i++) { lm[4*i] = lm[4*i+1] = lm[4*i+2] = (unsigned char)LM[b]; lm[4*i+3] = 255; }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, lm);
    }
    glViewport(0, 0, 256, 256);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 256, 0, 256, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    for (;;) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (b = 0; b < 8; b++) {
            float y0 = 256 - 32 * (b + 1), y1 = y0 + 32;
            glDisable(GL_BLEND); glDepthMask(GL_TRUE);
            for (k = 0; k < 16; k++) {
                glBindTexture(GL_TEXTURE_2D, t[k]);
                glBegin(GL_QUADS);
                glTexCoord2f(0, 0); glVertex2f(16 * k, y0);
                glTexCoord2f(1, 0); glVertex2f(16 * k + 16, y0);
                glTexCoord2f(1, 1); glVertex2f(16 * k + 16, y1);
                glTexCoord2f(0, 1); glVertex2f(16 * k, y1);
                glEnd();
            }
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            glBindTexture(GL_TEXTURE_2D, l[b]);
            glBegin(GL_QUADS);
            glTexCoord2f(0.1f / 256, 0.1f / 256); glVertex2f(0, y0);
            glTexCoord2f(4.1f / 256, 0.1f / 256); glVertex2f(256, y0);
            glTexCoord2f(4.1f / 256, 0.6f / 256); glVertex2f(256, y1);
            glTexCoord2f(0.1f / 256, 0.6f / 256); glVertex2f(0, y1);
            glEnd();
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_BLEND);
        }
        glXSwapBuffers(d, w);
    }
}
