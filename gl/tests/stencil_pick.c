/* stencil_pick.c - phase 4 F8: which config the stock toolkits get from
 * our GLX, with and without a stencil request. Built three ways by
 * gl/tests/run-stencil-pick.sh (in s31-glref): SDL2 (SDL_GL_STENCIL_SIZE),
 * SDL 1.2 (the rig's sdl12-compat) and freeglut (GLUT_STENCIL). Each run
 * prints "PICK <toolkit> asked=<0|8> GL_STENCIL_BITS=<n> attr=<n>". The
 * rule: asked 0 -> 0 (no stencil memory), asked 8 -> 8. s31, MIT. */
#include <stdio.h>
#include <stdlib.h>
#include <GL/gl.h>
#if defined(PICK_SDL2)
#include <SDL.h>
int main(int argc, char **argv)
{
	int want = argc > 1 ? atoi(argv[1]) : 0, got = -1;
	GLint bits = -1;
	SDL_Window *w;
	SDL_GLContext ctx;
	if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	if (want) SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, want);
	w = SDL_CreateWindow("p", 0, 0, 64, 48, SDL_WINDOW_OPENGL);
	if (!w) { printf("PICK sdl2 asked=%d FAILED %s\n", want, SDL_GetError()); return 1; }
	ctx = SDL_GL_CreateContext(w);
	if (!ctx) { printf("PICK sdl2 asked=%d FAILED %s\n", want, SDL_GetError()); return 1; }
	glGetIntegerv(GL_STENCIL_BITS, &bits);
	SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &got);
	printf("PICK sdl2 asked=%d GL_STENCIL_BITS=%d attr=%d\n", want, bits, got);
	SDL_Quit();
	return 0;
}
#elif defined(PICK_SDL12)
#include <SDL.h>
int main(int argc, char **argv)
{
	int want = argc > 1 ? atoi(argv[1]) : 0, got = -1;
	GLint bits = -1;
	if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	if (want) SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, want);
	if (!SDL_SetVideoMode(64, 48, 16, SDL_OPENGL)) {
		printf("PICK sdl12 asked=%d FAILED %s\n", want, SDL_GetError());
		return 1;
	}
	glGetIntegerv(GL_STENCIL_BITS, &bits);
	SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &got);
	printf("PICK sdl12 asked=%d GL_STENCIL_BITS=%d attr=%d\n", want, bits, got);
	SDL_Quit();
	return 0;
}
#else
#include <GL/freeglut.h>
static int want;
static void disp(void)
{
	GLint bits = -1;
	glGetIntegerv(GL_STENCIL_BITS, &bits);
	printf("PICK freeglut asked=%d GL_STENCIL_BITS=%d attr=%d\n", want, bits,
	       glutGet(GLUT_WINDOW_STENCIL_SIZE));
	exit(0);
}
int main(int argc, char **argv)
{
	want = argc > 1 ? atoi(argv[1]) : 0;
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH | (want ? GLUT_STENCIL : 0));
	glutInitWindowSize(64, 48);
	glutCreateWindow("p");
	glutDisplayFunc(disp);
	glutMainLoop();
	return 0;
}
#endif
