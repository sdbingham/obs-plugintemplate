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
#include "transcription/transcription.service.h"
#include <obs.h>
#include <obs-source.h>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QScrollArea>
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

	QLabel *mute_label = new QLabel(QStringLiteral("Caption when (optional mute source):"), s_dialog);
	QLabel *mute_hint = new QLabel(
		QStringLiteral("If set, transcription runs only when this source is unmuted, active, and showing. Leave empty to use the audio source above."),
		s_dialog);
	mute_hint->setWordWrap(true);
	mute_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QComboBox *mute_combo = new QComboBox(s_dialog);
	mute_combo->addItem(QStringLiteral("(None)"), QString());
	obs_enum_sources(enum_audio_sources, mute_combo);
	const char *saved_mute = audio_service_get_mute_source();
	if (saved_mute && saved_mute[0]) {
		int idx = mute_combo->findText(QString::fromUtf8(saved_mute));
		if (idx >= 0)
			mute_combo->setCurrentIndex(idx);
	}
	layout->addWidget(mute_label);
	layout->addWidget(mute_hint);
	layout->addWidget(mute_combo);

	QCheckBox *process_muted_cb = new QCheckBox(QStringLiteral("Process audio while capture source is muted"), s_dialog);
	process_muted_cb->setChecked(audio_service_get_process_while_muted());
	layout->addWidget(process_muted_cb);

	QCheckBox *only_visible_cb = new QCheckBox(QStringLiteral("Only transcribe when capture source is active and showing"), s_dialog);
	only_visible_cb->setChecked(audio_service_get_only_when_visible());
	layout->addWidget(only_visible_cb);

	QLabel *speech_label = new QLabel(QStringLiteral("Minimum speech confidence (0–1):"), s_dialog);
	QLabel *speech_hint = new QLabel(
		QStringLiteral("Only show segments when Whisper's speech confidence is at least this. Higher = fewer captions, less noise. Default 0.4."),
		s_dialog);
	speech_hint->setWordWrap(true);
	speech_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QDoubleSpinBox *speech_spin = new QDoubleSpinBox(s_dialog);
	speech_spin->setRange(0.0, 1.0);
	speech_spin->setSingleStep(0.05);
	speech_spin->setDecimals(2);
	speech_spin->setValue((double)transcription_service_get_speech_confidence_min());
	layout->addWidget(speech_label);
	layout->addWidget(speech_hint);
	layout->addWidget(speech_spin);

	QLabel *filter_label = new QLabel(QStringLiteral("Remove phrases (semicolon-separated):"), s_dialog);
	QLabel *filter_hint = new QLabel(QStringLiteral("Literal phrases to remove from captions. E.g. \"um; uh; the end\"."), s_dialog);
	filter_hint->setWordWrap(true);
	filter_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QLineEdit *filter_edit = new QLineEdit(s_dialog);
	filter_edit->setPlaceholderText(QStringLiteral("um; uh"));
	const char *filter_val = transcription_service_get_filter_phrases();
	if (filter_val && filter_val[0])
		filter_edit->setText(QString::fromUtf8(filter_val));
	layout->addWidget(filter_label);
	layout->addWidget(filter_hint);
	layout->addWidget(filter_edit);

	QLabel *replace_label = new QLabel(QStringLiteral("Replace phrases (from|to; semicolon-separated):"), s_dialog);
	QLabel *replace_hint = new QLabel(QStringLiteral("Literal replacements. E.g. \"foo|bar; w|with\"."), s_dialog);
	replace_hint->setWordWrap(true);
	replace_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QLineEdit *replace_edit = new QLineEdit(s_dialog);
	replace_edit->setPlaceholderText(QStringLiteral("foo|bar; x|y"));
	const char *replace_val = transcription_service_get_replace_phrases();
	if (replace_val && replace_val[0])
		replace_edit->setText(QString::fromUtf8(replace_val));
	layout->addWidget(replace_label);
	layout->addWidget(replace_hint);
	layout->addWidget(replace_edit);

	auto apply_selection = [audio_combo, mute_combo, process_muted_cb, only_visible_cb, speech_spin, filter_edit, replace_edit]() {
		int idx = audio_combo->currentIndex();
		if (idx <= 0) {
			audio_service_set_source(nullptr);
		} else {
			QByteArray name_utf8 = audio_combo->currentText().toUtf8();
			audio_service_set_source(name_utf8.isEmpty() ? nullptr : name_utf8.constData());
		}
		idx = mute_combo->currentIndex();
		if (idx <= 0) {
			audio_service_set_mute_source(nullptr);
		} else {
			QByteArray name_utf8 = mute_combo->currentText().toUtf8();
			audio_service_set_mute_source(name_utf8.isEmpty() ? nullptr : name_utf8.constData());
		}
		audio_service_set_process_while_muted(process_muted_cb->isChecked());
		audio_service_set_only_when_visible(only_visible_cb->isChecked());
		transcription_service_set_speech_confidence_min((float)speech_spin->value());
		QByteArray f = filter_edit->text().trimmed().toUtf8();
		transcription_service_set_filter_phrases(f.isEmpty() ? nullptr : f.constData());
		QByteArray r = replace_edit->text().trimmed().toUtf8();
		transcription_service_set_replace_phrases(r.isEmpty() ? nullptr : r.constData());
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
