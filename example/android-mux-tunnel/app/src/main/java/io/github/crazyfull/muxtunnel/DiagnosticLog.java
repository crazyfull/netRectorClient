package io.github.crazyfull.muxtunnel;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.Charset;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;

/**
 * The app's own log (service events, probes, periodic statistics), kept in memory for the
 * Diagnostics screen. The core's log lives in native code ({@link MuxTunnelNative#nativeGetLog}) and
 * the engine (hev-socks5-tunnel) writes its own file; {@link #fullReport} puts the three together.
 */
final class DiagnosticLog {
    private static final String TAG = "MuxTunnel";
    private static final int MAX_LINES = 600;
    private static final int ENGINE_TAIL_BYTES = 48 * 1024;

    private static final Object LOCK = new Object();
    private static final ArrayList<String> LINES = new ArrayList<String>();

    private DiagnosticLog() {
    }

    static void add(String message) {
        Log.i(TAG, message);
        final String line = new SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(new Date()) + " " + message;
        synchronized (LOCK) {
            LINES.add(line);
            while (LINES.size() > MAX_LINES)
                LINES.remove(0);
        }
    }

    static void clear() {
        synchronized (LOCK) {
            LINES.clear();
        }
        try {
            MuxTunnelNative.nativeClearLog();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private static String appLines() {
        final StringBuilder text = new StringBuilder();
        synchronized (LOCK) {
            for (String line : LINES)
                text.append(line).append('\n');
        }
        return text.toString();
    }

    /** Last bytes of the engine's log file (empty when there is none). */
    static String engineTail(File file) {
        if (file == null || !file.exists())
            return "";
        try {
            final RandomAccessFile input = new RandomAccessFile(file, "r");
            try {
                final long length = input.length();
                final long start = Math.max(0, length - ENGINE_TAIL_BYTES);
                input.seek(start);
                final byte[] bytes = new byte[(int) (length - start)];
                input.readFully(bytes);
                String text = new String(bytes, Charset.forName("UTF-8"));
                if (start > 0) {
                    final int firstLine = text.indexOf('\n');       // drop the cut line
                    text = firstLine >= 0 ? text.substring(firstLine + 1) : text;
                }
                return text;
            } finally {
                input.close();
            }
        } catch (IOException e) {
            return "cannot read the engine log: " + e.getMessage() + "\n";
        }
    }

    /** Everything, for the screen and for sharing. */
    static String fullReport(File engineLog) {
        String core;
        try {
            core = MuxTunnelNative.nativeGetLog();
        } catch (UnsatisfiedLinkError e) {
            core = "(native library not loaded)\n";
        }
        return "== app (build " + DefaultSettings.BUILD_STAMP + ", " + android.os.Build.MANUFACTURER + " "
                + android.os.Build.MODEL + ", Android " + android.os.Build.VERSION.RELEASE
                + " / API " + android.os.Build.VERSION.SDK_INT + ")\n"
                + appLines()
                + "\n== core (mux client)\n" + core
                + "\n== engine (hev-socks5-tunnel, last part)\n" + engineTail(engineLog);
    }
}
