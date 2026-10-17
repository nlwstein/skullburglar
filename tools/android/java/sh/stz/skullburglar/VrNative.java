package sh.stz.skullburglar;

import android.app.Activity;

// Headset mode's one entry point (src/gfx/xr/vr.cpp). Loading this class loads the native GPU plugin rather than
// the game, because the OpenXR session's Vulkan instance, device and swapchain belong to whoever presents the
// frame - and that is the plugin, which owns the renderer. Java's native method lookup searches the libraries the
// class loaded, so the plugin's copy of the entry point is the one that runs; it is deliberately not in GameNative,
// whose library (libmain) must keep working on devices where the plugin cannot load at all.
//
// Separate from GameNative also because the import screen has no business loading a GPU plugin.
final class VrNative {
    static {
        System.loadLibrary("c++_shared");
        // The plugin links the runtime (librexruntime.so) like the game does.
        System.loadLibrary("rexruntime");
        System.loadLibrary("rexgpu-native");
    }

    private VrNative() {}

    // The activity and its JavaVM, which the OpenXR loader has to be given before any OpenXR call. Must run before
    // the game asks for a headset - i.e. before SDLActivity.onCreate starts the game thread.
    static native void setActivity(Activity activity);
}
