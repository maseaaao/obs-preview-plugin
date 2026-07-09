#include "mjpeg_http_server.hpp"

#include "jpeg_encoder.hpp"

#include <WS2tcpip.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace {
class WsaSession {
public:
	WsaSession()
	{
		WSADATA data = {};
		ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}

	~WsaSession()
	{
		if (ok_)
			WSACleanup();
	}

	bool ok() const { return ok_; }

private:
	bool ok_ = false;
};

class AtomicCounterGuard {
public:
	explicit AtomicCounterGuard(std::atomic_int &counter) : counter_(counter) { counter_.fetch_add(1); }
	~AtomicCounterGuard() { counter_.fetch_sub(1); }

private:
	std::atomic_int &counter_;
};

class ThreadDoneGuard {
public:
	explicit ThreadDoneGuard(std::shared_ptr<std::atomic_bool> done) : done_(std::move(done)) {}
	~ThreadDoneGuard()
	{
		if (done_)
			done_->store(true);
	}

private:
	std::shared_ptr<std::atomic_bool> done_;
};

std::string statusText(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 404:
		return "Not Found";
	case 503:
		return "Service Unavailable";
	default:
		return "OK";
	}
}

std::string httpHeader(int status, const std::string &contentType, size_t length = 0)
{
	std::ostringstream out;
	out << "HTTP/1.1 " << status << ' ' << statusText(status) << "\r\n";
	out << "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n";
	out << "Pragma: no-cache\r\n";
	out << "Connection: close\r\n";
	out << "Content-Type: " << contentType << "\r\n";
	if (length > 0)
		out << "Content-Length: " << length << "\r\n";
	out << "\r\n";
	return out.str();
}
}

MjpegHttpServer::~MjpegHttpServer()
{
	stop();
}

bool MjpegHttpServer::start(const PreviewSettings &settings)
{
	stop();

	static WsaSession wsa;
	if (!wsa.ok()) {
		setLastError("WSAStartup failed.");
		return false;
	}

	settings_ = SettingsStore::clamp(settings);
	setLastError({});
	clients_.store(0);
	streamClients_.store(0);
	snapshotWaiters_.store(0);
	frameDemandActive_.store(false);
	pendingRawReady_.store(false);
	submittedFrames_.store(0);
	encodedFrames_.store(0);
	droppedFrames_.store(0);
	rawBuffersAllocated_.store(0);
	rawBuffersReused_.store(0);
	encodeTimeUs_.store(0);
	maxEncodeTimeUs_.store(0);

	{
		std::lock_guard lock(mutex_);
		pendingRaw_.reset();
		latestJpeg_.reset();
		rawBufferPool_.clear();
		rawBufferPool_.reserve(2);
		generation_ = 0;
	}
	{
		std::lock_guard lock(clientThreadsMutex_);
		clientThreads_.reserve(static_cast<size_t>(settings_.maxClients));
	}
	running_.store(true);

	listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket_ == INVALID_SOCKET) {
		setLastError("Failed to create listen socket.");
		running_.store(false);
		return false;
	}

	BOOL reuse = TRUE;
	setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<u_short>(settings_.port));
	if (inet_pton(AF_INET, settings_.bindAddress.toUtf8().constData(), &address.sin_addr) != 1)
		address.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(listenSocket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
		setLastError("Failed to bind port " + std::to_string(settings_.port) + ". It may already be in use.");
		closesocket(listenSocket_);
		listenSocket_ = INVALID_SOCKET;
		running_.store(false);
		return false;
	}

	if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
		setLastError("Failed to listen on port " + std::to_string(settings_.port) + ".");
		closesocket(listenSocket_);
		listenSocket_ = INVALID_SOCKET;
		running_.store(false);
		return false;
	}

	encoderThread_ = std::thread(&MjpegHttpServer::encoderLoop, this);
	acceptThread_ = std::thread(&MjpegHttpServer::acceptLoop, this);
	return true;
}

