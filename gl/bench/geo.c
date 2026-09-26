/* geo.c - phase 3a geometry-side bench cases (GEOV), run by geo.sh:
   1 clear-heavy: glClear(colour|depth) then 4 flat depth-tested triangles
     (built at 320x240 and 640x400: the frame is almost all clear)
   3 indexed lit mesh: a 32x24 UV sphere (792 vertices, 1,536 triangles)
     through glDrawElements(GL_TRIANGLES, GL_UNSIGNED_SHORT), vertex and
     normal arrays, one directional light, smooth; each vertex is shared
     by up to 6 triangles (the post-transform vertex cache case)
   4 rotate-heavy: 256 glRotatef about arbitrary axes and 256 about z, each
     in its own glPushMatrix/glPopMatrix, no drawing but one triangle
   5 mech-like: 200 x (push, rotate about an arbitrary axis, one lit
     triangle, pop): the modelview inverse is needed at every glBegin
   6 specular, two lights: the sphere in immediate mode (glNormal3fv +
     glVertex3fv per vertex), material specular 1 shininess 32, LIGHT0
     directional, LIGHT1 positional with attenuation and a spot
   8 strip assembly (a correctness probe for the copy-free strips, G14):
     GL_LINE_STRIP, GL_LINE_LOOP, GL_TRIANGLE_FAN and GL_QUAD_STRIP with a
     colour per vertex, drawn GL_FLAT (the provoking vertex decides) and
     GL_SMOOTH, 2D, no depth; its frame must match the copying build's
   7 colour material: the sphere in immediate mode with glColor3fv per
     vertex under glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE),
     one light (the material changes at every vertex)
   9 full-coverage clear, far (review 3a m5): glClear(colour|depth), then one
     screen-filling flat quad at d ~0.994 (every row is drawn: the dirty
     rows save nothing; the depth epochs have room)
  10 full-coverage clear, near: the same quad at d ~0.1 (no epoch room: only
     the clear loop itself can help)
  11 approaching camera (review 3a M4): geo3's lit sphere whose distance
     swings 18 -> 3 -> 18 eye units over 20 frames, so depth epochs run out
     of room on the way in (demotions: a whole-buffer read-modify-write) and
     start again on the way out; built with 60 counted frames, and the
     "frames" line gives the dearest one
  12 far-plane background (review 3a M4): a screen-filling quad at
     glDepthRange(1, 1) under GL_LESS behind geo3's sphere - with stale
     pixels about, each epoch's first such fragment materialises the buffer
     (a whole-buffer read-modify-write); 60 counted frames
   s31, MIT. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <GL/gl.h>
#include "ui.h"

#define SL 32
#define ST 24
#define NV ((SL + 1) * (ST + 1))
static float pos[NV][3], nrm[NV][3];
static unsigned short idx[SL * ST * 6];
static int nidx;
static float spin;

static void mesh(void)
{
	int i, j, k = 0;
	for (j = 0; j <= ST; j++)
		for (i = 0; i <= SL; i++) {
			/* sin/cos in float from a table-free recurrence would be
			   app code; here it is init only, so libm is fine */
			float th = (float)j / ST * 3.14159265f, ph = (float)i / SL * 6.2831853f;
			float x = sinf(th) * cosf(ph), y = cosf(th), z = sinf(th) * sinf(ph);
			pos[k][0] = 1.6f * x; pos[k][1] = 1.6f * y; pos[k][2] = 1.6f * z;
			nrm[k][0] = x; nrm[k][1] = y; nrm[k][2] = z;
			k++;
		}
	for (j = 0; j < ST; j++)
		for (i = 0; i < SL; i++) {
			int a = j * (SL + 1) + i, b = a + SL + 1;
			idx[nidx++] = a; idx[nidx++] = b; idx[nidx++] = a + 1;
			idx[nidx++] = a + 1; idx[nidx++] = b; idx[nidx++] = b + 1;
		}
}

