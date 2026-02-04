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
#include <obs-defs.h>
#include <obs-source.h>
#include <obs-data.h>
#include <graphics/vec2.h>
#if ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTION_SOURCE_NAME "Transcription caption"
#define CAPTION_DISPLAY_BUF_SIZE 512
#define CAPTION_BOTTOM_MARGIN 24
#define CAPTION_SIDE_MARGIN 24
#define CAPTION_MAX_LINES 3
#define CAPTION_MIN_WIDTH 100
#define CAPTION_LINE_HEIGHT 1.2f
#define CAPTION_BASE_DIM 1000.0f
#define CAPTION_BASE_FONT 36
#define CAPTION_MIN_FONT 18
#define CAPTION_MAX_FONT 96
#define MAX_SUBTITLE_DURATION_MS 3000

#if ENABLE_FRONTEND_API
static obs_source_t *s_caption_source = NULL;
static obs_sceneitem_t *s_caption_item = NULL;
static uint64_t s_tick_count = 0;
static uint64_t s_last_segment_display_time_ms = 0; /* last time we showed non-empty text; for max subtitle duration */
static bool s_truncate_next_export = false;          /* for Phase 3 when captions owns SRT */
static uint32_t s_last_scene_w = 0;
static uint32_t s_last_scene_h = 0;
static bool s_layout_applied = false;
static float s_last_capture_width = 0.0f;
static float s_last_capture_center_x = 0.0f;
static float s_last_capture_bottom_y = 0.0f;
static bool s_shutting_down = false;

static void apply_caption_layout(void);
static void wrap_caption_text(const char *input, char *output, size_t output_size);
static float get_caption_box_height(uint32_t font_size);
static bool get_active_capture_bounds(float *center_x, float *bottom_y, float *width, float *height);
static uint32_t get_dynamic_font_size(uint32_t base_dim);

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
	if (s_shutting_down)
		return;
	if (!s_caption_source)
		return;
	apply_caption_layout();
	s_tick_count++;
	/* Update text every ~100 ms (approx 6 ticks at 60 fps) */
	if (s_tick_count % 6 != 0)
		return;
	uint64_t elapsed_ms = frontend_service_get_elapsed_ms();
	char buf[CAPTION_DISPLAY_BUF_SIZE];
	transcription_service_get_display_text(elapsed_ms, buf, sizeof(buf));
	/* Max subtitle duration: clear caption if no new segment for T ms (plan § 4.1.1) */
	if (buf[0] != '\0') {
		char wrapped[CAPTION_DISPLAY_BUF_SIZE];
		wrap_caption_text(buf, wrapped, sizeof(wrapped));
		s_last_segment_display_time_ms = elapsed_ms;
		set_caption_text(wrapped);
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
	obs_sceneitem_t *primary;
	obs_sceneitem_t *extras[8];
	size_t extras_count;
};

static bool is_caption_source_name(const char *name)
{
	if (!name)
		return false;
	size_t base_len = strlen(CAPTION_SOURCE_NAME);
	return strncmp(name, CAPTION_SOURCE_NAME, base_len) == 0;
}

static bool is_gdiplus_source_id(const char *id)
{
	if (!id)
		return false;
	return strcmp(id, "text_gdiplus_v3") == 0 || strcmp(id, "text_gdiplus_v2") == 0 ||
	       strcmp(id, "text_gdiplus") == 0;
}

static bool is_ft2_source_id(const char *id)
{
	if (!id)
		return false;
	return strcmp(id, "text_ft2_source_v2") == 0 || strcmp(id, "text_ft2_source") == 0;
}

static bool is_capture_source_id(const char *id)
{
	if (!id)
		return false;
	return strcmp(id, "window_capture") == 0 || strcmp(id, "monitor_capture") == 0 ||
	       strcmp(id, "game_capture") == 0 || strcmp(id, "dshow_input") == 0 ||
	       strcmp(id, "dshow_input_capture") == 0;
}

static bool collect_caption_items(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	const char *name = obs_source_get_name(src);
	if (!is_caption_source_name(name))
		return true;
	struct find_caption_param *p = (struct find_caption_param *)param;
	if (!p->primary) {
		p->primary = item;
		obs_sceneitem_addref(item);
	} else if (p->extras_count < (sizeof(p->extras) / sizeof(p->extras[0]))) {
		p->extras[p->extras_count++] = item;
		obs_sceneitem_addref(item);
	}
	return true;
}

