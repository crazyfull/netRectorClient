#ifndef LOCALSOCKSCONNECTION_H
#define LOCALSOCKSCONNECTION_H

#include "ClientConfig.h"
#include "TunnelPool.h"

#include "clsMultiplexedTunnel.h"
#include "clsTCPSocket.h"

#include <cstdint>
#include <string>

// One SOCKS5 client connection accepted on the local listener.
//
//   application <--TCP--> LocalSocksConnection <--mux stream--> tunnel <--> mux-tunnelServer
//
// The SOCKS5 greeting is answered locally (no round trip through the tunnel). The CONNECT
// request, and everything after it, goes through a mux stream unchanged (a UDP request, SOCKS5
// command 0x05 of hev-socks5-tunnel, goes through the separate UDP tunnel, see TunnelPool); the reply to the
// request comes back from mux-tunnelServer.
class LocalSocksConnection : public netrector::TCPSocket, private UdpTunnelWaiter {
public:
    explicit LocalSocksConnection(const ClientConfig& config);

    void onAccepted() override;
    void onReceiveData(const uint8_t* data, size_t length) override;
    void onReadEOF() override;
    void onPause() override;
    void onResume() override;
    void onClose() override;

private:
    enum class State {
        AwaitingGreeting,   // collecting the SOCKS5 method selection
        AwaitingRequest,    // greeting answered, the first request bytes open the stream
        WaitingUdpTunnel,   // UDP request received, the UDP tunnel is being connected
        Relaying,           // stream open, forwarding data
        Failed              // rejected, closing
    };

    void handleGreeting();
    void handleRequestBytes(const uint8_t* data, size_t length);
    void openStream(const uint8_t* data, size_t length, bool udp);
    void rejectRequest(uint8_t code = 0x01);

    // UdpTunnelWaiter
    void onUdpTunnelReady() override;
    void onUdpTunnelFailed() override;

    static void onStreamData(void* argument, uint32_t streamId, const uint8_t* data, size_t length);
    static void onStreamRemoteFin(void* argument, uint32_t streamId);
    static void onStreamClose(void* argument, uint32_t streamId);
    static void onStreamPause(void* argument, uint32_t streamId);
    static void onStreamResume(void* argument, uint32_t streamId);

    const ClientConfig& m_config;
    TunnelPool* m_pool = nullptr;
    netrector::MultiplexedTunnel* m_tunnel = nullptr;
    uint32_t m_streamId = 0;            // 0 while no stream is open
    State m_state = State::AwaitingGreeting;
    std::string m_greetingBuffer;       // only used before the relay starts
    bool m_udp = false;                 // this connection is a UDP-over-TCP flow
    bool m_waitingForUdpTunnel = false; // registered in the pool's waiter list
    bool m_finSentToStream = false;     // the local application finished sending
    bool m_streamFinReceived = false;   // the server side finished sending
};

#endif // LOCALSOCKSCONNECTION_H
