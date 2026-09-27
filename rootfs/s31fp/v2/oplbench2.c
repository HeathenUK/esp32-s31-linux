/*
 * oplbench2 = rootfs/audiofp/oplbench.c plus an FNV-1a hash of every output
 * sample (printed as the last column), so the four helper builds can be shown
 * to synthesise bit-identical audio. The hash costs ~1 multiply per sample.
 *
 * oplbench - time OpenTyrian's own OPL2 synthesis loop (its opl.c and
 * lds_play.c, compiled unmodified from the Buildroot tarball next to this
 * file) the way its SDL audio callback drives it: 44100 Hz mono, 2048-sample
 * callbacks, lds_update() at 70 Hz. Measures CPU time per output sample and
 * the number of active operators (by envelope state), so the soft-double cost
 * can be derived per operator rather than guessed. An instrument, not a port.
 *
 *   oplbench <music.mus> <song|-1 for all> <seconds> [rate]
 *   oplbench -r      per-helper costs (__muldf3 etc.) with OPL-shaped operands
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "opl.h"
#include "lds_play.h"

float music_volume = 1.0f;
unsigned int song_playing;
bool audio_disabled, music_disabled, samples_disabled;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Per-helper cost with operands shaped like the synthesiser's. */
static double wnow(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void routines(void)
{
	volatile double a = 0.73125, b = 3.0517578125e-05, c, q = 0.0625;
	volatile int wv = 12345, iv;
	volatile unsigned long base = 0;
	const int N = 400000;
	double t0, t, ovh;
	int i;

	t0 = wnow(); for (i = 0; i < N; i++) base += (unsigned)i; ovh = (wnow() - t0) / N;
	t0 = wnow(); for (i = 0; i < N; i++) c = a * b; t = (wnow() - t0) / N - ovh;
	printf("mul general    %.0f ns\n", t * 1e9);
	t0 = wnow(); for (i = 0; i < N; i++) c = a * (double)(i & 0x7fff); t = (wnow() - t0) / N - ovh;
	printf("floatsidf+mul  %.0f ns\n", t * 1e9);
	t0 = wnow(); for (i = 0; i < N; i++) c = (double)(i & 0x7fff); t = (wnow() - t0) / N - ovh;
	printf("floatsidf      %.0f ns\n", t * 1e9);
	t0 = wnow(); for (i = 0; i < N; i++) c = a * q; t = (wnow() - t0) / N - ovh;
	printf("mul by 1/16    %.0f ns\n", t * 1e9);
	t0 = wnow(); for (i = 0; i < N; i++) iv = (int)(a * 16384.0); t = (wnow() - t0) / N - ovh;
	printf("mul+fixdfsi    %.0f ns\n", t * 1e9);
	t0 = wnow(); for (i = 0; i < N; i++) iv = a > b; t = (wnow() - t0) / N - ovh;
	printf("gtdf2          %.0f ns\n", t * 1e9);
	t0 = wnow(); for (i = 0; i < N; i++) c = a + b; t = (wnow() - t0) / N - ovh;
	printf("adddf3         %.0f ns\n", t * 1e9);
	(void)wv; (void)iv; (void)c;
}

int main(int argc, char **argv)
{
	FILE *f;
	unsigned short count;
	unsigned int *off;
	int song, s0, s1, secs, rate;
	long ct;
	static Bit16s buf[2048];

	if (argc > 1 && !strcmp(argv[1], "-r")) {
		routines();
		return 0;
	}
	if (argc < 4) {
		fprintf(stderr, "usage: %s music.mus song|-1 seconds [rate]\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fread(&count, 2, 1, f);
	off = malloc((count + 1) * 4);
	fread(off, 4, count, f);
	fseek(f, 0, SEEK_END);
	off[count] = ftell(f);
	song = atoi(argv[2]);
	secs = atoi(argv[3]);
	rate = argc > 4 ? atoi(argv[4]) : 44100;
	s0 = song < 0 ? 0 : song;
	s1 = song < 0 ? count - 1 : song;
	printf("# song rate secs cpu_s us_per_sample realtime_x ops_avg ops_max att dec sus rel hash\n");
	for (song = s0; song <= s1; song++) {
		long total = (long)secs * rate, done = 0;
		double t0, t;
		unsigned long opsum = 0, opmax = 0, st[6] = {0}, blocks = 0;
		unsigned int h = 2166136261u;

		adlib_init(rate);
		if (!lds_load(f, off[song], off[song + 1] - off[song]))
			continue;
		ct = 0;
		t0 = now();
		while (done < total) {
			/* the body of loudness.c audio_cb, music part, for 2048 samples */
			Bit16s *pos = buf;
			long remaining = 2048;
			while (remaining > 0) {
				while (ct < 0) {
					ct += rate;
					lds_update();
				}
				long i = (long)((ct / REFRESH) + 4) & ~3;
				i = (i > remaining) ? remaining : i;
				adlib_getsample(pos, i);
				pos += i;
				remaining -= i;
				ct -= (long)(REFRESH * i);
#ifdef OPLSTATS	/* host only: op[] is static once Buildroot's 0001 patch is applied */
				{
					unsigned n = 0;
					for (int k = 0; k < MAXOPERATORS; k++) {
						st[op[k].op_state]++;
						n += op[k].op_state != OF_TYPE_OFF;
					}
					opsum += n; blocks++;
					if (n > opmax) opmax = n;
				}
#else
				blocks++;
#endif
			}
			for (int smp = 0; smp < 2048; smp++) {
				buf[smp] *= music_volume;
				h = (h ^ (unsigned short)buf[smp]) * 16777619u;
			}
			done += 2048;
		}
		t = now() - t0;
		printf("%d %d %d %.3f %.2f %.3f %.2f %lu %.2f %.2f %.2f %.2f %08x\n", song, rate, secs, t,
		       t * 1e6 / done, (double)done / rate / t, (double)opsum / blocks, opmax,
		       (double)st[0] / blocks, (double)st[1] / blocks, (double)st[3] / blocks,
		       (double)(st[2] + st[4]) / blocks, h);
		fflush(stdout);
	}
	return 0;
}
