// SPDX-License-Identifier: GPL-2.0
/*
 * ALSA virtual driver
 *
 * Copyright 2023 Ivan Orlov <ivan.orlov0322@gmail.com>
 *
 * This is a simple virtual ALSA driver, which can be used for audio applications/ALSA middle layer
 * testing or fuzzing.
 * It can:
 *	- Simulate 'playback' and 'capture' actions
 *	- Generate random or pattern-based capture data
 *	- Check playback buffer for containing looped template, and notify about the results
 *	through the debugfs entry
 *	- Register custom RESET ioctl and notify when it is called through the debugfs entry
 *
 * The driver supports framerates from 8 kHz to 48 kHz, however on the higher framerates it works
 * with delays, so it is better to choose lower framerates (for example, 8 kHz).
 *
 * You can find the corresponding selftest in the 'alsa' selftests folder.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <sound/pcm.h>
#include <sound/core.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/debugfs.h>

#define DEVNAME "alsavd"
#define CARD_NAME "virtualcard"
#define TIMER_PER_SEC 5
#define TIMER_INTERVAL (HZ / TIMER_PER_SEC)

#define FILL_MODE_RAND	0
#define FILL_MODE_PAT	1

#define MAX_PATTERN_LEN 1024

static int index = -1;
static char *id = "alsav";
static bool enable = 1;

static short fill_mode = FILL_MODE_PAT;
static char fill_pattern[MAX_PATTERN_LEN] = "abacaba";
static ssize_t pattern_len = 7;
static struct timer_list timer;

static u8 playback_capture_test;
static u8 ioctl_reset_test;
static struct dentry *driver_debug_dir;

module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard");
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");
module_param(fill_mode, short, 0600);
MODULE_PARM_DESC(fill_mode, "Buffer fill mode: rand(0) or pattern(1)");

struct alsav {
	struct snd_pcm_substream *substream;
	struct snd_pcm *pcm;
	size_t buf_pos;
	size_t period_pos;
	size_t b_rw;
	bool is_buf_corrupted;
	size_t period_bytes;
	size_t total_bytes;
	struct platform_device *pdev;
	struct snd_card *card;
};

static struct alsav *alsav;

static struct snd_pcm_hardware snd_alsav_hw = {
	.info = (SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER |
		 SNDRV_PCM_INFO_MMAP_VALID),
	.formats =		SNDRV_PCM_FMTBIT_S16_LE,
	.rates =		SNDRV_PCM_RATE_8000_48000,
	.rate_min =		8000,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		1,
	.buffer_bytes_max =	32768,
	.period_bytes_min =	4096,
	.period_bytes_max =	32768,
	.periods_min =		1,
	.periods_max =		1024,
};

static inline void inc_buf_pos(size_t by, size_t bytes)
{
	alsav->total_bytes += by;
	alsav->buf_pos += by;
	alsav->buf_pos %= bytes;
}

/*
 * Check one block of the buffer. Here we iterate the buffer until we find '0'. This condition is
 * necessary because we need to detect when the reading/writing ends, so we assume that the pattern
 * doesn't contain zeros.
 */
static void check_buf_block(struct alsav *alsav, struct snd_pcm_runtime *runtime)
{
	size_t i;
	u8 current_byte;

	for (i = 0; i < alsav->b_rw; i++) {
		current_byte = runtime->dma_area[alsav->buf_pos];
		if (!current_byte)
			break;
		if (current_byte != fill_pattern[alsav->total_bytes % pattern_len]) {
			alsav->is_buf_corrupted = true;
			break;
		}
		inc_buf_pos(1, runtime->dma_bytes);
	}
	inc_buf_pos(alsav->b_rw - i, runtime->dma_bytes);
}

static void fill_block_pattern(struct alsav *alsav, struct snd_pcm_runtime *runtime)
{
	size_t i;

	for (i = 0; i < alsav->b_rw; i++) {
		runtime->dma_area[alsav->buf_pos] = fill_pattern[alsav->total_bytes % pattern_len];
		inc_buf_pos(1, runtime->dma_bytes);
	}
}