static bool find_caption_source_by_prefix(void *param, obs_source_t *source)
{
	obs_source_t **out = (obs_source_t **)param;
	if (*out)
		return false;
	const char *name = obs_source_get_name(source);
	if (!is_caption_source_name(name))
		return true;
	*out = obs_source_get_ref(source);
	return false;
}

static uint32_t get_caption_font_size(obs_data_t *settings)
{
	uint32_t font_size = CAPTION_BASE_FONT;
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		int size = (int)obs_data_get_int(font_obj, "size");
		if (size > 0)
			font_size = (uint32_t)size;
		obs_data_release(font_obj);
	}
	return font_size;
}

static bool get_scene_dimensions(uint32_t *scene_w, uint32_t *scene_h)
{
	if (s_shutting_down)
		return false;
	uint32_t w = 0;
	uint32_t h = 0;
	if (s_caption_item) {
		obs_scene_t *scene = obs_sceneitem_get_scene(s_caption_item);
		if (scene) {
			obs_source_t *scene_source = obs_scene_get_source(scene);
			w = obs_source_get_width(scene_source);
			h = obs_source_get_height(scene_source);
		}
	}
	if (w == 0 || h == 0) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			w = ovi.base_width;
			h = ovi.base_height;
		}
	}
	if (w == 0 || h == 0)
		return false;
	*scene_w = w;
	*scene_h = h;
	return true;
}

struct capture_bounds_param {
	bool found;
	float center_x;
	float bottom_y;
	float width;
	float height;
};

static void get_sceneitem_rect(obs_sceneitem_t *item, float *left, float *top, float *width, float *height)
{
	if (s_shutting_down)
		return;
	struct vec2 pos;
	struct vec2 scale;
	struct vec2 bounds;
	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_scale(item, &scale);
	obs_sceneitem_get_bounds(item, &bounds);

	float w = 0.0f;
	float h = 0.0f;
	uint32_t bounds_type = obs_sceneitem_get_bounds_type(item);
	if (bounds_type != OBS_BOUNDS_NONE && bounds.x > 0.0f && bounds.y > 0.0f) {
		w = bounds.x;
		h = bounds.y;
	} else {
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (src) {
			w = (float)obs_source_get_width(src) * scale.x;
			h = (float)obs_source_get_height(src) * scale.y;
		}
	}

	uint32_t align = obs_sceneitem_get_alignment(item);
	float x = pos.x;
	float y = pos.y;
	if (align & OBS_ALIGN_RIGHT)
		x = pos.x - w;
	else if (!(align & OBS_ALIGN_LEFT))
		x = pos.x - (w / 2.0f);
	if (align & OBS_ALIGN_BOTTOM)
		y = pos.y - h;
	else if (!(align & OBS_ALIGN_TOP))
		y = pos.y - (h / 2.0f);

	*left = x;
	*top = y;
	*width = w;
	*height = h;
}

static bool find_active_capture_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	(void)scene;
	if (item == s_caption_item)
		return true;
	if (!obs_sceneitem_visible(item))
		return true;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	const char *id = obs_source_get_id(src);
	if (!is_capture_source_id(id))
		return true;

	struct capture_bounds_param *out = (struct capture_bounds_param *)param;
	float left = 0.0f;
	float top = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
	get_sceneitem_rect(item, &left, &top, &width, &height);
	if (width <= 0.0f || height <= 0.0f)
		return true;
	out->found = true;
	out->width = width;
	out->height = height;
	out->center_x = left + (width / 2.0f);
	out->bottom_y = top + height;
	return true;
}

static bool get_active_capture_bounds(float *center_x, float *bottom_y, float *width, float *height)
{
	if (s_shutting_down)
		return false;
	if (!s_caption_item)
		return false;
	obs_scene_t *scene = obs_sceneitem_get_scene(s_caption_item);
	if (!scene)
		return false;

	struct capture_bounds_param found = {0};
	obs_scene_enum_items(scene, find_active_capture_item, &found);
	if (!found.found)
		return false;
	*center_x = found.center_x;
	*bottom_y = found.bottom_y;
	*width = found.width;
	*height = found.height;
	return true;
}

static float get_caption_box_height(uint32_t font_size)
{
	if (font_size == 0)
		font_size = CAPTION_BASE_FONT;
	return (float)font_size * (float)CAPTION_MAX_LINES * CAPTION_LINE_HEIGHT;
}

static uint32_t get_dynamic_font_size(uint32_t base_dim)
{
	if (base_dim == 0)
		base_dim = (uint32_t)CAPTION_BASE_DIM;
	float scale = (float)base_dim / CAPTION_BASE_DIM;
	float size = (float)CAPTION_BASE_FONT * scale;
	if (size < CAPTION_MIN_FONT)
		size = CAPTION_MIN_FONT;
	if (size > CAPTION_MAX_FONT)
		size = CAPTION_MAX_FONT;
	return (uint32_t)(size + 0.5f);
}

