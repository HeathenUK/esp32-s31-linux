/* rampt3 (generated from rampt2.c by the build step below):
 * V_PolyBlend's untextured SRC_ALPHA quad over a grey ramp.
 * bands: 0 ramp; 1-3 ramp + flat (215,186,69)/255 at alpha .3/.1/.5 (the
 * bonus flash colour); 4 16 flat untextured greys v = 16k+5, no blend;
 * 5-7 black + the flash colour at alpha .3/.1/.5.
 * (rampt2 header follows)
 * rampt2: which of QuakeSpasm's fragment operations lose a 565 level.
 * 256x256 window, 8 bands of 32 rows; every band should reproduce band 0
 * (a grey ramp x = 0..255 drawn with GL_REPLACE, GL_NEAREST) exactly,
 * except band 5 (see below). Each is an identity in exact arithmetic:
 *  0 REPLACE, NEAREST                          (the reference)
 *  1 MODULATE with glColor4f(1,1,1,1)          (Draw_*, alias models, glow)
 *  2 band 0, then the same ramp blended SRC_ALPHA/ONE_MINUS_SRC_ALPHA with
 *    colour alpha 0.75 (MODULATE)              (scr_sbaralpha 0.75, conback)
 *  3 GL_LINEAR magnification at texel centres  (every linear-filtered texture)
 *  4 band 0, then the lightmap pass with L = 128 (2x modulate = identity)
 *  5 16 constant 64x64 textures v = 8k+4 with a full mip chain, drawn 16:1
 *    minified with GL_LINEAR_MIPMAP_LINEAR     (the world's trilinear default)
 *    expected: v truncated to 565
 *  6 untextured, vertex colours x/255 Gouraud (0 at left, 1 at right)
 *  7 band 0, then a black texture added GL_ONE/GL_ONE (fullbright pass)
 * Build: gcc rampt2.c -lGL -lX11. Loops swapping; the glref shim captures. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
static void quad(float x0, float y0, float x1, float y1, float s0, float s1)
{
    glBegin(GL_QUADS);
    glTexCoord2f(s0, 0); glVertex2f(x0, y0);
    glTexCoord2f(s1, 0); glVertex2f(x1, y0);
    glTexCoord2f(s1, 1); glVertex2f(x1, y1);
    glTexCoord2f(s0, 1); glVertex2f(x0, y1);
    glEnd();
}
int main(void)
{
    Display *d = XOpenDisplay(NULL);
    int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1,
                   GLX_BLUE_SIZE, 1, GLX_DEPTH_SIZE, 1, None };
    XVisualInfo *vi;
    XSetWindowAttributes swa;
    Window w;
    GLXContext cx;
    GLuint ramp_t, lin_t, lm_t, blk_t, cst_t[16];
    static unsigned char ramp[256 * 4], buf[64 * 64 * 4];
    int i, k, lv, sz;
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
    for (i = 0; i < 256; i++) { ramp[4*i] = ramp[4*i+1] = ramp[4*i+2] = (unsigned char)i; ramp[4*i+3] = 255; }
    glGenTextures(1, &ramp_t);
    glBindTexture(GL_TEXTURE_2D, ramp_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp);
    glGenTextures(1, &lin_t);
    glBindTexture(GL_TEXTURE_2D, lin_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp);
    for (i = 0; i < 16 * 16; i++) { buf[4*i] = buf[4*i+1] = buf[4*i+2] = 128; buf[4*i+3] = 255; }
    glGenTextures(1, &lm_t);
    glBindTexture(GL_TEXTURE_2D, lm_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, 4, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    for (i = 0; i < 16 * 16; i++) { buf[4*i] = buf[4*i+1] = buf[4*i+2] = 0; }
    glGenTextures(1, &blk_t);
    glBindTexture(GL_TEXTURE_2D, blk_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, 4, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    glGenTextures(16, cst_t);
    for (k = 0; k < 16; k++) {
        glBindTexture(GL_TEXTURE_2D, cst_t[k]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        for (i = 0; i < 64 * 64; i++) { buf[4*i] = buf[4*i+1] = buf[4*i+2] = (unsigned char)(8 * k + 4); buf[4*i+3] = 255; }
        for (lv = 0, sz = 64; sz >= 1; lv++, sz >>= 1)
            glTexImage2D(GL_TEXTURE_2D, lv, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    }
    glViewport(0, 0, 256, 256);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 256, 0, 256, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();

    for (;;) {
        static const float al[3] = { 0.3f, 0.1f, 0.5f };
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#define Y0(b) (256.0f - 32 * ((b) + 1))
#define Y1(b) (256.0f - 32 * (b))
        for (k = 0; k < 4; k++) {
            glEnable(GL_TEXTURE_2D);
            glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            glBindTexture(GL_TEXTURE_2D, ramp_t);
            quad(0, Y0(k), 256, Y1(k), 0, 1);
        }
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (k = 0; k < 3; k++) {
            glBegin(GL_QUADS);
            glColor4f(215 / 255.0f, 186 / 255.0f, 69 / 255.0f, al[k]);
            glVertex2f(0, Y0(k + 1)); glVertex2f(256, Y0(k + 1)); glVertex2f(256, Y1(k + 1)); glVertex2f(0, Y1(k + 1));
            glEnd();
            glBegin(GL_QUADS);
            glColor4f(215 / 255.0f, 186 / 255.0f, 69 / 255.0f, al[k]);
            glVertex2f(0, Y0(k + 5)); glVertex2f(256, Y0(k + 5)); glVertex2f(256, Y1(k + 5)); glVertex2f(0, Y1(k + 5));
            glEnd();
        }
        glDisable(GL_BLEND);
        for (k = 0; k < 16; k++) {
            float v = (16 * k + 5) / 255.0f;
            glBegin(GL_QUADS);
            glColor4f(v, v, v, 1);
            glVertex2f(16 * k, Y0(4)); glVertex2f(16 * k + 16, Y0(4)); glVertex2f(16 * k + 16, Y1(4)); glVertex2f(16 * k, Y1(4));
            glEnd();
        }
        glColor4f(1, 1, 1, 1);
        glXSwapBuffers(d, w);
    }
}
