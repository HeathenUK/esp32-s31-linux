/* HW3 claim test: Zbb min/max twin of SDL 1.2 SDL_MixAudio inner loops vs SDL's C.
 * Reference = verbatim loops from SDL-1.2 src/audio/SDL_mixer.c (format passed
 * explicitly because SDL reads it from the static `current_audio`). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t Uint8; typedef int8_t Sint8; typedef int16_t Sint16; typedef uint32_t Uint32; typedef uint16_t Uint16;
#define SDL_MIX_MAXVOLUME 128
#define AUDIO_U8 0x0008
#define AUDIO_S8 0x8008
#define AUDIO_S16LSB 0x8010
static const Uint8 mix8[] = {
#include "mix8.inc"
};
#define ADJUST_VOLUME(s, v)	(s = (s*v)/SDL_MIX_MAXVOLUME)
#define ADJUST_VOLUME_U8(s, v)	(s = (((s-128)*v)/SDL_MIX_MAXVOLUME)+128)
__attribute__((noinline)) void ref_mix(Uint8 *dst, const Uint8 *src, Uint32 len, int volume, Uint16 format)
{
	if ( volume == 0 ) return;
	switch (format) {
	case AUDIO_U8: { Uint8 src_sample;
		while ( len-- ) { src_sample = *src; ADJUST_VOLUME_U8(src_sample, volume);
			*dst = mix8[*dst+src_sample]; ++dst; ++src; } } break;
	case AUDIO_S8: { Sint8 *dst8, *src8; Sint8 src_sample; int dst_sample;
		const int max_audioval = ((1<<(8-1))-1); const int min_audioval = -(1<<(8-1));
		src8 = (Sint8 *)src; dst8 = (Sint8 *)dst;
		while ( len-- ) { src_sample = *src8; ADJUST_VOLUME(src_sample, volume);
			dst_sample = *dst8 + src_sample;
			if ( dst_sample > max_audioval ) { *dst8 = max_audioval; } else
			if ( dst_sample < min_audioval ) { *dst8 = min_audioval; } else { *dst8 = dst_sample; }
			++dst8; ++src8; } } break;
	case AUDIO_S16LSB: { Sint16 src1, src2; int dst_sample;
		const int max_audioval = ((1<<(16-1))-1); const int min_audioval = -(1<<(16-1));
		len /= 2;
		while ( len-- ) { src1 = ((src[1])<<8|src[0]); ADJUST_VOLUME(src1, volume);
			src2 = ((dst[1])<<8|dst[0]); src += 2; dst_sample = src1+src2;
			if ( dst_sample > max_audioval ) { dst_sample = max_audioval; } else
			if ( dst_sample < min_audioval ) { dst_sample = min_audioval; }
			dst[0] = dst_sample&0xFF; dst_sample >>= 8; dst[1] = dst_sample&0xFF; dst += 2; } } break;
	}
}
static inline int zmin(int a, int b){ int r; __asm__("min %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
static inline int zmax(int a, int b){ int r; __asm__("max %0,%1,%2":"=r"(r):"r"(a),"r"(b)); return r; }
/* x/128 truncating toward zero, as a shift with sign correction */
static inline int tdiv128(int x){ return (x + ((x >> 31) & 127)) >> 7; }
/* Twin, variant NAIVE=1: arithmetic shift (floor) - the negative control */
#ifndef NAIVE
#define DIV(x) tdiv128(x)
#else
#define DIV(x) ((x) >> 7)
#endif
__attribute__((noinline)) void twin_mix(Uint8 *dst, const Uint8 *src, Uint32 len, int volume, Uint16 format)
{
	if (volume == 0) return;
	if (format == AUDIO_S16LSB) {
		Sint16 *d = (Sint16 *)dst; const Sint16 *s = (const Sint16 *)src; /* LE, aligned in harness */
		Sint16 *e = d + (len >> 1);
		if (volume == 128) {
			while (d < e) { int x = *d + *s++; *d++ = zmax(zmin(x, 32767), -32768); }
		} else {
			while (d < e) {
#ifdef NOWRAP
				int a = DIV(*s * volume); s++;
#else
				int a = (Sint16)DIV(*s * volume); s++;   /* SDL stores into Sint16 */
#endif
				int x = *d + a; *d++ = zmax(zmin(x, 32767), -32768); }
		}
	} else if (format == AUDIO_S8) {
		Sint8 *d = (Sint8 *)dst; const Sint8 *s = (const Sint8 *)src; Sint8 *e = d + len;
		while (d < e) { int a = (Sint8)DIV(*s * volume); s++; int x = *d + a; *d++ = zmax(zmin(x, 127), -128); }
	} else if (format == AUDIO_U8) {
		Uint8 *d = dst; const Uint8 *s = src; Uint8 *e = d + len;
		while (d < e) {
			int a = (Uint8)(DIV((*s - 128) * volume) + 128); s++;
			*d = zmax(zmin(*d + a - 128, 0xFE), 0); d++; }
	}
}
int main(int argc, char **argv)
{
	const char *m = argv[1];
	if (!strcmp(m, "s16")) { /* s16 v slo shi : all s in [slo,shi) x all 65536 d */
		int v = atoi(argv[2]), slo = atoi(argv[3]), shi = atoi(argv[4]);
		static Sint16 dr[65536], dt[65536], sb[65536];
		unsigned long long bad = 0, n = 0; int first = 1;
		for (int s = slo; s < shi; s++) {
			for (int i = 0; i < 65536; i++) { dr[i] = dt[i] = (Sint16)(i - 32768); sb[i] = (Sint16)s; }
			ref_mix((Uint8 *)dr, (Uint8 *)sb, 131072, v, AUDIO_S16LSB);
			twin_mix((Uint8 *)dt, (Uint8 *)sb, 131072, v, AUDIO_S16LSB);
			n += 65536;
			if (memcmp(dr, dt, sizeof dr)) for (int i = 0; i < 65536; i++) if (dr[i] != dt[i]) {
				bad++; if (first) { printf("FIRST MISMATCH v=%d s=%d d=%d ref=%d twin=%d\n", v, s, i - 32768, dr[i], dt[i]); first = 0; } }
		}
		printf("s16 v=%d s=[%d,%d) pairs=%llu mismatches=%llu\n", v, slo, shi, n, bad);
		return bad != 0;
	}
	if (!strcmp(m, "b8")) { /* S8 and U8, all s x all d, volumes vlo..vhi */
		int vlo = atoi(argv[2]), vhi = atoi(argv[3]);
		unsigned long long bad[2] = {0,0}, n = 0; int first[2] = {1,1};
		static Uint8 dr[256], dt[256], sb[256];
		for (int v = vlo; v <= vhi; v++) for (int f = 0; f < 2; f++) for (int s = 0; s < 256; s++) {
			Uint16 fmt = f ? AUDIO_U8 : AUDIO_S8;
			for (int i = 0; i < 256; i++) { dr[i] = dt[i] = i; sb[i] = s; }
			ref_mix(dr, sb, 256, v, fmt); twin_mix(dt, sb, 256, v, fmt);
			if (!f) n += 256;
			for (int i = 0; i < 256; i++) if (dr[i] != dt[i]) { bad[f]++;
				if (first[f]) { printf("FIRST MISMATCH %s v=%d s=%d d=%d ref=%d twin=%d\n", f ? "U8" : "S8", v, s, i, dr[i], dt[i]); first[f] = 0; } }
		}
		printf("S8 v=[%d,%d] pairs=%llu mismatches=%llu\nU8 v=[%d,%d] pairs=%llu mismatches=%llu\n", vlo, vhi, n, bad[0], vlo, vhi, n, bad[1]);
		return (bad[0] | bad[1]) != 0;
	}
	if (!strcmp(m, "bench")) { /* bench which fmt v n reps */
		int which = atoi(argv[2]); Uint16 fmt = strtol(argv[3], 0, 0); int v = atoi(argv[4]), n = atoi(argv[5]), reps = atoi(argv[6]);
		Uint8 *d = malloc(n), *s = malloc(n); unsigned x = 12345;
		for (int i = 0; i < n; i++) { x = x * 1103515245 + 12345; d[i] = x >> 16; x = x * 1103515245 + 12345; s[i] = x >> 16; }
		for (int r = 0; r < reps; r++) { if (which == 1) ref_mix(d, s, n, v, fmt); else if (which == 2) twin_mix(d, s, n, v, fmt); }
		printf("%u\n", d[n / 2]);
		return 0;
	}
	return 2;
}
