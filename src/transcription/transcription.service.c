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
#include "transcription/transcription.service.h"
#include <obs.h>
#include <obs-module.h>
#include <util/threading.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#if ENABLE_WHISPER
#include "transcription/whisper_wrapper.h"
#endif

static char last_nonspace_char(const char *text);
static bool starts_with_lowercase(const char *text);
static bool is_sentence_terminator(char c);
static size_t word_count_limited(const char *text, size_t max_words);
static void append_segment_text(char *dst, size_t dst_size, const char *src);

#define CAPTION_SEGMENT_MAX 128
#define CAPTION_TEXT_MAX 256
#define STUB_SEGMENT_DURATION_MS 2000
#define DEFAULT_MODEL_FILE "ggml-tiny.en.bin"
#define DEFAULT_MODEL_SUBPATH "models/" DEFAULT_MODEL_FILE
#define CHUNK_QUEUE_MAX 120
#define CONFIG_SECTION "Transcription"
#define CONFIG_KEY_SPEECH_CONFIDENCE_MIN "SpeechConfidenceMin"
#define CONFIG_KEY_FILTER_PHRASES "FilterPhrases"
#define CONFIG_KEY_REPLACE_PHRASES "ReplacePhrases"
#define CONFIG_KEY_MODEL_FILE "ModelFile"
#define CONFIG_KEY_LANGUAGE "Language"
#define CONFIG_KEY_INITIAL_PROMPT "InitialPrompt"
#define DEFAULT_SPEECH_CONFIDENCE_MIN 0.4f
#define FILTER_REPLACE_BUF_SIZE (CAPTION_TEXT_MAX * 2)
#define SRT_TEXT_MAX 512
#define SRT_MERGE_MAX_GAP_MS 500
#define SRT_MERGE_MAX_DURATION_MS 8000
#define SRT_MERGE_MAX_CHARS 240
#define SRT_MIN_SEGMENT_CHARS 12
#define SRT_MIN_SEGMENT_MS 900
#define DISPLAY_MERGE_MAX_GAP_MS 700
#define DISPLAY_PENDING_MIN_CHARS 18
#define DISPLAY_PENDING_MIN_WORDS 3
#define DISPLAY_PENDING_MAX_MS 1800
#define DISPLAY_HISTORY_GAP_MS 700
#define DISPLAY_HISTORY_MAX_SEGMENTS 4

static char *s_filter_phrases = NULL;   /* semicolon-separated phrases to remove (literal) */
static char *s_replace_phrases = NULL;  /* semicolon-separated "from|to" pairs (literal) */
static char *s_model_file = NULL;       /* model file name (e.g. ggml-tiny.en.bin) */
static char *s_language = NULL;         /* language code (e.g. "en") or "auto" */
static char *s_initial_prompt = NULL;   /* optional initial prompt */

/* Skip empty or whitespace-only segments (plan § 5.8: skip empty after filter) */
static bool is_empty_or_whitespace(const char *text)
{
	if (!text)
		return true;
	while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
		text++;
	return *text == '\0';
}

/* Trim leading/trailing whitespace in-place; return pointer to start of content. */
static void trim_in_place(char *text)
{
	if (!text)
		return;
	size_t len = strlen(text);
	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n' || text[len - 1] == '\r'))
		text[--len] = '\0';
	char *p = text;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (p != text && *p)
		memmove(text, p, strlen(p) + 1);
}

/* Lowercase one char for substring check. */
static char to_lower(char c)
{
	if (c >= 'A' && c <= 'Z')
		return (char)(c + ('a' - 'A'));
	return c;
}

static bool is_repetitive_phrase(const char *text)
{
	if (!text)
		return false;
	char buf[CAPTION_TEXT_MAX];
	size_t len = strlen(text);
	if (len >= CAPTION_TEXT_MAX)
		len = CAPTION_TEXT_MAX - 1;
	memcpy(buf, text, len + 1);
	for (size_t i = 0; buf[i]; i++) {
		char c = buf[i];
		if (c >= 'A' && c <= 'Z')
			buf[i] = (char)(c + ('a' - 'A'));
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			continue;
		else
			buf[i] = ' ';
	}
	const char *words[64];
	size_t counts[64];
	size_t unique = 0;
	size_t total = 0;
	char *tok = strtok(buf, " \t\r\n");
	while (tok && total < 64) {
		if (tok[0]) {
			total++;
			size_t i = 0;
			for (; i < unique; i++) {
				if (strcmp(words[i], tok) == 0) {
					counts[i]++;
					break;
				}
			}
			if (i == unique) {
				words[unique] = tok;
				counts[unique] = 1;
				unique++;
			}
		}
		tok = strtok(NULL, " \t\r\n");
	}
	if (total < 4)
		return false;
	size_t max_count = 0;
	for (size_t i = 0; i < unique; i++) {
		if (counts[i] > max_count)
			max_count = counts[i];
	}
	if (unique <= 1)
		return true;
	if (max_count * 100 / total >= 80)
		return true;
	return false;
}

