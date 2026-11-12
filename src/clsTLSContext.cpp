#include "clsTLSContext.h"

#ifdef NETRECTOR_HAS_TLS
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
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
