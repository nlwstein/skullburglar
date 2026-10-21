// Aim assist on/off (GitHub #15).
//
// The game's aim assist is native code. Every frame the player's pawn script runs ProcessViewRotation, which calls
// the native DragControlsToFollowTarget(out Rotator CurrentRot, bool DoAimAssist):
//   - C1Pawn (on foot): DoAimAssist = AimAssistOn (always true; only the ToggleAimAssist cheat changes it) and
//     the player is moving a stick and not looking through a scope;
//   - C1WeaponPawn (mounted guns and vehicle turrets): always true.
// The native picks the nearest enemy (by team) within range and inside a cone around the view (Coalesced.ini
// [C1Game.C1Pawn] AimAssistRangeInCM=10000, AimAssistAngleInDeg=3, AimAssistPercent=75; [C1Game.C1WeaponPawn]
// 50000 / 1.5 / 25), with no line-of-sight test, and with DoAimAssist true it drags the view to follow that
// enemy, which slows the crosshair over it. So it also catches enemies behind walls: the original game's behaviour,
// which the port runs unchanged.
//
// The script thunks (sub_82E14680, sub_82E25BF0) call the C++ virtuals below, (this, FRotator& CurrentRot,
// UBOOL DoAimAssist). With the setting off, DoAimAssist is passed as false: the path the game itself takes
// while scoped or with the sticks centred (the target bookkeeping still runs; only the drag is skipped).
//   sub_82E95C50 = AC1Pawn::DragControlsToFollowTarget        (vtable 0x821747F0 + 1724)
//   sub_82EAAB80 = AC1WeaponPawn::DragControlsToFollowTarget  (vtable 0x8217F260 + 1404)
//
// BOTS_AIM_ASSIST_TRACE=1 logs every 5 s how often the drag was asked for, how often a target was in range and how
// often the view was moved.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(aim_assist, true, "bots/Input",
                    "Aim assist: the game slows and pulls the crosshair toward enemies near it while you move the "
                    "sticks (also through walls, as on the Xbox 360). Off turns it off on foot and on mounted guns");

extern "C" void __imp__sub_82E95C50(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EAAB80(PPCContext& __restrict ctx, uint8_t* base);

namespace {

struct Trace {
  std::atomic<uint64_t> calls{0}, asked{0}, target{0}, moved{0};
  std::atomic<int64_t> last{0};  // seconds, last log line
};

// Guest address -> host pointer (as Raw() in game_hooks.cpp).
uint8_t* Raw(uint8_t* base, uint32_t address) {
  return base + (uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u));
}

bool TraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_AIM_ASSIST_TRACE");
    return v && *v == '1';
  }();
  return on;
}

// target_field: the pawn's current aim-assist target (the native keeps it at this offset; null when none is in range).
void Drag(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), const char* who, Trace& t,
          uint32_t target_field) {
  const bool asked = ctx.r5.u32 != 0;
  const bool on = REXCVAR_GET(aim_assist);
  static std::atomic<int> state{-1};  // last setting logged
  if (asked && state.exchange(on) != int(on)) REXLOG_INFO("aim assist: {} ({})", on ? "on" : "off", who);
  if (asked && !on) ctx.r5.u64 = 0;
  if (!TraceOn()) {
    original(ctx, base);
    return;
  }
  // CurrentRot is a guest FRotator (Pitch, Yaw, Roll ints); the view was dragged if it changed.
  const uint32_t self = ctx.r3.u32, rot = ctx.r4.u32;
  uint8_t before[12], after[12];
  std::memcpy(before, Raw(base, rot), 12);
  original(ctx, base);
  std::memcpy(after, Raw(base, rot), 12);
  ++t.calls;
  if (asked) ++t.asked;
  uint32_t target;
  std::memcpy(&target, Raw(base, self + target_field), 4);
  if (asked && target) ++t.target;
  if (std::memcmp(before, after, 12) != 0) ++t.moved;
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t prev = t.last.load();
  if (now - prev >= 5 && t.last.compare_exchange_strong(prev, now)) {
    REXLOG_INFO("aim assist trace ({} {:08X}, setting {}): {} calls, drag asked {}, with a target {}, view moved {}", who,
                self, on ? "on" : "off", t.calls.exchange(0), t.asked.exchange(0), t.target.exchange(0),
                t.moved.exchange(0));
  }
}

Trace g_pawn, g_turret;

}  // namespace

extern "C" void sub_82E95C50(PPCContext& __restrict ctx, uint8_t* base) {
  Drag(ctx, base, __imp__sub_82E95C50, "on foot", g_pawn, 2772);
}

extern "C" void sub_82EAAB80(PPCContext& __restrict ctx, uint8_t* base) {
  Drag(ctx, base, __imp__sub_82EAAB80, "mounted gun", g_turret, 1924);
}
