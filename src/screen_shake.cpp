// Screen shake scale (GitHub #22).
//
// Every camera shake in the game (explosions, weapon fire, melee hits, damage, vehicles, Kismet shakes) goes through
// the player camera's C1CamMod_ScreenShake modifier: ShakeView(ViewShakeInfo, Scale) queues a shake, and each frame
// ModifyCamera calls the native EvaluateViewShake(ShakeIndex, DeltaTime, out TPOV OutPOV) once per live shake, which
// ADDS that shake's offset (Location, Rotation, FOV) to the camera's point of view. The exec thunk
// (sub_82E24AF8, UC1CamMod_ScreenShake::execEvaluateViewShake) calls the C++ virtual at vtable +292 with
//   (this = r3, ShakeIndex = r4, DeltaTime = f1, OutPOV = r6)    OutPOV = { FVector Location; FRotator Rotation; float FOV }
// so wrapping that virtual and keeping only `screen_shake` percent of the offset it added scales every shake the same
// way, whatever its curve and whoever started it, and a shake's own lifetime and rumble are untouched.
//
// The virtual's address is read from the object's vtable the first time the thunk runs and hooked then (the dispatch
// table takes the replacement, as hybrid.cpp does for the file-open thunks).
//
// BOTS_SHAKE_TRACE=1 logs every evaluation: the offset the game added and the offset kept (location in cm, rotation in
// Unreal units, 65536 per turn, FOV in degrees), for measuring the setting without screenshots.
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "shake_math.h"

REXCVAR_DEFINE_DOUBLE(screen_shake, 100.0, "bots/Display",
                      "Camera shake (explosions, gunfire, hits) in percent of the game's own: 100 is as shipped, "
                      "0 turns it off")
    .range(0.0, 100.0);

extern "C" void __imp__sub_82E24AF8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

using HostFn = void (*)(PPCContext&, uint8_t*);
constexpr uint32_t kEvaluateVtableOffset = 292;  // C1CamMod_ScreenShake::EvaluateViewShake

std::atomic<HostFn> g_evaluate{nullptr};  // the game's own virtual, set once it is hooked

uint8_t* Raw(uint8_t* base, uint32_t address) {
  return base + (uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u));
}

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Raw(base, address), 4);
  return __builtin_bswap32(v);
}
void StoreU32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, address), &v, 4);
}
float LoadF32(uint8_t* base, uint32_t address) { return std::bit_cast<float>(LoadU32(base, address)); }

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_SHAKE_TRACE");
    return v && *v == '1';
  }();
  return on;
}

// The TPOV: Location x y z (floats), Rotation pitch yaw roll (ints), FOV (float).
struct Pov {
  float loc[3];
  int32_t rot[3];
  float fov;
};
Pov ReadPov(uint8_t* base, uint32_t at) {
  Pov p;
  for (int i = 0; i < 3; ++i) {
    p.loc[i] = LoadF32(base, at + 4 * i);
    p.rot[i] = int32_t(LoadU32(base, at + 12 + 4 * i));
  }
  p.fov = LoadF32(base, at + 24);
  return p;
}

void EvaluateHook(PPCContext& __restrict ctx, uint8_t* base) {
  const HostFn original = g_evaluate.load(std::memory_order_acquire);
  const uint32_t pov_at = ctx.r6.u32;
  const float factor = bots::shake::Factor(REXCVAR_GET(screen_shake));
  const bool trace = TraceOn();
  if ((factor >= 1.0f && !trace) || !pov_at) {
    original(ctx, base);
    return;
  }
  const uint32_t idx = ctx.r4.u32;
  const double dt = ctx.f1.f64;
  const Pov before = ReadPov(base, pov_at);
  original(ctx, base);
  const uint64_t result = ctx.r3.u64;  // the game's "still shaking" flag
  const Pov after = ReadPov(base, pov_at);
  if (factor < 1.0f) {
    for (int i = 0; i < 3; ++i) {
      StoreU32(base, pov_at + 4 * i, std::bit_cast<uint32_t>(bots::shake::Scaled(before.loc[i], after.loc[i], factor)));
      StoreU32(base, pov_at + 12 + 4 * i, uint32_t(bots::shake::Scaled(before.rot[i], after.rot[i], factor)));
    }
    StoreU32(base, pov_at + 24, std::bit_cast<uint32_t>(bots::shake::Scaled(before.fov, after.fov, factor)));
  }
  if (trace) {
    const Pov kept = ReadPov(base, pov_at);
    const auto len = [](const float* a, const float* b) {
      return std::sqrt(double(b[0] - a[0]) * (b[0] - a[0]) + double(b[1] - a[1]) * (b[1] - a[1]) +
                       double(b[2] - a[2]) * (b[2] - a[2]));
    };
    const auto rlen = [](const int32_t* a, const int32_t* b) {
      double s = 0;
      for (int i = 0; i < 3; ++i) {
        const double d = double(int32_t(uint32_t(b[i]) - uint32_t(a[i])));
        s += d * d;
      }
      return std::sqrt(s);
    };
    REXLOG_INFO("shake: idx {} dt {:.4f} factor {:.2f} | game offset loc {:.3f} rot {:.1f} fov {:+.3f} | kept loc {:.3f} rot {:.1f} fov {:+.3f}",
                idx, dt, factor, len(before.loc, after.loc), rlen(before.rot, after.rot), after.fov - before.fov,
                len(before.loc, kept.loc), rlen(before.rot, kept.rot), kept.fov - before.fov);
  }
  ctx.r3.u64 = result;
}

}  // namespace

// The exec thunk. Hooks the virtual it calls the first time it runs.
extern "C" void sub_82E24AF8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<bool> tried{false};
  if (!tried.exchange(true)) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t vtable = self ? LoadU32(base, self) : 0;
    const uint32_t target = vtable ? LoadU32(base, vtable + kEvaluateVtableOffset) : 0;
    auto* memory = rex::system::kernel_memory();
    HostFn original = target ? rex::runtime::ResolveIndirectFunction(target) : nullptr;
    const bool ok = original && memory && memory->HasAnyFunctionTable() && (g_evaluate.store(original), memory->SetFunction(target, EvaluateHook));
    REXLOG_INFO("screen shake: EvaluateViewShake at {:08X} (vtable {:08X}) {}", target, vtable,
                ok ? "hooked" : "NOT hooked, setting has no effect");
  }
  __imp__sub_82E24AF8(ctx, base);
}
