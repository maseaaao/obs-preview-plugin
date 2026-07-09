#include "mjpeg_http_server.hpp"

#include "jpeg_encoder.hpp"

#include <WS2tcpip.h>

#include <algorithm>
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
	for (auto &thread : clientThreads_) {
		if (thread.joinable())
			thread.join();
	}
	clientThreads_.clear();

	{
		std::lock_guard stateLock(mutex_);
		pendingRaw_.reset();
		latestJpeg_.clear();
		generation_ = 0;
	}
}

bool MjpegHttpServer::running() const
{
	return running_.load();
}

void MjpegHttpServer::submitFrame(RawFrame &&frame)
{
	if (!running_.load())
		return;

	{
		std::lock_guard lock(mutex_);
		pendingRaw_ = std::move(frame);
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

		std::lock_guard lock(clientThreadsMutex_);
		clientThreads_.emplace_back(&MjpegHttpServer::clientLoop, this, client);
	}
}

void MjpegHttpServer::clientLoop(SOCKET client)
{
	clients_.fetch_add(1);

	char buffer[4096] = {};
	const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
	if (received <= 0) {
		closesocket(client);
		clients_.fetch_sub(1);
		return;
	}

	const auto path = parsePath(std::string(buffer, buffer + received));
	if (path == "/" || path == "/index.html")
		handleIndex(client);
	else if (path == "/preview.mjpg")
		handlePreview(client);
	else if (path == "/snapshot.jpg")
		handleSnapshot(client);
	else if (path == "/health")
		handleHealth(client);
	else {
		const std::string body = "Not found\n";
		sendAll(client, httpHeader(404, "text/plain; charset=utf-8", body.size()));
		sendAll(client, body);
	}

	closesocket(client);
	clients_.fetch_sub(1);
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
		}

		auto jpeg = JpegEncoder::encodeRgba(raw.rgba.data(), raw.width, raw.height, raw.quality);
		if (jpeg.empty())
			continue;

		{
			std::lock_guard lock(mutex_);
			latestJpeg_ = std::move(jpeg);
			++generation_;
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
	std::ostringstream body;
	body << "{\"running\":" << (running_.load() ? "true" : "false") << ",\"fps\":" << settings_.fps
	     << ",\"width\":" << settings_.width << ",\"height\":" << settings_.height
	     << ",\"quality\":" << settings_.quality << ",\"clients\":" << clients_.load() << "}\n";

	const auto text = body.str();
	sendAll(client, httpHeader(200, "application/json", text.size()));
	sendAll(client, text);
}

void MjpegHttpServer::handleSnapshot(SOCKET client)
{
	std::vector<uint8_t> jpeg;
	{
		std::lock_guard lock(mutex_);
		jpeg = latestJpeg_;
	}

	if (jpeg.empty()) {
		const std::string body = "No frame is available yet\n";
		sendAll(client, httpHeader(503, "text/plain; charset=utf-8", body.size()));
		sendAll(client, body);
		return;
	}

	sendAll(client, httpHeader(200, "image/jpeg", jpeg.size()));
	sendAll(client, reinterpret_cast<const char *>(jpeg.data()), jpeg.size());
}

void MjpegHttpServer::handlePreview(SOCKET client)
{
	const std::string header =
		"HTTP/1.1 200 OK\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
		"Pragma: no-cache\r\n"
		"Connection: close\r\n"
		"Content-Type: multipart/x-mixed-replace; boundary=obs-lan-preview\r\n\r\n";
	if (!sendAll(client, header))
		return;

	uint64_t generation = 0;
	while (running_.load()) {
		std::vector<uint8_t> jpeg;
		if (!waitForJpeg(generation, jpeg, generation))
			break;

		std::ostringstream part;
		part << "--obs-lan-preview\r\n"
		     << "Content-Type: image/jpeg\r\n"
		     << "Content-Length: " << jpeg.size() << "\r\n\r\n";
		if (!sendAll(client, part.str()))
			break;
		if (!sendAll(client, reinterpret_cast<const char *>(jpeg.data()), jpeg.size()))
			break;
		if (!sendAll(client, "\r\n", 2))
			break;
	}
}

bool MjpegHttpServer::waitForJpeg(uint64_t previousGeneration, std::vector<uint8_t> &jpeg, uint64_t &generation)
{
	std::unique_lock lock(mutex_);
	frameCv_.wait(lock, [&]() { return !running_.load() || generation_ != previousGeneration; });
	if (!running_.load())
		return false;

	jpeg = latestJpeg_;
	generation = generation_;
	return !jpeg.empty();
}

bool MjpegHttpServer::sendAll(SOCKET socket, const char *data, size_t size)
{
	size_t sent = 0;
	while (sent < size) {
		const int chunk = send(socket, data + sent, static_cast<int>(std::min<size_t>(size - sent, 16 * 1024)), 0);
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
