#include "clsTCPSocket.h"
#include "clsLog.h"
#include "clsEventLoop.h"
#include "clsTLSContext.h"
#include "clsDNSLookup.h"

#ifdef NETRECTOR_HAS_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif
#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

#if defined(_WIN32)
    #include <mstcpip.h>
#endif

namespace netrector {

namespace {
// TLS helpers: without BoringSSL (NETRECTOR_HAS_TLS undefined) m_ssl is always nullptr
#ifdef NETRECTOR_HAS_TLS
inline void sslFree(SSL*& ssl) { if (ssl) { SSL_free(ssl); ssl = nullptr; } }
inline void sslShutdown(SSL* ssl) { SSL_shutdown(ssl); }
#else
inline void sslFree(SSL*&) {}
inline void sslShutdown(SSL*) {}
#endif

// result of ioRead/ioWrite
constexpr ssize_t IO_AGAIN = -1;
constexpr ssize_t IO_ERROR = -2;

// deferred work (EventLoop::defer)
constexpr int DEFER_READ = 1;
constexpr int DEFER_CLOSE = 2;

constexpr int MAX_IOV_BATCH = 64;

inline void setIntOption(NrSocket fd, int level, int name, int value)
{
    setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}
} // namespace

TCPSocket::TCPSocket()
{

}

TCPSocket::~TCPSocket()
{
    // normally the socket is closed already (fd == INVALID); this covers objects that were deleted
    // directly by their owner.
    if (m_dnsPending && m_pReactor) {
        m_pReactor->cancelDNS(this);
        m_dnsPending = false;
    }

    unregisterSocket();

    if (m_SocketContext.fd != NR_INVALID_SOCKET) {
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
    }

    sslFree(m_ssl);

    delete m_connInfo;
}

TCPSocket::ConnectInfo& TCPSocket::connInfo()
{
    if (!m_connInfo)
        m_connInfo = new ConnectInfo;
    return *m_connInfo;
}

TCPSocket::socketStatus TCPSocket::getStatus() const
{
    return status;
}

void TCPSocket::setStatus(socketStatus newStatus)
{
    // once closed the status cannot change any more
    if (status != Closed)
        status = newStatus;
}

void TCPSocket::updateLastActive()
{
    if (m_pReactor)
        m_SocketContext.lastActive = m_pReactor->getCachedNow();
}

EventLoop *TCPSocket::getReactor() const
{
    return m_pReactor;
}

void TCPSocket::setReactor(EventLoop *r) {
    m_pReactor = r;
}

// ----------------------------------------------------------------------------
// socket options
// ----------------------------------------------------------------------------
void TCPSocket::setSocketOption(NrSocket fd, int name, bool isEnable)
{
    setIntOption(fd, SOL_SOCKET, name, isEnable ? 1 : 0);
}

void TCPSocket::setSocketShared(NrSocket fd, bool isEnable)
{
#if defined(SO_REUSEPORT) && !defined(_WIN32)
    setSocketOption(fd, SO_REUSEPORT, isEnable);
#else
    (void)fd; (void)isEnable;
#endif
}

int TCPSocket::setSocketNonblocking(NrSocket fd) {
    return platform::setNonBlocking(fd) ? 0 : -1;
}

void TCPSocket::setSocketResourceAddress(NrSocket fd, bool isEnable)
{
#if defined(_WIN32)
    // SO_REUSEADDR means "steal the port" on Windows: never set it
    (void)fd; (void)isEnable;
#else
    setSocketOption(fd, SO_REUSEADDR, isEnable);
#endif
}

void TCPSocket::setSocketNoDelay(NrSocket fd, bool isEnable)
{
    setIntOption(fd, IPPROTO_TCP, TCP_NODELAY, isEnable ? 1 : 0);
}

void TCPSocket::setSocketCork(NrSocket fd, bool isEnable)
{
#if defined(TCP_CORK)
    setIntOption(fd, IPPROTO_TCP, TCP_CORK, isEnable ? 1 : 0);
#else
    (void)fd; (void)isEnable;
#endif
}

void TCPSocket::setSocketKeepAlive(NrSocket fd, bool isEnable) {
    if (fd == NR_INVALID_SOCKET)
        return;

    setIntOption(fd, SOL_SOCKET, SO_KEEPALIVE, isEnable ? 1 : 0);
    if (!isEnable)
        return;

#if defined(_WIN32)
    tcp_keepalive ka;
    ka.onoff = 1;
    ka.keepalivetime = KEEPIDLE_SECS * 1000;
    ka.keepaliveinterval = KEEPINTVL_SECS * 1000;
    DWORD bytes = 0;
    WSAIoctl(fd, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytes, nullptr, nullptr);
#else
    #if defined(TCP_KEEPIDLE)
    setIntOption(fd, IPPROTO_TCP, TCP_KEEPIDLE, KEEPIDLE_SECS);
    #elif defined(TCP_KEEPALIVE)
    setIntOption(fd, IPPROTO_TCP, TCP_KEEPALIVE, KEEPIDLE_SECS);      // Apple: idle time
    #endif
    #if defined(TCP_KEEPINTVL)
    setIntOption(fd, IPPROTO_TCP, TCP_KEEPINTVL, KEEPINTVL_SECS);
    #endif
    #if defined(TCP_KEEPCNT)
    setIntOption(fd, IPPROTO_TCP, TCP_KEEPCNT, KEEPCNT_MAX);
    #endif
#endif
}

// for gaming and VOIP
void TCPSocket::setSocketLowDelay(NrSocket fd, bool isEnable)
{
#if defined(IP_TOS) && defined(IPTOS_LOWDELAY)
    setIntOption(fd, IPPROTO_IP, IP_TOS, isEnable ? IPTOS_LOWDELAY : 0);
#else
    (void)fd; (void)isEnable;
#endif
}


SocketContext *TCPSocket::getSocketContext()
{
    return &m_SocketContext;
}

// ----------------------------------------------------------------------------
// callback dispatch
// ----------------------------------------------------------------------------
void TCPSocket::handleOnData(const uint8_t *d, size_t n) {
    if(m_onData){
        m_onData(m_callbacksArg, d, n);
    }else{
        onReceiveData(d, n);
    }
}

void TCPSocket::handleOnAccepted()
{
    if(m_onAccepted){
        m_onAccepted(m_callbacksArg);
    }else{
        onAccepted();
    }
}

void TCPSocket::handleOnClose()
{
    if(m_onClose){
        m_onClose(m_callbacksArg);
    }else{
        onClose();
    }
}

void TCPSocket::handleOnConnectFailed()
{
    if(m_onConnectFailed){
        m_onConnectFailed(m_callbacksArg);
    }else{
        onConnectFailed();
    }
}

void TCPSocket::handleOnConnecting()
{
    if(m_onConnecting){
        m_onConnecting(m_callbacksArg);
    }else{
        onConnecting();
    }
}

void TCPSocket::handleOnConnected()
{
    if(m_onConnected){
        m_onConnected(m_callbacksArg);
    }else{
        onConnected();
    }
}

void TCPSocket::handleOnPause() {
    if (m_onPause)
        m_onPause(m_callbacksArg);
    else
        onPause();
}

void TCPSocket::handleOnResume() {
    if (m_onResume)
        m_onResume(m_callbacksArg);
    else
        onResume();
}

void TCPSocket::handleOnEOF()
{
    if (m_onEOF)
        m_onEOF(m_callbacksArg);
    else
        onReadEOF();
}

// Default: a socket whose send queue is full stops reading from itself (right for request/response
// protocols). Proxies control the reading of the other side with setOnPause/setOnResume.
void TCPSocket::onPause()
{
    pause_reading();
}

void TCPSocket::onResume()
{
    resume_reading();
}

// ----------------------------------------------------------------------------
// registration in the loop
// ----------------------------------------------------------------------------

// What the loop has to wait for, derived from the state of the socket.
uint32_t TCPSocket::wantedInterest() const
{
    switch (status) {
    case Connecting:
        return EV_WRITE;

    case Handshaking:
        return EV_READ | (m_tlsHandshakeWantsWrite ? (uint32_t)EV_WRITE : 0u);

    case Connected: {
        uint32_t i = 0;
        if ((!m_readPaused && !m_readEOF) || m_tlsWriteWantsRead)
            i |= EV_READ;
        if (!m_SocketContext.writeQueue.empty() || m_tlsReadWantsWrite)
            i |= EV_WRITE;
        return i;
    }

    case Closing:
        if (m_lingering)
            return EV_READ;
        return (m_SocketContext.writeQueue.empty() ? 0u : (uint32_t)EV_WRITE) | (m_tlsWriteWantsRead ? (uint32_t)EV_READ : 0u);

    default:
        return 0;
    }
}

void TCPSocket::updateInterest()
{
    if (!m_registered || !m_pReactor)
        return;

    uint32_t wanted = wantedInterest();
    if (wanted == m_interest)
        return;

    m_interest = wanted;
    m_pReactor->setInterest(m_SocketContext.token, wanted);
}

bool TCPSocket::registerSocket(NrSocket fd, uint32_t interest)
{
    if (!m_pReactor)
        return false;

    uint64_t token = m_pReactor->addObject(this, fd, interest, EventLoop::KindTcp);
    if (token == 0)
        return false;

    m_SocketContext.fd = fd;
    m_SocketContext.token = token;
    m_registered = true;
    m_interest = interest;
    return true;
}

void TCPSocket::unregisterSocket()
{
    if (m_registered && m_pReactor)
        m_pReactor->removeObject(m_SocketContext.token);

    m_registered = false;
    m_SocketContext.token = 0;
    m_interest = 0;
}

// ----------------------------------------------------------------------------
// read control
// ----------------------------------------------------------------------------
void TCPSocket::pause_reading() {
    if (m_readPaused)
        return;

    m_readPaused = true;
    updateInterest();
}

void TCPSocket::resume_reading() {
    if (!m_readPaused)
        return;

    m_readPaused = false;
    if (!m_pReactor || !m_registered || status != Connected || m_readEOF)
        return;

    updateInterest();   // level-triggered: data waiting in the kernel is reported at once

    // already decrypted data inside SSL is invisible to the loop
    if (m_ssl && !m_deferredRead) {
        m_deferredRead = true;
        m_pReactor->defer(m_SocketContext.token, DEFER_READ);
    }
}

bool TCPSocket::isReadPaused() const
{
    return m_readPaused;
}

void TCPSocket::setAllowHalfClose(bool enable)
{
    m_allowHalfClose = enable;
}

void TCPSocket::setAutoDelete(bool enable)
{
    m_autoDelete = enable;
}

void TCPSocket::setIdleTimeout(uint32_t seconds)
{
    m_idleTimeout = seconds;
}

int TCPSocket::socketError()
{
    return platform::socketError(m_SocketContext.fd);
}

int TCPSocket::getErrorCode()
{
    if (m_lastError != 0)
        return m_lastError;

    return socketError();
}

// ----------------------------------------------------------------------------
// close
// ----------------------------------------------------------------------------
void TCPSocket::close(bool force) {
    if (status == Closed)
        return;

    if (m_closeReason == NotClosed)
        m_closeReason = ClosedByApp;

    if (!m_pReactor) {
        // never attached to a loop: nothing to close
        status = Closed;
        return;
    }

    if (force || m_abortPending) {
        finalizeClose();
        return;
    }

    if (status == Closing)
        return; // already closing

    if (status != Connected) {
        // Ready / Connecting / Handshaking: nothing to flush
        finalizeClose();
        return;
    }

    // graceful: flush the send queue first
    m_pendingClose = true;
    setStatus(Closing);
    updateLastActive();

    if (m_SocketContext.writeQueue.empty()) {
        drainComplete();
    } else {
        // no more data for the application; only wait for the socket to become writable
        updateInterest();
    }
}

// The send queue is empty: send FIN. If the peer has not sent its FIN yet we wait for it
// (lingering close): closing a descriptor that still has unread data would send RST and
// destroy the data the peer has not read yet.
void TCPSocket::drainComplete()
{
    if (m_ssl && !m_writeShutdown)
        sslShutdown(m_ssl);     // close_notify (best effort)

    ::shutdown(m_SocketContext.fd, NR_SHUT_WR);
    m_writeShutdown = true;

    if (m_readEOF) {
        finalizeClose();
        return;
    }

    m_lingering = true;
    updateLastActive();
    updateInterest();
    lingerDiscard();
}

void TCPSocket::lingerDiscard()
{
    // separate buffer: close() may be called from a callback of another socket that is parsing
    // the loop's shared read buffer right now.
    char buf[4096];

    for (int i = 0; i < 64; i++) {
        ssize_t n = platform::recvData(m_SocketContext.fd, buf, sizeof(buf));
        if (n > 0)
            continue;

        if (n == 0) {
            finalizeClose();
            return;
        }

        int e = platform::lastError();
        if (platform::isInterrupted(e))
            continue;

        if (platform::isWouldBlock(e))
            return; // wait for the FIN (or LINGER_TIMEOUT_SECS)

        finalizeClose();
        return;
    }

    // the peer keeps sending: do not wait any longer
    finalizeClose();
}

void TCPSocket::shutdownWrite()
{
    if (status != Connected || m_shutdownWrPending)
        return;

    m_shutdownWrPending = true;
    if (m_SocketContext.writeQueue.empty())
        doShutdownWrite();
    // otherwise it happens when the send queue is empty (onWritable)
}

void TCPSocket::doShutdownWrite()
{
    if (m_writeShutdown)
        return;

    if (m_ssl)
        sslShutdown(m_ssl);

    ::shutdown(m_SocketContext.fd, NR_SHUT_WR);
    m_writeShutdown = true;

    // both directions are finished
    if (m_readEOF)
        finalizeClose();
}

// The only real close. Runs exactly once.
void TCPSocket::finalizeClose()
{
    if (status == Closed)
        return;

    if (m_closeReason == NotClosed)
        m_closeReason = (m_lastError != 0) ? ClosedByError : ClosedByApp;
    status = Closed;

    if (m_dnsPending) {
        m_pReactor->cancelDNS(this);
        m_dnsPending = false;
    }

    unregisterSocket();

    if (m_SocketContext.fd != NR_INVALID_SOCKET) {
        platform::closeSocket(m_SocketContext.fd);

        // the descriptor number is free for the next connection now
        m_SocketContext.fd = NR_INVALID_SOCKET;
    }

    sslFree(m_ssl);

    m_SocketContext.writeQueue.clear();

    if (m_connInfo) {
        delete m_connInfo;
        m_connInfo = nullptr;
    }

    handleOnClose();

    if (m_autoDelete)
        m_pReactor->deleteLater(this);
}

// Error inside code called by the application (send): onClose must not run in the middle of the
// caller, so the close is moved to the end of the loop iteration.
void TCPSocket::abortLater()
{
    if (m_abortPending || status == Closed)
        return;

    if (m_closeReason == NotClosed)
        m_closeReason = ClosedByError;

    m_abortPending = true;
    if (m_registered)
        m_pReactor->defer(m_SocketContext.token, DEFER_CLOSE);
}

void TCPSocket::onDeferred(int kind)
{
    if (status == Closed)
        return;

    if (kind == DEFER_CLOSE) {
        finalizeClose();
        return;
    }

    if (kind == DEFER_READ) {
        m_deferredRead = false;
        if (m_abortPending) {
            finalizeClose();
            return;
        }
        if (status == Connected)
            onReadable();
        updateInterest();
    }
}

void TCPSocket::onLoopShutdown()
{
    close(true);
}

void TCPSocket::onMaintenance(uint64_t nowSeconds)
{
    checkTimeout(nowSeconds);
    if (status != Closed)
        onTick(nowSeconds);
}

void TCPSocket::checkTimeout(uint64_t now)
{
    if (status == Closed)
        return;

    uint64_t idle = (now > m_SocketContext.lastActive) ? (now - m_SocketContext.lastActive) : 0;

    switch (status) {
    case Connecting:
    case Handshaking:
        if (idle >= (uint64_t)CONNECT_TIMEOUT_SECS) {
            m_closeReason = ClosedByTimeout;
            m_lastError = err::TimedOut;
            if (m_isAccepted)
                finalizeClose();
            else
                failConnect(err::TimedOut);
        }
        break;

    case Closing:
        if (idle >= (uint64_t)(m_lingering ? LINGER_TIMEOUT_SECS : CLOSING_TIMEOUT_SECS))
            finalizeClose();    // the original reason (app/peer) is kept
        break;

    case Connected:
        if (m_idleTimeout != 0 && idle >= m_idleTimeout) {
            m_closeReason = ClosedByTimeout;
            m_lastError = err::TimedOut;
            finalizeClose();
        }
        break;

    default:
        break;
    }
}

// ----------------------------------------------------------------------------
// connect
// ----------------------------------------------------------------------------
void TCPSocket::failConnect(int e)
{
    if (status == Closed)
        return;

    // other addresses of the same name (if DNS returned several)
    if (tryNextAddress(e))
        return;

    m_lastError = e;
    m_closeReason = (e == err::TimedOut) ? ClosedByTimeout : ConnectFailed;
    if (!m_connectFailNotified) {
        m_connectFailNotified = true;
        handleOnConnectFailed();
    }

    finalizeClose();
}

static bool parseLiteralAddress(const char* host, uint16_t port, sockaddr_storage& ss, socklen_t& len)
{
    memset(&ss, 0, sizeof(ss));

    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&ss);
    if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }

    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
    if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        return true;
    }

    return false;
}

