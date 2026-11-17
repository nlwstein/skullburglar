// See win_accessibility.h.
#include "win_accessibility.h"

#include <windows.h>

#include <rex/logging.h>

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace bots {

namespace {

// The flag bits are the same in all three structs; keep them honest against the real macros.
static_assert(access::kFeatureOn == SKF_STICKYKEYSON && access::kFeatureOn == TKF_TOGGLEKEYSON &&
              access::kFeatureOn == FKF_FILTERKEYSON);
static_assert(access::kHotkeyActive == SKF_HOTKEYACTIVE && access::kHotkeyActive == TKF_HOTKEYACTIVE &&
              access::kHotkeyActive == FKF_HOTKEYACTIVE);
static_assert(access::kConfirmHotkey == SKF_CONFIRMHOTKEY && access::kConfirmHotkey == TKF_CONFIRMHOTKEY &&
              access::kConfirmHotkey == FKF_CONFIRMHOTKEY);

std::mutex g_mutex;
bool g_saved = false;          // the player's settings were read
bool g_blocked = false;        // the shortcuts are off right now (so the saved settings are not in force)
STICKYKEYS g_sticky = {sizeof(STICKYKEYS), 0};
TOGGLEKEYS g_toggle = {sizeof(TOGGLEKEYS), 0};
FILTERKEYS g_filter = {sizeof(FILTERKEYS), 0};

void Apply(bool blocked) {
  STICKYKEYS sticky = g_sticky;
  TOGGLEKEYS toggle = g_toggle;
  FILTERKEYS filter = g_filter;
  if (blocked) {
    sticky.dwFlags = access::BlockedFlags(g_sticky.dwFlags);
    toggle.dwFlags = access::BlockedFlags(g_toggle.dwFlags);
    filter.dwFlags = access::BlockedFlags(g_filter.dwFlags);
  }
  // Only what changes is written: a player with a feature on, or with the shortcut already off, sees no change.
  if (g_sticky.dwFlags != access::BlockedFlags(g_sticky.dwFlags))
    SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &sticky, 0);
  if (g_toggle.dwFlags != access::BlockedFlags(g_toggle.dwFlags))
    SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof(TOGGLEKEYS), &toggle, 0);
  if (g_filter.dwFlags != access::BlockedFlags(g_filter.dwFlags))
    SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &filter, 0);
  g_blocked = blocked;
  REXLOG_INFO("accessibility shortcuts {}: sticky {:#x}, toggle {:#x}, filter {:#x}", blocked ? "off (game has focus)" : "put back",
              blocked ? sticky.dwFlags : g_sticky.dwFlags, blocked ? toggle.dwFlags : g_toggle.dwFlags,
              blocked ? filter.dwFlags : g_filter.dwFlags);
}

}  // namespace

void SetAccessibilityShortcutsBlocked(bool blocked) {
  std::lock_guard lock(g_mutex);
  if (!g_saved) {
    if (!blocked) return;
    g_sticky = {sizeof(STICKYKEYS), 0};
    g_toggle = {sizeof(TOGGLEKEYS), 0};
    g_filter = {sizeof(FILTERKEYS), 0};
    if (!SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(STICKYKEYS), &g_sticky, 0) ||
        !SystemParametersInfoW(SPI_GETTOGGLEKEYS, sizeof(TOGGLEKEYS), &g_toggle, 0) ||
        !SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(FILTERKEYS), &g_filter, 0)) {
      return;  // can't read the player's settings, so change nothing
    }
    g_saved = true;
    std::atexit(RestoreAccessibilityShortcuts);
  }
  if (g_blocked == blocked) return;
  Apply(blocked);
}

void RestoreAccessibilityShortcuts() {
  // Not the mutex's lock_guard in a crash: try_lock, and restore regardless if another thread holds it.
  const bool locked = g_mutex.try_lock();
  if (g_saved && g_blocked) Apply(false);
  if (locked) g_mutex.unlock();
}

}  // namespace bots
