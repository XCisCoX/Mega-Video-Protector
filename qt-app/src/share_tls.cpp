#include "videovault/app/share_tls.hpp"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTcpSocket>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#include <windows.h>
#include <wincrypt.h>
#include <schannel.h>
#include <security.h>
#ifndef SP_PROT_TLS1_3_SERVER
#define SP_PROT_TLS1_3_SERVER 0x00001000
#endif
#endif

namespace videovault::app {
namespace {

constexpr int kHandshakeMs = 8000;
constexpr int kMaxHandshakeBytes = 64 * 1024;

#if defined(_WIN32)

bool writeSocket(QTcpSocket* socket, const char* data, int size) {
    int off = 0;
    while (off < size) {
        const qint64 wrote = socket->write(data + off, size - off);
        if (wrote <= 0) return false;
        off += static_cast<int>(wrote);
        if (!socket->waitForBytesWritten(20000)) return false;
    }
    return true;
}

DWORD contextFlags() {
    return ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY
        | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_EXTENDED_ERROR;
}

bool encodeObject(const char* type, const void* data, std::vector<BYTE>* out) {
    DWORD size = 0;
    if (!CryptEncodeObject(X509_ASN_ENCODING, type, data, nullptr, &size) || size == 0) {
        return false;
    }
    out->assign(size, 0);
    return CryptEncodeObject(X509_ASN_ENCODING, type, data, out->data(), &size) == TRUE;
}

bool parseIpv4(const QString& text, BYTE out[4]) {
    const QStringList parts = text.split(QLatin1Char('.'));
    if (parts.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        const int value = parts.at(i).toInt(&ok);
        if (!ok || value < 0 || value > 255) return false;
        out[i] = static_cast<BYTE>(value);
    }
    return true;
}

#endif

} // namespace

struct ShareTlsIdentity::Impl {
#if defined(_WIN32)
    HCRYPTPROV provider{0};
    std::wstring container;
    PCCERT_CONTEXT certificate{nullptr};
    CredHandle credentials{};
    bool haveCredentials{false};
    QString fingerprint;

    ~Impl() {
        if (haveCredentials) {
            FreeCredentialsHandle(&credentials);
            haveCredentials = false;
        }
        if (certificate != nullptr) {
            CertFreeCertificateContext(certificate);
            certificate = nullptr;
        }
        if (provider != 0) {
            CryptReleaseContext(provider, 0);
            provider = 0;
        }
        if (!container.empty()) {
            HCRYPTPROV doomed = 0;
            CryptAcquireContextW(&doomed, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                CRYPT_DELETEKEYSET);
            container.clear();
        }
    }

    bool generate(const QStringList& addresses, QString* error) {
        quint32 words[4] = {};
        QRandomGenerator::system()->fillRange(words);
        const QByteArray hex(reinterpret_cast<const char*>(words), static_cast<int>(sizeof(words)));
        container = QStringLiteral("MVPShare-%1").arg(QString::fromLatin1(hex.toHex())).toStdWString();
        if (!CryptAcquireContextW(&provider, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                CRYPT_NEWKEYSET)) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            container.clear();
            return false;
        }
        HCRYPTKEY key = 0;
        if (!CryptGenKey(provider, AT_KEYEXCHANGE, (2048U << 16) | CRYPT_EXPORTABLE, &key)) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        struct KeyCloser {
            HCRYPTKEY key;
            ~KeyCloser() { if (key != 0) CryptDestroyKey(key); }
        } closer{key};