static bool is_low_diversity_noise(const char *text)
{
	if (!text)
		return false;
	char buf[CAPTION_TEXT_MAX];
	size_t len = strlen(text);
	if (len >= CAPTION_TEXT_MAX)
		len = CAPTION_TEXT_MAX - 1;
	memcpy(buf, text, len + 1);
	for (size_t i = 0; buf[i]; i++) {
		char c = buf[i];
		if (c >= 'A' && c <= 'Z')
			buf[i] = (char)(c + ('a' - 'A'));
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			continue;
		else
			buf[i] = ' ';
	}
	const char *words[64];
	size_t counts[64];
	size_t unique = 0;
	size_t total = 0;
	char *tok = strtok(buf, " \t\r\n");
	while (tok && total < 64) {
		if (tok[0]) {
			total++;
			size_t i = 0;
			for (; i < unique; i++) {
				if (strcmp(words[i], tok) == 0) {
					counts[i]++;
					break;
				}
			}
			if (i == unique) {
				words[unique] = tok;
				counts[unique] = 1;
				unique++;
			}
		}
		tok = strtok(NULL, " \t\r\n");
	}
	if (total < 6)
		return false;
	size_t max_count = 0;
	for (size_t i = 0; i < unique; i++) {
		if (counts[i] > max_count)
			max_count = counts[i];
	}
	if (unique * 100 / total < 34)
		return true;
	if (max_count * 100 / total >= 75)
		return true;
	return false;
}

static void strip_trailing_punct(char *s)
{
	if (!s)
		return;
	size_t len = strlen(s);
	while (len > 0) {
		char c = s[len - 1];
		if (c == '.' || c == ',' || c == '!' || c == '?' || c == ':' || c == ';')
			s[--len] = '\0';
		else
			break;
	}
}

/* Replace all occurrences of 'from' with 'to' in src, write result to dst. Literal only. */
static void replace_all_to_dst(char *dst, size_t dst_size, const char *src, const char *from, const char *to)
{
	if (!dst || dst_size == 0 || !src)
		return;
	dst[0] = '\0';
	if (!from)
		from = "";
	if (!to)
		to = "";
	size_t from_len = strlen(from);
	const char *p = src;
	size_t out = 0;
	while (*p && out < dst_size - 1) {
		if (from_len > 0 && strncmp(p, from, from_len) == 0) {
			size_t to_len = strlen(to);
			if (out + to_len < dst_size) {
				memcpy(dst + out, to, to_len + 1);
				out += to_len;
			}
			p += from_len;
		} else {
			dst[out++] = *p++;
			dst[out] = '\0';
		}
	}
	dst[out] = '\0';
}

/* Apply user filter (remove) and replace rules to text in place. Format: FilterPhrases "a;b;c", ReplacePhrases "x|y;u|v". */
static void apply_user_filter_replace(char *text, size_t buf_size)
{
	if (!text || buf_size == 0)
		return;
	if ((!s_filter_phrases || !s_filter_phrases[0]) && (!s_replace_phrases || !s_replace_phrases[0]))
		return;
	static char work[FILTER_REPLACE_BUF_SIZE];
	static char work2[FILTER_REPLACE_BUF_SIZE];
	size_t work_size = sizeof(work);
	strncpy(work, text, work_size - 1);
	work[work_size - 1] = '\0';
	/* Apply replace rules (from|to; from|to) */
	if (s_replace_phrases && s_replace_phrases[0]) {
		char *str = bstrdup(s_replace_phrases);
		if (str) {
			for (char *tok = strtok(str, ";"); tok; tok = strtok(NULL, ";")) {
				char *pipe = strchr(tok, '|');
				if (pipe) {
					*pipe = '\0';
					replace_all_to_dst(work2, sizeof(work2), work, tok, pipe + 1);
					strncpy(work, work2, work_size - 1);
					work[work_size - 1] = '\0';
				}
			}
			bfree(str);
		}
	}
	/* Apply filter phrases (remove; literal replace with "") */
	if (s_filter_phrases && s_filter_phrases[0]) {
		char *str = bstrdup(s_filter_phrases);
		if (str) {
			for (char *tok = strtok(str, ";"); tok; tok = strtok(NULL, ";")) {
				replace_all_to_dst(work2, sizeof(work2), work, tok, "");
				strncpy(work, work2, work_size - 1);
				work[work_size - 1] = '\0';
			}
			bfree(str);
		}
	}
	strncpy(text, work, buf_size - 1);
	text[buf_size - 1] = '\0';
	trim_in_place(text);
}

