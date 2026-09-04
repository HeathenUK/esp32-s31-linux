/*
 * s31-tone - a continuous test tone that FOLLOWS the desktop's output.
 *
 * ALSA binds an application to a device when it opens it, so a tone started
 * before the output picker is used keeps playing to wherever it began, and
 * switching the destination in the panel appears to do nothing at all. That
 * makes it useless as a test instrument for exactly the thing it is meant to
 * test. So this watches /etc/asound.conf - which is what the picker rewrites
 * - and reopens "default" when it changes.
 *
 * Two things this has to get right, both learned the hard way:
 *
 *  - Never park in a blocking write. A loopback with nothing reading it does
 *    not drain, so writei never returns, the config is never re-read, and the
 *    tone is stuck on a dead device for good. Waiting with a timeout keeps
 *    the switch responsive whatever the current device is doing.
 *
 *  - Wake as rarely as possible. A syscall costs milliseconds on this board,
 *    so the wake-up count, not the arithmetic, is what a test tone costs. One
 *    wake per half-buffer with avail_min set to match is ~10/s; letting ALSA
 *    wake us per 512-frame period instead measured four times the CPU.
 *
 * usage: s31-tone [hz] [percent]      defaults: 440 Hz at 20% of full scale
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#define RATE		44100
#define CONF		"/etc/asound.conf"
#define STALL_MS	3000

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static time_t conf_mtime(void)
{
	struct stat st;

	return stat(CONF, &st) == 0 ? st.st_mtime : 0;
}

/*
 * One table holding a whole number of cycles, so playing it end to end
 * repeats seamlessly. At 44100 Hz, 440 Hz needs 44 cycles to land on an
 * integer sample count (44100 * 44 / 440 = 4410); anything else clicks once
 * per wrap. The output chunk is sized to the device, not to the table, so
 * the table is read circularly and phase is carried in tone_pos.
 */
static int16_t *tone;
static int tone_frames;
static int tone_pos;

static int build_tone(int hz, int pct)
{
	int i, cycles;
	double amp = 32767.0 * (pct < 1 ? 1 : pct > 100 ? 100 : pct) / 100.0;

	for (cycles = 1; cycles <= 1000; cycles++) {
		double f = (double)RATE * cycles / hz;

		if (fabs(f - floor(f + 0.5)) < 1e-9) {
			tone_frames = (int)(f + 0.5);
			break;
		}
	}
	if (!tone_frames)
		tone_frames = RATE / 10;	/* close enough; may click */
	tone = malloc((size_t)tone_frames * 2 * sizeof(*tone));
	if (!tone)
		return 0;
	for (i = 0; i < tone_frames; i++) {
		double v = amp * sin(2.0 * M_PI * hz * i / RATE);

		tone[2 * i] = tone[2 * i + 1] = (int16_t)v;
	}
	return tone_frames;
}

/* copy n frames out of the circular table, carrying phase across the wrap */
static void tone_fill(int16_t *dst, int n)
{
	while (n > 0) {
		int run = tone_frames - tone_pos;

		if (run > n)
			run = n;
		memcpy(dst, tone + 2 * tone_pos, (size_t)run * 2 * sizeof(*dst));
		dst += 2 * run;
		tone_pos += run;
		if (tone_pos >= tone_frames)
			tone_pos = 0;
		n -= run;
	}
}

static snd_pcm_uframes_t chunk;		/* frames written per wake-up */
static int16_t *scratch;

/*
 * 200 ms is not a latency preference, it is a compatibility one: snd-aloop
 * makes the second opener of a cable match the first, and the A2DP daemon
 * opens the capture side at 200 ms. Asking for anything else fails the open
 * outright once Bluetooth is the destination.
 */
static const unsigned int latencies[] = { 200000, 100000, 500000 };