bool TCPSocket::connectTo(const char* host, uint16_t port)
{
    if(!m_pReactor){
        NR_LOG(LOG_INFO, "error: EventLoop not set (call setReactor first)");
        return false;
    }

    if(!host || !*host || getStatus() != Ready){
        return false;
    }

    m_SocketContext.port = port;
    connInfo().host = host;
    m_isAccepted = false;

    // an IP literal needs no DNS
    sockaddr_storage ss;
    socklen_t len = 0;
    if (parseLiteralAddress(host, port, ss, len)) {
        setStatus(Connecting);
        updateLastActive();
        startConnect(reinterpret_cast<sockaddr*>(&ss), len);
        return true;
    }

    setStatus(Connecting);
    updateLastActive();
    m_dnsPending = true;
    if (!m_pReactor->getIPbyName(host, connect_cb, this, DNSLookup::Any)) {
        m_dnsPending = false;
        status = Ready;
        return false;
    }

    return true;
}

void TCPSocket::connect_cb(const char * /*hostname*/, char **ips, size_t count, DNSLookup::QUERY_TYPE /*qtype*/, void *p)
{
    TCPSocket *self = static_cast<TCPSocket*>(p);
    if(!self){
        return;
    }

    self->onResolved(ips, count);
}

void TCPSocket::onResolved(char **ips, size_t count)
{
    m_dnsPending = false;
    if (status != Connecting)
        return;

    if (!ips || count == 0) {
        failConnect(err::HostUnreach);
        return;
    }

    // keep every address; if the first does not connect the next one is tried
    ConnectInfo& info = connInfo();
    info.addrs.clear();
    for (size_t i = 0; i < count && i < 8; i++)
        info.addrs.push_back(ips[i]);
    info.nextAddr = 0;

    sockaddr_storage ss;
    socklen_t len = 0;
    if (!parseLiteralAddress(info.addrs[0].c_str(), m_SocketContext.port, ss, len)) {
        failConnect(err::HostUnreach);
        return;
    }
    info.nextAddr = 1;

    updateLastActive();
    startConnect(reinterpret_cast<sockaddr*>(&ss), len);
}

