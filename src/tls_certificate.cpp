#include "tls_certificate.hpp"

#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <Ncrypt.h>

#include <QFile>
#include <QStringList>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace {
// v1 stored CERT_FRIENDLY_NAME_PROP_ID with an invalid pvData value.  Use new
// identifiers so an existing malformed certificate is never read by CryptoAPI.
constexpr wchar_t rootSubject[] = L"CN=LAN Preview Local CA v2";
constexpr wchar_t rootContainer[] = L"LAN Preview Local CA v2";
constexpr wchar_t rootFriendlyName[] = L"LAN Preview Local CA v2 (managed)";

std::wstring wide(const std::string &text)
{
	if (text.empty())
		return {};
	const auto count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring result(static_cast<size_t>(count), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
	return result;
}

std::wstring errorText(DWORD code)
{
	wchar_t *message = nullptr;
	const auto count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
						 nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
	std::wstring result = count && message ? std::wstring(message, count) : L"Windows error " + std::to_wstring(code);
	if (message)
		LocalFree(message);
	return result;
}

std::string utf8(const std::wstring &text)
{
	if (text.empty())
		return {};
	const auto count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<size_t>(count), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
	return result;
}

bool makeName(const std::wstring &subject, CERT_NAME_BLOB &name, std::vector<BYTE> &storage)
{
	DWORD size = 0;
	if (!CertStrToNameW(X509_ASN_ENCODING, subject.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &size, nullptr))
		return false;
	storage.resize(size);
	if (!CertStrToNameW(X509_ASN_ENCODING, subject.c_str(), CERT_X500_NAME_STR, nullptr, storage.data(), &size, nullptr))
		return false;
	name.cbData = size;
	name.pbData = storage.data();
	return true;
}

bool createKeyContainer(const wchar_t *name)
{
	HCRYPTPROV provider = 0;
	CryptAcquireContextW(&provider, name, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_DELETEKEYSET);
	if (!CryptAcquireContextW(&provider, name, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_NEWKEYSET))
		return false;
	HCRYPTKEY key = 0;
	// The CA and leaf keys never leave the Windows key container.
	const bool ok = CryptGenKey(provider, AT_KEYEXCHANGE, 2048u << 16, &key) == TRUE;
	if (key)
		CryptDestroyKey(key);
	CryptReleaseContext(provider, 0);
	return ok;
}

bool encodeObject(LPCSTR type, const void *value, std::vector<BYTE> &encoded)
{
	DWORD size = 0;
	if (!CryptEncodeObject(X509_ASN_ENCODING, type, value, nullptr, &size))
		return false;
	encoded.resize(size);
	return CryptEncodeObject(X509_ASN_ENCODING, type, value, encoded.data(), &size) == TRUE;
}

struct ExtensionStorage {
	std::vector<std::vector<BYTE>> bytes;
	std::vector<CERT_EXTENSION> extensions;

	bool add(LPCSTR oid, LPCSTR type, const void *value, bool critical = false)
	{
		bytes.emplace_back();
		if (!encodeObject(type, value, bytes.back()))
			return false;
		CERT_EXTENSION extension = {};
		extension.pszObjId = const_cast<char *>(oid);
		extension.fCritical = critical;
		extension.Value.cbData = static_cast<DWORD>(bytes.back().size());
		extension.Value.pbData = bytes.back().data();
		extensions.push_back(extension);
		return true;
	}
};

bool populateRootExtensions(ExtensionStorage &storage)
{
	CERT_BASIC_CONSTRAINTS2_INFO constraints = {};
	constraints.fCA = TRUE;
	CRYPT_BIT_BLOB usage = {};
	BYTE usageBytes[] = {static_cast<BYTE>(CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_CERT_SIGN_KEY_USAGE | CERT_CRL_SIGN_KEY_USAGE)};
	usage.cbData = sizeof(usageBytes);
	usage.pbData = usageBytes;
	return storage.add(szOID_BASIC_CONSTRAINTS2, X509_BASIC_CONSTRAINTS2, &constraints, true) &&
	       storage.add(szOID_KEY_USAGE, X509_KEY_USAGE, &usage, true);
}

bool populateServerExtensions(ExtensionStorage &storage, const std::wstring &hostname, const std::vector<std::string> &ipv4Addresses)
{
	std::vector<std::array<BYTE, 4>> ips;
	for (const auto &ipv4 : ipv4Addresses) {
		std::array<BYTE, 4> ip = {};
		if (InetPtonA(AF_INET, ipv4.c_str(), ip.data()) == 1)
			ips.push_back(ip);
	}
	if (ips.empty())
		return false;

	std::vector<CERT_ALT_NAME_ENTRY> names(ips.size() + 1);
	names[0].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
	names[0].pwszDNSName = const_cast<wchar_t *>(hostname.c_str());
	for (size_t index = 0; index < ips.size(); ++index) {
		names[index + 1].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
		names[index + 1].IPAddress.cbData = static_cast<DWORD>(ips[index].size());
		names[index + 1].IPAddress.pbData = ips[index].data();
	}
	CERT_ALT_NAME_INFO altNames = {static_cast<DWORD>(names.size()), names.data()};

	CERT_ENHKEY_USAGE eku = {};
	LPSTR usageOid[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
	eku.cUsageIdentifier = 1;
	eku.rgpszUsageIdentifier = usageOid;
	BYTE usageBytes[] = {static_cast<BYTE>(CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE)};
	CRYPT_BIT_BLOB usage = {sizeof(usageBytes), usageBytes, 0};
	return storage.add(szOID_SUBJECT_ALT_NAME2, X509_ALTERNATE_NAME, &altNames) &&
	       storage.add(szOID_ENHANCED_KEY_USAGE, X509_ENHANCED_KEY_USAGE, &eku) &&
	       storage.add(szOID_KEY_USAGE, X509_KEY_USAGE, &usage, true);
}

PCCERT_CONTEXT createSelfSigned(const std::wstring &subject, const wchar_t *container, const ExtensionStorage &storage)
{
	if (!createKeyContainer(container))
		return nullptr;

	CERT_NAME_BLOB name = {};
	std::vector<BYTE> nameBytes;
	if (!makeName(subject, name, nameBytes))
		return nullptr;

	CRYPT_KEY_PROV_INFO provider = {};
	provider.pwszContainerName = const_cast<wchar_t *>(container);
	provider.pwszProvName = const_cast<wchar_t *>(MS_ENH_RSA_AES_PROV_W);
	provider.dwProvType = PROV_RSA_AES;
	provider.dwKeySpec = AT_KEYEXCHANGE;
	CERT_EXTENSIONS extensions = {static_cast<DWORD>(storage.extensions.size()),
					     const_cast<PCERT_EXTENSION>(storage.extensions.data())};
	CRYPT_ALGORITHM_IDENTIFIER algorithm = {};
	algorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

	SYSTEMTIME start = {};
	GetSystemTime(&start);
	SYSTEMTIME end = start;
	end.wYear = static_cast<WORD>(end.wYear + 10);
	return CertCreateSelfSignCertificate(0, &name, 0, &provider, &algorithm, &start, &end, &extensions);
}

PCCERT_CONTEXT findRoot()
{
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"CA");
	if (!store)
		return nullptr;
	PCCERT_CONTEXT found = nullptr;
	PCCERT_CONTEXT result = nullptr;
	while ((found = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SUBJECT_STR_W, rootSubject, found))) {
		wchar_t friendlyName[128] = {};
		DWORD size = sizeof(friendlyName);
		if (CertGetCertificateContextProperty(found, CERT_FRIENDLY_NAME_PROP_ID, friendlyName, &size) &&
		    wcscmp(friendlyName, rootFriendlyName) == 0) {
			result = CertDuplicateCertificateContext(found);
			break;
		}
	}
	if (found)
		CertFreeCertificateContext(found);
	CertCloseStore(store, 0);
	return result;
}

