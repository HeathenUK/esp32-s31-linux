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
 *
 * NEVER BLOCK THE APPLICATION ON A SINK NOBODY DRAINS (2026-09-27).
 *
 * The Bluetooth sink is a loopback, and a loopback is only worth writing to
 * while s31-bt has an A2DP sink to send it to. Moving /run/s31-sink to it
 * mid-stream with no transport stalled the application's stream on the
 * board (docs/gl-performance-campaign-2026-09-27.md), on the old path and
 * the new one alike. Three rules now make that impossible:
 *
 *  - The loopback is used only while s31-bt says it can carry it:
 *    /run/s31-bt-sink holds s31-bt's pid while route mode is on AND a
 *    transport exists. Until then the stream stays on the codec - the same
 *    fall-back the desktop makes on a disconnect - and it moves across the
 *    moment the file appears, and back the moment it goes (or its pid dies).
 *
 *  - Every sink is opened non-blocking. A blocking open of a busy PCM sleeps
 *    in the kernel until the device is free, which may be never.
 *
 *  - Writes to anything but the codec are bounded: non-blocking, with
 *    snd_pcm_wait() and a deadline of two sink buffers. A sink that takes
 *    nothing for that long is declared stalled, the chunk goes to the codec
 *    instead, and the stalled sink is not tried again until s31-bt
 *    republishes or the desktop re-selects. The codec keeps its one blocking
 *    ioctl per period: it is DMA-driven, and a poll per period would be a
 *    syscall on every transfer for a failure it does not have.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "s31resample.h"

#define SINK_FILE	"/run/s31-sink"
#define SINK_DEFAULT	"hw:0,0"
/* present, holding s31-bt's pid, while the loopback has somewhere to go */
#define READY_FILE	"/run/s31-bt-sink"

