/*
OBS Transcription
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "config.h"
#include "common/plugin-support.h"
#include "audio/audio.service.h"
#include <obs.h>
#include <obs-source.h>
#include <media-io/audio-resampler.h>
#include <util/platform.h>
#include <util/threading.h>
#include <string.h>
#include <stdlib.h>

#define CHUNK_SEC 1
#define CHUNK_FRAMES (16000 * CHUNK_SEC)

static char *s_audio_source_name = NULL;
static obs_source_t *s_capture_source = NULL;
static audio_resampler_t *s_resampler = NULL;
static float *s_buffer = NULL;
static uint32_t s_buffer_frames = 0;
static pthread_mutex_t s_buffer_mutex;

static void on_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	(void)param;
	(void)source;
	if (muted || !s_resampler || !s_buffer)
		return;

	uint8_t *out_planes[1] = {NULL};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	bool res = audio_resampler_resample(s_resampler, out_planes, &out_frames, &ts_offset,
					    (const uint8_t *const *)audio_data->data, audio_data->frames);
	if (!res || out_frames == 0 || !out_planes[0])
		return;

	float *out = (float *)out_planes[0];

	pthread_mutex_lock(&s_buffer_mutex);
	if (s_buffer_frames + out_frames <= CHUNK_FRAMES) {
		memcpy(s_buffer + s_buffer_frames, out, out_frames * sizeof(float));
		s_buffer_frames += out_frames;
	} else {
		s_buffer_frames = 0;
		obs_log(LOG_DEBUG, "audio chunk ready (1 s), discarded (Phase 1)");
	}
	pthread_mutex_unlock(&s_buffer_mutex);
}

void audio_service_init(void)
{
	pthread_mutex_init_value(&s_buffer_mutex);
}

void audio_service_unload(void)
{
	audio_service_stop();
	if (s_audio_source_name) {
		bfree(s_audio_source_name);
		s_audio_source_name = NULL;
	}
	pthread_mutex_destroy(&s_buffer_mutex);
}

void audio_service_set_source(const char *name)
{
	if (s_audio_source_name)
		bfree(s_audio_source_name);
	s_audio_source_name = name ? bstrdup(name) : NULL;
}

void audio_service_start(void)
{
	if (!s_audio_source_name || !s_audio_source_name[0]) {
		obs_log(LOG_INFO, "no audio source selected; skipping capture");
		return;
	}
	obs_source_t *source = obs_get_source_by_name(s_audio_source_name);
	if (!source) {
		obs_log(LOG_WARNING, "audio source not found: %s", s_audio_source_name);
		return;
	}
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0) {
		obs_log(LOG_WARNING, "source is not audio: %s", s_audio_source_name);
		obs_source_release(source);
		return;
	}

	struct resample_info dst = {
		.samples_per_sec = 16000,
		.format = AUDIO_FORMAT_FLOAT,
		.speakers = SPEAKERS_MONO,
	};
	struct resample_info src = {
		.samples_per_sec = 48000,
		.format = AUDIO_FORMAT_FLOAT,
		.speakers = SPEAKERS_STEREO,
	};
	s_resampler = audio_resampler_create(&dst, &src);
	if (!s_resampler) {
		obs_log(LOG_ERROR, "failed to create audio resampler");
		obs_source_release(source);
		return;
	}

	s_buffer = (float *)bmalloc(CHUNK_FRAMES * sizeof(float));
	s_buffer_frames = 0;
	s_capture_source = source;
	obs_source_add_audio_capture_callback(source, on_audio_capture, NULL);
	obs_log(LOG_INFO, "audio capture started for source: %s", s_audio_source_name);
}

void audio_service_stop(void)
{
	if (s_capture_source) {
		obs_source_remove_audio_capture_callback(s_capture_source, on_audio_capture, NULL);
		obs_source_release(s_capture_source);
		s_capture_source = NULL;
	}
	if (s_resampler) {
		audio_resampler_destroy(s_resampler);
		s_resampler = NULL;
	}
	if (s_buffer) {
		bfree(s_buffer);
		s_buffer = NULL;
	}
	s_buffer_frames = 0;
	obs_log(LOG_INFO, "audio capture stopped");
}
