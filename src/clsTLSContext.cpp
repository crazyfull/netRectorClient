#include "clsTLSContext.h"

#ifdef NETRECTOR_HAS_TLS
#include <openssl/bio.h>
#include <openssl/ec_key.h>
#include <openssl/nid.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace netrector {

#ifdef NETRECTOR_HAS_TLS

static int alpnSelectCallback(SSL* /*ssl*/, const uint8_t** out, uint8_t* outlen, const uint8_t* in, unsigned inlen, void* arg)
{
    TLSContext* self = static_cast<TLSContext*>(arg);
    const std::string& wire = self->alpnWire();
    if (wire.empty())
        return SSL_TLSEXT_ERR_NOACK;

    // the server's order of preference wins
    if (SSL_select_next_proto(const_cast<uint8_t**>(out), outlen,
                              reinterpret_cast<const uint8_t*>(wire.data()), (unsigned)wire.size(),
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    return SSL_TLSEXT_ERR_OK;
}

static SSL_CTX* newBaseContext()
{
    SSL_CTX* ctx = SSL_CTX_new(TLS_method());
    if (!ctx)
        return nullptr;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // PARTIAL_WRITE: SSL_write returns after every record (matches the non-blocking send path)
    // ACCEPT_MOVING_WRITE_BUFFER: a retry may come with another pointer
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    return ctx;
}

TLSContext::TLSContext(SSL_CTX *ctx, bool isServer, bool verifyPeer)
    : m_ctx(ctx), m_isServer(isServer), m_verifyPeer(verifyPeer)
{
}

TLSContext::~TLSContext()
{
    if (m_ctx)
        SSL_CTX_free(m_ctx);
}

std::string TLSContext::lastErrorString()
{
    std::string out;
    uint32_t code;
    char buf[256];
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!out.empty())
            out += "; ";
        out += buf;
    }
    return out;
}

TLSContext *TLSContext::createServer(const char *certChainFile, const char *privateKeyFile, std::string *error)
{
    ERR_clear_error();
    SSL_CTX* ctx = newBaseContext();
    if (!ctx) {
        if (error) *error = lastErrorString();
        return nullptr;
    }

    if (!certChainFile || !privateKeyFile
        || SSL_CTX_use_certificate_chain_file(ctx, certChainFile) != 1
        || SSL_CTX_use_PrivateKey_file(ctx, privateKeyFile, SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(ctx) != 1) {
        if (error) {
            *error = lastErrorString();
            if (error->empty()) *error = "certificate or private key not usable";
        }
        SSL_CTX_free(ctx);
        return nullptr;
    }

    static const uint8_t sessionIdContext[] = "netRector";
    SSL_CTX_set_session_id_context(ctx, sessionIdContext, sizeof(sessionIdContext) - 1);

    TLSContext* self = new TLSContext(ctx, true, false);
    SSL_CTX_set_alpn_select_cb(ctx, alpnSelectCallback, self);
    return self;
}

// adds every certificate of a PEM text to the trust store of ctx; returns how many were added
static int addPemCertificates(SSL_CTX* ctx, const char* pem, size_t len)
{
    BIO* bio = BIO_new_mem_buf(pem, (int)len);
    if (!bio)
        return 0;

    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    int added = 0;
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        if (X509_STORE_add_cert(store, cert) == 1)
            added++;
        X509_free(cert);
    }
    BIO_free(bio);
    ERR_clear_error();      // the loop ends with a "no start line" error: not an error
    return added;
}

// Generates an ECDSA P-256 key and a self-signed certificate valid for 10 years (CN and
// subjectAltName = commonName) and installs both into ctx. Returns false on any failure.
static bool installSelfSignedCertificate(SSL_CTX* ctx, const char* commonName)
{
    bool ok = false;
    EVP_PKEY* key = EVP_PKEY_new();
    EC_KEY* ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    X509* cert = X509_new();

    if (key && ec && cert && EC_KEY_generate_key(ec) == 1) {
        EVP_PKEY_assign_EC_KEY(key, ec);    // key owns ec from now on
        ec = nullptr;

        uint8_t serial[8];
        RAND_bytes(serial, sizeof(serial));
        uint64_t serialNumber = 0;
        for (uint8_t byte : serial)
            serialNumber = (serialNumber << 8) | byte;
        serialNumber &= 0x7fffffffffffffffULL;      // positive

        constexpr long OneDay = 24 * 60 * 60;
        X509_NAME* name = X509_get_subject_name(cert);

        ok = X509_set_version(cert, 2) == 1
             && ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert), serialNumber) == 1
             && X509_gmtime_adj(X509_getm_notBefore(cert), -OneDay) != nullptr
             && X509_gmtime_adj(X509_getm_notAfter(cert), 3650 * OneDay) != nullptr
             && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                           reinterpret_cast<const uint8_t*>(commonName), -1, -1, 0) == 1
             && X509_set_issuer_name(cert, name) == 1
             && X509_set_pubkey(cert, key) == 1;

        if (ok) {
            // subjectAltName: modern clients ignore the CN; failure here is not fatal
            const std::string altName = std::string("DNS:") + commonName;
            X509V3_CTX extensionContext;
            X509V3_set_ctx_nodb(&extensionContext);
            X509V3_set_ctx(&extensionContext, cert, cert, nullptr, nullptr, 0);
            if (X509_EXTENSION* extension = X509V3_EXT_nconf_nid(nullptr, &extensionContext, NID_subject_alt_name, altName.c_str())) {
                X509_add_ext(cert, extension, -1);
                X509_EXTENSION_free(extension);
            }
        }

        ok = ok && X509_sign(cert, key, EVP_sha256()) > 0
             && SSL_CTX_use_certificate(ctx, cert) == 1
             && SSL_CTX_use_PrivateKey(ctx, key) == 1;
    }

    if (ec)
        EC_KEY_free(ec);
    if (cert)
        X509_free(cert);
    if (key)
        EVP_PKEY_free(key);
    return ok;
}

