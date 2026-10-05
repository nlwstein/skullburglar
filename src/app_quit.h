// Quitting the app from game code (the menus' Quit option, game_hooks.cpp). BotsApp installs the handler; it must be
// safe to call from any thread.
#pragma once

#include <functional>
#include <utility>

namespace bots {

inline std::function<void()>& QuitHandler() {
  static std::function<void()> handler;
  return handler;
}

inline void SetQuitHandler(std::function<void()> handler) { QuitHandler() = std::move(handler); }

inline void RequestQuit() {
  if (QuitHandler()) QuitHandler()();
}

}  // namespace bots
