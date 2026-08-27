// SPDX-License-Identifier: GPL-2.0-only
/*
 * Is there a tone in this recording, and at what frequency?
 *
 * Written because the board can hear itself - the Korvo-1's mic array is on
 * the same codec as the speaker - but has no way to send a WAV back: there is
 * no nc and no httpd here, and 128 KB of base64 does not survive the console.
 * So the analysis happens on the board and only the numbers come back.
 *
 * A Goertzel filter per frequency of interest, plus overall RMS. That is
 * enough to answer the only question that matters: is the energy concentrated
 * at the tone we played, or spread across the band like noise?
 *
 *   tonecheck <file.wav> [rate] [f1 f2 ...]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Magnitude of one frequency bin, normalised to the number of samples. */
static double goertzel(const short *x, int n, double freq, double rate)
{
	double w = 2.0 * M_PI * freq / rate;
	double c = 2.0 * cos(w);
	double s0, s1 = 0, s2 = 0;
	int i;

	for (i = 0; i < n; i++) {
		s0 = x[i] + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (n / 2.0);
}

int main(int argc, char **argv)
{
	static const double defaults[] = { 110, 220, 440, 880, 1320, 2000, 4000 };
	double rate = argc > 2 ? atof(argv[2]) : 16000.0;
	short *mono;
	unsigned char hdr[44];
	FILE *f;
	long n, i, frames;
	double rms = 0, peak_mag = 0, peak_f = 0, total = 0;
	int nf, k;
	const double *freqs;
	double parsed[16];

	if (argc < 2) {
		fprintf(stderr, "usage: tonecheck file.wav [rate] [f1 f2 ...]\n");
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f) { perror("open"); return 1; }
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fprintf(stderr, "short file\n"); return 1; }

	fseek(f, 0, SEEK_END);
	n = (ftell(f) - 44) / 2;		/* 16-bit samples, both channels */
	fseek(f, 44, SEEK_SET);
	mono = malloc(n * sizeof(*mono));
	if (!mono) return 1;
	if (fread(mono, 2, n, f) != (size_t)n) { /* short read is fine */ }
	fclose(f);

	/* Fold stereo to mono in place; the mics are summed for this purpose. */
	frames = n / 2;
	for (i = 0; i < frames; i++)
		mono[i] = (short)(((int)mono[i * 2] + mono[i * 2 + 1]) / 2);

	for (i = 0; i < frames; i++)
		rms += (double)mono[i] * mono[i];
	rms = sqrt(rms / frames);

	if (argc > 3) {
		nf = argc - 3;
		if (nf > 16) nf = 16;
		for (k = 0; k < nf; k++)
			parsed[k] = atof(argv[3 + k]);
		freqs = parsed;
	} else {
		nf = sizeof(defaults) / sizeof(defaults[0]);
		freqs = defaults;
	}

	printf("frames=%ld rate=%.0f rms=%.1f (%.2f%% of full scale)\n",
	       frames, rate, rms, 100.0 * rms / 32768.0);
	for (k = 0; k < nf; k++) {
		double m = goertzel(mono, frames, freqs[k], rate);

		total += m;
		if (m > peak_mag) { peak_mag = m; peak_f = freqs[k]; }
		printf("  %7.1f Hz : %10.1f\n", freqs[k], m);
	}
	/*
	 * A real tone puts most of the measured energy in one bin. Noise
	 * spreads it, so the ratio is the answer, not the absolute numbers -
	 * which depend on how loud the speaker is and how far away the mic is.
	 */
	printf("peak %.1f Hz, %.0f%% of the measured energy in that bin\n",
	       peak_f, total > 0 ? 100.0 * peak_mag / total : 0.0);
	return 0;
}