void MjpegHttpServer::stop()
{
	if (!running_.exchange(false))
		return;

	if (listenSocket_ != INVALID_SOCKET) {
		shutdown(listenSocket_, SD_BOTH);
		closesocket(listenSocket_);
		listenSocket_ = INVALID_SOCKET;
	}

	frameCv_.notify_all();
	rawCv_.notify_all();

	if (acceptThread_.joinable())
		acceptThread_.join();
	if (encoderThread_.joinable())
		encoderThread_.join();

	std::lock_guard lock(clientThreadsMutex_);
	for (auto &clientThread : clientThreads_) {
		if (clientThread.thread.joinable())
			clientThread.thread.join();
	}
	clientThreads_.clear();

	{
		std::lock_guard stateLock(mutex_);
		pendingRaw_.reset();
		latestJpeg_.reset();
		rawBufferPool_.clear();
		generation_ = 0;
	}
	pendingRawReady_.store(false);
	streamClients_.store(0);
	snapshotWaiters_.store(0);
	frameDemandActive_.store(false);
}

bool MjpegHttpServer::running() const
{
	return running_.load();
}

void MjpegHttpServer::setFrameDemandCallback(FrameDemandCallback callback)
{
	std::lock_guard lock(demandCallbackMutex_);
	frameDemandCallback_ = std::move(callback);
}

bool MjpegHttpServer::shouldCaptureFrame()
{
	if (!running_.load())
		return false;
	if (streamClients_.load() <= 0 && snapshotWaiters_.load() <= 0)
		return false;
	if (pendingRawReady_.load()) {
		droppedFrames_.fetch_add(1);
		return false;
	}
	return true;
}

std::vector<uint8_t> MjpegHttpServer::takeRawBuffer(size_t size)
{
	std::lock_guard lock(mutex_);
	for (auto it = rawBufferPool_.begin(); it != rawBufferPool_.end(); ++it) {
		if (it->capacity() >= size) {
			std::vector<uint8_t> buffer = std::move(*it);
			rawBufferPool_.erase(it);
			rawBuffersReused_.fetch_add(1);
			return buffer;
		}
	}

	rawBuffersAllocated_.fetch_add(1);
	return std::vector<uint8_t>();
}

void MjpegHttpServer::submitFrame(RawFrame &&frame)
{
	if (!running_.load() || (streamClients_.load() <= 0 && snapshotWaiters_.load() <= 0))
		return;

	{
		std::lock_guard lock(mutex_);
		if (pendingRaw_) {
			droppedFrames_.fetch_add(1);
			return;
		}
		pendingRaw_ = std::move(frame);
		pendingRawReady_.store(true);
		submittedFrames_.fetch_add(1);
	}
	rawCv_.notify_one();
}

int MjpegHttpServer::clientCount() const
{
	return clients_.load();
}

std::string MjpegHttpServer::lastError() const
{
	std::lock_guard lock(mutex_);
	return lastError_;
}

void MjpegHttpServer::setLastError(std::string error)
{
	std::lock_guard lock(mutex_);
	lastError_ = std::move(error);
}

PreviewSettings MjpegHttpServer::settings() const
{
	return settings_;
}

std::string MjpegHttpServer::lanUrl(int port)
{
	static WsaSession wsa;
	if (!wsa.ok())
		return "http://127.0.0.1:" + std::to_string(port) + "/";

	char hostname[256] = {};
	if (gethostname(hostname, sizeof(hostname)) != 0)
		return "http://127.0.0.1:" + std::to_string(port) + "/";

	addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo *result = nullptr;
	if (getaddrinfo(hostname, nullptr, &hints, &result) != 0)
		return "http://127.0.0.1:" + std::to_string(port) + "/";

	std::string ip = "127.0.0.1";
	for (auto *addr = result; addr; addr = addr->ai_next) {
		auto *ipv4 = reinterpret_cast<sockaddr_in *>(addr->ai_addr);
		char buffer[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer));
		if (std::string(buffer).rfind("127.", 0) != 0) {
			ip = buffer;
			break;
		}
	}

	freeaddrinfo(result);
	return "http://" + ip + ":" + std::to_string(port) + "/";
}

