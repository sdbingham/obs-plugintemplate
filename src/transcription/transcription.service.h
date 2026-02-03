/*
Plugin Name
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

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void transcription_service_init(void);
void transcription_service_unload(void);

/** Reset segment/caption state for a new recording. Call from frontend on RECORDING_STARTING. */
void transcription_service_reset_for_recording(void);

/** Push an audio chunk (16 kHz mono float) for transcription. Called from audio thread. */
void transcription_service_push_audio_chunk(const float *samples, uint32_t num_samples, uint64_t start_ts_ms);

/** Get the caption text to display at the given recording elapsed time (ms). Called from main thread. */
void transcription_service_get_display_text(uint64_t elapsed_ms, char *buf, size_t buf_size);

/** Write caption segments to an SRT file. Path is the recording video path; SRT is written to same dir with .srt extension. Called from main thread. */
void transcription_service_write_srt(const char *video_path);

/** Wait until the transcription worker queue drains (no pending chunks). Returns true if idle before timeout. */
bool transcription_service_wait_for_idle(uint32_t timeout_ms);

/** Minimum speech confidence (0.0–1.0). Segments with Whisper no-speech probability above (1 - value) are hidden. Default 0.4. */
float transcription_service_get_speech_confidence_min(void);
void transcription_service_set_speech_confidence_min(float value);

/** Semicolon-separated phrases to remove from segment text (literal match). Empty = none. */
const char *transcription_service_get_filter_phrases(void);
void transcription_service_set_filter_phrases(const char *value);

/** Semicolon-separated "from|to" replacement pairs (literal). E.g. "foo|bar; x|y". Empty = none. */
const char *transcription_service_get_replace_phrases(void);
void transcription_service_set_replace_phrases(const char *value);

#ifdef __cplusplus
}
#endif