struct route {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	char sink[64];		/* what is open now */
	char want[64];		/* what /run/s31-sink asks for */
	char failed[64];	/* last sink that would not open; not retried */
	char said[64];		/* last sink announced on stderr */
	unsigned retry_ctr;	/* pacing: retry a failed sink every 10 checks */
	struct timespec seen;	/* /run/s31-sink mtime (ns: two switches in */
	ino_t seen_ino;		/* one second are two switches) */
	struct timespec ready_mt; /* READY_FILE as last read */
	ino_t ready_ino;
	pid_t ready_pid;	/* 0: absent or unreadable */
	int stalled;		/* want took nothing for stall_ms; see header */
	int bounded;		/* slave is non-blocking: sink_write() waits */
	int stall_ms;		/* no progress for this long = stalled */
	int pace;		/* no sink could be opened: sleep, not block */
	struct timespec pace_next; /* when the next paced chunk is due */
	int pfd;		/* what the application polls; see the header */
	int evfd;		/* signalled eventfd, parked behind pfd when no sink */
	unsigned follow_ctr;	/* throttle the /run/s31-sink stat() */
	unsigned follow_every;	/* transfers per check, ~300 ms of audio */
	snd_pcm_uframes_t transferred;
	/*
	 * What transfer() does to the application's frames before the sink
	 * sees them. CONV_PASS: nothing (the sink is plug:, or the codec at
	 * the app's own rate and channel count). CONV_DUP: mono to stereo.
	 * CONV_RESAMPLE: s31resample.h, to a codec rate of the same family.
	 */
	enum { CONV_PASS, CONV_DUP, CONV_RESAMPLE } conv;
	struct s31rs rs;
	int16_t *obuf;		/* converted frames, stereo */
	unsigned ocap;		/* obuf capacity, frames */
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

static int xrun(struct route *r);

static void slave_close(struct route *r)
{
	if (r->slave) {
		snd_pcm_close(r->slave);
		r->slave = NULL;
		/*
		 * pfd is a dup of the sink's descriptor, and while it exists
		 * the device is still open - so reopening a single-substream
		 * sink (the codec) after it went away found it busy. Park the
		 * signalled eventfd there instead, as at startup.
		 */
		dup2(r->evfd, r->pfd);
	}
}

/*
 * Negotiate the sink: `rate` and `channels` are what it should run at (the
 * sink's side of any conversion), `exact` says whether the rate may move.
 */
static int sink_params(struct route *r, snd_pcm_t *pcm, const char *name,
		       unsigned rate, unsigned channels, int exact, int conv,
		       const char **step)
{
	int err;

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
		snd_pcm_uframes_t buf, per, want_buf, want_per, amin;
		int dir = 0;
		struct pollfd pfd;

		if (!rate || !r->io.rate || !r->io.buffer_size ||
		    !r->io.period_size)
			return -EINVAL;
		*step = "hw_params";
		snd_pcm_hw_params_alloca(&hw);
		if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
		    (err = snd_pcm_hw_params_set_access(pcm, hw,
				SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
		    (err = snd_pcm_hw_params_set_format(pcm, hw,
				r->io.format)) < 0 ||
		    (err = snd_pcm_hw_params_set_channels(pcm, hw,
				channels)) < 0 ||
		    (err = exact ?
			snd_pcm_hw_params_set_rate(pcm, hw, rate, 0) :
			snd_pcm_hw_params_set_rate_near(pcm, hw,
				&rate, &dir)) < 0)
			return err;
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
		 * A WHOLE NUMBER OF THE APPLICATION'S PERIODS (2026-09-27).
		 *
		 * The codec's buffer must be an integer number of periods of
		 * 128..1023 frames. At 48000 the value above is 4096, which
		 * divides; at 44100 it is 3763 = 53 x 71, which no period in
		 * that range divides, so hw_params failed with EINVAL and
		 * every 44.1 kHz application whose ring was under 3763 frames
		 * (SDL 1.2 with samples <= 1024, ring = 2 x samples) got no
		 * sound at all: 404-418 underrun messages in 12 s, zero
		 * callbacks, silence. 32000 gives 2730 and was exposed the
		 * same way. Rounding down to the application's period makes
		 * the division exact at every rate; measured with the
		 * prototype (artifacts/audio/first-principles-2026-09-27):
		 * 418 underrun messages -> 0, and sound.
		 */
		if (want_per)
			buf -= buf % want_per;
		if (buf < want_buf)
			buf = want_buf;
		per = want_per;
		if (exact) {
			/*
			 * The codec itself, not plug: - so nothing rounds for
			 * us. Its period tops out at 1023 frames and its
			 * buffer at the SRAM ring (8192), and the buffer must
			 * be a whole number of periods. A converted stream's period in
			 * codec frames can exceed that (36 kHz x 1024 is 1365
			 * at 48 kHz), so split it into equal parts that fit,
			 * then take the largest whole number of them the
			 * buffer holds.
			 */
			snd_pcm_uframes_t pmax = 0, bmax = 0;
			unsigned k = 1;

			if (snd_pcm_hw_params_get_period_size_max(hw, &pmax,
								  &dir) < 0 ||
			    snd_pcm_hw_params_get_buffer_size_max(hw, &bmax) < 0)
				return -EINVAL;
			while (per > pmax) {
				k++;
				per = (want_per + k - 1) / k;
			}
			if (buf > bmax)
				buf = bmax;
			buf -= buf % per;
		}
		if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw,
				&buf)) < 0)
			return err;
		dir = 0;
		if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw,
				&per, &dir)) < 0 ||
		    (err = snd_pcm_hw_params(pcm, hw)) < 0)
			return err;
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
		if (amin > buf)
			amin = buf;
		*step = "sw_params";
		snd_pcm_sw_params_alloca(&sw);
		if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
		    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw,
				buf)) < 0 ||
		    (err = snd_pcm_sw_params_set_avail_min(pcm, sw,
				amin)) < 0 ||
		    (err = snd_pcm_sw_params(pcm, sw)) < 0)
			return err;
		/* Put the sink's descriptor behind the number the app polls. */
		*step = "dup2";
		if (snd_pcm_poll_descriptors(pcm, &pfd, 1) == 1 &&
		    dup2(pfd.fd, r->pfd) < 0) {
			err = -errno;
			return err;
		}
		if (getenv("S31ROUTE_DEBUG"))
			fprintf(stderr, "s31route: %s app %u Hz %u ch ring %lu/%lu -> sink %u Hz %u ch %lu/%lu avail_min %lu conv %s\n",
				name, r->io.rate, r->io.channels,
				(unsigned long)r->io.buffer_size,
				(unsigned long)r->io.period_size, rate,
				channels, (unsigned long)buf, (unsigned long)per,
				(unsigned long)amin,
				conv == CONV_RESAMPLE ? "s31rs" :
				conv == CONV_DUP ? "dup" : "none");
	}
	return 0;
}

