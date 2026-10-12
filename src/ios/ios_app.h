// iOS and tvOS platform glue (experimental): app container paths, the app lifecycle, and on iOS the on-screen touch
// controller and the input driver that feeds it to the game as a synthetic pad for player one.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace bots::ios {

// <container>/Documents (visible in the Files app with UIFileSharingEnabled) and
// <container>/Library/Caches.
std::filesystem::path DocumentsDir();
std::filesystem::path CachesDir();
// Where the game files, saves and logs go: Documents on iOS. tvOS apps have no persistent storage of their own, so
// there it is Caches, which the system may purge when the device runs short of space.
std::filesystem::path DataDir();

// Shows the import screen and serves an upload page on the local network until the player's disc image is extracted
// into game_dir, then calls done on the main thread (game_import.mm). Call on the main thread.
void ImportGame(const std::filesystem::path& game_dir, std::function<void()> done);
// Keeps a folder of game files out of device backups (iOS; tvOS has none).
void ExcludeFromBackup(const std::filesystem::path& dir);

// What the on-screen controller is pressing. Written on the UI thread, read by the input driver.
struct TouchPadState {
  std::atomic<bool> visible{false};  // hidden while a real controller is connected
  std::atomic<uint16_t> buttons{0};  // XINPUT_GAMEPAD_* bits
  std::atomic<uint8_t> left_trigger{0};
  std::atomic<uint8_t> right_trigger{0};
  std::atomic<int16_t> thumb_lx{0};
  std::atomic<int16_t> thumb_ly{0};
  std::atomic<int16_t> thumb_rx{0};
  std::atomic<int16_t> thumb_ry{0};
};
TouchPadState& TouchPad();

// Keeps the screen awake and quits when the app goes to the background (any thread; runs on the main queue). On iOS
// it also adds the on-screen controller over the game's window, shown while no game controller is connected.
void InstallAppUI();

}  // namespace bots::ios
