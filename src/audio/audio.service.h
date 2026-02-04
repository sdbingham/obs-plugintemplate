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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_service_init(void);
void audio_service_unload(void);

/** Set the audio source by name (e.g. from settings). Saves to config. Call before start. */
void audio_service_set_source(const char *name);

/** Get the current audio source name (saved in config). Returns empty string if none. */
const char *audio_service_get_source(void);

/** Optional "mute source": only transcribe when this source is unmuted/active/showing. Empty = use same source. */
void audio_service_set_mute_source(const char *name);
const char *audio_service_get_mute_source(void);

/** If false, do not push audio when capture source is muted. Default false. */
void audio_service_set_process_while_muted(bool value);
bool audio_service_get_process_while_muted(void);

/** If true, only push when capture source is active and showing. Default true. */
void audio_service_set_only_when_visible(bool value);
bool audio_service_get_only_when_visible(void);

/** Chunk latency in milliseconds (controls live caption delay). */
uint32_t audio_service_get_latency_ms(void);
void audio_service_set_latency_ms(uint32_t value);

/** Start capturing from the selected source. Called by frontend when recording starts. */
void audio_service_start(void);

/** Stop capturing. Called by frontend when recording stops. */
void audio_service_stop(void);

#ifdef __cplusplus
}
#endif