static void fill_block_random(struct alsav *alsav, struct snd_pcm_runtime *runtime)
{
	size_t in_cur_block = runtime->dma_bytes - alsav->buf_pos;

	if (alsav->b_rw <= in_cur_block) {
		get_random_bytes(&runtime->dma_area[alsav->buf_pos], alsav->b_rw);
	} else {
		get_random_bytes(&runtime->dma_area[alsav->buf_pos], in_cur_block);
		get_random_bytes(runtime->dma_area, alsav->b_rw - in_cur_block);
	}
	inc_buf_pos(alsav->b_rw, runtime->dma_bytes);
}

static void fill_block(struct alsav *alsav, struct snd_pcm_runtime *runtime)
{
	switch (fill_mode) {
	case FILL_MODE_RAND:
		fill_block_random(alsav, runtime);
		break;
	case FILL_MODE_PAT:
		fill_block_pattern(alsav, runtime);
		break;
	}
}

/*
 * Here we iterate through the buffer by (buffer_size / iterates_per_second) bytes.
 * The driver uses timer to simulate the hardware pointer moving, and notify the PCM middle layer
 * about period elapsed.
 */
static void timer_timeout(struct timer_list *data)
{
	struct snd_pcm_runtime *runtime;

	if (!alsav->substream)
		return;
	runtime = alsav->substream->runtime;

	if (alsav->substream->stream == SNDRV_PCM_STREAM_PLAYBACK && !alsav->is_buf_corrupted)
		check_buf_block(alsav, runtime);
	else if (alsav->substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		fill_block(alsav, runtime);
	else
		inc_buf_pos(alsav->b_rw, runtime->dma_bytes);

	alsav->period_pos += alsav->b_rw;
	if (alsav->period_pos >= alsav->period_bytes) {
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
	alsav->is_buf_corrupted = false;
	alsav->period_pos = 0;
	alsav->total_bytes = 0;

	playback_capture_test = 0;
	ioctl_reset_test = 0;

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
	playback_capture_test = !alsav->is_buf_corrupted;
	return 0;
}

static int snd_alsav_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	alsav->period_bytes = frames_to_bytes(runtime, runtime->period_size);
	// We want to record RATE samples per sec, it is rate * sample_bytes bytes
	alsav->b_rw = runtime->rate * runtime->sample_bits / 8 / TIMER_PER_SEC;
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
	kfree(alsav);
	return 0;
}

// These callbacks are required, but empty - all freeing occurs in pdev_remove
static int snd_alsav_dev_free(struct snd_device *device)
{
	return 0;
}

static void alsav_pdev_release(struct device *dev)
{
}

static int snd_alsav_pcm_prepare(struct snd_pcm_substream *substream)
{
	return 0;
}

static int snd_alsav_pcm_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	return 0;
}

static int snd_alsav_pcm_hw_free(struct snd_pcm_substream *substream)
{
	return 0;
}

static int snd_alsav_ioctl(struct snd_pcm_substream *substream, unsigned int cmd, void *arg)
{
	switch (cmd) {
	case SNDRV_PCM_IOCTL1_RESET:
		ioctl_reset_test = 1;
		break;
	}
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static const struct snd_pcm_ops snd_alsav_playback_ops = {
	.open =		snd_alsav_pcm_open,
	.close =	snd_alsav_pcm_close,
	.trigger =	snd_alsav_pcm_trigger,
	.hw_params =	snd_alsav_pcm_hw_params,
	.ioctl =	snd_alsav_ioctl,
	.hw_free =	snd_alsav_pcm_hw_free,
	.prepare =	snd_alsav_pcm_prepare,
	.pointer =	snd_alsav_pcm_pointer,
};

static const struct snd_pcm_ops snd_alsav_capture_ops = {
	.open =		snd_alsav_pcm_open,
	.close =	snd_alsav_pcm_close,
	.trigger =	snd_alsav_pcm_trigger,
	.hw_params =	snd_alsav_pcm_hw_params,
	.hw_free =	snd_alsav_pcm_hw_free,
	.ioctl =	snd_alsav_ioctl,
	.prepare =	snd_alsav_pcm_prepare,
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

	err = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, &alsav->pdev->dev,
					     64 * 1024, 64 * 1024);
	return err;
}

