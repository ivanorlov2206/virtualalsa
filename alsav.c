#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <sound/pcm.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/string.h>

#define DEVNAME "alsavd"
#define CARD_NAME "virtualcard"
#define TIMER_PER_SEC 5
#define TIMER_INTERVAL HZ / TIMER_PER_SEC

#define FILL_MODE_RAND	0
#define FILL_MODE_SEQ	1
#define FILL_MODE_PAT	2

static int index = -1;
static char *id = "alsav";
static bool enable = 1;

static short fill_mode = FILL_MODE_RAND;
static char *fill_pattern = "abacaba";

static struct timer_list timer;

module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard");
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");
module_param(fill_mode, short, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(fill_mode, "Buffer fill mode: rand(0) or seq(1)");
module_param(fill_pattern, charp, S_IRUSR | S_IWUSR);

struct alsav {
	struct snd_pcm_substream *substream;
	struct snd_pcm *pcm;
	size_t buf_pos;
	size_t period_pos;
	size_t b_read;
	size_t period_bytes;
	struct platform_device *pdev;
	struct snd_card *card;
};

static struct alsav *alsav;

static struct snd_pcm_hardware snd_alsav_hw = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_8000,
	.rate_min =		8000,
	.rate_max =		8000,
	.channels_min =		1,
	.channels_max =		1,
	.buffer_bytes_max =	32768,
	.period_bytes_min =	4096,
	.period_bytes_max =	32768,
	.periods_min =		1,
	.periods_max =		1024,
};

void timer_timeout(struct timer_list *data)
{
	struct snd_pcm_runtime *runtime;

	if (!alsav->substream)
		return;
	runtime = alsav->substream->runtime;
	// We want to record RATE samples per sec, it is rate * sample_bytes
	alsav->buf_pos += alsav->b_read;
	alsav->buf_pos %= runtime->dma_bytes;
	alsav->period_pos += alsav->b_read;

	if (alsav->period_pos >= alsav->period_bytes){
		alsav->period_pos %= alsav->period_bytes;
		snd_pcm_period_elapsed(alsav->substream);
	}

	mod_timer(&timer, jiffies + TIMER_INTERVAL);
}

static int snd_alsav_pcm_open(struct snd_pcm_substream *substream)
{
	struct alsav *alsav = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	runtime->hw = snd_alsav_hw;
	alsav->substream = substream;
	alsav->buf_pos = 0;
	alsav->period_pos = 0;

	timer_shutdown_sync(&timer);
	timer_setup(&timer, timer_timeout, 0);
	mod_timer(&timer, jiffies + TIMER_INTERVAL);
	return 0;
}

static int snd_alsav_pcm_close(struct snd_pcm_substream *substream)
{
	struct alsav *alsav = snd_pcm_substream_chip(substream);
	timer_shutdown_sync(&timer);
	alsav->substream = NULL;
	return 0;
}

static int snd_alsav_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	return 0;
}

static int snd_alsav_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

static int snd_alsav_pcm_prepare(struct snd_pcm_substream *substream)
{
	return 0;
}

static void fill_buffer_seq(void *buf, size_t bytes)
{
	size_t i;
	u8 *buffer = buf;
	for (i = 0; i < bytes; i++) {
		buffer[i] = (u8) (i % 256);
	}
}

static void fill_buffer_rand(void *buf, size_t bytes)
{
	get_random_bytes(buf, bytes);
}

static void fill_buffer_pattern(void *buf, size_t bytes)
{
	size_t i;
	u8 *buffer = buf;
	size_t pattern_len = strlen(fill_pattern);

	for (i = 0; i < bytes; i++) {
		buffer[i] = fill_pattern[i % pattern_len];
	}
}

static void fill_buffer(struct snd_pcm_runtime *runtime)
{
	switch (fill_mode) {
	case FILL_MODE_RAND:
		fill_buffer_rand(runtime->dma_area, runtime->dma_bytes);
		break;
	case FILL_MODE_PAT:
		fill_buffer_pattern(runtime->dma_area, runtime->dma_bytes);
		break;
	case FILL_MODE_SEQ:
	default:
		fill_buffer_seq(runtime->dma_area, runtime->dma_bytes);
		break;
	}
}

static int snd_alsav_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	pr_info("Area: %p len: %zd 1 period: %ld\n", runtime->dma_area, runtime->dma_bytes,
		runtime->period_size);
	alsav->period_bytes = frames_to_bytes(runtime, runtime->period_size);
	alsav->b_read = runtime->rate * runtime->sample_bits / 8 / TIMER_PER_SEC;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		fill_buffer(runtime);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t snd_alsav_pcm_pointer(struct snd_pcm_substream *substream)
{
	return bytes_to_frames(substream->runtime, alsav->buf_pos);
}

