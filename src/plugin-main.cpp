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

void stopPreview()
{
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

	if (!server.start(settings))
		return false;

	capture.setFrameCallback([](RawFrame &&frame) { server.submitFrame(std::move(frame)); });
	if (!capture.start(settings)) {
		server.setLastError(capture.lastError());
		server.stop();
		return false;
	}

	return true;
}

void applySettings(const PreviewSettings &settings)
{
	std::lock_guard lock(stateMutex);
	currentSettings = SettingsStore::clamp(settings);
	settingsStore.save(currentSettings);
	startPreview(currentSettings);
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
