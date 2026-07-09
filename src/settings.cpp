#include "settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace {
constexpr int minDimension = 64;
constexpr int maxDimension = 4096;
}

void SettingsStore::setPath(QString path)
{
	path_ = std::move(path);
}

PreviewSettings SettingsStore::load() const
{
	PreviewSettings settings;

	QFile file(path_);
	if (!file.open(QIODevice::ReadOnly))
		return settings;

	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject())
		return settings;

	const auto obj = doc.object();
	settings.enabled = obj.value("enabled").toBool(settings.enabled);
	settings.bindAddress = obj.value("bindAddress").toString(settings.bindAddress);
	settings.port = obj.value("port").toInt(settings.port);
	settings.fps = obj.value("fps").toInt(settings.fps);
	settings.width = obj.value("width").toInt(settings.width);
	settings.height = obj.value("height").toInt(settings.height);
	settings.keepAspect = obj.value("keepAspect").toBool(settings.keepAspect);
	settings.quality = obj.value("quality").toInt(settings.quality);
	settings.maxClients = obj.value("maxClients").toInt(settings.maxClients);

	return clamp(settings);
}

bool SettingsStore::save(const PreviewSettings &input) const
{
	const auto settings = clamp(input);
	const QFileInfo info(path_);
	QDir().mkpath(info.absolutePath());

	QJsonObject obj;
	obj["enabled"] = settings.enabled;
	obj["bindAddress"] = settings.bindAddress;
	obj["port"] = settings.port;
	obj["fps"] = settings.fps;
	obj["width"] = settings.width;
	obj["height"] = settings.height;
	obj["keepAspect"] = settings.keepAspect;
	obj["quality"] = settings.quality;
	obj["maxClients"] = settings.maxClients;

	QFile file(path_);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;

	file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
	return true;
}

PreviewSettings SettingsStore::clamp(PreviewSettings settings)
{
	settings.port = std::clamp(settings.port, 1, 65535);
	settings.fps = std::clamp(settings.fps, 1, 30);
	settings.width = std::clamp(settings.width, minDimension, maxDimension);
	settings.height = std::clamp(settings.height, minDimension, maxDimension);
	settings.quality = std::clamp(settings.quality, 1, 100);
	settings.maxClients = std::clamp(settings.maxClients, 1, 64);

	if (settings.bindAddress.trimmed().isEmpty())
		settings.bindAddress = "0.0.0.0";

	return settings;
}
