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
#include "frontend/frontend.service.h"
#include "transcription/transcription.service.h"
#include <obs.h>
#include <obs-module.h>
#include <obs-source.h>
#include <media-io/audio-resampler.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/config-file.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define CONFIG_SECTION "Transcription"
#define CONFIG_KEY_AUDIO_SOURCE "AudioSource"
#define CONFIG_KEY_MUTE_SOURCE "MuteSource"
#define CONFIG_KEY_PROCESS_WHILE_MUTED "ProcessWhileMuted"
#define CONFIG_KEY_ONLY_WHEN_VISIBLE "OnlyWhenVisible"

#define CHUNK_SEC 3
#define CHUNK_FRAMES (16000 * CHUNK_SEC)

static char *s_audio_source_name = NULL;
static char *s_mute_source_name = NULL; /* when set, only push when this source is unmuted/active/showing */
static bool s_process_while_muted = false;
static bool s_only_when_visible = true;
static obs_source_t *s_capture_source = NULL;
static audio_resampler_t *s_resampler = NULL;
static float *s_buffer = NULL;
static uint32_t s_buffer_frames = 0;
static pthread_mutex_t s_buffer_mutex;
static uint64_t s_audio_sample_count = 0;
static double s_audio_sumsq = 0.0;
static float s_audio_peak = 0.0f;
static uint64_t s_audio_last_log_ns = 0;

static bool should_push_audio(void)
{
	if (!s_capture_source)
		return false;
	if (s_only_when_visible && (!obs_source_active(s_capture_source) || !obs_source_showing(s_capture_source)))
		return false;
	if (!s_process_while_muted && obs_source_muted(s_capture_source))
		return false;
	if (s_mute_source_name && s_mute_source_name[0]) {
		obs_source_t *mute_src = obs_get_source_by_name(s_mute_source_name);
		if (!mute_src) {
			/* Mute source deleted or renamed; allow push (same as "same source" behavior). */
			return true;
		}
		bool ok = obs_source_active(mute_src) && obs_source_showing(mute_src) && obs_source_enabled(mute_src) && !obs_source_muted(mute_src);
		obs_source_release(mute_src);
		if (!ok)
			return false;
	}
	return true;
}

static void on_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	(void)param;
	(void)source;
	if (!s_process_while_muted && muted)
		return;
	if (!s_resampler || !s_buffer)
		return;
	if (!should_push_audio())
		return;

	uint8_t *out_planes[1] = {NULL};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;
	bool res = audio_resampler_resample(s_resampler, out_planes, &out_frames, &ts_offset,
					    (const uint8_t *const *)audio_data->data, audio_data->frames);
	if (!res || out_frames == 0 || !out_planes[0])
		return;

	float *out = (float *)out_planes[0];

	/* Lightweight audio stats (debug): RMS/peak every ~2 seconds. */
	for (uint32_t i = 0; i < out_frames; i++) {
		float v = out[i];
		s_audio_sumsq += (double)v * (double)v;
		float av = fabsf(v);
		if (av > s_audio_peak)
			s_audio_peak = av;
	}
	s_audio_sample_count += out_frames;
	uint64_t now_ns = os_gettime_ns();
	if (s_audio_last_log_ns == 0)
		s_audio_last_log_ns = now_ns;
	if (now_ns - s_audio_last_log_ns >= 2000000000ULL) {
		double rms = (s_audio_sample_count > 0) ? sqrt(s_audio_sumsq / (double)s_audio_sample_count) : 0.0;
		obs_log(LOG_INFO, "audio stats: frames=%" PRIu64 " rms=%.6f peak=%.6f",
			s_audio_sample_count, rms, s_audio_peak);
		s_audio_sample_count = 0;
		s_audio_sumsq = 0.0;
		s_audio_peak = 0.0f;
		s_audio_last_log_ns = now_ns;
	}

	pthread_mutex_lock(&s_buffer_mutex);
	if (s_buffer_frames + out_frames <= CHUNK_FRAMES) {
		memcpy(s_buffer + s_buffer_frames, out, out_frames * sizeof(float));
		s_buffer_frames += out_frames;
	} else {
		/* Chunk full: push current buffer to transcription */
		uint64_t elapsed_ms = frontend_service_get_elapsed_ms();
		uint64_t chunk_duration_ms = (uint64_t)s_buffer_frames * 1000 / 16000;
		uint64_t start_ts_ms = (elapsed_ms >= chunk_duration_ms) ? (elapsed_ms - chunk_duration_ms) : 0;
		float *chunk_copy = (float *)bmalloc(s_buffer_frames * sizeof(float));
		if (chunk_copy) {
			memcpy(chunk_copy, s_buffer, s_buffer_frames * sizeof(float));
			transcription_service_push_audio_chunk(chunk_copy, s_buffer_frames, start_ts_ms);
		}
		/* Keep overflow from this callback for the next chunk */
		uint32_t take = out_frames;
		if (take > CHUNK_FRAMES)
			take = CHUNK_FRAMES;
		memcpy(s_buffer, out, take * sizeof(float));
		s_buffer_frames = take;
	}
	pthread_mutex_unlock(&s_buffer_mutex);
}

