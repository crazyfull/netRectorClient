#ifndef TUNNELPOOL_H
#define TUNNELPOOL_H

#include "ClientConfig.h"
#include "TunnelSlot.h"
#include "WebSocketClientHandshake.h"

#include "clsEventLoop.h"
#include "clsMultiplexedTunnel.h"
#include "clsTLSContext.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ClientTunnel;

// A local SOCKS5 connection waiting for the UDP tunnel to come up (see TunnelPool::requestUdpTunnel).
class UdpTunnelWaiter {
public:
    virtual ~UdpTunnelWaiter() = default;
    virtual void onUdpTunnelReady() = 0;        // a UDP stream can be opened now
    virtual void onUdpTunnelFailed() = 0;       // connecting failed
};

// The mux tunnels of the application (all on one EventLoop thread).
//
// The pool keeps each tunnel connected (reconnect with exponential back-off, driven by tick())
// and hands out streams round-robin.
class TunnelPool {
public:
    TunnelPool(netrector::EventLoop& loop, const ClientConfig& config,
               const WebSocketClientHandshake& handshake, size_t tunnelCount,
               netrector::TLSContext* tlsContext = nullptr);

    // Reactor thread: starts connecting all tunnels.
    void start();

    // Loop thread: no more reconnects (used when shutting down).
    void stop();

    // Loop thread: call regularly from the main loop. Starts the reconnects whose delay expired
    // (netRectorClient has no timers, so the pool keeps its own deadlines).
    void tick(uint64_t nowMs);

    // Loop thread: number of tunnels that finished the handshake and are connected right now.
    size_t connectedCount() const;

    // Loop thread: loop-clock time of the earliest pending reconnect, 0 when none is scheduled.
    uint64_t nextDeadlineMs() const;

    // Reactor thread: opens a stream on the next ready tunnel (round-robin). Returns the stream id
    // and the tunnel it belongs to, or 0 when no tunnel can take a new stream.
    uint32_t openStream(netrector::MultiplexedTunnel::OnStreamDataFn dataFunction,
                        netrector::MultiplexedTunnel::OnStreamCloseFn closeFunction,
                        void* argument, netrector::MultiplexedTunnel*& tunnel);

    // ---- UDP-over-TCP: one separate tunnel, connected on demand, closed when idle ----

    // Reactor thread: asks for the UDP tunnel. Returns false when UDP is disabled / the pool is
    // stopped / a connect attempt was made too recently. Otherwise `waiter` gets exactly one
    // onUdpTunnelReady() or onUdpTunnelFailed() (the callback may come before this returns).
    // Call cancelUdpWait() before the waiter is destroyed.
    bool requestUdpTunnel(UdpTunnelWaiter* waiter);
    void cancelUdpWait(UdpTunnelWaiter* waiter);

    // Reactor thread: opens a stream on the UDP tunnel (0 when it is not ready / full).
    uint32_t openUdpStream(netrector::MultiplexedTunnel::OnStreamDataFn dataFunction,
                           netrector::MultiplexedTunnel::OnStreamCloseFn closeFunction,
                           void* argument, netrector::MultiplexedTunnel*& tunnel);

    // Reactor thread: a datagram frame moved through a UDP stream (resets the idle timer).
    void noteUdpActivity();

    // Loop thread: the screen is off / on. Off uses the shorter idle time (UdpIdleScreenOffSeconds).
    void setScreenOff(bool screenOff);

    // Loop thread: true while the UDP tunnel is connected (or connecting).
    bool udpTunnelUp() const { return m_udpTunnel != nullptr; }

    // Notifications from ClientTunnel (reactor thread).
    void onTunnelReady(ClientTunnel& tunnel);
    void onTunnelClosed(ClientTunnel& tunnel);
    void onTunnelGoAway(ClientTunnel& tunnel);

private:
    void connectUdpTunnel();
    void closeUdpTunnel(const char* reason);
    void failUdpWaiters();
    void checkUdpIdle(uint64_t nowMs);

    void connectSlot(size_t index);
    void scheduleReconnect(size_t index);
    TunnelSlot* findSlot(ClientTunnel& tunnel, size_t& index);

    netrector::EventLoop& m_loop;
    const ClientConfig& m_config;
    const WebSocketClientHandshake& m_handshake;
    netrector::TLSContext* m_tlsContext;    // nullptr = plain TCP
    std::string m_serverName;               // SNI
    std::vector<TunnelSlot> m_slots;
    size_t m_nextSlot = 0;                  // round-robin cursor
    bool m_stopped = false;

    // UDP tunnel (separate from the slots above)
    ClientTunnel* m_udpTunnel = nullptr;    // owned by the event loop, cleared in onTunnelClosed()
    bool m_udpReady = false;
    std::vector<UdpTunnelWaiter*> m_udpWaiters;
    uint64_t m_udpLastActivityMs = 0;
    uint64_t m_udpNextAttemptMs = 0;
    bool m_screenOff = false;
    uint64_t m_nowMs = 0;
};

#endif // TUNNELPOOL_H