/*
 * The codec's own rate for an application rate, or 0 to leave it to plug:.
 *
 * Native when the codec has the rate. Otherwise the smallest codec rate at
 * or above it IN THE SAME FAMILY - multiples of 11025 go to the 44.1 kHz
 * family, everything else to the 8/48 kHz one - so 44.1 kHz content is never
 * resampled to 48 kHz or the other way round, and conversion is always
 * upward (36000 -> 48000, 4:3; 12000 -> 16000; 33075 -> 44100).
 * alsa-lib's plug: chose the NEAREST rate instead, which once the codec
 * learned 32000 sent 36000 DOWN to it, band-limiting it to 16 kHz.
 */
static unsigned codec_rate(snd_pcm_t *pcm, unsigned in)
{
	static const unsigned f44[] = { 11025, 22050, 44100, 0 };
	static const unsigned f48[] = { 8000, 16000, 24000, 32000, 48000, 0 };
	const unsigned *fam[2];
	snd_pcm_hw_params_t *hw;
	int f, i;

	snd_pcm_hw_params_alloca(&hw);
	if (snd_pcm_hw_params_any(pcm, hw) < 0)
		return 0;
	if (!snd_pcm_hw_params_test_rate(pcm, hw, in, 0))
		return in;
	fam[0] = in % 11025 ? f48 : f44;
	fam[1] = in % 11025 ? f44 : f48;
	for (f = 0; f < 2; f++)
		for (i = 0; fam[f][i]; i++)
			if (fam[f][i] >= in &&
			    !snd_pcm_hw_params_test_rate(pcm, hw, fam[f][i], 0))
				return fam[f][i];
	return 0;
}

static int sink_is_codec(const char *sink)
{
	return !strcmp(sink, "hw:0,0") || !strcmp(sink, "hw:0");
}

/*
 * Open a sink without ever sleeping in open(). A blocking open of a busy
 * PCM waits in the kernel until the device is free, and an application
 * inside that wait is stalled for as long as someone else holds it. The
 * codec is then put back into blocking mode: its writei sleeps in the kernel
 * in one ioctl, which is the cheapest pacing there is. Everything else stays
 * non-blocking and is waited on, with a deadline, by sink_write().
 */
static int sink_open_pcm(snd_pcm_t **pcmp, const char *name, int bounded)
{
	int err = snd_pcm_open(pcmp, name, SND_PCM_STREAM_PLAYBACK,
			       SND_PCM_NONBLOCK);

	if (err < 0)
		return err;
	if (!bounded && (err = snd_pcm_nonblock(*pcmp, 0)) < 0) {
		snd_pcm_close(*pcmp);
		*pcmp = NULL;
	}
	return err;
}

/*
 * The codec, opened as itself, with s31route doing any rate conversion and
 * the mono -> stereo route (option 3 of the 2026-09-27 audio study). Any
 * failure returns an error and the caller falls back to plug:, which is what
 * every stream got before.
 */
static int open_direct(struct route *r, const char *sink, snd_pcm_t **pcmp,
		       struct s31rs *rs, int *conv, const char **step)
{
	snd_pcm_t *pcm = NULL;
	unsigned in = r->io.rate, out;
	int err;

	memset(rs, 0, sizeof(*rs));

