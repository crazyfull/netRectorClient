#ifndef TUNNELPOOL_H
#define TUNNELPOOL_H

#include "ClientConfig.h"
#include "TunnelSlot.h"
#include "WebSocketClientHandshake.h"

#include "clsEventLoop.h"
#include "clsMultiplexedTunnel.h"

#include <cstddef>
#include <cstdint>
#include <vector>

class ClientTunnel;

// The mux tunnels of the application (all on one EventLoop thread).
//
// The pool keeps each tunnel connected (reconnect with exponential back-off, driven by tick())
// and hands out streams round-robin.
class TunnelPool {
public:
    TunnelPool(netrector::EventLoop& loop, const ClientConfig& config,
               const WebSocketClientHandshake& handshake, size_t tunnelCount);

    // Reactor thread: starts connecting all tunnels.
    void start();

    // Loop thread: no more reconnects (used when shutting down).
    void stop();

    // Loop thread: call regularly from the main loop. Starts the reconnects whose delay expired
    // (netRectorClient has no timers, so the pool keeps its own deadlines).
    void tick(uint64_t nowMs);

    // Loop thread: loop-clock time of the earliest pending reconnect, 0 when none is scheduled.
    uint64_t nextDeadlineMs() const;

    // Reactor thread: opens a stream on the next ready tunnel (round-robin). Returns the stream id
    // and the tunnel it belongs to, or 0 when no tunnel can take a new stream.
    uint32_t openStream(netrector::MultiplexedTunnel::OnStreamDataFn dataFunction,
                        netrector::MultiplexedTunnel::OnStreamCloseFn closeFunction,
                        void* argument, netrector::MultiplexedTunnel*& tunnel);

    // Notifications from ClientTunnel (reactor thread).
    void onTunnelReady(ClientTunnel& tunnel);
    void onTunnelClosed(ClientTunnel& tunnel);
    void onTunnelGoAway(ClientTunnel& tunnel);

private:
    void connectSlot(size_t index);
    void scheduleReconnect(size_t index);
    TunnelSlot* findSlot(ClientTunnel& tunnel, size_t& index);

    netrector::EventLoop& m_loop;
    const ClientConfig& m_config;
    const WebSocketClientHandshake& m_handshake;
    std::vector<TunnelSlot> m_slots;
    size_t m_nextSlot = 0;                  // round-robin cursor
    bool m_stopped = false;
    uint64_t m_nowMs = 0;
};

#endif // TUNNELPOOL_H
