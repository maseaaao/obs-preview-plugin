#pragma once

#include <QString>

#include <array>

struct PreviewResolution {
	int width = 0;
	int height = 0;
};

struct PreviewSettings {
	bool enabled = false;
	QString bindAddress = "0.0.0.0";
	int port = 9181;
	int fps = 5;
	int resolutionScale = 33;
	// Filled from the active OBS output before capture begins.
	int width = 0;
	int height = 0;
	// Used only to migrate pre-scale settings.
	int legacyWidth = 0;
	int legacyHeight = 0;
	int quality = 70;
	int maxClients = 8;
};

class SettingsStore {
public:
	void setPath(QString path);

	PreviewSettings load() const;
	bool save(const PreviewSettings &settings) const;

	static PreviewSettings clamp(PreviewSettings settings);
	static int normalizeFps(int value);
	static int normalizeResolutionScale(int value);
	static PreviewResolution scaledResolution(int sourceWidth, int sourceHeight, int scalePercent);
	static int migrateResolutionScale(int legacyWidth, int legacyHeight, int sourceWidth, int sourceHeight);

	static constexpr std::array<int, 14> frameRates = {1, 2, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60};
	static constexpr std::array<int, 6> resolutionScales = {10, 25, 33, 50, 75, 100};

private:
	QString path_;
};
