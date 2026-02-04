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
#include "captions/captions.service.h"
#include "transcription/transcription.service.h"
#if ENABLE_FRONTEND_API
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>
#endif
#if ENABLE_QT
#include "frontend/frontend_settings_ui.h"
#endif
#include <stdbool.h>
#include <stdint.h>

#if ENABLE_FRONTEND_API
static bool s_recording = false;
static uint64_t s_t0_ns = 0;
static pthread_t s_srt_thread;
static pthread_mutex_t s_srt_mutex;
static bool s_srt_thread_running = false;

typedef struct {
	char *path;
} srt_job_t;

static void *srt_writer_thread(void *data)
{
	srt_job_t *job = (srt_job_t *)data;
	if (job && job->path) {
		if (!transcription_service_wait_for_idle(15000))
			obs_log(LOG_WARNING, "transcription: worker not idle before SRT write (timeout, continuing)");
		transcription_service_write_srt(job->path);
		bfree(job->path);
	}
	bfree(job);
	pthread_mutex_lock(&s_srt_mutex);
	s_srt_thread_running = false;
	pthread_mutex_unlock(&s_srt_mutex);
	return NULL;
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	(void)private_data;
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING) {
		/* Reset segment/caption state and SRT index for new recording (plan § 4.1.1) */
		captions_service_reset_for_recording(false); /* overwrite vs append: Phase 3 */
		transcription_service_reset_for_recording();
		obs_log(LOG_INFO, "recording starting, state reset");
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		s_recording = true;
		s_t0_ns = os_gettime_ns();
		captions_service_attach_to_current_scene();
		audio_service_start();
		obs_log(LOG_INFO, "recording started, t0 set");
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		audio_service_stop();
		s_recording = false;
		s_t0_ns = 0;
		{
			char *last_rec = obs_frontend_get_last_recording();
			if (last_rec && last_rec[0]) {
				pthread_mutex_lock(&s_srt_mutex);
				bool running = s_srt_thread_running;
				pthread_mutex_unlock(&s_srt_mutex);
				if (running)
					pthread_join(s_srt_thread, NULL);

				srt_job_t *job = bmalloc(sizeof(*job));
				if (job) {
					job->path = bstrdup(last_rec);
					pthread_mutex_lock(&s_srt_mutex);
					s_srt_thread_running = (pthread_create(&s_srt_thread, NULL, srt_writer_thread, job) == 0);
					pthread_mutex_unlock(&s_srt_mutex);
					if (!s_srt_thread_running) {
						bfree(job->path);
						bfree(job);
					}
				}
			}
			bfree(last_rec);
		}
		obs_log(LOG_INFO, "recording stopped");
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		captions_service_detach_from_scene();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		captions_service_on_exit();
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
	pthread_mutex_init_value(&s_srt_mutex);
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
	pthread_mutex_lock(&s_srt_mutex);
	bool running = s_srt_thread_running;
	pthread_mutex_unlock(&s_srt_mutex);
	if (running)
		pthread_join(s_srt_thread, NULL);
	pthread_mutex_destroy(&s_srt_mutex);
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
