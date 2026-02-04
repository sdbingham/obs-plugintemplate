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
#include <obs-module.h>
#include <obs-source.h>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSslSocket>
#include <QSpinBox>
#include <QThread>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

static QDialog *s_dialog = nullptr;

struct ModelOption {
	const char *id;
	const char *label;
	const char *file;
	const char *url;
};

static const ModelOption kModelOptions[] = {
	{"tiny.en", "Tiny (English, fastest)", "ggml-tiny.en.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"},
	{"base.en", "Base (English)", "ggml-base.en.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"},
	{"small.en", "Small (English, better accuracy)", "ggml-small.en.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin"},
	{"medium.en", "Medium (English, best accuracy)", "ggml-medium.en.bin",
	 "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin"},
};

static const ModelOption *find_model_by_file(const QString &file)
{
	for (const auto &opt : kModelOptions) {
		if (file == QString::fromUtf8(opt.file))
			return &opt;
	}
	return nullptr;
}

static QString model_config_path(const char *file)
{
	if (!file || !file[0])
		return QString();
	QString subpath = QStringLiteral("models/") + QString::fromUtf8(file);
	char *path = obs_module_config_path(subpath.toUtf8().constData());
	if (!path)
		return QString();
	QString out = QString::fromUtf8(path);
	bfree(path);
	return out;
}

struct ModelDownloadState {
	QNetworkAccessManager *manager = nullptr;
	QNetworkReply *reply = nullptr;
	QFile *file = nullptr;
	QThread *thread = nullptr;
	QProgressBar *progress = nullptr;
	QLabel *status = nullptr;
	QPushButton *retry = nullptr;
	QDialogButtonBox *buttons = nullptr;
	QString dest_path;
	QString tmp_path;
	QString pending_model_file;
	bool downloading = false;
};

class WinHttpDownloadWorker : public QObject {
	Q_OBJECT
public:
	QString url;
	QString tmp_path;
	explicit WinHttpDownloadWorker(const QString &u, const QString &tmp)
		: url(u), tmp_path(tmp)
	{
	}

signals:
	void progress(qint64 received, qint64 total);
	void finished(bool ok, const QString &error);

public slots:
	void run();
};

void WinHttpDownloadWorker::run()
{
#ifdef _WIN32
	URL_COMPONENTS parts = {};
	wchar_t host[256] = {};
	wchar_t path[2048] = {};
	std::wstring url_w = url.toStdWString();
	parts.dwStructSize = sizeof(parts);
	parts.lpszHostName = host;
	parts.dwHostNameLength = (DWORD)_countof(host);
	parts.lpszUrlPath = path;
	parts.dwUrlPathLength = (DWORD)_countof(path);
	parts.dwSchemeLength = (DWORD)-1;
	if (!WinHttpCrackUrl(url_w.c_str(), 0, 0, &parts)) {
		emit finished(false, QStringLiteral("Download failed: invalid URL."));
		return;
	}

	HINTERNET hSession = WinHttpOpen(L"obs-transcription/1.0",
					 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
					 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) {
		emit finished(false, QStringLiteral("Download failed: WinHTTP init error."));
		return;
	}
	HINTERNET hConnect = WinHttpConnect(hSession, host, parts.nPort, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		emit finished(false, QStringLiteral("Download failed: WinHTTP connect error."));
		return;
	}
	const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER,
						WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!hRequest) {
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		emit finished(false, QStringLiteral("Download failed: WinHTTP request error."));
		return;
	}
	BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				     WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
	if (!ok || !WinHttpReceiveResponse(hRequest, NULL)) {
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		emit finished(false, QStringLiteral("Download failed: WinHTTP send error."));
		return;
	}

	qint64 total = -1;
	wchar_t len_buf[64] = {};
	DWORD len_buf_len = sizeof(len_buf);
	DWORD idx = 0;
	if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
				len_buf, &len_buf_len, &idx)) {
		total = _wtoi64(len_buf);
	}

	QFile out(tmp_path);
	if (!out.open(QIODevice::WriteOnly)) {
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		emit finished(false, QStringLiteral("Download failed: cannot write file."));
		return;
	}

	const DWORD chunk = 64 * 1024;
	std::vector<char> buffer(chunk);
	DWORD bytes_read = 0;
	qint64 received = 0;
	do {
		bytes_read = 0;
		if (!WinHttpReadData(hRequest, buffer.data(), chunk, &bytes_read)) {
			out.close();
			WinHttpCloseHandle(hRequest);
			WinHttpCloseHandle(hConnect);
			WinHttpCloseHandle(hSession);
			emit finished(false, QStringLiteral("Download failed: WinHTTP read error."));
			return;
		}
		if (bytes_read > 0) {
			out.write(buffer.data(), bytes_read);
			received += bytes_read;
			emit progress(received, total);
		}
	} while (bytes_read > 0);

	out.flush();
	out.close();
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	emit finished(true, QString());
#else
	emit finished(false, QStringLiteral("Download failed: WinHTTP not supported on this platform."));
