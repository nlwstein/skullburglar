package sh.stz.skullburglar;

// The game library's entry points for the app's own screens (src/android/android_app.cpp). Loading it runs no game
// code beyond static setup; SkullburglarActivity loads the same libraries again, which is a no-op.
final class GameNative {
    static {
        System.loadLibrary("c++_shared");
        System.loadLibrary("rexruntime");
        System.loadLibrary("main");
    }

    private GameNative() {}

    // Blocking: extracts the disc image open as fd (kept open by the caller until this returns) into dest.
    // Returns null on success, else a message for the player.
    static native String extractIso(int fd, String dest);

    // {bytes done, bytes total} of the running extraction; zeros before it has counted the files.
    static native long[] extractProgress();

    static native void cancelExtract();

    // The on-screen controller's state: XINPUT_GAMEPAD_* button bits, triggers 0-255, sticks -32767..32767 (up is
    // positive).
    static native void setPad(boolean visible, int buttons, int leftTrigger, int rightTrigger, int lx, int ly, int rx,
                              int ry);
}
