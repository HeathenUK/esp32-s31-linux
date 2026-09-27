/*
 * scan2.h - one-pass signature scanner for the s31fp v2 interception
 * (test-only redesign). The 2026-09-20 preload ran memmem() once per
 * signature over every executable segment of every loaded object: ~10 full
 * passes over libc, libSDL, ... = +250 ms per exec. This walks a segment once,
 * 2-byte aligned (RVC), filtering on the first halfword through an 8 KB
 * bitmap, and compares the full prefix only on a filter hit.
 */
#include <stdint.h>
#include <string.h>
#include "sigs2.h"
#define NSIG (sizeof(s31fp_sigs) / sizeof(s31fp_sigs[0]))
static uint8_t s31_bm[8192];
static void scan_init(void)
{
	for (unsigned i = 0; i < NSIG; i++) {
		unsigned h = s31fp_sigs[i].bytes[0] | s31fp_sigs[i].bytes[1] << 8;
		s31_bm[h >> 3] |= 1u << (h & 7);
	}
}
/* calls hit(ctx, sig index, address) for every full match */
static unsigned scan_seg(const uint8_t *base, size_t len,
			 void (*hit)(void *, unsigned, uint8_t *), void *ctx)
{
	const uint16_t *p = (const uint16_t *)((uintptr_t)(base + 1) & ~(uintptr_t)1);
	const uint8_t *end = base + len;
	const uint16_t *e = (const uint16_t *)(end - 24);	/* shortest signature */
	unsigned n = 0;
	if (len < 24) return 0;
	for (; p < e; p++) {
		unsigned h = *p;
		if (!(s31_bm[h >> 3] & (1u << (h & 7))))
			continue;
		for (unsigned i = 0; i < NSIG; i++) {
			const struct s31fp_sig *g = &s31fp_sigs[i];
			if ((g->bytes[0] | g->bytes[1] << 8) == h &&
			    (const uint8_t *)p + g->len <= end &&
			    !memcmp(p, g->bytes, g->len)) {
				hit(ctx, i, (uint8_t *)p);
				n++;
				break;
			}
		}
	}
	return n;
}
