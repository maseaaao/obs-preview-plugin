#pragma once

#include "settings.hpp"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct RawFrame {
	int width = 0;
	int height = 0;
	int quality = 70;
	uint64_t timestamp = 0;
	std::vector<uint8_t> rgba;
};

class ObsFrameCapture {
public:
	using FrameCallback = std::function<void(RawFrame &&frame)>;

	ObsFrameCapture() = default;
	~ObsFrameCapture();

	bool start(const PreviewSettings &settings);
	void stop();
	bool running() const;

	void setFrameCallback(FrameCallback callback);
	std::string lastError() const;

private:
	static void onRawVideo(void *param, video_data *frame);
	void handleRawVideo(video_data *frame);
	static uint32_t frameRateDivisor(int requestedFps);

	mutable std::mutex mutex_;
	FrameCallback callback_;
	std::string lastError_;
	video_scale_info conversion_ = {};
	PreviewSettings settings_;
	std::atomic_bool running_{false};
};
