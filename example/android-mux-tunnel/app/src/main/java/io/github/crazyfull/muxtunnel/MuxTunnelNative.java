package io.github.crazyfull.muxtunnel;

/**
 * JNI binding of the mux-tunnelClient core (netRectorClient). {@link #nativeRun} blocks, so it must
 * be called from its own thread; {@link #nativeStop} may be called from any thread.
 */
final class MuxTunnelNative {
    /** nativeRun result: the local SOCKS5 port could not be opened. */
    static final int RESULT_CANNOT_LISTEN = 1;
    /** nativeRun result: TLS was requested but this build has no TLS (no BoringSSL). */
    static final int RESULT_TLS_UNAVAILABLE = 3;

    private MuxTunnelNative() {
    }

    static {
        System.loadLibrary("muxtunnel");
    }

    /** Forgets a stop request left over from a previous run. Call before starting the thread. */
    static native void nativeReset();

    static native int nativeRun(String listenAddress, int listenPort, String cdnAddress, int cdnPort,
                                int tunnelCount, String handshakeHost, String handshakePath,
                                String webSocketKey, String expectedAccept, int keepAliveSeconds,
                                boolean verbose, boolean useTls,
                                boolean udpEnabled, int udpIdleSeconds, int udpIdleScreenOffSeconds);

    /** Tunnels that are connected right now (0 when the core is not running). Thread-safe. */
    static native int nativeConnectedTunnels();

    /** 1 while the UDP tunnel is connected or connecting (0 when UDP is off). Thread-safe. */
    static native int nativeUdpTunnelUp();

    /** Screen off: the UDP tunnel closes sooner when idle. Thread-safe. */
    static native void nativeSetScreenOff(boolean screenOff);

    static native void nativeStop();

    /** The core's log (newest last) as one text, kept for the Diagnostics screen. */
    static native String nativeGetLog();

    static native void nativeClearLog();
}
