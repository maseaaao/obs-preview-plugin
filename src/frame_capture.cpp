#include "frame_capture.hpp"
#include "capture_pacing.hpp"

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
	conversion_.format = VIDEO_FORMAT_BGR3;
	conversion_.width = static_cast<uint32_t>(settings_.width);
	conversion_.height = static_cast<uint32_t>(settings_.height);
	conversion_.range = VIDEO_RANGE_FULL;
	conversion_.colorspace = VIDEO_CS_SRGB;
	lastAcceptedTimestamp_ = 0;
	hasAcceptedTimestamp_ = false;
	frameIntervalNs_ = 1000000000ULL / static_cast<uint64_t>(settings_.fps);

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

void ObsFrameCapture::setShouldCaptureCallback(ShouldCaptureCallback callback)
{
	std::lock_guard lock(mutex_);
	shouldCapture_ = std::move(callback);
}

void ObsFrameCapture::setBufferCallback(BufferCallback callback)
{
	std::lock_guard lock(mutex_);
	bufferCallback_ = std::move(callback);
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

	ShouldCaptureCallback shouldCapture;
	{
		std::lock_guard lock(mutex_);
		shouldCapture = shouldCapture_;
	}

	if (shouldCapture && !shouldCapture())
		return;
	if (!acceptTimestamp(frame->timestamp))
		return;

	FrameCallback callback;
	BufferCallback bufferCallback;
	{
		std::lock_guard lock(mutex_);
		callback = callback_;
		bufferCallback = bufferCallback_;
	}
	if (!callback)
		return;

	RawFrame raw;
	raw.width = settings_.width;
	raw.height = settings_.height;
	raw.quality = settings_.quality;
	raw.timestamp = frame->timestamp;
	const auto frameBytes = static_cast<size_t>(raw.width) * static_cast<size_t>(raw.height) * 3;
	raw.bgr = bufferCallback ? bufferCallback(frameBytes) : std::vector<uint8_t>();
	raw.bgr.resize(frameBytes);

	const auto rowBytes = static_cast<size_t>(raw.width) * 3;
	const auto sourceStride = static_cast<size_t>(frame->linesize[0]);
	auto *dst = raw.bgr.data();
	const auto *src = frame->data[0];

	for (int y = 0; y < raw.height; ++y)
		std::memcpy(dst + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * sourceStride, rowBytes);

	callback(std::move(raw));
}

uint32_t ObsFrameCapture::frameRateDivisor(int requestedFps)
{
	obs_video_info info = {};
	if (!obs_get_video_info(&info) || info.fps_den == 0)
		return 1;

	const double obsFps = static_cast<double>(info.fps_num) / static_cast<double>(info.fps_den);
	const auto divisor = static_cast<uint32_t>(std::max(1.0, std::floor(obsFps / std::max(1, requestedFps))));
	return divisor;
}

bool ObsFrameCapture::acceptTimestamp(uint64_t timestamp)
{
	return acceptFrameTimestamp(lastAcceptedTimestamp_, hasAcceptedTimestamp_, frameIntervalNs_, timestamp);
}