/* Known Whisper hallucinations (BoH-style): drop these so real speech is not drowned out. */
static bool is_known_hallucination(const char *text)
{
	if (!text)
		return false;
	if (is_repetitive_phrase(text))
		return true;
	if (is_low_diversity_noise(text))
		return true;
	char buf[CAPTION_TEXT_MAX];
	size_t len = strlen(text);
	if (len >= CAPTION_TEXT_MAX)
		len = CAPTION_TEXT_MAX - 1;
	memcpy(buf, text, len + 1);
	trim_in_place(buf);
	if (buf[0] == '\0')
		return false;
	for (size_t i = 0; buf[i]; i++)
		buf[i] = to_lower(buf[i]);
	{
		/* Drop repeated single-word spam like "you you you". */
		char tmp[CAPTION_TEXT_MAX];
		strncpy(tmp, buf, sizeof(tmp) - 1);
		tmp[sizeof(tmp) - 1] = '\0';
		const char *first = NULL;
		char *tok = strtok(tmp, " \t\r\n");
		size_t count = 0;
		bool all_same = true;
		while (tok) {
			strip_trailing_punct(tok);
			if (tok[0]) {
				if (!first)
					first = tok;
				else if (strcmp(first, tok) != 0)
					all_same = false;
				count++;
			}
			tok = strtok(NULL, " \t\r\n");
		}
		if (count >= 3 && all_same)
			return true;
	}
	/* Exact or contains known hallucination phrases */
	if (strcmp(buf, "the end") == 0)
		return true;
	if (strstr(buf, "thanks for watching") != NULL)
		return true;
	if (strstr(buf, "thank you for watching") != NULL)
		return true;
	if (strstr(buf, "www.") != NULL)
		return true;
	if (strstr(buf, "beadaholique") != NULL)
		return true;
	/* Music/silence junk: ♪ (UTF-8 E2 99 AA), "pfft", "music" as single-word filler */
	if (strstr(text, "\xE2\x99\xAA") != NULL)
		return true;
	if (strstr(buf, "pfft") != NULL)
		return true;
	if (strcmp(buf, "music") == 0)
		return true;
	return false;
}

typedef struct {
	char text[CAPTION_TEXT_MAX];
	uint64_t start_ms;
	uint64_t end_ms;
} caption_segment_t;

typedef struct {
	char text[SRT_TEXT_MAX];
	uint64_t start_ms;
	uint64_t end_ms;
} srt_segment_t;

static caption_segment_t s_caption_segments[CAPTION_SEGMENT_MAX];
static uint32_t s_caption_count = 0;
static pthread_mutex_t s_caption_mutex;
static caption_segment_t s_pending_segment = {0};
static bool s_pending_active = false;
static pthread_mutex_t s_pending_mutex;

typedef struct {
	float *samples;
	uint32_t num_samples;
	uint64_t start_ts_ms;
} audio_chunk_t;

static audio_chunk_t s_chunk_queue[CHUNK_QUEUE_MAX];
static uint32_t s_queue_head = 0;
static uint32_t s_queue_count = 0;
static bool s_worker_running = false;
static bool s_worker_busy = false;
static pthread_t s_worker_thread;
static pthread_mutex_t s_input_mutex;
static pthread_cond_t s_input_cond;
#if ENABLE_WHISPER
static pthread_mutex_t s_whisper_mutex;
#endif
#if ENABLE_WHISPER
static void *s_whisper_ctx = NULL; /* whisper_wrapper_ctx_t */
#endif
static float s_speech_confidence_min = DEFAULT_SPEECH_CONFIDENCE_MIN; /* 0.0–1.0; no_speech threshold = 1 - this */
static uint64_t s_last_drop_log_ns = 0;

#if ENABLE_WHISPER
static char *build_model_path(const char *model_file)
{
	if (!model_file || !model_file[0])
		model_file = DEFAULT_MODEL_FILE;
	char subpath[256];
	int written = snprintf(subpath, sizeof(subpath), "models/%s", model_file);
	if (written <= 0 || (size_t)written >= sizeof(subpath))
		return NULL;
	return obs_module_config_path(subpath);
}

static void transcription_reload_model_locked(void)
{
	char *model_path = build_model_path(s_model_file);
	if (!model_path) {
		obs_log(LOG_WARNING, "transcription: failed to build model path");
		return;
	}
	FILE *f = fopen(model_path, "rb");
	if (!f) {
		obs_log(LOG_WARNING, "transcription: model file not found: %s", model_path);
		bfree(model_path);
		return;
	}
	fclose(f);
	if (s_whisper_ctx) {
		whisper_wrapper_free(s_whisper_ctx);
		s_whisper_ctx = NULL;
	}
	s_whisper_ctx = whisper_wrapper_init(model_path, 1);
	if (!s_whisper_ctx) {
		s_whisper_ctx = whisper_wrapper_init(model_path, 0);
		if (s_whisper_ctx)
			obs_log(LOG_INFO, "transcription: Whisper loaded (CPU) from %s", model_path);
		else
			obs_log(LOG_ERROR, "transcription: failed to load Whisper model: %s", model_path);
	} else {
		obs_log(LOG_INFO, "transcription: Whisper loaded from %s", model_path);
	}
	bfree(model_path);
}
#endif

static void push_caption_segment(const char *text, uint64_t start_ms, uint64_t end_ms)
{
	if (is_empty_or_whitespace(text))
		return;
	pthread_mutex_lock(&s_caption_mutex);
	if (s_caption_count >= CAPTION_SEGMENT_MAX) {
		/* Drop oldest */
		memmove(&s_caption_segments[0], &s_caption_segments[1], (s_caption_count - 1) * sizeof(caption_segment_t));
		s_caption_count--;
	}
	caption_segment_t *seg = &s_caption_segments[s_caption_count];
	size_t len = strlen(text);
	if (len >= CAPTION_TEXT_MAX)
		len = CAPTION_TEXT_MAX - 1;
	memcpy(seg->text, text, len);
	seg->text[len] = '\0';
	seg->start_ms = start_ms;
	seg->end_ms = end_ms;
	s_caption_count++;
	pthread_mutex_unlock(&s_caption_mutex);
}

