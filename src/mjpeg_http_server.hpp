#pragma once

#include "frame_capture.hpp"
#include "settings.hpp"

#include <WinSock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class MjpegHttpServer {
public:
	~MjpegHttpServer();

	bool start(const PreviewSettings &settings);
	void stop();
	bool running() const;

	void submitFrame(RawFrame &&frame);

	int clientCount() const;
	std::string lastError() const;
	void setLastError(std::string error);
	PreviewSettings settings() const;

	static std::string lanUrl(int port);

private:
	void acceptLoop();
	void clientLoop(SOCKET client);
	void encoderLoop();

	void handleIndex(SOCKET client);
	void handleHealth(SOCKET client);
	void handleSnapshot(SOCKET client);
	void handlePreview(SOCKET client);

	bool waitForJpeg(uint64_t previousGeneration, std::vector<uint8_t> &jpeg, uint64_t &generation);

	static bool sendAll(SOCKET socket, const char *data, size_t size);
	static bool sendAll(SOCKET socket, const std::string &data);
	static std::string parsePath(const std::string &request);

	mutable std::mutex mutex_;
	std::condition_variable frameCv_;
	std::condition_variable rawCv_;

	PreviewSettings settings_;
	std::string lastError_;
	std::vector<uint8_t> latestJpeg_;
	std::optional<RawFrame> pendingRaw_;
	uint64_t generation_ = 0;

	SOCKET listenSocket_ = INVALID_SOCKET;
	std::thread acceptThread_;
	std::thread encoderThread_;
	std::vector<std::thread> clientThreads_;
	mutable std::mutex clientThreadsMutex_;
	std::atomic_bool running_{false};
	std::atomic_int clients_{0};
};
