// BOTS_INVULNERABLE=1: the game's own "Invulnerable" cheat (C1ProfileSettings EC1CheatID 2) reads as
// active, so unattended soaks keep playing instead of dying every minute (backlog B-039/B-044).
// Debug aid only; the profile is not modified.
//
// sub_82E1AC28 = UC1ProfileSettings::execIsCheatActive(FFrame& Stack, void* Result). All script call
// sites pass a constant (C1Character/C1VehicleBase/C1GunTurret: IsCheatActive(2)), so the cheat id
// is peeked from the bytecode before the original runs: EX_IntZero 0x25, EX_IntOne 0x26,
// EX_IntConstByte 0x2C <byte>.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mnk_input.h"

extern "C" void __imp__sub_82E1AC28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823598E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823501E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82BC86F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2EB38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2EBB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2E860(PPCContext& __restrict ctx, uint8_t* base);

namespace {
// Guest address → host pointer, as REX_RAW_ADDR in generated/default/bots_pch.h (Mac arm64 layout).
uint8_t* Raw(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
}

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t v;
  __builtin_memcpy(&v, Raw(base, address), 4);
  return __builtin_bswap32(v);
}

int PeekConstInt(uint8_t* base, uint32_t code) {
  switch (*Raw(base, code)) {
    case 0x25: return 0;
    case 0x26: return 1;
    case 0x2C: return *Raw(base, code + 1);
    default: return -1;
  }
}

bool InvulnerableRequested() {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_INVULNERABLE");
    return v && *v == '1';
  }();
  return on;
}
}  // namespace

extern "C" void sub_82E1AC28(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t stack = ctx.r4.u32, result = ctx.r5.u32;
  const int cheat = InvulnerableRequested() ? PeekConstInt(base, LoadU32(base, stack + 0xC)) : -1;
  __imp__sub_82E1AC28(ctx, base);
  // BOTS_TEST_GUEST_AV=1: deliberate guest write to page 0 (never committed) from a guest thread, to
  // check the crash report (guest regs, call chain, VM ops, minidump). Test aid only.
  static const bool test_av = [] {
    const char* v = std::getenv("BOTS_TEST_GUEST_AV");
    return v && *v == '1';
  }();
  if (test_av) {
    REXLOG_WARN("BOTS_TEST_GUEST_AV: writing guest 0x00000010 (lr {:08X})", uint32_t(ctx.lr));
    const uint32_t junk = 0xDEADBEEF;
    __builtin_memcpy(Raw(base, 0x10), &junk, 4);
  }
  if (cheat == 2) {
    static std::atomic<uint64_t> hits{0};
    uint64_t n = ++hits;
    if (n == 1 || n == 100 || n == 10000) REXLOG_INFO("BOTS_INVULNERABLE: IsCheatActive(2) forced true ({}x)", n);
    const uint32_t one = __builtin_bswap32(1u);
    __builtin_memcpy(Raw(base, result), &one, 4);
    ctx.r3.u64 = 1;
  }
}

// B-041: sub_82E8E148 (native reached from script, during level load / reload) does
//   r3 = sub_8244EE48(this->+0x208)   // find component of an AIIU-package class on that object
//   r3->vtbl[0x158/4](r3)             // no null check
// The lookup sometimes returns null (component not attached yet — timing; would crash on a 360
// too), and the guest null read aborted the game (~1 in 12 checkpoint loads, run 89). Mid-asm hook
// at 0x82E8E270 (config/default.toml): when r3 == 0 skip just that call (jump to 0x82E8E280); the
// rest of the function only uses this->+0x208 itself.
bool BotsGuardMissingAiiuComponent(PPCRegister& r3) {
  if (r3.u32 != 0) return false;
  static std::atomic<uint64_t> hits{0};
  uint64_t n = ++hits;
  if (n == 1 || n == 10 || n == 100) REXLOG_WARN("B-041 guard: null AIIU component at 0x82E8E270 skipped ({}x)", n);
  return true;
}

// B-059: sub_82FD1D20(this) does
//   obj = 0; this->+292->vtbl[248/4](..., name, &obj)   // look an object up by name
//   if (obj) for (i < obj->+96->+36) if (obj->+96->+32[i].type == 1) ...   // returns -1 if none
// In chapter 11060 (Mac sweep, seed 11060, ~243 s in; reproducible) obj->+96 is still null:
// obj sits in memory committed a few ms earlier, so it is found before it is filled in (timing; a
// null there would crash a 360 too). Mid-asm hook at 0x82FD1D94 (lwz r9,36(r10)): when r10 == 0
// jump to 0x82FD1DE0, the function's "no entries" exit, which returns -1 like an empty list.
bool BotsGuardNullEntryList(PPCRegister& r10, PPCRegister& r11) {
  if (r10.u32 != 0) return false;
  static std::atomic<uint64_t> hits{0};
  uint64_t n = ++hits;
  if (n <= 3 || n == 10 || n == 100 || n == 1000) {
    REXLOG_WARN("B-059 guard: null entry list on object {:08X} at 0x82FD1D94 skipped ({}x)", r11.u32, n);
  }
  return true;
}

