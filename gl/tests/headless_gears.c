/*
 * headless_gears.c - gears through libGL.so.1's context API, no X.
 *
 *   headless_gears [width height frames out.ppm]    (320 240 100 /tmp/gears.ppm)
 *
 * Renders Brian Paul's gears (public domain; the classic glxgears scene,
 * display lists, one light, flat + smooth shading, back-face culling,
 * GL_NORMALIZE) into a malloc'd RGB565 buffer bound with s31gl_bind_color,
 * times every frame, prints ms/frame (mean, min, max) and writes the last
 * frame as a binary PPM. Built against the standard GL headers and linked
 * against the shipped library, exactly as a stock client would see it.
 * s31, MIT (the gear geometry is public domain).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GL/gl.h>
#ifdef MESA_REF
/* the same scene through a real GLX + Mesa (host rig ground truth):
   cc -DMESA_REF headless_gears.c -lGL -lX11 -lm, run under Xvfb depth 16 */
#include <GL/glx.h>
#else
#include "s31gl.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265f
#endif

static void gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
		 GLint teeth, GLfloat tooth_depth)
{
	GLint i;
	GLfloat r0, r1, r2, angle, da, u, v, len;

	r0 = inner_radius;
	r1 = outer_radius - tooth_depth / 2.0f;
	r2 = outer_radius + tooth_depth / 2.0f;
	da = 2.0f * (float)M_PI / teeth / 4.0f;

	glShadeModel(GL_FLAT);
	glNormal3f(0.0f, 0.0f, 1.0f);

	/* front face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= teeth; i++) {
		angle = i * 2.0f * (float)M_PI / teeth;
		glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
		glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
		if (i < teeth) {
			glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
			glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), width * 0.5f);
		}
	}
	glEnd();

	/* front sides of teeth */
	glBegin(GL_QUADS);
	for (i = 0; i < teeth; i++) {
		angle = i * 2.0f * (float)M_PI / teeth;
		glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
		glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), width * 0.5f);
		glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), width * 0.5f);
		glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), width * 0.5f);
	}
	glEnd();

	glNormal3f(0.0f, 0.0f, -1.0f);

	/* back face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= teeth; i++) {
		angle = i * 2.0f * (float)M_PI / teeth;
		glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);
		glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
		if (i < teeth) {
			glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), -width * 0.5f);
			glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
		}
	}
	glEnd();

	/* back sides of teeth */
	glBegin(GL_QUADS);
	for (i = 0; i < teeth; i++) {
		angle = i * 2.0f * (float)M_PI / teeth;
		glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), -width * 0.5f);
		glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), -width * 0.5f);
		glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), -width * 0.5f);
		glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);
	}
	glEnd();

	/* outward faces of teeth */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < teeth; i++) {
		angle = i * 2.0f * (float)M_PI / teeth;
		glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
		glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);
		u = r2 * cosf(angle + da) - r1 * cosf(angle);
		v = r2 * sinf(angle + da) - r1 * sinf(angle);
		len = sqrtf(u * u + v * v);
		u /= len;
		v /= len;
		glNormal3f(v, -u, 0.0f);
		glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), width * 0.5f);
		glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), -width * 0.5f);
		glNormal3f(cosf(angle), sinf(angle), 0.0f);
		glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), width * 0.5f);
		glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), -width * 0.5f);
		u = r1 * cosf(angle + 3 * da) - r2 * cosf(angle + 2 * da);
		v = r1 * sinf(angle + 3 * da) - r2 * sinf(angle + 2 * da);
		glNormal3f(v, -u, 0.0f);
		glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), width * 0.5f);
		glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), -width * 0.5f);
		glNormal3f(cosf(angle), sinf(angle), 0.0f);
	}
	glVertex3f(r1 * cosf(0), r1 * sinf(0), width * 0.5f);
	glVertex3f(r1 * cosf(0), r1 * sinf(0), -width * 0.5f);
	glEnd();

	glShadeModel(GL_SMOOTH);

	/* inside radius cylinder */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= teeth; i++) {
		angle = i * 2.0f * (float)M_PI / teeth;
		glNormal3f(-cosf(angle), -sinf(angle), 0.0f);
		glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
		glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
	}
	glEnd();
}

static GLfloat view_rotx = 20.0f, view_roty = 30.0f, view_rotz = 0.0f;
static GLuint gear1, gear2, gear3;
static GLfloat angle = 0.0f;

static void draw(void)
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glPushMatrix();
	glRotatef(view_rotx, 1.0f, 0.0f, 0.0f);
	glRotatef(view_roty, 0.0f, 1.0f, 0.0f);
	glRotatef(view_rotz, 0.0f, 0.0f, 1.0f);

	glPushMatrix();
	glTranslatef(-3.0f, -2.0f, 0.0f);
	glRotatef(angle, 0.0f, 0.0f, 1.0f);
	glCallList(gear1);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(3.1f, -2.0f, 0.0f);
	glRotatef(-2.0f * angle - 9.0f, 0.0f, 0.0f, 1.0f);
	glCallList(gear2);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(-3.1f, 4.2f, 0.0f);
	glRotatef(-2.0f * angle - 25.0f, 0.0f, 0.0f, 1.0f);
	glCallList(gear3);
	glPopMatrix();

	glPopMatrix();
}

static void reshape(int width, int height)
{
	GLfloat h = (GLfloat)height / (GLfloat)width;

	glViewport(0, 0, (GLint)width, (GLint)height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0.0f, 0.0f, -40.0f);
}

