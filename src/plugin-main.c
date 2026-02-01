/*
Plugin Name
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

#include <obs-module.h>
#include "common/plugin-support.h"
#include "transcription/transcription.module.h"
#include "audio/audio.module.h"
#include "captions/captions.module.h"
#include "frontend/frontend.module.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);

	/* Load feature modules (NestJS-style: module init order) */
	frontend_module_init();
	audio_module_init();
	transcription_module_init();
	captions_module_init();

	return true;
}

void obs_module_unload(void)
{
	/* Unload in reverse order */
	captions_module_unload();
	transcription_module_unload();
	audio_module_unload();
	frontend_module_unload();

	obs_log(LOG_INFO, "plugin unloaded");
}
