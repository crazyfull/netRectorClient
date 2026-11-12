#include "clsUDPSocket.h"
#include "clsEventLoop.h"
#include "clsTCPSocket.h"
#include <cstring>
#include <vector>

namespace netrector {

UDPSocket::UDPSocket() {}

UDPSocket::~UDPSocket() {
    if (m_registered && m_pReactor) {
        m_pReactor->removeObject(m_SocketContext.token);
        m_registered = false;
    }

    if (m_SocketContext.fd != NR_INVALID_SOCKET) {
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
    }
}

void UDPSocket::setReactor(EventLoop* r) {
    m_pReactor = r;
}

void UDPSocket::setMaxDatagramSize(size_t bytes)
{
    if (bytes < 64) bytes = 64;
    if (bytes > 65535) bytes = 65535;
    m_maxDatagram = bytes;
}

bool UDPSocket::create(int family)
{
    if (m_SocketContext.fd != NR_INVALID_SOCKET || m_closed)
        return false;

    NrSocket fd = platform::createSocket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == NR_INVALID_SOCKET) {
        m_lastError = platform::lastError();
        return false;
    }

    platform::disableUdpConnReset(fd);
    m_SocketContext.fd = fd;
    return true;
}

bool UDPSocket::registerWithReactor()
{
    if (!m_pReactor || m_registered)
        return m_registered;

    uint64_t token = m_pReactor->addObject(this, m_SocketContext.fd, EV_READ, EventLoop::KindUdp);
    if (token == 0) {
        m_lastError = err::NoMemory;
        return false;
    }

    m_SocketContext.token = token;
    m_registered = true;
    return true;
}

bool UDPSocket::open(int family)
{
    if (!m_pReactor)
        return false;

    if (!create(family))
        return false;

    if (!registerWithReactor()) {
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
        return false;
    }
    return true;
}

static bool parseAddress(const char* ip, uint16_t port, sockaddr_storage& addr, socklen_t& len, int& family, bool anyIfEmpty)
{
    memset(&addr, 0, sizeof(addr));
    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&addr);
    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&addr);

    if (anyIfEmpty && (!ip || !*ip)) {
        family = AF_INET;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_ANY);
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }

    if (!ip || !*ip)
        return false;

    if (inet_pton(AF_INET, ip, &a4->sin_addr) == 1) {
        family = AF_INET;
        a4->sin_family = AF_INET;
        a4->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }

    if (inet_pton(AF_INET6, ip, &a6->sin6_addr) == 1) {
        family = AF_INET6;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        return true;
    }

    return false;
}