// After a failed connect to one address: try the next address of the same name.
// true: a new attempt started (the socket is still Connecting)
bool TCPSocket::tryNextAddress(int e)
{
    if (status != Connecting || !m_connInfo || m_connInfo->nextAddr >= m_connInfo->addrs.size())
        return false;

    // only errors that belong to the host or the network (not a general timeout)
    if (e != err::ConnRefused && e != err::HostUnreach && e != err::NetUnreach && e != err::TimedOut && e != err::ConnReset)
        return false;

    sockaddr_storage ss;
    socklen_t len = 0;
    const std::string& ip = m_connInfo->addrs[m_connInfo->nextAddr++];
    if (!parseLiteralAddress(ip.c_str(), m_SocketContext.port, ss, len))
        return false;

    // drop the previous attempt
    unregisterSocket();
    if (m_SocketContext.fd != NR_INVALID_SOCKET) {
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
    }

    updateLastActive();
    startConnect(reinterpret_cast<sockaddr*>(&ss), len);
    return status == Connecting;
}

void TCPSocket::startConnect(const sockaddr *addr, socklen_t addrLen)
{
    NrSocket fd = platform::createSocket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == NR_INVALID_SOCKET) {
        failConnect(platform::lastError());
        return;
    }

    TCPSocket::setSocketNoDelay(fd, true);

    int ret;
    int e = 0;
    do {
        ret = ::connect(fd, addr, addrLen);
        e = ret == 0 ? 0 : platform::lastError();
    } while (ret != 0 && platform::isInterrupted(e) && false);

    // a non-blocking connect is "in progress" (an interrupted one continues in the background)
    if (ret != 0 && e != err::InProgress && !platform::isInterrupted(e)) {
        platform::closeSocket(fd);
        failConnect(e);
        return;
    }

    // the result (even an immediate success) is reported as "writable"
    if (!registerSocket(fd, EV_WRITE)) {
        platform::closeSocket(fd);
        failConnect(err::NoMemory);
        return;
    }

    updateLastActive();
    handleOnConnecting();
}

