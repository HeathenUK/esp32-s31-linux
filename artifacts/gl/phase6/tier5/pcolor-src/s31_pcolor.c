/*
 * s31_pcolor.c - phase 6 tier 5: page colouring of libGL's hot small
 * state (S31GL_PCOLOR; the design is in s31_pcolor.h). s31, MIT.
 */
#define _GNU_SOURCE
#include "zgl.h"
#include "s31_pcolor.h"

int s31pc_on = -1;                      /* -1: not read yet, 0 off, 1 on */
uintptr_t s31pc_spmin = UINTPTR_MAX;
uintptr_t s31pc_alo, s31pc_alen;        /* the arena (0 len: none) */
int s31pc_hold;                         /* set around the null context */

#if defined(__linux__) && !defined(S31GL_NO_PCOLOR)
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PG 4096u
#define NCOL 8
#define ARENA_PAGES 4
#define POOL_MAX 64                     /* 256 kB at the very most */
#define POOL_STEP 16

static int pm_fd = -1;
static int pc_frame_at = 3, pc_frames, pc_done, pc_verbose = 1;
static uintptr_t a_off;

/* the frame colour of a resident page; -1 not resident, -2 no PFN shown */
static int col_of(uintptr_t va)
{
  unsigned long long e;
  if (pread(pm_fd, &e, 8, (off_t)(va / PG) * 8) != 8) return -1;
  if (!(e >> 63)) return -1;
  e &= (1ULL << 55) - 1;
  if (e == 0) return -2;
  return (int)(e & (NCOL - 1));
}

/* ---- a pool of populated pages to take frames of a wanted colour from */
static uintptr_t pl_va[POOL_MAX];
static signed char pl_col[POOL_MAX];
static unsigned char pl_used[POOL_MAX];
static int pl_n, pl_grows;

static int pool_grow(void)
{
  int i, n = POOL_STEP;
  void *m;
  if (pl_n + n > POOL_MAX) n = POOL_MAX - pl_n;
  if (n <= 0) return 0;
  m = mmap(NULL, n * PG, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (m == MAP_FAILED) return 0;
  pl_grows++;
  for (i = 0; i < n; i++) {
    uintptr_t va = (uintptr_t)m + i * PG;
    pl_va[pl_n] = va;
    pl_col[pl_n] = (signed char)col_of(va);
    pl_used[pl_n] = 0;
    pl_n++;
  }
  return n;
}

static int pool_take(int want)
{
  int i;
  for (;;) {
    for (i = 0; i < pl_n; i++)
      if (!pl_used[i] && pl_col[i] == want) { pl_used[i] = 1; return i; }
    if (!pool_grow()) return -1;
  }
}

static void pool_release(void)
{
  int i;
  for (i = 0; i < pl_n; i++)
    if (!pl_used[i]) munmap((void *)pl_va[i], PG);
  pl_n = 0;
}

/* copy by words: no libc mem* (the board's may be PIE code, and this can
   run with nothing but a private stack) */
static void pg_copy(uintptr_t dst, uintptr_t src)
{
  unsigned int *d = (unsigned int *)dst;
  const unsigned int *s = (const unsigned int *)src;
  unsigned int i;
  for (i = 0; i < PG / 4; i++) d[i] = s[i];
}

/* give the page at va a frame of colour want, same contents, same address */
static int recolour(uintptr_t va, int want, int prot)
{
  int k = pool_take(want);
  void *r;
  if (k < 0) return 0;
  pg_copy(pl_va[k], va);
  r = mremap((void *)pl_va[k], PG, PG, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)va);
  if (r == MAP_FAILED) { pl_used[k] = 0; return 0; }
  if (prot != (PROT_READ | PROT_WRITE)) mprotect((void *)va, PG, prot);
  return 1;
}

void s31pc_init(void)
{
  const char *e;
  int c;
  if (s31pc_on >= 0) return;
  s31pc_on = 0;
  e = getenv("S31GL_PCOLOR");
  if (e == NULL || atoi(e) <= 0) return;
  if ((e = getenv("S31GL_PCFRAME")) && atoi(e) > 0) pc_frame_at = atoi(e);
  pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  c = pm_fd >= 0 ? col_of((uintptr_t)&s31pc_on & ~(uintptr_t)(PG - 1)) : -1;
  if (c < 0) {
    fprintf(stderr, "libGL: S31GL_PCOLOR: no page frames in /proc/self/pagemap "
            "(%s) - off\n", c == -2 ? "not root" : "unreadable");
    if (pm_fd >= 0) close(pm_fd);
    pm_fd = -1;
    return;
  }
  s31pc_on = 1;
}

