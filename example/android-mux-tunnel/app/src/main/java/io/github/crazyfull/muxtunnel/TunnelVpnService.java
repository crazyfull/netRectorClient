package io.github.crazyfull.muxtunnel;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.net.VpnService;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.nio.charset.Charset;
import java.util.Locale;

import hev.htproxy.TProxyService;

/**
 * Foreground VPN service: runs the mux-tunnelClient core (SOCKS5 on loopback) and the
 * hev-socks5-tunnel engine that turns the phone's IP traffic into SOCKS5 connections.
 *
 * <pre>
 * apps -> TUN -> hev-socks5-tunnel -> SOCKS5 127.0.0.1:port -> mux-tunnelClient -> CDN -> server
 * </pre>
 *
 * The app itself is excluded from the VPN, so the tunnel connections to the CDN do not loop back
 * into the TUN. A partial wake lock and a Wi-Fi lock keep it working with the screen off.
 */
public class TunnelVpnService extends VpnService {
    static final String ACTION_START = "io.github.crazyfull.muxtunnel.START";
    static final String ACTION_STOP = "io.github.crazyfull.muxtunnel.STOP";

    private static final String TAG = "MuxTunnel";
    private static final String CHANNEL_ID = "mux_tunnel";
    private static final int NOTIFICATION_ID = 1;
    private static final String HEV_CONFIG_FILE = "hev-socks5-tunnel.yml";

    private static volatile TunnelState sState = TunnelState.STOPPED;
    private static volatile int sErrorResource = 0;

    static TunnelState getState() {
        return sState;
    }

    /** String resource id that explains the last ERROR state (0 = none). */
    static int getErrorResource() {
        return sErrorResource;
    }

    private final Object lock = new Object();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private ParcelFileDescriptor tun;
    private Thread coreThread;
    private volatile boolean stopping;
    private boolean hevStarted;
    private PowerManager.WakeLock wakeLock;
    private WifiManager.WifiLock wifiLock;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = intent == null ? null : intent.getAction();

        if (ACTION_STOP.equals(action)) {
            TunnelSettings.setWantedRunning(this, false);
            stopTunnel(TunnelState.STOPPED, 0);
            stopSelf();
            return START_NOT_STICKY;
        }

        // A null intent is the system restarting the service after it was killed
        if (intent == null && !TunnelSettings.isWantedRunning(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }

        // Must be called within seconds of startForegroundService()
        showForeground(getString(R.string.notification_starting));
        startTunnel();
        return START_STICKY;
    }

    @Override
    public void onRevoke() {
        // The user turned the VPN off in the system dialog or another VPN app took over
        TunnelSettings.setWantedRunning(this, false);
        stopTunnel(TunnelState.STOPPED, 0);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        stopTunnel(sState == TunnelState.ERROR ? TunnelState.ERROR : TunnelState.STOPPED, sErrorResource);
        super.onDestroy();
    }

    // ---------------------------------------------------------------- start / stop

