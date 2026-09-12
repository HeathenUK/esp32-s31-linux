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
 *  - The descriptor NUMBER the application polls is ours and never changes;
 *    what sits behind it is the current slave's descriptor, dup2()ed into
 *    place each time the sink is reopened. Reopening the slave would
 *    otherwise change the descriptor, and an application that polled the
 *    old one would wait for ever on a device that no longer exists.
 *
 *    The first version left a permanently signalled eventfd there, on the
 *    theory that "the blocking write to the slave provides the pacing". It
 *    does not. alsa-lib's blocking write sleeps in poll() only while the
 *    plugin's OWN ring is full, and a descriptor that is always ready turns
 *    that sleep into a spin: poll, pointer (a delay ioctl on the sink),
 *    still full, poll again - for the whole of every period. Measured with
 *    prboom over 10 s: 20,153 context switches and ~20 s of accounted CPU
 *    through the plugin against 707 and 0.77 s straight to plughw. The
 *    slave's writei never blocked, because its buffer (200 ms) was bigger
 *    than the application's ring, so nothing ever slept. Doom ran at 15 fps
 *    with sound and 32 without, and the whole gap was this spin.
 *
 *    The slave's ring is therefore sized to mirror the application's, and
 *    its avail_min is chosen so that the sink says "writable" exactly when
 *    the plugin's ring has a period free. Then the poll really sleeps.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#define SINK_FILE	"/run/s31-sink"
#define SINK_DEFAULT	"hw:0,0"

struct route {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	char sink[64];
	time_t seen;
	int pfd;		/* what the application polls; see the header */
	unsigned follow_ctr;	/* throttle the /run/s31-sink stat() */
	snd_pcm_uframes_t transferred;
	unsigned up;		/* integer upsample factor; 1 = pass through */
	char *ubuf;		/* staging for the expanded frames */
	size_t ubuf_bytes;
};

/*
 * Rate conversion by REPEATING frames, where the arithmetic allows it.
 *
 * This codec's driver has an exact-match coefficient table with no 22050 or
 * 11025 entry, so every low-rate stream on this board is converted - and
 * until now that conversion was alsa-lib's general resampler inside plug,
 * which interpolates in floating point, per sample, on a hart whose double
 * is a library call. Doom and Quake both ask for 22050, and 22050 x 2 is
 * exactly 44100, a rate the hardware runs natively. Duplicating each frame
 * is then a memcpy and is arithmetically identical to nearest-neighbour
 * upsampling - not an approximation of the resampler but a different, exact
 * answer for integer ratios.
 *
 * The factor is the smallest whole multiple that lands in the hardware's own
 * range, so 22050 doubles to 44100, 11025 quadruples to 44100, 16000 triples
 * to 48000 and 44100 is left alone. Anything that does not divide exactly
 * keeps the old path, resampler and all.
 */
static unsigned upsample_factor(unsigned rate)
{
	unsigned n;

	if (!rate)
		return 1;
	for (n = 2; n <= 6; n++)
		if (rate * n >= 44100 && rate * n <= 48000)
			return n;
	return 1;
}

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

static int xrun(struct route *r);

