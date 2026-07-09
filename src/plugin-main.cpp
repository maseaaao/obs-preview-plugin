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

	if (!settings.enabled) {
		server.setLastError({});
		return true;
	}

	configureCaptureCallbacks();
	server.setFrameDemandCallback(handleFrameDemand);
	if (!server.start(settings)) {
		server.setFrameDemandCallback({});
		return false;
	}

	return true;
}

void applySettings(const PreviewSettings &settings)
{
	const auto clamped = SettingsStore::clamp(settings);
	{
		std::lock_guard lock(stateMutex);
		currentSettings = clamped;
	}
	settingsStore.save(clamped);
	startPreview(clamped);
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