	*step = "open direct";
	err = sink_open_pcm(&pcm, sink, 0);
	if (err < 0)
		return err;
	*step = "codec rate";
	out = codec_rate(pcm, in);
	err = -EINVAL;
	if (!out)
		goto fail;
	if (out != in) {
		*step = "s31rs_init";
		if (s31rs_init(rs, in, out, r->io.channels) < 0)
			goto fail;
		*conv = CONV_RESAMPLE;
	} else {
		*conv = r->io.channels == 1 ? CONV_DUP : CONV_PASS;
	}
	err = sink_params(r, pcm, sink, out, 2, 1, *conv, step);
	if (err < 0)
		goto fail;
	*pcmp = pcm;
	return 0;
fail:
	s31rs_free(rs);
	snd_pcm_close(pcm);
	return err;
}

static int slave_open(struct route *r, const char *sink)
{
	char name[80];
	snd_pcm_t *old = r->slave, *pcm = NULL;
	const char *step = "open";
	struct s31rs rs;
	int err, conv = CONV_PASS, bounded = !sink_is_codec(sink);
	snd_pcm_uframes_t buf = 0, per = 0;

	/*
	 * Open the new sink BEFORE closing the old one. A switch to a sink
	 * that will not open (busy, unplugged, asking for what it cannot do)
	 * then leaves the stream where it was instead of returning -ENODEV
	 * to an application that treats that as the end of sound.
	 */
	/*
	 * The codec directly, converting here (open_direct above), unless
	 * S31ROUTE_PLUG=1 asks for the old path for an A/B.
	 */
	if (sink_is_codec(sink) && !getenv("S31ROUTE_PLUG")) {
		snprintf(name, sizeof(name), "%s", sink);
		err = open_direct(r, sink, &pcm, &rs, &conv, &step);
		if (!err)
			goto done;
		if (getenv("S31ROUTE_DEBUG"))
			fprintf(stderr, "s31route: %s %s: %s - falling back to plug\n",
				step, name, snd_strerror(err));
		pcm = NULL;
		step = "open";
	}
	conv = CONV_PASS;
	memset(&rs, 0, sizeof(rs));
	/*
	 * Otherwise through "plug", so the sink is free to want a different
	 * rate or format from the one the application negotiated with us.
	 * The loopback in particular is fixed at whatever s31-bt opened.
	 */
	/*
	 * Quoted. Unquoted, ALSA splits "plug:hw:0,0" on the comma and reads
	 * the trailing 0 as a second argument to plug, which fails with
	 * "Unknown parameter 1" and leaves the sink unopenable.
	 */
	snprintf(name, sizeof(name), "plug:'%s'", sink);
	err = sink_open_pcm(&pcm, name, bounded);
	if (err < 0)
		goto done;
	err = sink_params(r, pcm, sink, r->io.rate, r->io.channels, 0, conv,
			  &step);
done:
	if (err < 0) {
		if (getenv("S31ROUTE_DEBUG"))
			fprintf(stderr, "s31route: %s %s: %s\n", step, name,
				snd_strerror(err));
		if (pcm)
			snd_pcm_close(pcm);
		s31rs_free(&rs);
		return err;
	}
	/* commit: the new sink and the conversion that goes with it */
	r->slave = pcm;
	s31rs_free(&r->rs);
	r->rs = rs;
	r->conv = conv;
	r->bounded = bounded;
	snprintf(r->sink, sizeof(r->sink), "%s", sink);
	/*
	 * The stall deadline: two of the sink's buffers, plus scheduling
	 * slack. A healthy sink frees avail_min - at most one buffer - within
	 * one buffer's time, so this is never reached by a sink that drains,
	 * and it is short enough that the application's own ring (at least
	 * four periods) is what covers the one gap a stall costs.
	 */
	r->stall_ms = 250;
	if (snd_pcm_get_params(pcm, &buf, &per) == 0 && r->io.rate) {
		int ms = (int)((uint64_t)buf * 2000 / r->io.rate) + 50;

		if (ms > r->stall_ms)
			r->stall_ms = ms;
	}
	r->pace = 0;
	if (old)
		snd_pcm_close(old);
	return 0;
}

