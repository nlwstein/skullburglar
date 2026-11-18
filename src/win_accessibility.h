// Windows accessibility shortcuts (B-360): the keyboard Aim default includes Shift, and five quick Shift presses open the
// Sticky Keys prompt (holding Shift 8 seconds, Filter Keys; holding Num Lock 5 seconds, Toggle Keys), which takes focus
// away from the game. Microsoft's documented pattern for games, "Disabling Shortcut Keys in Games":
//   https://learn.microsoft.com/en-us/windows/win32/dxtecharts/disabling-shortcut-keys-in-games
// is to turn the shortcut off while the game has focus, and only for a player who hasn't turned the feature itself on,
// then put the player's settings back when the game loses focus or exits.
#pragma once

#include <cstdint>

namespace bots::access {

// The bits are the same in STICKYKEYS.dwFlags, TOGGLEKEYS.dwFlags and FILTERKEYS.dwFlags (SKF_/TKF_/FKF_ prefixes).
constexpr uint32_t kFeatureOn = 0x1;       // SKF_STICKYKEYSON / TKF_TOGGLEKEYSON / FKF_FILTERKEYSON
constexpr uint32_t kHotkeyActive = 0x4;    // *_HOTKEYACTIVE: the keyboard shortcut turns the feature on
constexpr uint32_t kConfirmHotkey = 0x8;   // *_CONFIRMHOTKEY: ask before it does

// The flags to set while the game has focus. A feature the player has turned on is left exactly as it is.
constexpr uint32_t BlockedFlags(uint32_t flags) {
  return (flags & kFeatureOn) ? flags : flags & ~(kHotkeyActive | kConfirmHotkey);
}

}  // namespace bots::access

namespace bots {

// Windows only (src/win_accessibility.cpp; a no-op elsewhere). true: save the player's Sticky / Toggle / Filter Keys
// settings the first time and turn their shortcuts off; false: put the saved settings back. The settings are also
// restored at exit and in the crash handler. Off with the windows_block_accessibility_shortcuts cvar.
#if defined(_WIN32)
void SetAccessibilityShortcutsBlocked(bool blocked);
void RestoreAccessibilityShortcuts();
#else
inline void SetAccessibilityShortcutsBlocked(bool) {}
inline void RestoreAccessibilityShortcuts() {}
#endif

}  // namespace bots