    private void startTunnel() {
        synchronized (lock) {
            if (sState == TunnelState.STARTING || sState == TunnelState.RUNNING)
                return;

            sState = TunnelState.STARTING;
            sErrorResource = 0;
            stopping = false;
            TunnelSettings.setWantedRunning(this, true);

            final TunnelSettings settings = TunnelSettings.load(this);
            if (!settings.isValid()) {
                failLocked(R.string.error_settings);
                return;
            }

            acquireLocks();

            final int socksPort = findFreePort();

            // 1. the mux client: SOCKS5 listener on loopback + tunnels to the CDN
            MuxTunnelNative.nativeReset();
            coreThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    final int code = MuxTunnelNative.nativeRun(
                            DefaultSettings.SOCKS_ADDRESS, socksPort, settings.cdnAddress, settings.cdnPort,
                            settings.tunnelCount, settings.handshakeHost, settings.handshakePath,
                            settings.webSocketKey, settings.expectedAccept,
                            DefaultSettings.KEEP_ALIVE_SECONDS, false);
                    onCoreExited(code);
                }
            }, "mux-core");
            coreThread.start();

            // 2. the TUN interface
            try {
                tun = buildVpn(settings);
            } catch (Exception e) {
                Log.e(TAG, "cannot establish the VPN", e);
                tun = null;
            }
            if (tun == null) {
                failLocked(R.string.error_vpn_permission);
                return;
            }

            // 3. the engine that moves packets between the TUN and the SOCKS5 port
            try {
                final File config = new File(getFilesDir(), HEV_CONFIG_FILE);
                writeFile(config, buildHevConfig(socksPort));
                hevStarted = TProxyService.TProxyStartService(config.getAbsolutePath(), tun.getFd());
            } catch (Exception e) {
                Log.e(TAG, "cannot start the tun2socks engine", e);
                hevStarted = false;
            }
            if (!hevStarted) {
                failLocked(R.string.error_engine);
                return;
            }

            sState = TunnelState.RUNNING;
            showForeground(getString(R.string.notification_running, settings.cdnAddress, settings.cdnPort));
        }
    }

    /** Stops everything and publishes the final state. Safe to call more than once. */
    private void stopTunnel(TunnelState finalState, int errorResource) {
        synchronized (lock) {
            stopLocked(finalState, errorResource);
        }
    }

    private void stopLocked(TunnelState finalState, int errorResource) {
        stopping = true;

        if (hevStarted) {
            try {
                TProxyService.TProxyStopService();
            } catch (Throwable t) {
                Log.w(TAG, "stopping the engine failed", t);
            }
            hevStarted = false;
        }

        if (tun != null) {
            try {
                tun.close();
            } catch (IOException ignored) {
            }
            tun = null;
        }

        if (coreThread != null) {
            MuxTunnelNative.nativeStop();
            try {
                coreThread.join(3000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            coreThread = null;
        }

        releaseLocks();
        sErrorResource = errorResource;
        sState = finalState;
        stopForeground(true);
    }

    private void failLocked(int errorResource) {
        Log.e(TAG, "start failed: " + getString(errorResource));
        TunnelSettings.setWantedRunning(this, false);
        stopLocked(TunnelState.ERROR, errorResource);
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                stopSelf();
            }
        });
    }

    /** Called on the core thread when nativeRun() returns. */
    private void onCoreExited(final int code) {
        // No lock here: stopLocked() holds it while it joins this thread
        if (stopping)
            return;
        // The core ended by itself: the SOCKS5 port could not be opened
        Log.e(TAG, "mux client exited with code " + code);
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                synchronized (lock) {
                    if (stopping)
                        return;
                    TunnelSettings.setWantedRunning(TunnelVpnService.this, false);
                    stopLocked(TunnelState.ERROR, R.string.error_core);
                }
                stopSelf();
            }
        });
    }

    // ---------------------------------------------------------------- VPN + engine config

    private ParcelFileDescriptor buildVpn(TunnelSettings settings) {
        Builder builder = new Builder();
        builder.setSession(getString(R.string.app_name));
        builder.setMtu(DefaultSettings.MTU);
        builder.addAddress(DefaultSettings.TUN_IPV4, 32);
        builder.addRoute("0.0.0.0", 0);
        builder.addDnsServer(DefaultSettings.DNS_ADDRESS);

        try {
            builder.addAddress(DefaultSettings.TUN_IPV6, 128);
            builder.addRoute("::", 0);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "IPv6 is not available on this device", e);
        }

        try {
            // the tunnel connections to the CDN must not go into the VPN again
            builder.addDisallowedApplication(getPackageName());
        } catch (Exception e) {
            Log.w(TAG, "cannot exclude the app from the VPN", e);
        }

        if (Build.VERSION.SDK_INT >= 29)
            builder.setMetered(false);

        return builder.establish();
    }

    private String buildHevConfig(int socksPort) {
        return String.format(Locale.US,
                "tunnel:\n"
                + "  mtu: %d\n"
                + "  ipv4: %s\n"
                + "  ipv6: '%s'\n"
                + "socks5:\n"
                + "  port: %d\n"
                + "  address: %s\n"
                + "  udp: 'udp'\n"
                + "mapdns:\n"
                + "  address: %s\n"
                + "  port: 53\n"
                + "  network: %s\n"
                + "  netmask: %s\n"
                + "  cache-size: 10000\n"
                + "misc:\n"
                + "  task-stack-size: 81920\n"
                + "  tcp-buffer-size: 65536\n"
                + "  connect-timeout: 10000\n"
                + "  tcp-read-write-timeout: 300000\n"
                + "  log-level: warn\n",
                DefaultSettings.MTU, DefaultSettings.TUN_IPV4, DefaultSettings.TUN_IPV6,
                socksPort, DefaultSettings.SOCKS_ADDRESS,
                DefaultSettings.DNS_ADDRESS, DefaultSettings.MAPPED_NETWORK, DefaultSettings.MAPPED_NETMASK);
    }

    private static void writeFile(File file, String content) throws IOException {
        FileOutputStream out = new FileOutputStream(file);
        try {
            out.write(content.getBytes(Charset.forName("UTF-8")));
        } finally {
            out.close();
        }
    }

    /** A free loopback port for the local SOCKS5 listener. */
    private static int findFreePort() {
        try {
            ServerSocket socket = new ServerSocket(0, 1, InetAddress.getByName(DefaultSettings.SOCKS_ADDRESS));
            try {
                return socket.getLocalPort();
            } finally {
                socket.close();
            }
        } catch (IOException e) {
            return DefaultSettings.SOCKS_FALLBACK_PORT;
        }
    }

    // ---------------------------------------------------------------- keep running with the screen off

    @SuppressWarnings("deprecation")
    private void acquireLocks() {
        try {
            PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
            wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "muxtunnel:core");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();

            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi != null) {
                wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "muxtunnel:wifi");
                wifiLock.setReferenceCounted(false);
                wifiLock.acquire();
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot acquire the wake locks", e);
        }
    }

    private void releaseLocks() {
        try {
            if (wakeLock != null && wakeLock.isHeld())
                wakeLock.release();
            if (wifiLock != null && wifiLock.isHeld())
                wifiLock.release();
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot release the wake locks", e);
        }
        wakeLock = null;
        wifiLock = null;
    }

    // ---------------------------------------------------------------- notification

    private void showForeground(String text) {
        try {
            startForeground(NOTIFICATION_ID, buildNotification(text));
        } catch (RuntimeException e) {
            // e.g. Android 12+ refuses a foreground start from the background
            Log.w(TAG, "cannot start in the foreground", e);
        }
    }

    private static int pendingIntentFlags() {
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23)
            flags |= PendingIntent.FLAG_IMMUTABLE;
        return flags;
    }

    @SuppressWarnings("deprecation")
    private Notification buildNotification(String text) {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                    getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW);
            manager.createNotificationChannel(channel);
        }

        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), pendingIntentFlags());
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, TunnelVpnService.class).setAction(ACTION_STOP), pendingIntentFlags());

        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        builder.setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(text)
                .setContentIntent(open)
                .setOngoing(true)
                .addAction(R.drawable.ic_notification, getString(R.string.action_stop), stop);
        return builder.build();
    }
}