void TCPSocket::onConnectReady()
{
    int e = socketError();
    if (e != 0) {
        failConnect(e);
        return;
    }

    updateLastActive();

    if (m_tlsCtx) {
        if (!tlsStart(false)) {
            failConnect(err::Protocol);
            return;
        }
        updateInterest();
        tlsHandshake();
        updateInterest();
        return;
    }

    // connected successfully
    setStatus(Connected);
    updateInterest();
    handleOnConnected();
    updateInterest();
}

void TCPSocket::_accepted(NrSocket fd)
{
    if(getStatus() != Ready || !m_pReactor){
        platform::closeSocket(fd);
        // object of a factory that was not used: release it like a normal close
        if (m_pReactor && getStatus() == Ready)
            finalizeClose();
        return;
    }

    m_isAccepted = true;
    updateLastActive();

    setSocketNoDelay(fd, true);
    setSocketKeepAlive(fd, true);

    if (!registerSocket(fd, EV_READ)) {
        // connection limit or out of memory: close so that the object is released
        m_SocketContext.fd = fd;
        finalizeClose();
        return;
    }

    if (m_tlsCtx) {
        if (!tlsStart(true)) {
            finalizeClose();
            return;
        }
        updateInterest();
        tlsHandshake();     // onAccepted is called after the handshake
        updateInterest();
        return;
    }

    setStatus(TCPSocket::Connected);
    updateInterest();
    handleOnAccepted();
    updateInterest();
}

