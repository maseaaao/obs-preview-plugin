#pragma once

#include <WinSock2.h>
#include <Windows.h>
#include <Wincrypt.h>

#include <QString>

#include <string>
#include <vector>

class LocalCertificateAuthority {
public:
	LocalCertificateAuthority() = default;
	~LocalCertificateAuthority();

	bool ensure(const std::string &hostname, const std::vector<std::string> &ipv4Addresses, std::string &error);
	PCCERT_CONTEXT serverCertificate() const;
	bool exportCertificate(const QString &path, QString &error) const;
	QString fingerprint() const;

private:
	PCCERT_CONTEXT root_ = nullptr;
	PCCERT_CONTEXT server_ = nullptr;
};