void init(void)
{
	static const float lp[4] = { 5, 5, 10, 0 }, red[4] = { 0.8f, 0.1f, 0.0f, 1 };
	if (GEOV == 8) return;
	glEnable(GL_DEPTH_TEST);
	if (GEOV == 1 || GEOV == 9 || GEOV == 10) return;
	mesh();
	glLightfv(GL_LIGHT0, GL_POSITION, lp);
	glEnable(GL_LIGHT0);
	glEnable(GL_LIGHTING);
	glEnable(GL_CULL_FACE);
	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
	if (GEOV == 6) {
		static const float w[4] = { 1, 1, 1, 1 }, l1p[4] = { -2, 3, 2, 1 },
			l1d[4] = { 0.6f, 0.6f, 1.0f, 1 }, sd[3] = { 0.5f, -0.8f, -0.4f };
		glMaterialfv(GL_FRONT, GL_SPECULAR, w);
		glMaterialf(GL_FRONT, GL_SHININESS, 32);
		glLightfv(GL_LIGHT1, GL_POSITION, l1p);
		glLightfv(GL_LIGHT1, GL_DIFFUSE, l1d);
		glLightfv(GL_LIGHT1, GL_SPECULAR, l1d);
		glLightf(GL_LIGHT1, GL_LINEAR_ATTENUATION, 0.1f);
		glLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, sd);
		glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, 60);
		glLightf(GL_LIGHT1, GL_SPOT_EXPONENT, 4);
		glEnable(GL_LIGHT1);
	}
	if (GEOV == 7) {
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glEnable(GL_COLOR_MATERIAL);
	}
	if (GEOV == 3 || GEOV == 11 || GEOV == 12) {
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_NORMAL_ARRAY);
		glVertexPointer(3, GL_FLOAT, 0, pos);
		glNormalPointer(GL_FLOAT, 0, nrm);
	}
}

static float aspect = 0.75f;
void reshape(int w, int h)
{
	aspect = (float)h / w;
	glViewport(0, 0, w, h);
	if (GEOV == 8) {
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, w, 0, h, -1, 1);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();
		return;
	}
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glFrustum(-1, 1, -(float)h / w, (float)h / w, 2, 20);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glTranslatef(0, 0, -6);
}