#endif
}

static void set_model_status(ModelDownloadState *state, const QString &text, bool is_error = false)
{
	if (!state || !state->status)
		return;
	state->status->setText(text);
	if (is_error)
		state->status->setStyleSheet(QStringLiteral("color: #cc3333;"));
	else
		state->status->setStyleSheet(QStringLiteral("color: gray;"));
}

static void finish_model_download(ModelDownloadState *state, bool success, const QString &message)
{
	if (!state)
		return;
	state->downloading = false;
	if (state->buttons)
		state->buttons->setEnabled(true);
	if (state->retry)
		state->retry->setEnabled(true);
	if (state->reply) {
		state->reply->deleteLater();
		state->reply = nullptr;
	}
	if (state->thread) {
		state->thread->quit();
		state->thread->wait();
		delete state->thread;
		state->thread = nullptr;
	}
	if (state->file) {
		state->file->flush();
		state->file->close();
		delete state->file;
		state->file = nullptr;
	}
	if (!success) {
		if (!state->tmp_path.isEmpty())
			QFile::remove(state->tmp_path);
		set_model_status(state, message, true);
		if (state->progress) {
			state->progress->setValue(0);
			state->progress->setFormat(QStringLiteral("0%"));
		}
		return;
	}
	QFile::remove(state->dest_path);
	if (!QFile::rename(state->tmp_path, state->dest_path)) {
		set_model_status(state, QStringLiteral("Download failed: could not move file into place."), true);
		if (state->progress) {
			state->progress->setValue(0);
			state->progress->setFormat(QStringLiteral("0%"));
		}
		return;
	}
	set_model_status(state, QStringLiteral("Downloaded to %1").arg(state->dest_path));
	if (state->progress) {
		state->progress->setValue(100);
		state->progress->setFormat(QStringLiteral("100%"));
	}
	if (!state->pending_model_file.isEmpty())
		transcription_service_set_model_file(state->pending_model_file.toUtf8().constData());
}

