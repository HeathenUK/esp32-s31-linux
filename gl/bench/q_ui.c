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
 * Then one "frames" line: the cheapest and the dearest counted frame
 * (review 3a M4: the tail, not only the mean).
 *
 * The depth buffer is the bench's own (s31gl_bind_depth, calloc'd, as the
 * GLX layer gives every drawable one; review 3a M3): q_bufs holds the
 * colour and depth addresses for gl/bench/qemu/memclass.c, which counts the
 * loads and stores per buffer, stack and the rest. -DQ_NODEPTH keeps the
 * core's private depth buffer.
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

#ifndef Q_NAME_SUFFIX
#define Q_NAME_SUFFIX ""
#endif

/* prof.py counts only between the two calls of this (the counted frames) */
__attribute__((noinline)) void q_mark(void) { __asm__ volatile(""); }
/* the end of each counted frame (memclass.c: per-frame memory traffic) */
__attribute__((noinline)) void q_fmark(void) { __asm__ volatile(""); }
/* what memclass.c classifies by: colour buffers 0 and 1, depth */
struct { unsigned int n, addr[3], len[3]; } q_bufs;

int ui_loop(int argc, char **argv, const char *name)
{
	int w = QW, h = QH, i;
	unsigned short *fb = calloc((size_t)w * h, 2);
#if defined(Q_BUFS) && Q_BUFS == 2
	/* phase 3a: two colour buffers bound in turn after every frame, as
	   GLX's two SHM segments are for a drawable of 200 kB or less
	   (gl/glx/glx_present.c want_bufs) */
	unsigned short *fbs[2] = { fb, calloc((size_t)w * h, 2) };
	int cur = 0;
#define Q_SWAP() do { cur ^= 1; fb = fbs[cur]; s31gl_bind_color(c, fb, w, h, w * 2); } while (0)
#else
#define Q_SWAP() do { } while (0)
#endif
	s31gl_ctx *c = s31gl_create_context(NULL);
	unsigned long d0;
	unsigned long long t0, tt, ft[QN], fmin = ~0ull, fmax = 0, fp;
	int imax = 0;
	unsigned int fnv = 2166136261u;
	char path[64];
	FILE *f;

	(void)argc; (void)argv;
	s31gl_make_current(c);
	s31gl_bind_color(c, fb, w, h, w * 2);
	/* the buffer is the bench's alone, as a GLX drawable's is (phase 3a
	   dirty boxes; Q_NORETAIN=1 measures without them) */
#ifndef Q_NORETAIN
	{
		/* weak: an older tree (gl/bench/base*) has no such call */
		extern void s31gl_set_retained(s31gl_ctx *, int) __attribute__((weak));
		if (s31gl_set_retained) s31gl_set_retained(c, 1);
	}
#endif
#ifndef Q_NODEPTH
	{
		/* + 64: the depth-epoch tail (S31GL_DEPTH_TAIL, 20 in phase 3a;
		   older trees ignore it) */
		void *zd = calloc((size_t)w * h * 2 + 64, 1);
		s31gl_bind_depth(c, zd);
		q_bufs.addr[2] = (unsigned int)zd;
		q_bufs.len[2] = (unsigned int)w * h * 2 + 64;
	}
#endif
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
	q_bufs.addr[0] = (unsigned int)fb;
	q_bufs.len[0] = (unsigned int)w * h * 2;
#if defined(Q_BUFS) && Q_BUFS == 2
	q_bufs.addr[1] = (unsigned int)fbs[1];
	q_bufs.len[1] = (unsigned int)w * h * 2;
#endif
	q_bufs.n = 3;
	for (i = 0; i < QWARM; i++) {
		idle();
		s31gl_frame_end(c);
		Q_SWAP();
	}
	d0 = dcalls;
	{ extern void dc_mark(void); dc_mark(); }
	q_mark();
	t0 = rdinstret();
	for (i = 0; i < QN; i++) {
		idle();
		s31gl_frame_end(c);
		if (i + 1 < QN) Q_SWAP();   /* the hash below reads the last frame */
		ft[i] = rdinstret();
		q_fmark();
	}
	tt = rdinstret() - t0;
	q_mark();
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
	printf("%s%s %dx%d: %.4f Minsn/frame, %.0f soft-double calls/frame, fb %08x\n",
	       name, Q_NAME_SUFFIX, w, h, tt / (double)QN / 1e6, (dcalls - d0) / (double)QN, fnv);
#ifdef QFEAT
	snprintf(path, sizeof path, "feat%d_%s_%dx%d.raw", QFEAT, name, w, h);
#else
	snprintf(path, sizeof path, "%s%s_%dx%d.raw", name, Q_NAME_SUFFIX, w, h);
#endif
	/* the soft-double breakdown, one line (bench.sh keeps it in dcalls.txt) */
	{ extern void dump_dcalls_since(const char *, int, int, int);
#ifdef QFEAT
	  char fn[32]; snprintf(fn, sizeof fn, "feat%d %s", QFEAT, name);
	  dump_dcalls_since(fn, w, h, QN);
#else
	  dump_dcalls_since(name, w, h, QN);
#endif
	}
	for (i = 0, fp = t0; i < QN; i++) {
		unsigned long long d = ft[i] - fp;
		fp = ft[i];
		if (d < fmin) fmin = d;
		if (d > fmax) { fmax = d; imax = i; }
	}
	printf("frames %s%s %dx%d: min %.4f max %.4f Mframe (frame %d of %d)\n", name, Q_NAME_SUFFIX,
	       w, h, fmin / 1e6, fmax / 1e6, imax, QN);
	f = fopen(path, "wb");
	if (f) {
		fwrite(fb, 2, (size_t)w * h, f);
		fclose(f);
	}
	return 0;
}
