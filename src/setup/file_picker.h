// Native "open file" dialogs for the launcher (B-061): the disc image, and the folder for the game files (desktop
// builds). Blocking and modal; call them on the UI thread. They return an empty path when cancelled or unavailable
// (the setup window also takes a typed disc image path).
//
// A fullscreen game window can hide them: on Windows the dialog opened behind the launcher and the window looked
// frozen (its owner was GetActiveWindow(), and a fullscreen swapchain can keep the screen to itself), and zenity on
// Linux is another program's window. So the app sets a PickerHost: the window that owns the dialogs, and a hook that
// takes the window out of fullscreen while a dialog is open and puts it back afterwards (OpenPicker).
#pragma once

#include <filesystem>
#include <functional>
#include <utility>

namespace bots::setup {

struct PickerHost {
  void* owner = nullptr;                 // native window (HWND on Windows) that owns the dialogs; null: none
  std::function<void(bool open)> around;  // called with true before a dialog opens and false after it closes
};

inline PickerHost& CurrentPickerHost() {
  static PickerHost host;
  return host;
}
inline void SetPickerHost(PickerHost host) { CurrentPickerHost() = std::move(host); }

std::filesystem::path PickIsoFile();
std::filesystem::path PickFolder();

// Runs a picker with the host's hook around it.
template <typename Pick>
std::filesystem::path OpenPicker(Pick pick) {
  const PickerHost& host = CurrentPickerHost();
  if (host.around) host.around(true);
  std::filesystem::path result = pick();
  if (host.around) host.around(false);
  return result;
}

}  // namespace bots::setup