        DWORD nameSize = 0;
        const wchar_t* subject = L"CN=MegaVideoProtect";
        if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, nullptr, &nameSize,
                nullptr)) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        std::vector<BYTE> name(nameSize);
        if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, nullptr, name.data(), &nameSize,
                nullptr)) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }

        BYTE usageBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE | CERT_KEY_ENCIPHERMENT_KEY_USAGE;
        CRYPT_BIT_BLOB usageBlob{};
        usageBlob.cbData = 1;
        usageBlob.pbData = &usageBits;
        usageBlob.cUnusedBits = 0;
        std::vector<BYTE> usageEncoded;
        if (!encodeObject(X509_KEY_USAGE, &usageBlob, &usageEncoded)) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }

        LPSTR purposes[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        CERT_ENHKEY_USAGE eku{};
        eku.cUsageIdentifier = 1;
        eku.rgpszUsageIdentifier = purposes;
        std::vector<BYTE> ekuEncoded;
        if (!encodeObject(X509_ENHANCED_KEY_USAGE, &eku, &ekuEncoded)) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }

        std::vector<CERT_EXTENSION> extensions(2);
        extensions[0].pszObjId = const_cast<LPSTR>(szOID_KEY_USAGE);
        extensions[0].fCritical = FALSE;
        extensions[0].Value.cbData = static_cast<DWORD>(usageEncoded.size());
        extensions[0].Value.pbData = usageEncoded.data();
        extensions[1].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
        extensions[1].fCritical = FALSE;
        extensions[1].Value.cbData = static_cast<DWORD>(ekuEncoded.size());
        extensions[1].Value.pbData = ekuEncoded.data();

        std::vector<std::array<BYTE, 4>> ipBytes;
        for (const QString& address : addresses) {
            std::array<BYTE, 4> ip{};
            if (parseIpv4(address, ip.data())) ipBytes.push_back(ip);
        }
        std::vector<CERT_ALT_NAME_ENTRY> altEntries(ipBytes.size());
        for (std::size_t i = 0; i < ipBytes.size(); ++i) {
            altEntries[i] = {};
            altEntries[i].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
            altEntries[i].IPAddress.cbData = 4;
            altEntries[i].IPAddress.pbData = ipBytes[i].data();
        }
        std::vector<BYTE> sanEncoded;
        if (!altEntries.empty()) {
            CERT_ALT_NAME_INFO alt{};
            alt.cAltEntry = static_cast<DWORD>(altEntries.size());
            alt.rgAltEntry = altEntries.data();
            if (encodeObject(X509_ALTERNATE_NAME, &alt, &sanEncoded)) {
                CERT_EXTENSION san{};
                san.pszObjId = const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2);
                san.fCritical = FALSE;
                san.Value.cbData = static_cast<DWORD>(sanEncoded.size());
                san.Value.pbData = sanEncoded.data();
                extensions.push_back(san);
            }
        }
        CERT_EXTENSIONS extensionList{};
        extensionList.cExtension = static_cast<DWORD>(extensions.size());
        extensionList.rgExtension = extensions.data();

        CRYPT_KEY_PROV_INFO keyInfo{};
        keyInfo.pwszContainerName = container.data();
        keyInfo.pwszProvName = const_cast<LPWSTR>(MS_ENH_RSA_AES_PROV_W);
        keyInfo.dwProvType = PROV_RSA_AES;
        keyInfo.dwKeySpec = AT_KEYEXCHANGE;

        CRYPT_ALGORITHM_IDENTIFIER signature{};
        signature.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

        SYSTEMTIME notBefore{};
        GetSystemTime(&notBefore);
        FILETIME fileTime{};
        SystemTimeToFileTime(&notBefore, &fileTime);
        ULARGE_INTEGER stamp{};
        stamp.LowPart = fileTime.dwLowDateTime;
        stamp.HighPart = fileTime.dwHighDateTime;
        stamp.QuadPart -= 24ULL * 60ULL * 60ULL * 10000000ULL;
        fileTime.dwLowDateTime = stamp.LowPart;
        fileTime.dwHighDateTime = stamp.HighPart;
        FileTimeToSystemTime(&fileTime, &notBefore);
        SYSTEMTIME notAfter{};
        GetSystemTime(&notAfter);
        SystemTimeToFileTime(&notAfter, &fileTime);
        stamp.LowPart = fileTime.dwLowDateTime;
        stamp.HighPart = fileTime.dwHighDateTime;
        stamp.QuadPart += 2ULL * 24ULL * 60ULL * 60ULL * 10000000ULL;
        fileTime.dwLowDateTime = stamp.LowPart;
        fileTime.dwHighDateTime = stamp.HighPart;
        FileTimeToSystemTime(&fileTime, &notAfter);

        CERT_NAME_BLOB subjectBlob{};
        subjectBlob.cbData = nameSize;
        subjectBlob.pbData = name.data();
        certificate = CertCreateSelfSignCertificate(provider, &subjectBlob, 0, &keyInfo, &signature,
            &notBefore, &notAfter, &extensionList);
        if (certificate == nullptr) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }

        const QByteArray der(reinterpret_cast<const char*>(certificate->pbCertEncoded),
            static_cast<int>(certificate->cbCertEncoded));
        fingerprint = QString::fromLatin1(QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex());

        SCHANNEL_CRED credentialsData{};
        credentialsData.dwVersion = SCHANNEL_CRED_VERSION;
        credentialsData.cCreds = 1;
        credentialsData.paCred = &certificate;
        credentialsData.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER | SP_PROT_TLS1_3_SERVER;
        credentialsData.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER;
        TimeStamp expiry{};
        SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
            SECPKG_CRED_INBOUND, nullptr, &credentialsData, nullptr, nullptr, &credentials, &expiry);
        if (status != SEC_E_OK) {
            credentialsData.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
            status = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
                SECPKG_CRED_INBOUND, nullptr, &credentialsData, nullptr, nullptr, &credentials, &expiry);
        }
        if (status != SEC_E_OK) {
            if (error != nullptr) *error = QStringLiteral("Could not start the encrypted connection.");
            return false;
        }
        haveCredentials = true;
        return true;
    }