static void set_caption_source_layout(uint32_t box_w, uint32_t box_h)
{
	if (!s_caption_source || box_w == 0)
		return;
	obs_data_t *settings = obs_source_get_settings(s_caption_source);
	if (!settings)
		return;
	int width = (int)box_w;
	if (width < CAPTION_MIN_WIDTH)
		width = CAPTION_MIN_WIDTH;
	uint32_t font_size = get_dynamic_font_size(box_h);
	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (!font_obj)
		font_obj = obs_data_create();
	if (font_obj) {
		const char *face = obs_data_get_string(font_obj, "face");
		const char *style = obs_data_get_string(font_obj, "style");
		if (!face || !face[0])
			obs_data_set_string(font_obj, "face", "Arial");
		if (!style || !style[0])
			obs_data_set_string(font_obj, "style", "Regular");
		obs_data_set_int(font_obj, "size", (int)font_size);
		obs_data_set_obj(settings, "font", font_obj);
		obs_data_release(font_obj);
	}
	uint32_t height = (uint32_t)get_caption_box_height(font_size);

	const char *source_id = obs_source_get_id(s_caption_source);
	if (is_gdiplus_source_id(source_id)) {
		obs_data_set_bool(settings, "word_wrap", true);
		obs_data_set_bool(settings, "extents", true);
		obs_data_set_bool(settings, "extents_wrap", true);
		obs_data_set_int(settings, "extents_cx", width);
		obs_data_set_int(settings, "extents_cy", (int)height);
	} else if (is_ft2_source_id(source_id)) {
		obs_data_set_bool(settings, "word_wrap", true);
		obs_data_set_int(settings, "custom_width", width);
	} else {
		obs_data_set_bool(settings, "word_wrap", true);
		obs_data_set_int(settings, "custom_width", width);
	}
	obs_data_set_string(settings, "align", "center");
	obs_data_set_string(settings, "valign", "bottom");

	obs_source_update(s_caption_source, settings);
	obs_data_release(settings);
}

