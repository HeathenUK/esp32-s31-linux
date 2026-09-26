/*
 * q_ui.c - the ui_loop() of gl/tinygl/examples/ui.h for the qemu
 * instruction-count bench: our libGL objects (the core of libGL.so.1, no
 * GLX) render into a calloc'd RGB565 buffer bound with s31gl_bind_color,
 * exactly as the GLX layer binds its MIT-SHM segment. s31, MIT.
 *
 * Prints one line per run:
 *   <demo> <W>x<H>: <M instructions>/frame, <soft-double calls>/frame, fb <fnv32>
 * Frames: QWARM (3) warm-up frames, then QN (10) counted ones. minstret is
 * exact under qemu -icount shift=0 (one instruction = one count).
 * The last frame is written to <demo>_<W>x<H>.raw (RGB565, semihosting).
 */
#include <stdio.h>
#include <stdlib.h>
#include <GL/gl.h>
#include "s31gl.h"
#include "ui.h"

#ifndef QWARM
#define QWARM 3
#endif
#ifndef QN
#define QN 10
#endif

extern unsigned long dcalls;
void dump_dcalls(void);

static inline unsigned long long rdinstret(void)
{
	unsigned lo, hi, h2;
	do {
		__asm__ volatile("csrr %0,minstreth" : "=r"(hi));
		__asm__ volatile("csrr %0,minstret" : "=r"(lo));
		__asm__ volatile("csrr %0,minstreth" : "=r"(h2));
	} while (hi != h2);
	return ((unsigned long long)hi << 32) | lo;
}

void swap_buffers(void) {}

int ui_loop(int argc, char **argv, const char *name)
{
	int w = QW, h = QH, i;
	unsigned short *fb = calloc((size_t)w * h, 2);
	s31gl_ctx *c = s31gl_create_context(NULL);
	unsigned long d0;
	unsigned long long t0, tt;
	unsigned int fnv = 2166136261u;
	char path[64];
	FILE *f;

	(void)argc; (void)argv;
	s31gl_make_current(c);
	s31gl_bind_color(c, fb, w, h, w * 2);
	init();
	reshape(w, h);
#ifdef QFEAT
	/* feat.sh: one added feature, set after the demo's own init; the
	   demos' draw() never touches these, so it holds for every frame */
	switch (QFEAT) {
	case 1: glEnable(GL_FOG); glFogi(GL_FOG_MODE, GL_LINEAR);
		glFogf(GL_FOG_START, 5); glFogf(GL_FOG_END, 60); break;
	case 2: glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); break;
	case 3: glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.1f); break;
	case 4: glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE); break;
	case 5: glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
	}
#endif
	for (i = 0; i < QWARM; i++) {
		idle();
		s31gl_frame_end(c);
	}
	d0 = dcalls;
	t0 = rdinstret();
	for (i = 0; i < QN; i++) {
		idle();
		s31gl_frame_end(c);
	}
	tt = rdinstret() - t0;
#ifdef ZP_COUNT
	{ extern unsigned long zp_spans, zp_chunks, zp_px;
	  printf("zp: %lu spans %lu chunks %lu px per %d frames (incl. warm-up)\n", zp_spans, zp_chunks, zp_px, QN + QWARM); }
#endif
	for (i = 0; i < w * h; i++) {
		fnv ^= fb[i];
		fnv *= 16777619u;
	}
#ifdef QFEAT
	printf("feat%d ", QFEAT);
#endif
	printf("%s %dx%d: %.4f Minsn/frame, %.0f soft-double calls/frame, fb %08x\n",
	       name, w, h, tt / (double)QN / 1e6, (dcalls - d0) / (double)QN, fnv);
#ifdef QFEAT
	snprintf(path, sizeof path, "feat%d_%s_%dx%d.raw", QFEAT, name, w, h);
#else
	snprintf(path, sizeof path, "%s_%dx%d.raw", name, w, h);
#endif
	f = fopen(path, "wb");
	if (f) {
		fwrite(fb, 2, (size_t)w * h, f);
		fclose(f);
	}
	return 0;
}