void draw(void)
{
	int i;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	switch (GEOV) {
	case 1:
		glBegin(GL_TRIANGLES);
		for (i = 0; i < 4; i++) {
			float x = -1.5f + i * 0.9f;
			glColor3f(0.2f * i, 0.5f, 1.0f - 0.2f * i);
			glVertex3f(x, -0.5f, -0.1f * i); glVertex3f(x + 0.8f, -0.5f, 0); glVertex3f(x + 0.4f, 0.5f, 0.1f * i);
		}
		glEnd();
		break;
	case 9: case 10: {
		/* eye space: z = -19 (d ~0.994) or -2.2 (d ~0.1), 5% past the
		   frustum's edges */
		float z = GEOV == 9 ? -19.0f : -2.2f, hx = -z * 0.5f * 1.05f, hy = hx * aspect;
		glPushMatrix();
		glLoadIdentity();
		glColor3f(0.2f + 0.01f * (spin / 3), 0.4f, 0.6f);
		glBegin(GL_QUADS);
		glVertex3f(-hx, -hy, z); glVertex3f(hx, -hy, z); glVertex3f(hx, hy, z); glVertex3f(-hx, hy, z);
		glEnd();
		glPopMatrix();
		break;
	}
	case 12: {
		float hx = 20.0f * 0.5f * 1.05f, hy = hx * aspect;
		glPushMatrix();
		glLoadIdentity();
		glDisable(GL_LIGHTING);
		glColor3f(0.1f, 0.1f, 0.3f);
		glDepthRange(1, 1);
		glBegin(GL_QUADS);
		glVertex3f(-hx, -hy, -19.0f); glVertex3f(hx, -hy, -19.0f); glVertex3f(hx, hy, -19.0f); glVertex3f(-hx, hy, -19.0f);
		glEnd();
		glDepthRange(0, 1);
		glEnable(GL_LIGHTING);
		glPopMatrix();
	}
		/* fall through: the sphere in front */
	case 11:
		glPushMatrix();
		if (GEOV == 11) {
			/* spin advances 3 a frame: a 20-frame period */
			float dz = 10.5f + 7.5f * cosf(spin * (6.2831853f / 60.0f));
			glTranslatef(0, 0, 6.0f - dz);
		}
		glRotatef(spin, 0.3f, 1, 0.1f);
		glDrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_SHORT, idx);
		glPopMatrix();
		break;
	case 3:
		glPushMatrix();
		glRotatef(spin, 0.3f, 1, 0.1f);
		glDrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_SHORT, idx);
		glPopMatrix();
		break;
	case 4:
		for (i = 0; i < 256; i++) {
			glPushMatrix();
			glRotatef(spin + i * 1.37f, 0.3f + i * 0.01f, 1, -0.2f);
			glPopMatrix();
			glPushMatrix();
			glRotatef(spin - i * 2.11f, 0, 0, 1);
			glPopMatrix();
		}
		glBegin(GL_TRIANGLES);
		glVertex3f(0, 0, 0); glVertex3f(0.1f, 0, 0); glVertex3f(0, 0.1f, 0);
		glEnd();
		break;
	case 5:
		for (i = 0; i < 200; i++) {
			float x = -2.0f + (i % 20) * 0.2f, y = -1.5f + (i / 20) * 0.3f;
			glPushMatrix();
			glTranslatef(x, y, 0);
			glRotatef(spin + i * 7.0f, 1, 0.5f, 0.25f);
			glBegin(GL_TRIANGLES);
			glNormal3f(0, 0, 1);
			glVertex3f(0, 0, 0); glVertex3f(0.15f, 0, 0); glVertex3f(0, 0.2f, 0);
			glEnd();
			glPopMatrix();
		}
		break;
	case 8: {
		static const GLenum modes[4] = { GL_LINE_STRIP, GL_LINE_LOOP, GL_TRIANGLE_FAN, GL_QUAD_STRIP };
		int k, sm, j;
		for (sm = 0; sm < 2; sm++) {
			glShadeModel(sm ? GL_SMOOTH : GL_FLAT);
			for (k = 0; k < 4; k++) {
				float ox = 10 + k * 76, oy = 10 + sm * 115;
				glBegin(modes[k]);
				for (j = 0; j < 9; j++) {
					float a = j * 0.7f + spin * 0.01f, r = (j & 1) ? 30 : 18;
					glColor3f((j * 37 % 11) / 10.0f, (j * 53 % 7) / 6.0f, (j & 3) / 3.0f);
					if (modes[k] == GL_QUAD_STRIP)
						glVertex2f(ox + j * 8, oy + ((j & 1) ? 90 : 20) + 6 * sinf(a));
					else
						glVertex2f(ox + 35 + r * cosf(a), oy + 50 + r * sinf(a));
				}
				glEnd();
			}
		}
		break;
	}
	case 7:
		glPushMatrix();
		glRotatef(spin, 0.3f, 1, 0.1f);
		glBegin(GL_TRIANGLES);
		for (i = 0; i < nidx; i++) {
			const float *n = nrm[idx[i]];
			glColor3f(0.5f + 0.5f * n[0], 0.5f + 0.5f * n[1], 0.5f);
			glNormal3fv(n);
			glVertex3fv(pos[idx[i]]);
		}
		glEnd();
		glPopMatrix();
		break;
	case 6:
		glPushMatrix();
		glRotatef(spin, 0.3f, 1, 0.1f);
		glBegin(GL_TRIANGLES);
		for (i = 0; i < nidx; i++) {
			glNormal3fv(nrm[idx[i]]);
			glVertex3fv(pos[idx[i]]);
		}
		glEnd();
		glPopMatrix();
		break;
	}
}
void idle(void) { spin += 3.0f; draw(); }
GLenum key(int k) { (void)k; return GL_FALSE; }
int main(int argc, char **argv)
{
	static char name[8];
	snprintf(name, sizeof name, "geo%d", GEOV);
	return ui_loop(argc, argv, name);
}
