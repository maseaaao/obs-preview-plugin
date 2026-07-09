#include "settings_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

SettingsDialog::SettingsDialog(PreviewSettings settings, const MjpegHttpServer &server, ApplyCallback apply, QWidget *parent)
	: QDialog(parent),
	  server_(server),
	  apply_(std::move(apply))
{
	settings = SettingsStore::clamp(settings);

	setWindowTitle("LAN Preview");
	resize(420, 280);

	enabled_ = new QCheckBox("Enable LAN preview", this);
	enabled_->setChecked(settings.enabled);

	port_ = new QSpinBox(this);
	port_->setRange(1, 65535);
	port_->setValue(settings.port);

	fps_ = new QSpinBox(this);
	fps_->setRange(1, 30);
	fps_->setValue(settings.fps);
	fps_->setSuffix(" FPS");

	width_ = new QSpinBox(this);
	width_->setRange(64, 4096);
	width_->setValue(settings.width);
	width_->setSuffix(" px");

	height_ = new QSpinBox(this);
	height_->setRange(64, 4096);
	height_->setValue(settings.height);
	height_->setSuffix(" px");

	keepAspect_ = new QCheckBox("Keep aspect ratio", this);
	keepAspect_->setChecked(settings.keepAspect);

	quality_ = new QSpinBox(this);
	quality_->setRange(1, 100);
	quality_->setValue(settings.quality);
	quality_->setSuffix("%");

	maxClients_ = new QSpinBox(this);
	maxClients_->setRange(1, 64);
	maxClients_->setValue(settings.maxClients);

	const double aspect = settings.width > 0 ? static_cast<double>(settings.height) / settings.width : 9.0 / 16.0;
	auto adjustingAspect = std::make_shared<bool>(false);
	connect(width_, qOverload<int>(&QSpinBox::valueChanged), this, [this, aspect, adjustingAspect](int value) {
		if (*adjustingAspect || !keepAspect_->isChecked())
			return;
		*adjustingAspect = true;
		height_->setValue(std::clamp(static_cast<int>(std::lround(value * aspect)), 64, 4096));
		*adjustingAspect = false;
	});
	connect(height_, qOverload<int>(&QSpinBox::valueChanged), this, [this, aspect, adjustingAspect](int value) {
		if (*adjustingAspect || !keepAspect_->isChecked() || aspect <= 0.0)
			return;
		*adjustingAspect = true;
		width_->setValue(std::clamp(static_cast<int>(std::lround(value / aspect)), 64, 4096));
		*adjustingAspect = false;
	});

	status_ = new QLabel(this);
	url_ = new QLabel(this);
	url_->setTextInteractionFlags(Qt::TextSelectableByMouse);

	auto *warning = new QLabel("Warning: LAN preview has no password. Anyone on the same network can view it while enabled.", this);
	warning->setWordWrap(true);

	auto *form = new QFormLayout;
	form->addRow(enabled_);
	form->addRow("Port", port_);
	form->addRow("Frame rate", fps_);
	form->addRow("Width", width_);
	form->addRow("Height", height_);
	form->addRow("", keepAspect_);
	form->addRow("JPEG quality", quality_);
	form->addRow("Max clients", maxClients_);
	form->addRow("Status", status_);
	form->addRow("URL", url_);

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
	settings.fps = fps_->value();
	settings.width = width_->value();
	settings.height = height_->value();
	settings.keepAspect = keepAspect_->isChecked();
	settings.quality = quality_->value();
	settings.maxClients = maxClients_->value();
	return SettingsStore::clamp(settings);
}

void SettingsDialog::refreshStatus()
{
	const auto settings = collect();
	if (server_.running()) {
		status_->setText(QString("Running, %1 client(s)").arg(server_.clientCount()));
		url_->setText(QString::fromStdString(MjpegHttpServer::lanUrl(settings.port)));
	} else {
		const auto error = server_.lastError();
		status_->setText(error.empty() ? "Stopped" : QString("Stopped: %1").arg(QString::fromStdString(error)));
		url_->setText(QString::fromStdString(MjpegHttpServer::lanUrl(settings.port)));
	}
}