static void start_model_download(ModelDownloadState *state, const ModelOption &model)
{
	if (!state || !state->manager)
		return;
	const QString dest_path = model_config_path(model.file);
	if (dest_path.isEmpty()) {
		set_model_status(state, QStringLiteral("Download failed: could not resolve model path."), true);
		return;
	}
	if (QFileInfo::exists(dest_path)) {
		state->pending_model_file = QString::fromUtf8(model.file);
		set_model_status(state, QStringLiteral("Model already downloaded."));
		if (state->progress) {
			state->progress->setValue(100);
			state->progress->setFormat(QStringLiteral("100%"));
		}
		transcription_service_set_model_file(state->pending_model_file.toUtf8().constData());
		return;
	}
	if (state->downloading && state->reply) {
		state->reply->abort();
	}
	if (state->thread) {
		state->thread->quit();
		state->thread->wait();
		delete state->thread;
		state->thread = nullptr;
	}

	state->downloading = true;
	state->pending_model_file = QString::fromUtf8(model.file);
	state->dest_path = dest_path;
	state->tmp_path = dest_path + QStringLiteral(".partial");
	if (state->buttons)
		state->buttons->setEnabled(false);
	if (state->retry)
		state->retry->setEnabled(false);

	QFileInfo dest_info(dest_path);
	QDir().mkpath(dest_info.absolutePath());

	if (!QSslSocket::supportsSsl()) {
#ifdef _WIN32
		set_model_status(state, QStringLiteral("Downloading via WinHTTP..."));
		state->thread = new QThread();
		WinHttpDownloadWorker *worker = new WinHttpDownloadWorker(QString::fromUtf8(model.url), state->tmp_path);
		worker->moveToThread(state->thread);
		QObject::connect(state->thread, &QThread::started, worker, &WinHttpDownloadWorker::run);
		QObject::connect(worker, &WinHttpDownloadWorker::progress, state->status,
				 [state](qint64 received, qint64 total) {
					 if (state->progress) {
						 if (total > 0) {
							 int pct = (int)((received * 100) / total);
							 state->progress->setRange(0, 100);
							 state->progress->setValue(pct);
							 state->progress->setFormat(QStringLiteral("%1%").arg(pct));
						 } else {
							 state->progress->setRange(0, 0);
						 }
					 }
					 if (state->status && total > 0) {
						 double mb = (double)received / (1024.0 * 1024.0);
						 double mb_total = (double)total / (1024.0 * 1024.0);
						 state->status->setText(QStringLiteral("Downloading... %1 / %2 MB")
										 .arg(mb, 0, 'f', 1)
										 .arg(mb_total, 0, 'f', 1));
					 }
				 },
				 Qt::QueuedConnection);
		QObject::connect(worker, &WinHttpDownloadWorker::finished, state->status,
				 [state](bool ok, const QString &err) {
					 finish_model_download(state, ok, ok ? QString() : err);
				 },
				 Qt::QueuedConnection);
		QObject::connect(worker, &WinHttpDownloadWorker::finished, worker, &QObject::deleteLater);
		state->thread->start();
		return;
#else
		set_model_status(state, QStringLiteral("SSL not available. Cannot download over HTTPS."), true);
		return;
#endif
	}

	if (state->file) {
		state->file->close();
		delete state->file;
	}
	state->file = new QFile(state->tmp_path);
	if (!state->file->open(QIODevice::WriteOnly)) {
		delete state->file;
		state->file = nullptr;
		finish_model_download(state, false, QStringLiteral("Download failed: cannot write temp file."));
		return;
	}

	QNetworkRequest request(QUrl(QString::fromUtf8(model.url)));
	request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("obs-transcription"));
	state->reply = state->manager->get(request);
	set_model_status(state, QStringLiteral("Downloading %1...").arg(QString::fromUtf8(model.label)));
	if (state->progress) {
		state->progress->setValue(0);
		state->progress->setFormat(QStringLiteral("0%"));
	}

	QObject::connect(state->reply, &QNetworkReply::readyRead, [state]() {
		if (state->file)
			state->file->write(state->reply->readAll());
	});
	QObject::connect(state->reply, &QNetworkReply::downloadProgress,
			 [state](qint64 received, qint64 total) {
				 if (state->progress) {
					 if (total > 0) {
						 int pct = (int)((received * 100) / total);
						 state->progress->setValue(pct);
						 state->progress->setFormat(QStringLiteral("%1%").arg(pct));
					 } else {
						 state->progress->setRange(0, 0);
					 }
				 }
				 if (state->status && total > 0) {
					 double mb = (double)received / (1024.0 * 1024.0);
					 double mb_total = (double)total / (1024.0 * 1024.0);
					 state->status->setText(QStringLiteral("Downloading... %1 / %2 MB")
									.arg(mb, 0, 'f', 1)
									.arg(mb_total, 0, 'f', 1));
				 }
			 });
	QObject::connect(state->reply, &QNetworkReply::finished, [state]() {
		if (!state->reply)
			return;
		if (state->reply->error() != QNetworkReply::NoError) {
			finish_model_download(state, false,
					      QStringLiteral("Download failed: %1").arg(state->reply->errorString()));
			return;
		}
		finish_model_download(state, true, QString());
	});
}

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
	s_dialog->setMinimumWidth(720);
	s_dialog->setSizeGripEnabled(true);
	QVBoxLayout *layout = new QVBoxLayout(s_dialog);
	/* Audio input group */
	QGroupBox *audio_group = new QGroupBox(QStringLiteral("Audio Input"), s_dialog);
	QVBoxLayout *audio_layout = new QVBoxLayout(audio_group);
	QLabel *label = new QLabel(QStringLiteral("Audio source to transcribe:"), audio_group);
	QLabel *hint = new QLabel(QStringLiteral("Use a microphone or audio input (e.g. Mic/Aux) for speech. Video-only sources will not produce captions."), audio_group);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QComboBox *audio_combo = new QComboBox(audio_group);
	audio_combo->addItem(QStringLiteral("(None)"), QString());
	obs_enum_sources(enum_audio_sources, audio_combo);
	const char *saved_source = audio_service_get_source();
	if (saved_source && saved_source[0]) {
		int idx = audio_combo->findText(QString::fromUtf8(saved_source));
		if (idx >= 0)
			audio_combo->setCurrentIndex(idx);
	}
	QLabel *mute_label = new QLabel(QStringLiteral("Caption when (optional mute source):"), audio_group);
	QLabel *mute_hint = new QLabel(
		QStringLiteral("If set, transcription runs only when this source is unmuted, active, and showing. Leave empty to use the audio source above."),
		audio_group);
	mute_hint->setWordWrap(true);
	mute_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QComboBox *mute_combo = new QComboBox(audio_group);
	mute_combo->addItem(QStringLiteral("(None)"), QString());
	obs_enum_sources(enum_audio_sources, mute_combo);
	const char *saved_mute = audio_service_get_mute_source();
	if (saved_mute && saved_mute[0]) {
		int idx = mute_combo->findText(QString::fromUtf8(saved_mute));
		if (idx >= 0)
			mute_combo->setCurrentIndex(idx);
	}
	QCheckBox *process_muted_cb = new QCheckBox(QStringLiteral("Process audio while capture source is muted"), audio_group);
	process_muted_cb->setChecked(audio_service_get_process_while_muted());
	QCheckBox *only_visible_cb = new QCheckBox(QStringLiteral("Only transcribe when capture source is active and showing"), audio_group);
	only_visible_cb->setChecked(audio_service_get_only_when_visible());
	audio_layout->addWidget(label);
	audio_layout->addWidget(hint);
	audio_layout->addWidget(audio_combo);
	audio_layout->addWidget(mute_label);
	audio_layout->addWidget(mute_hint);
	audio_layout->addWidget(mute_combo);
	audio_layout->addWidget(process_muted_cb);
	audio_layout->addWidget(only_visible_cb);
	layout->addWidget(audio_group);

	/* Latency and quality group */
	QGroupBox *quality_group = new QGroupBox(QStringLiteral("Latency and Quality"), s_dialog);
	QVBoxLayout *quality_layout = new QVBoxLayout(quality_group);
	QLabel *latency_label = new QLabel(QStringLiteral("Caption latency (ms):"), quality_group);
	QLabel *latency_hint = new QLabel(
		QStringLiteral("Lower = faster live captions, but can reduce accuracy. Recommended range 1500–5000 ms."),
		quality_group);
	latency_hint->setWordWrap(true);
	latency_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QSpinBox *latency_spin = new QSpinBox(quality_group);
	latency_spin->setRange(1500, 5000);
	latency_spin->setSingleStep(250);
	latency_spin->setSuffix(QStringLiteral(" ms"));
	latency_spin->setValue((int)audio_service_get_latency_ms());
	QLabel *speech_label = new QLabel(QStringLiteral("Minimum speech confidence (0–1):"), quality_group);
	QLabel *speech_hint = new QLabel(
		QStringLiteral("Only show segments when Whisper's speech confidence is at least this. Higher = fewer captions, less noise. Default 0.4."),
		quality_group);
	speech_hint->setWordWrap(true);
	speech_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QDoubleSpinBox *speech_spin = new QDoubleSpinBox(quality_group);
	speech_spin->setRange(0.0, 1.0);
	speech_spin->setSingleStep(0.05);
	speech_spin->setDecimals(2);
	speech_spin->setValue((double)transcription_service_get_speech_confidence_min());
	quality_layout->addWidget(latency_label);
	quality_layout->addWidget(latency_hint);
	quality_layout->addWidget(latency_spin);
	quality_layout->addWidget(speech_label);
	quality_layout->addWidget(speech_hint);
	quality_layout->addWidget(speech_spin);
	layout->addWidget(quality_group);

	/* Text processing group */
	QGroupBox *text_group = new QGroupBox(QStringLiteral("Text Processing"), s_dialog);
	QVBoxLayout *text_layout = new QVBoxLayout(text_group);
	QLabel *filter_label = new QLabel(QStringLiteral("Remove phrases (semicolon-separated):"), text_group);
	QLabel *filter_hint = new QLabel(QStringLiteral("Literal phrases to remove from captions. E.g. \"um; uh; the end\"."), text_group);
	filter_hint->setWordWrap(true);
	filter_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QLineEdit *filter_edit = new QLineEdit(text_group);
	filter_edit->setPlaceholderText(QStringLiteral("um; uh"));
	const char *filter_val = transcription_service_get_filter_phrases();
	if (filter_val && filter_val[0])
		filter_edit->setText(QString::fromUtf8(filter_val));
	QLabel *replace_label = new QLabel(QStringLiteral("Replace phrases (from|to; semicolon-separated):"), text_group);
	QLabel *replace_hint = new QLabel(QStringLiteral("Literal replacements. E.g. \"foo|bar; w|with\"."), text_group);
	replace_hint->setWordWrap(true);
	replace_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QLineEdit *replace_edit = new QLineEdit(text_group);
	replace_edit->setPlaceholderText(QStringLiteral("foo|bar; x|y"));
	const char *replace_val = transcription_service_get_replace_phrases();
	if (replace_val && replace_val[0])
		replace_edit->setText(QString::fromUtf8(replace_val));
	text_layout->addWidget(filter_label);
	text_layout->addWidget(filter_hint);
	text_layout->addWidget(filter_edit);
	text_layout->addWidget(replace_label);
	text_layout->addWidget(replace_hint);
	text_layout->addWidget(replace_edit);
	layout->addWidget(text_group);

	/* Model + language group */
	QGroupBox *model_group = new QGroupBox(QStringLiteral("Model and Language"), s_dialog);
	QVBoxLayout *model_layout = new QVBoxLayout(model_group);
	QLabel *model_label = new QLabel(QStringLiteral("Whisper model:"), model_group);
	QLabel *model_hint = new QLabel(
		QStringLiteral("Larger models are more accurate but slower. Models are downloaded on selection."),
		model_group);
	model_hint->setWordWrap(true);
	model_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QComboBox *model_combo = new QComboBox(model_group);
	for (const auto &opt : kModelOptions)
		model_combo->addItem(QString::fromUtf8(opt.label), QString::fromUtf8(opt.file));
	const char *saved_model = transcription_service_get_model_file();
	if (saved_model && saved_model[0]) {
		int idx = model_combo->findData(QString::fromUtf8(saved_model));
		if (idx >= 0)
			model_combo->setCurrentIndex(idx);
	}
	model_layout->addWidget(model_label);
	model_layout->addWidget(model_hint);
	model_layout->addWidget(model_combo);

	ModelDownloadState *model_state = new ModelDownloadState();
	model_state->manager = new QNetworkAccessManager(model_group);
	model_state->status = new QLabel(model_group);
	model_state->status->setWordWrap(true);
	model_state->status->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	model_state->progress = new QProgressBar(model_group);
	model_state->progress->setRange(0, 100);
	model_state->progress->setValue(0);
	model_state->progress->setFormat(QStringLiteral("0%"));
	model_state->retry = new QPushButton(QStringLiteral("Retry download"), model_group);
	model_state->retry->setEnabled(false);
	model_layout->addWidget(model_state->status);
	model_layout->addWidget(model_state->progress);
	model_layout->addWidget(model_state->retry);
	QObject::connect(s_dialog, &QObject::destroyed, [model_state]() {
		if (model_state->reply) {
			model_state->reply->abort();
			model_state->reply->deleteLater();
		}
		if (model_state->thread) {
			model_state->thread->quit();
			model_state->thread->wait();
			delete model_state->thread;
		}
		if (model_state->file) {
			model_state->file->close();
			delete model_state->file;
		}
		delete model_state;
	});

	auto update_model_status = [model_state, model_combo]() {
		const QString model_file = model_combo->currentData().toString();
		const ModelOption *model_opt = find_model_by_file(model_file);
		if (!model_opt) {
			set_model_status(model_state, QStringLiteral("Unknown model selection."), true);
			model_state->retry->setEnabled(false);
			return;
		}
		const QString dest_path = model_config_path(model_opt->file);
		if (dest_path.isEmpty()) {
			set_model_status(model_state, QStringLiteral("Model path unavailable."), true);
			model_state->retry->setEnabled(false);
			return;
		}
		if (QFileInfo::exists(dest_path)) {
			set_model_status(model_state, QStringLiteral("Model ready."));
			model_state->progress->setValue(100);
			model_state->progress->setFormat(QStringLiteral("100%"));
			model_state->retry->setEnabled(false);
		} else {
			set_model_status(model_state, QStringLiteral("Model not downloaded."));
			model_state->progress->setValue(0);
			model_state->progress->setFormat(QStringLiteral("0%"));
			model_state->retry->setEnabled(true);
		}
	};

	QObject::connect(model_combo, &QComboBox::currentIndexChanged, s_dialog,
			 [update_model_status]() { update_model_status(); });
	QObject::connect(model_state->retry, &QPushButton::clicked, s_dialog, [model_state, model_combo]() {
		const ModelOption *model_opt = find_model_by_file(model_combo->currentData().toString());
		if (model_opt)
			start_model_download(model_state, *model_opt);
	});
	update_model_status();

	QLabel *lang_label = new QLabel(QStringLiteral("Whisper language (e.g. en, es, auto):"), model_group);
	QLabel *lang_hint = new QLabel(
		QStringLiteral("Use \"auto\" to detect language. Note: *.en models only support English."),
		model_group);
	lang_hint->setWordWrap(true);
	lang_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QLineEdit *lang_edit = new QLineEdit(model_group);
	lang_edit->setPlaceholderText(QStringLiteral("en"));
	const char *lang_val = transcription_service_get_language();
	if (lang_val && lang_val[0])
		lang_edit->setText(QString::fromUtf8(lang_val));
	model_layout->addWidget(lang_label);
	model_layout->addWidget(lang_hint);
	model_layout->addWidget(lang_edit);

	QLabel *prompt_label = new QLabel(QStringLiteral("Initial prompt (optional):"), model_group);
	QLabel *prompt_hint = new QLabel(
		QStringLiteral("Biases transcription toward domain terms (e.g. names, jargon). Leave empty for none."),
		model_group);
	prompt_hint->setWordWrap(true);
	prompt_hint->setStyleSheet(QStringLiteral("color: gray; font-size: small;"));
	QLineEdit *prompt_edit = new QLineEdit(model_group);
	prompt_edit->setPlaceholderText(QStringLiteral("Meeting transcript about ..."));
	const char *prompt_val = transcription_service_get_initial_prompt();
	if (prompt_val && prompt_val[0])
		prompt_edit->setText(QString::fromUtf8(prompt_val));
	model_layout->addWidget(prompt_label);
	model_layout->addWidget(prompt_hint);
	model_layout->addWidget(prompt_edit);
	layout->addWidget(model_group);

	auto apply_selection = [audio_combo, mute_combo, process_muted_cb, only_visible_cb, latency_spin, speech_spin, filter_edit,
				replace_edit, model_combo, model_state, lang_edit, prompt_edit]() {
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
		audio_service_set_latency_ms((uint32_t)latency_spin->value());
		transcription_service_set_speech_confidence_min((float)speech_spin->value());
		QByteArray f = filter_edit->text().trimmed().toUtf8();
		transcription_service_set_filter_phrases(f.isEmpty() ? nullptr : f.constData());
		QByteArray r = replace_edit->text().trimmed().toUtf8();
		transcription_service_set_replace_phrases(r.isEmpty() ? nullptr : r.constData());
		const QString model_file = model_combo->currentData().toString();
		const ModelOption *model_opt = find_model_by_file(model_file);
		if (model_opt)
			start_model_download(model_state, *model_opt);
		QByteArray lang = lang_edit->text().trimmed().toUtf8();
		transcription_service_set_language(lang.isEmpty() ? nullptr : lang.constData());
		QByteArray prompt = prompt_edit->text().trimmed().toUtf8();
		transcription_service_set_initial_prompt(prompt.isEmpty() ? nullptr : prompt.constData());
	};

	QDialogButtonBox *buttons = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply,
		s_dialog);
	model_state->buttons = buttons;
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

#include "frontend_settings_ui.moc"
