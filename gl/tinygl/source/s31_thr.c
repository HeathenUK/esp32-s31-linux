/*
 * s31_thr.c - phase 6: the second rasteriser thread (S31GL_THREADS; the
 * design is in s31_thr.h, the measurements in artifacts/gl/phase6/
 * THREADS.txt). s31, MIT.
 *
 * The worker's path is PIE-free by construction (it may run on Linux CPU1,
 * hart 0, which has no PIE: a trap there bounces the task): no libc mem* /
 * str* call, every copy an s31_wcopy word loop, no allocation (its tables
 * are allocated by the application's thread when the worker is created),
 * and its only system call is futex. gl/tests/thrchk-pie.sh checks the
 * objects it reaches.
 */
#include "zgl.h"
#include "zpipe.h"
#include "ztri.h"
#include "s31_thr.h"

#if defined(__linux__) && !defined(S31GL_NO_THREADS)
#define S31T_PTHREAD 1
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#endif

int s31t_mode = -1;                  /* -1: not read yet */
static int t_band = 16, t_sk = 1, t_sn = 2, t_ring_kb = 64, t_spin = 4000,
           t_check;

enum { R_PAD = 1, R_PIPE, R_TRI, R_QUIT };

typedef struct {
  unsigned int type, size;           /* size in bytes, a multiple of 8 */
} RecH;

typedef struct {
  RecH h;
  ZTri T;
  ZVtxG v[3];
  unsigned char flat[4], fspec[4];
  int mt, pad;
} RecTri;

typedef struct {
  RecH h;
  const void *mp, *mx, *mzb;         /* main's structures: relocation */
  int pad;
  ZBuffer zb;
  ZPipe p;
  ZPipeX x;
} RecPipe;

#define RSZ(T) ((unsigned int)((sizeof(T) + 7) & ~7u))

typedef struct S31Thr {
  S31ThrHot hot;                     /* first: GLContext.pipe.thr points here */
  GLContext *c;
  int mode;
  unsigned char *ring;
  unsigned int rsize;
  /* free-running byte counts: head written by main, tail by the worker */
  volatile unsigned int head, tail;
  volatile int wsleep, msleep;
  volatile unsigned int mwant;       /* the tail main sleeps for */
  unsigned int sent_serial;
  int sent_valid;
  int rows;                          /* the ysize own[] was made for */
  unsigned char own[S31T_MAXROWS];
  unsigned short wcnt[S31T_MAXROWS + 1];
  /* the worker's own buffer view, pipe and tables */
  ZBuffer wzb;
  ZPipe wp;
  ZPipeX wx;
  unsigned char *w_wtab, *w_btab, *w_satab;
  int w_wtab_sh, w_btab_a, w_satab_a;
  int started;
  unsigned long long winsn;
  unsigned int st[8];
#ifdef S31T_PTHREAD
  pthread_t th;
#endif
} S31Thr;

static unsigned int g_st[8];
static unsigned long long g_winsn;

/* ------------------------------------------------------------ helpers */

#if defined(__riscv) && !defined(__linux__)
/* the bare-metal counter (gl/bench): mode 2's worker share */
static inline unsigned long long rdinsn(void)
{
  unsigned int hi, lo, h2;
  do {
    __asm__ volatile("csrr %0,minstreth" : "=r"(hi));
    __asm__ volatile("csrr %0,minstret" : "=r"(lo));
    __asm__ volatile("csrr %0,minstreth" : "=r"(h2));
  } while (hi != h2);
  return ((unsigned long long)hi << 32) | lo;
}
#else
static inline unsigned long long rdinsn(void) { return 0; }
#endif