static void save_audio_source_config(void)
{
	char *path = obs_module_config_path("config.ini");
	if (!path)
		return;
	config_t *config = NULL;
	if (config_open(&config, path, CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS) {
		bfree(path);
		return;
	}
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_AUDIO_SOURCE, s_audio_source_name ? s_audio_source_name : "");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_MUTE_SOURCE, s_mute_source_name ? s_mute_source_name : "");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_PROCESS_WHILE_MUTED, s_process_while_muted ? "true" : "false");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_ONLY_WHEN_VISIBLE, s_only_when_visible ? "true" : "false");
	config_save(config);
	config_close(config);
	bfree(path);
}

static void load_audio_source_config(void)
{
	char *path = obs_module_config_path("config.ini");
	if (!path)
		return;
	config_t *config = NULL;
	if (config_open(&config, path, CONFIG_OPEN_EXISTING) != CONFIG_SUCCESS) {
		bfree(path);
		return;
	}
	const char *saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_AUDIO_SOURCE);
	if (saved && saved[0]) {
		if (s_audio_source_name)
			bfree(s_audio_source_name);
		s_audio_source_name = bstrdup(saved);
	}
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_MUTE_SOURCE);
	if (saved) {
		if (s_mute_source_name)
			bfree(s_mute_source_name);
		s_mute_source_name = saved[0] ? bstrdup(saved) : NULL;
	}
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_PROCESS_WHILE_MUTED);
	s_process_while_muted = (saved && (strcmp(saved, "true") == 0 || strcmp(saved, "1") == 0));
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_ONLY_WHEN_VISIBLE);
	s_only_when_visible = true;
	if (saved && (strcmp(saved, "false") == 0 || strcmp(saved, "0") == 0))
		s_only_when_visible = false;
	config_close(config);
	bfree(path);
}

void audio_service_init(void)
{
	pthread_mutex_init_value(&s_buffer_mutex);
	load_audio_source_config();
}

void audio_service_unload(void)
{
	audio_service_stop();
	if (s_audio_source_name) {
		bfree(s_audio_source_name);
		s_audio_source_name = NULL;
	}
	if (s_mute_source_name) {
		bfree(s_mute_source_name);
		s_mute_source_name = NULL;
	}
	pthread_mutex_destroy(&s_buffer_mutex);
}

void audio_service_set_source(const char *name)
{
	if (s_audio_source_name)
		bfree(s_audio_source_name);
	s_audio_source_name = name && name[0] ? bstrdup(name) : NULL;
	save_audio_source_config();
}

const char *audio_service_get_source(void)
{
	return s_audio_source_name ? s_audio_source_name : "";
}

void audio_service_set_mute_source(const char *name)
{
	if (s_mute_source_name)
		bfree(s_mute_source_name);
	s_mute_source_name = (name && name[0]) ? bstrdup(name) : NULL;
	save_audio_source_config();
}

const char *audio_service_get_mute_source(void)
{
	return s_mute_source_name ? s_mute_source_name : "";
}

void audio_service_set_process_while_muted(bool value)
{
	s_process_while_muted = value;
	save_audio_source_config();
}

bool audio_service_get_process_while_muted(void)
{
	return s_process_while_muted;
}

void audio_service_set_only_when_visible(bool value)
{
	s_only_when_visible = value;
	save_audio_source_config();
}

bool audio_service_get_only_when_visible(void)
{
	return s_only_when_visible;
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

	struct obs_audio_info ai;
	bool have_audio_info = obs_get_audio_info(&ai);
	if (!have_audio_info) {
		obs_log(LOG_WARNING, "failed to get OBS audio info; using default 48k stereo");
		ai.samples_per_sec = 48000;
		ai.speakers = SPEAKERS_STEREO;
	}

	struct resample_info dst = {
		.samples_per_sec = 16000,
		.format = AUDIO_FORMAT_FLOAT,
		.speakers = SPEAKERS_MONO,
	};
	struct resample_info src = {
		.samples_per_sec = ai.samples_per_sec,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = ai.speakers,
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
	obs_log(LOG_INFO, "audio capture started for source: %s (src: %u Hz, format=%d, speakers=%d)",
		s_audio_source_name, src.samples_per_sec, (int)src.format, (int)src.speakers);
}

void audio_service_stop(void)
{
	if (s_capture_source) {
		obs_source_remove_audio_capture_callback(s_capture_source, on_audio_capture, NULL);
		obs_source_release(s_capture_source);
		s_capture_source = NULL;
	}
	if (s_buffer && s_buffer_frames > 0) {
		pthread_mutex_lock(&s_buffer_mutex);
		if (s_buffer_frames > 0) {
			uint64_t elapsed_ms = frontend_service_get_elapsed_ms();
			uint64_t chunk_duration_ms = (uint64_t)s_buffer_frames * 1000 / 16000;
			uint64_t start_ts_ms = (elapsed_ms >= chunk_duration_ms) ? (elapsed_ms - chunk_duration_ms) : 0;
			float *chunk_copy = (float *)bmalloc(s_buffer_frames * sizeof(float));
			if (chunk_copy) {
				memcpy(chunk_copy, s_buffer, s_buffer_frames * sizeof(float));
				transcription_service_push_audio_chunk(chunk_copy, s_buffer_frames, start_ts_ms);
				obs_log(LOG_INFO, "audio capture flush: pushed %u frames", s_buffer_frames);
			}
			s_buffer_frames = 0;
		}
		pthread_mutex_unlock(&s_buffer_mutex);
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
