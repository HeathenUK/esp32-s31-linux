/*
 * s31resample.h - the rate converter and channel router inside s31route.
 *
 * WHY THIS EXISTS (artifacts/audio/first-principles-2026-09-27, option 3)
 * Until 2026-09-27 every stream that did not match the codec exactly went
 * through alsa-lib's plug: layer, which routed mono to stereo (~110 cycles an
 * output frame) and, for rates the codec has no clock row for, ran the
 * builtin "linear" converter: ~590 cycles an output frame measured, images
 * only ~23 dB down near the top of the band, and a nearest-rate choice that
 * sent 36000 to 32000 (a DOWNWARD conversion, band-limiting 36 kHz content to
 * 16 kHz) once the codec learned 32000. This does both jobs itself.
 *
 * WHAT IT IS
 * A rational polyphase FIR: output rate / input rate = L / M in lowest terms,
 * L phases of S31RS_TAPS taps each, cut from one Kaiser-windowed sinc
 * prototype. Every phase is normalised to unity DC gain. Output sample k sits
 * at input position k*M/L; the integer part picks the input window and the
 * remainder picks the phase, so the rate is exact and there is no drift.
 *
 * SINGLE PRECISION ONLY. This core has F and no D, so float is hardware and
 * double is a library call (__muldf3 ~723 ns vs ~24 ns). Nothing here is
 * double, including the one-off table construction: sin(pi x) and I0 are
 * computed with float polynomials/series below rather than musl's sinf,
 * which evaluates in double and costs ~9 us a call on this board. Build with
 * -Wdouble-promotion -fsingle-precision-constant and check the object for
 * __*df* references (build-s31route.sh does).
 *
 * Header-only so that the plugin, the board benchmark (rootfs/audiofp/
 * rsbench.c) and the host quality sweep (rootfs/audiofp/rstest.c) compile
 * the identical code.
 */
#ifndef S31RESAMPLE_H
#define S31RESAMPLE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef S31RS_TAPS
#define S31RS_TAPS	24	/* taps per phase, at the input rate */
#endif
#ifndef S31RS_BETA
#define S31RS_BETA	7.0f	/* Kaiser beta: ~70 dB stopband */
#endif
#ifndef S31RS_CUTOFF
#define S31RS_CUTOFF	0.5f	/* -6 dB point, fraction of the INPUT rate */
#endif
#define S31RS_MAX_L	160	/* phases; 37800 -> 48000 needs 80 */

struct s31rs {
	unsigned L, M;		/* out/in = L/M, coprime */
	unsigned ch;		/* input channels, 1 or 2; output is always 2 */
	unsigned p;		/* phase of the next output, 0..L-1 */
	int n;			/* newest input index the next output needs */
	unsigned cap;		/* new-sample capacity of x[] */
	float *h;		/* L * TAPS, h[p*TAPS + j] weights x[n-TAPS+1+j] */
	float *x[2];		/* TAPS-1 of history, then this call's input */
};

static inline unsigned s31rs_gcd(unsigned a, unsigned b)
{
	while (b) {
		unsigned t = a % b;

		a = b;
		b = t;
	}
	return a;
}

/* sin(pi t), float only; |error| < 1e-7 over all t. */
static inline float s31rs_sinpi(float t)
{
	float x, x2;
	int k = (int)(t * 0.5f + (t >= 0.0f ? 0.5f : -0.5f));

	t -= 2.0f * (float)k;			/* t in [-1, 1] */
	if (t > 0.5f)
		t = 1.0f - t;
	else if (t < -0.5f)
		t = -1.0f - t;
	x = 3.14159265f * t;			/* |x| <= pi/2 */
	x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
		x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f +
		x2 * (-1.0f / 39916800.0f))))));
}

/* Modified Bessel I0 by its power series; converges fast for x <= 12. */
static inline float s31rs_i0(float x)
{
	float sum = 1.0f, term = 1.0f, q = x * x * 0.25f;
	int m;

	for (m = 1; m < 32; m++) {
		term *= q / ((float)m * (float)m);
		sum += term;
		if (term < sum * 1e-8f)
			break;
	}
	return sum;
}

static inline float s31rs_sqrt(float v)
{
	return v > 0.0f ? __builtin_sqrtf(v) : 0.0f;	/* fsqrt.s */
}

static inline void s31rs_free(struct s31rs *rs)
{
	free(rs->h);
	free(rs->x[0]);
	free(rs->x[1]);
	memset(rs, 0, sizeof(*rs));
}

static inline void s31rs_reset(struct s31rs *rs)
{
	unsigned c;

	rs->p = 0;
	rs->n = S31RS_TAPS - 1;
	for (c = 0; c < rs->ch; c++)
		if (rs->x[c])
			memset(rs->x[c], 0, (S31RS_TAPS - 1) * sizeof(float));
}

/*
 * Set up in_rate -> out_rate for `ch` input channels. Upsampling (and equal
 * rates) only: the caller picks an output rate at or above the input.
 * Returns 0, or -1 if the ratio needs more than S31RS_MAX_L phases or memory
 * ran out - the caller then leaves conversion to alsa-lib as before.
 */
