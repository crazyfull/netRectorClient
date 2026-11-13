package io.github.crazyfull.muxtunnel;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

/** Settings (CDN address, port, tunnel count) and the Start / Stop button. */
public class MainActivity extends Activity {
    private static final int REQUEST_VPN = 1;
    private static final int REQUEST_NOTIFICATIONS = 2;
    private static final long REFRESH_MS = 700;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            updateUi();
            handler.postDelayed(this, REFRESH_MS);
        }
    };

    private EditText cdnAddress;
    private EditText cdnPort;
    private EditText tunnelCount;
    private EditText handshakeHost;
    private EditText handshakePath;
    private EditText webSocketKey;
    private EditText expectedAccept;
    private Button toggleButton;
    private Button batteryButton;
    private TextView statusView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        cdnAddress = (EditText) findViewById(R.id.cdn_address);
        cdnPort = (EditText) findViewById(R.id.cdn_port);
        tunnelCount = (EditText) findViewById(R.id.tunnel_count);
        handshakeHost = (EditText) findViewById(R.id.handshake_host);
        handshakePath = (EditText) findViewById(R.id.handshake_path);
        webSocketKey = (EditText) findViewById(R.id.ws_key);
        expectedAccept = (EditText) findViewById(R.id.ws_accept);
        toggleButton = (Button) findViewById(R.id.toggle_button);
        batteryButton = (Button) findViewById(R.id.battery_button);
        statusView = (TextView) findViewById(R.id.status);

        TunnelSettings settings = TunnelSettings.load(this);
        cdnAddress.setText(settings.cdnAddress);
        cdnPort.setText(String.valueOf(settings.cdnPort));
        tunnelCount.setText(String.valueOf(settings.tunnelCount));
        handshakeHost.setText(settings.handshakeHost);
        handshakePath.setText(settings.handshakePath);
        webSocketKey.setText(settings.webSocketKey);
        expectedAccept.setText(settings.expectedAccept);

        toggleButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                onToggle();
            }
        });
        batteryButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                requestIgnoreBatteryOptimizations();
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        handler.post(refresh);
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresh);
        super.onPause();
    }

    // ---------------------------------------------------------------- actions

    private void onToggle() {
        final TunnelState state = TunnelVpnService.getState();
        if (state == TunnelState.RUNNING || state == TunnelState.STARTING) {
            startService(new Intent(this, TunnelVpnService.class).setAction(TunnelVpnService.ACTION_STOP));
            return;
        }

        final TunnelSettings settings = readSettings();
        if (settings == null)
            return;
        settings.save(this);

        if (Build.VERSION.SDK_INT >= 33)
            requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, REQUEST_NOTIFICATIONS);

        // The system asks the user once to allow this app to be a VPN
        final Intent prepare = VpnService.prepare(this);
        if (prepare != null)
            startActivityForResult(prepare, REQUEST_VPN);
        else
            startTunnelService();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_VPN) {
            if (resultCode == RESULT_OK)
                startTunnelService();
            else
                Toast.makeText(this, R.string.error_vpn_permission, Toast.LENGTH_LONG).show();
        }
    }

    private void startTunnelService() {
        final Intent intent = new Intent(this, TunnelVpnService.class).setAction(TunnelVpnService.ACTION_START);
        if (Build.VERSION.SDK_INT >= 26)
            startForegroundService(intent);
        else
            startService(intent);
        updateUi();
    }

    private void requestIgnoreBatteryOptimizations() {
        if (Build.VERSION.SDK_INT < 23)
            return;
        try {
            startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
        } catch (RuntimeException e) {
            startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
        }
    }

    // ---------------------------------------------------------------- settings form

    /** Reads and validates the form; shows an error and returns null when something is wrong. */
    private TunnelSettings readSettings() {
        final TunnelSettings settings = new TunnelSettings();

        settings.cdnAddress = cdnAddress.getText().toString().trim();
        if (settings.cdnAddress.isEmpty() || settings.cdnAddress.indexOf(' ') >= 0) {
            cdnAddress.setError(getString(R.string.error_address));
            return null;
        }

        final Integer port = parseInt(cdnPort.getText().toString());
        if (port == null || port < 1 || port > 65535) {
            cdnPort.setError(getString(R.string.error_port));
            return null;
        }
        settings.cdnPort = port;

        final Integer tunnels = parseInt(tunnelCount.getText().toString());
        if (tunnels == null || tunnels < DefaultSettings.MIN_TUNNEL_COUNT || tunnels > DefaultSettings.MAX_TUNNEL_COUNT) {
            tunnelCount.setError(getString(R.string.error_tunnels,
                    DefaultSettings.MIN_TUNNEL_COUNT, DefaultSettings.MAX_TUNNEL_COUNT));
            return null;
        }
        settings.tunnelCount = tunnels;

        settings.handshakeHost = handshakeHost.getText().toString().trim();
        settings.handshakePath = handshakePath.getText().toString().trim();
        if (!settings.handshakePath.startsWith("/")) {
            handshakePath.setError(getString(R.string.error_path));
            return null;
        }
        settings.webSocketKey = webSocketKey.getText().toString().trim();
        settings.expectedAccept = expectedAccept.getText().toString().trim();
        return settings;
    }

    private static Integer parseInt(String text) {
        try {
            return Integer.valueOf(text.trim());
        } catch (NumberFormatException e) {
            return null;
        }
    }

    // ---------------------------------------------------------------- UI state

    private void updateUi() {
        final TunnelState state = TunnelVpnService.getState();
        final boolean editable = state == TunnelState.STOPPED || state == TunnelState.ERROR;

        cdnAddress.setEnabled(editable);
        cdnPort.setEnabled(editable);
        tunnelCount.setEnabled(editable);
        handshakeHost.setEnabled(editable);
        handshakePath.setEnabled(editable);
        webSocketKey.setEnabled(editable);
        expectedAccept.setEnabled(editable);

        switch (state) {
        case STARTING:
            statusView.setText(R.string.status_starting);
            toggleButton.setText(R.string.button_stop);
            break;
        case RUNNING:
            statusView.setText(R.string.status_running);
            toggleButton.setText(R.string.button_stop);
            break;
        case ERROR:
            final int error = TunnelVpnService.getErrorResource();
            statusView.setText(error != 0 ? getString(R.string.status_error, getString(error))
                                          : getString(R.string.status_stopped));
            toggleButton.setText(R.string.button_start);
            break;
        default:
            statusView.setText(R.string.status_stopped);
            toggleButton.setText(R.string.button_start);
            break;
        }

        batteryButton.setVisibility(Build.VERSION.SDK_INT >= 23 && !isIgnoringBatteryOptimizations()
                ? View.VISIBLE : View.GONE);
    }

    private boolean isIgnoringBatteryOptimizations() {
        if (Build.VERSION.SDK_INT < 23)
            return true;
        final PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
        return power != null && power.isIgnoringBatteryOptimizations(getPackageName());
    }
}