#endif
};

ShareTlsIdentity::ShareTlsIdentity(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ShareTlsIdentity::~ShareTlsIdentity() = default;

std::shared_ptr<ShareTlsIdentity> ShareTlsIdentity::create(
    const QStringList& addresses, QString* fingerprint, QString* error) {
#if defined(_WIN32)
    auto impl = std::make_unique<Impl>();
    if (!impl->generate(addresses, error)) return nullptr;
    if (fingerprint != nullptr) *fingerprint = impl->fingerprint;
    return std::shared_ptr<ShareTlsIdentity>(new ShareTlsIdentity(std::move(impl)));
#else
    Q_UNUSED(addresses)
    Q_UNUSED(fingerprint)
    if (error != nullptr) {
        *error = QStringLiteral("Encrypted sharing is not available on this system.");
    }
    return nullptr;
#endif
}

struct TlsChannel::Impl {
    QTcpSocket socket;
    std::shared_ptr<ShareTlsIdentity> identity;
    QByteArray pending;
    QByteArray plain;
    bool open{false};
#if defined(_WIN32)
    CtxtHandle context{};
    bool haveContext{false};
    SecPkgContext_StreamSizes sizes{};

    ~Impl() {
        if (haveContext) {
            DeleteSecurityContext(&context);
            haveContext = false;
        }
        if (socket.state() != QAbstractSocket::UnconnectedState) {
            socket.disconnectFromHost();
        }
    }

    bool sendEncrypted(const char* data, int size) {
        if (size <= 0) return true;
        const int limit = sizes.cbMaximumMessage > 0 ? static_cast<int>(sizes.cbMaximumMessage) : 16384;
        int off = 0;
        while (off < size) {
            const int chunk = std::min(limit, size - off);
            std::vector<char> packet(static_cast<std::size_t>(sizes.cbHeader) + static_cast<std::size_t>(chunk)
                + static_cast<std::size_t>(sizes.cbTrailer));
            SecBuffer buffers[4] = {};
            buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            buffers[0].pvBuffer = packet.data();
            buffers[0].cbBuffer = sizes.cbHeader;
            buffers[1].BufferType = SECBUFFER_DATA;
            buffers[1].pvBuffer = packet.data() + sizes.cbHeader;
            buffers[1].cbBuffer = static_cast<unsigned long>(chunk);
            buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            buffers[2].pvBuffer = packet.data() + sizes.cbHeader + chunk;
            buffers[2].cbBuffer = sizes.cbTrailer;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            std::memcpy(buffers[1].pvBuffer, data + off, static_cast<std::size_t>(chunk));
            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;
            if (EncryptMessage(&context, 0, &desc, 0) != SEC_E_OK) return false;
            QByteArray wire;
            for (int i = 0; i < 3; ++i) {
                if (buffers[i].cbBuffer > 0 && buffers[i].pvBuffer != nullptr) {
                    wire.append(static_cast<const char*>(buffers[i].pvBuffer),
                        static_cast<int>(buffers[i].cbBuffer));
                }
            }
            if (!wire.isEmpty() && !writeSocket(&socket, wire.constData(), wire.size())) return false;
            off += chunk;
        }
        return true;
    }

    bool takePlain(int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        while (plain.isEmpty()) {
            if (pending.isEmpty()) {
                const int left = timeoutMs - static_cast<int>(timer.elapsed());
                if (left <= 0 || !socket.waitForReadyRead(left)) return false;
                pending += socket.readAll();
                if (pending.size() > kMaxHandshakeBytes * 4) return false;
                if (pending.isEmpty()) return false;
            }
            SecBuffer buffers[4] = {};
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[0].pvBuffer = pending.data();
            buffers[0].cbBuffer = static_cast<unsigned long>(pending.size());
            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;
            const SECURITY_STATUS status = DecryptMessage(&context, &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                const int left = timeoutMs - static_cast<int>(timer.elapsed());
                if (left <= 0 || !socket.waitForReadyRead(left)) return false;
                pending += socket.readAll();
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) {
                open = false;
                return false;
            }
            if (status != SEC_E_OK) return false;
            QByteArray data;
            QByteArray extra;
            for (const SecBuffer& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0 && buffer.pvBuffer != nullptr) {
                    data.append(static_cast<const char*>(buffer.pvBuffer), static_cast<int>(buffer.cbBuffer));
                } else if (buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer > 0 && buffer.pvBuffer != nullptr) {
                    extra.append(static_cast<const char*>(buffer.pvBuffer), static_cast<int>(buffer.cbBuffer));
                }
            }
            pending = extra;
            if (data.isEmpty() && extra.isEmpty()) return false;
            plain += data;
        }
        return true;
    }
#endif
};