static bool should_merge_display(const caption_segment_t *prev, const caption_segment_t *cur)
{
	if (!prev || !cur)
		return false;
	if (cur->start_ms < prev->end_ms) {
		/* Overlap: treat as no gap. */
	} else if (cur->start_ms - prev->end_ms > DISPLAY_MERGE_MAX_GAP_MS) {
		return false;
	}
	char last = last_nonspace_char(prev->text);
	bool prev_ends_sentence = is_sentence_terminator(last);
	bool next_starts_lower = starts_with_lowercase(cur->text);
	size_t prev_len = strlen(prev->text);
	if (!prev_ends_sentence)
		return true;
	if (next_starts_lower)
		return true;
	if (prev_len < DISPLAY_PENDING_MIN_CHARS)
		return true;
	if (word_count_limited(prev->text, DISPLAY_PENDING_MIN_WORDS) < DISPLAY_PENDING_MIN_WORDS)
		return true;
	return false;
}

static bool should_commit_pending(const caption_segment_t *seg)
{
	if (!seg)
		return false;
	size_t len = strlen(seg->text);
	if (len >= DISPLAY_PENDING_MIN_CHARS)
		return true;
	if (word_count_limited(seg->text, DISPLAY_PENDING_MIN_WORDS) >= DISPLAY_PENDING_MIN_WORDS)
		return true;
	char last = last_nonspace_char(seg->text);
	if (is_sentence_terminator(last))
		return true;
	uint64_t dur = seg->end_ms - seg->start_ms;
	if (dur >= DISPLAY_PENDING_MAX_MS)
		return true;
	return false;
}

static void commit_pending_if_needed(bool force)
{
	pthread_mutex_lock(&s_pending_mutex);
	if (!s_pending_active)
		goto done;
	if (!force && !should_commit_pending(&s_pending_segment))
		goto done;
	push_caption_segment(s_pending_segment.text, s_pending_segment.start_ms,
			     s_pending_segment.end_ms);
	memset(&s_pending_segment, 0, sizeof(s_pending_segment));
	s_pending_active = false;
done:
	pthread_mutex_unlock(&s_pending_mutex);
}

static void process_segment_for_display(const char *text, uint64_t start_ms, uint64_t end_ms)
{
	caption_segment_t cur = {0};
	size_t len = strlen(text);
	if (len >= CAPTION_TEXT_MAX)
		len = CAPTION_TEXT_MAX - 1;
	memcpy(cur.text, text, len);
	cur.text[len] = '\0';
	cur.start_ms = start_ms;
	cur.end_ms = end_ms;

	pthread_mutex_lock(&s_pending_mutex);
	if (!s_pending_active) {
		s_pending_segment = cur;
		s_pending_active = true;
		pthread_mutex_unlock(&s_pending_mutex);
		commit_pending_if_needed(false);
		return;
	}

	if (should_merge_display(&s_pending_segment, &cur)) {
		append_segment_text(s_pending_segment.text, sizeof(s_pending_segment.text),
				    cur.text);
		if (cur.end_ms > s_pending_segment.end_ms)
			s_pending_segment.end_ms = cur.end_ms;
		pthread_mutex_unlock(&s_pending_mutex);
		commit_pending_if_needed(false);
		return;
	}

	pthread_mutex_unlock(&s_pending_mutex);
	commit_pending_if_needed(true);
	pthread_mutex_lock(&s_pending_mutex);
	s_pending_segment = cur;
	s_pending_active = true;
	pthread_mutex_unlock(&s_pending_mutex);
	commit_pending_if_needed(false);
}

static void *worker_thread(void *arg)
{
	(void)arg;
	while (s_worker_running) {
		pthread_mutex_lock(&s_input_mutex);
		while (s_worker_running && s_queue_count == 0)
			pthread_cond_wait(&s_input_cond, &s_input_mutex);
		audio_chunk_t chunk = {NULL, 0, 0};
		if (s_queue_count > 0) {
			uint32_t idx = s_queue_head;
			chunk = s_chunk_queue[idx];
			s_chunk_queue[idx].samples = NULL;
			s_queue_head = (s_queue_head + 1) % CHUNK_QUEUE_MAX;
			s_queue_count--;
			s_worker_busy = true;
		}
		pthread_mutex_unlock(&s_input_mutex);

		if (chunk.samples == NULL)
			continue;

#if ENABLE_WHISPER
		if (s_whisper_ctx) {
			const char *lang = transcription_service_get_language();
			const char *prompt = transcription_service_get_initial_prompt();
			pthread_mutex_lock(&s_whisper_mutex);
			if (s_whisper_ctx) {
				int ret = whisper_wrapper_run(s_whisper_ctx, chunk.samples, chunk.num_samples, chunk.start_ts_ms,
							      lang, prompt);
				if (ret == 0) {
					int n = whisper_wrapper_get_segment_count(s_whisper_ctx);
					for (int i = 0; i < n; i++) {
						float no_speech = whisper_wrapper_get_segment_no_speech_prob(s_whisper_ctx, i);
						float speech_conf = 1.0f - no_speech;
						/* Skip low-confidence speech. */
						if (speech_conf < s_speech_confidence_min)
							continue;
						char seg_text[CAPTION_TEXT_MAX];
						uint64_t t0_ms, t1_ms;
		whisper_wrapper_get_segment(s_whisper_ctx, i, seg_text, sizeof(seg_text), &t0_ms, &t1_ms);
		if (is_empty_or_whitespace(seg_text))
			continue;
		if (is_known_hallucination(seg_text))
			continue; /* BoH-style: drop known hallucination phrases so real speech shows. */
		apply_user_filter_replace(seg_text, sizeof(seg_text));
		if (is_empty_or_whitespace(seg_text))
			continue;
		if (is_repetitive_phrase(seg_text))
			continue;
		if (is_low_diversity_noise(seg_text))
			continue;
		process_segment_for_display(seg_text, t0_ms, t1_ms);
	}
				}
			}
			pthread_mutex_unlock(&s_whisper_mutex);
		} else {
			uint64_t end_ms = chunk.start_ts_ms + STUB_SEGMENT_DURATION_MS;
			push_caption_segment("[Phase 2 — transcription stub]", chunk.start_ts_ms, end_ms);
		}
#else
		uint64_t end_ms = chunk.start_ts_ms + STUB_SEGMENT_DURATION_MS;
		push_caption_segment("[Phase 2 — transcription stub]", chunk.start_ts_ms, end_ms);
#endif

		bfree(chunk.samples);
		pthread_mutex_lock(&s_input_mutex);
		s_worker_busy = false;
		pthread_mutex_unlock(&s_input_mutex);
	}
	return NULL;
}

