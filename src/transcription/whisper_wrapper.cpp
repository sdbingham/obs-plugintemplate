/*
OBS Transcription
Copyright (C) <Year> <Developer> <Email Address>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "transcription/whisper_wrapper.h"
#include <whisper.h>
#include <cstring>

extern "C" {

whisper_wrapper_ctx_t whisper_wrapper_init(const char *model_path, int use_gpu)
{
	if (!model_path || !model_path[0])
		return nullptr;
	struct whisper_context_params cparams = whisper_context_default_params();
	cparams.use_gpu = (use_gpu != 0);
	cparams.gpu_device = 0;
	struct whisper_context *ctx =
		whisper_init_from_file_with_params(model_path, cparams);
	return static_cast<whisper_wrapper_ctx_t>(ctx);
}

void whisper_wrapper_free(whisper_wrapper_ctx_t ctx)
{
	if (ctx)
		whisper_free(static_cast<struct whisper_context *>(ctx));
}

static uint64_t s_last_start_ms = 0;

int whisper_wrapper_run(whisper_wrapper_ctx_t ctx, const float *samples, int n_samples, uint64_t start_ms,
			const char *language, const char *initial_prompt)
{
	if (!ctx || !samples || n_samples <= 0)
		return -1;
	s_last_start_ms = start_ms;
	struct whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
	wparams.n_threads = 4;
	wparams.no_timestamps = false;
	const bool detect_language = (!language || !language[0] || strcmp(language, "auto") == 0);
	wparams.language = detect_language ? "auto" : language;
	wparams.detect_language = detect_language;
	/* Reduce hallucinations: suppress blank and non-speech tokens. */
	wparams.suppress_blank = true;
	wparams.suppress_nst = true;
	wparams.initial_prompt = (initial_prompt && initial_prompt[0]) ? initial_prompt : nullptr;
	wparams.no_speech_thold = 0.5f;   /* let more segments through; we filter junk via is_known_hallucination */
	/* Allow context to improve accuracy across chunks. */
	wparams.no_context = false;
	wparams.beam_search.beam_size = 5;
	int ret = whisper_full(static_cast<struct whisper_context *>(ctx), wparams, samples, n_samples);
	return (ret == 0) ? 0 : -1;
}

int whisper_wrapper_get_segment_count(whisper_wrapper_ctx_t ctx)
{
	if (!ctx)
		return 0;
	return whisper_full_n_segments(static_cast<struct whisper_context *>(ctx));
}

void whisper_wrapper_get_segment(whisper_wrapper_ctx_t ctx, int i, char *text_buf, size_t buf_size,
				 uint64_t *t0_ms, uint64_t *t1_ms)
{
	if (!ctx || !text_buf || buf_size == 0)
		return;
	struct whisper_context *wctx = static_cast<struct whisper_context *>(ctx);
	const char *text = whisper_full_get_segment_text(wctx, i);
	if (text) {
		size_t len = strlen(text);
		if (len >= buf_size)
			len = buf_size - 1;
		memcpy(text_buf, text, len);
		text_buf[len] = '\0';
	} else {
		text_buf[0] = '\0';
	}
	/* Whisper returns t0/t1 in centiseconds (1/100 s); convert to ms for recording time. */
	if (t0_ms) {
		int64_t t0_cs = whisper_full_get_segment_t0(wctx, i);
		*t0_ms = (t0_cs >= 0) ? (s_last_start_ms + (uint64_t)(t0_cs * 10)) : s_last_start_ms;
	}
	if (t1_ms) {
		int64_t t1_cs = whisper_full_get_segment_t1(wctx, i);
		*t1_ms = (t1_cs >= 0) ? (s_last_start_ms + (uint64_t)(t1_cs * 10)) : s_last_start_ms;
	}
}

float whisper_wrapper_get_segment_no_speech_prob(whisper_wrapper_ctx_t ctx, int i)
{
	if (!ctx)
		return 1.0f;
	return whisper_full_get_segment_no_speech_prob(static_cast<struct whisper_context *>(ctx), i);
}

} /* extern "C" */
