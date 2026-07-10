#pragma once

#include "mjpeg_http_server.hpp"
#include "settings.hpp"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QPushButton;

class SettingsDialog : public QDialog {
public:
	using ApplyCallback = std::function<void(const PreviewSettings &settings)>;

	SettingsDialog(PreviewSettings settings, const MjpegHttpServer &server, ApplyCallback apply, QWidget *parent = nullptr);

private:
	PreviewSettings collect() const;
	QString lanUrlForPort(int port, bool tls) const;
	void copyUrl(const QString &url, const QString &label);
	void refreshResolution();
	void refreshStatus();

	const MjpegHttpServer &server_;
	ApplyCallback apply_;

	QCheckBox *enabled_ = nullptr;
	QSpinBox *port_ = nullptr;
	QSpinBox *httpPort_ = nullptr;
	QComboBox *fps_ = nullptr;
	QComboBox *scale_ = nullptr;
	QSpinBox *quality_ = nullptr;
	QSpinBox *maxClients_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *resolution_ = nullptr;
	QLabel *fingerprint_ = nullptr;
	QPushButton *url_ = nullptr;
	QPushButton *awakeUrl_ = nullptr;
	QPushButton *httpUrl_ = nullptr;
	QPushButton *exportCertificate_ = nullptr;
};
