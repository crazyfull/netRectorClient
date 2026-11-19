#ifndef DEFAULTCLIENTCONFIG_H
#define DEFAULTCLIENTCONFIG_H

#include <cstddef>
#include <cstdint>

// Static defaults of mux-tunnelClient. Edit them here and rebuild, or override any of them
// at run time with a command line option (see ClientCommandLine).
namespace DefaultClientConfig {

// Local SOCKS5 listener.
constexpr const char* ListenAddress = "0.0.0.0";
constexpr uint16_t ListenPort = 1080;

// Where the tunnels connect to: the CDN edge (or mux-tunnelServer itself when there is no CDN).
constexpr const char* CdnAddress = "irc.x81.ir";
constexpr uint16_t CdnPort = 8080;

// Websocket-style handshake sent as the first bytes of every tunnel connection.
constexpr const char* HandshakeHost = "mojz.ir";    //bot.mojz.ir
constexpr const char* HandshakePath = "/";
constexpr const char* WebSocketKey = "dGhlIHNhbXBsZSBub25jzQ==";
// Value of Sec-WebSocket-Accept the 101 response must contain (empty = do not check).
constexpr const char* ExpectedWebSocketAccept = "dGhlIHNhbXBsZSBub25jzQ=";

// TLS (HTTPS) for the tunnel connections. The handshake request is sent inside the TLS session.
// The client needs no certificate or key. By default the server certificate is NOT verified, so
// a self-signed or otherwise invalid certificate works; enable verification for a real certificate.
constexpr bool UseTls = false;
constexpr bool TlsVerifyPeer = false;
// Record-layer "SNI bypass" of the TLS tunnels (see ClientConfig::sniTLSBypass). Off by default.
constexpr bool SniTlsBypass = false;
// SNI (and the name checked when verifying). Empty = use HandshakeHost.
constexpr const char* TlsServerName = "";
// CA bundle used when verifying. Empty = system CAs (on iOS and Windows a bundle must be given).
constexpr const char* TlsCaFile = "";

// Number of mux tunnels (1 - 16). New SOCKS5 connections are spread over them round-robin.
constexpr int MinTunnelCount = 1;
constexpr int MaxTunnelCount = 16;
constexpr int TunnelCount = 1;

// Mux ping interval in seconds (0 = off). Also keeps idle CDN connections alive.
constexpr uint32_t KeepAliveSeconds = 30;
// Time a tunnel has to complete connect + handshake before it is dropped and retried.
constexpr uint32_t HandshakeTimeoutSeconds = 10;
// Time a local SOCKS5 client has to finish the greeting.
constexpr uint32_t LocalGreetingTimeoutSeconds = 30;
// Maximum number of concurrent streams (SOCKS5 connections) per tunnel. Keep it <= the server's.
constexpr size_t MaxStreamsPerTunnel = 32768;
// Longest time the main loop sleeps between two passes (ms). There are no timers in
// netRectorClient, so the application drives the event loop itself: it sleeps until the next
// tunnel reconnect is due, but never longer than this (also the latency of a stop request).
constexpr int MaxLoopSliceMs = 1000;
// Shortest sleep, to avoid a busy loop when a deadline has just passed.
constexpr int MinLoopSliceMs = 10;

// UDP-over-TCP (SOCKS5 FWD UDP of hev-socks5-tunnel). OFF by default: the TCP tunnels never carry
// UDP. When enabled, UDP flows use ONE separate tunnel that is connected on the first UDP flow and
// closed again when no datagram moved for a while (the next flow reconnects it).
constexpr bool UdpEnabled = false;
// Close the UDP tunnel after this many seconds without any datagram (0 = never close it).
constexpr uint32_t UdpIdleSeconds = 60;
// Same, while the screen is off (see ClientApplication::setScreenOff). Active calls keep it open.
constexpr uint32_t UdpIdleScreenOffSeconds = 15;
// Mux ping interval of the UDP tunnel (0 = off). Keep it above the idle time if you do not want
// pings to keep an unused tunnel alive; the idle close happens first anyway.
constexpr uint32_t UdpKeepAliveSeconds = 30;
// Concurrent UDP flows (streams) on the UDP tunnel. Keep it <= the server's --udp-max-streams.
constexpr size_t UdpMaxStreams = 256;
// Shortest time between two connect attempts of the UDP tunnel after a failure.
constexpr int UdpRetryDelayMs = 1000;

// Delay before a lost tunnel is re-established; doubles on every failure up to the maximum.
constexpr int ReconnectInitialDelayMs = 500;
constexpr int ReconnectMaxDelayMs = 10000;

} // namespace DefaultClientConfig

#endif // DEFAULTCLIENTCONFIG_H
