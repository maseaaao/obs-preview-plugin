#include "settings_dialog.hpp"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include <obs.h>

SettingsDialog::SettingsDialog(PreviewSettings settings, const MjpegHttpServer &server, ApplyCallback apply, QWidget *parent)
	: QDialog(parent),
	  server_(server),
	  apply_(std::move(apply))
{
	settings = SettingsStore::clamp(settings);

	setWindowTitle("LAN Preview");
	resize(520, 330);

	enabled_ = new QCheckBox("Enable LAN preview", this);
	enabled_->setChecked(settings.enabled);

	port_ = new QSpinBox(this);
	port_->setRange(1, 65535);
	port_->setValue(settings.port);

	fps_ = new QComboBox(this);
	for (const auto fps : SettingsStore::frameRates)
		fps_->addItem(QString::number(fps) + " FPS", fps);
	fps_->setCurrentIndex(fps_->findData(settings.fps));

	scale_ = new QComboBox(this);
	for (const auto scale : SettingsStore::resolutionScales)
		scale_->addItem(QString::number(scale) + "%", scale);
	scale_->setCurrentIndex(scale_->findData(settings.resolutionScale));

	quality_ = new QSpinBox(this);
	quality_->setRange(1, 100);
	quality_->setValue(settings.quality);
	quality_->setSuffix("%");

	maxClients_ = new QSpinBox(this);
	maxClients_->setRange(1, 64);
	maxClients_->setValue(settings.maxClients);

	connect(scale_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { refreshResolution(); });

	status_ = new QLabel(this);
	resolution_ = new QLabel(this);
	fingerprint_ = new QLabel(this);
	fingerprint_->setWordWrap(true);
	url_ = new QPushButton(this);
	awakeUrl_ = new QPushButton(this);
	exportCertificate_ = new QPushButton("Export trusted-device certificate…", this);
	for (auto *button : {url_, awakeUrl_}) {
		button->setFlat(true);
		button->setCursor(Qt::PointingHandCursor);
		button->setStyleSheet("QPushButton { color: palette(link); text-align: left; padding: 0; }");
	}
	connect(url_, &QPushButton::clicked, this, [this]() { copyUrl(url_->text(), "Preview URL copied"); });
	connect(awakeUrl_, &QPushButton::clicked, this, [this]() { copyUrl(awakeUrl_->text(), "Stay-awake URL copied"); });
	connect(exportCertificate_, &QPushButton::clicked, this, [this]() {
		const auto path = QFileDialog::getSaveFileName(this, "Export OBS LAN Preview CA", "obs-lan-preview-ca.pem",
								       "Certificate (*.pem *.cer)");
		if (path.isEmpty())
			return;
		QString error;
		if (!server_.exportTrustedCertificate(path, error)) {
			status_->setText(error);
			return;
		}
		status_->setText("Certificate exported. Install and trust it on the device before opening the HTTPS URL.");
	});

	auto *warning = new QLabel("Warning: LAN preview has no password. Anyone on the same network can view it while enabled.", this);
	warning->setWordWrap(true);

	auto *form = new QFormLayout;
	form->addRow(enabled_);
	form->addRow("Port", port_);
	form->addRow("Frame rate", fps_);
	form->addRow("Output scale", scale_);
	form->addRow("Resulting resolution", resolution_);
	form->addRow("JPEG quality", quality_);
	form->addRow("Max clients", maxClients_);
	form->addRow("Status", status_);
	form->addRow("Preview URL (click to copy)", url_);
	form->addRow("Stay-awake URL (click to copy)", awakeUrl_);
	form->addRow("Trusted-device CA", exportCertificate_);
	form->addRow("CA SHA-256", fingerprint_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
	connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() {
		if (apply_)
			apply_(collect());
		refreshStatus();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(warning);
	layout->addWidget(buttons);

	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this]() { refreshStatus(); });
	timer->start(1000);
	refreshStatus();
}

PreviewSettings SettingsDialog::collect() const
{
	PreviewSettings settings;
	settings.enabled = enabled_->isChecked();
	settings.bindAddress = "0.0.0.0";
	settings.port = port_->value();
	settings.fps = fps_->currentData().toInt();
	settings.resolutionScale = scale_->currentData().toInt();
	settings.quality = quality_->value();
	settings.maxClients = maxClients_->value();
	return SettingsStore::clamp(settings);
}

QString SettingsDialog::lanUrlForPort(int port)
{
	if (cachedUrlPort_ != port || cachedUrl_.isEmpty()) {
		cachedUrlPort_ = port;
		cachedUrl_ = QString::fromStdString(MjpegHttpServer::lanUrl(port));
	}
	return cachedUrl_;
}

void SettingsDialog::copyUrl(const QString &url, const QString &label)
{
	QGuiApplication::clipboard()->setText(url);
	status_->setText(label);
}

void SettingsDialog::refreshResolution()
{
	obs_video_info info = {};
	if (!obs_get_video_info(&info) || info.output_width == 0 || info.output_height == 0) {
		resolution_->setText("OBS video output is not active");
		return;
	}

	const auto output = SettingsStore::scaledResolution(static_cast<int>(info.output_width), static_cast<int>(info.output_height),
							      scale_->currentData().toInt());
	QString text = QString("%1 × %2 (from %3 × %4)")
			       .arg(output.width)
			       .arg(output.height)
			       .arg(info.output_width)
			       .arg(info.output_height);
	if (scale_->currentData().toInt() == 100 && (info.output_width >= 2560 || info.output_height >= 1440))
		text += " — high CPU and memory load";
	resolution_->setText(text);
}

void SettingsDialog::refreshStatus()
{
	const auto settings = collect();
	refreshResolution();
	const auto previewUrl = lanUrlForPort(settings.port);
	url_->setText(previewUrl);
	awakeUrl_->setText(previewUrl + "?stay-awake=1");
	fingerprint_->setText(server_.certificateFingerprint().isEmpty() ? "Enable and apply preview to create the local CA." :
				      server_.certificateFingerprint());
	if (server_.running()) {
		status_->setText(QString("Running, %1 client(s)").arg(server_.clientCount()));
	} else {
		const auto error = server_.lastError();
		status_->setText(error.empty() ? "Stopped" : QString("Stopped: %1").arg(QString::fromStdString(error)));
	}
}