static snd_pcm_sframes_t slave_write(struct route *r, const char *buf,
				     snd_pcm_uframes_t size)
{
	unsigned fb = r->io.channels * 2;	/* S16_LE only; see constraints */
	snd_pcm_uframes_t i;
	snd_pcm_sframes_t n;
	unsigned k;
	char *p;

	if (r->up <= 1)
		return snd_pcm_writei(r->slave, buf, size);
	if (r->ubuf_bytes < (size_t)size * r->up * fb) {
		size_t want = (size_t)size * r->up * fb;
		char *nb = realloc(r->ubuf, want);

		if (!nb)
			return -ENOMEM;
		r->ubuf = nb;
		r->ubuf_bytes = want;
	}
	/*
	 * Stereo S16 is FOUR BYTES - one 32-bit store, not a memcpy call.
	 *
	 * The first version of this loop called memcpy() once per copied
	 * frame: at 22050 doubled that is ~44,000 four-byte calls a second,
	 * every one of them a call into code executing from 80 MHz XIP
	 * flash, to move a single word. Specialising the two shapes the
	 * constraints actually allow (stereo and mono S16) turns the whole
	 * expansion into aligned word stores with the branch hoisted out.
	 */
	p = r->ubuf;
	if (fb == 4) {
		const uint32_t *src = (const uint32_t *)buf;
		uint32_t *dst = (uint32_t *)p;

		if (r->up == 2) {
			for (i = 0; i < size; i++) {
				uint32_t v = src[i];

				*dst++ = v;
				*dst++ = v;
			}
		} else {
			for (i = 0; i < size; i++) {
				uint32_t v = src[i];

				for (k = 0; k < r->up; k++)
					*dst++ = v;
			}
		}
	} else if (fb == 2) {
		const uint16_t *src = (const uint16_t *)buf;
		uint16_t *dst = (uint16_t *)p;

		for (i = 0; i < size; i++) {
			uint16_t v = src[i];

			for (k = 0; k < r->up; k++)
				*dst++ = v;
		}
	} else {
		for (i = 0; i < size; i++) {
			const char *src = buf + (size_t)i * fb;

			for (k = 0; k < r->up; k++, p += fb)
				memcpy(p, src, fb);
		}
	}
	n = snd_pcm_writei(r->slave, r->ubuf, (snd_pcm_uframes_t)size * r->up);
	if (n < 0)
		return n;
	/*
	 * Report the APPLICATION's frames. writei is blocking here and the
	 * rings are mirrored, so a short write is an xrun's business rather
	 * than a routine partial - but round down regardless, because
	 * claiming a frame we only partly wrote would desynchronise the
	 * pointer for the rest of the stream.
	 */
	return n / (snd_pcm_sframes_t)r->up;
}

static void slave_close(struct route *r)
{
	if (r->slave) {
		snd_pcm_close(r->slave);
		r->slave = NULL;
	}
}

/*
 * One attempt at opening the sink. `direct` skips alsa-lib's plug entirely
 * and talks to the device itself, which is only possible when we can satisfy
 * every parameter exactly - our own format and channel count, and a rate the
 * hardware takes natively (the frame-repeat multiple, or the plain rate).
 * That is the point of doing the conversion here: plug's job was to bridge
 * the rate, and once this plugin bridges it there is nothing left for plug
 * to do but copy every period through another ring. The fallback keeps it
 * for the sinks that still need it - the Bluetooth loopback runs at whatever
 * s31-bt opened, and an odd rate has no whole multiple to reach.
 */
