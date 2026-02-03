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

#ifndef OBS_TRANSCRIPTION_WHISPER_WRAPPER_H
#define OBS_TRANSCRIPTION_WHISPER_WRAPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque Whisper context for C callers. */
typedef void *whisper_wrapper_ctx_t;

/**
 * Load model and create context.
 * \param model_path Path to ggml model (e.g. ggml-tiny.en.bin).
 * \param use_gpu 1 to try GPU (CUDA), 0 for CPU only.
 * \return Context or NULL on failure.
 */
whisper_wrapper_ctx_t whisper_wrapper_init(const char *model_path, int use_gpu);

/** Free context and release model. */
void whisper_wrapper_free(whisper_wrapper_ctx_t ctx);

/**
 * Run transcription on PCM (16 kHz mono float).
 * \param ctx Context from whisper_wrapper_init.
 * \param samples PCM float samples.
 * \param n_samples Number of samples.
 * \param start_ms Start time of this chunk in recording (ms); added to segment t0/t1.
 * \return 0 on success, non-zero on failure.
 */
int whisper_wrapper_run(whisper_wrapper_ctx_t ctx, const float *samples, int n_samples, uint64_t start_ms);

/** Number of segments from last whisper_wrapper_run. */
int whisper_wrapper_get_segment_count(whisper_wrapper_ctx_t ctx);

/**
 * Get segment text and timestamps (recording time = start_ms + whisper segment time).
 * \param ctx Context.
 * \param i Segment index (0 .. count-1).
 * \param text_buf Output buffer for UTF-8 text.
 * \param buf_size Size of text_buf.
 * \param t0_ms Output: segment start time (ms, recording).
 * \param t1_ms Output: segment end time (ms, recording).
 */
void whisper_wrapper_get_segment(whisper_wrapper_ctx_t ctx, int i, char *text_buf, size_t buf_size,
				 uint64_t *t0_ms, uint64_t *t1_ms);

/**
 * No-speech probability for segment i (0..count-1) from last whisper_wrapper_run.
 * Higher = Whisper thinks this segment is likely silence/no speech. Use to drop
 * low-confidence segments instead of a text blocklist.
 */
float whisper_wrapper_get_segment_no_speech_prob(whisper_wrapper_ctx_t ctx, int i);

#ifdef __cplusplus
}
#endif

#endif
