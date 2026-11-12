#ifndef CLSUDPSOCKET_H
#define CLSUDPSOCKET_H
#include "SocketContext.h"
#include "clsPlatform.h"
#include "clsReactorObject.h"

namespace netrector {

class EventLoop;

// ============================== UDPSocket ====================================
// Non-blocking UDP socket on an EventLoop. Every datagram is delivered with a callback or the
// virtual function. Same API as netRector's UDPSocket (the batched recvmmsg of Linux is replaced
// by a plain recvfrom loop, which is the portable form).
//
//   UDPSocket* u = new MyUDP;  u->setReactor(&loop);  u->bind("0.0.0.0", 5353);
//   ... onDatagram(data, len, from, fromLen) ...
//   u->sendTo(buf, n, from, fromLen);  u->close();   // the loop deletes it after onClose
//
// Same rules as TCPSocket: create with new, call only from the loop thread, setAutoDelete.
class UDPSocket : public ReactorObject, public IoObject {
public:
    using OnDatagramFn = void(*)(void* arg, const uint8_t* data, size_t len, const struct sockaddr* from, socklen_t fromLen);
    using OnCloseFn = void(*)(void* arg);

    UDPSocket();
    virtual ~UDPSocket();

    void setReactor(EventLoop* r);
    EventLoop* getReactor() const { return m_pReactor; }

    // creates and binds the socket (ip: IPv4 or IPv6; nullptr/"" = all IPv4). port 0 = random.
    // reusePort: several sockets can bind the same port (where the OS supports SO_REUSEPORT)
    bool bind(const char* ip, uint16_t port, bool reusePort = false);

    // socket without bind (client). family: AF_INET / AF_INET6
    bool open(int family = AF_INET);

    // default destination of send() (UDP connect only sets the default address and a filter)
    bool connect(const char* ip, uint16_t port);

    // immediate send. false: kernel buffer full (the datagram is dropped) or error.
    bool sendTo(const void* data, size_t len, const struct sockaddr* to, socklen_t toLen);
    bool send(const void* data, size_t len);   // only after connect()

    void close();

    // maximum size of one datagram (default 2048; up to 65535). Larger datagrams are truncated.
    void setMaxDatagramSize(size_t bytes);

    void setOnDatagram(OnDatagramFn fn, void* arg) { m_onDatagram = fn; m_callbackArg = arg; }
    void setOnClose(OnCloseFn fn, void* arg) { m_onClose = fn; m_callbackArg = arg; }
    void setAutoDelete(bool enable) { m_autoDelete = enable; }

    virtual void onDatagram(const uint8_t* /*data*/, size_t /*len*/, const struct sockaddr* /*from*/, socklen_t /*fromLen*/) {}
    virtual void onClose() {}

    int fd() const { return m_SocketContext.fd == NR_INVALID_SOCKET ? -1 : (int)m_SocketContext.fd; }
    NrSocket handle() const { return m_SocketContext.fd; }
    bool isOpen() const { return m_SocketContext.fd != NR_INVALID_SOCKET; }
    uint16_t getLocalPort() const;
    int getErrorCode() const { return m_lastError; }
    SocketContext* getSocketContext() { return &m_SocketContext; }

    // internal (IoObject)
    void onIoEvents(uint32_t ev) override;
    void onDeferred(int kind) override;
    void onLoopShutdown() override { close(); }

private:
    EventLoop* m_pReactor { nullptr };
    SocketContext m_SocketContext {};
    OnDatagramFn m_onDatagram { nullptr };
    OnCloseFn m_onClose { nullptr };
    void* m_callbackArg { nullptr };
    size_t m_maxDatagram { 2048 };
    int m_lastError { 0 };
    bool m_registered { false };
    bool m_autoDelete { true };
    bool m_closed { false };
    bool m_deferredRead { false };

    bool create(int family);
    bool registerWithReactor();
    void readAll();
    void finalizeClose();
};

} // namespace netrector

#endif // CLSUDPSOCKET_H
