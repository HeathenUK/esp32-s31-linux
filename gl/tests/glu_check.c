/* glu_check.c - the real Mesa GLU against our libGL and against Mesa
 * (gl/tests/run-pixels.sh builds it twice: with Debian's libglu1-mesa and
 * with the board's GLU 9.0.3):
 *     glu_check PAGE [frames]
 *   1 quadrics (sphere, cylinder, disk, partial disk; fill, line, point and
 *     silhouette styles; smooth and flat normals; textured; inside
 *     orientation) under gluPerspective / gluLookAt, the tessellator
 *     (concave, holes, winding rules, boundary only, combine)
 *   2 gluBuild2DMipmaps / gluBuild1DMipmaps / gluBuild2DMipmapLevels,
 *     gluScaleImage (drawn with glDrawPixels, checksummed), gluOrtho2D,
 *     gluProject / gluUnProject on glGetDoublev's matrices, gluPickMatrix
 *     with GL_SELECT, gluErrorString / gluGetString
 *   3 a NURBS surface and curve (GLU renders them with the GL evaluators,
 *     which are plan F8 and not implemented: this page documents the gap)
 * Prints "glu ..." lines for gl/tests/logcmp.py. s31, MIT.
 */
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CALLBACK
#define CALLBACK
#endif
#define W 320
#define H 240

static int page = 1, first;

static void err(const char *what)
{
	GLenum e = glGetError();
	if (first) printf("glu error after %s: 0x%x\n", what, e);
}

static void light(void)
{
	float pos[4] = { 1, 1, 2, 0 }, dif[4] = { .8f, .7f, .5f, 1 }, spe[4] = { .5f, .5f, .5f, 1 };
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, dif);
	glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spe);
	glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 20);
}

/* a 3x2 grid of 106x120 views, each a perspective camera */
static void view(int i)
{
	glViewport((i % 3) * 106, (1 - i / 3) * 120, 106, 120);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(40, 106.0 / 120.0, 1, 20);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gluLookAt(2.5, 1.5, 4, 0, 0, 0, 0, 1, 0);
}

static GLuint checker(void)
{
	static unsigned char p[16 * 16 * 3];
	GLuint t;
	int x, y;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++) {
			unsigned char *q = p + (y * 16 + x) * 3;
			int on = ((x / 2) + (y / 2)) & 1;
			q[0] = on ? 250 : 40; q[1] = on ? 200 : 60; q[2] = (unsigned char)(x * 16);
		}
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, p);
	return t;
}

/* ---- tessellator callbacks ---- */
static void CALLBACK tbegin(GLenum m) { glBegin(m); }
static void CALLBACK tend(void) { glEnd(); }
static void CALLBACK tvertex(void *v) { glVertex3dv((const GLdouble *)v); }
static void CALLBACK terror(GLenum e) { printf("glu tess error %s\n", gluErrorString(e)); }
static GLdouble combined[64][3];
static int ncombined;
static void CALLBACK tcombine(GLdouble c[3], void *d[4], GLfloat w[4], void **out)
{
	GLdouble *v = combined[ncombined++ & 63];
	(void)d; (void)w;
	v[0] = c[0]; v[1] = c[1]; v[2] = c[2];
	*out = v;
}

static void tess_poly(GLUtesselator *t, GLdouble (*pts)[3], int n)
{
	int i;
	gluTessBeginContour(t);
	for (i = 0; i < n; i++) gluTessVertex(t, pts[i], pts[i]);
	gluTessEndContour(t);
}

