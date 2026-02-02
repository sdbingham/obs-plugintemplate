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

void captions_service_init(void);
void captions_service_unload(void);

/** Add caption source to the current scene (e.g. when recording starts). Call from main thread. */
void captions_service_attach_to_current_scene(void);

/** Remove caption from scene and release scene item. Call before scene collection unloads. */
void captions_service_detach_from_scene(void);

#ifdef __cplusplus
}
#endif
