/*
 * sdltone - an SDL audio client whose only job is to be measured.
 *
 *   sdltone <rate> <channels> <samples> <secs> [load_iters_per_sample] [hog]
 *
 * Opens the default device through SDL exactly the way a game does
 * (SDL_OpenAudio, S16, callback), plays a quiet 440 Hz sine, and optionally
 * burns a fixed amount of integer work per output sample inside the callback
 * so a synthesiser of known cost can be emulated. It prints, per run: what
 * SDL granted, the callback's CPU cost (thread CPU clock) per sample, the
 * largest gap between callbacks against the period, the CPUs the callback ran
 * on, and how often it moved. Underruns are counted from alsa-lib's own
 * "underrun occurred" lines on stderr (snd_pcm_recover with silent=0, which is
 * what SDL 1.2 and SDL 2 both call), so nothing here needs to be trusted for
 * that. hog=N adds N CPU-bound spinner threads, standing in for a game loop
 * (and, with N=2, for the desktop drawing it).
 *
 * The same source builds against SDL 1.2 and SDL 2 (both keep SDL_OpenAudio).
 * An instrument (ours), not a client: nothing on the board is changed by it.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>

static volatile unsigned long sink;
static unsigned long iters, calls, late, moves, cpumask;
static int lastcpu = -1, tid;
static double period_s, maxgap, lastwall, cpu_s, sumgap, thr_first, thr_last;
static float phase, step;
static int chans;

static double wall(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static double tcpu(void)
{
	struct timespec t;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void cb(void *u, Uint8 *stream, int len)
{
	Sint16 *s = (Sint16 *)stream;
	int frames = len / 2 / chans, i, c;
	double w = wall(), c0 = tcpu();

	/* thread CPU between callback entries = callback + SDL + alsa-lib +
	 * plugin + kernel write path; minus the callback = the platform path */
	if (!calls)
		thr_first = c0;
	thr_last = c0;
	int cpu = sched_getcpu();

	(void)u;
	if (!tid)
		tid = (int)syscall(SYS_gettid);
	if (lastwall > 0) {
		double g = w - lastwall;
		sumgap += g;
		if (g > maxgap)
			maxgap = g;
		if (g > 1.5 * period_s)
			late++;
	}
	lastwall = w;
	if (cpu >= 0) {
		if (lastcpu >= 0 && cpu != lastcpu)
			moves++;
		lastcpu = cpu;
		cpumask |= 1ul << cpu;
	}
	for (i = 0; i < frames; i++) {
		Sint16 v = (Sint16)(1200.0f * sinf(phase));
		unsigned long k, a = sink;

		phase += step;
		if (phase > 6.2831853f)
			phase -= 6.2831853f;
		for (k = 0; k < iters; k++)
			a = a * 2654435761u + k;
		sink = a;
		for (c = 0; c < chans; c++)
			*s++ = v;
	}
	calls++;
	cpu_s += tcpu() - c0;
}

static volatile int stop;

static void *spinner(void *u)
{
	volatile double x = 0;

	(void)u;
	while (!stop)
		x += 1.0;
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t th[8];
	int nth = 0;
	SDL_AudioSpec want, got;
	int secs, hog;
	double t0;

	if (argc < 5) {
		fprintf(stderr, "usage: %s rate channels samples secs [iters/sample] [hog]\n", argv[0]);
		return 2;
	}
	memset(&want, 0, sizeof(want));
	want.freq = atoi(argv[1]);
	want.channels = (Uint8)atoi(argv[2]);
	want.samples = (Uint16)atoi(argv[3]);
	want.format = AUDIO_S16SYS;
	want.callback = cb;
	secs = atoi(argv[4]);
	iters = argc > 5 ? strtoul(argv[5], NULL, 0) : 0;
	hog = argc > 6 ? atoi(argv[6]) : 0;
	if (SDL_Init(SDL_INIT_AUDIO) < 0 || SDL_OpenAudio(&want, &got) < 0) {
		fprintf(stderr, "sdltone: %s\n", SDL_GetError());
		return 1;
	}
	chans = got.channels;
	step = 6.2831853f * 440.0f / (float)got.freq;
	period_s = (double)got.samples / got.freq;
	printf("sdltone: want %d Hz %d ch %d samples; got %d Hz %d ch %d samples (%.1f ms/callback) iters %lu hog %d\n",
	       want.freq, want.channels, want.samples, got.freq, got.channels, got.samples,
	       period_s * 1e3, iters, hog);
	fflush(stdout);
	SDL_PauseAudio(0);
	for (nth = 0; nth < hog && nth < 8; nth++)
		pthread_create(&th[nth], NULL, spinner, NULL);
	t0 = wall();
	while (wall() - t0 < secs)
		SDL_Delay(100);
	stop = 1;
	while (nth > 0)
		pthread_join(th[--nth], NULL);
	SDL_PauseAudio(1);
	if (calls > 1) {
		double thr = thr_last - thr_first, per = (double)(calls - 1) * got.samples;
		printf("sdltone: thread_cpu %.2f us/sample, path (thread - callback) %.2f us/sample = %.2f%% of a core\n",
		       thr * 1e6 / per, (thr - cpu_s * (calls - 1) / calls) * 1e6 / per,
		       100.0 * (thr - cpu_s * (calls - 1) / calls) / (per / got.freq));
	}
	printf("sdltone: pid %d tid %d calls %lu (expect %.0f) cb_cpu %.2f us/sample (%.1f%% of a core) meangap %.1f ms maxgap %.1f ms late %lu cpus 0x%lx moves %lu\n",
	       getpid(), tid, calls, secs / period_s,
	       calls ? cpu_s * 1e6 / ((double)calls * got.samples) : 0.0,
	       100.0 * cpu_s / secs, calls > 1 ? sumgap * 1e3 / (calls - 1) : 0.0,
	       maxgap * 1e3, late, cpumask, moves);
	SDL_CloseAudio();
	SDL_Quit();
	return 0;
}