static void save_transcription_config(void)
{
	char *path = obs_module_config_path("config.ini");
	if (!path)
		return;
	config_t *config = NULL;
	if (config_open(&config, path, CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS) {
		bfree(path);
		return;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", (double)s_speech_confidence_min);
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_SPEECH_CONFIDENCE_MIN, buf);
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_FILTER_PHRASES, s_filter_phrases ? s_filter_phrases : "");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_REPLACE_PHRASES, s_replace_phrases ? s_replace_phrases : "");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_MODEL_FILE, s_model_file ? s_model_file : "");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_LANGUAGE, s_language ? s_language : "");
	config_set_string(config, CONFIG_SECTION, CONFIG_KEY_INITIAL_PROMPT, s_initial_prompt ? s_initial_prompt : "");
	config_save(config);
	config_close(config);
	bfree(path);
}

static void load_transcription_config(void)
{
	char *path = obs_module_config_path("config.ini");
	if (!path)
		return;
	config_t *config = NULL;
	if (config_open(&config, path, CONFIG_OPEN_EXISTING) != CONFIG_SUCCESS) {
		bfree(path);
		return;
	}
	const char *saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_SPEECH_CONFIDENCE_MIN);
	if (saved && saved[0]) {
		double v = 0.0;
		if (sscanf(saved, "%lf", &v) == 1 && v >= 0.0 && v <= 1.0)
			s_speech_confidence_min = (float)v;
	}
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_FILTER_PHRASES);
	if (saved) {
		if (s_filter_phrases)
			bfree(s_filter_phrases);
		s_filter_phrases = saved[0] ? bstrdup(saved) : NULL;
	}
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_REPLACE_PHRASES);
	if (saved) {
		if (s_replace_phrases)
			bfree(s_replace_phrases);
		s_replace_phrases = saved[0] ? bstrdup(saved) : NULL;
	}
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_MODEL_FILE);
	if (saved) {
		if (s_model_file)
			bfree(s_model_file);
		s_model_file = saved[0] ? bstrdup(saved) : NULL;
	}
	if (!s_model_file)
		s_model_file = bstrdup(DEFAULT_MODEL_FILE);
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_LANGUAGE);
	if (saved) {
		if (s_language)
			bfree(s_language);
		s_language = saved[0] ? bstrdup(saved) : NULL;
	}
	if (!s_language)
		s_language = bstrdup("en");
	saved = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_INITIAL_PROMPT);
	if (saved) {
		if (s_initial_prompt)
			bfree(s_initial_prompt);
		s_initial_prompt = saved[0] ? bstrdup(saved) : NULL;
	}
	config_close(config);
	bfree(path);
}

float transcription_service_get_speech_confidence_min(void)
{
	return s_speech_confidence_min;
}

void transcription_service_set_speech_confidence_min(float value)
{
	if (value < 0.0f)
		value = 0.0f;
	if (value > 1.0f)
		value = 1.0f;
	s_speech_confidence_min = value;
	save_transcription_config();
}

const char *transcription_service_get_filter_phrases(void)
{
	return s_filter_phrases ? s_filter_phrases : "";
}

void transcription_service_set_filter_phrases(const char *value)
{
	if (s_filter_phrases)
		bfree(s_filter_phrases);
	s_filter_phrases = (value && value[0]) ? bstrdup(value) : NULL;
	save_transcription_config();
}

const char *transcription_service_get_replace_phrases(void)
{
	return s_replace_phrases ? s_replace_phrases : "";
}

void transcription_service_set_replace_phrases(const char *value)
{
	if (s_replace_phrases)
		bfree(s_replace_phrases);
	s_replace_phrases = (value && value[0]) ? bstrdup(value) : NULL;
	save_transcription_config();
}

const char *transcription_service_get_model_file(void)
{
	return s_model_file ? s_model_file : DEFAULT_MODEL_FILE;
}

