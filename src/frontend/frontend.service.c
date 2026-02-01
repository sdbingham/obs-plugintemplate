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
#include "frontend/frontend.service.h"
#include "audio/audio.service.h"
#if ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#include <util/platform.h>
#endif
#if ENABLE_QT
#include "frontend/frontend_settings_ui.h"
#endif
#include <stdbool.h>
#include <stdint.h>

#if ENABLE_FRONTEND_API
static bool s_recording = false;
static uint64_t s_t0_ns = 0;

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	(void)private_data;
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		s_recording = true;
		s_t0_ns = os_gettime_ns();
		audio_service_start();
		obs_log(LOG_INFO, "recording started, t0 set");
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		s_recording = false;
		s_t0_ns = 0;
		audio_service_stop();
		obs_log(LOG_INFO, "recording stopped");
	}
}
#endif

#if ENABLE_QT
static void on_tools_menu_clicked(void *private_data)
{
	(void)private_data;
	frontend_settings_show_dialog();
}
#endif

void frontend_service_init(void)
{
#if ENABLE_FRONTEND_API
	obs_frontend_add_event_callback(on_frontend_event, NULL);
#endif
#if ENABLE_QT
	obs_frontend_add_tools_menu_item("Transcription Settings", on_tools_menu_clicked, NULL);
#endif
}

void frontend_service_unload(void)
{
#if ENABLE_FRONTEND_API
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	s_recording = false;
	s_t0_ns = 0;
#endif
}

bool frontend_service_is_recording(void)
{
#if ENABLE_FRONTEND_API
	return s_recording;
#else
	(void)0;
	return false;
#endif
}

uint64_t frontend_service_get_elapsed_ms(void)
{
#if ENABLE_FRONTEND_API
	if (!s_recording || s_t0_ns == 0)
		return 0;
	return (os_gettime_ns() - s_t0_ns) / 1000000;
#else
	(void)0;
	return 0;
#endif
}
