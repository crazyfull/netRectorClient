#ifndef MUXTUNNELCORE_H
#define MUXTUNNELCORE_H

// C interface of the mux-tunnelClient core (netRectorClient), used from Swift.
//
//   muxTunnelCoreRun()              blocks the calling thread until muxTunnelCoreStop() is called
//   muxTunnelCoreStop()             thread-safe
//   muxTunnelCoreConnectedTunnels() thread-safe, number of tunnels that are connected right now
//
// The SOCKS5 listener opened by muxTunnelCoreRun() is what hev-socks5-tunnel connects to.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MuxTunnelCoreConfig {
    const char* listenAddress;          // local SOCKS5 listener, e.g. "127.0.0.1"
    uint16_t listenPort;
    const char* cdnAddress;             // where the tunnels connect: IP or domain
    uint16_t cdnPort;
    int32_t tunnelCount;                // 1...16
    const char* handshakeHost;          // Host header of the websocket-style handshake
    const char* handshakePath;
    const char* webSocketKey;
    const char* expectedAccept;         // empty = do not check
    uint32_t keepAliveSeconds;          // 0 = off
    int32_t verbose;                    // non-zero = debug log
} MuxTunnelCoreConfig;

enum {
    muxTunnelCoreResultStopped = 0,         // stopped by muxTunnelCoreStop()
    muxTunnelCoreResultCannotListen = 1,    // the SOCKS5 port could not be opened
    muxTunnelCoreResultInvalidConfig = 2
};

// Clears a stop request left over from a previous run. Call before starting the thread.
void muxTunnelCoreReset(void);

int32_t muxTunnelCoreRun(const MuxTunnelCoreConfig* config);

void muxTunnelCoreStop(void);

int32_t muxTunnelCoreConnectedTunnels(void);

// Finds the file descriptor of the utun interface of this Network Extension process
// (-1 when it cannot be found). Fallback for packetFlow's "socket.fileDescriptor" key.
int32_t muxTunnelCoreFindUtunDescriptor(void);

#ifdef __cplusplus
}
#endif

#endif // MUXTUNNELCORE_H
