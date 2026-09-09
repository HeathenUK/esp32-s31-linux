/*
 * s31route - an ALSA output plugin that follows the desktop's chosen sink.
 *
 * ALSA binds an application to a device when it opens it, so switching the
 * output while something is playing cannot move that stream: the application
 * holds the device it opened until it closes, and a switch is silent until
 * the next track. The rule on this board is that off-the-shelf software is
 * not modified, so the switch has to happen underneath it.
 *
 * This plugin sits where "default" points. It forwards to whichever device
 * /run/s31-sink names - the codec, or the loopback that s31-bt streams out
 * over A2DP - and reopens that device underneath the application when the
 * file changes. aplay and mpg123 need no knowledge of any of it.
 *
 * Two deliberate choices:
 *
 *  - The sink is named in its OWN file, not read out of asound.conf. This
 *    plugin is referenced from asound.conf, and having it parse the file that
 *    names it invites a recursion that fails in confusing ways.
 *
 *  - The descriptor the application polls is ours, an eventfd, not the
 *    slave's. Reopening the slave changes its descriptors, and an application
 *    that polled the old ones would wait for ever on a device that no longer
 *    exists. Ours never changes, and the write below blocks instead.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <stdint.h>

#define SINK_FILE	"/run/s31-sink"
#define SINK_DEFAULT	"hw:0,0"

struct route {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	char sink[64];
	time_t seen;
	int efd;
	snd_pcm_uframes_t transferred;
};

static void sink_read(struct route *r, char *out, size_t n)
{
	FILE *f = fopen(SINK_FILE, "r");

	snprintf(out, n, "%s", SINK_DEFAULT);
	if (!f)
		return;
	if (fgets(out, (int)n, f)) {
		char *e = out + strlen(out);

		while (e > out && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
			*--e = 0;
		if (!out[0])
			snprintf(out, n, "%s", SINK_DEFAULT);
	}
	fclose(f);
}

static void slave_close(struct route *r)
{
	if (r->slave) {
		snd_pcm_close(r->slave);
		r->slave = NULL;
	}
}

static int slave_open(struct route *r)
{
	char name[80];
	int err;

	slave_close(r);
	/*
	 * Through "plug", so the sink is free to want a different rate or
	 * format from the one the application negotiated with us. The
	 * loopback in particular is fixed at whatever s31-bt opened.
	 */
	/*
	 * Quoted. Unquoted, ALSA splits "plug:hw:0,0" on the comma and reads
	 * the trailing 0 as a second argument to plug, which fails with
	 * "Unknown parameter 1" and leaves the sink unopenable.
	 */
	snprintf(name, sizeof(name), "plug:'%s'", r->sink);
	err = snd_pcm_open(&r->slave, name, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		r->slave = NULL;
		return err;
	}
	/*
	 * Negotiate explicitly, with the *_near variants, which CLAMP to what
	 * the sink can do instead of failing.
	 *
	 * The first version called snd_pcm_set_params() with a fixed 200 ms,
	 * and that broke every SDL application's sound: the codec (hw:0,0)
	 * reports BUFFER_SIZE [256..4096] and PERIOD_SIZE [128..1023], so at
	 * 44100 Hz a 200 ms request (8820 frames) fails with "Unable to get
	 * period size", and SDL - which had already opened the device - went
	 * on to write into a PCM that was never configured. prboom ran
	 * -nosound because of it, written off as a missing backend.
	 *
	 * A retry ladder (200/100/50/25 ms) fixed the open but settled on
	 * 50 ms, because 100 ms overshoots 4096 by 314 frames - and the codec
	 * underran permanently at 50 ms (XRUN in 40 of 40 samples). Asking
	 * the sink for its maximum through plug: does not help either: plug
	 * can convert, so it reports a range far wider than the hardware's.
	 *
	 * set_buffer_size_near(200 ms) asks the question the right way round:
	 * the sink answers with the most it can hold - 4096 frames, ~93 ms,
	 * nearly double the headroom - and there is nothing to retry and no
	 * spurious error to print. The loopback sink has room for the full
	 * 200 ms and gets it, exactly as before.
	 */
	{
		snd_pcm_hw_params_t *hw;
		snd_pcm_sw_params_t *sw;
		unsigned rate = r->io.rate;
		snd_pcm_uframes_t buf, per;
		int dir = 0;

		if (!rate) {
			err = -EINVAL;
			goto done;
		}
		buf = (snd_pcm_uframes_t)rate / 5;	/* 200 ms, or as much as fits */
		snd_pcm_hw_params_alloca(&hw);
		if ((err = snd_pcm_hw_params_any(r->slave, hw)) < 0 ||
		    (err = snd_pcm_hw_params_set_access(r->slave, hw,
				SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
		    (err = snd_pcm_hw_params_set_format(r->slave, hw,
				r->io.format)) < 0 ||
		    (err = snd_pcm_hw_params_set_channels(r->slave, hw,
				r->io.channels)) < 0 ||
		    (err = snd_pcm_hw_params_set_rate_near(r->slave, hw,
				&rate, &dir)) < 0 ||
		    (err = snd_pcm_hw_params_set_buffer_size_near(r->slave, hw,
				&buf)) < 0)
			goto done;
		per = buf / 4;			/* four periods, like set_params */
		dir = 0;
		if ((err = snd_pcm_hw_params_set_period_size_near(r->slave, hw,
				&per, &dir)) < 0 ||
		    (err = snd_pcm_hw_params(r->slave, hw)) < 0)
			goto done;
		/* Start once the buffer is full and wake per period. */
		snd_pcm_sw_params_alloca(&sw);
		if ((err = snd_pcm_sw_params_current(r->slave, sw)) < 0 ||
		    (err = snd_pcm_sw_params_set_start_threshold(r->slave, sw,
				buf)) < 0 ||
		    (err = snd_pcm_sw_params_set_avail_min(r->slave, sw,
				per)) < 0 ||
		    (err = snd_pcm_sw_params(r->slave, sw)) < 0)
			goto done;
	}
done:
	if (err < 0) {
		slave_close(r);
		return err;
	}
	return 0;
}

/* Has the desktop moved the output since we last looked? */
static void sink_follow(struct route *r)
{
	char want[64];
	struct stat st;

	if (stat(SINK_FILE, &st) < 0) {
		if (!r->slave)
			slave_open(r);
		return;
	}
	if (st.st_mtime == r->seen && r->slave)
		return;
	r->seen = st.st_mtime;
	sink_read(r, want, sizeof(want));
	if (r->slave && !strcmp(want, r->sink))
		return;
	snprintf(r->sink, sizeof(r->sink), "%s", want);
	slave_open(r);
}

static int route_start(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	sink_follow(r);
	return r->slave ? 0 : -ENODEV;
}

static int route_stop(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	if (r->slave)
		snd_pcm_drop(r->slave);
	return 0;
}

static int route_prepare(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	r->transferred = 0;
	sink_follow(r);
	if (r->slave)
		snd_pcm_prepare(r->slave);
	return 0;
}

static snd_pcm_sframes_t route_pointer(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	/*
	 * What the hardware has actually PLAYED, not what we have handed on.
	 * Frames written to the slave are still sitting in its buffer, and
	 * reporting them as played overstates progress by a whole buffer:
	 * the application believes it is further behind than it is, and
	 * anything synchronising to the audio clock drifts by that much.
	 * snd_pcm_delay() is how much is still queued ahead of the last
	 * frame we wrote.
	 */
	snd_pcm_sframes_t delay = 0;

	if (!r->slave || snd_pcm_delay(r->slave, &delay) < 0 || delay < 0)
		delay = 0;
	if ((snd_pcm_uframes_t)delay > r->transferred)
		delay = (snd_pcm_sframes_t)r->transferred;
	return (snd_pcm_sframes_t)((r->transferred - (snd_pcm_uframes_t)delay)
				   % io->buffer_size);
}

static snd_pcm_sframes_t route_transfer(snd_pcm_ioplug_t *io,
					const snd_pcm_channel_area_t *areas,
					snd_pcm_uframes_t offset,
					snd_pcm_uframes_t size)
{
	struct route *r = io->private_data;
	const char *buf;
	snd_pcm_sframes_t n;

	sink_follow(r);
	if (!r->slave)
		return -ENODEV;
	buf = (const char *)areas->addr + (areas->first + areas->step * offset) / 8;
	n = snd_pcm_writei(r->slave, buf, size);
	if (n == -EPIPE) {
		snd_pcm_prepare(r->slave);
		n = snd_pcm_writei(r->slave, buf, size);
	}
	if (n == -ENODEV || n == -EIO) {
		/* the sink went away mid-stream; try to pick it up again */
		slave_close(r);
		sink_follow(r);
		if (!r->slave)
			return -ENODEV;
		n = snd_pcm_writei(r->slave, buf, size);
	}
	if (n < 0)
		return n;
	r->transferred += (snd_pcm_uframes_t)n;
	return n;
}

static int route_close(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	slave_close(r);
	if (r->efd >= 0)
		close(r->efd);
	free(r);
	return 0;
}

static const snd_pcm_ioplug_callback_t route_cb = {
	.start		= route_start,
	.stop		= route_stop,
	.prepare	= route_prepare,
	.pointer	= route_pointer,
	.transfer	= route_transfer,
	.close		= route_close,
};

static int route_constraints(snd_pcm_ioplug_t *io)
{
	static const unsigned int accesses[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED,
	};
	static const unsigned int formats[] = {
		SND_PCM_FORMAT_S16_LE,
	};
	int err;

	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_ACCESS,
					    1, accesses);
	if (err < 0)
		return err;
	err = snd_pcm_ioplug_set_param_list(io, SND_PCM_IOPLUG_HW_FORMAT,
					    1, formats);
	if (err < 0)
		return err;
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_CHANNELS,
					      1, 2);
	if (err < 0)
		return err;
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					      8000, 48000);
	if (err < 0)
		return err;
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					      1024, 65536);
	if (err < 0)
		return err;
	return snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIODS,
					       2, 16);
}

