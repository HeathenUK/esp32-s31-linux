/* glx_geo.c - phase 3a geometry-path probe, ours against Mesa through
 * tools/glref (gl/tests/run-geo.sh). One 320x240 window, 8x6 cells of 40:
 *   row 0: glVertex4f with w != 1 (w = 2, 0.5, -1 through a projection,
 *          mixed w in one triangle), unlit, smooth colour; then the same
 *          vertices from a size-4 vertex array and from a display list
 *   row 1: the same, lit (one directional light, smooth), and w != 1
 *          under a perspective glFrustum
 *   row 2: glRotatef by 0, 90, 180, 270, -90, 450, 3600 + 30 and 1e6
 *          degrees about z, and about an arbitrary axis (lit fan)
 *   row 3: lighting: two-sided, a spot with an exponent, attenuation,
 *          GL_SPECULAR material with shininess 1, 20 and 128, local viewer
 *   row 4: colour material (AMBIENT_AND_DIFFUSE, DIFFUSE, EMISSION,
 *          SPECULAR, FRONT, BACK, FRONT_AND_BACK) per-vertex colours
 *   row 5: strips/fans/loops, flat, inside a display list; GL_POLYGON; a
 *          non-affine modelview (the w row) with lighting; fog EXP/EXP2;
 *          glDrawElements (the vertex cache): a lit colour-material grid as
 *          GL_TRIANGLES, and GL_QUADS in GL_LINE polygon mode with an edge
 *          flag array (cached vertices keep their own edge flags)
 * Links -lGL -lX11 -lm only. s31, MIT. */
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define W 320
#define H 240
#define C 40

static void cell(int col, int row, int persp)
{
	glViewport(col * C, H - (row + 1) * C, C, C);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	if (persp) glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 10);
	else glOrtho(0, 1, 0, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	if (persp) glTranslatef(-0.5f, -0.5f, -2.0f);
}

static void tri_w(float w0, float w1, float w2)
{
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0); glNormal3f(0, 0, 1); glVertex4f(0.1f * w0, 0.1f * w0, 0, w0);
	glColor3f(0, 1, 0); glNormal3f(0.3f, 0, 1); glVertex4f(0.9f * w1, 0.2f * w1, 0, w1);
	glColor3f(0, 0, 1); glNormal3f(0, 0.4f, 1); glVertex4f(0.5f * w2, 0.9f * w2, 0, w2);
	glEnd();
}

static const float va[] = { 0.2f, 0.2f, 0, 2, 1.8f, 0.4f, 0, 2, 0.5f, 0.45f, 0, 0.5f };
static const float na[] = { 0, 0, 1, 0.2f, 0, 1, 0, 0.3f, 1 };
static const float ca[] = { 1, 1, 0, 0, 1, 1, 1, 0, 1 };

static void lit(int on)
{
	static const float lp[4] = { 0.3f, 0.5f, 1, 0 };
	if (!on) { glDisable(GL_LIGHTING); return; }
	glLightfv(GL_LIGHT0, GL_POSITION, lp);
	glEnable(GL_LIGHT0);
	glEnable(GL_LIGHTING);
}

static void fan(void)
{
	int i;
	glBegin(GL_TRIANGLE_FAN);
	glNormal3f(0, 0, 1);
	glVertex2f(0.5f, 0.5f);
	for (i = 0; i <= 8; i++) {
		float a = i * 0.785398f;
		glColor3f((i & 1) ? 1 : 0.3f, (i & 2) ? 1 : 0.3f, (i & 4) ? 1 : 0.3f);
		glNormal3f(cosf(a) * 0.5f, sinf(a) * 0.5f, 1);
		glVertex2f(0.5f + 0.4f * cosf(a), 0.5f + 0.4f * sinf(a));
	}
	glEnd();
}

static void sphere(void)
{
	int i, j;
	for (j = 0; j < 8; j++) {
		glBegin(GL_QUAD_STRIP);
		for (i = 0; i <= 12; i++) {
			int k;
			for (k = 0; k < 2; k++) {
				float th = (j + k) / 8.0f * 3.14159265f, ph = i / 12.0f * 6.2831853f;
				float x = sinf(th) * cosf(ph), y = cosf(th), z = sinf(th) * sinf(ph);
				glColor3f(0.5f + 0.5f * x, 0.5f + 0.5f * y, 0.5f + 0.5f * z);
				glNormal3f(x, y, z);
				glVertex3f(0.5f + 0.4f * x, 0.5f + 0.4f * y, 0.4f * z);
			}
		}
		glEnd();
	}
}

