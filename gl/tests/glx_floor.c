/* glx_floor.c - a Quake-like receding floor (review 4 R1, the phase 4
   review's floor.c): one big quad (2 triangles) or a tessellated grid, a
   64x64 box-filtered mipmapped texture built here so both GLs get the same
   levels. argv: frames minfilter(0 LML, 1 LMN, 2 NMN, 3 LINEAR, 4 NEAREST,
   5 NML) tess. run-p4apps.sh scores it against Mesa. s31, MIT. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 320
#define H 240
int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	int frames = argc > 1 ? atoi(argv[1]) : 3, mf = argc > 2 ? atoi(argv[2]) : 0, tess = argc > 3 ? atoi(argv[3]) : 1;
	GLenum mins[6] = { GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR, GL_NEAREST, GL_NEAREST_MIPMAP_LINEAR };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi = glXChooseVisual(d, DefaultScreen(d), attr);
	XSetWindowAttributes swa; Window win; GLXContext ctx; XEvent e;
	static unsigned char img[64 * 64 * 3], lv[64 * 64 * 3];
	int f, x, y, l, w, i, j;
	GLuint t;
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.event_mask = StructureNotifyMask;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
	XMapWindow(d, win); do XNextEvent(d, &e); while (e.type != MapNotify);
	ctx = glXCreateContext(d, vi, NULL, True); glXMakeCurrent(d, win, ctx);
	for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) {
		unsigned char *p = img + 3 * (y * 64 + x);
		int on = ((x >> 3) ^ (y >> 3)) & 1;
		p[0] = (unsigned char)(on ? 200 : 60) + (unsigned char)((x * 7 + y * 13) & 31);
		p[1] = (unsigned char)(on ? 150 : 90) + (unsigned char)((x * 3) & 31);
		p[2] = (unsigned char)(on ? 60 : 120) + (unsigned char)((y * 5) & 31);
		if (x % 16 == 0 || y % 16 == 0) p[0] = p[1] = p[2] = 250;
	}
	glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	/* box-filtered chain, built here so both GLs get the same levels */
	memcpy(lv, img, sizeof img);
	for (l = 0, w = 64; w >= 1; l++, w /= 2) {
		glTexImage2D(GL_TEXTURE_2D, l, GL_RGB, w, w, 0, GL_RGB, GL_UNSIGNED_BYTE, lv);
		if (w == 1) break;
		for (y = 0; y < w / 2; y++) for (x = 0; x < w / 2; x++) for (i = 0; i < 3; i++) {
			int s = lv[3 * ((2*y) * w + 2*x) + i] + lv[3 * ((2*y) * w + 2*x+1) + i] + lv[3 * ((2*y+1) * w + 2*x) + i] + lv[3 * ((2*y+1) * w + 2*x+1) + i];
			lv[3 * (y * (w / 2) + x) + i] = (unsigned char)((s + 2) / 4);
		}
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mins[mf]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mf == 2 || mf == 4 || mf == 5) ? GL_NEAREST : GL_LINEAR);
	glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnable(GL_DEPTH_TEST);
	for (f = 0; f < frames; f++) {
		glClearColor(.2f, .3f, .5f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glViewport(0, 0, W, H);
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-.133, .133, -.1, .1, .1, 200);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();
		glRotatef(8, 0, 1, 0);
		for (j = 0; j < tess; j++) for (i = 0; i < tess; i++) {
			float x0 = -60 + 120.0f * i / tess, x1 = -60 + 120.0f * (i + 1) / tess;
			float z0 = -1 - 150.0f * j / tess, z1 = -1 - 150.0f * (j + 1) / tess;
			float s0 = x0 / 4, s1 = x1 / 4, t0 = z0 / 4, t1 = z1 / 4;
			glBegin(GL_QUADS);
			glTexCoord2f(s0, t0); glVertex3f(x0, -1.5f, z0);
			glTexCoord2f(s1, t0); glVertex3f(x1, -1.5f, z0);
			glTexCoord2f(s1, t1); glVertex3f(x1, -1.5f, z1);
			glTexCoord2f(s0, t1); glVertex3f(x0, -1.5f, z1);
			glEnd();
		}
		glXSwapBuffers(d, win);
	}
	return 0;
}