static int slave_try(struct route *r, int direct, snd_pcm_t **out)
{
	char name[80];
	snd_pcm_t *pcm = NULL;
	const char *step = "open";
	int err;

	/*
	 * Open the new sink BEFORE closing the old one. A switch to a sink
	 * that will not open (busy, unplugged, asking for what it cannot do)
	 * then leaves the stream where it was instead of returning -ENODEV
	 * to an application that treats that as the end of sound.
	 */
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
	if (direct)
		snprintf(name, sizeof(name), "%s", r->sink);
	else
		snprintf(name, sizeof(name), "plug:'%s'", r->sink);
	err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0)
		goto done;
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
		unsigned rate = r->io.rate, up = upsample_factor(r->io.rate);
		snd_pcm_uframes_t buf, per, want_buf, want_per, amin;
		int dir = 0;
		struct pollfd pfd;

		if (!rate || !r->io.buffer_size || !r->io.period_size) {
			err = -EINVAL;
			goto done;
		}
		step = "hw_params";
		snd_pcm_hw_params_alloca(&hw);
		if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
		    (err = snd_pcm_hw_params_set_access(pcm, hw,
				SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
		    (err = snd_pcm_hw_params_set_format(pcm, hw,
				r->io.format)) < 0 ||
		    (err = snd_pcm_hw_params_set_channels(pcm, hw,
				r->io.channels)) < 0)
			goto done;
		/*
		 * Ask for the multiple only if the sink will take it EXACTLY.
		 * test_rate answers without narrowing the configuration space,
		 * so a sink that cannot do it (the Bluetooth loopback runs at
		 * whatever s31-bt opened) falls back to the plain rate with
		 * nothing committed and nothing to unwind. Committing first
		 * and checking after cannot be undone, and getting it wrong
		 * means the slave plays our doubled frames at the single rate:
		 * an octave down, at half speed.
		 */
		if (up > 1 && snd_pcm_hw_params_test_rate(pcm, hw,
							  r->io.rate * up, 0) < 0)
			up = 1;
		rate = r->io.rate * up;
		/*
		 * Direct means exact: without plug underneath there is nothing
		 * to convert a near miss, so a rate we cannot have is a reason
		 * to fall back, not to round. set_rate (not _near) says so.
		 */
		if (direct)
			err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0);
		else
			err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate,
							      &dir);
		if (err < 0)
			goto done;
		/*
		 * Whatever came back is what the slave believes it is playing,
		 * so the repeat factor has to follow it exactly or the pitch
		 * is wrong. A near-miss means pass-through, not a guess.
		 */
		r->up = (rate && rate % r->io.rate == 0) ? rate / r->io.rate : 1;
		if (r->up > 1)
			SNDERR("s31route: %u Hz -> %u Hz by %ux frame repeat",
			       r->io.rate, rate, r->up);
		/*
		 * Mirror the application's ring, in the sink's rate. The
		 * application can never be more than its own ring ahead of what
		 * has played (the pointer below reports played frames, not
		 * handed-on ones), so a bigger sink buffer buys no headroom at
		 * all - it only makes "sink writable" and "ring writable" two
		 * different questions, which is the spin described at the top.
		 */
		want_buf = (snd_pcm_uframes_t)((uint64_t)r->io.buffer_size * rate / r->io.rate);
		want_per = (snd_pcm_uframes_t)((uint64_t)r->io.period_size * rate / r->io.rate);
		buf = want_buf;
		if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw,
				&buf)) < 0)
			goto done;
		per = want_per;
		dir = 0;
		if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw,
				&per, &dir)) < 0 ||
		    (err = snd_pcm_hw_params(pcm, hw)) < 0)
			goto done;
		/*
		 * Wake the application only when its OWN ring has a period free.
		 * If the sink holds more than the ring (it clamped upwards, or
		 * the rates rounded that way), plain "a period free in the sink"
		 * would fire while the ring is still full and the write loop
		 * would spin on it. The sink has (buf - queued) free; the ring
		 * has a period free once queued <= want_buf - want_per; so ask
		 * for buf - want_buf + want_per, never less than one period.
		 *
		 * The sink also auto-starts only when full, which with mirrored
		 * rings is when the application's ring is full - and start()
		 * below starts it explicitly for applications that start
		 * earlier than that (SDL uses a threshold of one frame).
		 */
		/*
		 * Wake once per APPLICATION period, not once per sink period.
		 *
		 * This used to be `amin = per`, the sink's own period - and the
		 * sink's period is not the application's: prboom writes 1024
		 * frames at a time, but set_period_size_near() through plug:
		 * settles the codec on 256. With avail_min 256, alsa-lib's
		 * blocking write of 1024 frames became FOUR rounds of
		 * poll + wake + SYNC_PTR ioctl + convert-one-period + SYNC_PTR,
		 * where a direct plughw: open (avail_min = its 1024 period)
		 * does one. Measured: 2.4x the context switches and the audio
		 * thread at 31% of the core against 15% straight to plughw,
		 * with the extra all in ioctls (a ptrace PC sample put 28% of
		 * the thread's wall time inside SYNC_PTR).
		 *
		 * The application's period is the natural unit: it wakes when
		 * its whole write fits, exactly as it would on the raw device,
		 * with the same 2-period latency and the same underrun margin.
		 */
		amin = want_per;
		if (buf > want_buf && buf - want_buf + want_per > amin)
			amin = buf - want_buf + want_per;
		/*
		 * ...BUT NEVER MORE THAN THE SINK CAN EVER OFFER.
		 *
		 * All of the reasoning above assumes the sink can hold the
		 * application's ring. This codec cannot: it reports
		 * BUFFER_SIZE [256..4096], which at 44100 is 93 ms, while
		 * aplay at 22050 asks for a 125 ms period - and the frame
		 * repeat doubles that period into 5512 sink frames against a
		 * 4096-frame buffer. avail_min then exceeds the buffer
		 * entirely, alsa-lib clamps it to the buffer size, and the
		 * poll can only be satisfied when the sink is COMPLETELY
		 * EMPTY. That is an underrun every period by construction:
		 * 64 of them in an 8-second play, on every rate including
		 * native 44100, heard as intermittent crackling.
		 *
		 * Half the buffer leaves ~46 ms still queued at each wake,
		 * which is the margin that has to cover our scheduling
		 * latency on a board where a reclaim stall is tens of ms. The
		 * cost is that an application period larger than that takes
		 * more than one round through alsa-lib's write loop - which is
		 * unavoidable when the period does not fit in the hardware at
		 * all, and is much cheaper than an xrun.
		 */
		if (buf > 1 && amin > buf / 2)
			amin = buf / 2;
		if (amin < per)
			amin = per;
		if (amin > buf)
			amin = buf;
		step = "sw_params";
		snd_pcm_sw_params_alloca(&sw);
		if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
		    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw,
				buf)) < 0 ||
		    (err = snd_pcm_sw_params_set_avail_min(pcm, sw,
				amin)) < 0 ||
		    (err = snd_pcm_sw_params(pcm, sw)) < 0)
			goto done;
		/* Put the sink's descriptor behind the number the app polls. */
		step = "dup2";
		if (snd_pcm_poll_descriptors(pcm, &pfd, 1) == 1 &&
		    dup2(pfd.fd, r->pfd) < 0) {
			err = -errno;
			goto done;
		}
		if (getenv("S31ROUTE_DEBUG"))
			fprintf(stderr, "s31route: %s app %u Hz ring %lu/%lu -> sink %u Hz %lu/%lu avail_min %lu\n",
				name, r->io.rate,
				(unsigned long)r->io.buffer_size,
				(unsigned long)r->io.period_size, rate,
				(unsigned long)buf, (unsigned long)per,
				(unsigned long)amin);
	}
