package sh.stz.skullburglar;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

// The app's launcher. With the game files installed it starts the game straight away; otherwise it asks for the
// player's own disc image (the system file picker), extracts it into the folder the game reads (bots_app.h:
// <external files>/game) with the game's ExtractIso, then starts the game. A screen of its own, before the game's
// Vulkan surface exists, so the picker never takes the screen from the running renderer.
public class ImportActivity extends Activity {
    private static final String TAG = "Skullburglar";
    private static final int PICK_ISO = 1;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView status;
    private ProgressBar progress;
    private Button choose;
    private Button cancel;
    private Thread worker;

    static File dataDir(Activity activity) {
        File external = activity.getExternalFilesDir(null);
        return external != null ? external : activity.getFilesDir();
    }

    // As the game's IsGameDir (src/setup/app_paths.h).
    static boolean isGameDir(File dir) {
        return new File(dir, "default.xex").isFile() && new File(dir, "C1Game").isDirectory();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        installAssets();
        if (isGameDir(new File(dataDir(this), "game"))) {
            startGame();
            return;
        }
        buildUi();
    }

    // The APK's assets/pipelines holds the shader refs for this build (tools/android/build.sh REFS=..., the same
    // first-boot warm-up the desktop packages ship next to the executable - the game's own folder is a read-only
    // library directory on Android, so they come out of the APK instead). Copied next to the game files, where the
    // game looks for them (gfx/native.cpp). Rewritten every start, so an update replaces them; they are small.
    private void installAssets() {
        File dir = new File(dataDir(this), "pipelines");
        try {
            String[] names = getAssets().list("pipelines");
            if (names == null || names.length == 0) {
                return;
            }
            if (!dir.isDirectory() && !dir.mkdirs()) {
                return;
            }
            for (String name : names) {
                File out = new File(dir, name);
                try (InputStream in = getAssets().open("pipelines/" + name);
                     OutputStream to = new FileOutputStream(out)) {
                    byte[] buffer = new byte[1 << 16];
                    for (int n; (n = in.read(buffer)) > 0; ) {
                        to.write(buffer, 0, n);
                    }
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "shader refs: " + e);
        }
    }

    private void startGame() {
        Intent intent = new Intent(this, SkullburglarActivity.class);
        // Test builds only (DEBUGGABLE=1 tools/android/build.sh): pass on the game's command line and environment.
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            for (String key : new String[] {"args", "env"}) {
                String value = getIntent().getStringExtra(key);
                if (value != null) {
                    intent.putExtra(key, value);
                }
            }
        }
        startActivity(intent);
        finish();
    }

    private int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private TextView text(String s, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setLineSpacing(0, 1.15f);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(14);
        t.setLayoutParams(lp);
        return t;
    }