static void page_quadrics(void)
{
	GLUquadric *q = gluNewQuadric();
	GLUtesselator *t;
	GLuint tx = checker();
	static GLdouble star[10][3], outer[4][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 } };
	static GLdouble hole[4][3] = { { -.4, -.4, 0 }, { -.4, .4, 0 }, { .4, .4, 0 }, { .4, -.4, 0 } };
	static GLdouble bow[4][3] = { { -1, -1, 0 }, { 1, 1, 0 }, { 1, -1, 0 }, { -1, 1, 0 } };
	int i;

	for (i = 0; i < 10; i++) {
		double a = 3.14159265358979 * 2 * i / 10 + 3.14159265358979 / 2, r = i & 1 ? .4 : 1;
		star[i][0] = r * cos(a); star[i][1] = r * sin(a); star[i][2] = 0;
	}
	glEnable(GL_DEPTH_TEST);
	/* 0: a smooth lit sphere */
	view(0);
	light();
	gluQuadricNormals(q, GLU_SMOOTH);
	gluSphere(q, 1, 20, 14);
	/* 1: a flat-shaded textured cylinder with its end disks */
	view(1);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tx);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	gluQuadricTexture(q, GL_TRUE);
	gluQuadricNormals(q, GLU_FLAT);
	glTranslatef(0, 0, -1);
	gluCylinder(q, .8, .4, 2, 16, 4);
	glRotatef(180, 1, 0, 0);
	gluDisk(q, 0, .8, 16, 2);
	glDisable(GL_TEXTURE_2D);
	gluQuadricTexture(q, GL_FALSE);
	glDisable(GL_LIGHTING);
	/* 2: disks as lines and silhouette, a partial disk */
	view(2);
	glColor3f(.3f, 1, .3f);
	gluQuadricDrawStyle(q, GLU_LINE);
	gluDisk(q, .3, 1.2, 12, 3);
	glTranslatef(0, 1, 0);
	glColor3f(1, .5f, .2f);
	gluQuadricDrawStyle(q, GLU_SILHOUETTE);
	gluPartialDisk(q, .2, .9, 10, 2, 30, 200);
	glTranslatef(0, -2, 0);
	gluQuadricDrawStyle(q, GLU_FILL);
	glColor3f(.4f, .6f, 1);
	gluPartialDisk(q, .2, .9, 10, 2, -45, 135);
	/* 3: points, and an inside-out sphere, cut open by a clip plane */
	view(3);
	glColor3f(1, 1, .5f);
	gluQuadricDrawStyle(q, GLU_POINT);
	gluSphere(q, 1.3, 16, 10);
	gluQuadricDrawStyle(q, GLU_FILL);
	light();
	{
		GLdouble eq[4] = { 0, 0, -1, .2 };
		glClipPlane(GL_CLIP_PLANE0, eq);
		glEnable(GL_CLIP_PLANE0);
	}
	gluQuadricOrientation(q, GLU_INSIDE);
	gluQuadricNormals(q, GLU_SMOOTH);
	gluSphere(q, .9, 16, 12);
	gluQuadricOrientation(q, GLU_OUTSIDE);
	glDisable(GL_CLIP_PLANE0);
	glDisable(GL_LIGHTING);
	/* 4: the tessellator */
	glDisable(GL_DEPTH_TEST);
	glViewport(106, 0, 106, 120);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluOrtho2D(-2.2, 2.2, -2.5, 2.5);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	t = gluNewTess();
	gluTessCallback(t, GLU_TESS_BEGIN, (_GLUfuncptr)tbegin);
	gluTessCallback(t, GLU_TESS_END, (_GLUfuncptr)tend);
	gluTessCallback(t, GLU_TESS_VERTEX, (_GLUfuncptr)tvertex);
	gluTessCallback(t, GLU_TESS_ERROR, (_GLUfuncptr)terror);
	gluTessCallback(t, GLU_TESS_COMBINE, (_GLUfuncptr)tcombine);
	glPushMatrix();
	glTranslatef(-1.1f, 1.2f, 0);
	glColor3f(1, .8f, .2f);
	gluTessProperty(t, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
	gluTessBeginPolygon(t, NULL);
	tess_poly(t, star, 10);
	gluTessEndPolygon(t);
	glTranslatef(2.2f, 0, 0);
	glColor3f(.2f, .8f, 1);
	gluTessBeginPolygon(t, NULL);			/* a square with a hole */
	tess_poly(t, outer, 4);
	tess_poly(t, hole, 4);
	gluTessEndPolygon(t);
	glTranslatef(-2.2f, -2.4f, 0);
	glColor3f(1, .3f, .6f);
	gluTessProperty(t, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_NONZERO);
	gluTessBeginPolygon(t, NULL);			/* a bow tie: combine */
	tess_poly(t, bow, 4);
	gluTessEndPolygon(t);
	glTranslatef(2.2f, 0, 0);
	glColor3f(1, 1, 1);
	gluTessProperty(t, GLU_TESS_BOUNDARY_ONLY, GL_TRUE);
	gluTessProperty(t, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_POSITIVE);
	gluTessBeginPolygon(t, NULL);
	tess_poly(t, outer, 4);
	tess_poly(t, hole, 4);
	gluTessEndPolygon(t);
	glPopMatrix();
	gluDeleteTess(t);
	if (first) printf("glu tess combined %d vertices\n", ncombined);
	gluDeleteQuadric(q);
	glDeleteTextures(1, &tx);
	err("page 1");
}