static inline int s31rs_init(struct s31rs *rs, unsigned in_rate, unsigned out_rate,
		      unsigned ch)
{
	unsigned g, L, M, N, k, p, j;
	float c, i0b, fc;

	memset(rs, 0, sizeof(*rs));
	if (!in_rate || out_rate < in_rate || ch < 1 || ch > 2)
		return -1;
	g = s31rs_gcd(out_rate, in_rate);
	L = out_rate / g;
	M = in_rate / g;
	if (L > S31RS_MAX_L)
		return -1;
	rs->L = L;
	rs->M = M;
	rs->ch = ch;
	N = L * S31RS_TAPS;
	rs->h = malloc(N * sizeof(float));
	if (!rs->h)
		return -1;
	/*
	 * Prototype at the fine rate L * in_rate: sinc with its -6 dB point at
	 * S31RS_CUTOFF * in_rate, i.e. S31RS_CUTOFF / L of the fine rate,
	 * under a Kaiser window. Tap k of the prototype belongs to phase
	 * k % L, input offset k / L.
	 */
	c = (float)(N - 1) * 0.5f;
	fc = 2.0f * S31RS_CUTOFF / (float)L;	/* cycles/sample * 2 */
	i0b = s31rs_i0(S31RS_BETA);
	for (p = 0; p < L; p++) {
		float sum = 0.0f;

		for (j = 0; j < S31RS_TAPS; j++) {
			/* h[p][j] weights x[n - (TAPS-1) + j]: prototype tap
			 * (TAPS-1-j)*L + p */
			float t, w, r, v;

			k = (S31RS_TAPS - 1 - j) * L + p;
			t = (float)k - c;
			r = 2.0f * (float)k / (float)(N - 1) - 1.0f;
			w = s31rs_i0(S31RS_BETA * s31rs_sqrt(1.0f - r * r)) / i0b;
			v = t == 0.0f ? fc : s31rs_sinpi(fc * t) / (3.14159265f * t);
			rs->h[p * S31RS_TAPS + j] = v * w;
			sum += v * w;
		}
		for (j = 0; j < S31RS_TAPS; j++)
			rs->h[p * S31RS_TAPS + j] /= sum;
	}
	s31rs_reset(rs);
	return 0;
}

static inline int s31rs_reserve(struct s31rs *rs, unsigned frames)
{
	unsigned c;

	if (frames <= rs->cap)
		return 0;
	for (c = 0; c < rs->ch; c++) {
		float *nx = realloc(rs->x[c],
				(S31RS_TAPS - 1 + frames) * sizeof(float));

		if (!nx)
			return -1;
		if (!rs->x[c])
			memset(nx, 0, (S31RS_TAPS - 1) * sizeof(float));
		rs->x[c] = nx;
	}
	rs->cap = frames;
	return 0;
}

/* Upper bound on the output of s31rs_run() for `frames` of input. */
static inline unsigned s31rs_max_out(const struct s31rs *rs, unsigned frames)
{
	return (unsigned)(((uint64_t)frames * rs->L + rs->M - 1) / rs->M) + 1;
}

static inline int16_t s31rs_s16(float y)
{
	int v = (int)(y + (y >= 0.0f ? 0.5f : -0.5f));	/* fcvt.w.s, rtz */

	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

#if S31RS_TAPS % 4
#error S31RS_TAPS must be a multiple of 4
#endif
static inline float s31rs_dot(const float *h, const float *a)
{
	float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
	unsigned j;

#pragma GCC unroll 8
	for (j = 0; j < S31RS_TAPS; j += 4) {
		a0 += h[j] * a[j];
		a1 += h[j + 1] * a[j + 1];
		a2 += h[j + 2] * a[j + 2];
		a3 += h[j + 3] * a[j + 3];
	}
	return (a0 + a1) + (a2 + a3);
}

/*
 * Convert `frames` interleaved S16 frames of rs->ch channels into
 * interleaved stereo S16 at the output rate. Returns the frames written to
 * out (at most s31rs_max_out()), or -1 on allocation failure.
 */
static inline int s31rs_run(struct s31rs *rs, const int16_t *in, unsigned frames,
		     int16_t *out)
{
	const unsigned T = S31RS_TAPS;
	unsigned i, total, o = 0;
	float *x0, *x1;

	if (s31rs_reserve(rs, frames) < 0)
		return -1;
	x0 = rs->x[0] + T - 1;
	x1 = rs->ch == 2 ? rs->x[1] + T - 1 : NULL;
	if (x1)
		for (i = 0; i < frames; i++) {
			x0[i] = (float)in[2 * i];
			x1[i] = (float)in[2 * i + 1];
		}
	else
		for (i = 0; i < frames; i++)
			x0[i] = (float)in[i];
	total = T - 1 + frames;
	x0 = rs->x[0];
	x1 = rs->ch == 2 ? rs->x[1] : NULL;
	while ((unsigned)rs->n < total) {
		const float *h = rs->h + rs->p * T;
		const float *a = x0 + rs->n - (T - 1);

		/*
		 * Four independent accumulators, unrolled: one fmadd.s chain
		 * of T taps stalls on its own latency (measured 270 cycles an
		 * output frame mono at T=24, ~11 a tap); split four ways the
		 * loads and multiply-adds overlap.
		 */
		if (x1) {
			const float *b = x1 + rs->n - (T - 1);

			out[2 * o] = s31rs_s16(s31rs_dot(h, a));
			out[2 * o + 1] = s31rs_s16(s31rs_dot(h, b));
		} else {
			out[2 * o] = out[2 * o + 1] = s31rs_s16(s31rs_dot(h, a));
		}
		o++;
		rs->p += rs->M;
		while (rs->p >= rs->L) {
			rs->p -= rs->L;
			rs->n++;
		}
	}
	/* keep the last T-1 inputs as the next call's history */
	memmove(rs->x[0], rs->x[0] + frames, (T - 1) * sizeof(float));
	if (x1)
		memmove(rs->x[1], rs->x[1] + frames, (T - 1) * sizeof(float));
	rs->n -= (int)frames;
	return (int)o;
}

/* Mono to stereo at the same rate: the only other thing plug: did for us. */
static inline void s31rs_dup(const int16_t *in, unsigned frames, int16_t *out)
{
	unsigned i;

	for (i = 0; i < frames; i++)
		out[2 * i] = out[2 * i + 1] = in[i];
}

#endif