bool storeRoot(PCCERT_CONTEXT certificate)
{
	// CERT_FRIENDLY_NAME_PROP_ID takes a CRYPT_DATA_BLOB, not a raw wchar_t
	// pointer.  Passing the latter makes CryptoAPI interpret UTF-16 code units
	// as a length and pointer, which can crash inside crypt32.dll.
	CRYPT_DATA_BLOB friendlyName = {};
	friendlyName.cbData = sizeof(rootFriendlyName);
	friendlyName.pbData = reinterpret_cast<BYTE *>(const_cast<wchar_t *>(rootFriendlyName));
	if (!CertSetCertificateContextProperty(certificate, CERT_FRIENDLY_NAME_PROP_ID, 0, &friendlyName))
		return false;
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"CA");
	if (!store)
		return false;
	const bool ok = CertAddCertificateContextToStore(store, certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr) == TRUE;
	CertCloseStore(store, 0);
	return ok;
}

PCCERT_CONTEXT signServerCertificate(PCCERT_CONTEXT root, PCCERT_CONTEXT templateCertificate, const wchar_t *leafContainer)
{
	HCRYPTPROV_OR_NCRYPT_KEY_HANDLE rootKey = 0;
	DWORD keySpec = 0;
	BOOL releaseKey = FALSE;
	if (!CryptAcquireCertificatePrivateKey(root, CRYPT_ACQUIRE_USE_PROV_INFO_FLAG | CRYPT_ACQUIRE_SILENT_FLAG, nullptr,
						      &rootKey, &keySpec, &releaseKey))
		return nullptr;

	CERT_INFO info = *templateCertificate->pCertInfo;
	info.Issuer = root->pCertInfo->Subject;
	BYTE serial[16] = {};
	CryptGenRandom(static_cast<HCRYPTPROV>(rootKey), sizeof(serial), serial);
	info.SerialNumber = {sizeof(serial), serial};
	CRYPT_ALGORITHM_IDENTIFIER algorithm = {};
	algorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);
	info.SignatureAlgorithm = algorithm;

	DWORD size = 0;
	const bool sized = CryptSignAndEncodeCertificate(static_cast<HCRYPTPROV>(rootKey), keySpec, X509_ASN_ENCODING,
									 X509_CERT_TO_BE_SIGNED, &info, &algorithm, nullptr, nullptr, &size) == TRUE;
	std::vector<BYTE> encoded(size);
	const bool signedCertificate = sized && CryptSignAndEncodeCertificate(static_cast<HCRYPTPROV>(rootKey), keySpec,
		X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &info, &algorithm, nullptr, encoded.data(), &size) == TRUE;
	if (releaseKey)
		CryptReleaseContext(static_cast<HCRYPTPROV>(rootKey), 0);
	if (!signedCertificate)
		return nullptr;

	auto *certificate = CertCreateCertificateContext(X509_ASN_ENCODING, encoded.data(), size);
	if (!certificate)
		return nullptr;
	CRYPT_KEY_PROV_INFO leafProvider = {};
	leafProvider.pwszContainerName = const_cast<wchar_t *>(leafContainer);
	leafProvider.pwszProvName = const_cast<wchar_t *>(MS_ENH_RSA_AES_PROV_W);
	leafProvider.dwProvType = PROV_RSA_AES;
	leafProvider.dwKeySpec = AT_KEYEXCHANGE;
	if (!CertSetCertificateContextProperty(certificate, CERT_KEY_PROV_INFO_PROP_ID, 0, &leafProvider)) {
		CertFreeCertificateContext(certificate);
		return nullptr;
	}
	return certificate;
}
}