// ----------------------------------------------------------------------------
// TLS
// ----------------------------------------------------------------------------
bool TCPSocket::enableTLS(TLSContext *ctx, const char *serverName)
{
    if (!ctx || status != Ready)
        return false;

#ifndef NETRECTOR_HAS_TLS
    NR_LOG(LOG_ERROR, "enableTLS: netRectorClient was built without TLS support (BoringSSL not found)");
    return false;
#endif

    m_tlsCtx = ctx;
    if (serverName && *serverName)
        connInfo().serverName = serverName;
    return true;
}

bool TCPSocket::isTLS() const
{
    return m_tlsCtx != nullptr;
}

const char *TCPSocket::tlsVersion() const
{
#ifdef NETRECTOR_HAS_TLS
    return m_ssl ? SSL_get_version(m_ssl) : "";
#else
    return "";
#endif
}

std::string TCPSocket::tlsALPN() const
{
#ifdef NETRECTOR_HAS_TLS
    if (!m_ssl)
        return std::string();

    const uint8_t* data = nullptr;
    unsigned len = 0;
    SSL_get0_alpn_selected(m_ssl, &data, &len);
    return std::string(reinterpret_cast<const char*>(data), len);
#else
    return std::string();
#endif
}

#ifdef NETRECTOR_HAS_TLS

bool TCPSocket::tlsStart(bool isServer)
{
    ERR_clear_error();
    m_ssl = SSL_new(m_tlsCtx->native());
    if (!m_ssl)
        return false;

    if (SSL_set_fd(m_ssl, (int)m_SocketContext.fd) != 1) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
        return false;
    }

    if (isServer) {
        SSL_set_accept_state(m_ssl);
    } else {
        SSL_set_connect_state(m_ssl);

        std::string name;
        if (m_connInfo)
            name = m_connInfo->serverName.empty() ? m_connInfo->host : m_connInfo->serverName;
        if (!name.empty()) {
            unsigned char tmp[sizeof(in6_addr)];
            bool isIP = inet_pton(AF_INET, name.c_str(), tmp) == 1 || inet_pton(AF_INET6, name.c_str(), tmp) == 1;

            // SNI only for names (not for IP addresses)
            if (!isIP)
                SSL_set_tlsext_host_name(m_ssl, name.c_str());

            if (m_tlsCtx->verifyPeer()) {
                X509_VERIFY_PARAM* param = SSL_get0_param(m_ssl);
                int ok = isIP ? X509_VERIFY_PARAM_set1_ip_asc(param, name.c_str())
                              : X509_VERIFY_PARAM_set1_host(param, name.c_str(), name.size());
                if (ok != 1) {
                    SSL_free(m_ssl);
                    m_ssl = nullptr;
                    return false;
                }
            }
        }
    }

    m_tlsHandshakeWantsWrite = false;
    setStatus(Handshaking);
    updateLastActive();
    return true;
}

void TCPSocket::tlsHandshake()
{
    ERR_clear_error();
    int ret = SSL_do_handshake(m_ssl);
    if (ret == 1) {
        m_tlsHandshakeWantsWrite = false;
        setStatus(Connected);
        updateLastActive();
        updateInterest();

        if (m_isAccepted)
            handleOnAccepted();
        else
            handleOnConnected();
        return;
    }

    int e = SSL_get_error(m_ssl, ret);
    if (e == SSL_ERROR_WANT_READ) {
        m_tlsHandshakeWantsWrite = false;
        return;
    }

    if (e == SSL_ERROR_WANT_WRITE) {
        m_tlsHandshakeWantsWrite = true;
        return;
    }

    // handshake failed (certificate, protocol, or the connection was lost)
    m_lastError = err::Protocol;
    ERR_clear_error();
    if (m_isAccepted)
        finalizeClose();
    else
        failConnect(err::Protocol);
}

#else   // !NETRECTOR_HAS_TLS: never reached (enableTLS() refuses), kept so the class links

bool TCPSocket::tlsStart(bool) { return false; }
void TCPSocket::tlsHandshake() {}

#endif

// ----------------------------------------------------------------------------
// io
// ----------------------------------------------------------------------------
// >0: bytes | 0: EOF | IO_AGAIN | IO_ERROR
ssize_t TCPSocket::ioRead(char *buf, size_t cap)
{
#ifdef NETRECTOR_HAS_TLS
    if (m_ssl) {
        ERR_clear_error();
        int n = SSL_read(m_ssl, buf, (int)std::min<size_t>(cap, INT_MAX));
        if (n > 0)
            return n;

        switch (SSL_get_error(m_ssl, n)) {
        case SSL_ERROR_WANT_READ:
            return IO_AGAIN;

        case SSL_ERROR_WANT_WRITE:
            m_tlsReadWantsWrite = true;
            updateInterest();
            return IO_AGAIN;

        case SSL_ERROR_ZERO_RETURN:
            return 0;   // close_notify

        case SSL_ERROR_SYSCALL:
            // TCP closed without close_notify also counts as EOF
            if (ERR_peek_error() == 0 && n == 0)
                return 0;
            m_lastError = platform::lastError();
            ERR_clear_error();
            return IO_ERROR;

        default:
            ERR_clear_error();
            return IO_ERROR;
        }
    }
#endif

    for (;;) {
        ssize_t n = platform::recvData(m_SocketContext.fd, buf, cap);
        if (n >= 0)
            return n;

        int e = platform::lastError();
        if (platform::isInterrupted(e))
            continue;

        if (platform::isWouldBlock(e))
            return IO_AGAIN;

        m_lastError = e;
        return IO_ERROR;
    }
}