SND_PCM_PLUGIN_DEFINE_FUNC(s31route)
{
	struct route *r;
	uint64_t one = 1;
	int err;

	(void)conf;
	(void)root;
	if (stream != SND_PCM_STREAM_PLAYBACK)
		return -EINVAL;
	r = calloc(1, sizeof(*r));
	if (!r)
		return -ENOMEM;
	r->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (r->efd < 0) {
		free(r);
		return -errno;
	}
	/*
	 * Left permanently signalled. The application's poll then always
	 * says "you may write", and the blocking write to the slave provides
	 * the pacing - which is the same pacing it would have had writing to
	 * the device directly.
	 */
	if (write(r->efd, &one, sizeof(one)) < 0) {
		close(r->efd);
		free(r);
		return -errno;
	}
	sink_read(r, r->sink, sizeof(r->sink));

	r->io.version	= SND_PCM_IOPLUG_VERSION;
	r->io.name	= "ESP32-S31 output router";
	r->io.mmap_rw	= 0;
	r->io.callback	= &route_cb;
	r->io.private_data = r;
	r->io.poll_fd	= r->efd;
	r->io.poll_events = POLLIN;

	err = snd_pcm_ioplug_create(&r->io, name, stream, mode);
	if (err < 0) {
		close(r->efd);
		free(r);
		return err;
	}
	err = route_constraints(&r->io);
	if (err < 0) {
		snd_pcm_ioplug_delete(&r->io);
		return err;
	}
	*pcmp = r->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(s31route);
