/*
 * rsbench - cost of s31route's converter (rootfs/s31resample.h) on the board,
 * in ns and cycles (hart clock 320 MHz) per OUTPUT frame, for the paths the
 * plugin uses: resample mono / stereo and the mono -> stereo dup. Also checks
 * that the stereo path with L == R reproduces the mono path bit for bit.
 * Input is fed in 512-frame chunks, like an app period. Single precision.
 *
 *   rsbench [seconds_of_audio_per_case]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "s31resample.h"

static long long ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec * 1000000000ll + t.tv_nsec;
}

int main(int argc, char **argv)
{
	static const unsigned pairs[][2] = {
		{36000, 48000}, {12000, 16000}, {37800, 48000}, {40000, 48000},
	};
	int secs = argc > 1 ? atoi(argv[1]) : 4;
	unsigned pi, i;

	printf("rsbench taps %d\n", S31RS_TAPS);
	for (pi = 0; pi < sizeof(pairs) / sizeof(pairs[0]); pi++) {
		unsigned in = pairs[pi][0], out = pairs[pi][1], ch;
		unsigned n = in * secs;
		int16_t *x = malloc(n * 4), *y = malloc((size_t)(out * secs + 64) * 4);
		int16_t *ym = malloc((size_t)(out * secs + 64) * 4);
		unsigned seed = 1;

		for (i = 0; i < n; i++) {
			seed = seed * 1103515245u + 12345u;
			x[2 * i] = x[2 * i + 1] = (int16_t)((seed >> 16) & 0x3fff) - 0x2000;
		}
		for (ch = 1; ch <= 2; ch++) {
			struct s31rs rs;
			unsigned off = 0, got = 0;
			long long t0, t1;
			int16_t *mono = NULL;

			if (s31rs_init(&rs, in, out, ch) < 0) {
				printf("%u->%u ch %u: refused\n", in, out, ch);
				continue;
			}
			if (ch == 1) {	/* mono input = the left channel */
				mono = malloc(n * 2);
				for (i = 0; i < n; i++)
					mono[i] = x[2 * i];
			}
			t0 = ns();
			while (off < n) {
				unsigned c = n - off < 512 ? n - off : 512;

				got += s31rs_run(&rs, ch == 1 ? mono + off : x + 2 * off,
						 c, (ch == 1 ? ym : y) + 2 * got);
				off += c;
			}
			t1 = ns();
			printf("%u->%u ch %u: %u out frames, %.1f ns/frame = %.0f cycles/frame, %.2f%% of a core at %u Hz\n",
			       in, out, ch, got, (float)(t1 - t0) / (float)got,
			       (float)(t1 - t0) / (float)got * 0.32f,
			       (float)(t1 - t0) / (float)got * (float)out / 1e7f, out);
			if (ch == 2) {
				unsigned d = 0;

				for (i = 0; i < 2 * got; i++)
					if (y[i] != ym[i])
						d++;
				printf("   stereo(L=R) vs mono: %u differing samples of %u\n", d, 2 * got);
			}
			free(mono);
			s31rs_free(&rs);
		}
		free(x);
		free(y);
		free(ym);
	}
	{
		unsigned n = 48000 * secs;
		int16_t *m = calloc(n, 2), *o = malloc(n * 4);
		long long t0 = ns(), t1;
		unsigned off;

		for (off = 0; off < n; off += 512)
			s31rs_dup(m + off, n - off < 512 ? n - off : 512, o + 2 * off);
		t1 = ns();
		printf("dup mono->stereo: %.1f ns/frame = %.0f cycles/frame\n",
		       (float)(t1 - t0) / (float)n, (float)(t1 - t0) / (float)n * 0.32f);
	}
	return 0;
}