TLSContext *TLSContext::createServerSelfSigned(const char *commonName, std::string *error)
{
    ERR_clear_error();
    SSL_CTX* ctx = newBaseContext();
    if (!ctx) {
        if (error) *error = lastErrorString();
        return nullptr;
    }

    const char* name = (commonName && *commonName) ? commonName : "localhost";
    if (!installSelfSignedCertificate(ctx, name)) {
        if (error) {
            *error = lastErrorString();
            if (error->empty()) *error = "can not generate a self-signed certificate";
        }
        SSL_CTX_free(ctx);
        return nullptr;
    }

    static const uint8_t sessionIdContext[] = "netRector";
    SSL_CTX_set_session_id_context(ctx, sessionIdContext, sizeof(sessionIdContext) - 1);

    TLSContext* self = new TLSContext(ctx, true, false);
    SSL_CTX_set_alpn_select_cb(ctx, alpnSelectCallback, self);
    return self;
}

TLSContext *TLSContext::createClient(bool verifyPeer, const char *caFile, std::string *error)
{
    ERR_clear_error();
    SSL_CTX* ctx = newBaseContext();
    if (!ctx) {
        if (error) *error = lastErrorString();
        return nullptr;
    }

    if (verifyPeer) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        int ok;
        if (caFile && *caFile) {
            ok = SSL_CTX_load_verify_locations(ctx, caFile, nullptr);
        } else {
#if defined(__ANDROID__)
            // Android keeps the system CAs as hashed files in this directory
            ok = SSL_CTX_load_verify_locations(ctx, nullptr, "/system/etc/security/cacerts");
#else
            ok = SSL_CTX_set_default_verify_paths(ctx);
#endif
        }

        if (ok != 1) {
            if (error) {
                *error = lastErrorString();
                if (error->empty())
                    *error = "can not load CA certificates (pass a CA bundle: createClient(true, caFile) or createClientFromPEM)";
            }
            SSL_CTX_free(ctx);
            return nullptr;
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    return new TLSContext(ctx, false, verifyPeer);
}

TLSContext *TLSContext::createClientFromPEM(bool verifyPeer, const char *caPem, size_t caPemLen, std::string *error)
{
    ERR_clear_error();
    SSL_CTX* ctx = newBaseContext();
    if (!ctx) {
        if (error) *error = lastErrorString();
        return nullptr;
    }

    if (verifyPeer) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

        if (!caPem || caPemLen == 0 || addPemCertificates(ctx, caPem, caPemLen) == 0) {
            if (error) *error = "no CA certificate found in the PEM data";
            SSL_CTX_free(ctx);
            return nullptr;
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    return new TLSContext(ctx, false, verifyPeer);
}

bool TLSContext::setALPN(const std::vector<std::string> &protocols)
{
    std::string wire;
    for (const std::string& p : protocols) {
        if (p.empty() || p.size() > 255)
            return false;
        wire.push_back(static_cast<char>(p.size()));
        wire += p;
    }

    m_alpnWire = wire;

    if (!m_isServer) {
        // 0 means success in this API
        if (SSL_CTX_set_alpn_protos(m_ctx, reinterpret_cast<const uint8_t*>(m_alpnWire.data()), (unsigned)m_alpnWire.size()) != 0)
            return false;
    }
    return true;
}

bool TLSContext::isSupported() { return true; }

#else   // !NETRECTOR_HAS_TLS: BoringSSL was not found at build time, TLS is disabled

static const char* const kNoTLS = "netRectorClient was built without TLS support (BoringSSL not found)";

TLSContext::~TLSContext() {}
bool TLSContext::isSupported() { return false; }
std::string TLSContext::lastErrorString() { return kNoTLS; }
bool TLSContext::setALPN(const std::vector<std::string>&) { return false; }

TLSContext* TLSContext::createServer(const char*, const char*, std::string* error)
{
    if (error) *error = kNoTLS;
    return nullptr;
}

TLSContext* TLSContext::createServerSelfSigned(const char*, std::string* error)
{
    if (error) *error = kNoTLS;
    return nullptr;
}

TLSContext* TLSContext::createClient(bool, const char*, std::string* error)
{
    if (error) *error = kNoTLS;
    return nullptr;
}

TLSContext* TLSContext::createClientFromPEM(bool, const char*, size_t, std::string* error)
{
    if (error) *error = kNoTLS;
    return nullptr;
}

#endif

} // namespace netrector
