/*
 * PROTOTYPE COPY for artifacts/audio/first-principles-2026-09-27 - not the
 * shipped plugin. With no S31ROUTE_* variable set it behaves exactly like
 * rootfs/s31route.c. Test-only knobs (read at open / first transfer):
 *   S31ROUTE_HEADROOM=1  sink avail_min = app period (option D): converted
 *                        streams get the sink's whole ring, like direct ones
 *   S31ROUTE_SLICE_US=n  EEVDF custom slice for the feeding thread (option C)
 *   S31ROUTE_RR=prio     SCHED_RR for the feeding thread (option C)
 *   S31ROUTE_CPU=mask    affinity of the feeding thread (option B)
 *   S31ROUTE_BATCH=n     stage app writes and hand the kernel n frames at a
 *                        time (option H): one blocking kernel write, one
 *                        sleep and one wake per n frames instead of per app
 *                        period
 * It also prints "s31route: xruns N transfers M" at close.
 *
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
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sched.h>
#include <stdio.h>
#include <linux/sched/types.h>

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
	unsigned long xruns, transfers, kwrites;
	char *stage;
	snd_pcm_uframes_t nstage, batch, sinkbuf;
	unsigned fbytes;
};

static snd_pcm_sframes_t stage_flush(struct route *r)
{
	snd_pcm_uframes_t off = 0;

	while (off < r->nstage) {
		snd_pcm_sframes_t n = snd_pcm_writei(r->slave,
				r->stage + off * r->fbytes, r->nstage - off);
		if (n < 0) {
			r->nstage = 0;
			return n;
		}
		off += (snd_pcm_uframes_t)n;
		r->kwrites++;
	}
	r->nstage = 0;
	return 0;
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
	snd_pcm_t *old = r->slave, *pcm = NULL;
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
		unsigned rate = r->io.rate;
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
				r->io.channels)) < 0 ||
		    (err = snd_pcm_hw_params_set_rate_near(pcm, hw,
				&rate, &dir)) < 0)
			goto done;
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
		/*
		 * Give the physical codec the largest buffer it can safely
		 * sustain.  The application-facing ring remains unchanged; the
		 * avail_min calculation below still wakes SDL only when its own
		 * period fits.  This adds underrun headroom without increasing
		 * SDL's callback size or making its poll loop spin.
		 */
		/*
		 * At least the mirrored ring, never less. The rate-scaled
		 * 4096*rate/48000 alone is 941 frames at 11025, BELOW the
		 * application's own 1024-frame ring, so the sink was clamped
		 * under the ring it was meant to mirror. set_buffer_size_near
		 * clamps to what the hardware holds (32 KiB of SRAM ring).
		 */
		buf = (snd_pcm_uframes_t)((uint64_t)4096 * rate / 48000);
		/*
		 * PROTOTYPE FIX: a whole number of the app's periods. The
		 * shipped plugin asks for 4096*44100/48000 = 3763 frames at
		 * 44.1 kHz, which no integer period in the codec's 256..1023
		 * range divides, so hw_params fails with EINVAL and every
		 * 44.1 kHz app whose ring is under 3763 frames (SDL 1.2 with
		 * samples <= 1024) gets no sound at all. S31ROUTE_OLDBUF=1
		 * restores the shipped arithmetic for the A/B.
		 */
		if (!getenv("S31ROUTE_OLDBUF") && want_per)
			buf -= buf % want_per;
		if (buf < want_buf)
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
		if (!getenv("S31ROUTE_HEADROOM") &&
		    buf > want_buf && buf - want_buf + want_per > amin)
			amin = buf - want_buf + want_per;
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
				r->sink, r->io.rate,
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
	r->slave = pcm;
	{
		snd_pcm_uframes_t b = 0, p = 0;

		if (!snd_pcm_get_params(pcm, &b, &p))
			r->sinkbuf = b;
	}
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
	if (r->slave && r->follow_ctr++ % 128)
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
	if (r->nstage)
		stage_flush(r);
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
	r->nstage = 0;
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
	r->xruns++;
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

	/*
	 * THE THREAD THAT FEEDS THE CODEC OUTRANKS THE ONE THAT DRAWS.
	 *
	 * This callback runs on the application's audio thread (SDL's, for
	 * every game here). On one core at 100% busy, CFS gives that thread
	 * an equal share with the renderer and the desktop, and a late audio
	 * period is an audible gap where a late video frame is nothing.
	 * OpenTyrian is the case that forced it (2026-09-20): its FM-music
	 * synthesis at 44.1 kHz took 56% of the core at equal weight and still
	 * underran ten times a second - each underrun restarting playback
	 * from an empty buffer, heard as clipped fragments and never music.
	 *
	 * OFF BY DEFAULT, because the measurement said so: with the boost
	 * OpenTyrian's audio thread took 92-100% of the core, STILL underran
	 * (96-130 per 10 s against 101 without) and starved its own game
	 * thread to 7%. Its synthesis is double-precision on a core with no
	 * D - half the thread in __muldf3 - and needs more than the whole
	 * core at 44.1 kHz; priority cannot create cycles. Kept as a knob
	 * (S31ROUTE_NICE=-10) for a producer that fits but is being preempted.
	 * nice, never SCHED_FIFO: a runaway FIFO audio thread takes the board.
	 */
	{
		static __thread int raised;

		if (!raised) {
			const char *e = getenv("S31ROUTE_NICE");
			int prio = e ? atoi(e) : 0;

			raised = 1;
			if (prio)
				setpriority(PRIO_PROCESS,
					    (id_t)syscall(SYS_gettid), prio);
			if ((e = getenv("S31ROUTE_SLICE_US"))) {
				struct sched_attr a = { .size = sizeof(a) };

				a.sched_policy = SCHED_OTHER;
				a.sched_runtime = (unsigned long long)atoi(e) * 1000ull;
				if (syscall(SYS_sched_setattr, 0, &a, 0) < 0)
					perror("s31route: sched_setattr");
			}
			if ((e = getenv("S31ROUTE_RR"))) {
				struct sched_param sp = { .sched_priority = atoi(e) };

				if (sched_setscheduler(0, SCHED_RR, &sp) < 0)
					perror("s31route: SCHED_RR");
			}
			if ((e = getenv("S31ROUTE_CPU"))) {
				cpu_set_t m;
				unsigned long mask = strtoul(e, NULL, 16);
				int c;

				CPU_ZERO(&m);
				for (c = 0; c < 2; c++)
					if (mask & (1ul << c))
						CPU_SET(c, &m);
				if (sched_setaffinity(0, sizeof(m), &m) < 0)
					perror("s31route: affinity");
			}
		}
	}
	sink_follow(r);
	if (!r->slave)
		return -ENODEV;
	buf = (const char *)areas->addr + (areas->first + areas->step * offset) / 8;
	if (!r->fbytes) {
		const char *e = getenv("S31ROUTE_BATCH");

		r->fbytes = io->channels * 2;
		r->batch = e ? (snd_pcm_uframes_t)atoi(e) : 0;
		if (r->batch && r->batch < io->period_size)
			r->batch = io->period_size;
		/* in the sink's frames when rates match; conservative otherwise */
		if (r->batch && r->sinkbuf && r->batch > r->sinkbuf / 2)
			r->batch = r->sinkbuf / 2;
		if (r->batch)
			r->stage = malloc((r->batch + io->buffer_size) * r->fbytes);
		if (!r->stage)
			r->batch = 0;
	}
	if (r->batch) {
		memcpy(r->stage + r->nstage * r->fbytes, buf, size * r->fbytes);
		r->nstage += size;
		r->transferred += size;
		r->transfers++;
		if (r->nstage >= r->batch ||
		    snd_pcm_state(r->slave) != SND_PCM_STATE_RUNNING) {
			n = stage_flush(r);
			if (n == -EPIPE)
				return xrun(r);
			if (n < 0)
				return n;
		}
		return (snd_pcm_sframes_t)size;
	}
	n = snd_pcm_writei(r->slave, buf, size);
	r->kwrites++;
	if (n == -EPIPE)
		return xrun(r);
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
	r->transfers++;
	return n;
}