static inline unsigned int ld_acq(volatile unsigned int *p)
{
  return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void st_rel(volatile unsigned int *p, unsigned int v)
{
  __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline void cpu_relax(void)
{
  __asm__ volatile("" ::: "memory");
}

#ifdef S31T_PTHREAD
#if defined(SYS_futex)
#define S31T_FUTEX SYS_futex
#else
#define S31T_FUTEX SYS_futex_time64     /* rv32: time64 only (no timeout used) */
#endif
static void fx_wait(volatile unsigned int *a, unsigned int v)
{
  syscall(S31T_FUTEX, (void *)a, FUTEX_WAIT_PRIVATE, v, (void *)0, (void *)0, 0);
}
static void fx_wake(volatile unsigned int *a)
{
  syscall(S31T_FUTEX, (void *)a, FUTEX_WAKE_PRIVATE, 1, (void *)0, (void *)0, 0);
}
#endif

/* the mode when S31GL_THREADS is not set: 0; a gate build may compile
   another in (S31GL_HOST_DEFS=-DS31GL_THREADS_DEFAULT=1 gl/tests/run-3a.sh
   runs the whole host suite threaded - the suite passes no environment) */
#ifndef S31GL_THREADS_DEFAULT
#define S31GL_THREADS_DEFAULT 0
#endif

static void read_env(void)
{
  const char *e = getenv("S31GL_THREADS");
  s31t_mode = e ? atoi(e) : S31GL_THREADS_DEFAULT;
  if (s31t_mode < 0 || s31t_mode > 2) s31t_mode = 0;
#ifndef S31T_PTHREAD
  if (s31t_mode == 1) s31t_mode = 2;
#endif
  if ((e = getenv("S31GL_TBAND")) && atoi(e) > 0) t_band = atoi(e);
  if ((e = getenv("S31GL_TSPLIT"))) {
    int k = 0, n = 0;
    if (sscanf(e, "%d/%d", &k, &n) == 2 && n > 0 && k >= 0 && k <= n) {
      t_sk = k; t_sn = n;
    }
  }
  if ((e = getenv("S31GL_TRING")) && atoi(e) >= 16) t_ring_kb = atoi(e);
  if ((e = getenv("S31GL_TSPIN")) && atoi(e) >= 0) t_spin = atoi(e);
  if ((e = getenv("S31GL_TCHECK"))) t_check = atoi(e);
  if (s31t_mode)
    fprintf(stderr, "libGL: S31GL_THREADS=%d (band %d rows, worker %d of %d, ring %d kB)\n",
            s31t_mode, t_band, t_sk, t_sn, t_ring_kb);
}

/* the row table for ysize rows */
static void make_rows(S31Thr *t, int ysize)
{
  int y, n = 0;
  for (y = 0; y < ysize; y++) {
    int w = ((y / t_band) % t_sn) < t_sk;
    t->own[y] = (unsigned char)w;
    t->wcnt[y] = (unsigned short)n;
    n += w;
  }
  t->wcnt[ysize] = (unsigned short)n;
  t->rows = ysize;
}

/* ------------------------------------------------------------ the worker */

#define RELOC(field, from, sz, to) do {                                      \
    const char *v_ = (const char *)(field);                                  \
    if (v_ >= (const char *)(from) && v_ < (const char *)(from) + (sz))      \
      (field) = (__typeof__(field))(const void *)((const char *)(to) + (v_ - (const char *)(from))); \
  } while (0)

static void reloc_all(S31Thr *t, const RecPipe *r)
{
  ZPipe *p = &t->wp;
  ZPipeX *x = &t->wx;
  int k;
#define RL(f) do { RELOC(f, r->mp, sizeof(ZPipe), p); RELOC(f, r->mx, sizeof(ZPipeX), x); } while (0)
  p->x = x;
  p->zb = &t->wzb;
  p->thr = NULL;
  /* (the depth epochs are main's alone; only a stale-free view is kept) */
  RELOC(t->wzb.zmaxp, r->mzb, sizeof(ZBuffer), &t->wzb);
  RL(p->tex); RL(p->talpha); RL(p->tex1); RL(p->talpha1); RL(p->idx1_px);
  RL(p->stip);
  RL(x->slot_col);
  for (k = 0; k < 2; k++) {
    RL(x->tf[k].slot); RL(x->tf[k].cur0); RL(x->tf[k].cur1);
    RL(x->tf[k].tex0); RL(x->tf[k].talpha0);
  }
#undef RL
}

/* S31GL_TCHECK=1: any word of the worker's copies still pointing into
   main's structures is a relocation this file misses (printed once) */
__attribute__((cold, noinline))
static void check_reloc(S31Thr *t, const RecPipe *r)
{
  static int said;
  const unsigned long *w;
  unsigned int i, n;
  const struct { const void *a; unsigned int n; const char *name; } part[3] = {
    { &t->wp, sizeof(ZPipe), "ZPipe" }, { &t->wx, sizeof(ZPipeX), "ZPipeX" },
    { &t->wzb, sizeof(ZBuffer), "ZBuffer" } };
  int k;
  for (k = 0; k < 3; k++) {
    w = part[k].a;
    n = part[k].n / sizeof(long);
    for (i = 0; i < n; i++) {
      const char *v = (const char *)w[i];
      if ((v >= (const char *)r->mp && v < (const char *)r->mp + sizeof(ZPipe)) ||
          (v >= (const char *)r->mx && v < (const char *)r->mx + sizeof(ZPipeX)) ||
          (v >= (const char *)r->mzb && v < (const char *)r->mzb + sizeof(ZBuffer))) {
        if (said++ < 8)
          fprintf(stderr, "libGL: S31GL_TCHECK: %s word %u points into main's copy\n",
                  part[k].name, i);
      }
    }
  }
}

static void load_pipe(S31Thr *t, const RecPipe *r)
{
  ZPipeX *x = &t->wx;
  int want_sh = r->x.wtab ? r->x.wtab_sh : -1;
  s31_wcopy(&t->wzb, &r->zb, sizeof(ZBuffer) / 4);
  s31_wcopy(&t->wp, &r->p, sizeof(ZPipe) / 4);
  s31_wcopy(&t->wx, &r->x, sizeof(ZPipeX) / 4);
  t->wzb.pipe = &t->wp;
  reloc_all(t, r);
  /* the lazily built tables are the worker's own */
  x->wtab = t->w_wtab; x->wtab_sh = t->w_wtab_sh;
  if (want_sh >= 0 && want_sh != t->w_wtab_sh) {
    zpf_world_tables(x, want_sh);
    t->w_wtab_sh = x->wtab_sh;
  }
  x->btab = t->w_btab; x->btab_a = t->w_btab_a;
  x->satab = t->w_satab; x->satab_a = t->w_satab_a;
  x->stab = NULL;                    /* stencil batches are not threaded */
  if (__builtin_expect(t_check, 0)) check_reloc(t, r);
}

static void run_tri(S31Thr *t, const RecTri *r)
{
  ZPipe *p = &t->wp;
  p->flat[0] = r->flat[0]; p->flat[1] = r->flat[1];
  p->flat[2] = r->flat[2]; p->flat[3] = r->flat[3];
  p->flatspec[0] = r->fspec[0]; p->flatspec[1] = r->fspec[1];
  p->flatspec[2] = r->fspec[2];
  p->stip_on = 1;
  if (r->mt)
    ZB_fillBodyGeneralMT(&t->wzb, &r->T, &r->v[0], &r->v[1], &r->v[2], t->own, 1);
  else
    ZB_fillBodyGeneral(&t->wzb, &r->T, &r->v[0], &r->v[1], &r->v[2], t->own, 1);
  /* the stages keep the blend tables in the worker's ZPipeX */
  t->w_btab_a = t->wx.btab_a;
  t->w_satab_a = t->wx.satab_a;
}

/* one record at tail; returns 0 at R_QUIT */
static int run_one(S31Thr *t)
{
  unsigned int tl = t->tail;
  const RecH *h = (const RecH *)(t->ring + (tl & (t->rsize - 1)));
  unsigned int sz = h->size, ty = h->type;
  if (ty == R_PIPE) load_pipe(t, (const RecPipe *)h);
  else if (ty == R_TRI) run_tri(t, (const RecTri *)h);
  st_rel(&t->tail, tl + sz);
  return ty != R_QUIT;
}

#ifdef S31T_PTHREAD
static void wake_main(S31Thr *t)
{
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (t->msleep && (int)(t->tail - t->mwant) >= 0) {
    t->msleep = 0;
    fx_wake(&t->tail);
  }
}

static void *worker(void *arg)
{
  S31Thr *t = arg;
  for (;;) {
    unsigned int h = ld_acq(&t->head);
    if (h == t->tail) {
      int i;
      for (i = 0; i < t_spin; i++) {
        cpu_relax();
        if ((h = ld_acq(&t->head)) != t->tail) break;
      }
      if (h == t->tail) {
        t->wsleep = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        h = ld_acq(&t->head);
        if (h == t->tail) fx_wait(&t->head, h);
        t->wsleep = 0;
        continue;
      }
    }
    while (t->tail != h) {
      if (!run_one(t)) { wake_main(t); return NULL; }
      wake_main(t);
    }
  }
}
#endif

/* ------------------------------------------------------------ main's side */

/* wait until the worker's tail reaches want (mode 2: run it here) */
static void wait_tail(S31Thr *t, unsigned int want)
{
  if (t->mode == 2) {
    unsigned long long i0 = rdinsn();
    while ((int)(t->tail - want) < 0) run_one(t);
    t->winsn += rdinsn() - i0;
    return;
  }
#ifdef S31T_PTHREAD
  for (;;) {
    int i;
    unsigned int tl;
    for (i = 0; i < t_spin; i++) {
      if ((int)(ld_acq(&t->tail) - want) >= 0) return;
      cpu_relax();
    }
    t->mwant = want;
    t->msleep = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    tl = ld_acq(&t->tail);
    if ((int)(tl - want) >= 0) { t->msleep = 0; return; }
    fx_wait(&t->tail, tl);
    t->msleep = 0;
  }
#endif
}

/* room for sz contiguous bytes at head (a PAD record fills the end) */
static unsigned char *reserve(S31Thr *t, unsigned int sz)
{
  unsigned int h = t->head, off = h & (t->rsize - 1), room = t->rsize - off;
  if (room < sz) {
    /* pad to the start: needs the whole tail end free */
    if (t->rsize - (h - t->tail) < room + sz) {
      t->st[6]++;
      wait_tail(t, h + room + sz - t->rsize);
    }
    ((RecH *)(t->ring + off))->type = R_PAD;
    ((RecH *)(t->ring + off))->size = room;
    h += room;
    st_rel(&t->head, h);
    off = 0;
  } else if (t->rsize - (h - ld_acq(&t->tail)) < sz) {
    t->st[6]++;
    wait_tail(t, h + sz - t->rsize);
  }
  return t->ring + off;
}

static void publish(S31Thr *t, unsigned int sz)
{
  st_rel(&t->head, t->head + sz);
  t->hot.pending = 1;
  t->st[7] += sz;
#ifdef S31T_PTHREAD
  if (t->mode == 1) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (t->wsleep) fx_wake(&t->head);
  }
#endif
}

