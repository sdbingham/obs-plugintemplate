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
#include "transcription/transcription.service.h"
#include <obs.h>
#include <obs-module.h>
#include <util/threading.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#if ENABLE_WHISPER
#include "transcription/whisper_wrapper.h"
#endif

#define CAPTION_SEGMENT_MAX 128
#define CAPTION_TEXT_MAX 256
#define STUB_SEGMENT_DURATION_MS 2000
#define DEFAULT_MODEL_SUBPATH "models/ggml-tiny.en.bin"

typedef struct {
	char text[CAPTION_TEXT_MAX];
	uint64_t start_ms;
	uint64_t end_ms;
} caption_segment_t;

static caption_segment_t s_caption_segments[CAPTION_SEGMENT_MAX];
static uint32_t s_caption_count = 0;
static pthread_mutex_t s_caption_mutex;

typedef struct {
	float *samples;
	uint32_t num_samples;
	uint64_t start_ts_ms;
} audio_chunk_t;

static audio_chunk_t s_pending_chunk;
static bool s_worker_running = false;
static pthread_t s_worker_thread;
static pthread_mutex_t s_input_mutex;
static pthread_cond_t s_input_cond;
#if ENABLE_WHISPER
static void *s_whisper_ctx = NULL; /* whisper_wrapper_ctx_t */
#endif

static void push_caption_segment(const char *text, uint64_t start_ms, uint64_t end_ms)
{
	pthread_mutex_lock(&s_caption_mutex);
	if (s_caption_count >= CAPTION_SEGMENT_MAX) {
		/* Drop oldest */
		memmove(&s_caption_segments[0], &s_caption_segments[1], (s_caption_count - 1) * sizeof(caption_segment_t));
		s_caption_count--;
	}
	caption_segment_t *seg = &s_caption_segments[s_caption_count];
	size_t len = strlen(text);
	if (len >= CAPTION_TEXT_MAX)
		len = CAPTION_TEXT_MAX - 1;
	memcpy(seg->text, text, len);
	seg->text[len] = '\0';
	seg->start_ms = start_ms;
	seg->end_ms = end_ms;
	s_caption_count++;
	pthread_mutex_unlock(&s_caption_mutex);
}

static void *worker_thread(void *arg)
{
	(void)arg;
	while (s_worker_running) {
		pthread_mutex_lock(&s_input_mutex);
		while (s_worker_running && s_pending_chunk.samples == NULL)
			pthread_cond_wait(&s_input_cond, &s_input_mutex);
		audio_chunk_t chunk = s_pending_chunk;
		s_pending_chunk.samples = NULL;
		pthread_mutex_unlock(&s_input_mutex);

		if (chunk.samples == NULL)
			continue;

#if ENABLE_WHISPER
		if (s_whisper_ctx) {
			int ret = whisper_wrapper_run(s_whisper_ctx, chunk.samples, chunk.num_samples, chunk.start_ts_ms);
			if (ret == 0) {
				int n = whisper_wrapper_get_segment_count(s_whisper_ctx);
				for (int i = 0; i < n; i++) {
					char seg_text[CAPTION_TEXT_MAX];
					uint64_t t0_ms, t1_ms;
					whisper_wrapper_get_segment(s_whisper_ctx, i, seg_text, sizeof(seg_text), &t0_ms, &t1_ms);
					if (seg_text[0] != '\0')
						push_caption_segment(seg_text, t0_ms, t1_ms);
				}
			}
		} else {
			uint64_t end_ms = chunk.start_ts_ms + STUB_SEGMENT_DURATION_MS;
			push_caption_segment("[Phase 2 — transcription stub]", chunk.start_ts_ms, end_ms);
		}
#else
		uint64_t end_ms = chunk.start_ts_ms + STUB_SEGMENT_DURATION_MS;
		push_caption_segment("[Phase 2 — transcription stub]", chunk.start_ts_ms, end_ms);
#endif

		bfree(chunk.samples);
	}
	return NULL;
}

void transcription_service_push_audio_chunk(const float *samples, uint32_t num_samples, uint64_t start_ts_ms)
{
	if (!samples || num_samples == 0)
		return;
	float *copy = (float *)bmalloc(num_samples * sizeof(float));
	if (!copy)
		return;
	memcpy(copy, samples, num_samples * sizeof(float));

	pthread_mutex_lock(&s_input_mutex);
	if (s_pending_chunk.samples != NULL) {
		bfree(s_pending_chunk.samples);
	}
	s_pending_chunk.samples = copy;
	s_pending_chunk.num_samples = num_samples;
	s_pending_chunk.start_ts_ms = start_ts_ms;
	pthread_cond_signal(&s_input_cond);
	pthread_mutex_unlock(&s_input_mutex);
}