static void apply_caption_layout(void)
{
	if (s_shutting_down)
		return;
	if (!s_caption_item || !s_caption_source)
		return;
	uint32_t scene_w = 0;
	uint32_t scene_h = 0;
	if (!get_scene_dimensions(&scene_w, &scene_h))
		return;

	float capture_center_x = 0.0f;
	float capture_bottom_y = 0.0f;
	float capture_w = 0.0f;
	float capture_h = 0.0f;
	bool has_capture = get_active_capture_bounds(&capture_center_x, &capture_bottom_y, &capture_w, &capture_h);

	float target_w = has_capture ? capture_w
				     : (float)((scene_w > (CAPTION_SIDE_MARGIN * 2U))
						       ? (scene_w - (CAPTION_SIDE_MARGIN * 2U))
						       : scene_w);
	float target_center_x = has_capture ? capture_center_x : ((float)scene_w) / 2.0f;
	float target_bottom_y = has_capture ? capture_bottom_y : (float)scene_h;
	uint32_t box_h_base = has_capture ? (uint32_t)capture_h : scene_h;

	if (s_layout_applied && s_last_scene_w == scene_w && s_last_scene_h == scene_h &&
	    s_last_capture_width == target_w && s_last_capture_center_x == target_center_x &&
	    s_last_capture_bottom_y == target_bottom_y) {
		return;
	}

	set_caption_source_layout((uint32_t)target_w, box_h_base);
	s_last_scene_w = scene_w;
	s_last_scene_h = scene_h;
	s_last_capture_width = target_w;
	s_last_capture_center_x = target_center_x;
	s_last_capture_bottom_y = target_bottom_y;

	float bounds_w = target_w;
	if (bounds_w < (float)CAPTION_MIN_WIDTH)
		bounds_w = (float)CAPTION_MIN_WIDTH;
	obs_data_t *settings = obs_source_get_settings(s_caption_source);
	uint32_t font_size = settings ? get_caption_font_size(settings) : CAPTION_BASE_FONT;
	if (settings)
		obs_data_release(settings);
	float bounds_h = get_caption_box_height(font_size);

	struct vec2 bounds;
	bounds.x = bounds_w;
	bounds.y = bounds_h;
	obs_sceneitem_set_bounds(s_caption_item, &bounds);
	obs_sceneitem_set_bounds_type(s_caption_item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(s_caption_item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_alignment(s_caption_item, OBS_ALIGN_CENTER);

	struct vec2 pos;
	pos.x = target_center_x;
	pos.y = target_bottom_y - ((bounds_h / 2.0f) + (float)CAPTION_BOTTOM_MARGIN);
	if (pos.y < 0.0f)
		pos.y = 0.0f;
	obs_sceneitem_set_pos(s_caption_item, &pos);

	s_layout_applied = true;
}

static void wrap_caption_text(const char *input, char *output, size_t output_size)
{
	if (!output || output_size == 0) {
		return;
	}
	output[0] = '\0';
	if (!input || !input[0]) {
		return;
	}

	uint32_t scene_w = 0;
	uint32_t scene_h = 0;
	if (!get_scene_dimensions(&scene_w, &scene_h)) {
		snprintf(output, output_size, "%s", input);
		return;
	}
	float capture_center_x = 0.0f;
	float capture_bottom_y = 0.0f;
	float capture_w = 0.0f;
	float capture_h = 0.0f;
	bool has_capture = get_active_capture_bounds(&capture_center_x, &capture_bottom_y, &capture_w, &capture_h);
	uint32_t base_w = has_capture ? (uint32_t)capture_w : scene_w;

	uint32_t font_size = CAPTION_BASE_FONT;
	if (s_caption_source) {
		obs_data_t *settings = obs_source_get_settings(s_caption_source);
		if (settings) {
			font_size = get_caption_font_size(settings);
			obs_data_release(settings);
		}
	}

	float char_width = (float)font_size * 0.6f;
	int max_chars =
		(int)((base_w - (CAPTION_SIDE_MARGIN * 2U)) / (char_width > 1.0f ? char_width : 1.0f));
	if (max_chars < 10)
		max_chars = 10;

	size_t out_len = 0;
	int line_len = 0;
	int line_count = 1;
	const char *p = input;
	while (*p) {
		while (isspace((unsigned char)*p) && *p != '\n')
			p++;
		if (*p == '\n') {
			if (line_count >= CAPTION_MAX_LINES)
				break;
			if (out_len + 1 < output_size) {
				output[out_len++] = '\n';
				output[out_len] = '\0';
			}
			line_len = 0;
			line_count++;
			p++;
			continue;
		}
		if (!*p)
			break;

		const char *word_start = p;
		size_t word_len = 0;
		while (*p && !isspace((unsigned char)*p)) {
			word_len++;
			p++;
		}

		int needed = (line_len == 0) ? (int)word_len : (int)word_len + 1;
		if (line_len + needed > max_chars) {
			if (line_count >= CAPTION_MAX_LINES)
				break;
			if (out_len + 1 < output_size) {
				output[out_len++] = '\n';
				output[out_len] = '\0';
			}
			line_len = 0;
			line_count++;
		}

		if (line_len != 0) {
			if (out_len + 1 < output_size) {
				output[out_len++] = ' ';
				output[out_len] = '\0';
				line_len += 1;
			} else {
				break;
			}
		}

		size_t copy_len = word_len;
		if (out_len + copy_len >= output_size)
			copy_len = output_size - out_len - 1;
		if (copy_len == 0)
			break;
		memcpy(output + out_len, word_start, copy_len);
		out_len += copy_len;
		output[out_len] = '\0';
		line_len += (int)copy_len;
	}
}

static obs_source_t *create_caption_source(obs_data_t *settings)
{
	obs_source_t *source = NULL;
#ifdef _WIN32
	source = obs_source_create("text_gdiplus_v3", CAPTION_SOURCE_NAME, settings, NULL);
	if (!source)
		source = obs_source_create("text_gdiplus_v2", CAPTION_SOURCE_NAME, settings, NULL);
	if (!source)
		source = obs_source_create("text_gdiplus", CAPTION_SOURCE_NAME, settings, NULL);
#endif
	if (!source)
		source = obs_source_create("text_ft2_source_v2", CAPTION_SOURCE_NAME, settings, NULL);
	if (!source)
		source = obs_source_create("text_ft2_source", CAPTION_SOURCE_NAME, settings, NULL);
	return source;
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
	if (s_shutting_down)
		return;
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	if (!scene_source)
		return;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene) {
		obs_source_release(scene_source);
		return;
	}

	/* Look for existing "Transcription caption" in this scene (avoids duplicate + crash) */
	struct find_caption_param find = {0};
	obs_scene_enum_items(scene, collect_caption_items, &find);

	if (find.primary) {
		for (size_t i = 0; i < find.extras_count; i++) {
			if (find.extras[i]) {
				obs_sceneitem_remove(find.extras[i]);
				obs_sceneitem_release(find.extras[i]);
			}
		}
		if (s_caption_item) {
			obs_sceneitem_release(s_caption_item);
			s_caption_item = NULL;
		}
		if (s_caption_source) {
			obs_source_release(s_caption_source);
			s_caption_source = NULL;
		}
		s_caption_item = find.primary;
		obs_source_t *item_source = obs_sceneitem_get_source(s_caption_item);
		s_caption_source = item_source ? obs_source_get_ref(item_source) : NULL;
		s_layout_applied = false;
		obs_scene_release(scene);
		obs_source_release(scene_source);
		return;
	}

	/* Not in scene: release any stale refs (do NOT call obs_sceneitem_remove - can crash) */
	if (s_caption_item) {
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}

	if (!s_caption_source) {
		obs_source_t *existing = obs_get_source_by_name(CAPTION_SOURCE_NAME);
		if (!existing)
			obs_enum_sources(find_caption_source_by_prefix, &existing);
		if (existing)
			s_caption_source = existing;
	}

	/* Create source if we don't have one */
	if (!s_caption_source) {
		obs_data_t *settings = obs_data_create();
		if (!settings) {
			obs_scene_release(scene);
			obs_source_release(scene_source);
			return;
		}
		obs_data_set_string(settings, "text", "");
		obs_data_set_bool(settings, "read_from_file", false);
		/* Default appearance */
		obs_data_set_int(settings, "bk_color", 4278190080); /* 0xFF000000 */
		obs_data_set_int(settings, "bk_opacity", 30);
		obs_data_t *font = obs_data_create();
		if (font) {
			obs_data_set_string(font, "face", "Arial");
			obs_data_set_string(font, "style", "Regular");
			obs_data_set_int(font, "size", 36);
			obs_data_set_int(font, "flags", 0);
			obs_data_set_obj(settings, "font", font);
			obs_data_release(font);
		}
		s_caption_source = create_caption_source(settings);
		obs_data_release(settings);
		if (!s_caption_source) {
			obs_scene_release(scene);
			obs_source_release(scene_source);
			return;
		}
	}

	s_caption_item = obs_scene_add(scene, s_caption_source);
	obs_scene_release(scene);
	obs_source_release(scene_source);
	s_layout_applied = false;
	apply_caption_layout();
#endif
}

void captions_service_detach_from_scene(void)
{
#if ENABLE_FRONTEND_API
	if (s_shutting_down)
		return;
	/* Release our refs. Do not remove scene item during teardown (causes cleanup errors). */
	if (s_caption_item) {
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}
	if (s_caption_source) {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
	}
	s_layout_applied = false;
	s_last_scene_w = 0;
	s_last_scene_h = 0;
	s_last_capture_width = 0.0f;
	s_last_capture_center_x = 0.0f;
	s_last_capture_bottom_y = 0.0f;
#endif
}

#if ENABLE_FRONTEND_API
static void remove_caption_and_release_refs(void)
{
	if (s_shutting_down) {
		s_caption_item = NULL;
		s_caption_source = NULL;
		s_layout_applied = false;
		s_last_scene_w = 0;
		s_last_scene_h = 0;
		s_last_capture_width = 0.0f;
		s_last_capture_center_x = 0.0f;
		s_last_capture_bottom_y = 0.0f;
		return;
	}
	if (s_caption_item) {
		/* Do not remove during shutdown; OBS may already be tearing down scenes. */
		obs_sceneitem_release(s_caption_item);
		s_caption_item = NULL;
	}
	if (s_caption_source) {
		obs_source_release(s_caption_source);
		s_caption_source = NULL;
	}
	s_layout_applied = false;
	s_last_scene_w = 0;
	s_last_scene_h = 0;
	s_last_capture_width = 0.0f;
	s_last_capture_center_x = 0.0f;
	s_last_capture_bottom_y = 0.0f;
}
#endif

void captions_service_on_exit(void)
{
#if ENABLE_FRONTEND_API
	s_shutting_down = true;
	obs_remove_tick_callback(on_tick, NULL);
	/* Avoid touching scene objects during OBS teardown to prevent crashes. */
	remove_caption_and_release_refs();
#endif
}

void captions_service_unload(void)
{
#if ENABLE_FRONTEND_API
	s_shutting_down = true;
	obs_remove_tick_callback(on_tick, NULL);
	/* Cleanup normally done in captions_service_on_exit (OBS_FRONTEND_EVENT_EXIT); fallback if EXIT not fired. */
	remove_caption_and_release_refs();
#endif
}