TlsChannel::TlsChannel() : impl_(std::make_unique<Impl>()) {}

TlsChannel::~TlsChannel() = default;

bool TlsChannel::accept(const qintptr descriptor, const std::shared_ptr<ShareTlsIdentity>& identity) {
#if !defined(_WIN32)
    Q_UNUSED(descriptor)
    Q_UNUSED(identity)
    return false;
#else
    if (!identity || !identity->impl_->haveCredentials) return false;
    impl_->identity = identity;
    impl_->socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    if (!impl_->socket.setSocketDescriptor(descriptor)) return false;

    QByteArray incoming;
    QElapsedTimer timer;
    timer.start();
    bool established = false;
    while (timer.elapsed() < kHandshakeMs && !established) {
        if (incoming.isEmpty()) {
            const int left = kHandshakeMs - static_cast<int>(timer.elapsed());
            if (left <= 0 || !impl_->socket.waitForReadyRead(left)) return false;
            incoming += impl_->socket.readAll();
            if (incoming.size() > kMaxHandshakeBytes) return false;
            if (incoming.isEmpty()) return false;
        }
        SecBuffer input[2] = {};
        input[0].BufferType = SECBUFFER_TOKEN;
        input[0].pvBuffer = incoming.data();
        input[0].cbBuffer = static_cast<unsigned long>(incoming.size());
        input[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inputDesc{};
        inputDesc.ulVersion = SECBUFFER_VERSION;
        inputDesc.cBuffers = 2;
        inputDesc.pBuffers = input;

        SecBuffer output{};
        output.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc outputDesc{};
        outputDesc.ulVersion = SECBUFFER_VERSION;
        outputDesc.cBuffers = 1;
        outputDesc.pBuffers = &output;

        DWORD attributes = 0;
        const SECURITY_STATUS status = AcceptSecurityContext(&identity->impl_->credentials,
            impl_->haveContext ? &impl_->context : nullptr, &inputDesc, contextFlags(), SECURITY_NATIVE_DREP,
            &impl_->context, &outputDesc, &attributes, nullptr);
        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_COMPLETE_NEEDED
            || status == SEC_I_COMPLETE_AND_CONTINUE) {
            impl_->haveContext = true;
        }
        if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
            CompleteAuthToken(&impl_->context, &outputDesc);
        }
        bool wrote = true;
        if (output.cbBuffer > 0 && output.pvBuffer != nullptr) {
            wrote = writeSocket(&impl_->socket, static_cast<const char*>(output.pvBuffer),
                static_cast<int>(output.cbBuffer));
            FreeContextBuffer(output.pvBuffer);
        }
        if (!wrote) return false;
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            const int left = kHandshakeMs - static_cast<int>(timer.elapsed());
            if (left <= 0 || !impl_->socket.waitForReadyRead(left)) return false;
            incoming += impl_->socket.readAll();
            if (incoming.size() > kMaxHandshakeBytes) return false;
            continue;
        }
        if (input[1].BufferType == SECBUFFER_EXTRA && input[1].cbBuffer > 0) {
            incoming = incoming.right(static_cast<int>(input[1].cbBuffer));
        } else {
            incoming.clear();
        }
        if (status == SEC_E_OK || status == SEC_I_COMPLETE_NEEDED) {
            impl_->pending = incoming;
            established = true;
            break;
        }
        if (status != SEC_I_CONTINUE_NEEDED && status != SEC_I_COMPLETE_AND_CONTINUE) return false;
    }
    if (!established) return false;
    if (QueryContextAttributes(&impl_->context, SECPKG_ATTR_STREAM_SIZES, &impl_->sizes) != SEC_E_OK) {
        return false;
    }
    if (impl_->sizes.cbMaximumMessage == 0) impl_->sizes.cbMaximumMessage = 16384;
    impl_->open = true;
    return true;
