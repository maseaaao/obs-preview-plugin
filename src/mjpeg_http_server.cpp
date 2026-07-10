#include "mjpeg_http_server.hpp"

#include "jpeg_encoder.hpp"

#include <WS2tcpip.h>
#include <Iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

class HttpConnection {
public:
	~HttpConnection()
	{
		if (!tls_ && socket_ != INVALID_SOCKET) {
			shutdown(socket_, SD_BOTH);
			closesocket(socket_);
		}
	}

	bool open(SOCKET socket, bool tls, PCCERT_CONTEXT certificate)
	{
		tls_ = tls;
		if (tls_)
			return tlsConnection_.accept(socket, certificate);
		socket_ = socket;
		return true;
	}

	int receive(char *buffer, int size)
	{
		return tls_ ? tlsConnection_.receive(buffer, size) : recv(socket_, buffer, size, 0);
	}

	bool sendAll(const char *data, size_t size)
	{
		if (tls_)
			return tlsConnection_.sendAll(data, size);
		while (size > 0) {
			const auto sent = send(socket_, data, static_cast<int>(std::min<size_t>(size, 64 * 1024)), 0);
			if (sent <= 0)
				return false;
			data += sent;
			size -= static_cast<size_t>(sent);
		}
		return true;
	}

private:
	bool tls_ = false;
	SOCKET socket_ = INVALID_SOCKET;
	TlsConnection tlsConnection_;
};

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

class AtomicDecrementGuard {
public:
	explicit AtomicDecrementGuard(std::atomic_int &counter) : counter_(counter) {}
	~AtomicDecrementGuard() { counter_.fetch_sub(1); }

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

std::string localHostname()
{
	char hostname[256] = {};
	return gethostname(hostname, sizeof(hostname)) == 0 ? hostname : "localhost";
}

std::vector<std::string> localIpv4Addresses()
{
	std::vector<std::string> addresses;
	ULONG bufferSize = 15 * 1024;
	std::vector<BYTE> buffer(bufferSize);
	auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
	DWORD status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
						      nullptr, adapters, &bufferSize);
	if (status == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(bufferSize);
		adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
		status = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
						     nullptr, adapters, &bufferSize);
	}
	if (status == NO_ERROR) {
		for (auto *adapter = adapters; adapter; adapter = adapter->Next) {
			if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			for (auto *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
				if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET)
					continue;
				const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(unicast->Address.lpSockaddr);
				char text[INET_ADDRSTRLEN] = {};
				if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) && std::string(text).rfind("127.", 0) != 0)
					addresses.emplace_back(text);
			}
		}
	}
	std::sort(addresses.begin(), addresses.end());
	addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
	if (addresses.empty())
		addresses.emplace_back("127.0.0.1");
	return addresses;
}

std::string localIpv4()
{
	return localIpv4Addresses().front();
}

void appendU32(std::vector<uint8_t> &out, uint32_t value)
{
	out.push_back(static_cast<uint8_t>(value >> 24));
	out.push_back(static_cast<uint8_t>(value >> 16));
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value));
}

uint32_t crc32(const uint8_t *data, size_t size)
{
	uint32_t crc = 0xffffffffU;
	while (size--) {
		crc ^= *data++;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320U & static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1))));
	}
	return ~crc;
}

uint32_t adler32(const std::vector<uint8_t> &data)
{
	uint32_t a = 1;
	uint32_t b = 0;
	for (const auto byte : data) {
		a = (a + byte) % 65521U;
		b = (b + a) % 65521U;
	}
	return (b << 16) | a;
}

void appendPngChunk(std::vector<uint8_t> &png, const char type[4], const std::vector<uint8_t> &data)
{
	appendU32(png, static_cast<uint32_t>(data.size()));
	const auto start = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), data.begin(), data.end());
	appendU32(png, crc32(png.data() + start, 4 + data.size()));
}