static void put_pipe(S31Thr *t, GLContext *c)
{
  unsigned int sz = RSZ(RecPipe);
  RecPipe *r = (RecPipe *)reserve(t, sz);
  r->h.type = R_PIPE;
  r->h.size = sz;
  r->mp = &c->pipe;
  r->mx = &c->pipex;
  r->mzb = c->zb;
  s31_wcopy(&r->zb, c->zb, sizeof(ZBuffer) / 4);
  s31_wcopy(&r->p, &c->pipe, sizeof(ZPipe) / 4);
  s31_wcopy(&r->x, &c->pipex, sizeof(ZPipeX) / 4);
  publish(t, sz);
  t->sent_serial = c->pipe_serial;
  t->sent_valid = 1;
  t->st[2]++;
}

const unsigned char *s31t_tri(ZBuffer *zb, const ZTri *T, const void *a,
                              const void *b, const void *cc, int mt)
{
  S31Thr *t = (S31Thr *)zb->pipe->thr;
  GLContext *c = t->c;
  unsigned int sz;
  RecTri *r;
  int k, nw = 0, nt = 0;

  if (!t->hot.ok) {
    if (t->hot.pending) s31t_drain(c);
    t->st[4]++;
    return NULL;
  }
  if (__builtin_expect(t->rows != zb->ysize, 0)) {
    if (t->hot.pending) s31t_drain(c);
    make_rows(t, zb->ysize);
  }
  for (k = 0; k < 2; k++) {
    int ya = T->part[k].ya, yb = T->part[k].yb;
    if (ya < yb) {
      nw += t->wcnt[yb] - t->wcnt[ya];
      nt += yb - ya;
    }
  }
  if (nw == 0) return NULL;          /* main's rows only */
  if (!t->sent_valid || t->sent_serial != c->pipe_serial) put_pipe(t, c);
  sz = RSZ(RecTri);
  r = (RecTri *)reserve(t, sz);
  r->h.type = R_TRI;
  r->h.size = sz;
  s31_wcopy(&r->T, T, sizeof(ZTri) / 4);
  s31_wcopy(&r->v[0], a, sizeof(ZVtxG) / 4);
  s31_wcopy(&r->v[1], b, sizeof(ZVtxG) / 4);
  s31_wcopy(&r->v[2], cc, sizeof(ZVtxG) / 4);
  r->flat[0] = c->pipe.flat[0]; r->flat[1] = c->pipe.flat[1];
  r->flat[2] = c->pipe.flat[2]; r->flat[3] = c->pipe.flat[3];
  r->fspec[0] = c->pipe.flatspec[0]; r->fspec[1] = c->pipe.flatspec[1];
  r->fspec[2] = c->pipe.flatspec[2];
  r->mt = mt;
  publish(t, sz);
  t->st[1]++;
  if (nw == nt) { t->st[5]++; return (const unsigned char *)1; }
  return t->own;
}