void MjpegHttpServer::acceptLoop()
{
	while (running_.load()) {
		SOCKET client = accept(listenSocket_, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (running_.load())
				setLastError("Accept failed.");
			break;
		}

		if (clients_.load() >= settings_.maxClients) {
			const std::string body = "Too many clients\n";
			sendAll(client, httpHeader(503, "text/plain; charset=utf-8", body.size()));
			sendAll(client, body);
			closesocket(client);
			continue;
		}

		DWORD timeoutMs = 5000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));

		char buffer[4096] = {};
		const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
		if (received <= 0) {
			closesocket(client);
			continue;
		}

		const auto path = parsePath(std::string(buffer, buffer + received));
		if (path == "/" || path == "/index.html") {
			AtomicCounterGuard clientGuard(clients_);
			handleIndex(client);
			closesocket(client);
			continue;
		}
		if (path == "/health") {
			AtomicCounterGuard clientGuard(clients_);
			handleHealth(client);
			closesocket(client);
			continue;
		}
		if (path != "/preview.mjpg" && path != "/snapshot.jpg") {
			AtomicCounterGuard clientGuard(clients_);
			handleNotFound(client);
			closesocket(client);
			continue;
		}

		reapClientThreads();
		auto done = std::make_shared<std::atomic_bool>(false);
		std::lock_guard lock(clientThreadsMutex_);
		clientThreads_.push_back({std::thread(&MjpegHttpServer::clientLoop, this, client, path, done), done});
	}
}

void MjpegHttpServer::clientLoop(SOCKET client, std::string path, std::shared_ptr<std::atomic_bool> done)
{
	ThreadDoneGuard doneGuard(std::move(done));
	AtomicCounterGuard clientGuard(clients_);

	if (path == "/" || path == "/index.html")
		handleIndex(client);
	else if (path == "/preview.mjpg")
		handlePreview(client);
	else if (path == "/snapshot.jpg")
		handleSnapshot(client);
	else if (path == "/health")
		handleHealth(client);
	else
		handleNotFound(client);

	closesocket(client);
}

void MjpegHttpServer::encoderLoop()
{
	while (running_.load()) {
		RawFrame raw;
		{
			std::unique_lock lock(mutex_);
			rawCv_.wait(lock, [&]() { return !running_.load() || pendingRaw_.has_value(); });
			if (!running_.load())
				break;
			raw = std::move(*pendingRaw_);
			pendingRaw_.reset();
			pendingRawReady_.store(false);
		}

		if (streamClients_.load() <= 0 && snapshotWaiters_.load() <= 0) {
			droppedFrames_.fetch_add(1);
			recycleRawBuffer(std::move(raw.bgr));
			continue;
		}

		const auto encodeStart = std::chrono::steady_clock::now();
		auto encoded = JpegEncoder::encodeBgr(raw.bgr.data(), raw.width, raw.height, raw.quality);
		const auto encodeUs = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - encodeStart)
				.count());
		encodeTimeUs_.fetch_add(encodeUs);

		auto previousMax = maxEncodeTimeUs_.load();
		while (encodeUs > previousMax && !maxEncodeTimeUs_.compare_exchange_weak(previousMax, encodeUs)) {
		}

		if (encoded.empty()) {
			recycleRawBuffer(std::move(raw.bgr));
			continue;
		}

		JpegFrame jpeg = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
		recycleRawBuffer(std::move(raw.bgr));
		{
			std::lock_guard lock(mutex_);
			latestJpeg_ = std::move(jpeg);
			++generation_;
			encodedFrames_.fetch_add(1);
		}
		frameCv_.notify_all();
	}
}