std::vector<uint8_t> makeIconPng(int size)
{
	std::vector<uint8_t> raw;
	raw.reserve(static_cast<size_t>(size) * (static_cast<size_t>(size) * 4 + 1));
	for (int y = 0; y < size; ++y) {
		raw.push_back(0);
		for (int x = 0; x < size; ++x) {
			const bool camera = x > size * 20 / 100 && x < size * 72 / 100 && y > size * 32 / 100 && y < size * 68 / 100;
			const bool lens = (x - size / 2) * (x - size / 2) + (y - size / 2) * (y - size / 2) < (size / 9) * (size / 9);
			raw.push_back(camera && !lens ? 100 : 17);
			raw.push_back(camera && !lens ? 210 : 17);
			raw.push_back(camera && !lens ? 255 : 17);
			raw.push_back(255);
		}
	}

	std::vector<uint8_t> compressed = {0x78, 0x01};
	for (size_t offset = 0; offset < raw.size();) {
		const auto chunk = static_cast<uint16_t>(std::min<size_t>(65535, raw.size() - offset));
		compressed.push_back(offset + chunk == raw.size() ? 0x01 : 0x00);
		compressed.push_back(static_cast<uint8_t>(chunk));
		compressed.push_back(static_cast<uint8_t>(chunk >> 8));
		const auto complement = static_cast<uint16_t>(~chunk);
		compressed.push_back(static_cast<uint8_t>(complement));
		compressed.push_back(static_cast<uint8_t>(complement >> 8));
		compressed.insert(compressed.end(), raw.begin() + static_cast<ptrdiff_t>(offset), raw.begin() + static_cast<ptrdiff_t>(offset + chunk));
		offset += chunk;
	}
	appendU32(compressed, adler32(raw));

	std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
	std::vector<uint8_t> header;
	appendU32(header, static_cast<uint32_t>(size));
	appendU32(header, static_cast<uint32_t>(size));
	header.insert(header.end(), {8, 6, 0, 0, 0});
	appendPngChunk(png, "IHDR", header);
	appendPngChunk(png, "IDAT", compressed);
	appendPngChunk(png, "IEND", {});
	return png;
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
	if (!certificateAuthority_.ensure(localHostname(), localIpv4Addresses(), lastError_))
		return false;
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

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	if (inet_pton(AF_INET, settings_.bindAddress.toUtf8().constData(), &address.sin_addr) != 1)
		address.sin_addr.s_addr = htonl(INADDR_ANY);
	auto openListener = [&](int port, const char *protocol, SOCKET &listener) {
		listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == INVALID_SOCKET) {
			setLastError(std::string("Failed to create ") + protocol + " listener.");
			return false;
		}
		BOOL reuse = TRUE;
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
		address.sin_port = htons(static_cast<u_short>(port));
		if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR ||
		    listen(listener, SOMAXCONN) == SOCKET_ERROR) {
			setLastError(std::string("Failed to listen for ") + protocol + " on port " + std::to_string(port) +
				     ". It may already be in use.");
			closesocket(listener);
			listener = INVALID_SOCKET;
			return false;
		}
		return true;
	};
	if (!openListener(settings_.port, "HTTPS", httpsListenSocket_) ||
	    !openListener(settings_.httpPort, "HTTP", httpListenSocket_)) {
		if (httpsListenSocket_ != INVALID_SOCKET) {
			closesocket(httpsListenSocket_);
			httpsListenSocket_ = INVALID_SOCKET;
		}
		running_.store(false);
		return false;
	}

	encoderThread_ = std::thread(&MjpegHttpServer::encoderLoop, this);
	httpsAcceptThread_ = std::thread(&MjpegHttpServer::acceptLoop, this, httpsListenSocket_, true);
	httpAcceptThread_ = std::thread(&MjpegHttpServer::acceptLoop, this, httpListenSocket_, false);
	return true;
}

