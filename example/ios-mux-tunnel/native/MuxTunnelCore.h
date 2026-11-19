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
    int32_t useTls;                     // non-zero = wrap the tunnels in TLS (SNI = handshakeHost)
    int32_t sniBypass;                  // non-zero = TLS record-layer "SNI bypass" (only with useTls)
    int32_t udpEnabled;                 // non-zero = UDP-over-TCP on a separate, on-demand tunnel
    uint32_t udpIdleSeconds;            // the UDP tunnel closes after this long without datagrams (0 = never)
    uint32_t udpIdleScreenOffSeconds;   // same while the device sleeps (muxTunnelCoreSetScreenOff)
} MuxTunnelCoreConfig;

enum {
    muxTunnelCoreResultStopped = 0,         // stopped by muxTunnelCoreStop()
    muxTunnelCoreResultCannotListen = 1,    // the SOCKS5 port could not be opened
    muxTunnelCoreResultInvalidConfig = 2,
    muxTunnelCoreResultTlsUnavailable = 3   // useTls was set but the library has no TLS (no BoringSSL)
};

// Clears a stop request left over from a previous run. Call before starting the thread.
void muxTunnelCoreReset(void);

int32_t muxTunnelCoreRun(const MuxTunnelCoreConfig* config);

void muxTunnelCoreStop(void);

int32_t muxTunnelCoreConnectedTunnels(void);

// 1 while the UDP tunnel is connected (or connecting), else 0. Thread-safe.
int32_t muxTunnelCoreUdpTunnelUp(void);

// The device went to sleep / woke up: the UDP tunnel then closes sooner when idle. Thread-safe.
void muxTunnelCoreSetScreenOff(int32_t screenOff);

// Receives the library log (errors, warnings, info). level: 0 error, 1 warn, 2 info, 3 debug.
// The function may be called from any thread. nullptr restores the default (stderr).
typedef void (*MuxTunnelCoreLogFunction)(int32_t level, const char* message);
void muxTunnelCoreSetLogFunction(MuxTunnelCoreLogFunction function);

// Finds the file descriptor of the utun interface of this Network Extension process
// (-1 when it cannot be found). Fallback for packetFlow's "socket.fileDescriptor" key.
int32_t muxTunnelCoreFindUtunDescriptor(void);

#ifdef __cplusplus
}
#endif

#endif // MUXTUNNELCORE_H