void MjpegHttpServer::handleIndex(SOCKET client)
{
	const std::string body =
		"<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<title>OBS LAN Preview</title><style>body{margin:0;background:#111;color:#eee;font:14px system-ui}"
		"main{min-height:100vh;display:grid;place-items:center}img{display:block;width:100vw;height:100vh;object-fit:contain;object-position:center}</style></head>"
		"<body><main><img src=\"/preview.mjpg\" alt=\"OBS LAN Preview\"></main></body></html>";
	sendAll(client, httpHeader(200, "text/html; charset=utf-8", body.size()));
	sendAll(client, body);
}

void MjpegHttpServer::handleHealth(SOCKET client)
{
	const auto encodedFrames = encodedFrames_.load();
	const auto totalEncodeUs = encodeTimeUs_.load();
	size_t latestJpegBytes = 0;
	{
		std::lock_guard lock(mutex_);
		if (latestJpeg_)
			latestJpegBytes = latestJpeg_->size();
	}

	std::ostringstream body;
	body << "{\"running\":" << (running_.load() ? "true" : "false") << ",\"fps\":" << settings_.fps
	     << ",\"width\":" << settings_.width << ",\"height\":" << settings_.height
	     << ",\"quality\":" << settings_.quality << ",\"clients\":" << clients_.load()
	     << ",\"streamClients\":" << streamClients_.load() << ",\"snapshotWaiters\":" << snapshotWaiters_.load()
	     << ",\"frameDemand\":" << (frameDemandActive_.load() ? "true" : "false")
	     << ",\"submittedFrames\":" << submittedFrames_.load() << ",\"encodedFrames\":" << encodedFrames
	     << ",\"droppedFrames\":" << droppedFrames_.load() << ",\"latestJpegBytes\":" << latestJpegBytes
	     << ",\"rawBuffersAllocated\":" << rawBuffersAllocated_.load()
	     << ",\"rawBuffersReused\":" << rawBuffersReused_.load()
	     << ",\"avgEncodeMs\":"
	     << (encodedFrames > 0 ? static_cast<double>(totalEncodeUs) / static_cast<double>(encodedFrames) / 1000.0
				   : 0.0)
	     << ",\"maxEncodeMs\":" << static_cast<double>(maxEncodeTimeUs_.load()) / 1000.0 << "}\n";

	const auto text = body.str();
	sendAll(client, httpHeader(200, "application/json", text.size()));
	sendAll(client, text);
}

void MjpegHttpServer::handleNotFound(SOCKET client)
{
	const std::string body = "Not found\n";
	sendAll(client, httpHeader(404, "text/plain; charset=utf-8", body.size()));
	sendAll(client, body);
}

void MjpegHttpServer::handleSnapshot(SOCKET client)
{
	JpegFrame jpeg;
	uint64_t generation = 0;
	{
		std::lock_guard lock(mutex_);
		jpeg = latestJpeg_;
		generation = generation_;
	}

	if (streamClients_.load() <= 0) {
		setSnapshotWaiterActive(true);
		JpegFrame freshJpeg;
		uint64_t freshGeneration = generation;
		const bool fresh = waitForJpeg(generation, freshJpeg, freshGeneration, 2000);
		setSnapshotWaiterActive(false);
		if (fresh)
			jpeg = std::move(freshJpeg);
	}

	if (!jpeg || jpeg->empty()) {
		const std::string body = "No frame is available yet\n";
		sendAll(client, httpHeader(503, "text/plain; charset=utf-8", body.size()));
		sendAll(client, body);
		return;
	}

	sendAll(client, httpHeader(200, "image/jpeg", jpeg->size()));
	sendAll(client, reinterpret_cast<const char *>(jpeg->data()), jpeg->size());
}

