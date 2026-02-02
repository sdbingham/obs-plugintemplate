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
#include <QAbstractButton>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
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
	QLabel *hint = new QLabel(QStringLiteral("Use a microphone or audio input (e.g. Mic/Aux) for speech. Video-only sources will not produce captions."), s_dialog);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	layout->addWidget(label);
	layout->addWidget(hint);
	QComboBox *audio_combo = new QComboBox(s_dialog);
	audio_combo->addItem(QStringLiteral("(None)"), QString());
	obs_enum_sources(enum_audio_sources, audio_combo);
	const char *saved_source = audio_service_get_source();
	if (saved_source && saved_source[0]) {
		int idx = audio_combo->findText(QString::fromUtf8(saved_source));
		if (idx >= 0)
			audio_combo->setCurrentIndex(idx);
	}
	layout->addWidget(audio_combo);

	auto apply_selection = [audio_combo]() {
		int idx = audio_combo->currentIndex();
		if (idx <= 0) {
			audio_service_set_source(nullptr);
		} else {
			QByteArray name_utf8 = audio_combo->currentText().toUtf8();
			audio_service_set_source(name_utf8.isEmpty() ? nullptr : name_utf8.constData());
		}
	};

	QDialogButtonBox *buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
		s_dialog);
	QDialog *dialog = s_dialog;
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [apply_selection, dialog]() {
		apply_selection();
		dialog->accept();
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::clicked, dialog, [apply_selection, buttons](QAbstractButton *button) {
		if (buttons->buttonRole(button) == QDialogButtonBox::ApplyRole)
			apply_selection();
	});
	layout->addWidget(buttons);

	s_dialog->setAttribute(Qt::WA_DeleteOnClose);
	QObject::connect(s_dialog, &QObject::destroyed, []() { s_dialog = nullptr; });
	s_dialog->show();
}