void transcription_service_get_display_text(uint64_t elapsed_ms, char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;
	buf[0] = '\0';

	pthread_mutex_lock(&s_caption_mutex);
	for (uint32_t i = s_caption_count; i > 0; i--) {
		caption_segment_t *seg = &s_caption_segments[i - 1];
		if (elapsed_ms >= seg->start_ms && elapsed_ms <= seg->end_ms) {
			size_t len = strlen(seg->text);
			if (len >= buf_size)
				len = buf_size - 1;
			memcpy(buf, seg->text, len);
			buf[len] = '\0';
			pthread_mutex_unlock(&s_caption_mutex);
			return;
		}
	}
	/* No segment at this time: show most recent that has started */
	for (uint32_t i = s_caption_count; i > 0; i--) {
		caption_segment_t *seg = &s_caption_segments[i - 1];
		if (elapsed_ms >= seg->start_ms) {
			size_t len = strlen(seg->text);
			if (len >= buf_size)
				len = buf_size - 1;
			memcpy(buf, seg->text, len);
			buf[len] = '\0';
			break;
		}
	}
	pthread_mutex_unlock(&s_caption_mutex);
}

static void ms_to_srt_time(uint64_t ms, char *buf, size_t buf_size)
{
	uint64_t h = ms / 3600000;
	ms %= 3600000;
	uint64_t m = ms / 60000;
	ms %= 60000;
	uint64_t s = ms / 1000;
	uint64_t millis = ms % 1000;
	snprintf(buf, buf_size, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ",%03" PRIu64, h, m, s, millis);
}

void transcription_service_write_srt(const char *video_path)
{
	if (!video_path || !video_path[0])
		return;
	/* Derive SRT path: same directory and base name, .srt extension */
	char srt_path[1024];
	const char *dot = strrchr(video_path, '.');
	if (dot && (size_t)(dot - video_path) < sizeof(srt_path) - 5) {
		size_t len = (size_t)(dot - video_path);
		memcpy(srt_path, video_path, len);
		srt_path[len] = '\0';
		strcat(srt_path, ".srt");
	} else {
		snprintf(srt_path, sizeof(srt_path), "%s.srt", video_path);
	}
	FILE *f = fopen(srt_path, "w");
	if (!f) {
		obs_log(LOG_WARNING, "transcription: could not open SRT file: %s", srt_path);
		return;
	}
	pthread_mutex_lock(&s_caption_mutex);
	for (uint32_t i = 0; i < s_caption_count; i++) {
		caption_segment_t *seg = &s_caption_segments[i];
		char start_buf[32];
		char end_buf[32];
		ms_to_srt_time(seg->start_ms, start_buf, sizeof(start_buf));
		ms_to_srt_time(seg->end_ms, end_buf, sizeof(end_buf));
		fprintf(f, "%u\n%s --> %s\n%s\n\n", (unsigned)(i + 1), start_buf, end_buf, seg->text);
	}
	pthread_mutex_unlock(&s_caption_mutex);
	fclose(f);
	obs_log(LOG_INFO, "transcription: wrote SRT to %s", srt_path);
}

void transcription_service_init(void)
{
	pthread_mutex_init_value(&s_caption_mutex);
	pthread_mutex_init_value(&s_input_mutex);
	pthread_cond_init(&s_input_cond, NULL);
	s_pending_chunk.samples = NULL;
	s_caption_count = 0;
#if ENABLE_WHISPER
	s_whisper_ctx = NULL;
	char *model_path = obs_module_config_path(DEFAULT_MODEL_SUBPATH);
	if (model_path) {
		FILE *f = fopen(model_path, "rb");
		if (f) {
			fclose(f);
			s_whisper_ctx = whisper_wrapper_init(model_path, 1);
			if (s_whisper_ctx)
				obs_log(LOG_INFO, "transcription: Whisper loaded from %s", model_path);
			else {
				s_whisper_ctx = whisper_wrapper_init(model_path, 0);
				if (s_whisper_ctx)
					obs_log(LOG_INFO, "transcription: Whisper loaded (CPU) from %s", model_path);
			}
		}
		if (!s_whisper_ctx)
			obs_log(LOG_WARNING, "transcription: no model at %s; using stub. Add ggml-tiny.en.bin to plugin_config/obs-transcription/models/", model_path ? model_path : "(null)");
		bfree(model_path);
	}
#endif

	s_worker_running = true;
	if (pthread_create(&s_worker_thread, NULL, worker_thread, NULL) != 0) {
		obs_log(LOG_ERROR, "transcription: failed to start worker thread");
		s_worker_running = false;
	}
}

void transcription_service_unload(void)
{
	s_worker_running = false;
	pthread_mutex_lock(&s_input_mutex);
	pthread_cond_signal(&s_input_cond);
	pthread_mutex_unlock(&s_input_mutex);
	pthread_join(s_worker_thread, NULL);

	if (s_pending_chunk.samples) {
		bfree(s_pending_chunk.samples);
		s_pending_chunk.samples = NULL;
	}
#if ENABLE_WHISPER
	if (s_whisper_ctx) {
		whisper_wrapper_free(s_whisper_ctx);
		s_whisper_ctx = NULL;
	}
#endif

	pthread_cond_destroy(&s_input_cond);
	pthread_mutex_destroy(&s_input_mutex);
	pthread_mutex_destroy(&s_caption_mutex);
}
