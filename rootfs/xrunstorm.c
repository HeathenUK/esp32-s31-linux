/*
 * xrunstorm - underrun an ALSA playback device on purpose, over and over.
 *
 * Doom with sound hangs the board silently, on both sinks, about one warm
 * timedemo in two, and the one thing every hanging run had in common was
 * underruns being recovered while the machine was saturated. This drives
 * that path alone: the same ring SDL negotiates (two periods of 1024 at
 * 22050 Hz), a period written, then a sleep long enough to run dry, then
 * the -EPIPE recovery every application does. Hundreds of cycles a minute
 * instead of a handful per run.
 *
 *   xrunstorm [device] [seconds] [sleep_ms]
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "default";
	int secs = argc > 2 ? atoi(argv[2]) : 30;
	int sleep_ms = argc > 3 ? atoi(argv[3]) : 150;
	snd_pcm_t *pcm;
	snd_pcm_sw_params_t *sw;
	static short buf[1024 * 2];
	int err, i, xruns = 0, writes = 0;
	time_t end;
	float ph = 0;

	for (i = 0; i < 1024; i++) {
		buf[2 * i] = buf[2 * i + 1] = (short)(sinf(ph) * 8000.0f);
		ph += 2 * 3.14159265f * 440.0f / 22050.0f;
	}
	if ((err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		fprintf(stderr, "open %s: %s\n", dev, snd_strerror(err));
		return 1;
	}
	if ((err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				      SND_PCM_ACCESS_RW_INTERLEAVED, 2, 22050, 1,
				      2 * 1024 * 1000000 / 22050)) < 0) {
		fprintf(stderr, "set_params: %s\n", snd_strerror(err));
		return 1;
	}
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(pcm, sw);
	snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
	snd_pcm_sw_params_set_avail_min(pcm, sw, 1024);
	snd_pcm_sw_params(pcm, sw);
	end = time(NULL) + secs;
	while (time(NULL) < end) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, 1024);

		if (n < 0) {
			if (n == -EPIPE)
				xruns++;
			if ((err = snd_pcm_recover(pcm, (int)n, 1)) < 0) {
				fprintf(stderr, "unrecoverable: %s\n", snd_strerror(err));
				return 1;
			}
			continue;
		}
		writes++;
		if (writes % 2 == 0)
			usleep(sleep_ms * 1000);
		if (writes % 200 == 0) {
			printf("writes %d xruns %d\n", writes, xruns);
			fflush(stdout);
		}
	}
	printf("done: writes %d xruns %d\n", writes, xruns);
	snd_pcm_close(pcm);
	return 0;
}
