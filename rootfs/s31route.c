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
	err = snd_pcm_set_params(r->slave, r->io.format,
				 SND_PCM_ACCESS_RW_INTERLEAVED,
				 r->io.channels, r->io.rate, 1, 200000);
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
	 * Everything handed to transfer() has been written to the slave and
	 * is the slave's problem, so the application's view of the hardware
	 * pointer is simply how much it has given us.
	 */
	return (snd_pcm_sframes_t)(r->transferred % io->buffer_size);
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
