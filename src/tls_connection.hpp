#pragma once

#include <WinSock2.h>
#include <Windows.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <Sspi.h>
#include <Schannel.h>
#include <Wincrypt.h>

#include <cstddef>
#include <cstdint>
#include <vector>

inline constexpr size_t maxTlsBufferedBytes = 64 * 1024;

class TlsConnection {
public:
	TlsConnection() = default;
	~TlsConnection();

	bool accept(SOCKET socket, PCCERT_CONTEXT certificate);
	int receive(char *buffer, int size);
	bool sendAll(const char *data, size_t size);
	void close();

	TlsConnection(const TlsConnection &) = delete;
	TlsConnection &operator=(const TlsConnection &) = delete;

private:
	bool readEncrypted();
	bool decryptMore();
	bool sendRaw(const char *data, size_t size) const;

	SOCKET socket_ = INVALID_SOCKET;
	CredHandle credentials_ = {};
	CtxtHandle context_ = {};
	bool hasCredentials_ = false;
	bool hasContext_ = false;
	SecPkgContext_StreamSizes sizes_ = {};
	std::vector<uint8_t> encrypted_;
	std::vector<uint8_t> plaintext_;
	size_t plaintextOffset_ = 0;
};
