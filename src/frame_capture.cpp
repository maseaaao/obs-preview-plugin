#include "frame_capture.hpp"

#include <cstring>
#include <cmath>
#include <utility>

ObsFrameCapture::~ObsFrameCapture()
{
	stop();
}

bool ObsFrameCapture::start(const PreviewSettings &settings)
{
	stop();

	settings_ = SettingsStore::clamp(settings);
	{
		std::lock_guard lock(mutex_);
		lastError_.clear();
	}

	conversion_ = {};
	conversion_.format = VIDEO_FORMAT_RGBA;
	conversion_.width = static_cast<uint32_t>(settings_.width);
	conversion_.height = static_cast<uint32_t>(settings_.height);
	conversion_.range = VIDEO_RANGE_FULL;
	conversion_.colorspace = VIDEO_CS_SRGB;

	if (!obs_get_video()) {
		std::lock_guard lock(mutex_);
		lastError_ = "OBS video output is not active yet.";
		return false;
	}

	obs_add_raw_video_callback2(&conversion_, frameRateDivisor(settings_.fps), onRawVideo, this);
	running_.store(true);
	return true;
}

void ObsFrameCapture::stop()
{
	if (!running_.exchange(false))
		return;

	obs_remove_raw_video_callback(onRawVideo, this);
}

bool ObsFrameCapture::running() const
{
	return running_.load();
}

void ObsFrameCapture::setFrameCallback(FrameCallback callback)
{
	std::lock_guard lock(mutex_);
	callback_ = std::move(callback);
}

std::string ObsFrameCapture::lastError() const
{
	std::lock_guard lock(mutex_);
	return lastError_;
}

void ObsFrameCapture::onRawVideo(void *param, video_data *frame)
{
	static_cast<ObsFrameCapture *>(param)->handleRawVideo(frame);
}

void ObsFrameCapture::handleRawVideo(video_data *frame)
{
	if (!running_.load() || !frame || !frame->data[0])
		return;

	RawFrame raw;
	raw.width = settings_.width;
	raw.height = settings_.height;
	raw.quality = settings_.quality;
	raw.timestamp = frame->timestamp;
	raw.rgba.resize(static_cast<size_t>(raw.width) * static_cast<size_t>(raw.height) * 4);

	const auto rowBytes = static_cast<size_t>(raw.width) * 4;
	const auto sourceStride = static_cast<size_t>(frame->linesize[0]);
	auto *dst = raw.rgba.data();
	const auto *src = frame->data[0];

	for (int y = 0; y < raw.height; ++y)
		std::memcpy(dst + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * sourceStride, rowBytes);

	FrameCallback callback;
	{
		std::lock_guard lock(mutex_);
		callback = callback_;
	}

	if (callback)
		callback(std::move(raw));
}

uint32_t ObsFrameCapture::frameRateDivisor(int requestedFps)
{
	obs_video_info info = {};
	if (!obs_get_video_info(&info) || info.fps_den == 0)
		return 1;

	const double obsFps = static_cast<double>(info.fps_num) / static_cast<double>(info.fps_den);
	const auto divisor = static_cast<uint32_t>(std::max(1.0, std::round(obsFps / std::max(1, requestedFps))));
	return divisor;
}
