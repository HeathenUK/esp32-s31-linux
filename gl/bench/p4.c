/* p4.c - phase 4 feature costs (gl/bench/p4.sh), 320x240 ortho, every
   frame starting with a glClear:
   P4V 1 stencil write: prim4's 200 quads (10x20 px) with GL_ALWAYS /
         GL_REPLACE (a stencil stage that writes, colour on)
   P4V 2 stencil test: a half-screen stencil mask, then the 200 quads under
         GL_EQUAL (the read-only stage)
   P4V 3 stencil clear: glClear of colour + depth + stencil and 4 small
         stencil-writing triangles (the dirty range: the next clear writes
         only what they touched)
   P4V 4 blend GL_FUNC_REVERSE_SUBTRACT, ONE,ONE: prim5's 200 quads, over
         a grey clear (so the frame hash sees the equation)
   P4V 5 blend GL_MAX: the same
   P4V 6 GL_LINE_SMOOTH width 1, SRC_ALPHA,ONE (rRootage): prim2's 200 lines
   P4V 7 the same at width 3
   P4V 8 GL_POINT_SMOOTH size 4, SRC_ALPHA,ONE: 200 points
   Compare 1 and 2 with prim4, 3 with prim0, 4 and 5 with prim5, 6 and 7
   with prim2 (aliased blended lines) and prim3 (width 2). s31, MIT. */
#include <stdlib.h>
#include <GL/gl.h>
#include "ui.h"
#include "s31gl.h"
void init(void)
{
	if (P4V <= 3) s31gl_set_stencil_bits(s31gl_get_current(), 8);
	/* 4, 5: a grey background, so that REVERSE_SUBTRACT (D - S) and MAX
	   give frames of their own (review 4 R5-p4bench: over black, MAX
	   drew p4v1's frame and REVERSE_SUBTRACT an all-black one) */
	if (P4V == 4 || P4V == 5) glClearColor(0.55f, 0.45f, 0.6f, 1);
}
void reshape(int w, int h)
{
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glOrtho(0, w, 0, h, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}
static void quads(void)
{
	int i;
	glBegin(GL_QUADS);
	for (i = 0; i < 200; i++) { float fx = 10 + (i % 25) * 12, fy = 10 + (i / 25) * 25;
		glVertex2f(fx, fy); glVertex2f(fx + 10, fy); glVertex2f(fx + 10, fy + 20); glVertex2f(fx, fy + 20); }
	glEnd();
}
void draw(void)
{
	int i;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | (P4V <= 3 ? GL_STENCIL_BUFFER_BIT : 0));
	glColor4f(0.3f, 0.2f, 0.1f, 0.8f);
	switch (P4V) {
	case 1:
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_ALWAYS, 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		quads();
		glDisable(GL_STENCIL_TEST);
		break;
	case 2:
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_ALWAYS, 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glBegin(GL_QUADS);
		glVertex2f(0, 0); glVertex2f(160, 0); glVertex2f(160, 240); glVertex2f(0, 240);
		glEnd();
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glStencilFunc(GL_EQUAL, 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		quads();
		glDisable(GL_STENCIL_TEST);
		break;
	case 3:
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_ALWAYS, 1, 0xff);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		glBegin(GL_TRIANGLES);
		for (i = 0; i < 4; i++) {
			float fx = 30 + i * 70;
			glVertex2f(fx, 100); glVertex2f(fx + 30, 100); glVertex2f(fx + 15, 130);
		}
		glEnd();
		glDisable(GL_STENCIL_TEST);
		break;
	case 4: case 5:
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glBlendEquation(P4V == 4 ? GL_FUNC_REVERSE_SUBTRACT : GL_MAX);
		quads();
		glBlendEquation(GL_FUNC_ADD);
		glDisable(GL_BLEND);
		break;
	case 6: case 7:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glEnable(GL_LINE_SMOOTH);
		glLineWidth(P4V == 7 ? 3 : 1);
		glBegin(GL_LINES);
		for (i = 0; i < 200; i++) {
			glVertex2f(60 + (i % 40), 20 + i); glVertex2f(260 + (i % 40), 20 + i * 0.5f);
		}
		glEnd();
		glLineWidth(1);
		glDisable(GL_LINE_SMOOTH);
		glDisable(GL_BLEND);
		break;
	case 8:
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		glEnable(GL_POINT_SMOOTH);
		glPointSize(4);
		glBegin(GL_POINTS);
		for (i = 0; i < 200; i++) glVertex2f(10 + (i % 25) * 12.3f, 10 + (i / 25) * 25.7f);
		glEnd();
		glPointSize(1);
		glDisable(GL_POINT_SMOOTH);
		glDisable(GL_BLEND);
		break;
	}
}
void idle(void) { draw(); }
GLenum key(int k) { (void)k; return GL_FALSE; }
int main(int argc, char **argv)
{
	static char name[] = "p4v0";
	name[3] = (char)('0' + P4V);
	return ui_loop(argc, argv, name);
}
