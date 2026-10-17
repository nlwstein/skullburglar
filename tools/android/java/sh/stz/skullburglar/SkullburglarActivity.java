package sh.stz.skullburglar;

import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

// The game. ImportActivity (the launcher) starts it once the game files are installed. The game is libmain.so
// (SDL_main); SDL is linked into the runtime library, which registers SDL's JNI methods when it loads.
//
// Test builds (DEBUGGABLE=1 tools/android/build.sh) take the game's command line and environment as intent extras,
// since `am start` can't set either; ImportActivity passes them on:
//   adb shell am start -n <package>/sh.stz.skullburglar.ImportActivity \
//     --es args "--log_level=info" --es env "BOTS_INPUT=22:START;REX_GPU_PLUGIN=native"
public class SkullburglarActivity extends SDLActivity {
    private static final String TAG = "Skullburglar";

    private TouchPadView touchPad;

    private boolean debuggable() {
        return (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override
    protected String[] getArguments() {
        String args = debuggable() ? getIntent().getStringExtra("args") : null;
        if (args == null || args.trim().isEmpty()) {
            return new String[0];
        }
        return args.trim().split("\\s+");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Headset mode needs the activity before anything else: the game asks OpenXR for a headset when its
        // graphics provider comes up, which is on the thread super.onCreate starts (src/gfx/xr/vr.h). On a device
        // with no headset this costs one failed OpenXR call and the game runs flat.
        VrNative.setActivity(this);
        // Before the game's library loads: the game reads its debug switches with getenv.
        String env = debuggable() ? getIntent().getStringExtra("env") : null;
        if (env != null) {
            for (String pair : env.split(";")) {
                int eq = pair.indexOf('=');
                if (eq <= 0) {
                    continue;
                }
                try {
                    Os.setenv(pair.substring(0, eq), pair.substring(eq + 1), true);
                } catch (ErrnoException e) {
                    Log.w(TAG, "setenv " + pair + ": " + e);
                }
            }
        }
        // Landscape only: SDL picks the orientation itself (from this hint, else any for a resizable window) and
        // overrides the manifest's. Hints fall back to the environment.
        try {
            Os.setenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight", false);
        } catch (ErrnoException e) {
            Log.w(TAG, "setenv SDL_ORIENTATIONS: " + e);
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // On-screen controller over the game's surface; mLayout is null when SDL couldn't load the libraries.
        if (mLayout != null && !mBrokenLibraries) {
            touchPad = new TouchPadView(this);
            mLayout.addView(touchPad, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        // As on iOS: the renderer can't give up its surface and resume yet, so leaving the game quits it (the next
        // launch starts fresh, from the last checkpoint).
        Process.killProcess(Process.myPid());
    }

    @Override
    protected void onDestroy() {
        if (touchPad != null) {
            touchPad.release();
        }
        super.onDestroy();
    }
}