static int ts_eq(struct timespec a, struct timespec b)
{
	return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

/*
 * Can the loopback be heard? s31-bt keeps READY_FILE, holding its pid,
 * exactly while route mode is on and an A2DP transport exists. A file left
 * behind by a daemon that died does not count: its pid is checked.
 */
static int bt_ready(struct route *r)
{
	struct stat st;

	if (stat(READY_FILE, &st) < 0) {
		r->ready_ino = 0;
		r->ready_pid = 0;
		return 0;
	}
	if (st.st_ino != r->ready_ino || !ts_eq(st.st_mtim, r->ready_mt)) {
		FILE *f = fopen(READY_FILE, "r");
		long pid = 0;

		if (f) {
			if (fscanf(f, "%ld", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		r->ready_ino = st.st_ino;
		r->ready_mt = st.st_mtim;
		r->ready_pid = (pid_t)pid;
		/* republished: a sink that stalled before is worth a retry */
		r->stalled = 0;
		r->failed[0] = 0;
	}
	if (r->ready_pid <= 0)
		return 0;
	return kill(r->ready_pid, 0) == 0 || errno == EPERM;
}

/*
 * Has the desktop moved the output since we last looked - or has what it
 * asked for become (un)usable? `force` skips the throttle.
 */
static void sink_follow(struct route *r, int force)
{
	struct stat st;
	const char *eff;

	/*
	 * Called from every transfer (~86/s at a 256-frame period). A stat()
	 * per transfer is pure overhead when the sink almost never changes;
	 * check once per ~300 ms of audio instead (follow_every, set in
	 * prepare). That is also how quickly a stream follows s31-bt gaining
	 * or losing its transport. It was once per 128 transfers, which is
	 * 1.5-3.6 s depending on the period. When there is no sink at all
	 * the first check is immediate, so startup is not delayed.
	 */
	if (!force && (r->slave || r->pace) &&
	    r->follow_ctr++ % (r->follow_every ? r->follow_every : 1))
		return;
	if (stat(SINK_FILE, &st) < 0) {
		snprintf(r->want, sizeof(r->want), "%s", SINK_DEFAULT);
		r->seen_ino = 0;
	} else if (st.st_ino != r->seen_ino || !ts_eq(st.st_mtim, r->seen) ||
		   !r->want[0]) {
		/*
		 * Nanosecond mtime and the inode, not st_mtime: two switches
		 * inside one second used to leave the stream on the first.
		 * Any write re-arms a sink that stalled or would not open -
		 * selecting it again in the panel is the way to retry it.
		 */
		r->seen_ino = st.st_ino;
		r->seen = st.st_mtim;
		sink_read(r, r->want, sizeof(r->want));
		r->stalled = 0;
		r->failed[0] = 0;
	}
	eff = r->want;
	if (!sink_is_codec(eff)) {
		/* bt_ready() first: a republish is what clears `stalled` */
		int ready = bt_ready(r);

		if (r->stalled || !ready)
			eff = SINK_DEFAULT;	/* nobody would hear it */
	}
	if (r->slave && !strcmp(eff, r->sink))
		return;
	/*
	 * Tried and failed: wait for a change - or, with no sink at all
	 * (pacing), retry every ~3 s rather than on every check.
	 */
	if (!strcmp(eff, r->failed) && (r->slave || r->retry_ctr++ % 10))
		return;
	/* say where the stream went and why, once per decision */
	if (getenv("S31ROUTE_DEBUG") || strcmp(r->said, eff)) {
		fprintf(stderr, "s31route: %s -> %s (asked for %s%s)\n",
			r->slave ? r->sink : "none", eff, r->want,
			strcmp(eff, r->want) ? r->stalled ? ", stalled" :
			", no A2DP sink ready" : "");
		snprintf(r->said, sizeof(r->said), "%s", eff);
	}
	if (slave_open(r, eff) == 0) {
		r->failed[0] = 0;
		return;
	}
	snprintf(r->failed, sizeof(r->failed), "%s", eff);
	if (!r->slave && !sink_is_codec(eff))
		slave_open(r, SINK_DEFAULT);
}

static int route_start(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	sink_follow(r, 0);
	if (!r->slave)
		return 0;		/* transfer() paces until a sink opens */
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
	/* ~300 ms of the application's audio between sink checks */
	if (io->period_size && io->rate) {
		r->follow_every = (unsigned)((uint64_t)io->rate * 3 /
					     (io->period_size * 10));
		if (!r->follow_every)
			r->follow_every = 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &r->pace_next);
	sink_follow(r, 0);
	if (r->conv == CONV_RESAMPLE)
		s31rs_reset(&r->rs);	/* a restart is a new signal */
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
	if (!r->slave || !getenv("S31ROUTE_ACCURATE_DELAY"))
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

/*
 * Hand `m` frames to the sink. The codec: blocking writei, as always.
 * Anything else was opened non-blocking and is waited on here, and a sink
 * that frees no room for stall_ms comes back as -ETIMEDOUT instead of
 * holding the application for ever. snd_pcm_wait() consults the cached
 * avail first, so a sink with room costs no extra syscall.
 */
static snd_pcm_sframes_t sink_write(struct route *r, const void *p,
				    snd_pcm_uframes_t m, size_t fbytes)
{
	snd_pcm_uframes_t off = 0;
	int idle = 0;

	while (off < m) {
		snd_pcm_sframes_t n;

		if (r->bounded) {
			int w = snd_pcm_wait(r->slave, r->stall_ms);

			if (w == 0)
				return -ETIMEDOUT;
			if (w < 0)
				return w;
		}
		n = snd_pcm_writei(r->slave, (const char *)p + off * fbytes,
				   m - off);
		if (n > 0) {
			off += (snd_pcm_uframes_t)n;
			idle = 0;
			continue;
		}
		if (n == 0 || n == -EAGAIN) {
			/* ready by poll, refused by write: do not spin on it */
			if (!r->bounded || ++idle > 8)
				return -ETIMEDOUT;
			continue;
		}
		return n;
	}
	return (snd_pcm_sframes_t)off;
}

/*
 * One attempt at the current sink: convert the application's frames for it
 * if it is the codec at another rate or channel count, and hand them on.
 * Converting here rather than once per chunk means a retry on another sink
 * (below) gets frames converted for THAT sink.
 */
static snd_pcm_sframes_t transfer_once(struct route *r, const int16_t *in,
				       snd_pcm_uframes_t size)
{
	unsigned need;
	snd_pcm_sframes_t n;
	int m;

	if (r->conv == CONV_PASS) {
		n = sink_write(r, in, size, (size_t)r->io.channels * 2);
		return n < 0 ? n : (snd_pcm_sframes_t)size;
	}
	need = r->conv == CONV_RESAMPLE ?
	       s31rs_max_out(&r->rs, (unsigned)size) : (unsigned)size;
	if (need > r->ocap) {
		int16_t *o = realloc(r->obuf, (size_t)need * 4);

		if (!o)
			return -ENOMEM;
		r->obuf = o;
		r->ocap = need;
	}
	if (r->conv == CONV_RESAMPLE) {
		m = s31rs_run(&r->rs, in, (unsigned)size, r->obuf);
		if (m < 0)
			return -ENOMEM;
	} else {
		s31rs_dup(in, (unsigned)size, r->obuf);
		m = (int)size;
	}
	n = sink_write(r, r->obuf, (snd_pcm_uframes_t)m, 4);
	return n < 0 ? n : (snd_pcm_sframes_t)size;
}

/*
 * No sink will take the audio (the codec is busy, or a stalled sink could
 * not be replaced). Drain and pace: the chunk is dropped and the application is
 * held for exactly its duration, so it runs at its own rate and never waits
 * on a device - and the next sink check can pick a sink up again.
 */
static void pace_chunk(struct route *r, snd_pcm_uframes_t size)
{
	struct timespec now;
	uint64_t ns = r->io.rate ? (uint64_t)size * 1000000000ull / r->io.rate
				 : 0;

	clock_gettime(CLOCK_MONOTONIC, &now);
	/* more than a second adrift (a stop, a long preemption): start over */
	if (now.tv_sec > r->pace_next.tv_sec + 1 ||
	    now.tv_sec + 1 < r->pace_next.tv_sec)
		r->pace_next = now;
	r->pace_next.tv_nsec += (long)(ns % 1000000000ull);
	r->pace_next.tv_sec += (time_t)(ns / 1000000000ull);
	if (r->pace_next.tv_nsec >= 1000000000L) {
		r->pace_next.tv_nsec -= 1000000000L;
		r->pace_next.tv_sec++;
	}
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &r->pace_next,
			       NULL) == EINTR)
		;
}

/* The sink took nothing for stall_ms: say so once, and leave it. */
static void sink_stalled(struct route *r)
{
	fprintf(stderr, "s31route: %s took nothing for %d ms - nobody is draining it\n",
		r->sink, r->stall_ms);
	r->stalled = 1;
	sink_follow(r, 1);
	if (r->slave && r->bounded) {
		/* nothing to fall back to: drop it, and pace (pace_chunk) */
		slave_close(r);
	}
}

static snd_pcm_sframes_t route_transfer(snd_pcm_ioplug_t *io,
					const snd_pcm_channel_area_t *areas,
					snd_pcm_uframes_t offset,
					snd_pcm_uframes_t size)
{
	struct route *r = io->private_data;
	const char *buf;
	snd_pcm_sframes_t n = -ENODEV;
	int tries;

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
		}
	}
	sink_follow(r, 0);
	buf = (const char *)areas->addr + (areas->first + areas->step * offset) / 8;
	/*
	 * At most three attempts: the sink we have, then whatever replaced it
	 * after it stalled or went away (sink_follow picks the codec when the
	 * loopback is unusable), and once more for luck. A chunk that no sink
	 * takes is dropped and paced, never waited on.
	 */
	for (tries = 0; r->slave && tries < 3; tries++) {
		n = transfer_once(r, (const int16_t *)buf, size);
		if (n >= 0)
			break;
		if (n == -ETIMEDOUT) {
			if (!r->bounded)
				break;		/* the codec: drop, below */
			sink_stalled(r);
		} else if (n == -ENODEV || n == -EIO) {
			/* the sink went away mid-stream; pick it up again */
			slave_close(r);
			sink_follow(r, 1);
		} else {
			break;
		}
	}
	if (n == -EPIPE)
		return xrun(r);
	if (n == -ETIMEDOUT)
		n = (snd_pcm_sframes_t)size;	/* taken by nobody: dropped */
	if (!r->slave) {
		/*
		 * Nowhere to play - the codec busy in another process, or a
		 * stalled sink with nothing to replace it: drain and pace.
		 * Returning -ENODEV instead sent SDL 2 into a loop of 230,000
		 * "underrun" recoveries in 8 s at 13% of a core (host rig,
		 * codec held by two aplays), where this plays silence at the
		 * application's own rate and picks a sink up within ~3 s.
		 */
		r->pace = 1;
		pace_chunk(r, size);
		n = (snd_pcm_sframes_t)size;
	}
	if (n < 0)
		return n;
	r->transferred += size;
	return (snd_pcm_sframes_t)size;
}

static int route_close(snd_pcm_ioplug_t *io)
{
	struct route *r = io->private_data;

	slave_close(r);
	if (r->pfd >= 0)
		close(r->pfd);
	if (r->evfd >= 0)
		close(r->evfd);
	s31rs_free(&r->rs);
	free(r->obuf);
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
	 * eventfd holds the number, so a write goes straight through to
	 * transfer(), which paces it (pace_chunk) rather than sleeping on
	 * nothing. slave_open() dup2()s the sink's descriptor over it.
	 */
	r->evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (r->evfd < 0) {
		free(r);
		return -errno;
	}
	/*
	 * The number the application polls is a dup, so that slave_close()
	 * can park the eventfd behind it again (see there).
	 */
	if (write(r->evfd, &one, sizeof(one)) < 0 ||
	    (r->pfd = fcntl(r->evfd, F_DUPFD_CLOEXEC, 0)) < 0) {
		err = -errno;
		close(r->evfd);
		free(r);
		return err;
	}

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
		close(r->evfd);
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