/* ---------------------------------------------------------------- page 3 */

/* NURBS (GLU's C++ part): GLU draws them through the GL evaluators
   (glMap2f / glEvalMesh2), plan F8 - expected to differ until those exist */
static void page_nurbs(void)
{
	GLfloat knots[8] = { 0, 0, 0, 0, 1, 1, 1, 1 }, ctl[4][4][3], cc[4][3];
	int u, v, i;

	for (u = 0; u < 4; u++)
		for (v = 0; v < 4; v++) {
			ctl[u][v][0] = 2.0f * ((float)u - 1.5f) / 1.5f;
			ctl[u][v][1] = 2.0f * ((float)v - 1.5f) / 1.5f;
			ctl[u][v][2] = ((u == 1 || u == 2) && (v == 1 || v == 2)) ? 2.0f : -1.0f;
		}
	for (u = 0; u < 4; u++) {
		cc[u][0] = -1.5f + u; cc[u][1] = (u & 1) ? 1.5f : -1.0f; cc[u][2] = 1.5f;
	}
	for (i = 0; i < 2; i++) {
		GLUnurbs *n = gluNewNurbsRenderer();
		view(i * 4);
		glEnable(GL_DEPTH_TEST);
		light();
		glEnable(GL_AUTO_NORMAL);
		glScalef(.45f, .45f, .45f);
		glRotatef(-60, 1, 0, 0);
		gluNurbsProperty(n, GLU_SAMPLING_TOLERANCE, 25);
		gluNurbsProperty(n, GLU_DISPLAY_MODE, i ? GLU_OUTLINE_POLYGON : GLU_FILL);
		gluBeginSurface(n);
		gluNurbsSurface(n, 8, knots, 8, knots, 4 * 3, 3, &ctl[0][0][0], 4, 4, GL_MAP2_VERTEX_3);
		gluEndSurface(n);
		glDisable(GL_LIGHTING);
		glDisable(GL_AUTO_NORMAL);
		glColor3f(1, 1, 0);
		gluBeginCurve(n);
		gluNurbsCurve(n, 8, knots, 3, &cc[0][0], 4, GL_MAP1_VERTEX_3);
		gluEndCurve(n);
		gluDeleteNurbsRenderer(n);
		glDisable(GL_DEPTH_TEST);
	}
	err("page 3");
}

/* ---------------------------------------------------------------- page 2 */

