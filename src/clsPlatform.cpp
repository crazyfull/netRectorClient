#include "clsPlatform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
    #include <mstcpip.h>
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)       // not in every MinGW header
    #endif
#else
    #include <signal.h>
#endif

#if defined(__APPLE__)
    #include <sys/socket.h>
#endif

namespace netrector {

std::string errorString(int code)
{
#if defined(_WIN32)
    char* msg = nullptr;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, (DWORD)code, 0, (LPSTR)&msg, 0, nullptr);
    std::string out;
    if (n && msg) {
        out.assign(msg, n);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ' || out.back() == '.'))
            out.pop_back();
    } else {
        out = "error " + std::to_string(code);
    }
    if (msg)
        LocalFree(msg);
    return out;
#else
    char buf[160];
    buf[0] = '\0';
    #if defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char* s = strerror_r(code, buf, sizeof(buf));     // GNU variant returns the text
    return std::string(s ? s : "");
    #else
    if (strerror_r(code, buf, sizeof(buf)) != 0)            // XSI variant (Android, Apple, musl)
        snprintf(buf, sizeof(buf), "error %d", code);
    return std::string(buf);
    #endif
#endif
}

namespace platform {

#if defined(_WIN32)
static std::mutex g_initMutex;
static int g_initCount = 0;
#endif

bool init()
{
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_initCount == 0) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;
    }
    g_initCount++;
    return true;
#else
    // a write to a closed connection must return an error instead of killing the process
    static std::once_flag once;
    std::call_once(once, []() { ::signal(SIGPIPE, SIG_IGN); });
    return true;
#endif
}

void shutdown()
{
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_initCount > 0 && --g_initCount == 0)
        WSACleanup();
#endif
}

int lastError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool isWouldBlock(int e)
{
#if defined(_WIN32)
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

bool isInterrupted(int e)
{
#if defined(_WIN32)
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

uint64_t monotonicMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool setNonBlocking(NrSocket fd)
{
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool prepareSocket(NrSocket fd)
{
    if (!setNonBlocking(fd))
        return false;

#if !defined(_WIN32)
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
    #if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    #endif
#endif
    return true;
}

NrSocket createSocket(int family, int type, int protocol)
{
    NrSocket fd = ::socket(family, type, protocol);
    if (fd == NR_INVALID_SOCKET)
        return NR_INVALID_SOCKET;

    if (!prepareSocket(fd)) {
        int e = lastError();
        closeSocket(fd);
        (void)e;
        return NR_INVALID_SOCKET;
    }
    return fd;
}

void closeSocket(NrSocket fd)
{
    if (fd == NR_INVALID_SOCKET)
        return;
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

int socketError(NrSocket fd)
{
    if (fd == NR_INVALID_SOCKET)
        return 0;

    int e = 0;
    socklen_t len = sizeof(e);
#if defined(_WIN32)
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &len) != 0)
        e = lastError();
#else
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0)
        e = errno;
#endif
    return e;
}

ssize_t recvData(NrSocket fd, void* buf, size_t len)
{
#if defined(_WIN32)
    int n = ::recv(fd, static_cast<char*>(buf), (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
    return n;
#else
    return ::recv(fd, buf, len, 0);
#endif
}

ssize_t sendData(NrSocket fd, const void* buf, size_t len)
{
#if defined(_WIN32)
    int n = ::send(fd, static_cast<const char*>(buf), (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
    return n;
#else
    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    return ::send(fd, buf, len, flags);
#endif
}

ssize_t sendIov(NrSocket fd, const struct iovec* iov, int iovcnt)
{
#if defined(_WIN32)
    WSABUF bufs[64];
    int cnt = iovcnt > 64 ? 64 : iovcnt;
    for (int i = 0; i < cnt; i++) {
        bufs[i].buf = static_cast<CHAR*>(iov[i].iov_base);
        bufs[i].len = (ULONG)iov[i].iov_len;
    }
    DWORD sent = 0;
    if (WSASend(fd, bufs, (DWORD)cnt, &sent, 0, nullptr, nullptr) != 0)
        return -1;
    return (ssize_t)sent;
#else
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = const_cast<struct iovec*>(iov);
    msg.msg_iovlen = (size_t)(iovcnt > 64 ? 64 : iovcnt);

    int flags = 0;
    #if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
    #endif
    return ::sendmsg(fd, &msg, flags);
#endif
}

void disableUdpConnReset(NrSocket fd)
{
#if defined(_WIN32)
    // without this, an ICMP port-unreachable makes the next recvfrom() fail with WSAECONNRESET
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
#else
    (void)fd;
#endif
}

} // namespace platform
} // namespace netrector
