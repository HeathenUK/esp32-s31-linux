// SPDX-License-Identifier: GPL-2.0-only

#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iov_iter.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "s31_audio_sram.h"

struct s31_audio_linux;

struct s31_audio_pcm {
	struct s31_audio_linux *audio;
	struct snd_pcm_substream *substream;
	wait_queue_head_t wait;
	u32 stream;
	u32 last_hardware_bytes;
	bool capture;
	bool running;
};

struct s31_audio_linux {
	struct device *dev;
	void __iomem *control;
	void __iomem *rings;
	void __iomem *doorbell_h1_to_h0;
	void __iomem *doorbell_h0_to_h1;
	int irq;
	struct s31_audio_pcm pcm[S31_AUDIO_STREAM_COUNT];
};

static ssize_t audio_stats_show(struct device *dev,
				struct device_attribute *attribute, char *buffer)
{
	struct s31_audio_linux *audio = dev_get_drvdata(dev);
	ssize_t length = 0;

	(void)attribute;
	for (unsigned int stream = 0; stream < S31_AUDIO_STREAM_COUNT; stream++) {
		void __iomem *control = audio->control +
			offsetof(struct s31_audio_control, stream[stream]);

		length += sysfs_emit_at(buffer, length, "%u %u %u\n", stream,
			readl(control + offsetof(struct s31_audio_ring_control,
						 transferred_bytes)),
			readl(control + offsetof(struct s31_audio_ring_control, xruns)));
	}
	return length;
}
static DEVICE_ATTR_RO(audio_stats);

static struct attribute *s31_audio_attributes[] = {
	&dev_attr_audio_stats.attr,
	NULL,
};

static const struct attribute_group s31_audio_attribute_group = {
	.attrs = s31_audio_attributes,
};


static const struct snd_pcm_hardware s31_audio_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	/*
	 * Only the native wire rate is advertised. hart0 can resample other
	 * rates with its ASRC, but that path costs xruns - measured at 808 over
	 * four seconds against none at the wire rate - so let alsa-lib's plug
	 * layer resample in userspace instead and keep the hardware path
	 * bit-exact.
	 *
	 * The wire rate is 44100 (S31_AUDIO_HW_RATE), chosen because it is what
	 * this board's content and the A2DP route already use, so the common
	 * case converts nowhere at all.
	 */
	.rates = SNDRV_PCM_RATE_44100,
	.rate_min = S31_AUDIO_HW_RATE,
	.rate_max = S31_AUDIO_HW_RATE,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = S31_AUDIO_RING_BYTES,
	.period_bytes_min = 256,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 64,
};

static void __iomem *s31_stream_control(struct s31_audio_pcm *pcm)
{
	return pcm->audio->control + offsetof(struct s31_audio_control,
					    stream[pcm->stream]);
}

static void __iomem *s31_stream_data(struct s31_audio_pcm *pcm)
{
	return pcm->audio->rings + s31_audio_ring_offset(pcm->stream);
}

static u32 s31_control_read(struct s31_audio_pcm *pcm, size_t offset)
{
	return readl(s31_stream_control(pcm) + offset);
}

static void s31_control_write(struct s31_audio_pcm *pcm, size_t offset, u32 value)
{
	writel(value, s31_stream_control(pcm) + offset);
}

static void s31_audio_notify_hart0(struct s31_audio_pcm *pcm)
{
	/* Publish ring and state updates before raising the hart0 doorbell. */
	wmb();
	writel(1, pcm->audio->doorbell_h1_to_h0);
}

static u32 s31_hardware_bytes(struct s31_audio_pcm *pcm)
{
	return s31_control_read(pcm, pcm->capture ?
		offsetof(struct s31_audio_ring_control, producer) :
		offsetof(struct s31_audio_ring_control, consumer));
}

static void s31_audio_period_elapsed(struct s31_audio_pcm *pcm)
{
	struct snd_pcm_substream *substream = READ_ONCE(pcm->substream);
	u32 hardware_bytes;
	u32 period_bytes;

	if (!READ_ONCE(pcm->running) || !substream)
		return;
	hardware_bytes = s31_hardware_bytes(pcm);
	period_bytes = snd_pcm_lib_period_bytes(substream);
	if (period_bytes && hardware_bytes / period_bytes !=
			pcm->last_hardware_bytes / period_bytes) {
		pcm->last_hardware_bytes = hardware_bytes;
		snd_pcm_period_elapsed(substream);
	}
}