LocalCertificateAuthority::~LocalCertificateAuthority()
{
	if (server_)
		CertFreeCertificateContext(server_);
	if (root_)
		CertFreeCertificateContext(root_);
}

bool LocalCertificateAuthority::ensure(const std::string &hostname, const std::vector<std::string> &ipv4Addresses, std::string &error)
{
	if (server_)
		return true;

	root_ = findRoot();
	if (!root_) {
		ExtensionStorage rootExtensions;
		if (!populateRootExtensions(rootExtensions) || !(root_ = createSelfSigned(rootSubject, rootContainer, rootExtensions)) ||
		    !storeRoot(root_)) {
			error = "Unable to create the local certificate authority: " + utf8(errorText(GetLastError()));
			return false;
		}
	}

	const auto host = wide(hostname);
	if (host.empty()) {
		error = "Unable to determine the local hostname for the TLS certificate.";
		return false;
	}
	ExtensionStorage leafExtensions;
	if (!populateServerExtensions(leafExtensions, host, ipv4Addresses)) {
		error = "Unable to create certificate subject alternative names.";
		return false;
	}
	const auto leafContainer = std::wstring(L"LAN Preview Server ") + host;
	auto *temporary = createSelfSigned(L"CN=" + host, leafContainer.c_str(), leafExtensions);
	if (!temporary) {
		error = "Unable to create the local TLS key: " + utf8(errorText(GetLastError()));
		return false;
	}
	server_ = signServerCertificate(root_, temporary, leafContainer.c_str());
	CertFreeCertificateContext(temporary);
	if (!server_) {
		error = "Unable to sign the local TLS certificate: " + utf8(errorText(GetLastError()));
		return false;
	}
	return true;
}

PCCERT_CONTEXT LocalCertificateAuthority::serverCertificate() const
{
	return server_;
}

bool LocalCertificateAuthority::exportCertificate(const QString &path, QString &error) const
{
	if (!root_) {
		error = "The local CA has not been created yet. Enable the preview first.";
		return false;
	}
	DWORD size = 0;
	if (!CryptBinaryToStringW(root_->pbCertEncoded, root_->cbCertEncoded,
					  CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCRLF, nullptr, &size)) {
		error = "Unable to encode the CA certificate.";
		return false;
	}
	std::wstring pem(static_cast<size_t>(size), L'\0');
	if (!CryptBinaryToStringW(root_->pbCertEncoded, root_->cbCertEncoded,
					  CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCRLF, pem.data(), &size)) {
		error = "Unable to encode the CA certificate.";
		return false;
	}
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		error = "Unable to write the selected certificate file.";
		return false;
	}
	file.write(QString::fromWCharArray(pem.c_str()).toUtf8());
	return true;
}

QString LocalCertificateAuthority::fingerprint() const
{
	if (!root_)
		return {};
	BYTE hash[64] = {};
	DWORD size = sizeof(hash);
	if (!CertGetCertificateContextProperty(root_, CERT_SHA256_HASH_PROP_ID, hash, &size))
		return {};
	QStringList parts;
	for (DWORD i = 0; i < size; ++i)
		parts += QStringLiteral("%1").arg(hash[i], 2, 16, QLatin1Char('0')).toUpper();
	return parts.join(':');
}
