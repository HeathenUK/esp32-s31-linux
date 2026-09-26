/*
 * s31_ramtext.c - lever L1 (plan 6, stage 3b, mechanism (a)): run the hot
 * rasteriser from RAM instead of XIP flash. s31, MIT.
 *
 * How the pieces fit (s31_ramtext.h for the entry points):
 *
 * - Build. gl/api/ramtext.list names the hot functions. build-lib.sh moves
 *   their sections into "s31hot_text" (objcopy, after compiling: the code
 *   is byte for byte what it was), and gl/api/ramtext.ld links them into
 *   one contiguous range [__s31hot_start, __s31hot_end), after .text,
 *   behind the GL stubs (code the hot range never calls, so no call out of
 *   it can be relaxed to a 2-byte c.jal/c.j that a move could not fix).
 *   gl/api/ramtext.py then reads the relocations the linker emitted (-q),
 *   lists every PC-relative reference from inside the range to outside it
 *   (data, the GOT, other functions, PLT stubs) and writes that list into
 *   s31_ramtext_fix[] in the linked file. References that stay inside the
 *   range need nothing: they move with it.
 *
 * - Run (S31GL_RAMTEXT=1, default S31GL_RAMTEXT_DEFAULT). At the first
 *   context creation: copy the range into the whole pages of
 *   s31_ramtext_slot (in libGL's own .bss: anonymous RW pages inside the
 *   library's mapping, so within reach of a jal, and untouched - so free -
 *   until now), keeping its offset modulo 64 (cache lines). Then each
 *   listed site is retargeted so it reaches the same absolute address from
 *   the copy: a RISC-V auipc gets a new high part and its low halves (the
 *   addi/load/store/jalr that complete it) a new low part; a jal, an
 *   AArch64 b/bl/adr/ldr-literal move by the displacement (range-checked);
 *   an adrp gets the new page difference (its low 12 bits are absolute).
 *   Then the pages become RX (W^X: never writable and executable at once)
 *   and the entry points in s31_rt move with the copy. Any failure leaves
 *   s31_rt on the XIP copies, which are always correct.
 *
 * - I-cache. On the S31 the I-cache does not snoop the D-cache for PSRAM,
 *   and fence.i (or SYS_riscv_flush_icache, which is only fence.i on every
 *   hart: arch/riscv/kernel/sys_riscv.c -> flush_icache_mm) does not write
 *   the D-cache back. What does is the kernel's exec-PTE hook:
 *   mprotect(PROT_EXEC) -> set_pte_at -> flush_icache_pte, which for a
 *   page not yet PG_dcache_clean calls esp32s31_cache_sync_for_exec (D-cache
 *   writeback + I-cache invalidate of that page; patches/0005, cacheflush.c)
 *   and then flush_icache_mm. So the order is load-bearing: every write
 *   first, then one mprotect to RX, and no write after it. The fence.i
 *   syscall is still made (it is the documented user-space call on other
 *   RISC-V kernels); AArch64 uses __builtin___clear_cache.
 *
 * - Elsewhere (bare-metal gl/bench images, other architectures) the copy
 *   is compiled out and S31GL_RAMTEXT does nothing.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "s31_ramtext.h"

#ifndef S31GL_RAMTEXT_DEFAULT
#define S31GL_RAMTEXT_DEFAULT 0     /* board A/B first (plan 6 stage 3b) */
#endif

/* the XIP entry points; s31_ramtext_init may move them into the copy */
struct s31_rt s31_rt = {
  ZB_fillTriangleFlat, ZB_fillTriangleSmooth, ZB_fillTriangleMappingPerspective,
  ZB_fillTriangleFlat_lt, ZB_fillTriangleSmooth_lt,
  ZB_fillTriangleMappingPerspective_lt,
  gl_draw_triangle_fill, gl_draw_triangle_fill_pq,
  gl_vertex4f, glopCallList,
};