static irqreturn_t s31_audio_irq(int irq, void *data)
{
	struct s31_audio_linux *audio = data;

	(void)irq;
	writel(0, audio->doorbell_h0_to_h1);
	s31_audio_period_elapsed(&audio->pcm[S31_AUDIO_LINUX_STREAM]);
	s31_audio_period_elapsed(&audio->pcm[S31_AUDIO_CAPTURE_STREAM]);
	wake_up_interruptible(&audio->pcm[S31_AUDIO_CAPTURE_STREAM].wait);
	return IRQ_HANDLED;
}

static int s31_pcm_open(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	struct s31_audio_linux *audio = snd_soc_component_get_drvdata(component);
	struct s31_audio_pcm *pcm = &audio->pcm[
		substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
		S31_AUDIO_CAPTURE_STREAM : S31_AUDIO_LINUX_STREAM];
	u32 owner = s31_control_read(pcm,
		offsetof(struct s31_audio_ring_control, owner));

	if ((!pcm->capture && pcm->stream != S31_AUDIO_LINUX_STREAM) ||
	    owner != S31_AUDIO_OWNER_NONE)
		return -EBUSY;
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, owner),
			  S31_AUDIO_OWNER_LINUX);
	substream->runtime->hw = s31_audio_hardware;
	if (pcm->capture) {
		substream->runtime->hw.rates = SNDRV_PCM_RATE_44100;
		substream->runtime->hw.rate_min = S31_AUDIO_HW_RATE;
		substream->runtime->hw.rate_max = S31_AUDIO_HW_RATE;
	}
	substream->runtime->private_data = pcm;
	pcm->substream = substream;
	return 0;
}

static int s31_pcm_close(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct s31_audio_pcm *pcm = substream->runtime->private_data;

	WRITE_ONCE(pcm->running, false);
	wake_up_interruptible(&pcm->wait);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, state),
			  S31_AUDIO_STATE_CLOSED);
	/* Publish CLOSED before releasing ownership to another client. */
	wmb();
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, owner),
			  S31_AUDIO_OWNER_NONE);
	s31_audio_notify_hart0(pcm);
	WRITE_ONCE(pcm->substream, NULL);
	return 0;
}

static int s31_pcm_hw_params(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t buffer_bytes = params_buffer_bytes(params);

	if (buffer_bytes > S31_AUDIO_RING_BYTES)
		return -EINVAL;
	if (!runtime->dma_area || runtime->dma_bytes < buffer_bytes)
		return -ENOMEM;
	return 0;
}

static int s31_pcm_prepare(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct s31_audio_pcm *pcm = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 generation = s31_control_read(pcm,
		offsetof(struct s31_audio_ring_control, generation));

	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, state),
			  S31_AUDIO_STATE_PREPARED);
	/* Pair with hart0's fence before it commits a completed block. */
	wmb();
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, generation),
			  generation + 1);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, producer), 0);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, consumer), 0);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, rate),
			  runtime->rate);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, channels),
			  runtime->channels);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, format),
			  S31_AUDIO_FORMAT_U16_LE);
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, period_bytes),
			  snd_pcm_lib_period_bytes(substream));
	s31_control_write(pcm, offsetof(struct s31_audio_ring_control, state),
			  S31_AUDIO_STATE_PREPARED);
	pcm->last_hardware_bytes = 0;
	return 0;
}

static int s31_pcm_trigger(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream, int command)
{
	struct s31_audio_pcm *pcm = substream->runtime->private_data;

	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		s31_control_write(pcm, offsetof(struct s31_audio_ring_control, state),
				  S31_AUDIO_STATE_RUNNING);
		WRITE_ONCE(pcm->running, true);
		s31_audio_notify_hart0(pcm);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(pcm->running, false);
		wake_up_interruptible(&pcm->wait);
		s31_control_write(pcm, offsetof(struct s31_audio_ring_control, state),
				  S31_AUDIO_STATE_PREPARED);
		s31_audio_notify_hart0(pcm);
		return 0;
	default:
		return -EINVAL;
	}
}

static int s31_pcm_sync_stop(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct s31_audio_pcm *pcm = substream->runtime->private_data;

	synchronize_irq(pcm->audio->irq);
	return 0;
}

static snd_pcm_uframes_t s31_pcm_pointer(struct snd_soc_component *component,
					 struct snd_pcm_substream *substream)
{
	struct s31_audio_pcm *pcm = substream->runtime->private_data;
	u32 buffer_bytes = snd_pcm_lib_buffer_bytes(substream);
	u32 position = s31_hardware_bytes(pcm) % buffer_bytes;

	return bytes_to_frames(substream->runtime, position);
}

