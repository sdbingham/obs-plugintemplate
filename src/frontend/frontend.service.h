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

void frontend_service_init(void);
void frontend_service_unload(void);

/** True while recording is active (after RECORDING_STARTED, until RECORDING_STOPPED). */
bool frontend_service_is_recording(void);

/** Elapsed milliseconds since recording start; 0 if not recording. */
uint64_t frontend_service_get_elapsed_ms(void);

#ifdef __cplusplus
}
#endif
