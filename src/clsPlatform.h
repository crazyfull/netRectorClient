#ifndef CLSPLATFORM_H
#define CLSPLATFORM_H
// ============================== Platform =====================================
// Thin portability layer: Linux, Android, iOS/macOS (BSD sockets) and Windows (Winsock2).
// Everything platform specific in netRectorClient is in this header and clsPlatform.cpp,
// the rest of the library only talks to these helpers.
#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
    #define NR_SHUT_WR SD_SEND
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #if defined(_MSC_VER)
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
    #endif
    typedef SOCKET NrSocket;
    #define NR_INVALID_SOCKET INVALID_SOCKET

    // POSIX struct iovec, so TCPSocket::sendv() has the same signature everywhere
    #ifndef NR_HAVE_IOVEC
    #define NR_HAVE_IOVEC
    struct iovec {
        void* iov_base;
        size_t iov_len;
    };
    #endif
#else
    #define NR_SHUT_WR SHUT_WR
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/uio.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <cerrno>
    typedef int NrSocket;
    #define NR_INVALID_SOCKET (-1)
#endif

namespace netrector {

// Error codes returned by getErrorCode(). On POSIX they are errno values, on Windows they are
// Winsock error codes. Use errorString() to print them on every platform.
namespace err {
#if defined(_WIN32)
constexpr int WouldBlock  = WSAEWOULDBLOCK;
constexpr int InProgress  = WSAEWOULDBLOCK;     // a non-blocking connect() reports "would block"
constexpr int Interrupted = WSAEINTR;
constexpr int ConnRefused = WSAECONNREFUSED;
constexpr int ConnReset   = WSAECONNRESET;
constexpr int HostUnreach = WSAEHOSTUNREACH;
constexpr int NetUnreach  = WSAENETUNREACH;
constexpr int TimedOut    = WSAETIMEDOUT;
constexpr int NoBuffers   = WSAENOBUFS;
constexpr int NoMemory    = WSA_NOT_ENOUGH_MEMORY;
constexpr int InvalidArg  = WSAEINVAL;
constexpr int Protocol    = 71;                 // TLS / protocol failure (same value as Linux EPROTO)
constexpr int IoError     = 5;                  // EIO
#else
constexpr int WouldBlock  = EWOULDBLOCK;
constexpr int InProgress  = EINPROGRESS;
constexpr int Interrupted = EINTR;
constexpr int ConnRefused = ECONNREFUSED;
constexpr int ConnReset   = ECONNRESET;
constexpr int HostUnreach = EHOSTUNREACH;
constexpr int NetUnreach  = ENETUNREACH;
constexpr int TimedOut    = ETIMEDOUT;
constexpr int NoBuffers   = ENOBUFS;
constexpr int NoMemory    = ENOMEM;
constexpr int InvalidArg  = EINVAL;
constexpr int Protocol    = EPROTO;
constexpr int IoError     = EIO;
#endif
} // namespace err

// human readable text of an error code (errno / Winsock)
std::string errorString(int code);

namespace platform {

// Winsock start-up (reference counted, no-op elsewhere). Called by EventLoop; safe to call again.
bool init();
void shutdown();

int lastError();                    // errno / WSAGetLastError()
bool isWouldBlock(int e);
bool isInterrupted(int e);

uint64_t monotonicMs();             // steady clock, milliseconds

// socket()/accept() result prepared for the loop: non-blocking, close-on-exec, no SIGPIPE
NrSocket createSocket(int family, int type, int protocol);
bool prepareSocket(NrSocket fd);
void closeSocket(NrSocket fd);
bool setNonBlocking(NrSocket fd);
int socketError(NrSocket fd);       // SO_ERROR (0 = none)

ssize_t recvData(NrSocket fd, void* buf, size_t len);
ssize_t sendData(NrSocket fd, const void* buf, size_t len);                 // never raises SIGPIPE
ssize_t sendIov(NrSocket fd, const struct iovec* iov, int iovcnt);          // gather write

// Windows only (UDP "connection reset" quirk); no-op elsewhere
void disableUdpConnReset(NrSocket fd);

} // namespace platform
} // namespace netrector

#endif // CLSPLATFORM_H