// BOTS_EXEC: run UE3 console commands in game, the way the engine runs its own -EXEC= file at startup
// (sub_82358A38 @0x82358E90): GamePlayers(0) of Cast<UGameEngine>(GEngine), FExec subobject at
// +60, vtbl[0] = Exec(const TCHAR* Cmd, FOutputDevice& Ar), Ar = *GLog. The shipped story game
// allows cheats (CovertOne_Story.AllowCheats returns true), so "EnableCheats" then C1CheatManager
// commands work: God, KillBadGuys, CauseEvent <name>, Teleport, LoadChapter <id>, SloMo, ... (see
// dumps/script). Debug/testing aid (B-039).
//   BOTS_EXEC="<sec>:<cmd>;<sec>:<cmd>"  timed, seconds since the first input poll
//   BOTS_EXEC_FILE=<path>                polled every 0.5 s; each line is run, then the file is emptied
// Runs from sub_823598E0 (XInputGetState wrapper, called by the viewport's per-frame input poll on
// the game thread), i.e. at the same point a typed console command would.
namespace {
constexpr uint32_t kGEngine = 0x8380D380;  // UEngine* GEngine
constexpr uint32_t kGLog = 0x8376D514;     // FOutputDevice* GLog

struct ExecState {
  std::deque<std::pair<double, std::string>> timed;
  std::string file;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), next_poll = start;
  uint32_t buffer = 0;  // guest UTF-16BE command string
};

ExecState* Exec() {
  static ExecState* state = [] {
    const char* timed = std::getenv("BOTS_EXEC");
    const char* file = std::getenv("BOTS_EXEC_FILE");
    if (!(timed && *timed) && !(file && *file)) return static_cast<ExecState*>(nullptr);
    auto* st = new ExecState;
    if (file) st->file = file;
    for (std::string list = timed ? timed : "", item; !list.empty();) {
      size_t end = list.find(';');
      item = list.substr(0, end);
      list = end == std::string::npos ? "" : list.substr(end + 1);
      size_t colon = item.find(':');
      if (colon != std::string::npos) st->timed.emplace_back(std::atof(item.c_str()), item.substr(colon + 1));
    }
    return st;
  }();
  return state;
}

void RunConsoleCommand(PPCContext& ctx, uint8_t* base, ExecState& st, const std::string& cmd) {
  if (!st.buffer) st.buffer = rex::system::kernel_memory()->SystemHeapAlloc(0x400);
  ctx.r3.u64 = LoadU32(base, kGEngine);
  sub_823501E0(ctx, base);  // Cast<UGameEngine>
  const uint32_t engine = ctx.r3.u32;
  const uint32_t players = engine ? LoadU32(base, engine + 728) : 0;
  const uint32_t player = players && LoadU32(base, engine + 732) ? LoadU32(base, players) : 0;
  if (!player || !st.buffer) {
    REXLOG_WARN("BOTS_EXEC: no local player yet, dropped: {}", cmd);
    return;
  }
  uint8_t* out = Raw(base, st.buffer);
  size_t n = std::min<size_t>(cmd.size(), 0x1FE);
  for (size_t i = 0; i < n; ++i) { out[2 * i] = 0; out[2 * i + 1] = static_cast<uint8_t>(cmd[i]); }
  out[2 * n] = out[2 * n + 1] = 0;
  const uint32_t exec = player + 60;
  const uint32_t fn = LoadU32(base, LoadU32(base, exec));
  REXLOG_INFO("BOTS_EXEC: {}", cmd);
  ctx.r3.u64 = exec;
  ctx.r4.u64 = st.buffer;
  ctx.r5.u64 = LoadU32(base, kGLog);
  rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
}
}  // namespace

extern "C" void sub_823598E0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, state = ctx.r4.u32;  // XInputGetState(user, XINPUT_STATE*)
  __imp__sub_823598E0(ctx, base);
  // Button prompts follow the last device used: tell the keyboard/mouse driver what controller 0 reported.
  if (user == 0 && state && ctx.r3.u32 == 0) {
    if (auto* kbm = bots::MouseKeyboardDriver::instance()) {
      const uint8_t* s = Raw(base, state);  // big-endian XINPUT_STATE: packet, buttons, LT, RT, LX, LY, RX, RY
      auto i16 = [&](int at) { return int16_t(uint16_t(s[at] << 8 | s[at + 1])); };
      kbm->NoteGuestPad(uint16_t(i16(4)), s[6], s[7], i16(8), i16(10), i16(12), i16(14));
    }
  }
  ExecState* st = Exec();
  if (!st) return;
  auto now = std::chrono::steady_clock::now();
  std::deque<std::string> run;
  double t = std::chrono::duration<double>(now - st->start).count();
  while (!st->timed.empty() && st->timed.front().first <= t) {
    run.push_back(st->timed.front().second);
    st->timed.pop_front();
  }
  if (!st->file.empty() && now >= st->next_poll) {
    st->next_poll = now + std::chrono::milliseconds(500);
    std::error_code ec;
    if (std::filesystem::file_size(st->file, ec) > 0 && !ec) {
      std::ifstream in(st->file);
      for (std::string line; std::getline(in, line);)
        if (!line.empty()) run.push_back(line);
      in.close();
      std::ofstream(st->file, std::ios::trunc);
    }
  }
  if (run.empty()) return;
  const PPCRegister r3 = ctx.r3;  // keep the poll's return value
  for (auto& cmd : run) RunConsoleCommand(ctx, base, *st, cmd);
  ctx.r3 = r3;
}