static int snd_alsav_create(struct snd_card *card, struct platform_device *pdev,
			    struct alsav **r_alsav)
{
	struct alsav *alsav;
	int err;
	static const struct snd_device_ops ops = {
		.dev_free = snd_alsav_dev_free,
	};

	alsav = kzalloc(sizeof(*alsav), GFP_KERNEL);
	if (!alsav)
		return -ENOMEM;
	alsav->card = card;
	alsav->pdev = pdev;
	alsav->buf_pos = 0;
	alsav->period_pos = 0;

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, alsav, &ops);
	if (err < 0)
		goto _err_free_chip;

	err = snd_alsav_new_pcm(alsav);
	if (err < 0)
		goto _err_free_chip;

	*r_alsav = alsav;
	return 0;

_err_free_chip:
	snd_alsav_free(alsav);
	return err;
}

static int alsav_probe(struct platform_device *pdev)
{
	struct snd_card *card;
	int err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return err;

	err = snd_devm_card_new(&pdev->dev, index, id, THIS_MODULE, 0, &card);
	if (err < 0)
		return err;
	err = snd_alsav_create(card, pdev, &alsav);
	if (err < 0)
		return err;

	strcpy(card->driver, "VirtualALSA");
	strcpy(card->shortname, "VirtualALSA");
	strcpy(card->longname, "Virtual ALSA card");

	err = snd_card_register(card);
	if (err < 0)
		return err;

	return 0;
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

static ssize_t pattern_write(struct file *file, const char __user *buff, size_t len, loff_t *off)
{
	ssize_t to_write = len;

	if (*off + to_write > MAX_PATTERN_LEN)
		to_write = MAX_PATTERN_LEN - *off;

	// Crop silently everything over the buffer
	if (to_write <= 0)
		return len;

	if (copy_from_user(fill_pattern + *off, buff, to_write))
		return -EFAULT;
	pattern_len = *off + to_write;
	*off += to_write;

	return to_write;
}

static ssize_t pattern_read(struct file *file, char __user *buff, size_t len, loff_t *off)
{
	ssize_t to_read = len;

	if (*off + to_read >= MAX_PATTERN_LEN)
		to_read = MAX_PATTERN_LEN - *off;
	if (to_read <= 0)
		return 0;

	if (copy_to_user(buff, fill_pattern + *off, to_read))
		to_read = 0;
	else
		*off += to_read;

	return to_read;
}

static const struct file_operations fill_pattern_fops = {
	.read = pattern_read,
	.write = pattern_write,
};

static int init_debug_files(void)
{
	driver_debug_dir = debugfs_create_dir("alsav", NULL);
	if (!driver_debug_dir)
		return PTR_ERR(driver_debug_dir);
	debugfs_create_u8("pc_test", 0444, driver_debug_dir, &playback_capture_test);
	debugfs_create_u8("ioctl_test", 0444, driver_debug_dir, &ioctl_reset_test);
	debugfs_create_file("fill_pattern", 0600, driver_debug_dir, NULL, &fill_pattern_fops);

	return 0;
}

static void clear_debug_files(void)
{
	debugfs_remove_recursive(driver_debug_dir);
}

static int __init mod_init(void)
{
	int err = 0;

	err = init_debug_files();
	if (err)
		return err;
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
	clear_debug_files();

	platform_driver_unregister(&alsav_pdrv);
	platform_device_unregister(&alsav_pdev);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivan Orlov");
module_init(mod_init);
module_exit(mod_exit);
