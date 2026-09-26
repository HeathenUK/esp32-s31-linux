/*
 * s31_thr.h - phase 6: the second rasteriser thread (S31GL_THREADS). s31, MIT.
 *
 * The screen is split into row bands; every row belongs to exactly one of
 * two threads (S31Thr.own[y]: 0 the application's thread, "main", 1 the
 * worker). A general-path triangle (ztriangle_gen.c) is set up once, by
 * main - ztri_setup, the depth epoch and the dirty box, which are the only
 * parts that write shared state - and, when it covers a worker row, queued
 * in a single-producer single-consumer ring with its ZTri and vertices.
 * Both threads then run the SAME function (ZB_fillBodyGeneral /
 * ZB_fillBodyGeneralMT, one out-of-line copy each) on their own rows, main
 * at once and the worker when it reaches the record, so every pixel is
 * computed by the same instructions from the same inputs as with one thread
 * and is written by one thread only: bit-identical, no locks on the
 * buffers, and a pixel's writes stay in API order (only one thread ever
 * touches it, in queue order).
 *
 * The stages read and write the pipe (ZPipe, ZPipeX: per-triangle texel
 * stage and level choices, scratch texels, lazily built tables), so the
 * worker runs on its own copy. Main sends one (a PIPE record) whenever its
 * pipe was rebuilt (GLContext.pipe_serial) or after any sync; the pointers
 * into the copied structures are relocated, the lazily built tables are
 * the worker's own (rebuilt on key mismatch) and the stencil table is
 * carried in the record. Stencil and polygon-stipple batches are not
 * threaded (they sync and draw every row on main).
 *
 * Everything else that reads or writes the colour, depth or stencil
 * buffers, or texel storage the worker may be reading, first calls
 * S31T_SYNC (drain the ring and wait for the worker): tier-1 fillers,
 * lines, points, clears, the pixel paths, readbacks, the depth-epoch
 * rewrites, texture uploads / copies / deletes, buffer binds, make-current,
 * the frame hook, glFlush / glFinish / glXSwapBuffers, context destroy.
 *
 * Modes (S31GL_THREADS, read once): 0 one thread (default: nothing here
 * runs; the fillers pay one load and branch per triangle), 1 two threads
 * (Linux: a pthread; elsewhere mode 2), 2 deferred: the worker's records
 * are run on the calling thread at the next sync. Mode 2 is the validation
 * arm - a sync point that is missing shows as a wrong frame every time,
 * not as a race - and on the bare-metal RV32 counter it measures the work
 * split exactly (S31Thr.winsn: the worker's instructions).
 *   S31GL_TBAND    rows per band (default 16)
 *   S31GL_TSPLIT   k/n: the worker owns k of every n bands (default 1/2)
 *   S31GL_TRING    ring size in kB (default 64)
 */
#ifndef S31_THR_H
#define S31_THR_H

#include "zbuffer.h"

struct GLContext;
struct S31Thr;
struct ZTri;

/* the most rows a threaded buffer may have (a taller one draws on main) */
#define S31T_MAXROWS 1024

/* the part of S31Thr the fillers read (s31_thr.c has the rest) */
typedef struct S31ThrHot {
  int ok;                      /* this batch is threaded */
  int pending;                 /* records queued since the last sync */
  const unsigned char *own;    /* own[y]: 1 the worker's row */
  const unsigned short *wcnt;  /* worker rows below y (prefix counts) */
} S31ThrHot;

/* S31GL_THREADS (0 until the first context reads it) */
extern int s31t_mode;

/* the context's worker is idle and its queue empty afterwards */
void s31t_drain(struct GLContext *c);
#define S31T_SYNC(c) do { if (__builtin_expect((c)->pipe.thr != 0, 0) &&     \
                              (c)->pipe.thr->pending) s31t_drain(c); } while (0)

/* gl_build_pipe's last step: is the batch threadable (creates the worker
   at the first one) */
void s31t_pipe_built(struct GLContext *c);
/* the general filler (ztriangle_gen.c), after ztri_setup: queue T for the
   worker when it covers a worker row. Returns the row table main filters
   its spans with, NULL for every row, or (void *)1 when main owns none */
const unsigned char *s31t_tri(ZBuffer *zb, const struct ZTri *T, const void *a,
                              const void *b, const void *cc, int mt);
/* context destroy: sync, stop and free the worker */
void s31t_free(struct GLContext *c);
/* statistics (gl/bench q_replay: per frame), then zeroed:
   out[0] worker instructions (mode 2 on the RV32 counter, else 0),
   [1] triangles queued, [2] pipe records, [3] syncs, [4] main-only
   triangles (not threadable), [5] triangles main skipped entirely,
   [6] ring-full waits, [7] ring bytes written */
void tgl_thr_stats(unsigned int out[8]);

#endif