void transcription_service_set_model_file(const char *value)
{
	if (s_model_file)
		bfree(s_model_file);
	s_model_file = (value && value[0]) ? bstrdup(value) : bstrdup(DEFAULT_MODEL_FILE);
	save_transcription_config();
#if ENABLE_WHISPER
	pthread_mutex_lock(&s_whisper_mutex);
	transcription_reload_model_locked();
	pthread_mutex_unlock(&s_whisper_mutex);
#endif
}

const char *transcription_service_get_language(void)
{
	return s_language ? s_language : "";
}

void transcription_service_set_language(const char *value)
{
	if (s_language)
		bfree(s_language);
	s_language = (value && value[0]) ? bstrdup(value) : NULL;
	save_transcription_config();
}

const char *transcription_service_get_initial_prompt(void)
{
	return s_initial_prompt ? s_initial_prompt : "";
}

void transcription_service_set_initial_prompt(const char *value)
{
	if (s_initial_prompt)
		bfree(s_initial_prompt);
	s_initial_prompt = (value && value[0]) ? bstrdup(value) : NULL;
	save_transcription_config();
}

void transcription_service_reset_for_recording(void)
{
	pthread_mutex_lock(&s_pending_mutex);
	memset(&s_pending_segment, 0, sizeof(s_pending_segment));
	s_pending_active = false;
	pthread_mutex_unlock(&s_pending_mutex);
	pthread_mutex_lock(&s_caption_mutex);
	s_caption_count = 0;
	pthread_mutex_unlock(&s_caption_mutex);
}

void transcription_service_push_audio_chunk(const float *samples, uint32_t num_samples, uint64_t start_ts_ms)
{
	if (!samples || num_samples == 0)
		return;
	float *copy = (float *)bmalloc(num_samples * sizeof(float));
	if (!copy)
		return;
	memcpy(copy, samples, num_samples * sizeof(float));

	pthread_mutex_lock(&s_input_mutex);
	if (s_queue_count >= CHUNK_QUEUE_MAX) {
		/* Drop oldest chunk to make room */
		audio_chunk_t *old = &s_chunk_queue[s_queue_head];
		if (old->samples) {
			bfree(old->samples);
			old->samples = NULL;
		}
		s_queue_head = (s_queue_head + 1) % CHUNK_QUEUE_MAX;
		s_queue_count--;
		uint64_t now_ns = os_gettime_ns();
		if (now_ns - s_last_drop_log_ns > 2000000000ULL) {
			obs_log(LOG_WARNING, "transcription: audio backlog exceeded; dropping old chunks");
			s_last_drop_log_ns = now_ns;
		}
	}
	{
		uint32_t tail = (s_queue_head + s_queue_count) % CHUNK_QUEUE_MAX;
		s_chunk_queue[tail].samples = copy;
		s_chunk_queue[tail].num_samples = num_samples;
		s_chunk_queue[tail].start_ts_ms = start_ts_ms;
		s_queue_count++;
	}
	pthread_cond_signal(&s_input_cond);
	pthread_mutex_unlock(&s_input_mutex);
}

void transcription_service_get_display_text(uint64_t elapsed_ms, char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;
	buf[0] = '\0';

	pthread_mutex_lock(&s_caption_mutex);
	caption_segment_t *current = NULL;
	for (uint32_t i = s_caption_count; i > 0; i--) {
		caption_segment_t *seg = &s_caption_segments[i - 1];
		if (elapsed_ms >= seg->start_ms && elapsed_ms <= seg->end_ms) {
			current = seg;
			break;
		}
	}
	if (!current) {
		for (uint32_t i = s_caption_count; i > 0; i--) {
			caption_segment_t *seg = &s_caption_segments[i - 1];
			if (elapsed_ms >= seg->start_ms) {
				current = seg;
				break;
			}
		}
	}
	if (current) {
		char temp[CAPTION_TEXT_MAX];
		size_t len = strlen(current->text);
		if (len >= sizeof(temp))
			len = sizeof(temp) - 1;
		memcpy(temp, current->text, len);
		temp[len] = '\0';

		uint32_t used = 1;
		uint64_t cur_start = current->start_ms;
		for (uint32_t i = s_caption_count; i > 0 && used < DISPLAY_HISTORY_MAX_SEGMENTS; i--) {
			caption_segment_t *seg = &s_caption_segments[i - 1];
			if (seg == current)
				continue;
			if (seg->end_ms > cur_start)
				continue;
			if (cur_start - seg->end_ms > DISPLAY_HISTORY_GAP_MS)
				break;
			size_t seg_len = strlen(seg->text);
			size_t temp_len = strlen(temp);
			if (temp_len + seg_len + 2 >= sizeof(temp))
				break;
			char combined[CAPTION_TEXT_MAX];
			snprintf(combined, sizeof(combined), "%s %s", seg->text, temp);
			strncpy(temp, combined, sizeof(temp) - 1);
			temp[sizeof(temp) - 1] = '\0';
			used++;
			cur_start = seg->start_ms;
		}

		size_t out_len = strlen(temp);
		if (out_len >= buf_size)
			out_len = buf_size - 1;
		memcpy(buf, temp, out_len);
		buf[out_len] = '\0';
	}
	pthread_mutex_unlock(&s_caption_mutex);
}

