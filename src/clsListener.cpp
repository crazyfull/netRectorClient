#include "clsListener.h"
#include "clsLog.h"
#include "clsTCPSocket.h"
#include <cstring>

namespace netrector {

Listener::Listener(EventLoop* loop) : m_pReactor(loop) {}

Listener::~Listener()
{
    stop();
}

bool Listener::start(int port, const char* bindIP, EventLoop::acceptCallback cb, void* ctx)
{
    if (!m_pReactor || m_fd != NR_INVALID_SOCKET || port < 0 || port > 65535)
        return false;

    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t addrLen = 0;
    int family = AF_INET;
    bool dualStack = false;

    sockaddr_in* a4 = reinterpret_cast<sockaddr_in*>(&addr);
    sockaddr_in6* a6 = reinterpret_cast<sockaddr_in6*>(&addr);

    if (!bindIP || !*bindIP || strcmp(bindIP, "*") == 0) {
        family = AF_INET6;
        dualStack = true;
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons((uint16_t)port);
        addrLen = sizeof(sockaddr_in6);
    } else if (inet_pton(AF_INET, bindIP, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons((uint16_t)port);
        addrLen = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, bindIP, &a6->sin6_addr) == 1) {
        family = AF_INET6;
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((uint16_t)port);
        addrLen = sizeof(sockaddr_in6);
    } else {
        m_lastError = err::InvalidArg;
        return false;
    }

    NrSocket fd = platform::createSocket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == NR_INVALID_SOCKET && dualStack) {
        // no IPv6 on this system: IPv4 only
        family = AF_INET;
        dualStack = false;
        memset(&addr, 0, sizeof(addr));
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_ANY);
        a4->sin_port = htons((uint16_t)port);
        addrLen = sizeof(sockaddr_in);
        fd = platform::createSocket(family, SOCK_STREAM, IPPROTO_TCP);
    }

    if (fd == NR_INVALID_SOCKET) {
        m_lastError = platform::lastError();
        return false;
    }

    TCPSocket::setSocketResourceAddress(fd, true);

    if (dualStack) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), addrLen) != 0 || ::listen(fd, LISTEN_BACKLOG) != 0) {
        m_lastError = platform::lastError();
        NR_LOG(LOG_ERROR, "Listener: cannot listen on port %d (%s)", port, errorString(m_lastError).c_str());
        platform::closeSocket(fd);
        return false;
    }

    uint64_t token = m_pReactor->addObject(this, fd, EV_READ, EventLoop::KindListener);
    if (token == 0) {
        m_lastError = err::NoMemory;
        platform::closeSocket(fd);
        return false;
    }

    if (port == 0) {
        // random port: report the real one
        sockaddr_storage bound;
        socklen_t blen = sizeof(bound);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
            port = ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                                     : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    }

    m_fd = fd;
    m_token = token;
    m_port = port;
    m_factory = cb;
    m_ctx = ctx;
    m_paused = false;
    return true;
}

void Listener::stop()
{
    if (m_token && m_pReactor)
        m_pReactor->removeObject(m_token);
    m_token = 0;

    if (m_fd != NR_INVALID_SOCKET) {
        platform::closeSocket(m_fd);
        m_fd = NR_INVALID_SOCKET;
    }
    m_paused = false;
}

void Listener::onIoEvents(uint32_t ev)
{
    if (m_fd == NR_INVALID_SOCKET || m_paused)
        return;

    if (ev & (EV_READ | EV_ERROR | EV_HUP))
        acceptLoop();
}

// out of descriptors / memory: stop accepting for a moment instead of spinning on a ready listener
void Listener::onMaintenance(uint64_t nowSeconds)
{
    if (m_paused && nowSeconds >= m_pausedAt + (uint64_t)ACCEPT_RETRY_SECS) {
        m_paused = false;
        if (m_token)
            m_pReactor->setInterest(m_token, EV_READ);
    }
}

void Listener::acceptLoop()
{
    EventLoop::acceptCallback factory = m_factory ? m_factory : m_pReactor->onAcceptedFactory();
    void* ctx = m_factory ? m_ctx : m_pReactor->onAcceptedContext();

    for (int i = 0; i < MAX_ACCEPT_PER_EVENT; i++) {
        NrSocket fd = ::accept(m_fd, nullptr, nullptr);
        if (fd == NR_INVALID_SOCKET) {
            int e = platform::lastError();
            if (platform::isInterrupted(e))
                continue;
            if (platform::isWouldBlock(e))
                return;

#if !defined(_WIN32)
            if (e == ECONNABORTED || e == EPROTO)
                continue;       // the client left before accept: next one
#else
            if (e == WSAECONNRESET)
                continue;
#endif

            m_lastError = e;
            NR_LOG(LOG_WARN, "Listener: accept failed (%s), pausing", errorString(e).c_str());
            m_paused = true;
            m_pausedAt = m_pReactor->getCachedNow();
            m_pReactor->setInterest(m_token, 0);
            return;
        }

        if (!platform::prepareSocket(fd)) {
            platform::closeSocket(fd);
            continue;
        }

        TCPSocket* sock = factory ? factory(ctx) : nullptr;
        if (!sock) {
            platform::closeSocket(fd);      // refused
            continue;
        }

        sock->setReactor(m_pReactor);
        sock->_accepted(fd);
    }
}

} // namespace netrector