#if !defined(S31GL_NO_RAMTEXT) && defined(__linux__) && ((defined(__riscv) && __riscv_xlen == 32) || defined(__aarch64__))
#define RT_COPY 1
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* gl/api/ramtext.py knows this layout: keep the two in step */
#define RT_MAGIC   0x52313353u     /* "S31R" */
#define RT_UNSET   0xffffffffu     /* the linked file was not processed */
#define RT_HDR     4               /* magic, count, length, FNV-1a of the range */
/* table entries (151 on RV32, 62 on AArch64, 2026-09-26) and slot bytes;
   ramtext.py says when to raise them. A sanitizer build of the same range
   needs far more (run-san.sh passes -D for both) */
#ifndef S31GL_RAMTEXT_FIXMAX
#define S31GL_RAMTEXT_FIXMAX 384
#endif
#ifndef S31GL_RAMTEXT_SLOT
#define S31GL_RAMTEXT_SLOT (20 * 1024)
#endif
#define RT_FIXMAX  S31GL_RAMTEXT_FIXMAX
#define RT_SLOT    S31GL_RAMTEXT_SLOT
#define RT_GRAIN   64              /* the copy keeps the range's offset mod this */

/* a table entry is (offset in the range << 3) | kind. An RV_HI (auipc) is
   followed by the entries of its low halves (addi/load/jalr: RV_LO_I,
   store: RV_LO_S), which are retargeted with it */
enum { K_RV_LO_S = 0, K_RV_HI = 1, K_RV_JAL = 2, K_A64_ADRP = 3, K_A64_B26 = 4,
       K_A64_ADR = 5, K_A64_LDLIT = 6, K_RV_LO_I = 7 };

extern const unsigned char __s31hot_start[] __attribute__((visibility("hidden")));
extern const unsigned char __s31hot_end[] __attribute__((visibility("hidden")));

/* written in place by ramtext.py; the initialiser keeps it in .rodata
   (non-zero) and says "not processed" until then */
__attribute__((used, visibility("hidden")))
const uint32_t s31_ramtext_fix[RT_FIXMAX] = { RT_MAGIC, RT_UNSET };

/* the RAM copy goes in the whole pages inside this array, and only those:
   mprotect works on pages, and the partial pages at its ends are shared
   with the rest of .bss */
static unsigned char s31_ramtext_slot[RT_SLOT + 4096];

