#include "settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <utility>

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
	if (obj.contains("resolutionScale")) {
		settings.resolutionScale = obj.value("resolutionScale").toInt(settings.resolutionScale);
	} else {
		settings.legacyWidth = obj.value("width").toInt();
		settings.legacyHeight = obj.value("height").toInt();
	}
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
	obj["resolutionScale"] = settings.resolutionScale;
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
	settings.fps = normalizeFps(settings.fps);
	settings.resolutionScale = normalizeResolutionScale(settings.resolutionScale);
	settings.quality = std::clamp(settings.quality, 1, 100);
	settings.maxClients = std::clamp(settings.maxClients, 1, 64);

	if (settings.bindAddress.trimmed().isEmpty())
		settings.bindAddress = "0.0.0.0";

	return settings;
}

int SettingsStore::normalizeFps(int value)
{
	int normalized = frameRates.front();
	for (const auto candidate : frameRates) {
		if (candidate > value)
			break;
		normalized = candidate;
	}
	return normalized;
}

int SettingsStore::normalizeResolutionScale(int value)
{
	int normalized = resolutionScales.front();
	for (const auto candidate : resolutionScales) {
		if (candidate > value)
			break;
		normalized = candidate;
	}
	return normalized;
}

PreviewResolution SettingsStore::scaledResolution(int sourceWidth, int sourceHeight, int scalePercent)
{
	if (sourceWidth <= 0 || sourceHeight <= 0)
		return {};

	const auto scale = static_cast<double>(normalizeResolutionScale(scalePercent)) / 100.0;
	int width = std::max(2, static_cast<int>(std::lround(static_cast<double>(sourceWidth) * scale)));
	if (width > 2 && (width & 1))
		--width;
	int height = std::max(2, static_cast<int>(std::lround(static_cast<double>(width) * sourceHeight / sourceWidth)));
	if (height > 2 && (height & 1))
		--height;
	return {width, height};
}

int SettingsStore::migrateResolutionScale(int legacyWidth, int legacyHeight, int sourceWidth, int sourceHeight)
{
	if (legacyWidth <= 0 || legacyHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0)
		return 33;

	const auto ratio = std::min(static_cast<double>(legacyWidth) / sourceWidth,
				    static_cast<double>(legacyHeight) / sourceHeight) *
			   100.0;
	return normalizeResolutionScale(static_cast<int>(std::floor(ratio)));
}
