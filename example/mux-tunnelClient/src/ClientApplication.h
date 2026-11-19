#ifndef CLIENTAPPLICATION_H
#define CLIENTAPPLICATION_H

#include "ClientConfig.h"
#include "WebSocketClientHandshake.h"

#include "clsEventLoop.h"
#include "clsTCPSocket.h"

#include <atomic>
#include <memory>
#include <mutex>

// Owns the netRectorClient event loop, the local SOCKS5 listener and the tunnel pools of mux-tunnelClient.
class ClientApplication {
public:
    explicit ClientApplication(const ClientConfig& config);

    // Runs until requestStop() is called or, when handleSignals is true, until SIGINT / SIGTERM.
    // Returns the process exit code (0 = clean stop, 1 = cannot listen). Blocks the calling thread.
    int run(bool handleSignals = true);

    // Thread-safe: makes run() return. May be called before run() too (run() then returns at once).
    void requestStop();

    // Thread-safe: how many tunnels are connected right now (handshake done). Updated by the loop
    // after every pass, so it follows connects and drops within a moment.
    int connectedTunnelCount() const { return m_connectedTunnels; }

    // Thread-safe: tells the client that the screen is off / on. With UDP enabled the UDP tunnel is
    // closed after the shorter UdpIdleScreenOffSeconds without datagrams while the screen is off.
    void setScreenOff(bool screenOff);

    // Thread-safe: 1 while the UDP tunnel is connected (or connecting), else 0. Always 0 when UDP is off.
    int udpTunnelUp() const { return m_udpTunnelUp; }

private:
    // accept factory of the local SOCKS5 listener
    static netrector::TCPSocket* createLocalConnection(void* context);

    ClientConfig m_config;
    WebSocketClientHandshake m_handshake;
    std::atomic<bool> m_stopRequested { false };
    std::atomic<int> m_connectedTunnels { 0 };
    std::atomic<int> m_udpTunnelUp { 0 };
    std::atomic<bool> m_screenOff { false };
    std::mutex m_loopMutex;                     // guards m_loop
    netrector::EventLoop* m_loop = nullptr;     // set while run() is active, used to wake it
};

#endif // CLIENTAPPLICATION_H
