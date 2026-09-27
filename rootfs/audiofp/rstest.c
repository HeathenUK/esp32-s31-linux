/*
 * rstest - host-side quality sweep of rootfs/s31resample.h (the s31route
 * converter) against plain linear interpolation (what alsa-lib's "linear"
 * converter does). HOST ONLY: this uses double for the analysis, never the
 * board.
 *
 *   cc -O2 -I rootfs -o /tmp/rstest rootfs/audiofp/rstest.c -lm && /tmp/rstest
 *
 * For each rate pair and each test tone (amplitude 16000 of 32767, 1 s,
 * fed in 512-frame chunks so the carried state is exercised), it fits the
 * tone at the output by least squares (DC + cos + sin at the exact input
 * frequency) and reports:
 *   gain   passband gain of the tone, dB
 *   resid  everything that is not the tone - images, aliasing, rounding -
 *          relative to the tone, dB (int16 rounding alone is ~-99 dB)
 */
#include <math.h>
#include <stdio.h>
#include "s31resample.h"

#define SECS 1

static int lin(const int16_t *x, unsigned n, unsigned in, unsigned out,
	       int16_t *y)
{
	unsigned k = 0;

	for (;; k++) {
		double pos = (double)k * in / out;
		unsigned i = (unsigned)pos;
		double f = pos - i, v;

		if (i + 1 >= n)
			break;
		v = x[i] + (x[i + 1] - x[i]) * f;
		y[2 * k] = y[2 * k + 1] = (int16_t)lrint(v);
	}
	return (int)k;
}

static void fit(const int16_t *y, int n, int skip, double w, double *gain,
		double *resid)
{
	/* 3x3 normal equations for [1, cos, sin] */
	double A[3][3] = {{0}}, B[3] = {0}, c[3], sig = 0, res = 0;
	int k, i, j;

	for (k = skip; k < n; k++) {
		double v[3] = {1, cos(w * k), sin(w * k)};

		for (i = 0; i < 3; i++) {
			for (j = 0; j < 3; j++)
				A[i][j] += v[i] * v[j];
			B[i] += v[i] * y[2 * k];
		}
	}
	/* solve by Cramer */
	{
		double d = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) -
			   A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
			   A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
		for (i = 0; i < 3; i++) {
			double M[3][3];
			int r, s;

			for (r = 0; r < 3; r++)
				for (s = 0; s < 3; s++)
					M[r][s] = s == i ? B[r] : A[r][s];
			c[i] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) -
				M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
				M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / d;
		}
	}
	for (k = skip; k < n; k++) {
		double t = c[0] + c[1] * cos(w * k) + c[2] * sin(w * k);

		sig += (t - c[0]) * (t - c[0]);
		res += (y[2 * k] - t) * (y[2 * k] - t);
	}
	*gain = 20 * log10(sqrt(c[1] * c[1] + c[2] * c[2]) / 16000.0);
	*resid = 10 * log10(res / sig + 1e-30);
}

int main(int argc, char **argv)
{
	static const unsigned pairs[][2] = {
		{36000, 48000}, {12000, 16000}, {37800, 48000},
		{33075, 44100}, {40000, 48000}, {30000, 32000},
	};
	static const double fr[] = {0.02, 0.1, 0.2, 0.3, 0.35, 0.4, 0.45};
	unsigned pi;

	printf("taps %d beta %.1f cutoff %.2f\n", S31RS_TAPS,
	       (double)S31RS_BETA, (double)S31RS_CUTOFF);
	for (pi = 0; pi < sizeof(pairs) / sizeof(pairs[0]); pi++) {
		unsigned in = pairs[pi][0], out = pairs[pi][1], fi;
		struct s31rs rs;
		unsigned n = in * SECS;
		int16_t *x = malloc(n * 2), *y = malloc((out * SECS + 64) * 4);
		int16_t *z = malloc((out * SECS + 64) * 4);

		if (s31rs_init(&rs, in, out, 1) < 0) {
			printf("%u->%u: init refused\n", in, out);
			continue;
		}
		printf("%u -> %u (L %u M %u)\n", in, out, rs.L, rs.M);
		printf("  tone/fs_in    Hz     poly gain  resid   | linear gain  resid\n");
		for (fi = 0; fi < sizeof(fr) / sizeof(fr[0]); fi++) {
			double f = fr[fi] * in, g1, r1, g2, r2;
			unsigned i, off = 0;
			int got = 0, ln;

			for (i = 0; i < n; i++)
				x[i] = (int16_t)lrint(16000 * sin(2 * M_PI * f * i / in));
			s31rs_reset(&rs);
			while (off < n) {
				unsigned c = n - off < 512 ? n - off : 512;

				got += s31rs_run(&rs, x + off, c, y + 2 * got);
				off += c;
			}
			ln = lin(x, n, in, out, z);
			fit(y, got, 2000, 2 * M_PI * f / out, &g1, &r1);
			fit(z, ln, 2000, 2 * M_PI * f / out, &g2, &r2);
			printf("  %.2f  %8.0f   %6.2f  %6.1f   |  %6.2f  %6.1f\n",
			       fr[fi], f, g1, r1, g2, r2);
		}
		s31rs_free(&rs);
		free(x);
		free(y);
		free(z);
	}
	return 0;
}
