// Force-included (-include) into the SDK's GPU sources that define cvars (flags.cpp, util/draw.cpp) when they are
// compiled into our binaries. Each module gets its own copy of a cvar's storage, but the registry is shared and
// keyed by name: the game registering these first left the xenos plugin's copies unregistered, so the emulated
// path never saw config or environment values for them (depth_float24_convert_in_pixel_shader among them). Here
// the definitions keep their storage and defaults but register nothing.
#pragma once

#include <string>
#include <utility>

#include <rex/cvar.h>

namespace bots::gfx {

// Accepts FlagRegistrar's chained setters.
struct UnregisteredFlag {
  template <typename... Args>
  UnregisteredFlag&& lifecycle(Args&&...) && { return std::move(*this); }
  template <typename... Args>
  UnregisteredFlag&& range(Args&&...) && { return std::move(*this); }
};

}  // namespace bots::gfx

#undef REXCVAR_DEFINE_BOOL
#define REXCVAR_DEFINE_BOOL(name, default_val, category, desc) \
  bool& FLAGS_##name##_storage_() {                            \
    static bool storage = (default_val);                       \
    return storage;                                            \
  }                                                            \
  [[maybe_unused]] static auto _cvar_reg_##name = ::bots::gfx::UnregisteredFlag {}

#undef REXCVAR_DEFINE_STRING
#define REXCVAR_DEFINE_STRING(name, default_val, category, desc) \
  std::string& FLAGS_##name##_storage_() {                       \
    static std::string storage = (default_val);                  \
    return storage;                                              \
  }                                                              \
  [[maybe_unused]] static auto _cvar_reg_##name = ::bots::gfx::UnregisteredFlag {}
