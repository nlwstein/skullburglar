// Which device the on-screen button prompts are drawn for (backlog B-105).
//
// The game's input side (mnk_input.cpp) writes it; the native renderer, a separate plugin library with no access to
// the game's symbols, reads it. The game owns the one instance and publishes its address in the process environment
// (BOTS_PROMPT_STATE, hex) during static initialization, before any plugin loads.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bots {

struct PromptState {
  std::atomic<uint32_t> keyboard_mouse{0};  // 1: show keyboard/mouse prompts
  std::atomic<uint32_t> generation{0};      // bumped on every change; the renderer reloads the prompt textures
};

inline constexpr const char* kPromptStateEnv = "BOTS_PROMPT_STATE";

// The game's instance, or null (another host, e.g. bots-replay). Looked up once.
inline const PromptState* FindPromptState() {
  static const PromptState* state = [] {
    char value[32] = {};
#ifdef _WIN32
    // The process environment block, not the CRT's copy: the plugin may not share the game's CRT.
    if (!GetEnvironmentVariableA(kPromptStateEnv, value, sizeof(value))) return static_cast<const PromptState*>(nullptr);
#else
    const char* v = std::getenv(kPromptStateEnv);
    if (!v) return static_cast<const PromptState*>(nullptr);
    for (size_t i = 0; v[i] && i + 1 < sizeof(value); ++i) value[i] = v[i];
#endif
    return reinterpret_cast<const PromptState*>(uintptr_t(std::strtoull(value, nullptr, 16)));
  }();
  return state;
}

}  // namespace bots