// >0: bytes | IO_AGAIN | IO_ERROR
ssize_t TCPSocket::ioWrite(const void *data, size_t len)
{
#ifdef NETRECTOR_HAS_TLS
    if (m_ssl) {
        ERR_clear_error();
        int n = SSL_write(m_ssl, data, (int)std::min<size_t>(len, INT_MAX));
        if (n > 0)
            return n;

        switch (SSL_get_error(m_ssl, n)) {
        case SSL_ERROR_WANT_WRITE:
            return IO_AGAIN;

        case SSL_ERROR_WANT_READ:
            m_tlsWriteWantsRead = true;
            return IO_AGAIN;

        default:
            ERR_clear_error();
            return IO_ERROR;
        }
    }
#endif

    for (;;) {
        ssize_t n = platform::sendData(m_SocketContext.fd, data, len);
        if (n >= 0)
            return n > 0 ? n : IO_AGAIN;

        int e = platform::lastError();
        if (platform::isInterrupted(e))
            continue;

        if (platform::isWouldBlock(e) || e == err::NoBuffers)
            return IO_AGAIN;

        m_lastError = e;
        return IO_ERROR;
    }
}

void TCPSocket::onIoEvents(uint32_t ev)
{
    if (status == Closed)
        return;

    if (m_abortPending) {
        finalizeClose();
        return;
    }

    if (status == Connecting) {
        if (ev & (EV_ERROR | EV_HUP)) {
            int e = socketError();
            failConnect(e != 0 ? e : err::ConnRefused);
            return;
        }

        if (ev & EV_WRITE)
            onConnectReady();
        return;     // data that came with the connect is reported by the next iteration
    }

    if (status == Handshaking) {
        if (ev & (EV_ERROR | EV_HUP)) {
            if (m_isAccepted)
                finalizeClose();
            else
                failConnect(socketError());
            return;
        }

        tlsHandshake();
        if (status == Connected) {
            // application data can arrive in the same read as the last handshake message
            onReadable();
        }
        updateInterest();
        return;
    }

    if (status == Closing) {
        if (m_lingering) {
            if (ev & (EV_READ | EV_HUP | EV_ERROR))
                lingerDiscard();
            return;
        }

        // flushing the send queue
        if (ev & (EV_ERROR | EV_HUP)) {
            finalizeClose();
            return;
        }

        if (ev & EV_WRITE)
            onWritable();
        updateInterest();
        return;
    }

    if (status != Connected)
        return;

    if (ev & (EV_READ | EV_HUP | EV_ERROR)) {
        if (!m_readPaused && !m_readEOF) {
            onReadable();           // reads the data, the EOF or the error
            if (status == Closed)
                return;
        } else if (ev & (EV_HUP | EV_ERROR)) {
            // the connection is dead and nobody reads: nothing is left to wait for
            if (m_closeReason == NotClosed)
                m_closeReason = (ev & EV_ERROR) ? ClosedByError : ClosedByPeer;
            if (ev & EV_ERROR)
                m_lastError = socketError();
            finalizeClose();
            return;
        }
    }

    if ((ev & EV_WRITE) && (status == Connected || (status == Closing && !m_lingering)))
        onWritable();

    updateInterest();
}

void TCPSocket::onReadable()
{
    if (status != Connected)
        return;

    // SSL_write was waiting for the peer: continue it first
    if (m_tlsWriteWantsRead) {
        m_tlsWriteWantsRead = false;
        if (!flushQueue())
            return;
        if (status != Connected)
            return;
    }

    if (m_readPaused || m_readEOF)
        return;

    // shared read buffer of the loop: the data must be consumed inside the callback
    char* buf = m_pReactor->readBuffer();
    size_t cap = m_pReactor->readBufferSize();

    for (int round = 0; ; round++)
    {
        if (round >= MAX_READS_PER_EVENT) {
            // other sockets are waiting; with TLS the rest is invisible to the loop, so continue
            // at the end of this iteration
            if (m_ssl && !m_deferredRead) {
                m_deferredRead = true;
                m_pReactor->defer(m_SocketContext.token, DEFER_READ);
            }
            break;
        }

        ssize_t bytesRec = ioRead(buf, cap);

        if(bytesRec > 0)  {
            updateLastActive();
            handleOnData(reinterpret_cast<uint8_t*>(buf), (size_t)bytesRec);

            // the callback may have closed or paused the socket
            if (status != Connected || m_readPaused || m_abortPending)
                break;

            // plain TCP, short read: the kernel buffer is empty. (Level-triggered: if more data
            // arrives the loop reports it again.)
            if (!m_ssl && (size_t)bytesRec < cap)
                break;

            continue;
        }

        if(bytesRec == IO_AGAIN)
            break;

        if(bytesRec == 0) {
            handleReadEOF();
            break;
        }

        // IO_ERROR
        finalizeClose();
        break;
    }
}

void TCPSocket::handleReadEOF()
{
    m_readEOF = true;
    if (m_closeReason == NotClosed)
        m_closeReason = ClosedByPeer;

    if (!m_allowHalfClose) {
        // flush the send queue, then close
        close(false);
        return;
    }

    // half-close: the socket stays open for writing
    updateInterest();
    handleOnEOF();

    if (status == Connected && m_writeShutdown)
        finalizeClose();    // both directions are finished
}

