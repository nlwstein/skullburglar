package sh.stz.skullburglar;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

// The game is libmain.so (SDL_main); SDL is linked into the runtime library, which registers SDL's JNI methods when
// it loads. Test launches pass the game's command line and environment as intent extras, since `am start` can't set
// either:
//   adb shell am start -n <package>/sh.stz.skullburglar.SkullburglarActivity \
//     --es args "--log_level=info" --es env "BOTS_INPUT=22:START;REX_GPU_PLUGIN=native"
public class SkullburglarActivity extends SDLActivity {
    private static final String TAG = "Skullburglar";

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override
    protected String[] getArguments() {
        String args = getIntent().getStringExtra("args");
        if (args == null || args.trim().isEmpty()) {
            return new String[0];
        }
        return args.trim().split("\\s+");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before the game's library loads: the game reads its debug switches with getenv.
        String env = getIntent().getStringExtra("env");
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
        super.onCreate(savedInstanceState);
    }
}