/* a 9x9 grid, 8x8 cells: every inner vertex is named by 6 triangles */
#define GN 9
static float gv[GN * GN][3], gn[GN * GN][3], gc[GN * GN][3];
static unsigned short gti[(GN - 1) * (GN - 1) * 6], gqi[(GN - 1) * (GN - 1) * 4];
static unsigned char gef[GN * GN];
static void grid_init(void)
{
	int i, j, k = 0, t = 0, q = 0;
	for (j = 0; j < GN; j++)
		for (i = 0; i < GN; i++, k++) {
			float x = i / (GN - 1.0f), y = j / (GN - 1.0f);
			gv[k][0] = 0.1f + 0.8f * x; gv[k][1] = 0.1f + 0.8f * y; gv[k][2] = 0;
			gn[k][0] = x - 0.5f; gn[k][1] = y - 0.5f; gn[k][2] = 1;
			gc[k][0] = x; gc[k][1] = y; gc[k][2] = 1 - x;
			gef[k] = (i + j) & 1;
		}
	for (j = 0; j < GN - 1; j++)
		for (i = 0; i < GN - 1; i++) {
			int a = j * GN + i, b = a + GN;
			gti[t++] = a; gti[t++] = a + 1; gti[t++] = b;
			gti[t++] = a + 1; gti[t++] = b + 1; gti[t++] = b;
			gqi[q++] = a; gqi[q++] = a + 1; gqi[q++] = b + 1; gqi[q++] = b;
		}
}