static snd_pcm_t *open_default(void)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_uframes_t buf = 0, per = 0;
	snd_pcm_sw_params_t *sw;
	unsigned int i;

	/*
	 * alsa-lib parses the configuration once and keeps it for the life of
	 * the process, so reopening "default" after the picker rewrote
	 * asound.conf silently lands on the OLD device. Only a fresh process
	 * saw the change - which is why switching by hand worked and
	 * switching in the panel did nothing. Drop the cache first.
	 */
	snd_config_update_free_global();
	if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return NULL;
	for (i = 0; i < sizeof(latencies) / sizeof(latencies[0]); i++)
		if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				       SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE,
				       1, latencies[i]) == 0)
			break;
	if (i == sizeof(latencies) / sizeof(latencies[0])) {
		snd_pcm_close(pcm);
		return NULL;
	}
	if (snd_pcm_get_params(pcm, &buf, &per) < 0 || !buf) {
		snd_pcm_close(pcm);
		return NULL;
	}
	/*
	 * Write half a buffer at a time and ask to be woken only when that
	 * much room exists. Without the avail_min, ALSA wakes us once per
	 * period and the tail of each table pass turns into a stream of
	 * tiny writes that never blocks - a spin costing half the core.
	 */
	chunk = buf / 2;
	if (chunk < per)
		chunk = per;
	snd_pcm_sw_params_alloca(&sw);
	if (snd_pcm_sw_params_current(pcm, sw) == 0) {
		snd_pcm_sw_params_set_avail_min(pcm, sw, chunk);
		snd_pcm_sw_params(pcm, sw);
	}
	free(scratch);
	scratch = malloc((size_t)chunk * 2 * sizeof(*scratch));
	if (!scratch) {
		snd_pcm_close(pcm);
		return NULL;
	}
	{
		snd_pcm_info_t *info;

		snd_pcm_info_alloca(&info);
		if (snd_pcm_info(pcm, info) == 0)
			fprintf(stderr, "s31-tone: playing to card %d device %d "
				"(%s), %lu-frame writes\n",
				snd_pcm_info_get_card(info),
				snd_pcm_info_get_device(info),
				snd_pcm_info_get_name(info),
				(unsigned long)chunk);
	}
	return pcm;
}

int main(int argc, char **argv)
{
	int hz = argc > 1 ? atoi(argv[1]) : 440;
	int pct = argc > 2 ? atoi(argv[2]) : 20;
	snd_pcm_t *pcm;
	time_t seen;
	uint64_t fed;
	int reopen = 0;

	if (hz < 20 || hz > 20000)
		hz = 440;
	if (!build_tone(hz, pct))
		return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	seen = conf_mtime();
	pcm = open_default();
	if (!pcm) {
		fprintf(stderr, "s31-tone: cannot open the default device\n");
		return 1;
	}
	fprintf(stderr, "s31-tone: %d Hz at %d%%, following %s\n", hz, pct, CONF);
	fed = now_ms();

	while (!stop) {
		time_t now;
		int r = snd_pcm_wait(pcm, 250);

		if (r < 0) {
			if (r == -EPIPE)
				snd_pcm_prepare(pcm);
			else
				reopen = 1;
		} else if (r > 0) {
			snd_pcm_sframes_t n;

			tone_fill(scratch, (int)chunk);
			n = snd_pcm_writei(pcm, scratch, chunk);
			if (n == -EPIPE)
				snd_pcm_prepare(pcm);
			else if (n < 0)
				reopen = 1;
			else
				fed = now_ms();
		}

		now = conf_mtime();
		if (now != seen) {
			seen = now;			/* the picker moved it */
			reopen = 1;
			fprintf(stderr, "s31-tone: output changed\n");
		} else if (!reopen && now_ms() - fed > STALL_MS) {
			/*
			 * Nothing has taken a sample in three seconds: the
			 * destination is wedged or its consumer went away.
			 * A fresh open is the only thing that recovers it.
			 */
			fprintf(stderr, "s31-tone: output stalled, reopening\n");
			reopen = 1;
		}
		if (reopen) {
			reopen = 0;
			snd_pcm_close(pcm);
			pcm = open_default();
			if (!pcm) {
				sleep(1);
				pcm = open_default();
				if (!pcm)
					break;
			}
			fed = now_ms();
		}
	}
	if (pcm) {
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}
	free(scratch);
	free(tone);
	return 0;
}
