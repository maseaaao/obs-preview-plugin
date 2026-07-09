#pragma once

#include "mjpeg_http_server.hpp"
#include "settings.hpp"

#include <QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QSpinBox;

class SettingsDialog : public QDialog {
public:
	using ApplyCallback = std::function<void(const PreviewSettings &settings)>;

	SettingsDialog(PreviewSettings settings, const MjpegHttpServer &server, ApplyCallback apply, QWidget *parent = nullptr);

private:
	PreviewSettings collect() const;
	void refreshStatus();

	const MjpegHttpServer &server_;
	ApplyCallback apply_;

	QCheckBox *enabled_ = nullptr;
	QCheckBox *keepAspect_ = nullptr;
	QSpinBox *port_ = nullptr;
	QSpinBox *fps_ = nullptr;
	QSpinBox *width_ = nullptr;
	QSpinBox *height_ = nullptr;
	QSpinBox *quality_ = nullptr;
	QSpinBox *maxClients_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *url_ = nullptr;
};
