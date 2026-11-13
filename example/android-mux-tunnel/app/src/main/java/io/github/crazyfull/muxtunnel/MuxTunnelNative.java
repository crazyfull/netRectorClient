package io.github.crazyfull.muxtunnel;

/**
 * JNI binding of the mux-tunnelClient core (netRectorClient). {@link #nativeRun} blocks, so it must
 * be called from its own thread; {@link #nativeStop} may be called from any thread.
 */
final class MuxTunnelNative {
    /** nativeRun result: the local SOCKS5 port could not be opened. */
    static final int RESULT_CANNOT_LISTEN = 1;

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
                                boolean verbose);

    static native void nativeStop();
}
