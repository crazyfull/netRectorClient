package io.github.crazyfull.muxtunnel;

import android.content.Context;
import android.content.SharedPreferences;

/** The user's configuration, kept in SharedPreferences. */
final class TunnelSettings {
    private static final String PREFS = "tunnel_settings";
    private static final String KEY_CDN_ADDRESS = "cdn_address";
    private static final String KEY_CDN_PORT = "cdn_port";
    private static final String KEY_TUNNELS = "tunnels";
    private static final String KEY_HS_HOST = "handshake_host";
    private static final String KEY_HS_PATH = "handshake_path";
    private static final String KEY_WS_KEY = "ws_key";
    private static final String KEY_WS_ACCEPT = "ws_accept";
    private static final String KEY_WANTED = "wanted_running";

    String cdnAddress = DefaultSettings.CDN_ADDRESS;
    int cdnPort = DefaultSettings.CDN_PORT;
    int tunnelCount = DefaultSettings.TUNNEL_COUNT;
    String handshakeHost = DefaultSettings.HANDSHAKE_HOST;
    String handshakePath = DefaultSettings.HANDSHAKE_PATH;
    String webSocketKey = DefaultSettings.WEBSOCKET_KEY;
    String expectedAccept = DefaultSettings.EXPECTED_ACCEPT;

    static TunnelSettings load(Context context) {
        SharedPreferences p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        TunnelSettings s = new TunnelSettings();
        s.cdnAddress = p.getString(KEY_CDN_ADDRESS, s.cdnAddress);
        s.cdnPort = p.getInt(KEY_CDN_PORT, s.cdnPort);
        s.tunnelCount = p.getInt(KEY_TUNNELS, s.tunnelCount);
        s.handshakeHost = p.getString(KEY_HS_HOST, s.handshakeHost);
        s.handshakePath = p.getString(KEY_HS_PATH, s.handshakePath);
        s.webSocketKey = p.getString(KEY_WS_KEY, s.webSocketKey);
        s.expectedAccept = p.getString(KEY_WS_ACCEPT, s.expectedAccept);
        return s;
    }

    void save(Context context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString(KEY_CDN_ADDRESS, cdnAddress)
                .putInt(KEY_CDN_PORT, cdnPort)
                .putInt(KEY_TUNNELS, tunnelCount)
                .putString(KEY_HS_HOST, handshakeHost)
                .putString(KEY_HS_PATH, handshakePath)
                .putString(KEY_WS_KEY, webSocketKey)
                .putString(KEY_WS_ACCEPT, expectedAccept)
                .apply();
    }

    /** True when the settings can be used to start the tunnel. */
    boolean isValid() {
        return cdnAddress != null && !cdnAddress.isEmpty() && cdnAddress.indexOf(' ') < 0
                && cdnPort >= 1 && cdnPort <= 65535
                && tunnelCount >= DefaultSettings.MIN_TUNNEL_COUNT
                && tunnelCount <= DefaultSettings.MAX_TUNNEL_COUNT
                && handshakePath != null && handshakePath.startsWith("/");
    }

    /** The user pressed Start and did not press Stop: the service restarts itself after being killed. */
    static boolean isWantedRunning(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_WANTED, false);
    }

    static void setWantedRunning(Context context, boolean wanted) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(KEY_WANTED, wanted).apply();
    }
}