void MjpegHttpServer::handlePreview(SOCKET client)
{
	setStreamClientActive(true);
	const std::string header =
		"HTTP/1.1 200 OK\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
		"Pragma: no-cache\r\n"
		"Connection: close\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=obs-lan-preview\r\n\r\n";
	if (!sendAll(client, header)) {
		setStreamClientActive(false);
		return;
	}

	uint64_t generation = 0;
	while (running_.load()) {
		JpegFrame jpeg;
		if (!waitForJpeg(generation, jpeg, generation))
			break;

		std::ostringstream part;
		part << "--obs-lan-preview\r\n"
		     << "Content-Type: image/jpeg\r\n"
		     << "Content-Length: " << jpeg->size() << "\r\n\r\n";
		if (!sendAll(client, part.str()))
			break;
		if (!sendAll(client, reinterpret_cast<const char *>(jpeg->data()), jpeg->size()))
			break;
		if (!sendAll(client, "\r\n", 2))
			break;
	}
	setStreamClientActive(false);
}

bool MjpegHttpServer::waitForJpeg(uint64_t previousGeneration, JpegFrame &jpeg, uint64_t &generation, int timeoutMs)
{
	std::unique_lock lock(mutex_);
	const auto ready = [&]() { return !running_.load() || generation_ != previousGeneration; };
	if (timeoutMs > 0) {
		if (!frameCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready))
			return false;
	} else {
		frameCv_.wait(lock, ready);
	}
	if (!running_.load())
		return false;

	jpeg = latestJpeg_;
	generation = generation_;
	return jpeg && !jpeg->empty();
}

void MjpegHttpServer::reapClientThreads()
{
	std::lock_guard lock(clientThreadsMutex_);
	for (auto it = clientThreads_.begin(); it != clientThreads_.end();) {
		if (it->done && it->done->load()) {
			if (it->thread.joinable())
				it->thread.join();
			it = clientThreads_.erase(it);
		} else {
			++it;
		}
	}
}

void MjpegHttpServer::recycleRawBuffer(std::vector<uint8_t> &&buffer)
{
	if (buffer.empty())
		return;

	buffer.clear();
	std::lock_guard lock(mutex_);
	if (rawBufferPool_.size() < 2)
		rawBufferPool_.push_back(std::move(buffer));
}

void MjpegHttpServer::setStreamClientActive(bool active)
{
	if (active)
		streamClients_.fetch_add(1);
	else
		streamClients_.fetch_sub(1);
	updateFrameDemand();
}

void MjpegHttpServer::setSnapshotWaiterActive(bool active)
{
	if (active)
		snapshotWaiters_.fetch_add(1);
	else
		snapshotWaiters_.fetch_sub(1);
	updateFrameDemand();
}

void MjpegHttpServer::updateFrameDemand()
{
	const bool needed = running_.load() && (streamClients_.load() > 0 || snapshotWaiters_.load() > 0);
	if (frameDemandActive_.exchange(needed) == needed)
		return;

	FrameDemandCallback callback;
	{
		std::lock_guard lock(demandCallbackMutex_);
		callback = frameDemandCallback_;
	}
	if (callback)
		callback(needed);
}

bool MjpegHttpServer::sendAll(SOCKET socket, const char *data, size_t size)
{
	constexpr size_t sendChunkSize = 64 * 1024;
	size_t sent = 0;
	while (sent < size) {
		const int chunk = send(socket, data + sent, static_cast<int>(std::min<size_t>(size - sent, sendChunkSize)), 0);
		if (chunk <= 0)
			return false;
		sent += static_cast<size_t>(chunk);
	}
	return true;
}

bool MjpegHttpServer::sendAll(SOCKET socket, const std::string &data)
{
	return sendAll(socket, data.data(), data.size());
}

std::string MjpegHttpServer::parsePath(const std::string &request)
{
	const auto firstSpace = request.find(' ');
	if (firstSpace == std::string::npos)
		return "/";
	const auto secondSpace = request.find(' ', firstSpace + 1);
	if (secondSpace == std::string::npos)
		return "/";
	auto path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
	const auto query = path.find('?');
	if (query != std::string::npos)
		path.resize(query);
	return path.empty() ? "/" : path;
}