#endif
}

qint64 TlsChannel::write(const char* data, const qint64 size) {
#if !defined(_WIN32)
    Q_UNUSED(data)
    Q_UNUSED(size)
    return -1;
#else
    if (!impl_->open || data == nullptr || size < 0) return -1;
    if (!impl_->sendEncrypted(data, static_cast<int>(size))) return -1;
    return size;
#endif
}

qint64 TlsChannel::write(const QByteArray& bytes) {
    return write(bytes.constData(), bytes.size());
}

bool TlsChannel::waitForBytesWritten(const int timeoutMs) {
    return impl_->socket.waitForBytesWritten(timeoutMs);
}

bool TlsChannel::waitForReadyRead(const int timeoutMs) {
#if !defined(_WIN32)
    Q_UNUSED(timeoutMs)
    return false;
#else
    if (!impl_->open) return false;
    if (!impl_->plain.isEmpty()) return true;
    return impl_->takePlain(timeoutMs);
#endif
}

QByteArray TlsChannel::readAll() {
    QByteArray out;
    out.swap(impl_->plain);
    return out;
}

void TlsChannel::flush() {
    impl_->socket.flush();
}

void TlsChannel::disconnectFromHost() {
#if defined(_WIN32)
    if (impl_->haveContext && impl_->identity && impl_->identity->impl_->haveCredentials) {
        DWORD token = SCHANNEL_SHUTDOWN;
        SecBuffer shutdown{};
        shutdown.pvBuffer = &token;
        shutdown.BufferType = SECBUFFER_TOKEN;
        shutdown.cbBuffer = sizeof(token);
        SecBufferDesc shutdownDesc{};
        shutdownDesc.ulVersion = SECBUFFER_VERSION;
        shutdownDesc.cBuffers = 1;
        shutdownDesc.pBuffers = &shutdown;
        if (ApplyControlToken(&impl_->context, &shutdownDesc) == SEC_E_OK) {
            SecBuffer output{};
            output.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc outputDesc{};
            outputDesc.ulVersion = SECBUFFER_VERSION;
            outputDesc.cBuffers = 1;
            outputDesc.pBuffers = &output;
            DWORD attributes = 0;
            const SECURITY_STATUS status = AcceptSecurityContext(&impl_->identity->impl_->credentials,
                &impl_->context, nullptr, contextFlags(), SECURITY_NATIVE_DREP, nullptr, &outputDesc,
                &attributes, nullptr);
            Q_UNUSED(status)
            if (output.cbBuffer > 0 && output.pvBuffer != nullptr) {
                writeSocket(&impl_->socket, static_cast<const char*>(output.pvBuffer),
                    static_cast<int>(output.cbBuffer));
                FreeContextBuffer(output.pvBuffer);
            }
        }
    }
#endif
    impl_->open = false;
    impl_->socket.disconnectFromHost();
}

bool TlsChannel::waitForDisconnected(const int timeoutMs) {
    if (impl_->socket.state() == QAbstractSocket::UnconnectedState) return true;
    return impl_->socket.waitForDisconnected(timeoutMs);
}

QAbstractSocket::SocketState TlsChannel::state() const {
    return impl_->socket.state();
}

} // namespace videovault::app
