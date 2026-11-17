package io.github.crazyfull.muxtunnel;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

/**
 * The complete log of the app, the mux core and the engine, refreshed every two seconds, with
 * Copy / Share so it can be sent without adb.
 */
public class DiagnosticsActivity extends Activity {
    private static final long REFRESH_MS = 2000;
    private static final int MAX_SHARED_CHARS = 200000;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private ScrollView scrollView;
    private TextView logView;
    private String shownText = "";

    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            update();
            handler.postDelayed(this, REFRESH_MS);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_diagnostics);
        setTitle(R.string.button_diagnostics);

        scrollView = (ScrollView) findViewById(R.id.log_scroll);
        logView = (TextView) findViewById(R.id.log_text);

        ((Button) findViewById(R.id.copy_button)).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                final ClipboardManager clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(ClipData.newPlainText("log", currentText()));
                Toast.makeText(DiagnosticsActivity.this, R.string.log_copied, Toast.LENGTH_SHORT).show();
            }
        });
        ((Button) findViewById(R.id.share_button)).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                String text = currentText();
                if (text.length() > MAX_SHARED_CHARS)
                    text = text.substring(text.length() - MAX_SHARED_CHARS);
                final Intent send = new Intent(Intent.ACTION_SEND);
                send.setType("text/plain");
                send.putExtra(Intent.EXTRA_TEXT, text);
                startActivity(Intent.createChooser(send, getString(R.string.button_share)));
            }
        });
        ((Button) findViewById(R.id.clear_button)).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                DiagnosticLog.clear();
                update();
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

    private String currentText() {
        return DiagnosticLog.fullReport(new File(getFilesDir(), TunnelVpnService.HEV_LOG_FILE));
    }

    private void update() {
        final String text = currentText();
        if (text.equals(shownText))
            return;
        shownText = text;

        // follow the newest lines only while the reader is already at the bottom
        final View content = scrollView.getChildAt(0);
        final boolean atBottom = content == null
                || content.getBottom() - (scrollView.getHeight() + scrollView.getScrollY()) < 150;
        logView.setText(text);
        if (atBottom) {
            scrollView.post(new Runnable() {
                @Override
                public void run() {
                    scrollView.fullScroll(View.FOCUS_DOWN);
                }
            });
        }
    }
}