static void page_images(void)
{
	static unsigned char src[13 * 7 * 3], dst[40 * 30 * 3], odd[100 * 60 * 4], one[20 * 3];
	static unsigned char big[300 * 200 * 3];
	GLuint t[3];
	GLdouble mv[16], pr[16], ox, oy, oz, bx, by, bz;
	GLint vp[4], i, x, y, r, w, h;
	unsigned int sum = 0;
	GLuint sel[64];

	for (i = 0; i < 13 * 7 * 3; i++) src[i] = (unsigned char)((i * 53) ^ (i >> 2));
	for (y = 0; y < 60; y++)
		for (x = 0; x < 100; x++) {
			unsigned char *p = odd + (y * 100 + x) * 4;
			p[0] = (unsigned char)(x * 2); p[1] = (unsigned char)(y * 4);
			p[2] = (unsigned char)(((x / 10 + y / 10) & 1) ? 220 : 30); p[3] = 255;
		}
	for (i = 0; i < 60; i++) one[i] = (unsigned char)(i * 13);
	for (i = 0; i < 300 * 200 * 3; i++) big[i] = (unsigned char)((i / 3 % 300) ^ (i / 900));

	/* gluScaleImage: pure CPU, reads the pixel store through glGetIntegerv */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	r = gluScaleImage(GL_RGB, 13, 7, GL_UNSIGNED_BYTE, src, 40, 30, GL_UNSIGNED_BYTE, dst);
	for (i = 0; i < 40 * 30 * 3; i++) sum = sum * 31 + dst[i];
	if (first) printf("glu gluScaleImage -> %d sum %u\n", r, sum);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluOrtho2D(0, W, 0, H);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRasterPos2i(4, 196);
	glDrawPixels(40, 30, GL_RGB, GL_UNSIGNED_BYTE, dst);
	/* glRect under gluOrtho2D */
	glColor3f(.8f, .2f, .6f);
	glRecti(50, 196, 90, 236);
	glColor3f(.2f, .6f, .8f);
	glRectf(60.5f, 206.5f, 80.5f, 226.5f);

	/* mipmaps: 100x60 RGBA scaled to 64x64 (via our proxy answers) */
	glGenTextures(3, t);
	glBindTexture(GL_TEXTURE_2D, t[0]);
	r = gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, 100, 60, GL_RGBA, GL_UNSIGNED_BYTE, odd);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	if (first) printf("glu gluBuild2DMipmaps 100x60 -> %d level 0 %dx%d\n", r, w, h);
	for (i = 1; first && i < 8; i++) {
		glGetTexLevelParameteriv(GL_TEXTURE_2D, i, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, i, GL_TEXTURE_HEIGHT, &h);
		printf("glu level %d %dx%d\n", i, w, h);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(100, 110);
	glTexCoord2f(1, 0); glVertex2f(228, 110);
	glTexCoord2f(1, 1); glVertex2f(228, 238);
	glTexCoord2f(0, 1); glVertex2f(100, 238);
	glEnd();
	/* an image larger than GL_MAX_TEXTURE_SIZE: GLU halves it */
	glBindTexture(GL_TEXTURE_2D, t[1]);
	r = gluBuild2DMipmaps(GL_TEXTURE_2D, 3, 300, 200, GL_RGB, GL_UNSIGNED_BYTE, big);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	/* our GL_MAX_TEXTURE_SIZE is 256 (Mesa's is larger): the size differs
	   by design, so only the result is compared */
	if (first) printf("glu gluBuild2DMipmaps 300x200 -> %d\n", r);
	if (first) fprintf(stderr, "300x200 level 0: %dx%d\n", w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(236, 110);
	glTexCoord2f(.25f, 0); glVertex2f(316, 110);
	glTexCoord2f(.25f, .25f); glVertex2f(316, 190);
	glTexCoord2f(0, .25f); glVertex2f(236, 190);
	glEnd();
	glDisable(GL_TEXTURE_2D);
	/* review G3: images whose nearest power of two exceeds the maximum.
	   GLU probes the proxy at level 1 with half the size; a proxy that
	   ignored the level had GLU pick a base the real upload refused, and
	   gluBuild2DMipmaps returned 0 with no level 0. The base must be the
	   largest size allowed, min(nearest power of two, GL_MAX_TEXTURE_SIZE),
	   with no GL error. Drawn nowhere; t[1] is rebuilt below the frame */
	if (first) {
		static unsigned char huge[512 * 512 * 3];
		static const int sz[2] = { 512, 384 };
		GLint mx, k;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mx);
		for (k = 0; k < 2; k++) {
			GLenum e;
			while (glGetError()) ;
			r = gluBuild2DMipmaps(GL_TEXTURE_2D, 3, sz[k], sz[k], GL_RGB, GL_UNSIGNED_BYTE, huge);
			e = glGetError();
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
			printf("glu gluBuild2DMipmaps %dx%d -> %d, glGetError 0x%x, level 0 is the largest allowed: %s\n",
			       sz[k], sz[k], r, e, w == (mx < 512 ? mx : 512) ? "yes" : "no");
		}
		glBindTexture(GL_TEXTURE_2D, t[1]);
		gluBuild2DMipmaps(GL_TEXTURE_2D, 3, 300, 200, GL_RGB, GL_UNSIGNED_BYTE, big);
	}
	/* gluBuild1DMipmaps: our glTexImage1D */
	glBindTexture(GL_TEXTURE_1D, t[2]);
	r = gluBuild1DMipmaps(GL_TEXTURE_1D, GL_RGB, 20, GL_RGB, GL_UNSIGNED_BYTE, one);
	glGetTexLevelParameteriv(GL_TEXTURE_1D, 0, GL_TEXTURE_WIDTH, &w);
	if (first) printf("glu gluBuild1DMipmaps 20 -> %d level 0 width %d\n", r, w);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEnable(GL_TEXTURE_1D);
	glBegin(GL_QUADS);
	glTexCoord1f(0); glVertex2f(4, 150);
	glTexCoord1f(1); glVertex2f(92, 150);
	glTexCoord1f(1); glVertex2f(92, 186);
	glTexCoord1f(0); glVertex2f(4, 186);
	glEnd();
	glDisable(GL_TEXTURE_1D);
	/* gluBuild2DMipmapLevels: levels 1..3 of a 32x32 base */
	glBindTexture(GL_TEXTURE_2D, t[1]);
	r = gluBuild2DMipmapLevels(GL_TEXTURE_2D, GL_RGBA, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE,
				   1, 1, 3, odd);
	if (first) printf("glu gluBuild2DMipmapLevels -> %d\n", r);
	glDeleteTextures(3, t);

	/* gluProject / gluUnProject on the matrices glGetDoublev reports */
	glViewport(10, 20, 200, 100);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(50, 2, .5, 30);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	gluLookAt(1, 2, 5, 0, .5, 0, 0, 1, 0);
	glRotatef(20, 0, 1, 0);
	glGetDoublev(GL_MODELVIEW_MATRIX, mv);
	glGetDoublev(GL_PROJECTION_MATRIX, pr);
	glGetIntegerv(GL_VIEWPORT, vp);
	r = gluProject(.5, -.25, 1, mv, pr, vp, &ox, &oy, &oz);
	if (first) printf("glu project -> %d %.3f %.3f %.5f\n", r, ox, oy, oz);
	r = gluUnProject(ox, oy, oz, mv, pr, vp, &bx, &by, &bz);
	if (first) printf("glu unproject -> %d %.4f %.4f %.4f\n", r, bx, by, bz);
	r = gluUnProject(60, 70, .5, mv, pr, vp, &bx, &by, &bz);
	if (first) printf("glu unproject mid -> %d %.4f %.4f %.4f\n", r, bx, by, bz);
	if (first) printf("glu mv %.4f %.4f %.4f %.4f  pr %.4f %.4f %.4f\n", mv[0], mv[5], mv[10],
			  mv[14], pr[0], pr[10], pr[14]);
	/* and drawn: a lit sphere where gluProject says the point is */
	{
		GLUquadric *q = gluNewQuadric();
		glEnable(GL_DEPTH_TEST);
		light();
		glTranslatef(.5f, -.25f, 1);
		gluSphere(q, .3, 12, 8);
		glDisable(GL_LIGHTING);
		glDisable(GL_DEPTH_TEST);
		gluDeleteQuadric(q);
	}
	/* gluPickMatrix + GL_SELECT: names of what is under a 5x5 pick box */
	glSelectBuffer(64, sel);
	glRenderMode(GL_SELECT);
	glInitNames();
	glPushName(0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPickMatrix(60, 60, 5, 5, vp);
	gluOrtho2D(0, 200, 0, 100);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	for (i = 1; i <= 4; i++) {
		glLoadName(i);
		glRectf(i * 20.0f, 20, i * 20.0f + 35, 60);
	}
	r = glRenderMode(GL_RENDER);
	if (first) {
		int k = 0, n;
		printf("glu pick hits %d:", r);
		for (n = 0; n < r && k < 64; n++) {
			int names = sel[k];
			printf(" [%d names", names);
			for (i = 0; i < names; i++) printf(" %u", sel[k + 3 + i]);
			printf("]");
			k += 3 + names;
		}
		printf("\n");
	}
	if (first) {
		printf("glu version %s\n", (const char *)gluGetString(GLU_VERSION));
		printf("glu error string %s / %s\n", (const char *)gluErrorString(GLU_INVALID_ENUM),
		       (const char *)gluErrorString(GL_INVALID_OPERATION));
		printf("glu check extension %d\n",
		       gluCheckExtension((const GLubyte *)"GLU_EXT_nurbs_tessellator",
					 gluGetString(GLU_EXTENSIONS)));
	}
	err("page 2");
}

static void draw(void)
{
	glClearColor(0.1f, 0.1f, 0.15f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	if (page == 1) page_quadrics();
	else if (page == 2) page_images();
	else page_nurbs();
}

int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	int frames, f;

	page = argc > 1 ? atoi(argv[1]) : 1;
	frames = argc > 2 ? atoi(argv[2]) : 4;
	if (!d) { fprintf(stderr, "no display\n"); return 1; }
	vi = glXChooseVisual(d, DefaultScreen(d), attr);
	if (!vi) { fprintf(stderr, "no visual\n"); return 1; }
	swa.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone);
	swa.border_pixel = 0;
	swa.event_mask = StructureNotifyMask;
	win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth,
			    InputOutput, vi->visual, CWColormap | CWBorderPixel | CWEventMask, &swa);
	XMapWindow(d, win);
	for (;;) {
		XEvent e;
		XNextEvent(d, &e);
		if (e.type == MapNotify) break;
	}
	ctx = glXCreateContext(d, vi, NULL, True);
	XFree(vi);
	glXMakeCurrent(d, win, ctx);
	for (f = 0; f < frames; f++) {
		first = f == 0;
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
