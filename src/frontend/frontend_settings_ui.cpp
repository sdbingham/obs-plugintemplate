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

#include "frontend/frontend_settings_ui.h"
#include "audio/audio.service.h"
#include <obs.h>
#include <obs-source.h>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QObject>
#include <QVBoxLayout>
#include <QWidget>

static QDialog *s_dialog = nullptr;

static bool enum_audio_sources(void *param, obs_source_t *source)
{
	QComboBox *combo = static_cast<QComboBox *>(param);
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return true;
	const char *name = obs_source_get_name(source);
	if (name && name[0])
		combo->addItem(QString::fromUtf8(name));
	return true;
}

void frontend_settings_show_dialog(void)
{
	if (s_dialog) {
		s_dialog->raise();
		s_dialog->activateWindow();
		return;
	}
	s_dialog = new QDialog();
	s_dialog->setWindowTitle(QStringLiteral("OBS Transcription Settings"));
	QVBoxLayout *layout = new QVBoxLayout(s_dialog);
	QLabel *label = new QLabel(QStringLiteral("Audio source to transcribe:"), s_dialog);
	layout->addWidget(label);
	QComboBox *audio_combo = new QComboBox(s_dialog);
	audio_combo->addItem(QStringLiteral("(None)"), QString());
	obs_enum_sources(enum_audio_sources, audio_combo);
	layout->addWidget(audio_combo);
	QObject::connect(audio_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), [audio_combo](int idx) {
		if (idx <= 0)
			audio_service_set_source(nullptr);
		else
			audio_service_set_source(audio_combo->currentText().toUtf8().constData());
	});
	s_dialog->setAttribute(Qt::WA_DeleteOnClose);
	QObject::connect(s_dialog, &QObject::destroyed, []() { s_dialog = nullptr; });
	s_dialog->show();
}