void s31t_drain(GLContext *c)
{
  S31Thr *t = (S31Thr *)c->pipe.thr;
  if (t == NULL) return;
  if (t->hot.pending) {
    wait_tail(t, t->head);
    t->hot.pending = 0;
    t->st[3]++;
  }
  /* anything may change before the next threaded triangle: send the pipe
     and the buffer view again */
  t->sent_valid = 0;
}

static S31Thr *thr_new(GLContext *c)
{
  S31Thr *t = gl_zalloc(sizeof(S31Thr));
  if (t == NULL) return NULL;
  t->c = c;
  t->mode = s31t_mode;
  t->rsize = 1024;
  while (t->rsize < (unsigned int)t_ring_kb * 1024u) t->rsize <<= 1;
  t->ring = gl_malloc(t->rsize);
  t->w_wtab = gl_malloc(1024 + 4096);
  t->w_btab = gl_malloc(192);
  t->w_satab = gl_malloc(256);
  if (!t->ring || !t->w_wtab || !t->w_btab || !t->w_satab) {
    gl_free(t->ring); gl_free(t->w_wtab); gl_free(t->w_btab); gl_free(t->w_satab);
    gl_free(t);
    return NULL;
  }
  t->w_wtab_sh = t->w_btab_a = t->w_satab_a = -1;
  t->hot.own = t->own;
  t->hot.wcnt = t->wcnt;
  t->rows = -1;
#ifdef S31T_PTHREAD
  if (t->mode == 1) {
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&t->th, &at, worker, t) == 0) t->started = 1;
    else t->mode = 2;                /* no thread: deferred on this one */
    pthread_attr_destroy(&at);
  }