bool TCPSocket::send(const void* data, size_t len) {
    if (!data || len == 0)
        return true;

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == NR_INVALID_SOCKET)
        return false;

    SendQueue* queue = &m_SocketContext.writeQueue;
    if (!queueHasRoom(len))
        return false;
    const char* p = static_cast<const char*>(data);

    // empty queue: try to send directly
    if (queue->empty()) {
        while (len > 0) {
            ssize_t n = ioWrite(p, len);
            if (n > 0) {
                p += n;
                len -= (size_t)n;
                continue;
            }

            if (n == IO_AGAIN)
                break;  // kernel buffer is full

            abortLater();
            return false;
        }

        updateLastActive();
        if (len == 0) {
            updateInterest();
            return true; // everything sent
        }
    }

    if (!queue->push(p, len)) {
        // out of memory
        m_lastError = err::NoMemory;
        abortLater();
        return false;
    }

    // data is waiting: wake up when the socket becomes writable
    updateInterest();

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

bool TCPSocket::sendv(const iovec *iov, int iovcnt)
{
    if (!iov || iovcnt <= 0)
        return true;

    if (iovcnt == 1)
        return send(iov[0].iov_base, iov[0].iov_len);

    size_t total = 0;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;

    if (total == 0)
        return true;

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == NR_INVALID_SOCKET)
        return false;

    SendQueue* queue = &m_SocketContext.writeQueue;
    if (!queueHasRoom(total))
        return false;

    // non-empty queue: append the pieces (one copy)
    if (!m_ssl && !queue->empty()) {
        if (!queue->pushv(iov, iovcnt)) {
            m_lastError = err::NoMemory;
            abortLater();
            return false;
        }
        updateInterest();
        if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
            m_writeBlocked = true;
            handleOnPause();
        }
        return true;
    }

    // TLS: the pieces go into one buffer so that they become one record
    if (m_ssl) {
        if (total <= m_pReactor->scratchBufferSize()) {
            char* scratch = m_pReactor->scratchBuffer();
            size_t off = 0;
            for (int i = 0; i < iovcnt; i++) {
                memcpy(scratch + off, iov[i].iov_base, iov[i].iov_len);
                off += iov[i].iov_len;
            }
            return send(scratch, total);
        }

        for (int i = 0; i < iovcnt; i++) {
            if (!send(iov[i].iov_base, iov[i].iov_len))
                return false;
        }
        return true;
    }

    // plain TCP with an empty queue: gather write straight to the kernel
    ssize_t n;
    int e = 0;
    for (;;) {
        n = platform::sendIov(m_SocketContext.fd, iov, iovcnt);
        if (n >= 0)
            break;
        e = platform::lastError();
        if (!platform::isInterrupted(e))
            break;
    }

    if (n < 0) {
        if (!platform::isWouldBlock(e) && e != err::NoBuffers) {
            m_lastError = e;
            abortLater();
            return false;
        }
        n = 0;
    }

    updateLastActive();
    if ((size_t)n == total)
        return true;

    // the rest goes into the queue (one copy, transactional)
    struct iovec restStack[64];
    std::vector<struct iovec> restHeap;
    struct iovec* rest = restStack;
    if (iovcnt > 64) {
        restHeap.resize((size_t)iovcnt);
        rest = restHeap.data();
    }
    int restCnt = 0;
    size_t skip = (size_t)n;
    for (int i = 0; i < iovcnt; i++) {
        if (skip >= iov[i].iov_len) {
            skip -= iov[i].iov_len;
            continue;
        }
        rest[restCnt].iov_base = static_cast<char*>(iov[i].iov_base) + skip;
        rest[restCnt].iov_len = iov[i].iov_len - skip;
        restCnt++;
        skip = 0;
    }

    if (!queue->pushv(rest, restCnt)) {
        m_lastError = err::NoMemory;
        abortLater();
        return false;
    }

    updateInterest();

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

// Hard cap of the send queue (setMaxSendQueue). Above it send() is refused (the connection stays
// open; the application is expected to follow onPause).
bool TCPSocket::queueHasRoom(size_t len)
{
    size_t limit = m_maxSendQueue ? m_maxSendQueue : MAX_SEND_QUEUE;
    if (m_SocketContext.writeQueue.size() + len > limit) {
        m_lastError = err::NoBuffers;
        return false;
    }
    return true;
}

// Sends the queue as far as the kernel accepts it. false: the socket was closed.
bool TCPSocket::flushQueue()
{
    SendQueue* queue = &m_SocketContext.writeQueue;

    while (!queue->empty()) {
        ssize_t n;
        SendQueue::Chunk* head = queue->front();

        if (m_ssl) {
            // a retried SSL_write must use the same data: always from the start of the queue
            n = ioWrite(head->unsent(), head->avail());
        } else {
            // several chunks in one gather write
            struct iovec iov[MAX_IOV_BATCH];
            int cnt = 0;
            for (SendQueue::Chunk* c = head; c && cnt < MAX_IOV_BATCH; c = c->next) {
                if (c->avail() == 0)
                    continue;
                iov[cnt].iov_base = const_cast<char*>(c->unsent());
                iov[cnt].iov_len = c->avail();
                cnt++;
            }

            if (cnt == 0) {
                queue->pop_front();     // empty chunk
                continue;
            }

            for (;;) {
                n = platform::sendIov(m_SocketContext.fd, iov, cnt);
                if (n >= 0)
                    break;
                int e = platform::lastError();
                if (platform::isInterrupted(e))
                    continue;
                if (platform::isWouldBlock(e) || e == err::NoBuffers) {
                    n = IO_AGAIN;
                } else {
                    m_lastError = e;
                    n = IO_ERROR;
                }
                break;
            }

            if (n == 0)
                n = IO_AGAIN;
        }

        if (n > 0) {
            queue->consume((size_t)n);
            updateLastActive();
            continue;
        }

        if (n == IO_AGAIN)
            break;

        finalizeClose();
        return false;
    }

    return true;
}

