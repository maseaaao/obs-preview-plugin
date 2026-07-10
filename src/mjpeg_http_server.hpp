#pragma once

#include "frame_capture.hpp"
#include "settings.hpp"
#include "tls_certificate.hpp"
#include "tls_connection.hpp"

#include <WinSock2.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class MjpegHttpServer {
public:
	using FrameDemandCallback = std::function<void(bool needed)>;

	~MjpegHttpServer();

	bool start(const PreviewSettings &settings);
	void stop();
	bool running() const;

	void setFrameDemandCallback(FrameDemandCallback callback);
	bool shouldCaptureFrame();
	std::vector<uint8_t> takeRawBuffer(size_t size);
	void submitFrame(RawFrame &&frame);

	int clientCount() const;
	std::string lastError() const;
	void setLastError(std::string error);
	PreviewSettings settings() const;
	bool exportTrustedCertificate(const QString &path, QString &error) const;
	QString certificateFingerprint() const;

	static std::string lanUrl(int port);

private:
	void acceptLoop();
	void clientLoop(SOCKET client, std::shared_ptr<std::atomic_bool> done);
	void encoderLoop();

	void handleIndex(TlsConnection &client, bool stayAwake);
	void handleManifest(TlsConnection &client, bool stayAwake);
	void handleServiceWorker(TlsConnection &client);
	void handleIcon(TlsConnection &client, int size);
	void handleHealth(TlsConnection &client);
	void handleSnapshot(TlsConnection &client);
	void handlePreview(TlsConnection &client);
	void handleNotFound(TlsConnection &client);

	using JpegFrame = std::shared_ptr<const std::vector<uint8_t>>;

	bool waitForJpeg(uint64_t previousGeneration, JpegFrame &jpeg, uint64_t &generation, int timeoutMs = 0);
	void reapClientThreads();
	void recycleRawBuffer(std::vector<uint8_t> &&buffer);
	void setStreamClientActive(bool active);
	void setSnapshotWaiterActive(bool active);
	void updateFrameDemand();

	static bool sendAll(TlsConnection &socket, const char *data, size_t size);
	static bool sendAll(TlsConnection &socket, const std::string &data);
	static std::string parsePath(const std::string &request);

	struct ClientThread {
		std::thread thread;
		std::shared_ptr<std::atomic_bool> done;
	};

	mutable std::mutex mutex_;
	std::condition_variable frameCv_;
	std::condition_variable rawCv_;
	mutable std::mutex demandCallbackMutex_;
	FrameDemandCallback frameDemandCallback_;

	PreviewSettings settings_;
	std::string lastError_;
	JpegFrame latestJpeg_;
	std::optional<RawFrame> pendingRaw_;
	std::vector<std::vector<uint8_t>> rawBufferPool_;
	uint64_t generation_ = 0;

	SOCKET listenSocket_ = INVALID_SOCKET;
	LocalCertificateAuthority certificateAuthority_;
	std::thread acceptThread_;
	std::thread encoderThread_;
	std::vector<ClientThread> clientThreads_;
	mutable std::mutex clientThreadsMutex_;
	std::atomic_bool running_{false};
	std::atomic_bool pendingRawReady_{false};
	std::atomic_bool frameDemandActive_{false};
	std::atomic_int clients_{0};
	std::atomic_int streamClients_{0};
	std::atomic_int snapshotWaiters_{0};
	std::atomic_uint64_t submittedFrames_{0};
	std::atomic_uint64_t encodedFrames_{0};
	std::atomic_uint64_t droppedFrames_{0};
	std::atomic_uint64_t rawBuffersAllocated_{0};
	std::atomic_uint64_t rawBuffersReused_{0};
	std::atomic_uint64_t encodeTimeUs_{0};
	std::atomic_uint64_t maxEncodeTimeUs_{0};
};
