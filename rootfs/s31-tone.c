/*
 * s31-tone - a continuous test tone that FOLLOWS the desktop's output.
 *
 * ALSA binds an application to a device when it opens it, so a tone started
 * before the output picker is used keeps playing to wherever it began, and
 * switching the destination in the panel appears to do nothing at all. That
 * makes it useless as a test instrument for exactly the thing it is meant to
 * test.
 *
 * So this watches /etc/asound.conf - which is what the picker rewrites - and
 * reopens "default" whenever it changes. Switch the output in the volume
 * popover and the tone moves within a fraction of a second.
 *
 * usage: s31-tone [hz] [percent]      defaults: 440 Hz at 20% of full scale
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#define RATE	44100
#define CONF	"/etc/asound.conf"

static volatile sig_atomic_t stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

/*
 * One buffer holding a whole number of periods, so looping it is seamless.
 * At 44100 Hz, 440 Hz needs 44 cycles to land on an integer sample count
 * (44100 * 44 / 440 = 4410); anything else clicks once per wrap.
 */
static int build_tone(int16_t **out, int hz, int pct)
{
	int frames = 0, i, cycles;
	double amp = 32767.0 * (pct < 1 ? 1 : pct > 100 ? 100 : pct) / 100.0;
	int16_t *buf;

	for (cycles = 1; cycles <= 1000; cycles++) {
		double f = (double)RATE * cycles / hz;

		if (fabs(f - floor(f + 0.5)) < 1e-9) {
			frames = (int)(f + 0.5);
			break;
		}
	}
	if (!frames)
		frames = RATE / 10;		/* close enough; may click */
	buf = malloc((size_t)frames * 2 * sizeof(*buf));
	if (!buf)
		return 0;
	for (i = 0; i < frames; i++) {
		double v = amp * sin(2.0 * M_PI * hz * i / RATE);

		buf[2 * i] = buf[2 * i + 1] = (int16_t)v;
	}
	*out = buf;
	return frames;
}

static time_t conf_mtime(void)
{
	struct stat st;

	return stat(CONF, &st) == 0 ? st.st_mtime : 0;
}

static snd_pcm_t *open_default(void)
{
	snd_pcm_t *pcm = NULL;

	if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return NULL;
	if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
			       SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE,
			       1, 200000) < 0) {
		snd_pcm_close(pcm);
		return NULL;
	}
	return pcm;
}

int main(int argc, char **argv)
{
	int hz = argc > 1 ? atoi(argv[1]) : 440;
	int pct = argc > 2 ? atoi(argv[2]) : 20;
	int16_t *tone = NULL;
	int frames;
	snd_pcm_t *pcm;
	time_t seen;

	if (hz < 20 || hz > 20000)
		hz = 440;
	frames = build_tone(&tone, hz, pct);
	if (!frames)
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

	while (!stop) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, tone,
						     (snd_pcm_uframes_t)frames);
		time_t now;

		if (n == -EPIPE) {		/* underrun: carry on */
			snd_pcm_prepare(pcm);
			continue;
		}
		if (n < 0) {
			snd_pcm_close(pcm);
			pcm = open_default();
			if (!pcm) {
				sleep(1);
				pcm = open_default();
				if (!pcm)
					break;
			}
			continue;
		}
		now = conf_mtime();
		if (now != seen) {
			/* the picker moved the output: follow it */
			seen = now;
			snd_pcm_close(pcm);
			pcm = open_default();
			fprintf(stderr, "s31-tone: output changed, reopened%s\n",
				pcm ? "" : " FAILED");
			if (!pcm) {
				sleep(1);
				pcm = open_default();
				if (!pcm)
					break;
			}
		}
	}
	if (pcm) {
		snd_pcm_drop(pcm);
		snd_pcm_close(pcm);
	}
	free(tone);
	return 0;
}