static void s31_copy_to_ring(struct s31_audio_pcm *pcm, u32 position,
			     const u8 *source, size_t bytes)
{
	size_t first = min_t(size_t, bytes, S31_AUDIO_RING_BYTES - position);

	memcpy_toio(s31_stream_data(pcm) + position, source, first);
	if (first != bytes)
		memcpy_toio(s31_stream_data(pcm), source + first, bytes - first);
}

static void s31_copy_from_ring(struct s31_audio_pcm *pcm, u32 position,
			       u8 *destination, size_t bytes)
{
	size_t first = min_t(size_t, bytes, S31_AUDIO_RING_BYTES - position);

	memcpy_fromio(destination, s31_stream_data(pcm) + position, first);
	if (first != bytes)
		memcpy_fromio(destination + first, s31_stream_data(pcm), bytes - first);
}

/*
 * The shared ring carries unsigned samples: hart0 flips the sign bit on the way
 * to and from I2S. ALSA and the codec both speak signed, and a DAI advertising
 * U16_LE has no format in common with any real codec, so convert here. Both
 * directions already stage through a bounce buffer, so this costs no extra
 * copy.
 */
static void s31_flip_sign(u8 *buffer, size_t bytes)
{
	__le16 *sample = (__le16 *)buffer;
	size_t count = bytes / sizeof(*sample);

	for (size_t i = 0; i < count; i++)
		sample[i] ^= cpu_to_le16(0x8000);
}

static int s31_pcm_copy(struct snd_soc_component *component,
			struct snd_pcm_substream *substream, int channel,
			unsigned long pos, struct iov_iter *iter,
			unsigned long bytes)
{
	struct s31_audio_pcm *pcm = substream->runtime->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t producer_offset = offsetof(struct s31_audio_ring_control, producer);
	size_t consumer_offset = offsetof(struct s31_audio_ring_control, consumer);
	u32 producer = s31_control_read(pcm, producer_offset);
	u32 consumer = s31_control_read(pcm, consumer_offset);
	u8 *bounce = runtime->dma_area;

	(void)channel;
	(void)pos;
	if (bytes > runtime->dma_bytes || !bounce)
		return -EINVAL;
	if (!pcm->capture) {
		if (producer - consumer > S31_AUDIO_RING_BYTES - bytes)
			return -EAGAIN;
		if (!copy_from_iter_full(bounce, bytes, iter))
			return -EFAULT;
		s31_flip_sign(bounce, bytes);
		s31_copy_to_ring(pcm, producer % S31_AUDIO_RING_BYTES, bounce, bytes);
		/* Publish playback samples before advancing the producer. */
		wmb();
		s31_control_write(pcm, producer_offset, producer + bytes);
	} else {
		long waited;

		waited = wait_event_interruptible_timeout(pcm->wait,
			({
				producer = s31_control_read(pcm, producer_offset);
				producer - consumer >= bytes ||
					!READ_ONCE(pcm->running);
			}), msecs_to_jiffies(500));
		if (waited < 0)
			return waited;
		if (!waited)
			return -ETIMEDOUT;
		if (!READ_ONCE(pcm->running))
			return -EPIPE;
		s31_copy_from_ring(pcm, consumer % S31_AUDIO_RING_BYTES, bounce, bytes);
		s31_flip_sign(bounce, bytes);
		if (copy_to_iter(bounce, bytes, iter) != bytes)
			return -EFAULT;
		/* Complete the capture copy before releasing ring space. */
		wmb();
		s31_control_write(pcm, consumer_offset, consumer + bytes);
	}
	return 0;
}

/*
 * Both directions share one ring each, sized by the transport rather than by
 * the substream, so the buffer is preallocated at its maximum.
 */
static int s31_pcm_construct(struct snd_soc_component *component,
			     struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, S31_AUDIO_RING_BYTES);
	return 0;
}

static const struct snd_soc_component_driver s31_component_driver = {
	.name = "esp32s31-audio",
	.open = s31_pcm_open,
	.close = s31_pcm_close,
	.hw_params = s31_pcm_hw_params,
	.prepare = s31_pcm_prepare,
	.trigger = s31_pcm_trigger,
	.sync_stop = s31_pcm_sync_stop,
	.pointer = s31_pcm_pointer,
	.copy = s31_pcm_copy,
	.pcm_new = s31_pcm_construct,
	.legacy_dai_naming = 1,
};

/*
 * hart0 owns the I2S hardware, so this DAI carries no bus configuration: it
 * exists to give the card something to link the codec against, and the format
 * is whatever the shared ring uses.
 */
static struct snd_soc_dai_driver s31_dai_driver = {
	.name = "esp32s31-hosted",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};


static void __iomem *s31_audio_map_rings(struct platform_device *pdev,
					 struct resource *resource)
{
	void *mapping;

