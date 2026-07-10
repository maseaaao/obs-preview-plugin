#include "tls_connection.hpp"

#include <Security.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace {
constexpr DWORD requestFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
				      ASC_REQ_EXTENDED_ERROR | ASC_REQ_STREAM | ASC_REQ_ALLOCATE_MEMORY;
}

TlsConnection::~TlsConnection()
{
	close();
}

bool TlsConnection::accept(SOCKET socket, PCCERT_CONTEXT certificate)
{
	socket_ = socket;
	SCHANNEL_CRED credentials = {};
	credentials.dwVersion = SCHANNEL_CRED_VERSION;
	credentials.cCreds = 1;
	credentials.paCred = &certificate;
	TimeStamp expiry = {};
	if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t *>(UNISP_NAME_W), SECPKG_CRED_INBOUND, nullptr, &credentials,
					      nullptr, nullptr, &credentials_, &expiry) != SEC_E_OK)
		return false;
	hasCredentials_ = true;

	SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
	while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE) {
		if (encrypted_.empty() || status == SEC_E_INCOMPLETE_MESSAGE) {
			if (!readEncrypted())
				return false;
		}

		SecBuffer inputBuffers[2] = {};
		inputBuffers[0].BufferType = SECBUFFER_TOKEN;
		inputBuffers[0].pvBuffer = encrypted_.data();
		inputBuffers[0].cbBuffer = static_cast<unsigned long>(encrypted_.size());
		inputBuffers[1].BufferType = SECBUFFER_EMPTY;
		SecBufferDesc input = {SECBUFFER_VERSION, 2, inputBuffers};
		SecBuffer outputBuffer = {0, SECBUFFER_TOKEN, nullptr};
		SecBufferDesc output = {SECBUFFER_VERSION, 1, &outputBuffer};
		DWORD attributes = 0;
		TimeStamp contextExpiry = {};
		status = AcceptSecurityContext(&credentials_, hasContext_ ? &context_ : nullptr, &input, requestFlags,
						       SECURITY_NATIVE_DREP, &context_, &output, &attributes, &contextExpiry);
		hasContext_ = status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE;

		if (outputBuffer.cbBuffer && outputBuffer.pvBuffer) {
			const bool sent = sendRaw(static_cast<const char *>(outputBuffer.pvBuffer), outputBuffer.cbBuffer);
			FreeContextBuffer(outputBuffer.pvBuffer);
			if (!sent)
				return false;
		}
		if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED && status != SEC_E_INCOMPLETE_MESSAGE)
			return false;

		if (inputBuffers[1].BufferType == SECBUFFER_EXTRA) {
			const auto offset = encrypted_.size() - inputBuffers[1].cbBuffer;
			std::vector<uint8_t> extra(encrypted_.begin() + static_cast<ptrdiff_t>(offset), encrypted_.end());
			encrypted_ = std::move(extra);
		} else {
			encrypted_.clear();
		}
	}

	if (status != SEC_E_OK)
		return false;
	return QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) == SEC_E_OK;
}

int TlsConnection::receive(char *buffer, int size)
{
	if (!buffer || size <= 0)
		return 0;
	while (plaintextOffset_ >= plaintext_.size()) {
		plaintext_.clear();
		plaintextOffset_ = 0;
		if (!decryptMore())
			return 0;
	}
	const auto count = std::min<size_t>(static_cast<size_t>(size), plaintext_.size() - plaintextOffset_);
	std::memcpy(buffer, plaintext_.data() + plaintextOffset_, count);
	plaintextOffset_ += count;
	return static_cast<int>(count);
}