static int32_t sext(uint32_t v, int bits)
{
  return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

static int fits(int64_t v, int bits)
{
  return v >= -((int64_t)1 << (bits - 1)) && v < ((int64_t)1 << (bits - 1));
}

static uint32_t get32(const unsigned char *p)
{
  uint32_t w;
  memcpy(&w, p, 4);            /* RVC code: sites are only 2-byte aligned */
  return w;
}

static void put32(unsigned char *p, uint32_t w)
{
  memcpy(p, &w, 4);
}

#if defined(__riscv)
/* the 12-bit immediate of an I-type (addi, loads, jalr) or S-type (stores)
   instruction */
static int32_t lo_get(uint32_t w, unsigned int kind)
{
  return kind == K_RV_LO_I ? sext(w >> 20, 12)
                           : sext(((w >> 25) << 5) | ((w >> 7) & 31), 12);
}

static uint32_t lo_put(uint32_t w, unsigned int kind, int32_t lo)
{
  uint32_t u = (uint32_t)lo & 0xfff;
  if (kind == K_RV_LO_I) return (w & 0x000fffff) | (u << 20);
  return (w & 0x01fff07f) | ((u >> 5) << 25) | ((u & 31) << 7);
}
#endif

/* Retarget the sites listed in t[] in the copy at dst (from s: moved by
   d): each keeps the absolute target it had. 0 on success */
static int fix_all(const uint32_t *t, uint32_t n, const unsigned char *s,
                   unsigned char *dst, size_t len)
{
  intptr_t d = (intptr_t)(dst - s);
  uint32_t i = 0;

  while (i < n) {
    uint32_t e = t[i++], o = e >> 3, kind = e & 7, w;
    int64_t imm;
    if (o > len - 4) return -1;
    w = get32(dst + o);
    switch (kind) {
#if defined(__riscv)
    case K_RV_HI: {            /* auipc + its low halves: T = pc + hi + lo */
      uint32_t j, w2, o2, k2;
      int64_t rel;
      int32_t lo, hi;
      if ((w & 0x7f) != 0x17 || i >= n) return -1;
      k2 = t[i] & 7; o2 = t[i] >> 3;
      if ((k2 != K_RV_LO_I && k2 != K_RV_LO_S) || o2 > len - 4) return -1;
      rel = (int64_t)sext(w & 0xfffff000, 32) + lo_get(get32(dst + o2), k2) - d;
      if (!fits(rel, 32)) return -1;
      hi = (int32_t)(((uint32_t)rel + 0x800) & 0xfffff000);
      lo = (int32_t)rel - hi;
      put32(dst + o, (w & 0xfff) | (uint32_t)hi);
      for (j = i; j < n && ((t[j] & 7) == K_RV_LO_I || (t[j] & 7) == K_RV_LO_S); j++) {
        o2 = t[j] >> 3; k2 = t[j] & 7;
        if (o2 > len - 4) return -1;
        w2 = get32(dst + o2);
        put32(dst + o2, lo_put(w2, k2, lo));
      }
      i = j;
      continue;
    }
    case K_RV_JAL:             /* imm[20|10:1|11|19:12], +-1 MiB */
      if ((w & 0x7f) != 0x6f) return -1;
      imm = sext(((w >> 31) << 20) | (((w >> 21) & 0x3ff) << 1) |
                 (((w >> 20) & 1) << 11) | (((w >> 12) & 0xff) << 12), 21) - d;
      if (!fits(imm, 21)) return -1;
      w = (w & 0xfff) | (((uint32_t)(imm >> 20) & 1) << 31) |
          (((uint32_t)(imm >> 1) & 0x3ff) << 21) | (((uint32_t)(imm >> 11) & 1) << 20) |
          (((uint32_t)(imm >> 12) & 0xff) << 12);
      break;
#else
    case K_A64_ADRP:           /* page(T) - page(pc), +-4 GiB; the low 12
                                  bits of T are absolute: unchanged */
    case K_A64_ADR:            /* T - pc, +-1 MiB */
      if ((w & 0x1f000000) != 0x10000000) return -1;
      if (((w >> 31) != 0) != (kind == K_A64_ADRP)) return -1;
      imm = sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21);
      if (kind == K_A64_ADRP) {
        uintptr_t pc = (uintptr_t)s + o;
        uintptr_t tp = (pc & ~(uintptr_t)4095) + (uintptr_t)(imm * 4096);
        imm = ((intptr_t)tp - (intptr_t)(((uintptr_t)dst + o) & ~(uintptr_t)4095)) / 4096;
      } else {
        imm -= d;
      }
      if (!fits(imm, 21)) return -1;
      w = (w & 0x9f00001f) | (((uint32_t)imm & 3) << 29) |
          ((((uint32_t)imm >> 2) & 0x7ffff) << 5);
      break;
    case K_A64_B26:            /* b / bl, words, +-128 MiB */
      if ((w & 0x7c000000) != 0x14000000) return -1;
      imm = sext(w & 0x3ffffff, 26) - d / 4;
      if (!fits(imm, 26)) return -1;
      w = (w & 0xfc000000) | ((uint32_t)imm & 0x3ffffff);
      break;
    case K_A64_LDLIT:          /* ldr (literal), words, +-1 MiB */
      if ((w & 0x3b000000) != 0x18000000) return -1;
      imm = sext((w >> 5) & 0x7ffff, 19) - d / 4;
      if (!fits(imm, 19)) return -1;
      w = (w & 0xff00001f) | (((uint32_t)imm & 0x7ffff) << 5);
      break;
#endif
    default:
      return -1;               /* a low half without its auipc, or a kind
                                  for another architecture */
    }
    put32(dst + o, w);
  }
  return 0;
}

static uint32_t fnv1a(const unsigned char *p, size_t n)
{
  uint32_t h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}

/* an entry point inside the range moves with it */
#define RT_MOVE(f) do { \
    uintptr_t a_ = (uintptr_t)s31_rt.f; \
    if (a_ >= (uintptr_t)__s31hot_start && a_ < (uintptr_t)__s31hot_end) { \
      s31_rt.f = (__typeof__(s31_rt.f))(a_ + d); moved++; } \
  } while (0)