bool UDPSocket::bind(const char* ip, uint16_t port, bool reusePort)
{
    if (!m_pReactor)
        return false;

    sockaddr_storage addr;
    socklen_t len = 0;
    int family = AF_INET;
    if (!parseAddress(ip, port, addr, len, family, true)) {
        m_lastError = err::InvalidArg;
        return false;
    }

    if (!create(family))
        return false;

    TCPSocket::setSocketResourceAddress(m_SocketContext.fd, true);
    if (reusePort)
        TCPSocket::setSocketShared(m_SocketContext.fd, true);

    int rcvbuf = 1024 * 1024; // 1MB
    setsockopt(m_SocketContext.fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    if (::bind(m_SocketContext.fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        m_lastError = platform::lastError();
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
        return false;
    }

    if (!registerWithReactor()) {
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
        return false;
    }

    m_SocketContext.port = port;
    return true;
}

bool UDPSocket::connect(const char* ip, uint16_t port)
{
    sockaddr_storage addr;
    socklen_t len = 0;
    int family = AF_INET;
    if (!parseAddress(ip, port, addr, len, family, false)) {
        m_lastError = err::InvalidArg;
        return false;
    }

    if (m_SocketContext.fd == NR_INVALID_SOCKET && !open(family))
        return false;

    if (::connect(m_SocketContext.fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        m_lastError = platform::lastError();
        return false;
    }
    return true;
}

bool UDPSocket::sendTo(const void* data, size_t len, const struct sockaddr* to, socklen_t toLen)
{
    if (!data || m_SocketContext.fd == NR_INVALID_SOCKET || m_closed)
        return false;

    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif

    ssize_t n;
    for (;;) {
#if defined(_WIN32)
        n = ::sendto(m_SocketContext.fd, static_cast<const char*>(data), (int)len, flags, to, toLen);
#else
        n = ::sendto(m_SocketContext.fd, data, len, flags, to, toLen);
#endif
        if (n >= 0)
            return true;

        int e = platform::lastError();
        if (platform::isInterrupted(e))
            continue;

        // UDP: when the buffer is full the datagram is dropped, like in the network
        m_lastError = e;
        return false;
    }
}

bool UDPSocket::send(const void* data, size_t len)
{
    return sendTo(data, len, nullptr, 0);
}

uint16_t UDPSocket::getLocalPort() const
{
    if (m_SocketContext.fd == NR_INVALID_SOCKET)
        return 0;

    sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(m_SocketContext.fd, reinterpret_cast<sockaddr*>(&local), &len) != 0)
        return 0;

    if (local.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
    return ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
}

void UDPSocket::onIoEvents(uint32_t ev)
{
    if (m_closed || m_SocketContext.fd == NR_INVALID_SOCKET)
        return;

    if (ev & EV_ERROR) {
        // ICMP "unreachable" on a connected socket: the socket stays open, the error is recorded
        int e = platform::socketError(m_SocketContext.fd);
        if (e != 0)
            m_lastError = e;
    }

    if (ev & (EV_READ | EV_ERROR | EV_HUP))
        readAll();
}

// recvfrom loop; every datagram goes through the loop's shared read buffer
void UDPSocket::readAll()
{
    char* buf = m_pReactor->readBuffer();
    size_t cap = m_pReactor->readBufferSize();
    size_t size = m_maxDatagram < cap ? m_maxDatagram : cap;

    for (int i = 0; i < MAX_UDP_PER_EVENT; i++) {
        sockaddr_storage from;
        socklen_t fromLen = sizeof(from);

        ssize_t n;
#if defined(_WIN32)
        n = ::recvfrom(m_SocketContext.fd, buf, (int)size, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
        n = ::recvfrom(m_SocketContext.fd, buf, size, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif
        if (n < 0) {
            int e = platform::lastError();
            if (platform::isInterrupted(e))
                continue;

            if (!platform::isWouldBlock(e))
                m_lastError = e;    // e.g. ECONNREFUSED on a connected socket
#if defined(_WIN32)
            // WSAEMSGSIZE: datagram larger than the buffer, it is truncated (the rest is dropped)
            if (e == WSAEMSGSIZE) {
                m_lastError = e;
                continue;
            }
#endif
            return;
        }

        m_SocketContext.lastActive = m_pReactor->getCachedNow();

        const uint8_t* data = reinterpret_cast<const uint8_t*>(buf);
        const sockaddr* src = reinterpret_cast<const sockaddr*>(&from);
        if (m_onDatagram)
            m_onDatagram(m_callbackArg, data, (size_t)n, src, fromLen);
        else
            onDatagram(data, (size_t)n, src, fromLen);

        if (m_closed)
            return;
    }

    // The socket is level-triggered: if datagrams are left the loop reports it again in the next
    // iteration, so the other sockets get their turn in between.
}

void UDPSocket::onDeferred(int /*kind*/)
{
    m_deferredRead = false;
    if (!m_closed && m_SocketContext.fd != NR_INVALID_SOCKET)
        readAll();
}

void UDPSocket::close()
{
    finalizeClose();
}

void UDPSocket::finalizeClose()
{
    if (m_closed)
        return;
    m_closed = true;

    if (m_registered && m_pReactor) {
        m_pReactor->removeObject(m_SocketContext.token);
        m_registered = false;
        m_SocketContext.token = 0;
    }

    if (m_SocketContext.fd != NR_INVALID_SOCKET) {
        platform::closeSocket(m_SocketContext.fd);
        m_SocketContext.fd = NR_INVALID_SOCKET;
    }

    if (m_onClose)
        m_onClose(m_callbackArg);
    else
        onClose();

    if (m_autoDelete && m_pReactor)
        m_pReactor->deleteLater(this);
}

} // namespace netrector
