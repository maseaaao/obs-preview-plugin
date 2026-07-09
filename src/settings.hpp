#pragma once

#include <QString>

struct PreviewSettings {
	bool enabled = false;
	QString bindAddress = "0.0.0.0";
	int port = 9181;
	int fps = 5;
	int width = 640;
	int height = 360;
	bool keepAspect = true;
	int quality = 70;
	int maxClients = 8;
};

class SettingsStore {
public:
	void setPath(QString path);

	PreviewSettings load() const;
	bool save(const PreviewSettings &settings) const;

	static PreviewSettings clamp(PreviewSettings settings);

private:
	QString path_;
};