static int snd_alsav_free(struct alsav *alsav)
{
	if (!alsav)
		return 0;
	if (alsav->card)
		snd_card_free(alsav->card);
	kfree(alsav);
	return 0;
}

static int snd_alsav_dev_free(struct snd_device *device)
{
	return 0;
}

static struct snd_pcm_ops snd_alsav_playback_ops = {
	.open =		snd_alsav_pcm_open,
	.close =	snd_alsav_pcm_close,
	.hw_params =	snd_alsav_pcm_hw_params,
	.hw_free =	snd_alsav_pcm_hw_free,
	.prepare =	snd_alsav_pcm_prepare,
	.trigger =	snd_alsav_pcm_trigger,
	.pointer =	snd_alsav_pcm_pointer,
};

static struct snd_pcm_ops snd_alsav_capture_ops = {
	.open =		snd_alsav_pcm_open,
	.close =	snd_alsav_pcm_close,
	.hw_params =	snd_alsav_pcm_hw_params,
	.hw_free =	snd_alsav_pcm_hw_free,
	.prepare =	snd_alsav_pcm_prepare,
	.trigger =	snd_alsav_pcm_trigger,
	.pointer =	snd_alsav_pcm_pointer,
};

static int snd_alsav_new_pcm(struct alsav *alsav)
{
	struct snd_pcm *pcm;
	int err;

	err = snd_pcm_new(alsav->card, "VirtualAlsa", 0, 1, 1, &pcm);
	if (err < 0)
		return err;
	pcm->private_data = alsav;
	strcpy(pcm->name, "VirtualAlsa");
	alsav->pcm = pcm;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_alsav_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_alsav_capture_ops);

	err = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &alsav->pdev->dev, 16 * 1024, 32 * 1024);
	return 0;
}

static int snd_alsav_create(struct snd_card *card, struct platform_device *pdev, struct alsav **r_alsav)
{
	struct alsav *alsav;
	int err;
	static const struct snd_device_ops ops = {
		.dev_free = snd_alsav_dev_free,
	};

	alsav = kzalloc(sizeof(*alsav), GFP_KERNEL);
	if (alsav == NULL)
		return -ENOMEM;
	alsav->card = card;
	alsav->pdev = pdev;
	alsav->buf_pos = 0;
	alsav->period_pos = 0;

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, alsav, &ops);
	if (err < 0) {
		snd_alsav_free(alsav);
		return err;
	}

	snd_alsav_new_pcm(alsav);
	*r_alsav = alsav;
	return 0;
}

static int alsav_probe(struct platform_device *pdev)
{
	struct snd_card *card;
	int err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err) {
		pr_info("Can't set mask\n");
		goto err1;
	}

	err = snd_card_new(&pdev->dev, index, id, THIS_MODULE, 0, &card);
	if (err < 0) {
		pr_info("Can't create new card\n");
		goto err1;
	}
	err = snd_alsav_create(card, pdev, &alsav);
	if (err < 0) {
		pr_info("Can't create our card instance\n");
		goto err2;
	}

	strcpy(card->driver, "VirtualALSA");
	strcpy(card->shortname, "VirtualALSA");
	strcpy(card->longname, "Virtual ALSA card");

	err = snd_card_register(card);
	if (err < 0) {
		pr_info("Can't register card\n");
		goto err2;
	}

	return 0;
err2:
	snd_card_free(card);
err1:
	return err;
}

static void alsav_pdev_release(struct device *dev)
{
}

static int pdev_remove(struct platform_device *dev)
{
	snd_alsav_free(alsav);
	return 0;
}

static struct platform_device alsav_pdev = {
	.name =		"alsav",
	.dev.release =	alsav_pdev_release,
};

static struct platform_driver alsav_pdrv = {
	.probe =	alsav_probe,
	.remove =	pdev_remove,
	.driver =	{
		.name = "alsav",
	},
};

static int __init mod_init(void)
{
	int err;

	err = platform_device_register(&alsav_pdev);
	if (err)
		return err;
	err = platform_driver_register(&alsav_pdrv);
	if (err)
		platform_device_unregister(&alsav_pdev);
	return err;
}

static void __exit mod_exit(void)
{
	platform_driver_unregister(&alsav_pdrv);
	platform_device_unregister(&alsav_pdev);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivan Orlov");
module_init(mod_init);
module_exit(mod_exit);