static void ms_to_srt_time(uint64_t ms, char *buf, size_t buf_size)
{
	uint64_t h = ms / 3600000;
	ms %= 3600000;
	uint64_t m = ms / 60000;
	ms %= 60000;
	uint64_t s = ms / 1000;
	uint64_t millis = ms % 1000;
	snprintf(buf, buf_size, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ",%03" PRIu64, h, m, s, millis);
}

static char last_nonspace_char(const char *text)
{
	if (!text)
		return '\0';
	size_t len = strlen(text);
	while (len > 0) {
		char c = text[len - 1];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
			return c;
		len--;
	}
	return '\0';
}

static bool starts_with_lowercase(const char *text)
{
	if (!text)
		return false;
	const unsigned char *p = (const unsigned char *)text;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (!*p)
		return false;
	return isalpha(*p) && islower(*p);
}

static bool is_sentence_terminator(char c)
{
	return c == '.' || c == '!' || c == '?';
}

static size_t word_count_limited(const char *text, size_t max_words)
{
	if (!text || max_words == 0)
		return 0;
	size_t count = 0;
	bool in_word = false;
	for (const char *p = text; *p; p++) {
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			in_word = false;
		} else if (!in_word) {
			in_word = true;
			count++;
			if (count >= max_words)
				return count;
		}
	}
	return count;
}

static bool should_merge_srt(const srt_segment_t *prev, const caption_segment_t *cur)
{
	if (!prev || !cur)
		return false;
	if (cur->start_ms < prev->end_ms) {
		/* Overlap: treat as no gap. */
	} else if (cur->start_ms - prev->end_ms > SRT_MERGE_MAX_GAP_MS) {
		return false;
	}
	uint64_t merged_duration = cur->end_ms - prev->start_ms;
	if (merged_duration > SRT_MERGE_MAX_DURATION_MS)
		return false;

	size_t prev_len = strlen(prev->text);
	size_t cur_len = strlen(cur->text);
	if (prev_len + cur_len + 2 > SRT_TEXT_MAX)
		return false;
	if (prev_len + cur_len + 2 > SRT_MERGE_MAX_CHARS)
		return false;

	char last = last_nonspace_char(prev->text);
	bool prev_ends_sentence = is_sentence_terminator(last);
	bool next_starts_lower = starts_with_lowercase(cur->text);
	uint64_t prev_dur = prev->end_ms - prev->start_ms;

	if (!prev_ends_sentence)
		return true;
	if (next_starts_lower)
		return true;
	if (prev_dur < SRT_MIN_SEGMENT_MS)
		return true;
	if (prev_len < SRT_MIN_SEGMENT_CHARS)
		return true;
	if (word_count_limited(prev->text, 3) < 3)
		return true;

	return false;
}

static void append_segment_text(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0 || !src || !src[0])
		return;
	size_t dst_len = strlen(dst);
	if (dst_len >= dst_size - 1)
		return;
	const char *p = src;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	if (!*p)
		return;
	char last = last_nonspace_char(dst);
	bool need_space = false;
	if (last && last != ' ' && last != '\n' && last != '\t' && last != '\r') {
		if (*p != '.' && *p != ',' && *p != '!' && *p != '?' && *p != ':' && *p != ';')
			need_space = true;
	}
	if (need_space && dst_len + 1 < dst_size - 1) {
		dst[dst_len++] = ' ';
		dst[dst_len] = '\0';
	}
	strncat(dst, p, dst_size - dst_len - 1);
}