// Zero-copy send: the application memory must stay valid until release is called
// (release == nullptr: static data). release is also called on close.
bool TCPSocket::sendNoCopy(const void *data, size_t len, SendQueue::ReleaseFn release, void *arg)
{
    if (!data || len == 0) {
        if (release) release(const_cast<void*>(data), arg);
        return true;
    }

    if (status != Connected || m_abortPending || m_shutdownWrPending || m_SocketContext.fd == NR_INVALID_SOCKET || !queueHasRoom(len)) {
        if (release) release(const_cast<void*>(data), arg);
        return false;
    }

    SendQueue* queue = &m_SocketContext.writeQueue;
    size_t sent = 0;

    if (queue->empty()) {
        while (sent < len) {
            ssize_t n = ioWrite(static_cast<const char*>(data) + sent, len - sent);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (n == IO_AGAIN)
                break;

            if (release) release(const_cast<void*>(data), arg);
            abortLater();
            return false;
        }

        updateLastActive();
        if (sent == len) {
            if (release) release(const_cast<void*>(data), arg);
            updateInterest();
            return true;
        }
    }

    if (!queue->pushExternal(data, len, sent, release, arg)) {
        if (release) release(const_cast<void*>(data), arg);
        m_lastError = err::NoMemory;
        abortLater();
        return false;
    }

    updateInterest();

    if (!m_writeBlocked && queue->size() > BACK_PRESSURE) {
        m_writeBlocked = true;
        handleOnPause();
    }

    return true;
}

void TCPSocket::onWritable() {

    if (status == Connecting) {
        onConnectReady();
        return;
    }

    if (status == Handshaking) {
        tlsHandshake();
        if (status != Connected)
            return;
    }

    if (status != Connected && !(status == Closing && !m_lingering))
        return;

    // SSL_read was waiting for the socket to become writable
    if (m_tlsReadWantsWrite) {
        m_tlsReadWantsWrite = false;
        if (status == Connected) {
            onReadable();
            if (status == Closed)
                return;
        }
    }

    if (!flushQueue())
        return;

    SendQueue* queue = &m_SocketContext.writeQueue;

    if (queue->empty()) {
        if (m_pendingClose) {
            drainComplete();
            return;
        }

        if (m_shutdownWrPending && !m_writeShutdown) {
            doShutdownWrite();
            if (status == Closed)
                return;
        }
    }

    // resume
    if (m_writeBlocked && queue->size() <= LOW_WATERMARK) {
        m_writeBlocked = false;
        if (status == Connected)
            handleOnResume();
    }

    updateInterest();
}

// ----------------------------------------------------------------------------
// accessors
// ----------------------------------------------------------------------------
int TCPSocket::fd() const
{
    return m_SocketContext.fd == NR_INVALID_SOCKET ? -1 : (int)m_SocketContext.fd;
}

TCPSocket *TCPSocket::getPointer()
{
    return this;
}

uint64_t TCPSocket::getLastActiveTime()
{
    return m_SocketContext.lastActive;
}

uint16_t TCPSocket::getLocalPort()
{
    if (m_SocketContext.fd == NR_INVALID_SOCKET)
        return 0;

    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(m_SocketContext.fd, (struct sockaddr*)&local, &len) != 0)
        return 0;

    if (local.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);

    return ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
}

bool TCPSocket::getPeerAddress(std::string &ip, uint16_t &port) const
{
    if (m_SocketContext.fd == NR_INVALID_SOCKET)
        return false;

    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(m_SocketContext.fd, (struct sockaddr*)&peer, &len) != 0)
        return false;

    char buf[INET6_ADDRSTRLEN] = {0};
    if (peer.ss_family == AF_INET6) {
        const sockaddr_in6* a6 = reinterpret_cast<const sockaddr_in6*>(&peer);
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            // dual-stack listener: IPv4 shows up as ::ffff:a.b.c.d
            inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], buf, sizeof(buf));
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
        }
        port = ntohs(a6->sin6_port);
    } else {
        const sockaddr_in* a4 = reinterpret_cast<const sockaddr_in*>(&peer);
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
        port = ntohs(a4->sin_port);
    }

    ip = buf;
    return true;
}

size_t TCPSocket::pendingBytes() const
{
    return m_SocketContext.writeQueue.size();
}

bool TCPSocket::isWriteBlocked() const
{
    return m_writeBlocked;
}

bool TCPSocket::isAccepted() const
{
    return m_isAccepted;
}

void TCPSocket::setOnData(OnDataFn fn, void* Arg) {
    m_onData = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnConnecting(OnConnectingFn fn, void* Arg)
{
    m_onConnecting = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnConnected(OnConnectedFn fn, void* Arg)
{
    m_onConnected= fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnAccepted(OnAcceptedFn fn, void* Arg)
{
    m_onAccepted = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnClose(OnCloseFn fn, void *Arg)
{
    m_onClose= fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnConnectFailed(OnConnectFailedFn fn, void *Arg)
{
    m_onConnectFailed= fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnPause(OnPauseFn fn, void* Arg) {
    m_onPause = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnResume(OnResumeFn fn, void* Arg) {
    m_onResume = fn;
    m_callbacksArg = Arg;
}

void TCPSocket::setOnEOF(OnEOFFn fn, void *Arg)
{
    m_onEOF = fn;
    m_callbacksArg = Arg;
}

} // namespace netrector