done:
	if (err < 0) {
		if (getenv("S31ROUTE_DEBUG"))
			fprintf(stderr, "s31route: %s %s: %s\n", step, name,
				snd_strerror(err));
		if (pcm)
			snd_pcm_close(pcm);
		return err;
	}
	*out = pcm;
	return 0;
}

static int slave_open(struct route *r)
{
	snd_pcm_t *old = r->slave, *pcm = NULL;
	int err;

	/*
	 * Open the new sink BEFORE closing the old one. A switch to a sink
	 * that will not open (busy, unplugged, asking for what it cannot do)
	 * then leaves the stream where it was instead of returning -ENODEV
	 * to an application that treats that as the end of sound.
	 */
	err = slave_try(r, 1, &pcm);
	if (err < 0)
		err = slave_try(r, 0, &pcm);
	if (err < 0)
		return err;
	r->slave = pcm;
	if (old)
		snd_pcm_close(old);
	return 0;
}

/* Has the desktop moved the output since we last looked? */
static void sink_follow(struct route *r)
{
	char want[64];
	struct stat st;

	/*
	 * Called from every transfer (~86/s at a 256-frame period). A stat()
	 * per transfer is pure overhead when the sink almost never changes;
	 * check roughly twice a second instead. A switch is picked up within
	 * ~0.5 s, which is imperceptible for a speaker/Bluetooth swap. The
	 * slave is always opened immediately when there is none (below), so
	 * startup is not delayed.
	 */
	if (r->slave && r->follow_ctr++ % 16)
		return;
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
	if (!r->slave)
		return -ENODEV;
	if (snd_pcm_state(r->slave) == SND_PCM_STATE_PREPARED)
		snd_pcm_start(r->slave);
	return 0;
}