void MjpegHttpServer::stop()
{
	if (!running_.exchange(false))
		return;

	for (auto *listener : {&httpsListenSocket_, &httpListenSocket_}) {
		if (*listener != INVALID_SOCKET) {
			shutdown(*listener, SD_BOTH);
			closesocket(*listener);
			*listener = INVALID_SOCKET;
		}
	}

	frameCv_.notify_all();
	rawCv_.notify_all();

	if (httpsAcceptThread_.joinable())
		httpsAcceptThread_.join();
	if (httpAcceptThread_.joinable())
		httpAcceptThread_.join();
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

bool MjpegHttpServer::exportTrustedCertificate(const QString &path, QString &error) const
{
	return certificateAuthority_.exportCertificate(path, error);
}

QString MjpegHttpServer::certificateFingerprint() const
{
	return certificateAuthority_.fingerprint();
}

std::string MjpegHttpServer::lanUrl(int port, bool tls)
{
	static WsaSession wsa;
	const std::string scheme = tls ? "https://" : "http://";
	if (!wsa.ok())
		return scheme + "127.0.0.1:" + std::to_string(port) + "/";
	return scheme + localIpv4() + ":" + std::to_string(port) + "/";
}

void MjpegHttpServer::acceptLoop(SOCKET listener, bool tls)
{
	while (running_.load()) {
		SOCKET client = accept(listener, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (running_.load())
				setLastError("Accept failed.");
			break;
		}

		if (clients_.fetch_add(1) >= settings_.maxClients) {
			clients_.fetch_sub(1);
			shutdown(client, SD_BOTH);
			closesocket(client);
			continue;
		}

		DWORD timeoutMs = 2000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));

		reapClientThreads();
		auto done = std::make_shared<std::atomic_bool>(false);
		std::lock_guard lock(clientThreadsMutex_);
		clientThreads_.push_back({std::thread(&MjpegHttpServer::clientLoop, this, client, done, tls), done});
	}
}