static int route_close(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	fprintf(stderr, "s31route: xruns %lu transfers %lu kernel_writes %lu\n", r->xruns, r->transfers, r->kwrites);
	free(r->stage);
	slave_close(r);
	if (r->pfd >= 0)
		close(r->pfd);
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

static int route_constraints(snd_pcm_ioplug_t *io, unsigned int max_rate)
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
	/*
	 * max_rate (asound.conf, default 48000) caps the rate this device
	 * OFFERS. That matters because a client negotiates with
	 * snd_pcm_hw_params_set_rate_near(): it asks for a rate and takes the
	 * nearest one offered. TyrQuake hard-codes a 48 kHz request, then mixes
	 * at whatever rate it is GRANTED (shm->speed = obtained.freq) and
	 * resamples every cached sound to it - so at 48 kHz its sound cache is
	 * ~4.4x the size of the native 11 kHz data. Offered only 11025, it is
	 * granted 11025 and nothing in the game changes. The "plug" slave below
	 * resamples up to whatever the sink wants, once, on our side.
	 * Measured 2026-09-21: sound OFF ran the Quake timedemo at 14.0 fps
	 * against 10.7 with sound at 48 kHz, with 868 major faults vs 3,674.
	 * Point one application at a capped device with AUDIODEV=<pcm>; the
	 * default device is unchanged.
	 */
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_RATE,
					      8000, max_rate);
	if (err < 0)
		return err;
	err = snd_pcm_ioplug_set_param_minmax(io, SND_PCM_IOPLUG_HW_PERIOD_BYTES,
					      1024, 65536);
	if (err < 0)
		return err;
	/*
	 * MINIMUM FOUR PERIODS. The application's own ring is what covers a
	 * frame dip: the sink can never be more than that ring ahead of what
	 * has played. SDL 1.2 asks for two periods of its slice (prboom:
	 * 2 x 512 frames = 93 ms at 11025) and Doom's dips run 100-400 ms
	 * (docs/current-state.md), so at two periods every dip is an
	 * underrun. Four periods is 186 ms at 11025 and 46 ms at 44100 - the
	 * minimum that actually helps - and the sink mirrors it below.
	 * S31ROUTE_MIN_PERIODS overrides for an A/B; the hard ceiling of 16
	 * (and the ring in SRAM: 32 KiB, 8192 frames) is unchanged.
	 */
	{
		const char *e = getenv("S31ROUTE_MIN_PERIODS");
		unsigned int min_periods = e ? (unsigned int)atoi(e) : 4;

		if (min_periods < 2)
			min_periods = 2;
		if (min_periods > 16)
			min_periods = 16;
		return snd_pcm_ioplug_set_param_minmax(io,
						       SND_PCM_IOPLUG_HW_PERIODS,
						       min_periods, 16);
	}
}

SND_PCM_PLUGIN_DEFINE_FUNC(s31route)
{
	struct route *r;
	uint64_t one = 1;
	unsigned int max_rate = 48000;
	snd_config_iterator_t it, next;
	int err;

	(void)root;
	if (stream != SND_PCM_STREAM_PLAYBACK)
		return -EINVAL;
	snd_config_for_each(it, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(it);
		const char *id;
		long v;

		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (!strcmp(id, "comment") || !strcmp(id, "type") ||
		    !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "max_rate")) {
			if (snd_config_get_integer(n, &v) < 0 ||
			    v < 8000 || v > 48000) {
				SNDERR("s31route: max_rate must be 8000..48000");
				return -EINVAL;
			}
			max_rate = (unsigned int)v;
			continue;
		}
		SNDERR("s31route: unknown field %s", id);
		return -EINVAL;
	}
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
	err = route_constraints(&r->io, max_rate);
	if (err < 0) {
		snd_pcm_ioplug_delete(&r->io);
		return err;
	}
	*pcmp = r->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(s31route);