/* Translate the sink's readiness; the descriptor is its, by dup2. */
static int route_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfds,
			      unsigned int nfds, unsigned short *revents)
{
	struct route *r = io->private_data;

	if (!r->slave || snd_pcm_poll_descriptors_revents(r->slave, pfds, nfds,
							  revents) < 0)
		*revents = pfds[0].revents;
	if ((*revents & POLLERR) && r->slave &&
	    snd_pcm_state(r->slave) == SND_PCM_STATE_XRUN)
		xrun(r);
	return 0;
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

/*
 * The sink ran dry. Report it as OUR underrun, as -EPIPE, which is the one
 * error every ALSA application knows how to recover from: snd_pcm_recover()
 * prepares and carries on, and prepare() below re-prepares the sink.
 *
 * The first version quietly re-prepared the sink and wrote again. That
 * left the sink PREPARED with less than a buffer queued, so it did not
 * restart, so its descriptor stayed "writable" while our ring stayed full,
 * and the application spun; and an underrun noticed from poll() instead
 * surfaced as POLLERR, which alsa-lib turns into -EIO when the plugin's own
 * state is not XRUN - and SDL calls -EIO unrecoverable and gives up sound
 * for the rest of the run ("ALSA write failed (unrecoverable): I/O error",
 * in 1 of 3 launches).
 */
static int xrun(struct route *r)
{
	snd_pcm_ioplug_set_state(&r->io, SND_PCM_STATE_XRUN);
	return -EPIPE;
}

static snd_pcm_sframes_t route_pointer(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;
	snd_pcm_sframes_t delay = 0;
	int err;

	if (!r->slave)
		return -ENODEV;
	/*
	 * Report handed-on frames, not truly-played frames.
	 *
	 * Querying the slave's real position with snd_pcm_delay() on every
	 * pointer call was the single biggest CPU cost of sound. On the codec
	 * it is a hardware ioctl, called ~400 times a second, and a slow
	 * pointer also made alsa-lib's avail loop spin (9,000+ pointer calls
	 * in a 24 s run against ~1,300 without). Removing it took fullscreen
	 * Doom with speaker sound from 10.3 to 19.1 fps - to parity with the
	 * Bluetooth loopback, whose software delay was cheap all along.
	 *
	 * Pacing does not depend on this: the blocking writei() to the slave
	 * provides it, exactly as a direct hw: open would. The cost is that
	 * the reported position overstates playback by up to one buffer
	 * (~46 ms), invisible to a game. An application that synchronises
	 * video to the audio clock can restore the exact query with
	 * S31ROUTE_ACCURATE_DELAY=1.
	 */
	if (!getenv("S31ROUTE_ACCURATE_DELAY"))
		return (snd_pcm_sframes_t)(r->transferred % io->buffer_size);
	err = snd_pcm_delay(r->slave, &delay);
	if (err == -EPIPE || snd_pcm_state(r->slave) == SND_PCM_STATE_XRUN)
		return xrun(r);
	if (err < 0 || delay < 0)
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
	n = slave_write(r, buf, size);
	if (n == -EPIPE)
		return xrun(r);
	if (n == -ENODEV || n == -EIO) {
		/* the sink went away mid-stream; try to pick it up again */
		slave_close(r);
		sink_follow(r);
		if (!r->slave)
			return -ENODEV;
		n = slave_write(r, buf, size);
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
	if (r->pfd >= 0)
		close(r->pfd);
	free(r->ubuf);
	free(r);
	return 0;
}

static const snd_pcm_ioplug_callback_t route_cb = {
	.start		= route_start,
	.stop		= route_stop,
	.prepare	= route_prepare,
	.pointer	= route_pointer,
	.transfer	= route_transfer,
	.poll_revents	= route_poll_revents,
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
	/*
	 * Until a sink is open there is nothing to wait for: a signalled
	 * eventfd holds the number, so a write goes straight through and
	 * reports -ENODEV rather than sleeping on nothing. slave_open()
	 * dup2()s the sink's descriptor over it.
	 */
	r->pfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (r->pfd < 0) {
		free(r);
		return -errno;
	}
	if (write(r->pfd, &one, sizeof(one)) < 0) {
		close(r->pfd);
		free(r);
		return -errno;
	}
	sink_read(r, r->sink, sizeof(r->sink));

	r->io.version	= SND_PCM_IOPLUG_VERSION;
	r->io.name	= "ESP32-S31 output router";
	r->io.mmap_rw	= 0;
	r->io.callback	= &route_cb;
	r->io.private_data = r;
	r->io.poll_fd	= r->pfd;
	r->io.poll_events = POLLOUT;

	err = snd_pcm_ioplug_create(&r->io, name, stream, mode);
	if (err < 0) {
		close(r->pfd);
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
