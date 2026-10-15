// Android platform glue (experimental): the native side of the app's Java screens (tools/android/java). The import
// screen (ImportActivity) extracts the player's disc image with the desktop launcher's ExtractIso; the on-screen
// controller (TouchPadView) writes the same pad state as the iOS one, read by the iOS TouchInputDriver
// (ios/touch_input.h), which bots_app.h registers on Android too.
#include <jni.h>

#include <atomic>
#include <memory>
#include <string>

#include "ios/ios_app.h"
#include "setup/iso_extract.h"

namespace bots::ios {

TouchPadState& TouchPad() {
  static TouchPadState state;
  return state;
}

}  // namespace bots::ios

namespace {

// One extraction at a time: the import screen polls it while a worker thread runs ExtractIso.
std::atomic<bots::setup::ExtractProgress*> g_progress{nullptr};

std::string Utf8(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* chars = env->GetStringUTFChars(s, nullptr);
  std::string out = chars ? chars : "";
  if (chars) env->ReleaseStringUTFChars(s, chars);
  return out;
}

}  // namespace

extern "C" {

// Blocking (call it on a worker thread): extracts the disc image open as descriptor fd, which the caller keeps open
// until this returns, into dest. Returns null on success, else the message to show the player.
JNIEXPORT jstring JNICALL Java_sh_stz_skullburglar_GameNative_extractIso(JNIEnv* env, jclass, jint fd,
                                                                         jstring dest) {
  auto progress = std::make_unique<bots::setup::ExtractProgress>();
  g_progress.store(progress.get());
  // The SDK maps a /proc/self/fd/<n> path as the descriptor itself (mapped_memory_posix.cpp).
  const auto result =
      bots::setup::ExtractIso("/proc/self/fd/" + std::to_string(fd), Utf8(env, dest), *progress);
  g_progress.store(nullptr);
  // A poll that read the pointer just before the store above may still be using it; it only reads atomics, briefly.
  static std::unique_ptr<bots::setup::ExtractProgress> retired;
  retired = std::move(progress);
  return result.ok ? nullptr : env->NewStringUTF(result.error.c_str());
}

// {bytes done, bytes total}; zeros when nothing is extracting.
JNIEXPORT jlongArray JNICALL Java_sh_stz_skullburglar_GameNative_extractProgress(JNIEnv* env, jclass) {
  jlong values[2] = {0, 0};
  if (auto* p = g_progress.load()) {
    values[0] = static_cast<jlong>(p->bytes_done.load());
    values[1] = static_cast<jlong>(p->bytes_total.load());
  }
  jlongArray out = env->NewLongArray(2);
  if (out) env->SetLongArrayRegion(out, 0, 2, values);
  return out;
}

JNIEXPORT void JNICALL Java_sh_stz_skullburglar_GameNative_cancelExtract(JNIEnv*, jclass) {
  if (auto* p = g_progress.load()) p->cancel = true;
}

// The on-screen controller's state, from the UI thread. Buttons are XINPUT_GAMEPAD_* bits.
JNIEXPORT void JNICALL Java_sh_stz_skullburglar_GameNative_setPad(JNIEnv*, jclass, jboolean visible, jint buttons,
                                                                  jint left_trigger, jint right_trigger, jint lx,
                                                                  jint ly, jint rx, jint ry) {
  auto& pad = bots::ios::TouchPad();
  pad.buttons.store(static_cast<uint16_t>(buttons), std::memory_order_relaxed);
  pad.left_trigger.store(static_cast<uint8_t>(left_trigger), std::memory_order_relaxed);
  pad.right_trigger.store(static_cast<uint8_t>(right_trigger), std::memory_order_relaxed);
  pad.thumb_lx.store(static_cast<int16_t>(lx), std::memory_order_relaxed);
  pad.thumb_ly.store(static_cast<int16_t>(ly), std::memory_order_relaxed);
  pad.thumb_rx.store(static_cast<int16_t>(rx), std::memory_order_relaxed);
  pad.thumb_ry.store(static_cast<int16_t>(ry), std::memory_order_relaxed);
  pad.visible.store(visible, std::memory_order_relaxed);
}

}  // extern "C"
