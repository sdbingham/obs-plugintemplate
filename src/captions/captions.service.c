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
#include <graphics/vec2.h>
#if ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#endif
#include <stdlib.h>
#include <string.h>

#define CAPTION_SOURCE_NAME "Transcription caption"
#define CAPTION_DISPLAY_BUF_SIZE 512
#define CAPTION_BOTTOM_MARGIN 24
#define MAX_SUBTITLE_DURATION_MS 3000

#if ENABLE_FRONTEND_API
static obs_source_t *s_caption_source = NULL;
static obs_sceneitem_t *s_caption_item = NULL;
static bool s_we_own_caption_source = false; /* true only when we created it; false when reused from scene */
static uint64_t s_tick_count = 0;
static uint64_t s_last_segment_display_time_ms = 0; /* last time we showed non-empty text; for max subtitle duration */
static bool s_truncate_next_export = false;          /* for Phase 3 when captions owns SRT */

static void set_caption_text(const char *text)
{
	if (!s_caption_source)
		return;
	obs_data_t *settings = obs_source_get_settings(s_caption_source);
	if (settings) {
		obs_data_set_string(settings, "text", text ? text : "");
		/* Force "text" mode so our updates are shown (reused source may have read_from_file true). */
		obs_data_set_bool(settings, "read_from_file", false);
		obs_source_update(s_caption_source, settings);
		obs_data_release(settings);
	}
}

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
	/* Max subtitle duration: clear caption if no new segment for T ms (plan § 4.1.1) */
	if (buf[0] != '\0') {
		s_last_segment_display_time_ms = elapsed_ms;
		set_caption_text(buf);
	} else {
		if (s_last_segment_display_time_ms != 0 &&
		    (elapsed_ms - s_last_segment_display_time_ms) > MAX_SUBTITLE_DURATION_MS) {
			set_caption_text("");
		} else if (s_last_segment_display_time_ms == 0) {
			set_caption_text(""); /* after reset, show empty until first segment */
		}
		/* Else: within window, keep previous text (do not update) */
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

#if ENABLE_FRONTEND_API
struct find_caption_param {
	obs_sceneitem_t *item;
	obs_source_t *source;
};

static bool find_caption_by_name(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	if (strcmp(obs_source_get_name(src), CAPTION_SOURCE_NAME) != 0)
		return true;
	struct find_caption_param *p = (struct find_caption_param *)param;
	p->item = item;
	p->source = src;
	return false; /* stop enum */
}

static void set_caption_position_bottom_center(void)
{
	if (!s_caption_item || !s_caption_source)
		return;
	obs_scene_t *scene = obs_sceneitem_get_scene(s_caption_item);
	if (!scene)
		return;
	obs_source_t *scene_source = obs_scene_get_source(scene);
	uint32_t scene_w = obs_source_get_width(scene_source);
	uint32_t scene_h = obs_source_get_height(scene_source);
	uint32_t cap_w = obs_source_get_width(s_caption_source);
	uint32_t cap_h = obs_source_get_height(s_caption_source);
	if (scene_w == 0 || scene_h == 0)
		return;
	struct vec2 pos;
	pos.x = (float)((int)scene_w - (int)cap_w) / 2.0f;
	if (pos.x < 0.0f)
		pos.x = 0.0f;
	pos.y = (float)((int)scene_h - (int)cap_h - CAPTION_BOTTOM_MARGIN);
	if (pos.y < 0.0f)
		pos.y = 0.0f;
	obs_sceneitem_set_pos(s_caption_item, &pos);
}
#endif

void captions_service_reset_for_recording(bool truncate_file)
{
#if ENABLE_FRONTEND_API
	s_last_segment_display_time_ms = 0;
	s_truncate_next_export = truncate_file;
	set_caption_text("");
#endif
	(void)truncate_file; /* used when captions owns SRT (Phase 3) */
}

void captions_service_attach_to_current_scene(void)
{
#if ENABLE_FRONTEND_API
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene) {
		obs_source_release(scene_source);
		return;
	}

	/* Look for existing "Transcription caption" in this scene (avoids duplicate + crash) */
	struct find_caption_param find = {NULL, NULL};
	obs_scene_enum_items(scene, find_caption_by_name, &find);

	if (find.source && find.item) {
		/* Reuse existing caption source in this scene (no duplicate, no remove = no crash) */
		if (s_caption_item)
			obs_sceneitem_release(s_caption_item);
		if (s_caption_source && s_we_own_caption_source)
			obs_source_release(s_caption_source);
		obs_sceneitem_addref(find.item);
		s_caption_item = find.item;
		s_caption_source = obs_sceneitem_get_source(s_caption_item); /* item holds source ref */
		s_we_own_caption_source = false;
		obs_scene_release(scene);
		set_caption_position_bottom_center();
		return;
	}

	/* Not in scene: release any stale refs (do NOT call obs_sceneitem_remove - can crash) */
	if (s_caption_item) {
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}

	/* Create source if we don't have one */
	if (!s_caption_source) {
		obs_data_t *settings = obs_data_create();
		if (!settings) {
			obs_scene_release(scene);
			return;
		}
		obs_data_set_string(settings, "text", "");
		obs_data_set_bool(settings, "read_from_file", false);
		s_caption_source = obs_source_create("text_ft2_source", CAPTION_SOURCE_NAME, settings, NULL);
		obs_data_release(settings);
		if (!s_caption_source) {
			obs_scene_release(scene);
			return;
		}
		s_we_own_caption_source = true;
	}

	s_caption_item = obs_scene_add(scene, s_caption_source);
	obs_scene_release(scene);
	set_caption_position_bottom_center();
#endif
}

void captions_service_detach_from_scene(void)
{
#if ENABLE_FRONTEND_API
	/* Release our refs. Do not remove scene item during teardown (causes cleanup errors). */
	if (s_caption_item) {
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}
	if (s_caption_source && s_we_own_caption_source) {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
		s_we_own_caption_source = false;
	} else {
		s_caption_source = NULL;
	}
#endif
}

#if ENABLE_FRONTEND_API
static void remove_caption_and_release_refs(void)
{
	if (s_caption_item) {
		obs_scene_t *scene = obs_sceneitem_get_scene(s_caption_item);
		if (scene)
			obs_sceneitem_remove(s_caption_item);
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}
	if (s_caption_source && s_we_own_caption_source) {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
		s_we_own_caption_source = false;
	} else {
		s_caption_source = NULL;
	}
}
#endif

void captions_service_on_exit(void)
{
#if ENABLE_FRONTEND_API
	/* Remove caption from scene and release refs on exit so OBS does not defer-destroy
	 * our source later (obs_source_destroy_defer can crash when run after plugin unload). */
	remove_caption_and_release_refs();
#endif
}

void captions_service_unload(void)
{
#if ENABLE_FRONTEND_API
	obs_remove_tick_callback(on_tick, NULL);
	/* Cleanup normally done in captions_service_on_exit (OBS_FRONTEND_EVENT_EXIT); fallback if EXIT not fired. */
	remove_caption_and_release_refs();
#endif
}