static void init(void)
{
	static GLfloat pos[4] = { 5.0f, 5.0f, 10.0f, 0.0f };
	static GLfloat red[4] = { 0.8f, 0.1f, 0.0f, 1.0f };
	static GLfloat green[4] = { 0.0f, 0.8f, 0.2f, 1.0f };
	static GLfloat blue[4] = { 0.2f, 0.2f, 1.0f, 1.0f };

	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);

	gear1 = glGenLists(1);
	glNewList(gear1, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
	gear(1.0f, 4.0f, 1.0f, 20, 0.7f);
	glEndList();

	gear2 = glGenLists(1);
	glNewList(gear2, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
	gear(0.5f, 2.0f, 2.0f, 10, 0.7f);
	glEndList();

	gear3 = glGenLists(1);
	glNewList(gear3, GL_COMPILE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, blue);
	gear(1.3f, 2.0f, 0.5f, 10, 0.7f);
	glEndList();

	glEnable(GL_NORMALIZE);
}

static double now_ms(clockid_t clk)
{
	struct timespec ts;
	clock_gettime(clk, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int write_ppm(const char *path, const unsigned short *px, int w, int h,
		     int pitch)
{
	FILE *f = fopen(path, "wb");
	int x, y;

	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (y = 0; y < h; y++) {
		const unsigned short *row = (const unsigned short *)((const char *)px + y * pitch);
		for (x = 0; x < w; x++) {
			unsigned c = row[x];
			unsigned char rgb[3];
			rgb[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
			rgb[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
			rgb[2] = (unsigned char)((c & 31) * 255 / 31);
			fwrite(rgb, 1, 3, f);
		}
	}
	return fclose(f);
}

#ifdef MESA_REF
int main(int argc, char **argv)
{
	int w = argc > 1 ? atoi(argv[1]) : 320;
	int h = argc > 2 ? atoi(argv[2]) : 240;
	int frames = argc > 3 ? atoi(argv[3]) : 100;
	const char *out = argc > 4 ? argv[4] : "/tmp/gears-mesa.ppm";
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext gc;
	unsigned char *rgb;
	unsigned short *px;
	int i, x, y;

	if (!d || !(vi = glXChooseVisual(d, DefaultScreen(d), attr))) {
		fprintf(stderr, "no display / visual\n");
		return 1;
	}
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, w, h, 0, vi->depth,
			    InputOutput, vi->visual, CWColormap | CWBorderPixel, &swa);
	XMapWindow(d, win);
	gc = glXCreateContext(d, vi, NULL, True);
	glXMakeCurrent(d, win, gc);
	printf("reference GL_RENDERER %s\n", glGetString(GL_RENDERER));
	init();
	reshape(w, h);
	for (i = 0; i < frames; i++) {
		angle += 2.0f;
		draw();
		if (i + 1 < frames)
			glXSwapBuffers(d, win);
	}
	glFinish();
	rgb = malloc((size_t)w * h * 3);
	px = malloc((size_t)w * h * 2);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	/* to RGB565, top row first, like the buffer the core renders into */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			const unsigned char *p = rgb + ((h - 1 - y) * w + x) * 3;
			px[y * w + x] = (unsigned short)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
		}
	write_ppm(out, px, w, h, w * 2);
	printf("wrote %s (frame %d)\n", out, frames);
	return 0;
}
#else
int main(int argc, char **argv)
{
	int w = argc > 1 ? atoi(argv[1]) : 320;
	int h = argc > 2 ? atoi(argv[2]) : 240;
	int frames = argc > 3 ? atoi(argv[3]) : 100;
	const char *out = argc > 4 ? argv[4] : "/tmp/gears.ppm";
	int pitch = w * 2, i;
	unsigned short *px;
	s31gl_ctx *ctx;
	double t0, t, sum = 0, mn = 1e9, mx = 0, c0, c1;
	GLenum err;

	if (w <= 0 || h <= 0 || frames <= 0) {
		fprintf(stderr, "usage: %s [width height frames out.ppm]\n", argv[0]);
		return 2;
	}
	px = malloc((size_t)pitch * h);
	ctx = s31gl_create_context(NULL);
	if (!px || !ctx) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	s31gl_bind_color(ctx, px, w, h, pitch);
	s31gl_make_current(ctx);
	printf("GL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\nGL_EXTENSIONS %s\n",
	       glGetString(GL_VENDOR), glGetString(GL_RENDERER),
	       glGetString(GL_VERSION), glGetString(GL_EXTENSIONS));

	init();
	reshape(w, h);

	c0 = now_ms(CLOCK_PROCESS_CPUTIME_ID);
	for (i = 0; i < frames; i++) {
		angle += 2.0f;
		t0 = now_ms(CLOCK_MONOTONIC);
		draw();
		glFinish();
		t = now_ms(CLOCK_MONOTONIC) - t0;
		s31gl_frame_end(ctx);
		sum += t;
		if (t < mn)
			mn = t;
		if (t > mx)
			mx = t;
	}
	c1 = now_ms(CLOCK_PROCESS_CPUTIME_ID);
	err = glGetError();
	printf("headless_gears %dx%d: %d frames, ms/frame mean %.2f min %.2f max %.2f "
	       "(cpu %.2f ms/frame), depth %d bytes, glGetError 0x%x\n",
	       w, h, frames, sum / frames, mn, mx, (c1 - c0) / frames,
	       s31gl_depth_bytes(ctx), err);
	if (write_ppm(out, px, w, h, pitch) != 0) {
		fprintf(stderr, "cannot write %s\n", out);
		return 1;
	}
	printf("wrote %s (frame %d)\n", out, frames);
	s31gl_make_current(NULL);
	s31gl_destroy_context(ctx);
	free(px);
	return err != GL_NO_ERROR;
}
#endif
