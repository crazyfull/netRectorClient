package io.github.crazyfull.muxtunnel;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.VpnService;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Debug;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
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
    static final String HEV_LOG_FILE = "hev-socks5-tunnel.log";
    private static final long STATS_INTERVAL_MS = 3000;

    private static volatile TunnelState sState = TunnelState.STOPPED;
    private static volatile int sErrorResource = 0;
    private static volatile int sTotalTunnels = 0;
    private static volatile boolean sUdpEnabled = false;

    static TunnelState getState() {
        return sState;
    }

    /** Tunnels connected right now (0 unless the service is running). */
    static int getConnectedTunnels() {
        return sState == TunnelState.RUNNING ? MuxTunnelNative.nativeConnectedTunnels() : 0;
    }

    /** Tunnels the running configuration asks for. */
    /** -1: UDP is off, 0: enabled but idle (connects on the first UDP flow), 1: the UDP tunnel is connected. */
    static int getUdpTunnelState() {
        if (!sUdpEnabled || (sState != TunnelState.RUNNING && sState != TunnelState.STARTING))
            return -1;
        return MuxTunnelNative.nativeUdpTunnelUp();
    }

    static int getTotalTunnels() {
        return sState == TunnelState.RUNNING || sState == TunnelState.STARTING ? sTotalTunnels : 0;
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
    private ConnectivityManager.NetworkCallback networkCallback;
    private long lastTxBytes = -1;
    private long lastRxBytes = -1;
    private int stalledTicks;
    private boolean screenReceiverRegistered;

    /** Screen off / on: the UDP tunnel closes sooner when idle while the screen is off. */
    private final BroadcastReceiver screenReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final boolean off = Intent.ACTION_SCREEN_OFF.equals(intent.getAction());
            DiagnosticLog.add("screen " + (off ? "off" : "on"));
            MuxTunnelNative.nativeSetScreenOff(off);
        }
    };

    /** Every few seconds: how many tunnels are up and how much the engine moved. */
    private final Runnable statsTicker = new Runnable() {
        @Override
        public void run() {
            if (sState != TunnelState.STARTING && sState != TunnelState.RUNNING)
                return;
            logStats();
            mainHandler.postDelayed(this, STATS_INTERVAL_MS);
        }
    };

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = intent == null ? null : intent.getAction();

        DiagnosticLog.add("service command: " + (action == null ? "restart by the system (null intent)" : action));

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
        DiagnosticLog.add("onRevoke: the VPN was revoked (turned off in the system or another VPN took over)");
        TunnelSettings.setWantedRunning(this, false);
        stopTunnel(TunnelState.STOPPED, 0);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        DiagnosticLog.add("onDestroy");
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
            sTotalTunnels = settings.tunnelCount;
            sUdpEnabled = settings.useUdp;
            DiagnosticLog.add("start: server " + settings.cdnAddress + ":" + settings.cdnPort + ", tunnels "
                    + settings.tunnelCount + ", TLS " + (settings.useTls ? "on" : "off") + (settings.sniBypass ? " (SNI bypass)" : "") + ", Host "
                    + settings.handshakeHost + ", path " + settings.handshakePath + ", UDP "
                    + (settings.useUdp ? "on" : "off"));
            if (!settings.isValid()) {
                failLocked(R.string.error_settings);
                return;
            }

            acquireLocks();
            lastTxBytes = -1;
            lastRxBytes = -1;
            stalledTicks = 0;
            new File(getFilesDir(), HEV_LOG_FILE).delete();      // a fresh engine log for this run
            probeServer(settings);
            registerNetworkCallback();

            final int socksPort = findFreePort();
            DiagnosticLog.add("SOCKS5 listener port " + socksPort);

            // 1. the mux client: SOCKS5 listener on loopback + tunnels to the CDN
            MuxTunnelNative.nativeReset();
            coreThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    final int code = MuxTunnelNative.nativeRun(
                            DefaultSettings.SOCKS_ADDRESS, socksPort, settings.cdnAddress, settings.cdnPort,
                            settings.tunnelCount, settings.handshakeHost, settings.handshakePath,
                            settings.webSocketKey, settings.expectedAccept,
                            DefaultSettings.KEEP_ALIVE_SECONDS, true, settings.useTls,
                            settings.useUdp, DefaultSettings.UDP_IDLE_SECONDS,
                            DefaultSettings.UDP_IDLE_SCREEN_OFF_SECONDS, settings.sniBypass);
                    DiagnosticLog.add("mux core returned " + code);
                    onCoreExited(code);
                }
            }, "mux-core");
            coreThread.start();

            registerScreenReceiver();

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
            DiagnosticLog.add("TUN established, fd " + tun.getFd() + ", mtu " + DefaultSettings.MTU);

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
            DiagnosticLog.add("engine (hev-socks5-tunnel) started");

            sState = TunnelState.RUNNING;
            mainHandler.removeCallbacks(statsTicker);
            mainHandler.postDelayed(statsTicker, STATS_INTERVAL_MS);
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
        DiagnosticLog.add("stopping (" + finalState + (errorResource != 0 ? ": " + getString(errorResource) : "") + ")");
        stopping = true;
        mainHandler.removeCallbacks(statsTicker);
        unregisterNetworkCallback();
        unregisterScreenReceiver();

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
        DiagnosticLog.add("stopped, state " + finalState);
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
        DiagnosticLog.add("the mux client ended by itself with code " + code);
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                synchronized (lock) {
                    if (stopping)
                        return;
                    TunnelSettings.setWantedRunning(TunnelVpnService.this, false);
                    stopLocked(TunnelState.ERROR, code == MuxTunnelNative.RESULT_TLS_UNAVAILABLE
                            ? R.string.error_tls : R.string.error_core);
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
                + "  udp: 'tcp'\n"      // UDP-in-TCP; the core refuses it (or carries it) depending on "Enable UDP"
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
                + "  log-file: '%s'\n"
                + "  log-level: info\n",
                DefaultSettings.MTU, DefaultSettings.TUN_IPV4, DefaultSettings.TUN_IPV6,
                socksPort, DefaultSettings.SOCKS_ADDRESS,
                DefaultSettings.DNS_ADDRESS, DefaultSettings.MAPPED_NETWORK, DefaultSettings.MAPPED_NETMASK,
                new File(getFilesDir(), HEV_LOG_FILE).getAbsolutePath());
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

    // ---------------------------------------------------------------- diagnostics

    /** Can this phone reach the server directly (the app is outside the VPN)? Logged, not acted on. */
    private void probeServer(final TunnelSettings settings) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final long begin = System.currentTimeMillis();
                final Socket socket = new Socket();
                try {
                    final InetSocketAddress address = new InetSocketAddress(settings.cdnAddress, settings.cdnPort);
                    if (address.isUnresolved()) {
                        DiagnosticLog.add("probe: cannot resolve " + settings.cdnAddress);
                        return;
                    }
                    socket.connect(address, 5000);
                    DiagnosticLog.add("probe: " + address.getAddress().getHostAddress() + ":" + settings.cdnPort
                            + " connected in " + (System.currentTimeMillis() - begin) + " ms, local address "
                            + socket.getLocalAddress().getHostAddress());
                } catch (IOException e) {
                    DiagnosticLog.add("probe: cannot connect to " + settings.cdnAddress + ":" + settings.cdnPort
                            + " after " + (System.currentTimeMillis() - begin) + " ms: " + e);
                } finally {
                    try {
                        socket.close();
                    } catch (IOException ignored) {
                    }
                }
            }
        }, "mux-probe").start();
    }

    private void logStats() {
        String traffic = "engine stats not available";
        try {
            final long[] stats = TProxyService.TProxyGetStats();       // tx p, tx B, rx p, rx B
            if (stats != null && stats.length >= 4) {
                traffic = "tx " + stats[0] + "p/" + stats[1] + "B, rx " + stats[2] + "p/" + stats[3] + "B";
                // Traffic leaves but nothing comes back: the tunnel looks up but does not carry data
                final boolean sent = lastTxBytes >= 0 && stats[1] > lastTxBytes;
                final boolean received = lastRxBytes >= 0 && stats[3] > lastRxBytes;
                stalledTicks = sent && !received ? stalledTicks + 1 : 0;
                if (stalledTicks == 2 || (stalledTicks > 2 && stalledTicks % 10 == 0))
                    DiagnosticLog.add("WARNING: data goes out but nothing comes back for "
                            + (stalledTicks * STATS_INTERVAL_MS / 1000) + " s");
                lastTxBytes = stats[1];
                lastRxBytes = stats[3];
            }
        } catch (Throwable t) {
            traffic = "engine stats failed: " + t;
        }
        DiagnosticLog.add("tick tunnels " + MuxTunnelNative.nativeConnectedTunnels() + "/" + sTotalTunnels
                + ", udp tunnel " + (MuxTunnelNative.nativeUdpTunnelUp() == 1 ? "up" : "down") + ", "
                + traffic + ", native heap " + (Debug.getNativeHeapAllocatedSize() / (1024 * 1024)) + " MB");
    }

    /** The phone's own network (Wi-Fi, mobile data) and its changes: a change is a common reason for a dead tunnel. */
    private void registerNetworkCallback() {
        if (Build.VERSION.SDK_INT < 24)
            return;
        try {
            final ConnectivityManager manager = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            networkCallback = new ConnectivityManager.NetworkCallback() {
                @Override
                public void onAvailable(Network network) {
                    DiagnosticLog.add("network: available " + network);
                }

                @Override
                public void onLost(Network network) {
                    DiagnosticLog.add("network: LOST " + network);
                }

                @Override
                public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) {
                    DiagnosticLog.add("network: " + network + " " + describe(capabilities));
                }

                @Override
                public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
                    DiagnosticLog.add("network: " + network + " interface " + properties.getInterfaceName()
                            + ", addresses " + properties.getLinkAddresses() + ", dns " + properties.getDnsServers());
                }
            };
            manager.registerDefaultNetworkCallback(networkCallback);
        } catch (RuntimeException e) {
            networkCallback = null;
            DiagnosticLog.add("network monitor not available: " + e);
        }
    }

    private void registerScreenReceiver() {
        try {
            final IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
            filter.addAction(Intent.ACTION_SCREEN_ON);
            registerReceiver(screenReceiver, filter);
            screenReceiverRegistered = true;
            final PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (power != null && Build.VERSION.SDK_INT >= 20)
                MuxTunnelNative.nativeSetScreenOff(!power.isInteractive());
        } catch (RuntimeException e) {
            DiagnosticLog.add("screen monitor not available: " + e);
        }
    }

    private void unregisterScreenReceiver() {
        if (!screenReceiverRegistered)
            return;
        try {
            unregisterReceiver(screenReceiver);
        } catch (RuntimeException ignored) {
        }
        screenReceiverRegistered = false;
    }

    private void unregisterNetworkCallback() {
        if (networkCallback == null)
            return;
        try {
            ((ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE)).unregisterNetworkCallback(networkCallback);
        } catch (RuntimeException ignored) {
        }
        networkCallback = null;
    }

    private static String describe(NetworkCapabilities capabilities) {
        final StringBuilder text = new StringBuilder();
        if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI))
            text.append("Wi-Fi");
        else if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR))
            text.append("mobile data");
        else if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN))
            text.append("VPN");
        else
            text.append("other transport");
        text.append(capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                ? ", internet validated" : ", NOT validated");
        return text.toString();
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
