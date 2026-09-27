/*
 * s31_pcolor.h - phase 6 tier 5: page colouring of libGL's hot small
 * state (S31GL_PCOLOR). s31, MIT.
 *
 * The S31 D-cache is 64 kB, 2-way, physically indexed: a way is 32 kB, so
 * bits 12-14 of the PHYSICAL address (the page frame's "colour", 8 of them)
 * pick the set together with the in-page offset. Which colour a page gets
 * is the kernel's choice, per launch; artifacts/gl/phase6/tier4/REPORT.txt
 * section 3 found the per-launch spread of PSRAM traffic is a handful of
 * hot small pages (the rasteriser's stack page, the GLContext's pages,
 * the GOT / .data / .bss) landing on one colour.
 *
 * With S31GL_PCOLOR=1 (Linux, root: /proc/self/pagemap shows frames) the
 * library picks those frames itself:
 *   - GLContext, its vertex array and ZBuffer come from a 4-page arena
 *     whose pages are given colours 0..3 at creation (s31pc_zalloc);
 *   - after frame S31GL_PCFRAME (default 3), the stack pages around the
 *     deepest glBegin seen (s31pc_probe, in gl_update_raster: cold) and
 *     libGL's resident writable pages (.data.rel.ro, .got, .data, .bss)
 *     get the least-loaded colours left: a page of the wanted colour is
 *     found in a small populated pool, the old contents copied into it and
 *     mremap'ed over the old page (same virtual address, new frame), then
 *     mlock'ed so reclaim cannot hand it a new frame. The stack work runs
 *     on a private stack with signals blocked, so nothing writes the pages
 *     being copied.
 * Nothing but physical placement changes: pixels are bit-identical by
 * construction. Default 0 (the arena is not used, the probe is a load and
 * a branch per raster update, nothing else runs).
 */
#ifndef S31_PCOLOR_H
#define S31_PCOLOR_H

#include <stddef.h>
#include <stdint.h>

extern int s31pc_on __attribute__((visibility("hidden")));
extern uintptr_t s31pc_spmin __attribute__((visibility("hidden")));
extern uintptr_t s31pc_alo __attribute__((visibility("hidden")));
extern uintptr_t s31pc_alen __attribute__((visibility("hidden")));
extern int s31pc_hold __attribute__((visibility("hidden")));  /* no arena */

void s31pc_init(void) __attribute__((visibility("hidden")));
void *s31pc_zalloc(size_t n) __attribute__((visibility("hidden")));
void s31pc_frame(void) __attribute__((visibility("hidden")));

/* the deepest stack pointer seen at a raster update (glBegin's depth) */
static inline void s31pc_probe(void)
{
  if (__builtin_expect(s31pc_on > 0, 0)) {
    uintptr_t sp;
#if defined(__riscv)
    __asm__ volatile ("mv %0, sp" : "=r"(sp));
#elif defined(__aarch64__)
    __asm__ volatile ("mov %0, sp" : "=r"(sp));
#else
    sp = (uintptr_t)__builtin_frame_address(0);
#endif
    if (sp < s31pc_spmin) s31pc_spmin = sp;
  }
}

/* an arena pointer: gl_free must not hand it to free() */
#define S31PC_OWNS(p) ((uintptr_t)(p) - s31pc_alo < s31pc_alen)

#endif
