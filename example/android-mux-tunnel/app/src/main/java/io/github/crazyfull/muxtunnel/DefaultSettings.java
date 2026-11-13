package io.github.crazyfull.muxtunnel;

/**
 * Static defaults of the app. They mirror example/mux-tunnelClient/src/DefaultClientConfig.h;
 * edit here and rebuild, or change them at run time in the app (saved in TunnelSettings).
 */
final class DefaultSettings {
    private DefaultSettings() {
    }

    // Where the tunnels connect to: the CDN edge (or mux-tunnelServer itself). IP or domain.
    static final String CDN_ADDRESS = "irc.x81.ir";
    static final int CDN_PORT = 8080;

    // Websocket-style handshake sent as the first bytes of every tunnel connection.
    static final String HANDSHAKE_HOST = "mojz.ir";
    static final String HANDSHAKE_PATH = "/";
    static final String WEBSOCKET_KEY = "dGhlIHNhbXBsZSBub25jzQ==";
    // Value of Sec-WebSocket-Accept the 101 response must contain (empty = do not check).
    static final String EXPECTED_ACCEPT = "dGhlIHNhbXBsZSBub25jzQ=";

    // Number of mux tunnels; SOCKS5 connections are spread over them round-robin.
    static final int MIN_TUNNEL_COUNT = 1;
    static final int MAX_TUNNEL_COUNT = 16;
    static final int TUNNEL_COUNT = 1;

    // Mux ping interval in seconds (0 = off). Also keeps idle CDN connections alive.
    static final int KEEP_ALIVE_SECONDS = 30;

    // Local SOCKS5 listener between the tun2socks engine and the mux client (loopback only).
    static final String SOCKS_ADDRESS = "127.0.0.1";
    static final int SOCKS_FALLBACK_PORT = 1080;

    // VPN interface (addresses are private, never leave the phone).
    static final String TUN_IPV4 = "198.18.0.1";
    static final String TUN_IPV6 = "fc00::1";
    static final String DNS_ADDRESS = "198.18.0.2";     // answered by the engine itself (mapped DNS)
    static final String MAPPED_NETWORK = "100.64.0.0";
    static final String MAPPED_NETMASK = "255.192.0.0";
    static final int MTU = 1500;
}