/* GLContext, its vertex array and ZBuffer: one arena of ARENA_PAGES pages,
   page i on colour i; zeroed, never freed (gl_free skips it) */
void *s31pc_zalloc(size_t n)
{
  void *p;
  if (s31pc_on < 0) s31pc_init();
  if (s31pc_on <= 0 || s31pc_hold) return NULL;
  if (s31pc_alen == 0) {
    int i, ok = 0;
    void *m = mmap(NULL, ARENA_PAGES * PG, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (m == MAP_FAILED) { s31pc_on = 0; return NULL; }
    for (i = 0; i < ARENA_PAGES; i++) {
      uintptr_t va = (uintptr_t)m + i * PG;
      if (col_of(va) == i || recolour(va, i, PROT_READ | PROT_WRITE)) ok++;
    }
    pool_release();
    mlock(m, ARENA_PAGES * PG);
    s31pc_alo = (uintptr_t)m;
    s31pc_alen = ARENA_PAGES * PG;
    if (pc_verbose)
      fprintf(stderr, "libGL: S31GL_PCOLOR arena %p, %d of %d pages on colours 0-%d\n",
              m, ok, ARENA_PAGES, ARENA_PAGES - 1);
  }
  n = (n + 15) & ~(size_t)15;
  if (a_off + n > s31pc_alen) return NULL;
  p = (void *)(s31pc_alo + a_off);
  a_off += n;
  return p;
}

/* ---- the frame-N pass: stack pages and libGL's writable pages */
#define MAXT 48
static struct { uintptr_t va; int w, prot, want, from; } tg[MAXT];
static int ntg;
static struct { uintptr_t lo, hi; int prot, x; } mp[512];
static int nmp;

static void maps_read(void)
{
  FILE *f = fopen("/proc/self/maps", "r");
  char line[256];
  nmp = 0;
  if (f == NULL) return;
  while (nmp < 512 && fgets(line, sizeof(line), f)) {
    unsigned long lo, hi;
    char perm[8];
    if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
    mp[nmp].lo = lo; mp[nmp].hi = hi;
    mp[nmp].prot = (perm[0] == 'r' ? PROT_READ : 0) | (perm[1] == 'w' ? PROT_WRITE : 0);
    mp[nmp].x = perm[2] == 'x';
    nmp++;
  }
  fclose(f);
}

static int map_of(uintptr_t va)
{
  int i;
  for (i = 0; i < nmp; i++)
    if (va >= mp[i].lo && va < mp[i].hi) return i;
  return -1;
}

static void add_target(uintptr_t va, int w, int stack)
{
  int m = map_of(va), i;
  if (ntg >= MAXT || m < 0 || mp[m].x || !(mp[m].prot & PROT_READ)) return;
  /* the bottom page of a growing stack keeps its VMA (VM_GROWSDOWN) */
  if (stack && va < mp[m].lo + PG) return;
  for (i = 0; i < ntg; i++) if (tg[i].va == va) return;
  if ((tg[ntg].from = col_of(va)) < 0) return;      /* not resident */
  tg[ntg].va = va; tg[ntg].w = w; tg[ntg].prot = mp[m].prot;
  ntg++;
}

static uintptr_t self_lo, self_hi;       /* the dl_iterate_phdr hit */
static int phdr_cb(struct dl_phdr_info *in, size_t sz, void *arg)
{
  int i, hit = 0;
  uintptr_t me = (uintptr_t)&s31pc_on;
  for (i = 0; i < in->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &in->dlpi_phdr[i];
    uintptr_t lo = in->dlpi_addr + ph->p_vaddr, hi = lo + ph->p_memsz;
    if (ph->p_type == PT_LOAD && me >= lo && me < hi) hit = 1;
  }
  if (!hit) return 0;
  for (i = 0; i < in->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &in->dlpi_phdr[i];
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W)) {
      self_lo = (in->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)(PG - 1);
      self_hi = (in->dlpi_addr + ph->p_vaddr + ph->p_memsz + PG - 1) & ~(uintptr_t)(PG - 1);
    }
  }
  return 1;
}

