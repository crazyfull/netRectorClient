#ifndef LOOPBACKLISTENER_H
#define LOOPBACKLISTENER_H

#include "clsListener.h"

// ============================== LoopbackListener ==============================
// A TCP listener that is reachable only from this device, on IPv4 and IPv6 at the same time:
// 127.0.0.1 and ::1 with the same port. Two sockets, because one socket cannot be both
// loopback-only and dual-stack.
//
// IPv6 is optional: where the system has no IPv6 loopback (or the port is taken there) the
// listener runs on IPv4 only, and the other way round. start() fails only when both fail.
//
//   LoopbackListener l(&loop);
//   l.start(0, myAcceptFactory, ctx);       // 0 = a free port, the same one for both families
//   l.port();                               // the real port
//
// Why not one socket on "::" with IPV6_V6ONLY off: that listens on every interface, not only on
// loopback, and an IPv4-mapped connect from an AF_INET6 socket is not reliable on every system
// (it fails inside an iOS Network Extension).
namespace netrector {

class LoopbackListener
{
public:
    explicit LoopbackListener(EventLoop* loop = nullptr);
    ~LoopbackListener();

    void setReactor(EventLoop* loop);

    bool start(int port, EventLoop::acceptCallback cb, void* ctx);
    void stop();

    int port() const { return m_port; }
    bool ipv4Active() const { return m_v4.isListening(); }
    bool ipv6Active() const { return m_v6.isListening(); }
    int lastError() const { return m_lastError; }

    static const char* ipv4Address() { return "127.0.0.1"; }
    static const char* ipv6Address() { return "::1"; }

private:
    Listener m_v4;
    Listener m_v6;
    int m_port = 0;
    int m_lastError = 0;
};

} // namespace netrector

#endif // LOOPBACKLISTENER_H
