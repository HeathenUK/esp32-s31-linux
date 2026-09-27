/*
 * wavgen - a short musical test signal as a WAV on stdout, for listening
 * checks of the audio path at a given rate (artifacts/audio/
 * first-principles-2026-09-27, IMPLEMENT). Single precision only.
 *
 *   wavgen <rate> <channels> | aplay -q
 *
 * Contents (~4.5 s):
 *   0.0-1.0 s  A4 = 440 Hz, pure, both channels: the PITCH reference. At the
 *              right rate it is concert A; a rate error shows as a flat or
 *              sharp A (32 kHz played through 96 kHz was ~19% flat).
 *   1.0-2.6 s  C5 E5 G5 C6 arpeggio, bright plucked tone (harmonics up to
 *              0.4 of the rate): stereo = LEFT only.
 *   2.6-3.4 s  the same, faster, RIGHT only (stereo) - mono plays both.
 *   3.4-4.5 s  C major chord, both channels, decaying.
 * Anything else heard - hiss under the notes, a rough/metallic edge on the
 * bright notes (rate-converter images), clicks, gaps - is a fault.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TAB 4096
static float sine[TAB];

static float sinpi(float t)	/* sin(pi t), float, as in s31resample.h */
{
	float x, x2;
	int k = (int)(t * 0.5f + (t >= 0.0f ? 0.5f : -0.5f));

	t -= 2.0f * (float)k;
	if (t > 0.5f)
		t = 1.0f - t;
	else if (t < -0.5f)
		t = -1.0f - t;
	x = 3.14159265f * t;
	x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
		x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

static float osc(float ph)	/* ph in cycles */
{
	ph -= (float)(int)ph;
	return sine[(int)(ph * TAB) & (TAB - 1)];
}

/* plucked: harmonics 1..n with 1/k amplitude, all below 0.4 * rate */
static float pluck(float f, float t, float rate, float decay)
{
	float s = 0.0f, env = 1.0f;
	int k;

	for (k = 1; k <= 12 && f * (float)k < 0.4f * rate; k++)
		s += osc(f * (float)k * t) / (float)k;
	/* exp(-t*decay) by a float series is enough here */
	{
		float x = t * decay, e = 1.0f, term = 1.0f;
		int i;

		for (i = 1; i < 20; i++) {
			term *= -x / (float)i;
			e += term;
		}
		env = e > 0.0f ? e : 0.0f;
		if (x > 6.0f)
			env = 0.0f;
	}
	return s * env * 0.12f;
}

static void put32(uint32_t v) { fwrite(&v, 4, 1, stdout); }
static void put16(uint16_t v) { fwrite(&v, 2, 1, stdout); }

int main(int argc, char **argv)
{
	unsigned rate, ch, n, i;
	float fr, C5 = 523.25f, E5 = 659.26f, G5 = 783.99f, C6 = 1046.5f;
	const float arp[4] = { 523.25f, 659.26f, 783.99f, 1046.5f };

	if (argc < 3)
		return fprintf(stderr, "usage: wavgen rate channels\n"), 1;
	rate = (unsigned)atoi(argv[1]);
	ch = (unsigned)atoi(argv[2]);
	if (rate < 8000 || rate > 96000 || ch < 1 || ch > 2)
		return 1;
	for (i = 0; i < TAB; i++)
		sine[i] = sinpi(2.0f * (float)i / TAB);
	fr = (float)rate;
	n = rate * 45 / 10;
	fwrite("RIFF", 4, 1, stdout); put32(36 + n * ch * 2);
	fwrite("WAVEfmt ", 8, 1, stdout); put32(16); put16(1); put16((uint16_t)ch);
	put32(rate); put32(rate * ch * 2); put16((uint16_t)(ch * 2)); put16(16);
	fwrite("data", 4, 1, stdout); put32(n * ch * 2);
	for (i = 0; i < n; i++) {
		float t = (float)i / fr, l = 0.0f, r = 0.0f;
		int16_t s[2];
		int c;

		if (t < 1.0f) {
			float a = t < 0.02f ? t / 0.02f : t > 0.95f ? (1.0f - t) / 0.05f : 1.0f;

			l = r = 0.3f * a * osc(440.0f * t);
		} else if (t < 2.6f) {
			float u = t - 1.0f;
			int k = (int)(u / 0.4f);

			l = pluck(arp[k > 3 ? 3 : k], u - 0.4f * (float)k, fr, 5.0f);
			r = ch == 1 ? l : 0.0f;
		} else if (t < 3.4f) {
			float u = t - 2.6f;
			int k = (int)(u / 0.2f);

			r = pluck(arp[k > 3 ? 3 : k], u - 0.2f * (float)k, fr, 8.0f);
			l = ch == 1 ? r : 0.0f;
		} else {
			float u = t - 3.4f;

			l = r = 0.4f * (pluck(C5, u, fr, 3.0f) + pluck(E5, u, fr, 3.0f) +
					pluck(G5, u, fr, 3.0f) + pluck(C6, u, fr, 3.0f));
		}
		if (ch == 1)
			l = 0.5f * (l + r);
		l = l > 0.9f ? 0.9f : l < -0.9f ? -0.9f : l;
		r = r > 0.9f ? 0.9f : r < -0.9f ? -0.9f : r;
		s[0] = (int16_t)(l * 32000.0f);
		s[1] = (int16_t)(r * 32000.0f);
		for (c = 0; c < (int)ch; c++)
			fwrite(&s[c], 2, 1, stdout);
	}
	return 0;
}