	if (resource_size(resource) != S31_AUDIO_RING_AREA_SIZE)
		return ERR_PTR(-EINVAL);
	mapping = devm_memremap(&pdev->dev, resource->start,
				resource_size(resource), MEMREMAP_WB);
	return mapping ? (void __iomem *)mapping : ERR_PTR(-ENOMEM);
}

static int s31_audio_probe(struct platform_device *pdev)
{
	struct s31_audio_linux *audio;
	struct resource *resource;
	int error;

	audio = devm_kzalloc(&pdev->dev, sizeof(*audio), GFP_KERNEL);
	if (!audio)
		return -ENOMEM;
	for (unsigned int i = 0; i < S31_AUDIO_STREAM_COUNT; i++)
		init_waitqueue_head(&audio->pcm[i].wait);
	audio->dev = &pdev->dev;
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "control");
	if (!resource)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing audio control\n");
	if (resource_size(resource) != S31_AUDIO_CONTROL_MAP_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid audio control size\n");
	audio->control = devm_ioremap(&pdev->dev, resource->start,
				      resource_size(resource));
	if (!audio->control)
		return dev_err_probe(&pdev->dev, -ENOMEM, "failed to map audio control\n");
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rings");
	if (!resource)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing audio rings\n");
	audio->rings = s31_audio_map_rings(pdev, resource);
	if (IS_ERR(audio->rings))
		return dev_err_probe(&pdev->dev, PTR_ERR(audio->rings),
				     "failed to map audio rings\n");
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "h1-to-h0");
	if (!resource)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing h1-to-h0 doorbell\n");
	audio->doorbell_h1_to_h0 = devm_ioremap(&pdev->dev, resource->start,
					     resource_size(resource));
	if (!audio->doorbell_h1_to_h0)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to map h1-to-h0 doorbell\n");
	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "h0-to-h1");
	if (!resource)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing h0-to-h1 doorbell\n");
	audio->doorbell_h0_to_h1 = devm_ioremap(&pdev->dev, resource->start,
					     resource_size(resource));
	if (!audio->doorbell_h0_to_h1)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to map h0-to-h1 doorbell\n");
	audio->irq = platform_get_irq(pdev, 0);
	if (audio->irq < 0)
		return audio->irq;
	writel(0, audio->doorbell_h0_to_h1);
	error = devm_request_irq(&pdev->dev, audio->irq, s31_audio_irq, 0,
				 dev_name(&pdev->dev), audio);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				     "failed to request audio doorbell IRQ\n");
	if (readl(audio->control + offsetof(struct s31_audio_control, magic)) !=
			S31_AUDIO_MAGIC ||
	    readl(audio->control + offsetof(struct s31_audio_control, abi_version)) !=
			S31_AUDIO_ABI_VERSION ||
	    !readl(audio->control + offsetof(struct s31_audio_control, ready)))
		return -ENODEV;

	/*
	 * Register as an ASoC component rather than creating a card directly:
	 * the codec lives on Linux's I2C bus and has to be linked to this
	 * transport, which a card in DT does. Streams map by direction -
	 * playback to the Linux ring, capture to the capture ring.
	 */
	for (unsigned int i = 0; i < 2; i++) {
		struct s31_audio_pcm *pcm = &audio->pcm[i ?
			S31_AUDIO_CAPTURE_STREAM : S31_AUDIO_LINUX_STREAM];

		pcm->audio = audio;
		pcm->stream = i ? S31_AUDIO_CAPTURE_STREAM : S31_AUDIO_LINUX_STREAM;
		pcm->capture = i != 0;
	}

	platform_set_drvdata(pdev, audio);
	error = devm_snd_soc_register_component(&pdev->dev,
					        &s31_component_driver,
					        &s31_dai_driver, 1);
	if (error)
		return dev_err_probe(&pdev->dev, error,
				     "failed to register ASoC component\n");
	error = devm_device_add_group(&pdev->dev, &s31_audio_attribute_group);
	if (error)
		return error;
	dev_info(&pdev->dev, "hosted PCM transport registered as an ASoC component\n");
	return 0;
}

static const struct of_device_id s31_audio_of_match[] = {
	{ .compatible = "espressif,esp32s31-freertos-audio" },
	{}
};
MODULE_DEVICE_TABLE(of, s31_audio_of_match);

static struct platform_driver s31_audio_driver = {
	.probe = s31_audio_probe,
	.driver = {
		.name = "esp32s31-freertos-audio",
		.of_match_table = s31_audio_of_match,
	},
};
module_platform_driver(s31_audio_driver);

MODULE_DESCRIPTION("ESP32-S31 FreeRTOS shared-memory ALSA driver");
MODULE_LICENSE("GPL");
