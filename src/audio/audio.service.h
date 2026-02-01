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

#ifdef __cplusplus
extern "C" {
#endif

void audio_service_init(void);
void audio_service_unload(void);

/** Set the audio source by name (e.g. from settings). Call before start. */
void audio_service_set_source(const char *name);

/** Start capturing from the selected source. Called by frontend when recording starts. */
void audio_service_start(void);

/** Stop capturing. Called by frontend when recording stops. */
void audio_service_stop(void);

#ifdef __cplusplus
}
#endif
