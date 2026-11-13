import Foundation

/// Static defaults of the app. They mirror example/mux-tunnelClient/src/DefaultClientConfig.h;
/// the user changes the editable ones in the app (saved by TunnelConfigurationStore).
enum DefaultTunnelSettings {
    // Placeholders: enter your own CDN / server in the app. IP or domain.
    static let cdnAddress = "irc.cdn.ir"
    static let cdnPort = 8080

    // Websocket-style handshake sent as the first bytes of every tunnel connection.
    static let handshakeHost = "sv1.server.ir"
    static let handshakePath = "/"
    static let webSocketKey = "dGhlIHNhbXBsZSBub25jzQ=="
    // Value of Sec-WebSocket-Accept the 101 response must contain (empty = do not check).
    static let expectedAccept = "dGhlIHNhbXBsZSBub25jzQ="

    // Number of mux tunnels; SOCKS5 connections are spread over them round-robin.
    static let minTunnelCount = 1
    static let maxTunnelCount = 16
    static let tunnelCount = 1

    // Mux ping interval in seconds (0 = off). Also keeps idle CDN connections alive.
    static let keepAliveSeconds: UInt32 = 30

    // Local SOCKS5 listener between the tun2socks engine and the mux client (loopback only).
    static let socksAddress = "127.0.0.1"

    // VPN interface (private addresses, they never leave the phone).
    static let tunnelRemoteAddress = "127.0.0.1"
    static let tunnelIPv4 = "198.18.0.1"
    static let tunnelIPv4Mask = "255.255.255.255"
    static let tunnelIPv6 = "fc00::1"
    static let tunnelIPv6PrefixLength = 128
    static let dnsAddress = "198.18.0.2"           // answered by the engine itself (mapped DNS)
    static let mappedNetwork = "100.64.0.0"
    static let mappedNetmask = "255.192.0.0"
    static let mtu = 1500

    // Bundle identifiers: the extension must be a child of the app.
    static let appBundleIdentifier = "io.github.crazyfull.muxtunnel"
    static let extensionBundleIdentifier = "io.github.crazyfull.muxtunnel.tunnel"
}
