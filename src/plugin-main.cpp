#include "frame_capture.hpp"
#include "mjpeg_http_server.hpp"
#include "settings.hpp"
#include "settings_dialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QPointer>
#include <QWidget>

#include <mutex>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-lan-preview", "en-US")

namespace {
SettingsStore settingsStore;
PreviewSettings currentSettings;
ObsFrameCapture capture;
MjpegHttpServer server;
std::mutex stateMutex;
QPointer<SettingsDialog> dialog;

PreviewSettings resolveOutputSettings(PreviewSettings settings)
{
	settings = SettingsStore::clamp(settings);
	obs_video_info info = {};
	if (!obs_get_video_info(&info) || info.output_width == 0 || info.output_height == 0)
		return settings;

	if (settings.legacyWidth > 0 && settings.legacyHeight > 0) {
		settings.resolutionScale = SettingsStore::migrateResolutionScale(settings.legacyWidth, settings.legacyHeight,
										       static_cast<int>(info.output_width), static_cast<int>(info.output_height));
		settings.legacyWidth = 0;
		settings.legacyHeight = 0;
	}
	const auto resolution = SettingsStore::scaledResolution(static_cast<int>(info.output_width), static_cast<int>(info.output_height),
									    settings.resolutionScale);
	settings.width = resolution.width;
	settings.height = resolution.height;
	return settings;
}

void configureCaptureCallbacks()
{
	capture.setFrameCallback([](RawFrame &&frame) { server.submitFrame(std::move(frame)); });
	capture.setShouldCaptureCallback([]() { return server.shouldCaptureFrame(); });
	capture.setBufferCallback([](size_t size) { return server.takeRawBuffer(size); });
}

void handleFrameDemand(bool needed)
{
	std::lock_guard lock(stateMutex);
	if (!currentSettings.enabled || !server.running())
		return;

	if (!needed) {
		capture.stop();
		return;
	}

	if (capture.running())
		return;

	configureCaptureCallbacks();
	if (!capture.start(currentSettings))
		server.setLastError(capture.lastError());
}

void stopPreview()
{
	server.setFrameDemandCallback({});
	capture.stop();
	server.stop();
}

bool startPreview(const PreviewSettings &settings)
{
	stopPreview();

	const auto resolved = resolveOutputSettings(settings);
	if (!resolved.enabled) {
		server.setLastError({});
		return true;
	}
	if (resolved.width <= 0 || resolved.height <= 0) {
		server.setLastError("OBS video output is not active yet.");
		return false;
	}

	configureCaptureCallbacks();
	server.setFrameDemandCallback(handleFrameDemand);
	if (!server.start(resolved)) {
		server.setFrameDemandCallback({});
		return false;
	}

	return true;
}

void applySettings(const PreviewSettings &settings)
{
	const auto clamped = SettingsStore::clamp(settings);
	const auto resolved = resolveOutputSettings(clamped);
	{
		std::lock_guard lock(stateMutex);
		currentSettings = resolved;
	}
	settingsStore.save(clamped);
	startPreview(resolved);
}

void showSettingsDialog(void *)
{
	if (dialog) {
		dialog->raise();
		dialog->activateWindow();
		return;
	}

	dialog = new SettingsDialog(currentSettings, server, applySettings,
				    static_cast<QWidget *>(obs_frontend_get_main_window()));
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void frontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		stopPreview();
}
}

bool obs_module_load(void)
{
	char *path = obs_module_config_path("settings.json");
	if (path) {
		settingsStore.setPath(QString::fromUtf8(path));
		bfree(path);
	}

	currentSettings = settingsStore.load();
	if (currentSettings.legacyWidth > 0 && currentSettings.legacyHeight > 0) {
		const auto resolved = resolveOutputSettings(currentSettings);
		if (resolved.legacyWidth == 0 && resolved.legacyHeight == 0) {
			currentSettings.resolutionScale = resolved.resolutionScale;
			currentSettings.legacyWidth = 0;
			currentSettings.legacyHeight = 0;
			settingsStore.save(currentSettings);
		}
	}
	currentSettings = resolveOutputSettings(currentSettings);

	obs_frontend_add_tools_menu_item("LAN Preview", showSettingsDialog, nullptr);
	obs_frontend_add_event_callback(frontendEvent, nullptr);

	if (currentSettings.enabled)
		startPreview(currentSettings);

	blog(LOG_INFO, "[obs-lan-preview] loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontendEvent, nullptr);
	stopPreview();
	blog(LOG_INFO, "[obs-lan-preview] unloaded");
}