static const char *rt_copy(size_t *bytes, size_t *pages, unsigned int *nfix,
                           int *nmoved)
{
  const uint32_t *t = s31_ramtext_fix;
  const unsigned char *s = __s31hot_start;
  size_t len = (size_t)(__s31hot_end - __s31hot_start), off, span;
  unsigned char *dst, *page;
  intptr_t d;
  uint32_t n;
  long pg;
  int moved = 0;

  /* the table is written after compilation: stop the compiler folding
     its initialiser into these reads */
  __asm__ ("" : "+r"(t));
  if (t[0] != RT_MAGIC) return "no fixup table";
  n = t[1];
  if (n == RT_UNSET) return "fixup table not generated (not linked by build-lib.sh)";
  if (n > RT_FIXMAX - RT_HDR || t[2] != len || len == 0) return "fixup table does not match";
  if (fnv1a(s, len) != t[3]) return "code does not match its fixup table";
  pg = sysconf(_SC_PAGESIZE);
  if (pg <= 0 || 4096 % pg) return "page size above 4096";
  /* the same offset modulo RT_GRAIN: loops keep their cache-line
     alignment, and the copy spans the fewest pages */
  off = (uintptr_t)s & (RT_GRAIN - 1);
  span = (off + len + 4095) & ~(size_t)4095;
  if (span > RT_SLOT) return "slot too small";

  page = (unsigned char *)(((uintptr_t)s31_ramtext_slot + 4095) & ~(uintptr_t)4095);
  dst = page + off;
  d = (intptr_t)(dst - s);
  memcpy(dst, s, len);
  if (fix_all(t + RT_HDR, n, s, dst, len)) {
    /* give the pages back; the XIP copy stays in use */
    madvise(page, span, MADV_DONTNEED);
    return "a fixup did not apply";
  }
#if defined(__aarch64__)
  __builtin___clear_cache((char *)dst, (char *)dst + len);
#endif
  /* W^X, and on the S31 the one step that writes the D-cache back and
     invalidates the I-cache for these pages (see the header comment) */
  if (mprotect(page, span, PROT_READ | PROT_EXEC)) {
    madvise(page, span, MADV_DONTNEED);
    return "mprotect failed";
  }
#if defined(__riscv)
#ifdef SYS_riscv_flush_icache
  syscall(SYS_riscv_flush_icache, (uintptr_t)dst, (uintptr_t)dst + len, 0);
#endif
  __asm__ volatile ("fence.i" ::: "memory");
#endif

  RT_MOVE(flat); RT_MOVE(smooth); RT_MOVE(map);
  RT_MOVE(flat_lt); RT_MOVE(smooth_lt); RT_MOVE(map_lt);
  RT_MOVE(draw_fill); RT_MOVE(draw_fill_pq);
  RT_MOVE(vertex4f); RT_MOVE(call_list);
  *bytes = len;
  *pages = span / 4096;
  *nfix = n;
  *nmoved = moved;
  return NULL;
}
#endif /* RT_COPY */

void s31_ramtext_init(void)
{
  static int done;
  const char *e;
  int on = S31GL_RAMTEXT_DEFAULT;

  if (done) return;
  done = 1;
  e = getenv("S31GL_RAMTEXT");
  if (e != NULL) on = atoi(e) != 0;
  if (!on) return;
#ifdef RT_COPY
  {
    size_t bytes = 0, pages = 0;
    unsigned int nfix = 0;
    int moved = 0;
    const char *why = rt_copy(&bytes, &pages, &nfix, &moved);
    /* one line, and only when asked for by name: the board A/B's proof
       of which arm ran */
    if (e == NULL) return;
    if (why) fprintf(stderr, "libGL: ramtext off: %s\n", why);
    else fprintf(stderr, "libGL: ramtext on: %u bytes in %u pages, "
                 "%u fixup entries, %d entry points\n", (unsigned)bytes,
                 (unsigned)pages, nfix, moved);
  }
#else
  if (e != NULL) fprintf(stderr, "libGL: ramtext off: not built for this target\n");
#endif
}
