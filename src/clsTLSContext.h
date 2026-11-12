#ifndef CLSTLSCONTEXT_H
#define CLSTLSCONTEXT_H

#include <cstdint>
#include <string>
#include <vector>

// forward declaration (BoringSSL)
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace netrector {

// ============================== TLSContext (BoringSSL) =======================
// Shared TLS settings (certificate, verification, ALPN). Create it once and share it between all
// sockets of all loops (it is thread-safe after creation). It must outlive the sockets using it.
//
//   client:  TLSContext* ctx = TLSContext::createClient(true, "cacert.pem");
//            sock->enableTLS(ctx);  sock->connectTo("example.com", 443);
//   server:  TLSContext* ctx = TLSContext::createServer("cert.pem", "key.pem");
//            // in the accept factory:  sock->enableTLS(ctx);
//
// CA certificates: Linux and Android load the system store by default. Windows, iOS and macOS have
// no PEM store BoringSSL can read, so pass a CA bundle with createClient(true, file) or
// createClientFromPEM(true, pem, len), e.g. the bundle shipped inside your app.
class TLSContext {
public:
    // false when the library was built without BoringSSL (NETRECTOR_HAS_TLS): createServer/
    // createClient return nullptr (reason in *error) and TCPSocket::enableTLS() returns false.
    static bool isSupported();

    // certChainFile: PEM (leaf + intermediates), privateKeyFile: PEM
    static TLSContext* createServer(const char* certChainFile, const char* privateKeyFile, std::string* error = nullptr);

    // verifyPeer=true: certificate and host name of the server are checked.
    // caFile=nullptr: system CA store (Linux, Android)
    static TLSContext* createClient(bool verifyPeer = true, const char* caFile = nullptr, std::string* error = nullptr);

    // like createClient, the CA certificates come from memory (PEM text, one or more certificates)
    static TLSContext* createClientFromPEM(bool verifyPeer, const char* caPem, size_t caPemLen, std::string* error = nullptr);

    ~TLSContext();
    TLSContext(const TLSContext&) = delete;
    TLSContext& operator=(const TLSContext&) = delete;

    // e.g. {"h2", "http/1.1"}; call before the first connection
    bool setALPN(const std::vector<std::string>& protocols);

    // Present for source compatibility with netRector. There is no worker thread for private key
    // operations in netRectorClient: always returns false.
    bool enableAsyncPrivateKey(int /*threads*/) { return false; }
    bool asyncPrivateKey() const { return false; }

    bool isServer() const { return m_isServer; }
    bool verifyPeer() const { return m_verifyPeer; }
    SSL_CTX* native() const { return m_ctx; }
    const std::string& alpnWire() const { return m_alpnWire; }

    // last BoringSSL errors of this thread (and clears the error queue)
    static std::string lastErrorString();

private:
    TLSContext(SSL_CTX* ctx, bool isServer, bool verifyPeer);

    SSL_CTX* m_ctx;
    bool m_isServer;
    bool m_verifyPeer;
    std::string m_alpnWire;     // wire format: len1 proto1 len2 proto2 ...
};

} // namespace netrector

#endif // CLSTLSCONTEXT_H