void MjpegHttpServer::clientLoop(SOCKET client, std::shared_ptr<std::atomic_bool> done, bool tls)
{
	ThreadDoneGuard doneGuard(std::move(done));
	AtomicDecrementGuard clientGuard(clients_);
	HttpConnection connection;
	if (!connection.open(client, tls, certificateAuthority_.serverCertificate()))
		return;

	char buffer[4096] = {};
	const int received = connection.receive(buffer, sizeof(buffer) - 1);
	if (received <= 0)
		return;

	const std::string request(buffer, buffer + received);
	const auto path = parsePath(request);
	const bool stayAwake = request.find("?stay-awake=1") != std::string::npos;
	if (path == "/" || path == "/index.html")
		handleIndex(connection, stayAwake, tls);
	else if (path == "/health")
		handleHealth(connection);
	else if (path == "/manifest.webmanifest")
		handleManifest(connection, stayAwake);
	else if (path == "/service-worker.js")
		handleServiceWorker(connection);
	else if (path == "/icon-192.png" || path == "/icon-512.png")
		handleIcon(connection, path == "/icon-512.png" ? 512 : 192);
	else if (path == "/preview.mjpg")
		handlePreview(connection);
	else if (path == "/snapshot.jpg")
		handleSnapshot(connection);
	else
		handleNotFound(connection);
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

void MjpegHttpServer::handleIndex(HttpConnection &client, bool stayAwake, bool tls)
{
	const std::string manifest = stayAwake ? "/manifest.webmanifest?stay-awake=1" : "/manifest.webmanifest";
	const std::string pwaHead = tls ? "<link rel=\"apple-touch-icon\" href=\"/icon-192.png\"><link rel=\"manifest\" href=\"" + manifest + "\">" : "";
	const std::string pwaScript = tls ? "if('serviceWorker'in navigator)navigator.serviceWorker.register('/service-worker.js').catch(()=>{});" : "";
	const std::string body =
		"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<meta name=\"theme-color\" content=\"#111111\"><meta name=\"apple-mobile-web-app-capable\" content=\"yes\">"
		"<meta name=\"apple-mobile-web-app-status-bar-style\" content=\"black\">" + pwaHead + "<title>OBS LAN Preview</title>"
		"<style>body{margin:0;background:#111;color:#eee;font:14px system-ui}main{min-height:100vh;display:grid;place-items:center}"
		"img{display:block;width:100vw;height:100vh;object-fit:contain;object-position:center}.status{position:fixed;left:12px;bottom:12px;"
		"padding:6px 9px;border-radius:7px;background:#000a;font-size:12px}.status button{margin-left:6px}</style></head>"
		"<body><main><img id=\"preview\" alt=\"OBS LAN Preview\"></main><div class=\"status\" id=\"status\" hidden></div>"
		"<script>(()=>{const image=document.getElementById('preview'),status=document.getElementById('status');"
		"const awake=new URLSearchParams(location.search).get('stay-awake')==='1';let lock=null,streaming=false;"
		"function setStatus(text,retry){status.hidden=!text;status.textContent=text;if(retry){const b=document.createElement('button');b.textContent='Try again';b.onclick=()=>requestLock();status.append(b)}}"
		"function start(){if(streaming||document.visibilityState!=='visible')return;streaming=true;image.src='/preview.mjpg?cache='+Date.now()}"
		"function releaseLock(){const current=lock;lock=null;if(current)current.release().catch(()=>{})}"
		"function stop(){if(streaming){streaming=false;image.removeAttribute('src')}releaseLock()}"
		"async function requestLock(){if(lock)return setStatus('Screen stays awake',false);if(!awake||document.visibilityState!=='visible'||!('wakeLock'in navigator))return setStatus(awake?'Wake lock is not supported':'',false);"
		"try{const sentinel=await navigator.wakeLock.request('screen');lock=sentinel;sentinel.addEventListener('release',()=>{if(lock===sentinel)lock=null;if(document.visibilityState==='visible')setStatus('Screen wake lock was released',true)});setStatus('Screen stays awake',false)}"
		"catch(e){lock=null;setStatus('Unable to keep screen awake',true)}}"
		"document.addEventListener('visibilitychange',()=>{if(document.hidden){stop();return}start();if(awake)requestLock()});window.addEventListener('pagehide',stop);" +
		pwaScript + "start();if(awake)requestLock()})();</script></body></html>";
	sendAll(client, httpHeader(200, "text/html; charset=utf-8", body.size()));
	sendAll(client, body);
}

void MjpegHttpServer::handleManifest(HttpConnection &client, bool stayAwake)
{
	const std::string suffix = stayAwake ? "?stay-awake=1" : "";
	const std::string name = stayAwake ? "OBS LAN Preview Awake" : "OBS LAN Preview";
	const std::string body = "{\"id\":\"/" + suffix + "\",\"name\":\"" + name +
		"\",\"short_name\":\"OBS Preview\",\"start_url\":\"/" + suffix +
		"\",\"display\":\"standalone\",\"background_color\":\"#111111\",\"theme_color\":\"#111111\","
		"\"icons\":[{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any maskable\"},"
		"{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}]}";
	sendAll(client, httpHeader(200, "application/manifest+json; charset=utf-8", body.size()));
	sendAll(client, body);
}

void MjpegHttpServer::handleServiceWorker(HttpConnection &client)
{
	const std::string body =
		"const CACHE='obs-lan-preview-v1';const SHELL=['/','/icon-192.png','/icon-512.png'];"
		"self.addEventListener('install',e=>e.waitUntil(caches.open(CACHE).then(c=>c.addAll(SHELL)).then(()=>self.skipWaiting())));"
		"self.addEventListener('activate',e=>e.waitUntil(self.clients.claim()));"
		"self.addEventListener('fetch',e=>{const u=new URL(e.request.url);if(u.origin===location.origin&&SHELL.includes(u.pathname))"
		"e.respondWith(caches.match(e.request).then(r=>r||fetch(e.request)))});";
	sendAll(client, httpHeader(200, "application/javascript; charset=utf-8", body.size()));
	sendAll(client, body);
}

void MjpegHttpServer::handleIcon(HttpConnection &client, int size)
{
	static const auto icon192 = makeIconPng(192);
	static const auto icon512 = makeIconPng(512);
	const auto &body = size == 512 ? icon512 : icon192;
	sendAll(client, httpHeader(200, "image/png", body.size()));
	sendAll(client, reinterpret_cast<const char *>(body.data()), body.size());
}

void MjpegHttpServer::handleHealth(HttpConnection &client)
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

void MjpegHttpServer::handleNotFound(HttpConnection &client)
{
	const std::string body = "Not found\n";
	sendAll(client, httpHeader(404, "text/plain; charset=utf-8", body.size()));
	sendAll(client, body);
}

void MjpegHttpServer::handleSnapshot(HttpConnection &client)
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

void MjpegHttpServer::handlePreview(HttpConnection &client)
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

bool MjpegHttpServer::sendAll(HttpConnection &socket, const char *data, size_t size)
{
	return socket.sendAll(data, size);
}

bool MjpegHttpServer::sendAll(HttpConnection &socket, const std::string &data)
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
