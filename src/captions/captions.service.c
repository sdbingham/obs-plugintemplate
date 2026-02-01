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
#include "captions/captions.service.h"
#include "frontend/frontend.service.h"
#include <obs.h>
#include <obs-source.h>
#include <obs-data.h>
#if ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#endif
#include <stdlib.h>

#define CAPTION_SOURCE_NAME "Transcription caption"
#define CAPTION_TEST_STRING "Phase 1 — test"

#if ENABLE_FRONTEND_API
static obs_source_t *s_caption_source = NULL;
static obs_sceneitem_t *s_caption_item = NULL;
static uint64_t s_tick_count = 0;

static void on_tick(void *param, float seconds)
{
	(void)param;
	(void)seconds;
	if (!s_caption_source)
		return;
	s_tick_count++;
	/* Update text every ~100 ms (approx 6 ticks at 60 fps) with test string */
	if (s_tick_count % 6 != 0)
		return;
	obs_data_t *settings = obs_source_get_settings(s_caption_source);
	if (settings) {
		obs_data_set_string(settings, "text", CAPTION_TEST_STRING);
		obs_source_update(s_caption_source, settings);
		obs_data_release(settings);
	}
}
#endif

void captions_service_init(void)
{
#if ENABLE_FRONTEND_API
	obs_data_t *settings = obs_data_create();
	if (!settings)
		return;
	obs_data_set_string(settings, "text", "");
	obs_data_set_bool(settings, "read_from_file", false);
	s_caption_source = obs_source_create("text_ft2_source", CAPTION_SOURCE_NAME, settings, NULL);
	obs_data_release(settings);
	if (!s_caption_source)
		return;
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (scene_source) {
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		if (scene) {
			s_caption_item = obs_scene_add(scene, s_caption_source);
			obs_scene_release(scene);
			if (s_caption_item) {
				obs_add_tick_callback(on_tick, NULL);
			} else {
				obs_source_release(s_caption_source);
				s_caption_source = NULL;
			}
		} else {
			obs_source_release(s_caption_source);
			s_caption_source = NULL;
		}
		obs_source_release(scene_source);
	} else {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
	}
#endif
}

void captions_service_unload(void)
{
#if ENABLE_FRONTEND_API
	obs_remove_tick_callback(on_tick, NULL);
	if (s_caption_item) {
		obs_sceneitem_remove(s_caption_item);
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}
	if (s_caption_source) {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
	}
#endif
}
