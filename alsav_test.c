// SPDX-License-Identifier: GPL-2.0
/*
 * This is the test which covers ALSA middle layer data transferring using
 * the virtual alsa driver (alsav).
 *
 * Copyright 2023 Ivan Orlov <ivan.orlov0322@gmail.com>
 */
#include <string.h>
#include <alsa/asoundlib.h>
#include "../kselftest_harness.h"

struct alsav_test_params {
	unsigned long buffer_size;
	unsigned long period_size;
	unsigned long channels;
	unsigned int rate;
	size_t sample_len;
	int time;
	snd_pcm_format_t format;
};

static int get_test_results(char *debug_name)
{
	int result;
	FILE *f;
	char fname[128];

	sprintf(fname, "/sys/kernel/debug/alsav/%s", debug_name);

	f = fopen(fname, "r");
	if (!f) {
		printf("Failed to open file\n");
		return -1;
	}
	fscanf(f, "%d", &result);
	fclose(f);

	return result;
}

size_t get_sample_len(unsigned int rate, unsigned long channels, snd_pcm_format_t format)
{
	return rate * channels * snd_pcm_format_physical_width(format) / 8;
}

int setup_handle(snd_pcm_t **handle, snd_pcm_sw_params_t *swparams, snd_pcm_hw_params_t *hwparams,
		 struct alsav_test_params *params, int card, snd_pcm_stream_t stream)
{
	char pcm_name[32];
	int err;

	sprintf(pcm_name, "hw:%d,0,0", card);
	err = snd_pcm_open(handle, pcm_name, stream, 0);
	if (err < 0)
		return err;
	snd_pcm_hw_params_any(*handle, hwparams);
	snd_pcm_hw_params_set_rate_resample(*handle, hwparams, 0);
	snd_pcm_hw_params_set_access(*handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(*handle, hwparams, params->format);
	snd_pcm_hw_params_set_channels(*handle, hwparams, params->channels);
	snd_pcm_hw_params_set_rate_near(*handle, hwparams, &params->rate, 0);
	snd_pcm_hw_params_set_period_size_near(*handle, hwparams, &params->period_size, 0);
	snd_pcm_hw_params_set_buffer_size_near(*handle, hwparams, &params->buffer_size);
	snd_pcm_hw_params(*handle, hwparams);
	snd_pcm_sw_params_current(*handle, swparams);

	snd_pcm_hw_params_set_rate_resample(*handle, hwparams, 0);
	snd_pcm_sw_params_set_avail_min(*handle, swparams, params->period_size);
	snd_pcm_hw_params_set_buffer_size_near(*handle, hwparams, &params->buffer_size);
	snd_pcm_hw_params_set_period_size_near(*handle, hwparams, &params->period_size, 0);
	snd_pcm_sw_params(*handle, swparams);
	snd_pcm_hw_params(*handle, hwparams);

	return 0;
}

FIXTURE(alsav) {
	int card;
	snd_pcm_sw_params_t *swparams;
	snd_pcm_hw_params_t *hwparams;
	unsigned short *samples;
	struct alsav_test_params params;
};

FIXTURE_TEARDOWN(alsav) {
	free(self->samples);
}

FIXTURE_SETUP(alsav) {
	char *card_name = malloc(127);

	ASSERT_NE(card_name, NULL);
	self->params.buffer_size = 16384;
	self->params.period_size = 4096;
	self->params.channels = 1;
	self->params.rate = 8000;
	self->params.format = SND_PCM_FORMAT_S16_LE;
	self->card = -1;

	self->params.sample_len = get_sample_len(self->params.rate, self->params.channels,
						 self->params.format);
	self->params.time = 4;
	self->samples = malloc(self->params.sample_len * self->params.time);
	ASSERT_NE(self->samples, NULL);

	while (snd_card_next(&self->card) >= 0) {
		if (self->card == -1)
			break;
		snd_card_get_name(self->card, &card_name);
		if (!strcmp(card_name, "VirtualALSA"))
			break;
	}
	free(card_name);
	ASSERT_NE(self->card, -1);
}

/*
 * Here we are trying to send the looped monotonically increasing sequence of bytes to the driver.
 * If our data isn't corrupted, the driver will set the content of 'pc_test' debugfs file to '1'
 */
TEST_F(alsav, playback) {
	snd_pcm_t *handle;
	unsigned char *it;
	size_t write_res;
	int test_results;
	int i;
	unsigned short *rsamples;
	struct alsav_test_params *params = &self->params;

	snd_pcm_sw_params_alloca(&self->swparams);
	snd_pcm_hw_params_alloca(&self->hwparams);

	ASSERT_EQ(setup_handle(&handle, self->swparams, self->hwparams, params,
			       self->card, SND_PCM_STREAM_PLAYBACK), 0);
	snd_pcm_format_set_silence(params->format, self->samples,
				   params->rate * params->channels * params->time);
	it = (unsigned char *)self->samples;
	for (i = 0; i < self->params.sample_len * params->time; i++)
		it[i] = i % 256;
	rsamples = self->samples;
	for (i = 0; i < params->time; i++) {
		write_res = snd_pcm_writei(handle, rsamples, params->rate);
		rsamples += params->rate;
		ASSERT_GE(write_res, 0);
	}

	snd_pcm_close(handle);
	test_results = get_test_results("pc_test");
	ASSERT_EQ(test_results, 1);
}

/*
 * Here we test that the virtual alsa driver returns looped and monotonically increasing sequence
 * of bytes
 */
TEST_F(alsav, capture) {
	snd_pcm_t *handle;
	unsigned char *it;
	size_t read_res;
	int i;
	unsigned short *rsamples;
	struct alsav_test_params *params = &self->params;

	snd_pcm_sw_params_alloca(&self->swparams);
	snd_pcm_hw_params_alloca(&self->hwparams);

	ASSERT_EQ(setup_handle(&handle, self->swparams, self->hwparams,
			       params, self->card, SND_PCM_STREAM_CAPTURE), 0);
	snd_pcm_format_set_silence(params->format, self->samples,
				   params->rate * params->channels * params->time);
	rsamples = self->samples;
	for (i = 0; i < params->time; i++) {
		read_res = snd_pcm_readi(handle, rsamples, params->rate);
		rsamples += params->rate;
		ASSERT_GE(read_res, 0);
	}
	snd_pcm_close(handle);
	it = (unsigned char *)self->samples;
	for (i = 0; i < self->params.sample_len * self->params.time; i++)
		ASSERT_EQ(it[i], i % 256);
}

/*
 * Here we are testing the custom ioctl definition inside the virtual driver. If it triggers
 * successfully, the driver sets the content of 'ioctl_test' debugfs file to '1'.
 */
TEST_F(alsav, reset_ioctl) {
	snd_pcm_t *handle;
	unsigned char *it;
	int test_res;
	struct alsav_test_params *params = &self->params;

	snd_pcm_sw_params_alloca(&self->swparams);
	snd_pcm_hw_params_alloca(&self->hwparams);

	ASSERT_EQ(setup_handle(&handle, self->swparams, self->hwparams, params,
			       self->card, SND_PCM_STREAM_CAPTURE), 0);
	snd_pcm_reset(handle);
	test_res = get_test_results("ioctl_test");
	ASSERT_EQ(test_res, 1);
	snd_pcm_close(handle);
}

TEST_HARNESS_MAIN
