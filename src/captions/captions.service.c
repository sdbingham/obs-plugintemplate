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
#include "transcription/transcription.service.h"
#include <obs.h>
#include <obs-source.h>
#include <obs-data.h>
#if ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#endif
#include <stdlib.h>

#define CAPTION_SOURCE_NAME "Transcription caption"
#define CAPTION_DISPLAY_BUF_SIZE 512

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
	/* Update text every ~100 ms (approx 6 ticks at 60 fps) */
	if (s_tick_count % 6 != 0)
		return;
	uint64_t elapsed_ms = frontend_service_get_elapsed_ms();
	char buf[CAPTION_DISPLAY_BUF_SIZE];
	transcription_service_get_display_text(elapsed_ms, buf, sizeof(buf));
	obs_data_t *settings = obs_source_get_settings(s_caption_source);
	if (settings) {
		obs_data_set_string(settings, "text", buf);
		obs_source_update(s_caption_source, settings);
		obs_data_release(settings);
	}
}
#endif

void captions_service_init(void)
{
#if ENABLE_FRONTEND_API
	obs_add_tick_callback(on_tick, NULL);
	/* Caption source is created on first attach (recording start or finished loading),
	 * so we never hold a source across scene collection changes at plugin load. */
#endif
}

void captions_service_attach_to_current_scene(void)
{
#if ENABLE_FRONTEND_API
	/* Recreate caption source if it was released (e.g. after scene collection change) */
	if (!s_caption_source) {
		obs_data_t *settings = obs_data_create();
		if (!settings)
			return;
		obs_data_set_string(settings, "text", "");
		obs_data_set_bool(settings, "read_from_file", false);
		s_caption_source = obs_source_create("text_ft2_source", CAPTION_SOURCE_NAME, settings, NULL);
		obs_data_release(settings);
		if (!s_caption_source)
			return;
	}
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	obs_source_release(scene_source);
	if (!scene)
		return;
	/* If already in this scene, nothing to do */
	if (s_caption_item && obs_sceneitem_get_scene(s_caption_item) == scene) {
		obs_scene_release(scene);
		return;
	}
	/* Remove from previous scene if any */
	if (s_caption_item) {
		obs_sceneitem_remove(s_caption_item);
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}
	s_caption_item = obs_scene_add(scene, s_caption_source);
	obs_scene_release(scene);
#endif
}

void captions_service_detach_from_scene(void)
{
#if ENABLE_FRONTEND_API
	/* Release our reference to the source only. Do not remove/release the scene item:
	 * the collection is about to unload and will destroy the scene and its items.
	 * Touching the item (remove/release) during teardown can cause cleanup errors. */
	if (s_caption_source) {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
	}
	s_caption_item = NULL;
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