static int moved, kept, failed;

static void pass_moves(void *arg)
{
  int i;
  for (i = 0; i < ntg; i++) {
    if (tg[i].want == tg[i].from) kept++;
    else if (recolour(tg[i].va, tg[i].want, tg[i].prot)) moved++;
    else { failed++; tg[i].want = tg[i].from; }
  }
  pool_release();
}

#if defined(__riscv) && __riscv_xlen == 32
/* call fn(arg) with sp = top (16-byte aligned); nothing is stored on the
   caller's stack while fn runs */
__attribute__((naked, noinline)) static void on_stack(void (*fn)(void *), void *arg, void *top)
{
  __asm__ volatile(
    "addi a2, a2, -16\n\t"
    "sw ra, 12(a2)\n\t"
    "sw sp, 8(a2)\n\t"
    "mv sp, a2\n\t"
    "mv t1, a0\n\t"
    "mv a0, a1\n\t"
    "jalr t1\n\t"
    "lw ra, 12(sp)\n\t"
    "lw t0, 8(sp)\n\t"
    "mv sp, t0\n\t"
    "ret\n\t");
}
#define PC_STACK 1
#endif

void s31pc_frame(void)
{
  int load[NCOL] = { 0 }, i, c, nstack = 0;
  uintptr_t va;
  if (s31pc_on <= 0 || pc_done || ++pc_frames < pc_frame_at) return;
  pc_done = 1;
  maps_read();
  ntg = 0;
  /* the arena's pages hold the context: weight 3 each on colours 0-3 */
  for (i = 0; i < ARENA_PAGES && s31pc_alen; i++) {
    c = col_of(s31pc_alo + i * PG);
    if (c >= 0) load[c] += 3;
  }
#ifdef PC_STACK
  /* the rasteriser's frames: from 3 kB below the deepest glBegin seen to
     1 kB above it, weight 4 (the hottest page of all in the model) */
  if (s31pc_spmin != UINTPTR_MAX) {
    for (va = (s31pc_spmin - 3072) & ~(uintptr_t)(PG - 1); va < s31pc_spmin + 1024; va += PG)
      add_target(va, 4, 1);
    nstack = ntg;
  }
#endif
  dl_iterate_phdr(phdr_cb, NULL);
  for (va = self_lo; va < self_hi; va += PG) add_target(va, 1, 0);
  /* least-loaded colour for each, heaviest first (the list is in weight
     order already); a tie keeps the page where it is */
  for (i = 0; i < ntg; i++) {
    int best = tg[i].from, k;
    for (k = 0; k < NCOL; k++)
      if (load[k] < load[best]) best = k;
    tg[i].want = best;
    load[best] += tg[i].w;
  }
  {
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
#ifdef PC_STACK
    {
      void *st = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
      if (st != MAP_FAILED) {
        on_stack(pass_moves, NULL, (char *)st + 4 * PG);
        munmap(st, 4 * PG);
      } else {
        ntg = ntg - nstack;                 /* no private stack: data only */
        memmove(tg, tg + nstack, ntg * sizeof(tg[0]));
        nstack = 0;
        pass_moves(NULL);
      }
    }
#else
    pass_moves(NULL);
#endif
    pthread_sigmask(SIG_SETMASK, &old, NULL);
  }
  for (i = 0; i < ntg; i++) mlock((void *)tg[i].va, PG);
  if (pc_verbose) {
    char b[160];
    int n = 0;
    for (i = 0; i < ntg && n < (int)sizeof(b) - 8; i++)
      n += snprintf(b + n, sizeof(b) - n, "%s%d>%d", i == nstack ? " |" : " ",
                    tg[i].from, tg[i].want);
    fprintf(stderr, "libGL: S31GL_PCOLOR frame %d: sp %#lx, %d stack + %d data pages, "
            "%d moved %d kept %d failed, pool %d grows; colours%s\n", pc_frames,
            (unsigned long)s31pc_spmin, nstack, ntg - nstack, moved, kept, failed,
            pl_grows, b);
  }
}

#else  /* not Linux: nothing to pick frames with */
void s31pc_init(void) { s31pc_on = 0; }
void *s31pc_zalloc(size_t n) { return NULL; }
void s31pc_frame(void) { }
#endif
