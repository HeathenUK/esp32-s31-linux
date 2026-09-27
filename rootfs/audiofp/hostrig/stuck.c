/*
 * pcm type "stuck": a sink that takes one buffer and then never drains -
 * its pointer never moves and its descriptor never becomes writable. It is
 * what "the loopback write blocks with no reader" looks like from the ALSA
 * API, whatever the cause on the board, so s31route's stall handling can be
 * exercised on the host. Test-only; never shipped.
 */
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <unistd.h>

struct stuck { snd_pcm_ioplug_t io; int fd; };

static snd_pcm_sframes_t st_pointer(snd_pcm_ioplug_t *io) { (void)io; return 0; }
static int st_start(snd_pcm_ioplug_t *io) { (void)io; return 0; }
static int st_stop(snd_pcm_ioplug_t *io) { (void)io; return 0; }
static snd_pcm_sframes_t st_transfer(snd_pcm_ioplug_t *io,
		const snd_pcm_channel_area_t *a, snd_pcm_uframes_t off,
		snd_pcm_uframes_t size) { (void)io; (void)a; (void)off; return (snd_pcm_sframes_t)size; }
static int st_close(snd_pcm_ioplug_t *io)
{
	struct stuck *s = io->private_data;
	close(s->fd); free(s); return 0;
}
static const snd_pcm_ioplug_callback_t cb = {
	.start = st_start, .stop = st_stop, .pointer = st_pointer,
	.transfer = st_transfer, .close = st_close,
};

SND_PCM_PLUGIN_DEFINE_FUNC(stuck)
{
	struct stuck *s = calloc(1, sizeof(*s));
	static const unsigned acc[] = { SND_PCM_ACCESS_RW_INTERLEAVED };
	static const unsigned fmt[] = { SND_PCM_FORMAT_S16_LE };
	int err;

	(void)root; (void)conf;
	s->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);	/* never signalled */
	s->io.version = SND_PCM_IOPLUG_VERSION;
	s->io.name = "stuck";
	s->io.callback = &cb;
	s->io.private_data = s;
	s->io.poll_fd = s->fd;
	s->io.poll_events = POLLIN;	/* nothing ever writes it */
	err = snd_pcm_ioplug_create(&s->io, name, stream, mode);
	if (err < 0)
		return err;
	snd_pcm_ioplug_set_param_list(&s->io, SND_PCM_IOPLUG_HW_ACCESS, 1, acc);
	snd_pcm_ioplug_set_param_list(&s->io, SND_PCM_IOPLUG_HW_FORMAT, 1, fmt);
	snd_pcm_ioplug_set_param_minmax(&s->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 2);
	snd_pcm_ioplug_set_param_minmax(&s->io, SND_PCM_IOPLUG_HW_RATE, 8000, 48000);
	snd_pcm_ioplug_set_param_minmax(&s->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 256, 65536);
	snd_pcm_ioplug_set_param_minmax(&s->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 64);
	*pcmp = s->io.pcm;
	return 0;
}
SND_PCM_PLUGIN_SYMBOL(stuck);