void transcription_service_write_srt(const char *video_path)
{
	if (!video_path || !video_path[0])
		return;
	commit_pending_if_needed(true);
	/* Derive SRT path: same directory and base name, .srt extension */
	char srt_path[1024];
	const char *dot = strrchr(video_path, '.');
	if (dot && (size_t)(dot - video_path) < sizeof(srt_path) - 5) {
		size_t len = (size_t)(dot - video_path);
		memcpy(srt_path, video_path, len);
		srt_path[len] = '\0';
		strcat(srt_path, ".srt");
	} else {
		snprintf(srt_path, sizeof(srt_path), "%s.srt", video_path);
	}
	FILE *f = fopen(srt_path, "w");
	if (!f) {
		obs_log(LOG_WARNING, "transcription: could not open SRT file: %s", srt_path);
		return;
	}

	caption_segment_t local_segments[CAPTION_SEGMENT_MAX];
	uint32_t local_count = 0;
	pthread_mutex_lock(&s_caption_mutex);
	local_count = s_caption_count;
	if (local_count > CAPTION_SEGMENT_MAX)
		local_count = CAPTION_SEGMENT_MAX;
	if (local_count > 0)
		memcpy(local_segments, s_caption_segments, local_count * sizeof(caption_segment_t));
	pthread_mutex_unlock(&s_caption_mutex);
	/* Ensure chronological order in case segments arrive slightly out of order. */
	for (uint32_t i = 1; i < local_count; i++) {
		caption_segment_t key = local_segments[i];
		uint32_t j = i;
		while (j > 0 && local_segments[j - 1].start_ms > key.start_ms) {
			local_segments[j] = local_segments[j - 1];
			j--;
		}
		local_segments[j] = key;
	}

	if (local_count == 0) {
		obs_log(LOG_WARNING, "transcription: no segments available; writing empty SRT");
		fclose(f);
		return;
	}

	srt_segment_t merged[CAPTION_SEGMENT_MAX];
	uint32_t merged_count = 0;
	for (uint32_t i = 0; i < local_count; i++) {
		caption_segment_t *seg = &local_segments[i];
		trim_in_place(seg->text);
		if (is_empty_or_whitespace(seg->text))
			continue;
		if (is_repetitive_phrase(seg->text))
			continue;
		if (merged_count == 0) {
			srt_segment_t *dst = &merged[merged_count++];
			dst->start_ms = seg->start_ms;
			dst->end_ms = seg->end_ms;
			dst->text[0] = '\0';
			append_segment_text(dst->text, sizeof(dst->text), seg->text);
			continue;
		}
		srt_segment_t *prev = &merged[merged_count - 1];
		if (should_merge_srt(prev, seg)) {
			append_segment_text(prev->text, sizeof(prev->text), seg->text);
			if (seg->end_ms > prev->end_ms)
				prev->end_ms = seg->end_ms;
			continue;
		}
		if (merged_count >= CAPTION_SEGMENT_MAX)
			break;
		srt_segment_t *dst = &merged[merged_count++];
		dst->start_ms = seg->start_ms;
		dst->end_ms = seg->end_ms;
		dst->text[0] = '\0';
		append_segment_text(dst->text, sizeof(dst->text), seg->text);
	}

	for (uint32_t i = 0; i < merged_count; i++) {
		srt_segment_t *seg = &merged[i];
		char start_buf[32];
		char end_buf[32];
		char line[640];
		ms_to_srt_time(seg->start_ms, start_buf, sizeof(start_buf));
		ms_to_srt_time(seg->end_ms, end_buf, sizeof(end_buf));
		snprintf(line, sizeof(line), "%u\n%s --> %s\n%s\n\n", (unsigned)(i + 1), start_buf, end_buf, seg->text);
		fputs(line, f);
	}
	fclose(f);
	obs_log(LOG_INFO, "transcription: wrote SRT to %s", srt_path);
}

bool transcription_service_wait_for_idle(uint32_t timeout_ms)
{
	uint64_t start = os_gettime_ns();
	while (true) {
		bool idle = false;
		pthread_mutex_lock(&s_input_mutex);
		idle = (s_queue_count == 0 && !s_worker_busy);
		pthread_mutex_unlock(&s_input_mutex);
		if (idle)
			return true;
		uint64_t elapsed_ms = (os_gettime_ns() - start) / 1000000;
		if (elapsed_ms >= timeout_ms)
			return false;
		os_sleep_ms(10);
	}
}

void transcription_service_init(void)
{
	pthread_mutex_init_value(&s_caption_mutex);
	pthread_mutex_init_value(&s_pending_mutex);
	pthread_mutex_init_value(&s_input_mutex);
	pthread_cond_init(&s_input_cond, NULL);
	s_queue_head = 0;
	s_queue_count = 0;
	s_worker_busy = false;
	for (uint32_t i = 0; i < CHUNK_QUEUE_MAX; i++)
		s_chunk_queue[i].samples = NULL;
	s_caption_count = 0;
	load_transcription_config();
	if (!s_language)
		s_language = bstrdup("en");
#if ENABLE_WHISPER
	pthread_mutex_init_value(&s_whisper_mutex);
	s_whisper_ctx = NULL;
	pthread_mutex_lock(&s_whisper_mutex);
	transcription_reload_model_locked();
	pthread_mutex_unlock(&s_whisper_mutex);
#endif

	s_worker_running = true;
	if (pthread_create(&s_worker_thread, NULL, worker_thread, NULL) != 0) {
		obs_log(LOG_ERROR, "transcription: failed to start worker thread");
		s_worker_running = false;
	}
}

void transcription_service_unload(void)
{
	s_worker_running = false;
	pthread_mutex_lock(&s_input_mutex);
	pthread_cond_signal(&s_input_cond);
	pthread_mutex_unlock(&s_input_mutex);
	pthread_join(s_worker_thread, NULL);

	for (uint32_t i = 0; i < CHUNK_QUEUE_MAX; i++) {
		if (s_chunk_queue[i].samples) {
			bfree(s_chunk_queue[i].samples);
			s_chunk_queue[i].samples = NULL;
		}
	}
	s_queue_head = 0;
	s_queue_count = 0;
#if ENABLE_WHISPER
	if (s_whisper_ctx) {
		whisper_wrapper_free(s_whisper_ctx);
		s_whisper_ctx = NULL;
	}
	pthread_mutex_destroy(&s_whisper_mutex);
#endif

	if (s_filter_phrases) {
		bfree(s_filter_phrases);
		s_filter_phrases = NULL;
	}
	if (s_replace_phrases) {
		bfree(s_replace_phrases);
		s_replace_phrases = NULL;
	}
	if (s_model_file) {
		bfree(s_model_file);
		s_model_file = NULL;
	}
	if (s_language) {
		bfree(s_language);
		s_language = NULL;
	}
	if (s_initial_prompt) {
		bfree(s_initial_prompt);
		s_initial_prompt = NULL;
	}
	pthread_cond_destroy(&s_input_cond);
	pthread_mutex_destroy(&s_input_mutex);
	pthread_mutex_destroy(&s_caption_mutex);
	pthread_mutex_destroy(&s_pending_mutex);
}