bool TlsConnection::sendAll(const char *data, size_t size)
{
	if (!data || !hasContext_ || sizes_.cbMaximumMessage == 0)
		return false;
	while (size > 0) {
		const auto chunk = std::min<size_t>(size, sizes_.cbMaximumMessage);
		std::vector<uint8_t> packet(sizes_.cbHeader + chunk + sizes_.cbTrailer);
		std::memcpy(packet.data() + sizes_.cbHeader, data, chunk);
		SecBuffer buffers[4] = {};
		buffers[0] = {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, packet.data()};
		buffers[1] = {static_cast<unsigned long>(chunk), SECBUFFER_DATA, packet.data() + sizes_.cbHeader};
		buffers[2] = {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, packet.data() + sizes_.cbHeader + chunk};
		buffers[3] = {0, SECBUFFER_EMPTY, nullptr};
		SecBufferDesc descriptor = {SECBUFFER_VERSION, 4, buffers};
		if (EncryptMessage(&context_, 0, &descriptor, 0) != SEC_E_OK)
			return false;
		const auto packetSize = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
		if (!sendRaw(reinterpret_cast<const char *>(packet.data()), packetSize))
			return false;
		data += chunk;
		size -= chunk;
	}
	return true;
}

void TlsConnection::close()
{
	if (socket_ != INVALID_SOCKET) {
		shutdown(socket_, SD_BOTH);
		closesocket(socket_);
		socket_ = INVALID_SOCKET;
	}
	if (hasContext_) {
		DeleteSecurityContext(&context_);
		hasContext_ = false;
	}
	if (hasCredentials_) {
		FreeCredentialsHandle(&credentials_);
		hasCredentials_ = false;
	}
}

bool TlsConnection::readEncrypted()
{
	if (encrypted_.size() >= maxTlsBufferedBytes)
		return false;
	std::array<uint8_t, 16 * 1024> chunk = {};
	const auto capacity = std::min(chunk.size(), maxTlsBufferedBytes - encrypted_.size());
	const auto received = recv(socket_, reinterpret_cast<char *>(chunk.data()), static_cast<int>(capacity), 0);
	if (received <= 0)
		return false;
	encrypted_.insert(encrypted_.end(), chunk.begin(), chunk.begin() + received);
	return true;
}

bool TlsConnection::decryptMore()
{
	while (true) {
		if (encrypted_.empty() && !readEncrypted())
			return false;
		SecBuffer buffers[4] = {};
		buffers[0] = {static_cast<unsigned long>(encrypted_.size()), SECBUFFER_DATA, encrypted_.data()};
		for (size_t index = 1; index < 4; ++index)
			buffers[index].BufferType = SECBUFFER_EMPTY;
		SecBufferDesc descriptor = {SECBUFFER_VERSION, 4, buffers};
		DWORD qualityOfProtection = 0;
		const auto status = DecryptMessage(&context_, &descriptor, 0, &qualityOfProtection);
		if (status == SEC_E_INCOMPLETE_MESSAGE) {
			if (!readEncrypted())
				return false;
			continue;
		}
		if (status == SEC_I_CONTEXT_EXPIRED)
			return false;
		if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE)
			return false;

		DWORD extraSize = 0;
		for (const auto &part : buffers) {
			if (part.BufferType == SECBUFFER_DATA && part.cbBuffer > 0) {
				const auto *bytes = static_cast<const uint8_t *>(part.pvBuffer);
				plaintext_.insert(plaintext_.end(), bytes, bytes + part.cbBuffer);
			}
			if (part.BufferType == SECBUFFER_EXTRA)
				extraSize = part.cbBuffer;
		}
		if (extraSize) {
			std::vector<uint8_t> extra(encrypted_.end() - static_cast<ptrdiff_t>(extraSize), encrypted_.end());
			encrypted_ = std::move(extra);
		} else {
			encrypted_.clear();
		}
		return !plaintext_.empty();
	}
}

bool TlsConnection::sendRaw(const char *data, size_t size) const
{
	while (size > 0) {
		const auto sent = send(socket_, data, static_cast<int>(std::min<size_t>(size, 64 * 1024)), 0);
		if (sent <= 0)
			return false;
		data += sent;
		size -= static_cast<size_t>(sent);
	}
	return true;
}
