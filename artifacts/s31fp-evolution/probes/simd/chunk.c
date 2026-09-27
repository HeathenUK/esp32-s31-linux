/* HW1 chunked commit/abort state machine, scalar twin, deterministic abort injection.
 * Abort model: inside chunk c (0-based count of sections entered), after j bytes of the
 * chunk were stored, the section aborts; committed progress (d,s,n) is unchanged, the
 * dispatcher re-enters. Each section aborts at most once (injection only on first entry). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define C 1024
static long abort_at_section = -1, abort_after_bytes;
static long sections;
static void *chunk_move_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
	void *ret = d;
	while (n) {				/* dispatcher */
		size_t k = n < C ? n : C;
		long sec = sections++;
		size_t lim = (sec == abort_at_section) ? (size_t)abort_after_bytes : k;
		size_t i;
		for (i = 0; i < k; i++) {	/* critical section body */
			if (i == lim) break;	/* abort: IP -> abort handler, no commit */
			d[i] = s[i];
		}
		if (i < k) continue;		/* aborted: redo from committed progress */
		d += k; s += k; n -= k;		/* commit */
	}
	return ret;
}
static const unsigned char *memchr_chunk(const unsigned char *p, int c, size_t n)
{
	while (n) {
		size_t k = n < C ? n : C; long sec = sections++;
		size_t lim = (sec == abort_at_section) ? (size_t)abort_after_bytes : k; size_t i;
		for (i = 0; i < k && i < lim; i++) if (p[i] == (unsigned char)c) break;
		if (i < k && i == lim) continue;	/* aborted before a result was committed */
		if (i < k) return p + i;
		p += k; n -= k;
	}
	return 0;
}
int main(void)
{
	static unsigned char buf[3 * 8192], ref[3 * 8192];
	unsigned x = 1; long cases = 0, badcpy = 0, badmove = 0, badmovecases_small = 0, badchr = 0;
	long badmove_by_gap[5] = {0};
	for (int it = 0; it < 4000; it++) {
		x = x * 1103515245 + 12345; size_t n = (x >> 8) % 8192;
		x = x * 1103515245 + 12345; size_t so = (x >> 8) % 16;
		x = x * 1103515245 + 12345; size_t dofs = 8192 + (x >> 8) % 16;
		size_t nchunks = (n + C - 1) / C;
		for (long a = 0; a <= (long)nchunks; a++) {
			x = x * 1103515245 + 12345; long j = n ? (x >> 8) % C : 0;
			/* memcpy, non-overlapping */
			{ unsigned y = it * 40503u + 7; for (int i = 0; i < (int)sizeof buf; i++) { y = y * 1664525u + 1013904223u; buf[i] = ref[i] = y >> 24; } }
			memcpy(ref + dofs, ref + so, n);
			sections = 0; abort_at_section = a; abort_after_bytes = j;
			chunk_move_fwd(buf + dofs, buf + so, n);
			if (memcmp(buf, ref, sizeof buf)) badcpy++;
			/* forward memmove, overlapping d < s with gap g */
			static const size_t gaps[5] = {1, 16, 512, 1023, 1024};
			for (int gi = 0; gi < 5; gi++) {
				size_t g = gaps[gi], s0 = 4096, d0 = s0 - g; size_t nn = n > 8000 ? 8000 : n;
				{ unsigned y = it * 2654435761u + 1; for (int i = 0; i < (int)sizeof buf; i++) { y = y * 1664525u + 1013904223u; buf[i] = ref[i] = y >> 24; } }
				memmove(ref + d0, ref + s0, nn);
				sections = 0; abort_at_section = a; abort_after_bytes = j;
				chunk_move_fwd(buf + d0, buf + s0, nn);
				if (memcmp(buf, ref, sizeof buf)) badmove_by_gap[gi]++;
			}
			/* memchr */
			for (int i = 0; i < 8192; i++) buf[i] = (unsigned char)(i * 7 + it) | 1;
			x = x * 1103515245 + 12345; if (n && (x & 1)) buf[so + (x >> 9) % n] = 0;
			sections = 0; abort_at_section = a; abort_after_bytes = j;
			const void *r1 = memchr(buf + so, 0, n), *r2 = memchr_chunk(buf + so, 0, n);
			if (r1 != r2) badchr++;
			cases++;
		}
	}
	printf("cases=%ld (every abort section k incl. none) memcpy_mismatch=%ld memchr_mismatch=%ld\n", cases, badcpy, badchr);
	printf("fwd memmove d=s-g mismatches: g=1:%ld g=16:%ld g=512:%ld g=1023:%ld g=1024:%ld\n",
	       badmove_by_gap[0], badmove_by_gap[1], badmove_by_gap[2], badmove_by_gap[3], badmove_by_gap[4]);
	return 0;
}