#endif
  return t;
}

void s31t_pipe_built(GLContext *c)
{
  S31Thr *t;
  if (s31t_mode < 0) read_env();
  if (!s31t_mode) return;
  if (c->pipe.thr == NULL) {
    t = thr_new(c);
    if (t == NULL) { s31t_mode = 0; return; }
    c->pipe.thr = &t->hot;
  }
  t = (S31Thr *)c->pipe.thr;
  /* stencil and polygon stipple batches draw on main (their tables and
     per-buffer state are not copied); so does a buffer taller than own[] */
  t->hot.ok = c->raster_general && !RASTER_STENCIL(c) && !c->poly_stipple_enabled &&
              c->zb->ysize > 0 && c->zb->ysize <= S31T_MAXROWS;
}

void s31t_free(GLContext *c)
{
  S31Thr *t = (S31Thr *)c->pipe.thr;
  if (t == NULL) return;
  s31t_drain(c);
#ifdef S31T_PTHREAD
  if (t->started) {
    RecH *h = (RecH *)reserve(t, 8);
    h->type = R_QUIT;
    h->size = 8;
    publish(t, 8);
    pthread_join(t->th, NULL);
  }
#endif
  c->pipe.thr = NULL;
  gl_free(t->ring); gl_free(t->w_wtab); gl_free(t->w_btab); gl_free(t->w_satab);
  gl_free(t);
}

/* per-context counts summed into one set (the bench has one context) */
void tgl_thr_stats(unsigned int out[8])
{
  GLContext *c = gl_get_context();
  int k;
  if (c && c->pipe.thr) {
    S31Thr *t = (S31Thr *)c->pipe.thr;
    for (k = 0; k < 8; k++) { g_st[k] += t->st[k]; t->st[k] = 0; }
    g_winsn += t->winsn;
    t->winsn = 0;
  }
  g_st[0] = (unsigned int)g_winsn;
  for (k = 0; k < 8; k++) { out[k] = g_st[k]; g_st[k] = 0; }
  g_winsn = 0;
}