    private Button button(String label, int fill, int textColor) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        b.setTextColor(textColor);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(fill);
        bg.setCornerRadius(dp(12));
        b.setBackground(bg);
        b.setPadding(dp(24), dp(12), dp(24), dp(12));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(14);
        b.setLayoutParams(lp);
        return b;
    }

    private void buildUi() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(32), dp(28), dp(32), dp(28));
        column.setGravity(Gravity.START);

        TextView title = text("Skullburglar", 30, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);

        TextView warning = text("Experimental: the Android version is new and has hardly been tested. It needs a recent "
                + "high-end phone or handheld with Vulkan, and it may crash or draw wrongly on yours.", 15,
                Color.rgb(0xF2, 0xB8, 0x4B));
        column.addView(warning);

        column.addView(text("To play, choose your own disc image of 50 Cent: Blood on the Sand (Xbox 360, .iso) on this "
                + "device. No game files are included. The game takes about 7 GB; you can delete the disc image once "
                + "it's installed.", 16, Color.rgb(0xCC, 0xCC, 0xCC)));

        choose = button("Choose disc image", Color.rgb(219, 168, 64), Color.BLACK);
        choose.setOnClickListener(v -> pick());
        column.addView(choose);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(8));
        plp.bottomMargin = dp(10);
        progress.setLayoutParams(plp);
        column.addView(progress);

        status = text("", 15, Color.rgb(0xCC, 0xCC, 0xCC));
        column.addView(status);

        cancel = button("Cancel", Color.rgb(0x3A, 0x3A, 0x3C), Color.WHITE);
        cancel.setVisibility(View.GONE);
        cancel.setOnClickListener(v -> {
            cancel.setEnabled(false);
            status.setText("Cancelling…");
            GameNative.cancelExtract();
        });
        column.addView(cancel);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(0x12, 0x12, 0x14));
        scroll.setFillViewport(true);
        scroll.addView(column);
        setContentView(scroll);
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");  // disc images have no settled MIME type
        try {
            startActivityForResult(intent, PICK_ISO);
        } catch (android.content.ActivityNotFoundException e) {
            status.setText("This device has no file picker.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ISO || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        extract(data.getData());
    }

    private void setBusy(boolean busy) {
        choose.setVisibility(busy ? View.GONE : View.VISIBLE);
        cancel.setVisibility(busy ? View.VISIBLE : View.GONE);
        cancel.setEnabled(true);
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
        progress.setProgress(0);
        if (busy) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    private void extract(Uri uri) {
        ParcelFileDescriptor file;
        try {
            file = getContentResolver().openFileDescriptor(uri, "r");
        } catch (Exception e) {
            Log.w(TAG, "open " + uri + ": " + e);
            status.setText("That file couldn't be opened.");
            return;
        }
        if (file == null) {
            status.setText("That file couldn't be opened.");
            return;
        }
        // A cloud document arrives as a stream with no size; the game needs a file it can read anywhere in.
        if (file.getStatSize() <= 0) {
            closeQuietly(file);
            status.setText("That file isn't stored on this device. Copy the disc image to the device first (for "
                    + "example into Downloads), then choose it there.");
            return;
        }
        setBusy(true);
        status.setText("Checking the disc image…");
        final String dest = new File(dataDir(this), "game").getAbsolutePath();
        worker = new Thread(() -> {
            String error;
            try {
                error = GameNative.extractIso(file.getFd(), dest);
            } catch (Throwable t) {
                Log.e(TAG, "extract", t);
                error = "Installing failed: " + t;
            }
            closeQuietly(file);
            final String result = error;
            handler.post(() -> finished(result));
        }, "iso-extract");
        worker.start();
        handler.post(poll);
    }

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (worker == null) {
                return;
            }
            long[] p = GameNative.extractProgress();
            if (p != null && p.length == 2 && p[1] > 0) {
                progress.setProgress((int) (p[0] * 1000 / p[1]));
                if (cancel.isEnabled()) {
                    status.setText(String.format(Locale.US, "Installing the game files… %.1f of %.1f GB. Keep the app "
                            + "open until it finishes.", p[0] / 1e9, p[1] / 1e9));
                }
            }
            handler.postDelayed(this, 250);
        }
    };

    private void finished(String error) {
        worker = null;
        handler.removeCallbacks(poll);
        if (isFinishing() || isDestroyed()) {
            return;
        }
        if (error == null && isGameDir(new File(dataDir(this), "game"))) {
            startGame();
            return;
        }
        setBusy(false);
        status.setText(error != null ? error : "The game files are incomplete. Try again.");
    }

    private static void closeQuietly(ParcelFileDescriptor file) {
        try {
            file.close();
        } catch (Exception ignored) {
            // nothing to do
        }
    }

    @Override
    protected void onDestroy() {
        // Leaving the screen stops the extraction; the partial files are discarded on the next try.
        if (worker != null) {
            GameNative.cancelExtract();
        }
        handler.removeCallbacks(poll);
        super.onDestroy();
    }
}