static void draw(void)
{
	int i, list;
	static const float spec[4] = { 1, 1, 1, 1 }, red[4] = { 0.9f, 0.2f, 0.1f, 1 },
		blue[4] = { 0.1f, 0.3f, 0.9f, 1 };
	static const float shin[3] = { 1, 20, 128 };
	static const float angles[8] = { 0, 90, 180, 270, -90, 450, 3630, 1e6f };
	static const GLenum cm[7] = { GL_AMBIENT_AND_DIFFUSE, GL_DIFFUSE, GL_EMISSION,
		GL_SPECULAR, GL_AMBIENT_AND_DIFFUSE, GL_AMBIENT_AND_DIFFUSE, GL_AMBIENT_AND_DIFFUSE };
	static const GLenum cmf[7] = { GL_FRONT_AND_BACK, GL_FRONT_AND_BACK, GL_FRONT_AND_BACK,
		GL_FRONT_AND_BACK, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK };

	/* every frame starts from the same material and colour: glDrawElements
	   below leaves the current colour (and a colour-material-tracked
	   material) undefined (GL 1.3 2.8), and the captured frame is the 2nd */
	static const float damb[4] = { 0.2f, 0.2f, 0.2f, 1 }, ddif[4] = { 0.8f, 0.8f, 0.8f, 1 };
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, damb);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, ddif);
	glColor4f(1, 1, 1, 1);
	glClearColor(0.15f, 0.15f, 0.15f, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glShadeModel(GL_SMOOTH);

	/* row 0 and 1: w != 1, unlit then lit */
	for (i = 0; i < 2; i++) {
		lit(i);
		cell(0, i, 0); tri_w(1, 1, 1);
		cell(1, i, 0); tri_w(2, 2, 2);
		cell(2, i, 0); tri_w(0.5f, 0.5f, 0.5f);
		cell(3, i, 0); tri_w(1, 2, 0.5f);
		cell(4, i, 0); tri_w(-1, -1, -1);
		cell(5, i, 0);
		glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_NORMAL_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		glVertexPointer(4, GL_FLOAT, 0, va); glNormalPointer(GL_FLOAT, 0, na);
		glColorPointer(3, GL_FLOAT, 0, ca);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);
		cell(6, i, 0);
		list = glGenLists(1);
		glNewList(list, GL_COMPILE); tri_w(1.5f, 0.75f, 3); glEndList();
		glCallList(list); glDeleteLists(list, 1);
		cell(7, i, 1); tri_w(1, 2, 0.5f);
	}

	/* row 2: rotations */
	lit(1);
	for (i = 0; i < 8; i++) {
		cell(i, 2, 0);
		glTranslatef(0.5f, 0.5f, 0);
		glRotatef(angles[i], 0, 0, 1);
		if (i >= 6) glRotatef(angles[i] * 0.37f, 0.3f, 1, 0.2f);
		glTranslatef(-0.5f, -0.5f, 0);
		glBegin(GL_TRIANGLES);
		glColor3f(1, 1, 1); glNormal3f(0, 0, 1);
		glVertex2f(0.2f, 0.2f); glVertex2f(0.8f, 0.3f); glVertex2f(0.4f, 0.8f);
		glEnd();
	}

	/* row 3: lighting */
	glEnable(GL_DEPTH_TEST);
	for (i = 0; i < 8; i++) {
		static const float l1p[4] = { 0.2f, 0.8f, 1.5f, 1 }, sd[3] = { 0.2f, -0.3f, -1 };
		cell(i, 3, 0);
		glPushMatrix();
		lit(1);
		glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, red);
		glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, blue);
		if (i < 3) { glMaterialfv(GL_FRONT, GL_SPECULAR, spec); glMaterialf(GL_FRONT, GL_SHININESS, shin[i]); }
		if (i == 3) glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
		if (i == 4 || i == 5) {
			static const float l1c[4] = { 0.5f, 0.9f, 0.6f, 1 };
			glLightfv(GL_LIGHT1, GL_DIFFUSE, l1c);
			glLightfv(GL_LIGHT1, GL_SPECULAR, l1c);
			glLightfv(GL_LIGHT1, GL_POSITION, l1p);
			glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, i == 4 ? 30 : 90);
			glLightf(GL_LIGHT1, GL_SPOT_EXPONENT, i == 4 ? 8 : 0.5f);
			glLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, sd);
			glLightf(GL_LIGHT1, GL_QUADRATIC_ATTENUATION, 0.4f);
			glEnable(GL_LIGHT1);
		}
		if (i == 6) { glMaterialfv(GL_FRONT, GL_SPECULAR, spec); glMaterialf(GL_FRONT, GL_SHININESS, 50);
			glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1); }
		if (i == 7) { glEnable(GL_NORMALIZE); glScalef(1, 1, 3); }
		if (i == 3) { glTranslatef(0.5f, 0.5f, 0); glRotatef(180, 0, 1, 0); glTranslatef(-0.5f, -0.5f, 0); }
		sphere();
		glPopMatrix();
		glDisable(GL_LIGHT1); glDisable(GL_NORMALIZE);
		glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0); glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 0);
		{ static const float z4[4] = { 0, 0, 0, 1 }; glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, z4); }
	}

	/* row 4: colour material */
	for (i = 0; i < 7; i++) {
		cell(i, 4, 0);
		lit(1);
		glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, i == 5);
		glColorMaterial(cmf[i], cm[i]);
		glEnable(GL_COLOR_MATERIAL);
		if (i == 5) { glTranslatef(0.5f, 0.5f, 0); glRotatef(180, 0, 1, 0); glTranslatef(-0.5f, -0.5f, 0); }
		sphere();
		glDisable(GL_COLOR_MATERIAL);
		glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
		glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, red);
		{ static const float z4[4] = { 0, 0, 0, 1 }; glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, z4);
		  glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, z4); }
	}
	glDisable(GL_DEPTH_TEST);

	/* row 5 */
	lit(0);
	glShadeModel(GL_FLAT);
	cell(0, 5, 0);
	list = glGenLists(1);
	glNewList(list, GL_COMPILE); fan(); glEndList();
	glCallList(list); glDeleteLists(list, 1);
	cell(1, 5, 0);
	glBegin(GL_LINE_LOOP);
	for (i = 0; i < 7; i++) { glColor3fv(i & 1 ? red : blue); glVertex2f(0.5f + 0.4f * cosf(i * 0.9f), 0.5f + 0.4f * sinf(i * 0.9f)); }
	glEnd();
	cell(2, 5, 0);
	glBegin(GL_POLYGON);
	for (i = 0; i < 7; i++) { glColor3f(i / 7.0f, 1 - i / 7.0f, 0.5f); glVertex2f(0.5f + 0.4f * cosf(i * 0.9f), 0.5f + 0.4f * sinf(i * 0.9f)); }
	glEnd();
	glShadeModel(GL_SMOOTH);
	cell(3, 5, 0);
	{ /* a non-affine modelview, lit */
		static const float m[16] = { 1, 0, 0, 0.3f, 0, 1, 0, 0.2f, 0, 0, 1, 0, 0, 0, 0, 1 };
		lit(1);
		glMultMatrixf(m);
		fan();
		lit(0);
	}
	/* glDrawElements: the vertex cache */
	grid_init();
	glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_NORMAL_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, gv); glNormalPointer(GL_FLOAT, 0, gn);
	glColorPointer(3, GL_FLOAT, 0, gc);
	cell(6, 5, 0);
	lit(1);
	glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	glEnable(GL_COLOR_MATERIAL);
	glEnable(GL_NORMALIZE);
	glDrawElements(GL_TRIANGLES, (GN - 1) * (GN - 1) * 6, GL_UNSIGNED_SHORT, gti);
	glDisable(GL_COLOR_MATERIAL); glDisable(GL_NORMALIZE);
	lit(0);
	cell(7, 5, 0);
	glEnableClientState(GL_EDGE_FLAG_ARRAY);
	glEdgeFlagPointer(0, gef);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	glDrawElements(GL_QUADS, (GN - 1) * (GN - 1) * 4, GL_UNSIGNED_SHORT, gqi);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glDisableClientState(GL_EDGE_FLAG_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glEdgeFlag(GL_TRUE);
	for (i = 0; i < 2; i++) {
		static const float fc[4] = { 0.8f, 0.8f, 0.9f, 1 };
		cell(4 + i, 5, 1);
		glEnable(GL_FOG);
		glFogi(GL_FOG_MODE, i ? GL_EXP2 : GL_EXP);
		glFogf(GL_FOG_DENSITY, 0.6f);
		glFogfv(GL_FOG_COLOR, fc);
		glTranslatef(0, 0, -1.5f + i * 0.3f);
		fan();
		glDisable(GL_FOG);
	}
}

int main(int argc, char **argv)
{
	int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 1, None };
	Display *d = XOpenDisplay(NULL);
	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Window win;
	GLXContext ctx;
	int frames = argc > 1 ? atoi(argv[1]) : 10, f;

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
		draw();
		glXSwapBuffers(d, win);
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