// B-104: engine-level mouse look. sub_82BC86F0(FViewport* vp, float dt) = FXenonViewport::ProcessInput (per
// frame, game thread). Its client *(vp+0x34) is the UGameViewportClient: vtable +0x18 = InputAxis(vp, cid, FName key,
// float delta, float dt, bool bGamepad), vtable +0x10 = InputKey(vp, cid, FName key, int event, float amount, bool
// bGamepad). Key FNames are 8-byte globals built by sub_8253BA50 (KEY_MouseX at 0x8380DEC0).
// Real mouse motion comes from MouseKeyboardDriver (mnk_input.h), sent as controller 0's MouseX / MouseY.
//   BOTS_MOUSE_TEST=<dx>[,<dy>[,<cid>]]   also inject a constant mouse delta every frame (test only)
namespace {
// tools/key_names.py lists them all.
constexpr uint32_t kKeyMouseX = 0x8380DEC0, kKeyMouseY = 0x8380D5B8, kKeyEnter = 0x8380E394;

uint64_t LoadU64(uint8_t* base, uint32_t address) {
  return (uint64_t(LoadU32(base, address)) << 32) | LoadU32(base, address + 4);
}

void InputAxis(PPCContext& ctx, uint8_t* base, uint32_t vp, uint32_t cid, uint32_t key, float delta, float dt,
               bool gamepad) {
  const uint32_t client = LoadU32(base, vp + 0x34);
  if (!client) return;
  ctx.r3.u64 = client;
  ctx.r4.u64 = vp;
  ctx.r5.u64 = cid;
  ctx.r6.u64 = LoadU64(base, key);
  ctx.f1.f64 = delta;
  ctx.f2.f64 = dt;
  ctx.r9.u64 = gamepad;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, client) + 0x18))(ctx, base);
}

// event: 0 pressed, 1 released (EInputEvent).
void InputKey(PPCContext& ctx, uint8_t* base, uint32_t vp, uint32_t cid, uint32_t key, uint32_t event, float amount,
              bool gamepad) {
  const uint32_t client = LoadU32(base, vp + 0x34);
  if (!client) return;
  ctx.r3.u64 = client;
  ctx.r4.u64 = vp;
  ctx.r5.u64 = cid;
  ctx.r6.u64 = LoadU64(base, key);
  ctx.r7.u64 = event;
  ctx.f1.f64 = amount;
  ctx.r9.u64 = gamepad;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, client) + 0x10))(ctx, base);
}
}  // namespace

// Menu mode for the keyboard/mouse driver (mnk_input.h): C1HUD natives. execNativePauseGame / execNativeUnpauseGame
// bracket the pause menu; execTickCashAndScore runs every gameplay HUD tick.
extern "C" void sub_82E2EB38(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2EB38(ctx, base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) kbm->SetPaused(true);
}

extern "C" void sub_82E2EBB8(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2EBB8(ctx, base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) kbm->SetPaused(false);
}

extern "C" void sub_82E2E860(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2E860(ctx, base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) kbm->NoteHudTick();
}

extern "C" void sub_82BC86F0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t vp = ctx.r3.u32;
  const float dt = float(ctx.f1.f64);
  __imp__sub_82BC86F0(ctx, base);
  static const struct Test {
    float dx = 0, dy = 0;
    uint32_t cid = 0;
    bool on = false;
  } test = [] {
    Test t;
    if (const char* v = std::getenv("BOTS_MOUSE_TEST"); v && *v) {
      t.on = true;
      std::sscanf(v, "%f,%f,%u", &t.dx, &t.dy, &t.cid);
      REXLOG_INFO("BOTS_MOUSE_TEST: dx {} dy {} controller {}", t.dx, t.dy, t.cid);
    }
    return t;
  }();
  if (!vp) return;
  float dx = 0, dy = 0;
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) {
    kbm->TakeMouseDelta(dx, dy);
    static std::vector<bool> enter;
    kbm->TakeEnterEvents(enter);
    for (const bool down : enter) InputKey(ctx, base, vp, 0, kKeyEnter, down ? 0 : 1, 1.0f, false);
  }
  if (dx) InputAxis(ctx, base, vp, 0, kKeyMouseX, dx, dt, false);
  if (dy) InputAxis(ctx, base, vp, 0, kKeyMouseY, dy, dt, false);
  if (!test.on) return;
  if (test.dx) InputAxis(ctx, base, vp, test.cid, kKeyMouseX, test.dx, dt, false);
  if (test.dy) InputAxis(ctx, base, vp, test.cid, kKeyMouseY, test.dy, dt, false);
}
