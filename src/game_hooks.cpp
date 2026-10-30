// BOTS_INVULNERABLE=1: the game's own "Invulnerable" cheat (C1ProfileSettings EC1CheatID 2) reads as
// active, so unattended soaks keep playing instead of dying every minute (backlog B-039/B-044).
// Debug aid only; the profile is not modified.
//
// sub_82E1AC28 = UC1ProfileSettings::execIsCheatActive(FFrame& Stack, void* Result). All script call
// sites pass a constant (C1Character/C1VehicleBase/C1GunTurret: IsCheatActive(2)), so the cheat id
// is peeked from the bytecode before the original runs: EX_IntZero 0x25, EX_IntOne 0x26,
// EX_IntConstByte 0x2C <byte>.
#include "path_utf8.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <bit>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "app_quit.h"
#include "coop.h"
#include "widescreen.h"
#include "gfx/capture.h"
#include "instr.h"
#include "input_math.h"
#include "mnk_input.h"
#include "prompt_state.h"
#include "scene_feed.h"

namespace bots::hybrid {
bool Command(const std::string& cmd);  // hybrid.cpp
}

// Controller settings (launcher, Controller section). Defaults are the game as shipped.
REXCVAR_DEFINE_DOUBLE(right_stick_deadzone, 20.0, "bots/Input",
                      "Right stick dead zone in percent of the stick's travel. 20 is the game's own (per axis); "
                      "lower values use a smaller round dead zone, 0 has none")
    .range(0.0, 20.0);
REXCVAR_DEFINE_BOOL(stick_acceleration, true, "bots/Input",
                    "The game's stick acceleration: turning speeds up while the stick is held fully over, and the "
                    "camera eases in when you push further. Off removes both");
REXCVAR_DEFINE_STRING(stick_response, "original", "bots/Input",
                      "Stick response curve for turning left and right: original (squared, small movements turn "
                      "slowly) or linear");
REXCVAR_DEFINE_BOOL(vibration, true, "bots/Input", "Controller vibration (rumble)");

extern "C" void __imp__sub_82E1AC28(PPCContext& __restrict ctx, uint8_t* base);
namespace bots::slowframe {
void Tick();  // slow_frame.cpp: BOTS_SLOW_SAMPLE
}
namespace bots::gc {
void Tick();  // gc.cpp: GC timing and slow-frame attribution
}
namespace bots::levelstream {
void Tick(uint8_t* base);  // level_stream.cpp: BOTS_STREAM_LOG=2 streaming-level dump
}

extern "C" void __imp__sub_823598E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823501E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82BC86F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82445388(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82445450(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2EB38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2EBB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2E860(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_828291E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82853F50(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_825C96A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82A88140(PPCContext& __restrict ctx, uint8_t* base);

namespace {
// Guest address → host pointer, as REX_RAW_ADDR in generated/default/bots_pch.h (Mac arm64 layout).
uint8_t* Raw(uint8_t* base, uint32_t address) {
  return base + (uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u));
}

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  uint32_t v;
  __builtin_memcpy(&v, Raw(base, address), 4);
  return __builtin_bswap32(v);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  __builtin_memcpy(Raw(base, address), &value, 4);
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

// BOTS_UELOG=1: mirror the engine's own log (debugf / warnf: net driver, travel, ...) into ours.
// The shipped game opens D:\C1Game\Logs\Launch.log, which fails on the read-only game drive.
// sub_825EF750 = FOutputDeviceRedirector::Serialize(const TCHAR* Data, EName Event), vtbl[1] of
// *GLog (vtable 0x82063BAC). Event is an FName index (number in the log line).
extern "C" void __imp__sub_825EF750(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_825EF750(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_UELOG");
    return v && *v == '1';
  }();
  if (on && ctx.r4.u32) {
    std::string line;
    const uint8_t* p = Raw(base, ctx.r4.u32);
    for (size_t i = 0; i < 1024; ++i, p += 2) {
      uint16_t c = static_cast<uint16_t>(p[0] << 8 | p[1]);
      if (!c) break;
      line.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    REXLOG_INFO("UE3 [{}] {}", ctx.r5.u32, line);
  }
  __imp__sub_825EF750(ctx, base);
}

// Co-op over system link (B-057). The game's co-op is Live-only (Join COOP needs a Live sign-in:
// --user_signin_live) and no Live service exists, so every session it creates or searches for is
// turned into a LAN one; UOnlineSubsystemLive already has the system-link path (XSession flags
// HOST|USES_PEER_NETWORK, LAN beacon discovery).
// sub_82BF2970 = CreateOnlineGame(this, byte HostingPlayerNum, UOnlineGameSettings* r5), vtbl[108];
//   its flag builder sub_82BE75F8 returns 0x21 (system link) when bit 30 of Settings+140
//   (bIsLanMatch) is set.
// sub_82BF31D8 = FindOnlineGames(this, byte ControllerId, UOnlineGameSearch* r5), vtbl[111]; bit 31
//   of Search+124 (bIsLanQuery) picks FindLanGames (sub_82BF0CD8) over FindLiveGames, which
//   broadcasts the query and polls for answers for LanQueryTimeout seconds.
extern "C" void __imp__sub_82BF2970(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82BF2970(PPCContext& __restrict ctx, uint8_t* base) {
  if (const uint32_t settings = ctx.r5.u32) {
    StoreU32(base, settings + 140, LoadU32(base, settings + 140) | 0x40000000u);
    // The LAN beacon (sub_82BF5248) only answers queries while NumOpenPublicConnections (+116,
    // copied from NumPublicConnections +108 here) > 0; invite-only co-op has private slots only.
    uint32_t pub = LoadU32(base, settings + 108), priv = LoadU32(base, settings + 112);
    REXLOG_INFO("B-057: CreateOnlineGame -> system link (flags {:08X}, public {}, private {})",
                LoadU32(base, settings + 140), pub, priv);
    if (pub == 0 && priv > 0) {
      StoreU32(base, settings + 108, priv);
      StoreU32(base, settings + 112, 0);
    }
  }
  __imp__sub_82BF2970(ctx, base);
}

extern "C" void __imp__sub_82BF31D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82BF31D8(PPCContext& __restrict ctx, uint8_t* base) {
  if (const uint32_t search = ctx.r5.u32) {
    StoreU32(base, search + 124, LoadU32(base, search + 124) | 0x80000000u);
    // LanQueryTimeout (this+744, float) is 0 in this Live-only build, so the LAN tick
    // (sub_82BEB1A0) ended every search on its first frame, before any answer could arrive.
    const uint32_t timeout = ctx.r3.u32 + 744;
    if (LoadU32(base, timeout) == 0) StoreU32(base, timeout, 0x40000000u);  // 2.0f
    REXLOG_INFO("B-057: FindOnlineGames -> LAN query");
  }
  __imp__sub_82BF31D8(ctx, base);
}

namespace split_screen {
extern bool running_ours;
// The thread that runs the HUD tick (Tick below): the game thread. XInputGetState is also polled from other threads
// (the loading screen's), where nothing may run script or console commands: a join made during a load ran
// DebugCreatePlayer and ServerRequestBuddyCharacter from the loading thread, which then blocked while the game thread
// waited on a render fence it held, and the game froze on the loading screen (seen on real pads; a minidump had both
// threads' guest stacks).
std::atomic<std::thread::id> game_thread{};
bool OffGameThread() {
  const std::thread::id id = game_thread.load(std::memory_order_relaxed);
  return id != std::thread::id{} && id != std::this_thread::get_id();
}
void Poll(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state);
bool Command(const std::string& cmd);
}  // namespace split_screen
namespace debug {
bool Command(uint8_t* base, const std::string& cmd);
void LogGuestStack(PPCContext& ctx, uint8_t* base, const char* tag, int frames = 16);
}  // namespace debug

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
    if (!(timed && *timed) && !(file && *file) && !bots::instr::Enabled()) return static_cast<ExecState*>(nullptr);
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

void RunPlayerCommand(PPCContext& ctx, uint8_t* base, ExecState& st, uint32_t player, const std::string& cmd,
                      bool log = true);

void RunConsoleCommand(PPCContext& ctx, uint8_t* base, ExecState& st, const std::string& cmd) {
  bots::instr::NoteCommand(cmd);
  // "peek <hex addr> [count]": log guest words (pointers the game fills in at runtime).
  if (cmd.rfind("peek ", 0) == 0) {
    char* end = nullptr;
    uint32_t addr = static_cast<uint32_t>(std::strtoul(cmd.c_str() + 5, &end, 16)) & ~3u;
    int count = std::max(1, std::min(64, static_cast<int>(std::strtol(end, nullptr, 10))));
    for (int i = 0; i < count; ++i, addr += 4) REXLOG_INFO("BOTS_EXEC peek {:08X} = {:08X}", addr, LoadU32(base, addr));
    return;
  }
  if (split_screen::Command(cmd) || debug::Command(base, cmd) || bots::scenefeed::Command(base, cmd)) return;
  if (!st.buffer) st.buffer = rex::system::kernel_memory()->SystemHeapAlloc(0x400);
  ctx.r3.u64 = LoadU32(base, kGEngine);
  sub_823501E0(ctx, base);  // Cast<UGameEngine>
  const uint32_t engine = ctx.r3.u32;
  // "p2:<command>" runs it as the second local player (split screen).
  const bool p2 = cmd.rfind("p2:", 0) == 0;
  const uint32_t index = p2 ? 1 : 0;
  const uint32_t players = engine ? LoadU32(base, engine + 728) : 0;
  const uint32_t player = players && LoadU32(base, engine + 732) > index ? LoadU32(base, players + 4 * index) : 0;
  if (!player || !st.buffer) {
    REXLOG_WARN("BOTS_EXEC: no local player yet, dropped: {}", cmd);
    return;
  }
  RunPlayerCommand(ctx, base, st, player, p2 ? cmd.substr(3) : cmd);
}

// Exec `cmd` through a LocalPlayer's FExec (+60).
void RunPlayerCommand(PPCContext& ctx, uint8_t* base, ExecState& st, uint32_t player, const std::string& cmd, bool log) {
  uint8_t* out = Raw(base, st.buffer);
  size_t n = std::min<size_t>(cmd.size(), 0x1FE);
  for (size_t i = 0; i < n; ++i) { out[2 * i] = 0; out[2 * i + 1] = static_cast<uint8_t>(cmd[i]); }
  out[2 * n] = out[2 * n + 1] = 0;
  const uint32_t exec = player + 60;
  const uint32_t fn = LoadU32(base, LoadU32(base, exec));
  if (log) REXLOG_INFO("BOTS_EXEC: {}", cmd);
  ctx.r3.u64 = exec;
  ctx.r4.u64 = st.buffer;
  ctx.r5.u64 = LoadU32(base, kGLog);
  const bool was = std::exchange(split_screen::running_ours, true);
  rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
  split_screen::running_ours = was;
}
}  // namespace

namespace {
namespace quit_option {
void FilterPad(uint8_t* base, uint32_t state);
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
  if (state && ctx.r3.u32 == 0) {
    quit_option::FilterPad(base, state);
    // Right stick dead zone setting: below the game's own 20% we re-map the stick (input_math.h). Every user.
    const double dz = REXCVAR_GET(right_stick_deadzone) / 100.0;
    if (dz < bots::input::kGameStickDeadZone - 1e-9) {
      uint8_t* s = Raw(base, state);  // big-endian XINPUT_STATE: ... RX at +12, RY at +14
      const bots::input::Stick in{int16_t(uint16_t(s[12] << 8 | s[13])), int16_t(uint16_t(s[14] << 8 | s[15]))};
      const bots::input::Stick out = bots::input::RemapRightStick(in, dz);
      static const bool trace = [] { const char* v = std::getenv("BOTS_FEEL_LOG"); return v && *v == '1'; }();
      if (trace && user == 0 && (in.x || in.y))
        REXLOG_INFO("FEELLOG stick ({}, {}) -> ({}, {})", in.x, in.y, out.x, out.y);
      s[12] = uint8_t(uint16_t(out.x) >> 8), s[13] = uint8_t(out.x);
      s[14] = uint8_t(uint16_t(out.y) >> 8), s[15] = uint8_t(out.y);
    }
  }
  split_screen::Poll(ctx, base, user, ctx.r3.u32 == 0 ? state : 0);
  if (split_screen::OffGameThread()) return;
  if (user == 0) {
    bots::slowframe::Tick();
    bots::gc::Tick();
    bots::levelstream::Tick(base);
    bots::scenefeed::Tick(base);
    if (!bots::scenefeed::LiveAtDraw()) bots::scenefeed::Live(base);
    bots::instr::GameTick(ctx, base);
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
  if (user == 0) bots::instr::TakeCommands(run);
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

// One 8-byte read. Two LoadU32 halves got merged by clang (arm64) into a 64-bit load whose constant offset
// 0x8380E394 came out sign-extended: InputKey(kKeyEnter) read base - 2 GiB and every menu click faulted (B-107).
uint64_t LoadU64(uint8_t* base, uint32_t address) {
  uint64_t v;
  __builtin_memcpy(&v, Raw(base, address), 8);
  return __builtin_bswap64(v);
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

// event: 0 pressed, 1 released (EInputEvent). `name` is the FName by value: {index, number} as one big-endian u64.
void InputKeyName(PPCContext& ctx, uint8_t* base, uint32_t vp, uint32_t cid, uint64_t name, uint32_t event,
                  float amount, bool gamepad) {
  const uint32_t client = LoadU32(base, vp + 0x34);
  if (!client) return;
  ctx.r3.u64 = client;
  ctx.r4.u64 = vp;
  ctx.r5.u64 = cid;
  ctx.r6.u64 = name;
  ctx.r7.u64 = event;
  ctx.f1.f64 = amount;
  ctx.r9.u64 = gamepad;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, client) + 0x10))(ctx, base);
}

void InputKey(PPCContext& ctx, uint8_t* base, uint32_t vp, uint32_t cid, uint32_t key, uint32_t event, float amount,
              bool gamepad) {
  InputKeyName(ctx, base, vp, cid, LoadU64(base, key), event, amount, gamepad);
}

// BOTS_MEMDUMP=<secs>:<file>[;<secs>:<file>...] (research): at those times after the first ProcessInput, write every
// committed guest page below 0xC0000000 to <file> as records {u32 address, u32 size (little-endian), bytes}.
void MemoryDumpTick(uint8_t* base, float dt) {
  struct Dump {
    float at;
    std::string path;
  };
  static std::vector<Dump> dumps = [] {
    std::vector<Dump> out;
    const char* v = std::getenv("BOTS_MEMDUMP");
    std::string s = v ? v : "";
    size_t start = 0;
    while (start < s.size()) {
      size_t end = s.find(';', start);
      if (end == std::string::npos) end = s.size();
      const std::string item = s.substr(start, end - start);
      if (const size_t colon = item.find(':'); colon != std::string::npos)
        out.push_back({std::strtof(item.c_str(), nullptr), item.substr(colon + 1)});
      start = end + 1;
    }
    return out;
  }();
  static float clock = 0;
  if (dumps.empty()) return;
  clock += dt;
  if (clock < dumps.front().at) return;
  const Dump d = dumps.front();
  dumps.erase(dumps.begin());
  FILE* f = std::fopen(d.path.c_str(), "wb");
  if (!f) return;
  auto* memory = rex::system::kernel_memory();
  uint64_t total = 0;
  for (uint64_t address = 0x10000; address < 0xC0000000ull;) {
    auto* heap = memory->LookupHeap(uint32_t(address));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
      address += 0x10000;
      continue;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && info.protect) {
      const uint32_t header[2] = {info.base_address, info.region_size};
      std::fwrite(header, sizeof(header), 1, f);
      std::fwrite(Raw(base, info.base_address), 1, info.region_size, f);
      total += info.region_size;
    }
    address = next > address ? next : address + 0x1000;
  }
  std::fclose(f);
  REXLOG_INFO("BOTS_MEMDUMP: {:.1f} s -> {} ({} MB)", clock, d.path, total >> 20);
}

// B-105 text prompts: the one localized string that names a pad button is [ButtonBar] PressStart, "Press START
// button" (title screen). While keyboard/mouse prompts are shown it reads "Press ENTER" (kbm_confirm), and "Press
// OPTIONS" for a PlayStation pad (B-160). The config cache's value and any labels already built from it are FStrings
// {data, count, max}; each replacement is no longer than the original, so it is written into the same buffers (and
// back for an Xbox pad). The guest heap is rescanned only when the text changes; labels built later copy the config
// value, which is already right.
namespace press_start {
constexpr char16_t kPad[] = u"Press START button";
constexpr uint32_t kPadCount = std::size(kPad);  // FString count: with the null; also the smallest buffer it is in
constexpr std::u16string_view kTexts[] = {u"Press START button", u"Press ENTER", u"Press OPTIONS"};
static_assert(kTexts[1].size() < kPadCount && kTexts[2].size() < kPadCount);

// Which of kTexts the prompts call for.
int TextFor(bots::PromptDevice device) {
  return device == bots::PromptDevice::kKeyboardMouse ? 1 : bots::IsPlayStation(device) ? 2 : 0;
}

std::vector<uint8_t> Utf16Be(const char16_t* s, uint32_t count) {
  std::vector<uint8_t> out;
  for (uint32_t i = 0; i < count; ++i) out.insert(out.end(), {uint8_t(s[i] >> 8), uint8_t(s[i])});
  return out;
}

// Committed guest regions below 0xC0000000, as BOTS_MEMDUMP walks them.
template <class F>
void ForEachRegion(F&& f) {
  auto* memory = rex::system::kernel_memory();
  for (uint64_t address = 0x10000; address < 0xC0000000ull;) {
    auto* heap = memory->LookupHeap(uint32_t(address));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
      address += 0x10000;
      continue;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    if ((info.state & rex::memory::kMemoryAllocationCommit) && info.protect) f(info.base_address, info.region_size);
    address = next > address ? next : address + 0x1000;
  }
}

bool Committed(uint32_t address, uint32_t size) {
  auto* heap = rex::system::kernel_memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(address, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) && info.protect &&
         uint64_t(address) + size <= uint64_t(info.base_address) + info.region_size;
}

void Rewrite(uint8_t* base, int from_text, int to_text) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const std::u16string_view from_view = kTexts[from_text], to_view = kTexts[to_text];
  const uint32_t from_count = uint32_t(from_view.size()) + 1, to_count = uint32_t(to_view.size()) + 1;
  const auto from = Utf16Be(from_view.data(), from_count), to = Utf16Be(to_view.data(), to_count);
  const uint32_t from_count_be = __builtin_bswap32(from_count), to_count_be = __builtin_bswap32(to_count);
  // One pass over UE3's heap (0x40000000-0x5FFFFFFF in every dump so far): an FString header {data, count, max} with
  // the right count (rare) whose data, also in the heap, holds the text.
  auto in_heap = [](uint32_t a) { return a >= 0x40000000u && a < 0x60000000u; };
  uint32_t strings = 0;
  ForEachRegion([&](uint32_t address, uint32_t size) {
    if (!in_heap(address)) return;
    uint8_t* p = Raw(base, address);
    for (uint32_t off = 4; off + 8 <= size; off += 4) {
      uint32_t count;
      std::memcpy(&count, p + off, 4);
      if (count != from_count_be) continue;
      const uint32_t data = LoadU32(base, address + off - 4), max = LoadU32(base, address + off + 4);
      if (!in_heap(data) || data & 1 || max < kPadCount || max > 0x1000) continue;
      if (!Committed(data, kPadCount * 2)) continue;
      uint8_t* text = Raw(base, data);
      if (std::memcmp(text, from.data(), from.size())) continue;
      std::memcpy(text, to.data(), to.size());
      std::memcpy(p + off, &to_count_be, 4);
      ++strings;
    }
  });
  const auto ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  static constexpr const char* kNames[] = {"Press START button", "Press ENTER", "Press OPTIONS"};
  REXLOG_INFO("prompts: PressStart text -> '{}' ({} strings, {:.0f} ms)", kNames[to_text], strings, ms);
}

// BOTS_PROMPT_TEST=<secs>:k|p[;...] (test only): switch the prompts to keyboard/mouse (k) or the pad (p) at that time.
void TestSwitch(float dt) {
  struct Switch {
    float at;
    bool keyboard;
  };
  static std::vector<Switch> plan = [] {
    std::vector<Switch> out;
    const char* v = std::getenv("BOTS_PROMPT_TEST");
    for (const char* p = v ? v : ""; *p;) {
      float at;
      char device;
      if (std::sscanf(p, "%f:%c", &at, &device) != 2) break;
      out.push_back({at, device == 'k'});
      p = std::strchr(p, ';');
      if (!p) break;
      ++p;
    }
    return out;
  }();
  static float clock = 0;
  if (plan.empty()) return;
  clock += dt;
  if (clock < plan.front().at) return;
  REXLOG_INFO("BOTS_PROMPT_TEST: {} at {:.1f} s", plan.front().keyboard ? "keyboard/mouse" : "pad", clock);
  bots::MouseKeyboardDriver::SetPromptMode(plan.front().keyboard);
  plan.erase(plan.begin());
}

void Tick(uint8_t* base, float dt) {
  TestSwitch(dt);
  static const bots::PromptState* state = bots::FindPromptState();
  static uint32_t seen = 0;
  static int shown = 0;  // kTexts index the guest strings hold
  if (!state) return;
  const uint32_t generation = state->generation.load(std::memory_order_relaxed);
  if (generation == seen) return;
  seen = generation;
  const int text = TextFor(state->Device());
  if (text == shown) return;
  Rewrite(base, shown, text);
  shown = text;
}
}  // namespace press_start

// B-107 menu pointer: focus follows the free cursor over menu widgets. The UE3 UI's own mouse picking needs viewport
// hit proxies, which the console build never renders, so we hit-test the widgets ourselves and call SetFocus on the
// hovered one. A click on a menu option is the engine's Enter (MouseKeyboardDriver menu mode). Toggles and sliders
// (Options pages) take the engine's Left/Right keys instead, which their WidgetInputAliases accept: a click on a
// toggle's left arrow is Left, anywhere else on the row Right; pressing on a slider's bar drags its value.
// Object layout (UE3 as built for this game, read from the live UClass/UProperty data):
//   GEngine+0x2E4 GameViewport, +0x5C UIController, +0x78 SceneClient, +0x100 ActiveScenes (TArray)
//   UObject: +0x00 vtable, +0x2C Name, +0x34 Class; UClass +0x3C SuperField; UProperty +0x64 Offset
//   UIScreenObject: +0x5C bit 31 bHidden, +0x60 Children, +0x94 FocusControls[player] = {Focused, LastFocused}
//   UIObject +0x204 RenderBounds[4] (left top right bottom, pixels of the 1280x720 UI)
//   UISlider +0x444 SliderValue (UIRangeData {Current, Min, Max, Nudge}; the volume sliders are 0..1 by 0.05)
namespace menu_pointer {
constexpr uint32_t kGEngine = 0x8380D380, kMaxWidgets = 32;

enum class Kind { kNone, kOption, kToggle, kSlider };

struct Widget {
  uint32_t object;
  Kind kind;
  float left, top, right, bottom;
};

float LoadF32(uint8_t* base, uint32_t address) {
  const uint32_t v = LoadU32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

bool Readable(uint32_t address) { return address >= 0x40000000u && address < 0xA0000000u && !(address & 3); }

// FName::Names (TArray<FNameEntry*>); an entry is {u32 index, FNameEntry* hash next, char name[]}.
constexpr uint32_t kNames = 0x837F98A0;

const char* NameOf(uint8_t* base, uint32_t index) {
  const uint32_t names = LoadU32(base, kNames);
  if (!Readable(names) || index >= LoadU32(base, kNames + 4)) return "?";
  const uint32_t entry = LoadU32(base, names + 4 * index);
  return Readable(entry) ? reinterpret_cast<const char*>(Raw(base, entry + 8)) : "?";
}

// Name indices this file needs, found once by scanning FName::Names (they all exist once the script packages load).
struct Names {
  uint32_t option = 0, toggle = 0, slider = 0, image = 0, left = 0, right = 0, main_menu = 0, quit = 0, title = 0, label = 0, online_message = 0;
  uint32_t easel = 0, music_player = 0, conveyer = 0, loadout = 0, driving_vehicle = 0;
};

const Names* GetNames(uint8_t* base) {
  static Names n;
  static bool ready = false;
  static int wait = 0;
  if (ready) return &n;
  if (wait-- > 0) return nullptr;
  wait = 60;
  const uint32_t names = LoadU32(base, kNames), count = LoadU32(base, kNames + 4);
  if (!Readable(names) || count > 0x100000) return nullptr;
  const std::pair<const char*, uint32_t*> wanted[] = {{"C1UI_MenuOption", &n.option}, {"C1UI_ToggleableOption", &n.toggle},
                                                      {"C1UI_Slider", &n.slider},     {"HudWidget_Image", &n.image},
                                                      {"Left", &n.left},              {"Right", &n.right},
                                                      {"C1UIScene_MainMenu", &n.main_menu}, {"Quit", &n.quit},
                                                      {"C1TitleScreen", &n.title}, {"HudWidget_Label", &n.label},
                                                      {"OnlinePlayMessage", &n.online_message}, {"SfUI_Easel", &n.easel},
                                                      {"C1UI_MusicPlayer", &n.music_player},
                                                      {"C1UI_DataConveyer", &n.conveyer},
                                                      {"C1UI_WeaponLoadout", &n.loadout},
                                                      {"C1DrivingVehicleBase", &n.driving_vehicle}};
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = LoadU32(base, names + 4 * i);
    if (!Readable(entry)) continue;
    const char* name = reinterpret_cast<const char*>(Raw(base, entry + 8));
    for (const auto& [text, index] : wanted)
      if (!*index && !std::strcmp(name, text)) *index = i;
  }
  for (const auto& [text, index] : wanted)
    if (!*index) return nullptr;
  ready = true;
  REXLOG_INFO("menu pointer: names option {:#x} toggle {:#x} slider {:#x} Left {:#x} Right {:#x}", n.option, n.toggle,
              n.slider, n.left, n.right);
  return &n;
}

// Whether the object's class is (or derives from) the class with this name index.
bool IsA(uint8_t* base, uint32_t object, uint32_t name_index) {
  uint32_t cls = LoadU32(base, object + 0x34);
  for (int depth = 0; depth < 8 && Readable(cls); ++depth) {
    if (LoadU32(base, cls + 0x2C) == name_index && !LoadU32(base, cls + 0x30)) return true;
    cls = LoadU32(base, cls + 0x3C);
  }
  return false;
}

Kind KindOf(uint8_t* base, const Names& n, uint32_t object) {
  if (IsA(base, object, n.option)) return Kind::kOption;
  if (IsA(base, object, n.toggle)) return Kind::kToggle;
  if (IsA(base, object, n.slider)) return Kind::kSlider;
  return Kind::kNone;
}

Widget Read(uint8_t* base, uint32_t object, Kind kind) {
  return {object, kind, LoadF32(base, object + 0x204), LoadF32(base, object + 0x208), LoadF32(base, object + 0x20C),
          LoadF32(base, object + 0x210)};
}

void Collect(uint8_t* base, const Names& n, uint32_t widget, int depth, std::vector<Widget>& out) {
  if (depth > 5 || !Readable(widget) || out.size() >= kMaxWidgets) return;
  if (LoadU32(base, widget + 0x5C) & 0x80000000u) return;  // bHidden
  if (depth > 0) {
    if (const Kind kind = KindOf(base, n, widget); kind != Kind::kNone) {
      out.push_back(Read(base, widget, kind));
      return;
    }
  }
  const uint32_t children = LoadU32(base, widget + 0x60);
  const int32_t count = int32_t(LoadU32(base, widget + 0x64));
  if (!Readable(children) || count <= 0 || count > 256) return;
  for (int32_t i = 0; i < count; ++i) Collect(base, n, LoadU32(base, children + 4 * i), depth + 1, out);
}

// A toggle's left arrow: of its HudWidget_Image children, the two rightmost are the arrows (another sits by the
// label). Returns false if they aren't there.
bool LeftArrow(uint8_t* base, const Names& n, uint32_t toggle, float& left, float& right) {
  float lefts[2] = {-1, -1}, rights[2] = {-1, -1};  // [0] rightmost, [1] next
  const uint32_t children = LoadU32(base, toggle + 0x60);
  const int32_t count = int32_t(LoadU32(base, toggle + 0x64));
  for (int32_t i = 0; Readable(children) && i < count && i < 16; ++i) {
    const uint32_t c = LoadU32(base, children + 4 * i);
    if (!Readable(c) || !IsA(base, c, n.image)) continue;
    const float l = LoadF32(base, c + 0x204), r = LoadF32(base, c + 0x20C);
    if (l > lefts[0]) {
      lefts[1] = lefts[0], rights[1] = rights[0];
      lefts[0] = l, rights[0] = r;
    } else if (l > lefts[1]) {
      lefts[1] = l, rights[1] = r;
    }
  }
  if (lefts[1] < 0) return false;
  left = lefts[1], right = rights[1];
  return true;
}

// Where the slider's marker sits at Min and at Max, in UI pixels: the right part of its bounds (the caption is on the
// left). Measured on the Audio page (bounds ending at 1138): value 0.5 draws the marker at x 1040, 1.0 at 1121.
// The drawn bar starts ~35 px left of the Min position; presses there count as Min.
void SliderBar(const Widget& w, float& left, float& right) {
  right = w.right - 17;
  left = w.right - 179;
}

void Focus(PPCContext& ctx, uint8_t* base, uint32_t widget) {
  // UUIScreenObject::SetFocus(Sender = None, PlayerIndex) is vtable +0x16C (execSetFocus 0x82508B78 calls it with
  // GetBestPlayerIndex(), sub_828291E0). It refuses disabled widgets itself.
  ctx.r3.u64 = widget;
  sub_828291E0(ctx, base);
  const uint32_t player = ctx.r3.u32;
  ctx.r3.u64 = widget;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = player;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, widget) + 0x16C))(ctx, base);
}

void Tap(PPCContext& ctx, uint8_t* base, uint32_t vp, uint32_t name_index) {
  const uint64_t name = uint64_t(name_index) << 32;
  InputKeyName(ctx, base, vp, 0, name, 0, 1.0f, false);
  InputKeyName(ctx, base, vp, 0, name, 1, 1.0f, false);
}

std::string ClassChain(uint8_t* base, uint32_t object) {
  std::string out;
  uint32_t cls = LoadU32(base, object + 0x34);
  for (int depth = 0; depth < 8 && Readable(cls); ++depth) {
    if (!out.empty()) out += " < ";
    out += NameOf(base, LoadU32(base, cls + 0x2C));
    cls = LoadU32(base, cls + 0x3C);
  }
  return out;
}

// BOTS_POINTER_TEST=<secs>:<x>,<y>[,d|u][;...] (test only): put the pointer at UI pixel x,y (1280 x GuestHeight) from that
// time on; `d` / `u` also press / release the left button there (a menu click, i.e. an Enter edge).
bool TestPointer(float dt, float& px, float& py, uint32_t& moves, std::vector<bool>& clicks) {
  struct Move {
    float at, x, y;
    char button;
  };
  static std::vector<Move> plan = [] {
    std::vector<Move> out;
    const char* v = std::getenv("BOTS_POINTER_TEST");
    for (const char* p = v ? v : ""; *p;) {
      Move m{};
      if (std::sscanf(p, "%f:%f,%f,%c", &m.at, &m.x, &m.y, &m.button) < 3) break;
      out.push_back(m);
      p = std::strchr(p, ';');
      if (!p) break;
      ++p;
    }
    if (!out.empty()) REXLOG_INFO("BOTS_POINTER_TEST: {} moves", out.size());
    return out;
  }();
  static float clock = 0;
  static size_t current = 0;
  if (plan.empty()) return false;
  clock += dt;
  while (current < plan.size() && clock >= plan[current].at) {
    const Move& m = plan[current];
    REXLOG_INFO("BOTS_POINTER_TEST: {:.0f},{:.0f}{} at {:.1f} s", m.x, m.y,
                m.button == 'd' ? " down" : m.button == 'u' ? " up" : "", clock);
    if (m.button == 'd' || m.button == 'u') clicks.push_back(m.button == 'd');
    ++current;
  }
  if (!current) return false;
  px = plan[current - 1].x / 1280.0f;
  py = plan[current - 1].y / float(bots::GuestHeight());
  moves = uint32_t(current);
  return true;
}

// A visible item carousel (C1UI_DataConveyer: Unlockables, the shop's weapons / taunts / counter-kills) or weapon
// loadout (the pay phone's) in this widget tree. Neither takes focus: the scene hands them its input.
uint32_t PadWidget(uint8_t* base, const Names& n, uint32_t widget, int depth) {
  if (depth > 3 || !Readable(widget) || (LoadU32(base, widget + 0x5C) & 0x80000000u)) return 0;  // bHidden
  if (depth > 0 && (IsA(base, widget, n.conveyer) || IsA(base, widget, n.loadout))) return widget;
  const uint32_t children = LoadU32(base, widget + 0x60);
  const int32_t count = int32_t(LoadU32(base, widget + 0x64));
  if (!Readable(children) || count <= 0 || count > 256) return 0;
  for (int32_t i = 0; i < count; ++i)
    if (const uint32_t found = PadWidget(base, n, LoadU32(base, children + 4 * i), depth + 1)) return found;
  return 0;
}

// The topmost active UI scene whose focused control is one of ours (its widgets are the ones to hover), or 0.
uint32_t MenuScene(uint8_t* base, const Names& n, uint32_t& focused) {
  const uint32_t engine = LoadU32(base, kGEngine);
  if (!Readable(engine)) return 0;
  const uint32_t viewport = LoadU32(base, engine + 0x2E4);
  if (!Readable(viewport)) return 0;
  const uint32_t ui = LoadU32(base, viewport + 0x5C);
  if (!Readable(ui)) return 0;
  const uint32_t client = LoadU32(base, ui + 0x78);
  if (!Readable(client)) return 0;
  const uint32_t scenes = LoadU32(base, client + 0x100);
  const int32_t scene_count = int32_t(LoadU32(base, client + 0x104));
  if (!Readable(scenes) || scene_count <= 0 || scene_count > 64) return 0;
  for (int32_t i = scene_count - 1; i >= 0; --i) {
    const uint32_t scene = LoadU32(base, scenes + 4 * i);
    if (!Readable(scene)) continue;
    const uint32_t focus = LoadU32(base, scene + 0x94);
    const uint32_t f = Readable(focus) && LoadU32(base, scene + 0x98) ? LoadU32(base, focus) : 0;
    if (!Readable(f) || KindOf(base, n, f) == Kind::kNone) {
      if (const uint32_t pad = PadWidget(base, n, scene, 0)) {
        focused = pad;
        return scene;
      }
    }
    if (!Readable(f)) continue;
    if (KindOf(base, n, f) == Kind::kNone) {
      // A screen that draws itself (the music player): it covers the menu below, which must not follow the pointer.
      if (IsA(base, f, n.easel)) {
        focused = f;
        return scene;
      }
      static uint32_t logged = 0;
      if (f != logged) {
        logged = f;
        REXLOG_DEBUG("menu pointer: focus {:08X} is {}", f, ClassChain(base, f));
      }
      continue;
    }
    focused = f;
    return scene;
  }
  return 0;
}

// The music player (front end and pause menu) is one C1UI_MusicPlayer easel that draws its two lists itself and takes
// pad buttons only: Tracks (left) and Playlist (right), D-pad to move, A adds / removes, X plays, Y shuffles, B goes
// back. With the pointer, hovering a row selects it, clicking a row is A and clicking a button-bar entry presses that
// button. A row in the other list is reached with the game's own D-pad Left/Right first (that also relabels the button
// bar: A is Add Track on Tracks, Remove Track on Playlist), then selected once the list has focus. Layout (live UProperty data; 1280x720 UI pixels):
//   +0x430 ViewStates[2], 0x2C each: +0x18 count, +0x1C first row shown, +0x20 rows shown, +0x24 selected
//   +0x8A0 ViewInFocus (0 Tracks, 1 Playlist), +0x8E4 MusicPlayerTopLeft, +0x90C TrackList X/Y/W/H,
//   +0x91C PlayList X/Y/W/H, +0x944 TrackListLineSpacing (both lists)
namespace music_player {
constexpr uint32_t kViewStates = 0x430, kViewSize = 0x2C, kCount = 0x18, kFirst = 0x1C, kShown = 0x20, kSelected = 0x24;
constexpr uint32_t kViewInFocus = 0x8A0, kTopLeft = 0x8E4, kTrackList = 0x90C, kPlayList = 0x91C, kLineSpacing = 0x944;
constexpr uint16_t kLeft = 0x0004, kRight = 0x0008, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;  // XINPUT_*

void StoreU32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, address), &v, 4);
}

// The list row under (x, y): its view (0 Tracks, 1 Playlist) and item index.
bool RowAt(uint8_t* base, uint32_t w, float x, float y, uint32_t& view, uint32_t& index) {
  const float ox = LoadF32(base, w + kTopLeft), oy = LoadF32(base, w + kTopLeft + 4);
  const float h = LoadF32(base, w + kLineSpacing);
  if (!(h > 1 && h < 100)) return false;
  for (uint32_t v = 0; v < 2; ++v) {
    const uint32_t list = w + (v ? kPlayList : kTrackList), state = w + kViewStates + v * kViewSize;
    const float left = ox + LoadF32(base, list), top = oy + LoadF32(base, list + 4);
    const float right = left + LoadF32(base, list + 8);
    const int32_t count = int32_t(LoadU32(base, state + kCount)), first = int32_t(LoadU32(base, state + kFirst));
    const int32_t shown = int32_t(LoadU32(base, state + kShown));
    if (x < left - 12 || x > right || y < top || y >= top + h * float(shown)) continue;
    const int32_t i = first + int32_t((y - top) / h);
    if (count <= 0 || count > 1000 || i < 0 || i >= count) return false;
    view = v, index = uint32_t(i);
    return true;
  }
  return false;
}

// The button bar along the bottom: Y, X, A, B from left to right (measured on the front end's music player).
uint16_t ButtonAt(float x, float y) {
  if (y < 620 || y > 672) return 0;
  if (x >= 195 && x < 400) return kY;
  if (x >= 470 && x < 690) return kX;
  if (x >= 730 && x < 960) return kA;
  if (x >= 1000 && x < 1110) return kB;
  return 0;
}

// Ultrawide fit (B-164, see easel_fit): the scale the music player `w` is drawn at, 1 at 720 lines.
float Fit(uint8_t* base, uint32_t w) {
  const uint32_t height = bots::GuestHeight();
  if (height >= 720) return 1;
  const menu_pointer::Names* n = menu_pointer::GetNames(base);
  return n && menu_pointer::IsA(base, w, n->music_player) ? float(height) / 720.0f : 1;
}

// A pointer position in UI pixels -> the music player's own 720-line layout, undoing its fit.
void FitPointer(uint8_t* base, uint32_t w, float& x, float& y) {
  const float fit = Fit(base, w);
  x = 640.0f + (x - 640.0f) / fit;
  y /= fit;
}

// Per frame while the music player has focus. `enter` as for menu_pointer::Tick: click edges on a row or a button
// become pad taps; the rest pass through.
void Tick(uint8_t* base, bots::MouseKeyboardDriver& kbm, uint32_t w, float x, float y, float dt, bool moved,
          std::vector<bool>& enter) {
  static bool click_taken = false;  // the current click became a pad tap: its release is ours too
  // A selection waiting for its list to get focus (after a Left/Right tap), and whether a click (A) follows it.
  static struct {
    bool on = false, click = false;
    uint32_t view = 0, index = 0;
    float age = 0;
  } pending;
  const auto focus = [&] { return LoadU32(base, w + kViewInFocus); };
  const auto apply = [&] {
    if (!pending.on) return;
    pending.age += dt;
    if (focus() == pending.view) {
      StoreU32(base, w + kViewStates + pending.view * kViewSize + kSelected, pending.index);
      if (pending.click) kbm.TapButtons(kA);
      pending.on = false;
    } else if (pending.age > 0.5f) {
      pending.on = false;  // the game didn't switch lists
    }
  };
  uint32_t view = 0, index = 0;
  const bool row = RowAt(base, w, x, y, view, index);
  const auto select = [&](bool click) {
    if (pending.on && pending.view == view) {  // already switching there: just update the target
      pending.index = index, pending.click = pending.click || click;
      return;
    }
    if (focus() != view) kbm.TapButtons(view ? kRight : kLeft);
    pending = {true, click, view, index, 0};
  };
  apply();
  if (moved && row) select(false);
  std::vector<bool> rest;
  for (const bool down : enter) {
    if (!down) {
      if (click_taken) click_taken = false;
      else rest.push_back(down);
      continue;
    }
    const uint16_t button = row ? kA : ButtonAt(x, y);
    if (!button) {
      rest.push_back(down);
      continue;
    }
    if (row) select(true);
    else kbm.TapButtons(button);
    click_taken = true;
  }
  enter.swap(rest);
  apply();  // same list: select (and click) now
}
}  // namespace music_player

// Clicks as pad taps, spaced out so a carousel finishes each step before the next.
class TapQueue {
 public:
  void Reset() { mask_ = 0, count_ = 0; }
  void Add(uint16_t mask, int count) { mask_ = mask, count_ = std::min(count, 8), wait_ = 0; }
  void Tick(bots::MouseKeyboardDriver& kbm, float dt, float spacing) {
    if (!count_ || (wait_ -= dt) > 0) return;
    kbm.TapButtons(mask_);
    --count_;
    wait_ = spacing;
  }

 private:
  uint16_t mask_ = 0;
  int count_ = 0;
  float wait_ = 0;
};

// Takes the click edges `handle` turns into taps (it returns true) and passes the rest through.
template <typename F>
void Clicks(std::vector<bool>& enter, bool& taken, F handle) {
  std::vector<bool> rest;
  for (const bool down : enter) {
    if (!down) {
      if (taken) taken = false;
      else rest.push_back(down);
    } else if (handle()) {
      taken = true;
    } else {
      rest.push_back(down);
    }
  }
  enter.swap(rest);
}

// An item carousel (C1UI_DataConveyer): Unlockables' Video / Stills / Cheats, the shop's weapons, taunts and
// counter-kills. It cycles on Left/Right and Enter / A buys or plays the middle item. Clicking an item beside the middle
// one cycles to it, clicking the « » arrows cycles one step, clicking the middle item is its Enter. Layout (live
// UProperty data): +0x468 ConveyerBelt (the item widgets, a ring that moves round as it cycles; 7 on Unlockables, the
// outer two half hidden under the arrows), +0x474 CurrentSelection (data index), +0x478 CentralWidgetIndex (the middle
// widget; Right moves it +1).
namespace conveyer {
constexpr uint32_t kBelt = 0x468, kCentral = 0x478;

void Tick(uint8_t* base, bots::MouseKeyboardDriver& kbm, uint32_t w, float x, float y, float dt, std::vector<bool>& enter) {
  static TapQueue taps;
  static uint32_t seen = 0;
  static bool taken = false;
  if (w != seen) {
    seen = w;
    taps.Reset();
    REXLOG_INFO("menu pointer: conveyer {:08X} ({}), {} items, middle {}", w, ClassChain(base, w),
                LoadU32(base, w + kBelt + 4), LoadU32(base, w + kCentral));
  }
  taps.Tick(kbm, dt, 0.3f);
  Clicks(enter, taken, [&] {
    const uint32_t items = LoadU32(base, w + kBelt);
    const int32_t count = int32_t(LoadU32(base, w + kBelt + 4)), central = int32_t(LoadU32(base, w + kCentral));
    if (!Readable(items) || count <= 0 || count > 32 || central < 0 || central >= count) return false;
    // The widgets themselves go round (CentralWidgetIndex follows the middle one), so an item's step is its distance
    // from the middle, around the ring. The fully shown ones are within `span`; with more than 3, the outer two sit
    // under the arrows.
    const int32_t span = count > 3 ? (count - 1) / 2 - 1 : (count - 1) / 2;
    float top = 1e9f, bottom = -1e9f, left = 1e9f, right = -1e9f;
    int32_t hit = 0;
    bool any = false;
    float best = 1e9f;
    for (int32_t d = -span; d <= span; ++d) {
      const uint32_t item = LoadU32(base, items + 4 * ((central + d + count) % count));
      if (!Readable(item)) return false;
      const float l = LoadF32(base, item + 0x204), t = LoadF32(base, item + 0x208), r = LoadF32(base, item + 0x20C),
                  b = LoadF32(base, item + 0x210);
      top = std::min(top, t), bottom = std::max(bottom, b), left = std::min(left, l), right = std::max(right, r);
      // Neighbours overlap: the item whose centre is nearest wins.
      if (x >= l && x <= r && y >= t && y <= b && std::abs(x - (l + r) / 2) < best)
        best = std::abs(x - (l + r) / 2), hit = d, any = true;
    }
    if (any && !hit) return false;  // the middle item: the engine's Enter (Select)
    int32_t steps = hit;
    if (!any) {
      if (y < top || y > bottom) return false;
      if (x < left && x > left - 120) steps = -1;
      else if (x > right && x < right + 120) steps = 1;
      else return false;
    }
    taps.Add(steps < 0 ? music_player::kLeft : music_player::kRight, std::abs(steps));
    return true;
  });
}
}  // namespace conveyer

// The pay phone's weapon loadout (C1UI_WeaponLoadout): four slots picked with the D-pad (Pistols Up, CloseCombat Left,
// Assault Right, Special Down), X cycles the slot's weapon, A confirms, Y opens the shop, B goes back. Hovering a slot
// focuses it, clicking the focused slot cycles its weapon. Layout (UProperty data): +0x418 SlotInFocus, +0x460
// WeaponSlots, 0x48-byte WeaponLoadOutSlotInfo {+0 WeaponGroup name, +0x10 Image, +0x14 Label}.
namespace loadout {
constexpr uint32_t kSlotInFocus = 0x418, kSlots = 0x460, kSlotSize = 0x48;

uint16_t Direction(const char* group) {
  if (!std::strcmp(group, "Pistols")) return 0x0001;  // XINPUT_GAMEPAD_DPAD_UP
  if (!std::strcmp(group, "Special")) return 0x0002;  // DOWN
  if (!std::strcmp(group, "CloseCombat")) return music_player::kLeft;
  if (!std::strcmp(group, "Assault")) return music_player::kRight;
  return 0;
}

bool Inside(uint8_t* base, uint32_t widget, float x, float y) {
  if (!Readable(widget)) return false;
  return x >= LoadF32(base, widget + 0x204) - 8 && x <= LoadF32(base, widget + 0x20C) + 8 &&
         y >= LoadF32(base, widget + 0x208) - 8 && y <= LoadF32(base, widget + 0x210) + 8;
}

// The slot under (x, y) (its image or label), or -1.
int32_t SlotAt(uint8_t* base, uint32_t w, float x, float y) {
  const uint32_t slots = LoadU32(base, w + kSlots);
  const int32_t count = int32_t(LoadU32(base, w + kSlots + 4));
  if (!Readable(slots) || count <= 0 || count > 8) return -1;
  for (int32_t i = 0; i < count; ++i) {
    const uint32_t s = slots + kSlotSize * i;
    if (Inside(base, LoadU32(base, s + 0x10), x, y) || Inside(base, LoadU32(base, s + 0x14), x, y)) return i;
  }
  return -1;
}

void Tick(uint8_t* base, bots::MouseKeyboardDriver& kbm, uint32_t w, float x, float y, bool moved,
          std::vector<bool>& enter) {
  static uint32_t seen = 0;
  static int32_t hovered = -1;
  static bool taken = false;
  const uint32_t slots = LoadU32(base, w + kSlots);
  if (w != seen) {
    seen = w;
    hovered = -1;
    const int32_t count = int32_t(LoadU32(base, w + kSlots + 4));
    REXLOG_INFO("menu pointer: weapon loadout {:08X}, {} slots, slot {} in focus", w, count,
                LoadU32(base, w + kSlotInFocus));
    for (int32_t i = 0; Readable(slots) && i < count && i < 8; ++i) {
      const uint32_t s = slots + kSlotSize * i, image = LoadU32(base, s + 0x10);
      if (Readable(image))
        REXLOG_INFO("menu pointer:   slot {} {} image [{:.0f} {:.0f} {:.0f} {:.0f}]", i, NameOf(base, LoadU32(base, s)),
                    LoadF32(base, image + 0x204), LoadF32(base, image + 0x208), LoadF32(base, image + 0x20C),
                    LoadF32(base, image + 0x210));
    }
  }
  const int32_t slot = SlotAt(base, w, x, y);
  const auto direction = [&] { return Direction(NameOf(base, LoadU32(base, slots + kSlotSize * slot))); };
  if (moved && slot != hovered) {
    hovered = slot;
    if (slot >= 0 && int32_t(LoadU32(base, w + kSlotInFocus)) != slot) kbm.TapButtons(direction());
  }
  Clicks(enter, taken, [&] {
    if (slot < 0) return false;  // elsewhere: the engine's Enter (A, confirm)
    const uint16_t d = direction();
    kbm.TapButtons(int32_t(LoadU32(base, w + kSlotInFocus)) == slot || !d ? music_player::kX : d);
    return true;
  });
}
}  // namespace loadout

bool extra_menu = false;            // another screen wants the free pointer (quit_option's press-start Quit)
bool pointer_valid = false;         // this frame's pointer, UI pixels (for quit_option)
float pointer_x = 0, pointer_y = 0;

void Tick(PPCContext& ctx, uint8_t* base, bots::MouseKeyboardDriver& kbm, uint32_t vp, float dt, std::vector<bool>& enter) {
  static uint32_t seen_moves = 0, dragging = 0;
  static bool click_taken = false;  // the current click went to a toggle/slider: its release is ours too
  static uint32_t tick = 0, hover_tick = 0;  // frames, and the last one that hit-tested a menu's widgets
  ++tick;
  const Names* n = GetNames(base);
  uint32_t focused = 0;
  const uint32_t scene = n ? MenuScene(base, *n, focused) : 0;
  kbm.SetMenuScene(scene != 0 || extra_menu);
  float px, py;
  uint32_t moves;
  pointer_valid = false;
  if (!TestPointer(dt, px, py, moves, enter) && !kbm.Pointer(px, py, moves)) {
    dragging = 0;
    return;
  }
  pointer_valid = true;
  pointer_x = px * 1280.0f, pointer_y = py * float(bots::GuestHeight());
  if (!n) return;
  if (focused && IsA(base, focused, n->music_player)) {
    dragging = 0;
    float mx = pointer_x, my = pointer_y;
    music_player::FitPointer(base, focused, mx, my);
    music_player::Tick(base, kbm, focused, mx, my, dt, moves != seen_moves, enter);
    seen_moves = moves;
    return;
  }
  if (focused && (IsA(base, focused, n->conveyer) || IsA(base, focused, n->loadout))) {
    dragging = 0;
    if (IsA(base, focused, n->conveyer)) conveyer::Tick(base, kbm, focused, pointer_x, pointer_y, dt, enter);
    else loadout::Tick(base, kbm, focused, pointer_x, pointer_y, moves != seen_moves, enter);
    seen_moves = moves;
    return;
  }
  std::vector<Widget> widgets;
  if (scene) Collect(base, *n, scene, 0, widgets);
  if (widgets.empty()) {
    dragging = 0;
    return;
  }
  // Window -> UI pixels (the UI is laid out at the back buffer size, 1280x720 or ultrawide 1280xN, and scaled to the
  // window).
  const float x = px * 1280.0f, y = py * float(bots::GuestHeight());
  const Widget* hovered = nullptr;
  for (const auto& w : widgets) {
    // Rows: the label's box is narrow, so accept the whole row height left of the right edge plus a margin.
    if (y >= w.top - 4 && y <= w.bottom + 4 && x >= w.left - 200 && x <= w.right + 200) hovered = &w;
  }
  // SetFocus can refuse (seen once while the main menu was opening), so keep asking for up to a second after a move
  // while the pointer stays on that widget. Not longer: pad/keyboard navigation must be able to move focus away.
  static uint32_t want = 0;
  static float want_age = 0;
  // Only a deliberate move within this menu moves the focus. A menu opens with the pointer wherever it was left (the
  // window's first motion event, a launcher click, a Steam Deck trackpad or touchscreen), and its first frame took
  // that stale position as a move: the main menu opened on whatever row the pointer happened to rest on (Music Player
  // under a centred pointer; in a Steam Deck report it opened on Quit, and the next A, meant for Play Game, quit the
  // game: GitHub #14). Small motion (a resting thumb on a trackpad, a warp when the pointer is freed) doesn't count.
  static uint32_t menu_scene = 0;
  static float anchor_x = 0, anchor_y = 0;
  static bool armed = false;
  constexpr float kDeliberate = 12.0f;  // UI pixels from where the pointer was when the menu opened
  if (scene != menu_scene || hover_tick + 1 != tick) {  // a new menu, or one that was closed and has reopened
    menu_scene = scene;
    anchor_x = x, anchor_y = y;
    armed = false;
    seen_moves = moves;
    want = 0;
  }
  if (moves != seen_moves && !armed) {
    if (std::abs(x - anchor_x) + std::abs(y - anchor_y) < kDeliberate) seen_moves = moves;
    else armed = true;
  }
  hover_tick = tick;
  if (moves != seen_moves) {
    seen_moves = moves;
    want = hovered ? hovered->object : 0;
    want_age = 0;
  } else if (want) {
    want_age += dt;
  }
  if (want && (focused == want || want_age > 1.0f || !hovered || hovered->object != want)) want = 0;
  if (want && !dragging) Focus(ctx, base, want);

  // Clicks on toggles and sliders.
  std::vector<bool> rest;
  for (const bool down : enter) {
    if (!down) {
      if (click_taken) {
        click_taken = false;
        dragging = 0;
      } else {
        rest.push_back(down);
      }
      continue;
    }
    if (!hovered || hovered->kind == Kind::kOption) {
      rest.push_back(down);
      continue;
    }
    click_taken = true;
    if (hovered->object != focused) Focus(ctx, base, hovered->object);
    if (hovered->kind == Kind::kToggle) {
      float l = 0, r = 0;
      const bool left = LeftArrow(base, *n, hovered->object, l, r) && x >= l - 8 && x <= r + 8;
      Tap(ctx, base, vp, left ? n->left : n->right);
    } else {
      float l, r;
      SliderBar(*hovered, l, r);
      if (x >= l - 40) dragging = hovered->object;
    }
  }
  enter.swap(rest);

  if (dragging) {
    // Step toward the pointer, one nudge per frame, through the slider's own Left/Right handling.
    const Widget w = Read(base, dragging, Kind::kSlider);
    float l, r;
    SliderBar(w, l, r);
    const float t = std::clamp((x - l) / (r - l), 0.0f, 1.0f);
    const float current = LoadF32(base, dragging + 0x444), lo = LoadF32(base, dragging + 0x448),
                hi = LoadF32(base, dragging + 0x44C), nudge = LoadF32(base, dragging + 0x450);
    const float target = lo + t * (hi - lo);
    if (nudge > 0 && hi > lo) {
      if (target > current + nudge / 2) Tap(ctx, base, vp, n->right);
      else if (target < current - nudge / 2) Tap(ctx, base, vp, n->left);
    }
  }
}
}  // namespace menu_pointer

// Quit on the main menu (PC convention; on the console you quit from the dashboard). The menu is authored in the
// front-end package, so the option is added to the live scene: a copy of its last option (UUIScreenObject::CreateWidget
// with that option as the archetype, so it looks and animates the same) one row lower, linked into the menu's up/down
// wrap-around, labelled with the game's own "Quit Game" (QuitText). Choosing it (A / START, Enter, a click) quits the
// app; those presses are kept from the game, since the copy also carries its archetype's actions.
// Layout (live UProperty data, see menu_pointer): UIScreenObject +0x3C Position (UIScreenValue_Bounds: Value[4], the
// left and top then extents whose meaning follows ScaleType[4] after them), UIObject +0x1A4 NavigationTargets {Targets[4], ForcedTargets[4]} (left, up,
// right, down), C1UIScene_Menu +0x2EC MenuOptions (TArray), C1UI_MenuOption +0x450 MyJitter (its label, a
// HudWidget_JitterLabel: vtable +0x2C8 is SetText(const FString&), what execSetText calls).
namespace quit_option {
using menu_pointer::LoadF32;
using menu_pointer::Readable;

constexpr uint32_t kPosition = 0x3C, kNavigation = 0x1A4, kMenuOptions = 0x2EC, kMyJitter = 0x450, kFlags = 0x4E8;
constexpr uint32_t kAutoLocalise = 0x80000000;  // C1UI_MenuOption: localise the label from the widget tag
constexpr uint32_t kUp = 1, kDown = 3;
constexpr uint16_t kStart = 0x0010, kA = 0x1000, kB = 0x2000;  // XINPUT_GAMEPAD_*

std::atomic<uint32_t> option{0};    // the Quit widget on the live main menu, or 0
std::atomic<bool> focused{false};  // it has focus (updated per frame)

void StoreU32(uint8_t* base, uint32_t address, uint32_t v) {
  v = __builtin_bswap32(v);
  __builtin_memcpy(Raw(base, address), &v, 4);
}

void StoreF32(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  StoreU32(base, address, v);
}

bool IsChild(uint8_t* base, uint32_t parent, uint32_t widget) {
  const uint32_t children = LoadU32(base, parent + 0x60);
  const int32_t count = int32_t(LoadU32(base, parent + 0x64));
  for (int32_t i = 0; widget && Readable(children) && i < count && i < 256; ++i)
    if (LoadU32(base, children + 4 * i) == widget) return true;
  return false;
}

void SetLabel(PPCContext& ctx, uint8_t* base, uint32_t label, const char* text) {
  static uint32_t buffer = 0;  // FString {data, count, max}, then its UTF-16BE text; SetText copies it
  if (!buffer) buffer = rex::system::kernel_memory()->SystemHeapAlloc(0x80);
  const uint32_t count = uint32_t(std::strlen(text)) + 1;
  if (!buffer || count > 0x30 || !Readable(label)) return;
  uint8_t* data = Raw(base, buffer + 0x10);
  for (uint32_t i = 0; i < count; ++i) data[2 * i] = 0, data[2 * i + 1] = uint8_t(text[i]);
  StoreU32(base, buffer, buffer + 0x10);
  StoreU32(base, buffer + 4, count);
  StoreU32(base, buffer + 8, count);
  ctx.r3.u64 = label;
  ctx.r4.u64 = buffer;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, label) + 0x2C8))(ctx, base);
}

// The game's own "Quit Game" in its language (B-128): Action_Quit from the language's coalesced loc file, read off the
// player's disc. The game takes its language from user_language (XLanguage: 3 German, 4 French; the disc has only
// those and English, which everything else gets). Strings there are 8-bit (Latin-1) and NUL-terminated, which is what
// SetLabel takes. "Quit" if it isn't found.
const std::string& QuitText() {
  static const std::string text = [] {
    const uint32_t language = rex::cvar::Query<uint32_t>("user_language");
    const char* ext = language == 3 ? "DEU" : language == 4 ? "FRA" : "INT";
    const std::filesystem::path file = rex::system::kernel_state()->emulator()->game_data_root() / "C1Game" /
                                       "Localization" / (std::string("Coalesced.") + ext);
    std::ifstream in(file, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::string_view kKey = "Action_Quit=";
    std::string value;
    if (const size_t at = data.find(kKey); at != std::string::npos) {
      for (size_t i = at + kKey.size(); i < data.size() && uint8_t(data[i]) >= 0x20 && value.size() < 40; ++i)
        value.push_back(data[i]);
    }
    REXLOG_INFO("main menu: Quit label from {}: {}", bots::PathUtf8(file.filename()), value.empty() ? "(not found)" : "found");
    return value.empty() ? std::string("Quit") : value;
  }();
  return text;
}

uint32_t Add(PPCContext& ctx, uint8_t* base, const menu_pointer::Names& n, uint32_t scene) {
  const uint32_t options = LoadU32(base, scene + kMenuOptions);
  const int32_t count = int32_t(LoadU32(base, scene + kMenuOptions + 4));
  if (!Readable(options) || count < 2 || count > 16) return 0;
  const uint32_t first = LoadU32(base, options), prev = LoadU32(base, options + 4 * (count - 2)),
                 last = LoadU32(base, options + 4 * (count - 1));
  if (!Readable(first) || !Readable(prev) || !Readable(last)) return 0;
  // CreateWidget(Owner = the scene, the last option's class, Archetype = the last option, Name = Quit). With name None
  // it is named after the class, and inserting it renamed the archetype option instead.
  ctx.r3.u64 = scene;
  ctx.r4.u64 = scene;
  ctx.r5.u64 = LoadU32(base, last + 0x34);
  ctx.r6.u64 = last;
  ctx.r7.u64 = uint64_t(n.quit) << 32;
  sub_82853F50(ctx, base);
  const uint32_t widget = ctx.r3.u32;
  if (!Readable(widget)) return 0;
  StoreU32(base, widget + 0x174, n.quit);  // WidgetTag
  StoreU32(base, widget + 0x178, 0);
  StoreU32(base, widget + kFlags, LoadU32(base, widget + kFlags) & ~kAutoLocalise);
  const float top = LoadF32(base, last + kPosition + 4);
  StoreF32(base, widget + kPosition + 4, top + (top - LoadF32(base, prev + kPosition + 4)));
  for (const uint32_t set : {kNavigation, kNavigation + 0x10}) {  // targets, forced targets
    StoreU32(base, widget + set + 4 * kUp, last);
    StoreU32(base, widget + set + 4 * kDown, first);
    StoreU32(base, last + set + 4 * kDown, widget);
    StoreU32(base, first + set + 4 * kUp, widget);
  }
  // InsertChild(widget, INDEX_NONE, bRenameOnDuplicateName = false): vtable +0x130 (execInsertChild).
  ctx.r3.u64 = scene;
  ctx.r4.u64 = widget;
  ctx.r5.s64 = -1;
  ctx.r6.u64 = 0;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, scene) + 0x130))(ctx, base);
  SetLabel(ctx, base, LoadU32(base, widget + kMyJitter), QuitText().c_str());
  REXLOG_INFO("main menu: added Quit ({:08X}, below {:08X})", widget, last);
  return widget;
}

// Whether the Quit option has focus now.
bool Focused(uint8_t* base) {
  const menu_pointer::Names* n = menu_pointer::GetNames(base);
  uint32_t focus = 0;
  return n && option && menu_pointer::MenuScene(base, *n, focus) && focus == option;
}

// The press-start screen (C1TitleScreen) has no menu, so it gets a "Quit" label in the bottom-right corner: a copy of
// the screen's own ESRB-notice label (OnlinePlayMessage, a HudWidget_Label, whose SetText is also vtable +0x2C8). It
// reads the game's "Quit Game" (QuitText) with keyboard and mouse, where clicking it quits (the pointer is free there),
// and "Press B to quit" with a pad (in German and French, the game's words and the button). B quits on either (on the
// keyboard, the keys bound to B). Off while the attract movie plays over the screen (C1TitleScreen +0x260 bInMovie),
// where B stops the movie as before.
namespace title {
constexpr uint32_t kInMovie = 0x80000000;
constexpr float kBounds[4] = {0.78f, 0.9f, 0.98f, 0.97f};  // left top right bottom, fraction of the viewport

std::atomic<bool> active{false};  // the screen is up with its Quit label
uint32_t label = 0;

uint32_t FindScene(uint8_t* base, uint32_t cls) {
  const uint32_t engine = LoadU32(base, menu_pointer::kGEngine);
  if (!Readable(engine)) return 0;
  const uint32_t viewport = LoadU32(base, engine + 0x2E4);
  if (!Readable(viewport)) return 0;
  const uint32_t ui = LoadU32(base, viewport + 0x5C);
  if (!Readable(ui)) return 0;
  const uint32_t client = LoadU32(base, ui + 0x78);
  if (!Readable(client)) return 0;
  const uint32_t scenes = LoadU32(base, client + 0x100);
  const int32_t count = int32_t(LoadU32(base, client + 0x104));
  for (int32_t i = 0; Readable(scenes) && i < count && i < 64; ++i) {
    const uint32_t scene = LoadU32(base, scenes + 4 * i);
    if (Readable(scene) && menu_pointer::IsA(base, scene, cls)) return scene;
  }
  return 0;
}

uint32_t Add(PPCContext& ctx, uint8_t* base, const menu_pointer::Names& n, uint32_t screen) {
  uint32_t archetype = 0;
  const uint32_t children = LoadU32(base, screen + 0x60);
  const int32_t count = int32_t(LoadU32(base, screen + 0x64));
  for (int32_t i = 0; Readable(children) && i < count && i < 64 && !archetype; ++i) {
    const uint32_t c = LoadU32(base, children + 4 * i);
    if (Readable(c) && LoadU32(base, c + 0x2C) == n.online_message && menu_pointer::IsA(base, c, n.label)) archetype = c;
  }
  if (!archetype) return 0;
  ctx.r3.u64 = screen;
  ctx.r4.u64 = screen;
  ctx.r5.u64 = LoadU32(base, archetype + 0x34);
  ctx.r6.u64 = archetype;
  ctx.r7.u64 = uint64_t(n.quit) << 32;
  sub_82853F50(ctx, base);
  const uint32_t widget = ctx.r3.u32;
  if (!Readable(widget)) return 0;
  StoreU32(base, widget + 0x174, n.quit);  // WidgetTag
  StoreU32(base, widget + 0x178, 0);
  for (int i = 0; i < 4; ++i) StoreF32(base, widget + kPosition + 4 * i, kBounds[i]);  // keeps the scale types
  ctx.r3.u64 = screen;
  ctx.r4.u64 = widget;
  ctx.r5.s64 = -1;
  ctx.r6.u64 = 0;
  rex::runtime::ResolveIndirectFunction(LoadU32(base, LoadU32(base, screen) + 0x130))(ctx, base);
  REXLOG_INFO("press start: added Quit ({:08X})", widget);
  return widget;
}

void Tick(PPCContext& ctx, uint8_t* base, const menu_pointer::Names& n, bool menu) {
  static uint32_t done = 0;  // the screen it was added to
  static int shown = -1;     // the text it shows: a PromptDevice
  static const bots::PromptState* prompts = bots::FindPromptState();
  uint32_t screen = menu ? 0 : FindScene(base, n.title);
  if (screen && LoadU32(base, screen + 0x260) & kInMovie) screen = 0;
  if (screen && (screen != done || !IsChild(base, screen, label))) {
    label = Add(ctx, base, n, screen);
    done = label ? screen : 0;
    shown = -1;
  }
  active = screen && label;
  menu_pointer::extra_menu = active;
  if (!active) return;
  const bots::PromptDevice device = prompts ? prompts->Device() : bots::PromptDevice::kXbox;
  if (int(device) != shown) {
    shown = int(device);
    // English keeps its sentence; the other languages get the game's own words and the button.
    const std::string& quit = quit_option::QuitText();
    const uint32_t language = rex::cvar::Query<uint32_t>("user_language");
    const bool english = language != 3 && language != 4;
    const char* button = bots::IsPlayStation(device) ? "CIRCLE" : "B";
    const std::string text = device == bots::PromptDevice::kKeyboardMouse ? quit
                             : english ? std::string("Press ") + button + " to quit"
                                       : quit + " (" + button + ")";
    SetLabel(ctx, base, label, text.c_str());
  }
}

// Whether the pointer is over the label (its RenderBounds, UI pixels), with some slack.
bool Hovered(uint8_t* base) {
  if (!active || !menu_pointer::pointer_valid) return false;
  const float x = menu_pointer::pointer_x, y = menu_pointer::pointer_y;
  return x >= LoadF32(base, label + 0x204) - 8 && x <= LoadF32(base, label + 0x20C) + 8 &&
         y >= LoadF32(base, label + 0x208) - 8 && y <= LoadF32(base, label + 0x210) + 8;
}
}  // namespace title

// Per frame (game thread): add the option to a main menu that lacks it, and track whether it has focus; the same for
// the press-start screen's label.
void Tick(PPCContext& ctx, uint8_t* base) {
  static uint32_t done = 0;  // the scene it was added to
  const menu_pointer::Names* n = menu_pointer::GetNames(base);
  uint32_t focus = 0;
  const uint32_t scene = n ? menu_pointer::MenuScene(base, *n, focus) : 0;
  if (n) title::Tick(ctx, base, *n, scene != 0);
  if (!scene || !menu_pointer::IsA(base, scene, n->main_menu)) {
    focused = false;
    return;
  }
  if (scene != done || !IsChild(base, scene, option)) {
    option = Add(ctx, base, *n, scene);
    done = option ? scene : 0;
  }
  focused = option && focus == option;
}

// Enter / click edges from the keyboard/mouse driver (after menu_pointer, which may just have moved the focus): while
// the main menu's Quit has focus, or on a click on the press-start screen's label, a press quits and none of them
// reach the game.
void TakeEnter(uint8_t* base, std::vector<bool>& enter) {
  if (enter.empty() || !(Focused(base) || title::Hovered(base))) return;
  if (std::find(enter.begin(), enter.end(), true) != enter.end()) {
    REXLOG_INFO("{}: Quit chosen", title::active ? "press start" : "main menu");
    bots::RequestQuit();
  }
  enter.clear();
}

// An XInputGetState result (big-endian buttons at +4): A / START while the main menu's Quit has focus, or B on the
// press-start screen, quit; those buttons are hidden from the game meanwhile.
void FilterPad(uint8_t* base, uint32_t state) {
  // A press that began before Quit was there doesn't count: track every button, not only the ones that quit on this
  // screen. (Tracking just the mask let the START that opened the main menu from the press-start screen, where only B
  // was watched, count as a fresh press if the menu opened with Quit in focus.)
  static std::atomic<uint16_t> held{0xFFFF};
  uint8_t* s = Raw(base, state);
  const uint16_t buttons = uint16_t(s[4] << 8 | s[5]);
  const uint16_t was = held.exchange(buttons);
  const uint16_t mask = focused ? (kA | kStart) : title::active ? kB : 0;
  if (!mask) return;
  if (buttons & mask & ~was) {
    REXLOG_INFO("{}: Quit chosen (controller)", focused ? "main menu" : "press start");
    bots::RequestQuit();
  }
  const uint16_t rest = buttons & ~mask;
  s[4] = uint8_t(rest >> 8), s[5] = uint8_t(rest);
}
}  // namespace quit_option

}  // namespace

// Menu mode for the keyboard/mouse driver (mnk_input.h): C1HUD natives. execNativePauseGame / execNativeUnpauseGame
// bracket the pause menu; execTickCashAndScore runs every gameplay HUD tick.
extern "C" void sub_82E2EB38(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2EB38(ctx, base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) kbm->SetPaused(true);
  bots::instr::SetPaused(true);
}

extern "C" void sub_82E2EBB8(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2EBB8(ctx, base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) kbm->SetPaused(false);
  bots::instr::SetPaused(false);
}

// Whether the local player is driving: GEngine +0x2D8 GamePlayers[0] (LocalPlayer) +0x40 Actor (C1PlayerController)
// +0x208 Controller.Pawn is a C1DrivingVehicleBase (Rolling Start's C1DrivingVehicle_PimpTruck, the Humvee). Mounted
// guns are SfTurret actors, not vehicles, so they keep the mouse.
static bool Driving(uint8_t* base) {
  using menu_pointer::Readable;
  const auto* n = menu_pointer::GetNames(base);
  const uint32_t engine = LoadU32(base, kGEngine);
  if (!n || !Readable(engine) || !LoadU32(base, engine + 0x2D8 + 4)) return false;
  const uint32_t players = LoadU32(base, engine + 0x2D8);
  const uint32_t player = Readable(players) ? LoadU32(base, players) : 0;
  const uint32_t controller = Readable(player) ? LoadU32(base, player + 0x40) : 0;
  const uint32_t pawn = Readable(controller) ? LoadU32(base, controller + 0x208) : 0;
  static uint32_t seen_controller = 0, seen_pawn = 0;
  if (controller != seen_controller || pawn != seen_pawn) {
    seen_controller = controller, seen_pawn = pawn;
    auto cls = [base](uint32_t o) {
      return Readable(o) && Readable(LoadU32(base, o + 0x34))
                 ? menu_pointer::NameOf(base, LoadU32(base, LoadU32(base, o + 0x34) + 0x2C)) : "-";
    };
    REXLOG_INFO("player: controller {:08X} {} pawn {:08X} {}", controller, cls(controller), pawn, cls(pawn));
  }
  return Readable(pawn) && menu_pointer::IsA(base, pawn, n->driving_vehicle);
}

// Split screen (B-134). The shipped GameViewportClient.SetSplitscreenConfiguration(SplitType) starts with
// `SplitscreenType = 0;` (eSST_NONE whatever it is passed), so every LocalPlayer keeps the full screen; the
// SplitscreenInfo layouts are intact. Its bytecode is patched in place to `SplitscreenType = SplitType;` and the
// engine's own layout then applies (LayoutPlayers re-applies SplitscreenType; `SetSplit <n>` picks 1 2P horizontal,
// 2 2P vertical). UObject +0x28 Outer, +0x2C Name, +0x34 Class; UField +0x40 Next; UStruct +0x50 Children,
// +0x58 Script (TArray<BYTE>). LocalPlayer +0x60 ControllerId, +0x68 Origin, +0x70 Size.
namespace split_screen {
using menu_pointer::Readable;

uint32_t FindField(uint8_t* base, uint32_t strct, const char* name) {
  for (uint32_t f = LoadU32(base, strct + 0x50), n = 0; Readable(f) && n < 4096; f = LoadU32(base, f + 0x40), ++n)
    if (!std::strcmp(menu_pointer::NameOf(base, LoadU32(base, f + 0x2C)), name)) return f;
  return 0;
}

// Once: rewrite the first 15 bytes. Before: 0F 01 <SplitscreenType> 24 00 (let member = byte 0) and
// 0F 00 <Idx> 25 (let Idx = 0; locals start zeroed, so it can go). After: 0F 01 <SplitscreenType> 00 <SplitType>
// then 5 x 0B (EX_Nothing). The loop's jump target (0x0F) is unchanged.
bool Patch(uint8_t* base) {
  static int state = 0;  // 0 not yet, 1 done, -1 gave up
  if (state) return state > 0;
  const uint32_t engine = LoadU32(base, kGEngine);
  const uint32_t viewport = Readable(engine) ? LoadU32(base, engine + 0x2E4) : 0;
  if (!Readable(viewport)) return false;
  uint32_t fn = 0;
  for (uint32_t cls = LoadU32(base, viewport + 0x34), depth = 0; Readable(cls) && !fn && depth < 8;
       cls = LoadU32(base, cls + 0x3C), ++depth)
    fn = FindField(base, cls, "SetSplitscreenConfiguration");
  const uint32_t param = Readable(fn) ? LoadU32(base, fn + 0x50) : 0;
  const uint32_t code = Readable(fn) ? LoadU32(base, fn + 0x58) : 0;
  state = -1;
  if (!Readable(param) || !Readable(code) || LoadU32(base, fn + 0x5C) < 15) {
    REXLOG_WARN("split screen: SetSplitscreenConfiguration not found");
    return false;
  }
  uint8_t* p = Raw(base, code);
  const uint32_t member = LoadU32(base, code + 2);
  if (!(p[0] == 0x0F && p[1] == 0x01 && p[6] == 0x24 && p[7] == 0x00 && p[8] == 0x0F && p[9] == 0x00 &&
        p[14] == 0x25) || !Readable(member) ||
      std::strcmp(menu_pointer::NameOf(base, LoadU32(base, member + 0x2C)), "SplitscreenType")) {
    REXLOG_WARN("split screen: unexpected SetSplitscreenConfiguration bytecode, not patched");
    return false;
  }
  p[6] = 0x00;  // EX_LocalVariable
  StoreU32(base, code + 7, param);
  std::memset(p + 11, 0x0B, 4);
  state = 1;
  REXLOG_INFO("split screen: SetSplitscreenConfiguration patched ({:08X})", fn);
  return true;
}

// The function `name` of the object's class (or a superclass), 0 if none.
uint32_t FindFunction(uint8_t* base, uint32_t object, const char* name) {
  uint32_t fn = 0;
  for (uint32_t cls = Readable(object) ? LoadU32(base, object + 0x34) : 0, depth = 0; Readable(cls) && !fn && depth < 12;
       cls = LoadU32(base, cls + 0x3C), ++depth)
    fn = FindField(base, cls, name);
  return fn;
}

// The offset of the object's property `name` (UProperty +0x64 Offset), 0xFFFFFFFF if its class has none.
uint32_t Prop(uint8_t* base, uint32_t object, const char* name) {
  const uint32_t prop = FindFunction(base, object, name);
  return prop ? LoadU32(base, prop + 0x64) : 0xFFFFFFFF;
}

// A joining network player's controller asks for the buddy's body from its Spectating state, but only
// `if (NetMode == NM_Client)`; a second local player on the host never does. Flag
// C1PlayerController.ServerRequestBuddyCharacter exec (UFunction +0x94 FunctionFlags, FUNC_Exec 0x200) so it can be run
// as that player.
void ExposeBuddyRequest(uint8_t* base, uint32_t controller) {
  static bool done = false;
  if (done) return;
  const uint32_t fn = FindFunction(base, controller, "ServerRequestBuddyCharacter");
  if (!fn) return;
  StoreU32(base, fn + 0x94, LoadU32(base, fn + 0x94) | 0x200);
  done = true;
  REXLOG_INFO("split screen: ServerRequestBuddyCharacter is exec ({:08X})", fn);
}

uint32_t Player(uint8_t* base, uint32_t index) {
  const uint32_t engine = LoadU32(base, kGEngine);
  if (!Readable(engine) || LoadU32(base, engine + 0x2DC) <= index) return 0;
  const uint32_t players = LoadU32(base, engine + 0x2D8);
  return Readable(players) ? LoadU32(base, players + 4 * index) : 0;
}

uint32_t PlayerCount(uint8_t* base) {
  const uint32_t engine = LoadU32(base, kGEngine);
  return Readable(engine) ? LoadU32(base, engine + 0x2DC) : 0;
}

// Controller.Pawn of a LocalPlayer's actor (+0x40), 0 if none. The property's offset is UProperty +0x64.
uint32_t PawnOf(uint8_t* base, uint32_t player) {
  static uint32_t offset = 0;
  const uint32_t controller = Readable(player) ? LoadU32(base, player + 0x40) : 0;
  if (!Readable(controller)) return 0;
  if (!offset)
    if (const uint32_t prop = FindFunction(base, controller, "Pawn")) offset = LoadU32(base, prop + 0x64);
  const uint32_t pawn = offset ? LoadU32(base, controller + offset) : 0;
  return Readable(pawn) ? pawn : 0;
}

// An object property's value (0 if the object has no such property or the value is not an object).
uint32_t ObjectProp(uint8_t* base, uint32_t object, const char* name) {
  const uint32_t at = Readable(object) ? Prop(base, object, name) : 0xFFFFFFFF;
  const uint32_t value = at != 0xFFFFFFFF ? LoadU32(base, object + at) : 0;
  return Readable(value) ? value : 0;
}

const char* ClassName(uint8_t* base, uint32_t object) {
  const uint32_t cls = Readable(object) ? LoadU32(base, object + 0x34) : 0;
  return Readable(cls) ? menu_pointer::NameOf(base, LoadU32(base, cls + 0x2C)) : "None";
}

// Player 2's loadout. A network player's join URL carries their profile's loadout (BuildJoinURL: ?gun0..3), which
// CovertOne_Story.Login puts in their controller's config_guns; ReplaceBuddyCharacter hands those out with the buddy's
// body. A second local player never logs in, so they came in with no weapon at all (no pistol in the first level).
// Before they ask for the body, give them player 1's weapons, one of each kind, as their loadout.
void GiveLoadout(uint8_t* base, uint32_t p0, uint32_t p1) {
  const uint32_t pc = Readable(p1) ? LoadU32(base, p1 + 0x40) : 0;
  const uint32_t guns = Readable(pc) ? Prop(base, pc, "config_guns") : 0xFFFFFFFF;
  if (guns == 0xFFFFFFFF) return;
  for (uint32_t i = 0; i < 4; ++i)
    if (LoadU32(base, pc + guns + 4 * i)) return;
  uint32_t item = ObjectProp(base, ObjectProp(base, PawnOf(base, p0), "InvManager"), "InventoryChain");
  uint32_t given = 0, kinds = 0;
  for (uint32_t k = 0; item && given < 4 && k < 64; ++k, item = ObjectProp(base, item, "Inventory")) {
    bool weapon = false;
    for (uint32_t c = LoadU32(base, item + 0x34), d = 0; Readable(c) && d < 12; c = LoadU32(base, c + 0x3C), ++d)
      weapon |= std::string_view(menu_pointer::NameOf(base, LoadU32(base, c + 0x2C))) == "C1Weapon";
    const uint32_t type = weapon ? Prop(base, item, "WeaponType") : 0xFFFFFFFF;
    const uint32_t kind = type != 0xFFFFFFFF ? Raw(base, item + type)[0] : 32;
    if (kind >= 32 || (kinds & (1u << kind)) || std::string_view(ClassName(base, item)) == "C1WeaponNone") continue;
    kinds |= 1u << kind;
    StoreU32(base, pc + guns + 4 * given++, LoadU32(base, item + 0x34));
    REXLOG_INFO("split screen: player 2's loadout gets {}", ClassName(base, item));
  }
}

// Player 2's screen effects. C1HUD's damage / low-health, death, reviving and slow-motion effects are post-process
// chains and material instances from the C1HUD package that every HUD points at: the same objects. Each HUD inserts
// a chain into its own player's post-process, but sets parameters on the shared materials, so one player getting hurt
// tinted the other's view too (network co-op has a copy per machine). Give player 2's HUD its own: copies of the chains
// and their effects (ConstructObject with the original as template) and material instances whose parent is the shared
// one (so they start out looking the same). Done once per HUD, before it has inserted any chain.
void OwnScreenEffects(PPCContext& ctx, uint8_t* base) {
  static uint32_t done = 0;
  const uint32_t p1 = Player(base, 1), pc0 = LoadU32(base, Player(base, 0) + 0x40);
  const uint32_t pc1 = Readable(p1) ? LoadU32(base, p1 + 0x40) : 0;
  const uint32_t hud0 = ObjectProp(base, pc0, "myHUD"), hud1 = ObjectProp(base, pc1, "myHUD");
  if (!hud0 || !hud1 || hud0 == hud1 || hud1 == done) return;
  if (LoadU32(base, p1 + 128)) return;  // LocalPlayer.PlayerPostProcessChains not empty: a chain is in, wait
  done = hud1;
  auto construct = [&](uint32_t cls, uint32_t from) {
    ctx.r3.u64 = cls;
    ctx.r4.u64 = hud1;
    ctx.r5.u64 = 0;  // NAME_None
    ctx.r6.u64 = 0;
    ctx.r7.u64 = from;  // template
    ctx.r8.u64 = 0;
    ctx.r9.u64 = 0;
    sub_825C96A0(ctx, base);  // ConstructObject
    return Readable(ctx.r3.u32) ? ctx.r3.u32 : 0u;
  };
  std::unordered_map<uint32_t, uint32_t> copies;
  auto own_material = [&](uint32_t mic) {
    if (const auto it = copies.find(mic); it != copies.end()) return it->second;
    if (std::string_view(ClassName(base, mic)) != "MaterialInstanceConstant" ||
        LoadU32(base, LoadU32(base, mic) + 400) != 0x82A88140)
      return mic;
    const uint32_t copy = construct(LoadU32(base, mic + 0x34), 0);
    if (!copy) return mic;
    ctx.r3.u64 = copy;
    ctx.r4.u64 = mic;
    sub_82A88140(ctx, base);  // UMaterialInstanceConstant::SetParent
    return copies[mic] = copy;
  };
  for (const char* name : {"DamageEffectMaterialInstance", "DeathEffectMaterialInstance",
                           "CriticalHealthBlurMaterialInstance", "SlowmoMIC"}) {
    const uint32_t at = Prop(base, hud1, name), mic = ObjectProp(base, hud1, name);
    if (mic && mic == ObjectProp(base, hud0, name)) StoreU32(base, hud1 + at, own_material(mic));
  }
  for (const char* name : {"DamageEffectPostProcess", "DeathPostProcessEffect", "RevivingEffectPostProcessChain",
                           "SlowmoEffectPostProcessChain"}) {
    const uint32_t at = Prop(base, hud1, name), chain = ObjectProp(base, hud1, name);
    if (!chain || chain != ObjectProp(base, hud0, name)) continue;
    const uint32_t copy = construct(LoadU32(base, chain + 0x34), chain);
    const uint32_t effects = copy ? Prop(base, copy, "Effects") : 0xFFFFFFFF;
    if (effects == 0xFFFFFFFF || LoadU32(base, copy + effects) == LoadU32(base, chain + effects)) continue;
    for (uint32_t i = 0, n = LoadU32(base, copy + effects + 4); i < n && i < 16; ++i) {
      const uint32_t slot = LoadU32(base, copy + effects) + 4 * i, effect = LoadU32(base, slot);
      const uint32_t own = Readable(effect) ? construct(LoadU32(base, effect + 0x34), effect) : 0;
      if (!own) continue;
      if (const uint32_t mic = ObjectProp(base, own, "Material"))
        StoreU32(base, own + Prop(base, own, "Material"), own_material(mic));
      StoreU32(base, slot, own);
    }
    StoreU32(base, hud1 + at, copy);
  }
  REXLOG_INFO("split screen: player 2's HUD {:08X} has its own screen effects ({} materials)", hud1, copies.size());
}

// Player 2's HUD widgets' materials. The same thing in the HUD scenes: a widget that draws through a material instance
// (the health bar: HudWidget_HealthBar writes LifeAmount into BarMatInstConst, which its BarImage draws as
// UIImage.ImageComponent.ImageRef.ImageTexture) shares the instance with player 1's copy of the widget, so both bars
// showed whoever wrote last. Every second, give player 2's scenes child instances of any shared one: one copy per
// original per pass, so a widget and the image drawing its material keep using the same one. Copies are made with
// player 2's LocalPlayer as outer, which is how a pass knows a widget already has its own.
void OwnHudMaterials(PPCContext& ctx, uint8_t* base) {
  static auto next = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  const uint32_t p1 = Player(base, 1);
  const uint32_t engine = LoadU32(base, kGEngine);
  const uint32_t viewport = Readable(engine) ? LoadU32(base, engine + 0x2E4) : 0;
  const uint32_t ui = ObjectProp(base, viewport, "UIController"), client = ObjectProp(base, ui, "SceneClient");
  const uint32_t active = Readable(client) ? Prop(base, client, "ActiveScenes") : 0xFFFFFFFF;
  if (!Readable(p1) || active == 0xFFFFFFFF) return;
  std::unordered_map<uint32_t, uint32_t> copies;
  auto own = [&](uint32_t object, uint32_t at) {
    const uint32_t mic = LoadU32(base, object + at);
    if (!Readable(mic) || LoadU32(base, mic + 0x28) == p1 ||
        std::string_view(ClassName(base, mic)) != "MaterialInstanceConstant" ||
        LoadU32(base, LoadU32(base, mic) + 400) != 0x82A88140)
      return;
    uint32_t& copy = copies[mic];
    if (!copy) {
      ctx.r3.u64 = LoadU32(base, mic + 0x34);
      ctx.r4.u64 = p1;
      ctx.r5.u64 = ctx.r6.u64 = ctx.r7.u64 = ctx.r8.u64 = ctx.r9.u64 = 0;
      sub_825C96A0(ctx, base);  // ConstructObject
      if (!Readable(ctx.r3.u32)) return;
      copy = ctx.r3.u32;
      ctx.r3.u64 = copy;
      ctx.r4.u64 = mic;
      sub_82A88140(ctx, base);  // UMaterialInstanceConstant::SetParent
    }
    StoreU32(base, object + at, copy);
  };
  // Every object property of the widget's own classes (not Object's) holding a material instance.
  auto own_props = [&](uint32_t object) {
    for (uint32_t cls = LoadU32(base, object + 0x34), depth = 0; Readable(cls) && depth < 16;
         cls = LoadU32(base, cls + 0x3C), ++depth) {
      if (std::string_view(menu_pointer::NameOf(base, LoadU32(base, cls + 0x2C))) == "Object") break;
      for (uint32_t f = LoadU32(base, cls + 0x50), n = 0; Readable(f) && n < 4096; f = LoadU32(base, f + 0x40), ++n)
        if (std::string_view(ClassName(base, f)) == "ObjectProperty") own(object, LoadU32(base, f + 0x64));
    }
  };
  std::function<void(uint32_t, int)> walk = [&](uint32_t w, int depth) {
    if (!Readable(w) || depth > 12) return;
    own_props(w);
    const uint32_t texture = ObjectProp(base, ObjectProp(base, w, "ImageComponent"), "ImageRef");
    if (const uint32_t at = texture ? Prop(base, texture, "ImageTexture") : 0xFFFFFFFF; at != 0xFFFFFFFF)
      own(texture, at);
    if (const uint32_t image = ObjectProp(base, w, "MyImage")) walk(image, depth + 1);
    if (const uint32_t co = Prop(base, w, "Children"); co != 0xFFFFFFFF)
      for (uint32_t k = 0, n = LoadU32(base, w + co + 4); k < n && k < 128; ++k)
        walk(LoadU32(base, LoadU32(base, w + co) + 4 * k), depth + 1);
    if (const uint32_t digits = Prop(base, w, "Digits"); digits != 0xFFFFFFFF)
      for (uint32_t d = 0; d < 7; ++d) walk(LoadU32(base, w + digits + 4 * d), depth + 1);
  };
  const uint32_t scenes = LoadU32(base, client + active);
  for (uint32_t i = 0, n = LoadU32(base, client + active + 4); i < n && i < 64; ++i) {
    const uint32_t scene = LoadU32(base, scenes + 4 * i);
    if (ObjectProp(base, scene, "PlayerOwner") == p1) walk(scene, 0);
  }
  if (!copies.empty())
    REXLOG_INFO("split screen: player 2's HUD widgets get their own copies of {} materials", copies.size());
}

// Joining. A pad that no local player uses pressing START during play (player 1 has a pawn) joins as player 2, or the
// "splitjoin" console command (tests) joins the first free one. From the input poll on the game thread, where typed
// console commands run too: DebugCreatePlayer <pad> (CreatePlayer spawns the controller, which waits in Spectating)
// and SetSplit 1 (top / bottom). Then, whenever player 2 has no pawn while player 1 has one - after joining, and
// after every level load or checkpoint restart, where the story game's Login sends the second controller to
// Spectating again - player 2 asks for the buddy's body, every few seconds until it has it.
struct Join {
  bool requested = false;
  int tries = 0;  // body requests since player 2 last had a pawn
  std::chrono::steady_clock::time_point next;
  uint16_t buttons[4] = {};
  ExecState exec;
  bool leave = false;  // player 2 asked to leave (quit from their pause menu, or "splitleave")
  std::atomic<uint32_t> last_press_pad{0xFF};  // pad that last pressed A or START, and when (steady clock ticks)
  std::atomic<int64_t> last_press_at{0};
  // A dialog moved to the player who asked for it (DeathMenus) keeps its input and focus on the player it opened for:
  // while it's up, the asker's A and B are read from the opener's pad instead (pad numbers, 0xFF = none).
  std::atomic<uint32_t> forward_from{0xFF}, forward_to{0xFF};
  std::atomic<uint32_t> fake_pad{0xFF}, fake_buttons{0};  // "padpress <user> <hex buttons>" (tests): held a few polls
  std::atomic<int> fake_polls{0};
  std::chrono::steady_clock::time_point relayout{};  // when player 1's HUD is laid out again after player 2 left
  int relayout_step = 0;                             // 1 close player 1's HUD scenes, 2 open them again
  bool front_end = false;  // the leave happens at the front end: no HUD to lay out again
  uint32_t swallow_start = 0;  // pads (bit per user) whose joining START press is hidden until released
  uint32_t connected = 0, seen = 0;  // pads (bit per user) whose last poll answered, and that have been polled
};
Join join;

// The game keeps which pads are connected from XN_SYS_INPUTDEVICESCHANGED, and the SDK sends that once, at boot.
// A pad plugged in (or woken) later then reads as unplugged for good: C1PlayerController.PlayerTick pauses whenever its
// player's pad is unplugged, so player 2 on such a pad got their pause menu every tick and could never resume (seen on
// real pads). So tell the game whenever a pad comes or goes, and on every join.
void NotifyPads(const char* why) {
  if (auto* kernel = rex::system::kernel_state()) {
    REXLOG_INFO("split screen: pads changed ({}), connected {:X}", why, join.connected);
    kernel->BroadcastNotification(0x00000012, 0);  // XN_SYS_INPUTDEVICESCHANGED
  }
}

bool Command(const std::string& cmd) {
  if (cmd == "splitjoin") return join.requested = true;
  if (cmd == "splitleave") return join.leave = true;
  if (cmd == "padsnotify") return NotifyPads("padsnotify"), true;
  if (cmd.rfind("padpress ", 0) == 0) {
    unsigned user = 0, buttons = 0;
    if (std::sscanf(cmd.c_str() + 9, "%u %x", &user, &buttons) != 2 || user >= 4) return false;
    join.fake_buttons = buttons, join.fake_pad = user, join.fake_polls = 6;
    REXLOG_INFO("split screen: pad {} presses {:04X}", user, buttons);
    return true;
  }
  return false;
}

bool PadUsed(uint8_t* base, uint32_t pad) {
  for (uint32_t i = 0, n = PlayerCount(base); i < n && i < 4; ++i)
    if (const uint32_t p = Player(base, i); Readable(p) && LoadU32(base, p + 0x60) == pad) return true;
  return false;
}

void Run(PPCContext& ctx, uint8_t* base, uint32_t player, const std::string& cmd) {
  if (!join.exec.buffer) join.exec.buffer = rex::system::kernel_memory()->SystemHeapAlloc(0x400);
  if (join.exec.buffer) RunPlayerCommand(ctx, base, join.exec, player, cmd, false);
}

// Every active UI scene (GameUISceneClient.ActiveScenes).
template <typename F>
void ForEachScene(uint8_t* base, F&& f) {
  const uint32_t engine = LoadU32(base, kGEngine);
  const uint32_t viewport = Readable(engine) ? LoadU32(base, engine + 0x2E4) : 0;
  const uint32_t ui = ObjectProp(base, viewport, "UIController"), client = ObjectProp(base, ui, "SceneClient");
  const uint32_t active = Readable(client) ? Prop(base, client, "ActiveScenes") : 0xFFFFFFFF;
  if (active == 0xFFFFFFFF) return;
  const uint32_t scenes = LoadU32(base, client + active);
  for (uint32_t i = 0, n = LoadU32(base, client + active + 4); i < n && i < 64; ++i)
    if (const uint32_t scene = LoadU32(base, scenes + 4 * i); Readable(scene)) f(scene);
}

// Flag a function exec (UFunction +0x94 FunctionFlags, FUNC_Exec 0x200) so a console command can run it.
bool MakeExec(uint8_t* base, uint32_t object, const char* name) {
  const uint32_t fn = Readable(object) ? FindFunction(base, object, name) : 0;
  if (!fn || std::string_view(ClassName(base, fn)) != "Function") return false;
  StoreU32(base, fn + 0x94, LoadU32(base, fn + 0x94) | 0x200);
  return true;
}

uint32_t HudOf(uint8_t* base, uint32_t player) {
  const uint32_t actor = Readable(player) ? LoadU32(base, player + 0x40) : 0;
  return ObjectProp(base, actor, "myHUD");
}

// Death menus with two local players.
// - C1HUD.DisplayDeathScene opens DeathSceneClientTemplate on a network client and DeathSceneTemplate (retry from the
//   checkpoint, quit) everywhere else, so player 2 got the host's menu. Retrying is the host's call
//   (C1PlayerController.OnLoadLastCheckpoint ignores anyone but ctrl_FiftyCent), so player 2's HUD gets the client
//   menu, as the buddy has over the network.
// - Every menu scene is INPUTMODE_Locked: its owner's input only, and everyone else's swallowed rather than passed to
//   the scenes below. Over the network each machine shows one player's menus; here player 2's death menu, opened
//   last, sat on top of player 1's and player 1 could not retry (seen on real pads). With two players, Locked scenes
//   become INPUTMODE_MatchingOnly (owner only, the rest goes down the stack).
// - Player 1's retry asks "load the last checkpoint?" in a dialog (C1SeqAct_DialogWindow, native) that opened as
//   player 2's, on player 2's button bar (seen on real pads): only player 2 could answer. A dialog that opens for a
//   player other than the one who just pressed A or START moves to the presser's half of the screen (owner and button
//   bar). Its input mask and focus stay with the player it opened for (focus is kept per player index: given the
//   presser's mask, it took no A from anyone), so while it's up the presser's A and B go to the opener's pad (Poll).
void DeathMenus(uint8_t* base, uint32_t count) {
  static uint32_t done = 0, template_offset = 0xFFFFFFFF, client_offset = 0xFFFFFFFF;
  static auto next = std::chrono::steady_clock::time_point{};
  if (count < 2) return;
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(100);
  const uint32_t p0 = Player(base, 0), p1 = Player(base, 1);
  if (const uint32_t hud = HudOf(base, p1); Readable(hud) && hud != done) {
    if (template_offset == 0xFFFFFFFF) template_offset = Prop(base, hud, "DeathSceneTemplate");
    if (client_offset == 0xFFFFFFFF) client_offset = Prop(base, hud, "DeathSceneClientTemplate");
    const uint32_t client = client_offset != 0xFFFFFFFF ? LoadU32(base, hud + client_offset) : 0;
    if (template_offset != 0xFFFFFFFF && Readable(client)) {
      StoreU32(base, hud + template_offset, client);
      done = hud;
      REXLOG_INFO("split screen: player 2's HUD {:08X} opens the client death menu ({:08X})", hud, client);
    }
  }
  static uint32_t moved = 0;
  bool moved_up = false;
  uint32_t presser = 0, presser_index = 0;
  const int64_t age = now.time_since_epoch().count() - join.last_press_at.load();
  if (age >= 0 && age < std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(2)).count())
    for (uint32_t i = 0; i < count && i < 4; ++i)
      if (const uint32_t player = Player(base, i); Readable(player) && LoadU32(base, player + 0x60) == join.last_press_pad)
        presser = player, presser_index = i;
  uint32_t dialog = 0, presser_bar = 0;
  ForEachScene(base, [&](uint32_t scene) {
    if (const uint32_t at = Prop(base, scene, "SceneInputMode"); at != 0xFFFFFFFF && Raw(base, scene + at)[0] == 1)
      Raw(base, scene + at)[0] = 2;
    const std::string_view cls = ClassName(base, scene);
    if (cls == "C1UIScene_DialogWindow" && scene != moved) dialog = scene;
    if (scene == moved) moved_up = true;
    if (cls.rfind("ButtonBarScene", 0) == 0 && ObjectProp(base, scene, "PlayerOwner") == presser) presser_bar = scene;
  });
  if (!moved_up && join.forward_to != 0xFF) {
    REXLOG_INFO("split screen: moved dialog closed, pad {} reads its own A and B again", join.forward_from.load());
    join.forward_from = 0xFF, join.forward_to = 0xFF;
  }
  if (!dialog || !presser) return;
  const uint32_t owner_at = Prop(base, dialog, "PlayerOwner");
  if (owner_at == 0xFFFFFFFF) return;
  moved = dialog;
  const uint32_t owner = LoadU32(base, dialog + owner_at);
  if (owner == presser || !Readable(owner)) return;
  StoreU32(base, dialog + owner_at, presser);
  join.forward_from = LoadU32(base, presser + 0x60), join.forward_to = LoadU32(base, owner + 0x60);
  if (const uint32_t bar_at = Prop(base, dialog, "MyButtonScene"); bar_at != 0xFFFFFFFF && presser_bar)
    StoreU32(base, dialog + bar_at, presser_bar);
  REXLOG_INFO("split screen: dialog {} ({:08X}) opened as {:08X}'s, moves to player {} who asked for it (pad {} "
              "answers through pad {})",
              menu_pointer::NameOf(base, LoadU32(base, dialog + 0x2C)), dialog, owner, presser_index + 1,
              join.forward_from.load(), join.forward_to.load());
}

// C1HUD.ReceivePauseMessage opens PauseMessageSceneTemplate (BuddyPaused: "the other player has paused the game, kick
// them?") on every HUD whose player isn't WorldInfo.Pauser. Over the network that's the other machine's player; with
// both players local it's player 2's HUD whenever anyone pauses (the pause is player 1's), and its kick action would
// end the session. So while more than one local player plays, the local HUDs have no template, and player 1's gets it
// back when they play alone again.
void SuppressBuddyPause(uint8_t* base, uint32_t count) {
  static uint32_t template_ = 0, hud_offset = 0, template_offset = 0xFFFFFFFF;
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t player = Player(base, i), actor = Readable(player) ? LoadU32(base, player + 0x40) : 0;
    if (!Readable(actor)) continue;
    if (!hud_offset)
      if (const uint32_t prop = FindFunction(base, actor, "myHUD")) hud_offset = LoadU32(base, prop + 0x64);
    const uint32_t hud = hud_offset ? LoadU32(base, actor + hud_offset) : 0;
    if (!Readable(hud)) continue;
    if (template_offset == 0xFFFFFFFF) template_offset = Prop(base, hud, "PauseMessageSceneTemplate");
    if (template_offset == 0xFFFFFFFF) return;
    const uint32_t now = LoadU32(base, hud + template_offset);
    if (count > 1 && now) {
      if (template_ != now) REXLOG_INFO("split screen: no pause message scene ({:08X}) while sharing the screen", now);
      template_ = now;
      StoreU32(base, hud + template_offset, 0);
    } else if (count == 1 && !now && template_) {
      StoreU32(base, hud + template_offset, template_);
    }
  }
}

void Poll(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state) {
  static const bool on = bots::SplitScreen();
  if (!on || user >= 4) return;
  if (const uint32_t bit = 1u << user, was = join.connected; !(join.seen & bit) || bool(was & bit) != bool(state)) {
    join.connected = state ? was | bit : was & ~bit;
    if (join.seen & bit) NotifyPads(state ? "connected" : "unplugged");
    join.seen |= bit;
  }
  if (state && join.fake_pad == user && join.fake_polls > 0) {
    const uint32_t fake = join.fake_buttons;
    Raw(base, state)[4] |= uint8_t(fake >> 8), Raw(base, state)[5] |= uint8_t(fake);
    if (--join.fake_polls == 0) join.fake_pad = 0xFF;
  }
  const uint16_t buttons = state ? uint16_t(Raw(base, state)[4] << 8 | Raw(base, state)[5]) : 0;
  const uint16_t prev_buttons = join.buttons[user];
  const bool start = (buttons & 0x10) && !(prev_buttons & 0x10);  // XINPUT_GAMEPAD_START, pressed now
  join.buttons[user] = buttons;
  // The START press that joins player 2 is only for joining: still held in the frames after the join, the new
  // player's controller read it as a pause, opening their pause menu (and closing player 1's if they had paused,
  // leaving a pause neither could lift: only the pauser unpauses).
  if (join.swallow_start & (1u << user)) {
    if (state && (buttons & 0x10))
      Raw(base, state)[5] &= uint8_t(~0x10);
    else
      join.swallow_start &= ~(1u << user);
  }
  if (const uint32_t from = join.forward_from, to = join.forward_to; state && from < 4 && to < 4 && from != to) {
    if (user == from) Raw(base, state)[4] &= uint8_t(~0x30);  // A and B (0x1000, 0x2000): not for the asker's menus
    if (user == to) Raw(base, state)[4] |= uint8_t((join.buttons[from] >> 8) & 0x30);
  }
  if ((buttons & 0x1010) & ~prev_buttons) {  // A or START pressed now
    join.last_press_pad = user;
    join.last_press_at = std::chrono::steady_clock::now().time_since_epoch().count();
  }
  if (OffGameThread()) return;  // joins, script and console commands only on the game thread
  if (!Patch(base)) return;
  const uint32_t p0 = Player(base, 0);
  if (!Readable(p0)) return;
  ExposeBuddyRequest(base, LoadU32(base, p0 + 0x40));
  SuppressBuddyPause(base, PlayerCount(base));
  DeathMenus(base, PlayerCount(base));
  const bool playing = PawnOf(base, p0) != 0;
  const PPCRegister r3 = ctx.r3;  // keep the poll's return value
  const auto now = std::chrono::steady_clock::now();
  if (PlayerCount(base) > 1) {
    OwnScreenEffects(ctx, base);
    OwnHudMaterials(ctx, base);
  }
  // Player 1 quit to the main menu with player 2 still in: the front end has no split screen and, with two profiles
  // signed in, keeps showing "multiple gamer profiles" (seen on real pads). Player 2 leaves there too.
  if (PlayerCount(base) > 1 && !join.leave) {
    const uint32_t pc = LoadU32(base, p0 + 0x40);
    if (Readable(pc) &&
        std::string_view(menu_pointer::NameOf(base, LoadU32(base, LoadU32(base, pc + 0x34) + 0x2C))) ==
            "C1EntryPlayerController")
      join.leave = join.front_end = true;
  }
  if (join.leave) {
    join.leave = false;
    if (const uint32_t p1 = Player(base, 1); PlayerCount(base) > 1 && Readable(p1)) {
      // Player 2 leaves: remove them (GameViewportClient execs; RemovePlayer destroys their pawn and controller) and
      // give player 1 the whole screen again. Player 1's HUD is laid out for it a moment later: reopened in the same
      // frame, its scenes still had the half screen's viewport size (run-121).
      REXLOG_INFO("split screen: player 2 leaves");
      // CovertOne_Story.Logout tells the other player their buddy left (C1PlayerController.OpenBuddyDisconnectDialog,
      // BuddyDisconnectConfirmation, a modal dialog for network co-op): player 1's controller gets no such scene.
      const uint32_t pc = LoadU32(base, p0 + 0x40);
      if (Readable(pc))
        if (const uint32_t at = Prop(base, pc, "m_BuddyDisconnectScene"); at != 0xFFFFFFFF) StoreU32(base, pc + at, 0);
      // Logout also queues a checkpoint reload (pending_load_checkpoint) and the buddy-left dialog
      // (pendingBuddyDisconnectMessage): keep those flag words as they were. And it leaves the front end a "Buddy
      // Quit, the game will restart from the last checkpoint" message (FE_ErrorMessage(1), shown at the next main
      // menu): FE_ErrorMessage(0) clears it, as joining an invite does.
      std::vector<std::pair<uint32_t, uint32_t>> flags;
      for (const char* name : {"pending_load_checkpoint", "pendingBuddyDisconnectMessage"})
        if (const uint32_t at = Readable(pc) ? Prop(base, pc, name) : 0xFFFFFFFF; at != 0xFFFFFFFF)
          flags.emplace_back(pc + at, LoadU32(base, pc + at));
      Run(ctx, base, p0, fmt::format("DebugRemovePlayer {}", LoadU32(base, p1 + 0x60)));
      for (const auto& [at, word] : flags) StoreU32(base, at, word);
      if (Readable(pc)) {
        const uint32_t fe_error = LoadU32(base, LoadU32(base, pc) + 1484);  // AC1PlayerController::FE_ErrorMessage
        ctx.r3.u64 = pc;
        ctx.r4.u64 = 0;
        rex::runtime::ResolveIndirectFunction(fe_error)(ctx, base);
      }
      Run(ctx, base, p0, "SetSplit 0");
      bots::SignOutPlayerTwo();
      join.relayout = now + std::chrono::milliseconds(500);
      join.relayout_step = join.front_end ? 0 : 1;
      join.front_end = false;
    }
  }
  // Player 2's own quit (QuitToMainMenuHUD from their menu) closes HUD scenes by template, the first found: player 1's
  // main HUD scene. Removing player 2 then hands player 1 theirs (runs 122-124: fcHUD3, player 2's object, owned by
  // player 1 and still 360 high), which ShowGameHUD would just reuse. So HideGameHUD twice (one scene per template
  // each), then reopen a second later.
  if (join.relayout_step && now >= join.relayout) {
    if (join.relayout_step == 1) {
      REXLOG_INFO("split screen: player 1's HUD takes the whole screen again");
      Run(ctx, base, p0, "HideGameHUD true");
      Run(ctx, base, p0, "HideGameHUD true");
      join.relayout = now + std::chrono::seconds(1);
      join.relayout_step = 2;
    } else {
      Run(ctx, base, p0, "ShowGameHUD true");
      Run(ctx, base, p0, "UnPauseShowHUD");
      join.relayout_step = 0;
      // Removing player 2 closes player 1's death menu too (both down, then player 2 quit from theirs): player 1 is
      // left dead with nothing to press (seen on real pads). Open it again.
      if (!PawnOf(base, p0) && MakeExec(base, HudOf(base, p0), "DisplayDeathScene")) {
        REXLOG_INFO("split screen: player 1 is dead, their death menu opens again");
        Run(ctx, base, p0, "DisplayDeathScene");
      }
    }
  }
  if (PlayerCount(base) == 1) {
    // BOTS_SPLIT_TEST's scripted input reaches every pad, so there only "splitjoin" joins.
    static const bool scripted = std::getenv("BOTS_SPLIT_TEST") != nullptr;
    int pad = -1;
    if (playing && start && !scripted && !PadUsed(base, user)) pad = int(user);
    if (playing && join.requested)
      for (uint32_t i = 0; i < 4 && pad < 0; ++i)
        if (!PadUsed(base, i)) pad = int(i);
    if (pad < 0) return;
    join.requested = false;
    join.tries = 0;
    join.next = now + std::chrono::seconds(2);
    REXLOG_INFO("split screen: controller {} joins", pad);
    NotifyPads("join");
    if (start && pad == int(user) && state) {
      join.swallow_start |= 1u << user;
      Raw(base, state)[5] &= uint8_t(~0x10);
    }
    // Player 2 plays as the XAM user of the controller it ends up with (after the swap below).
    bots::SignInPlayerTwo(scripted && LoadU32(base, p0 + 0x60) != 0 ? LoadU32(base, p0 + 0x60) : uint32_t(pad));
    Run(ctx, base, p0, fmt::format("DebugCreatePlayer {}", pad));
    Run(ctx, base, p0, "SetSplit 1");
    // Scripted input reaches every pad, so the title screen may have given player 1 controller 1: swap them back.
    if (scripted && LoadU32(base, p0 + 0x60) != 0) Run(ctx, base, p0, "SSSwapControllers");
    // Player 1's HUD scenes were laid out for the whole screen: reopen them for the top half (C1HUD execs). Hiding
    // closes HUD scenes by template, whoever owns them, so player 2's HUD opens again too. ShowGameHUD leaves the main
    // HUD scene hidden (it calls HideHUDScene right after opening it); UnPauseShowHUD shows it, as after a pause.
    Run(ctx, base, p0, "HideGameHUD true");
    Run(ctx, base, p0, "ShowGameHUD true");
    Run(ctx, base, p0, "UnPauseShowHUD");
    if (const uint32_t p1 = Player(base, 1); Readable(p1)) {
      Run(ctx, base, p1, "ShowGameHUD true");
      Run(ctx, base, p1, "UnPauseShowHUD");
    }
  } else if (const uint32_t p1 = Player(base, 1); Readable(p1)) {
    if (PawnOf(base, p1)) {
      if (join.tries) REXLOG_INFO("split screen: player 2 is in, pawn {:08X}", PawnOf(base, p1));
      join.tries = 0;
      join.next = now + std::chrono::seconds(2);  // a fresh level's first request waits a little too
    } else if (playing && now >= join.next) {
      if (++join.tries == 10) REXLOG_WARN("split screen: player 2 still has no pawn, retrying quietly");
      if (join.tries < 10) REXLOG_INFO("split screen: player 2 requests the buddy's body");
      GiveLoadout(base, p0, p1);
      Run(ctx, base, p1, "ServerRequestBuddyCharacter");
      join.next = now + std::chrono::seconds(3);
    } else if (!playing) {
      join.next = std::max(join.next, now + std::chrono::seconds(2));
    }
  }
  ctx.r3 = r3;
}

}  // namespace split_screen
namespace debug {
void Scenes(uint8_t* base, bool full, bool tree, const char* find, bool stack);
}
namespace split_screen {

// Scripted co-op moments black out both views with the HUD still drawn (B-134): log the GameViewportClient's
// bDisableWorldRendering word whenever it changes, with the active scenes (each with its own flag word).
void WatchWorldRendering(uint8_t* base, uint32_t player) {
  static uint32_t last = 0xFFFFFFFF;
  const uint32_t vc_prop = FindFunction(base, player, "ViewportClient");
  const uint32_t vc = vc_prop ? LoadU32(base, player + LoadU32(base, vc_prop + 0x64)) : 0;
  const uint32_t prop = Readable(vc) ? FindFunction(base, vc, "bDisableWorldRendering") : 0;
  if (!prop) return;
  const uint32_t word = LoadU32(base, vc + LoadU32(base, prop + 0x64));
  if (word == last) return;
  REXLOG_INFO("split screen: viewport client {:08X} bDisableWorldRendering word {:08X} (was {:08X})", vc, word, last);
  last = word;
  debug::Scenes(base, false, false, nullptr, false);
}

// The minigames (MinigameControl) black out both views with the HUD drawn, and bDisableWorldRendering stays put (B-134):
// with two players, log "splitcams" and the scenes whenever a player's view target, cinematic flags, camera fade or
// camera target changes (at most twice a second, 300 times a run). With BOTS_SPLIT_CAPTURE=1 (and BOTS_GFX_CAPTURE set),
// a second after a view target change also records a frame for bots-replay (4 a run); each one stalls the game about
// half a second, which reads as a stutter, so it's opt-in.
void WatchCameras(uint8_t* base) {
  static std::vector<uint32_t> last, last_targets;
  static int dumps = 0, captures = 0;
  static std::chrono::steady_clock::time_point next{}, capture_at{};
  static const bool capture = std::getenv("BOTS_GFX_CAPTURE") && [] {
    const char* v = std::getenv("BOTS_SPLIT_CAPTURE");
    return v && *v == '1';
  }();
  const auto now = std::chrono::steady_clock::now();
  if (capture_at != std::chrono::steady_clock::time_point{} && now >= capture_at) {
    capture_at = {};
    REXLOG_INFO("split screen: capturing a frame after the view change");
    bots::gfx::RequestCapture();
  }
  const uint32_t count = PlayerCount(base);
  // A camera actor that keeps its aspect ratio (the minigames' CameraActorReplicated) shrinks its player's view to a
  // 16:9 box inside the half-screen. The views then no longer cover the screen, so the viewport client fills it with a
  // black tile - drawn after the views, over both of them (seen on real pads; draw 417 of capture f12570). With two
  // players, camera actors fill their player's view instead. CameraActor's first bool, the top bit of its word.
  for (uint32_t i = 0; count > 1 && i < count && i < 4; ++i) {
    const uint32_t pc = LoadU32(base, Player(base, i) + 0x40);
    const uint32_t target = Readable(pc) ? LoadU32(base, pc + Prop(base, pc, "ViewTarget")) : 0;
    if (!Readable(target)) continue;
    const uint32_t at = Prop(base, target, "bConstrainAspectRatio");
    if (at == 0xFFFFFFFF || !(LoadU32(base, target + at) & 0x80000000)) continue;
    REXLOG_INFO("split screen: player {}'s camera actor {:08X} fills their view (no aspect ratio box)", i, target);
    StoreU32(base, target + at, LoadU32(base, target + at) & ~0x80000000u);
  }
  if (count < 2 || dumps >= 300 || now < next) return;
  std::vector<uint32_t> state, targets;
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t pc = LoadU32(base, Player(base, i) + 0x40);
    if (!Readable(pc)) continue;
    const uint32_t cam = LoadU32(base, pc + Prop(base, pc, "PlayerCamera"));
    targets.push_back(LoadU32(base, pc + Prop(base, pc, "ViewTarget")));
    state.insert(state.end(), {LoadU32(base, pc + Prop(base, pc, "ViewTarget")),
                               LoadU32(base, pc + Prop(base, pc, "bCinematicMode"))});
    if (Readable(cam))
      state.insert(state.end(), {LoadU32(base, cam + Prop(base, cam, "bEnableFading")),
                                 uint32_t(menu_pointer::LoadF32(base, cam + Prop(base, cam, "FadeAmount")) * 4),
                                 LoadU32(base, cam + Prop(base, cam, "FadeColor")),
                                 LoadU32(base, cam + Prop(base, cam, "ViewTarget"))});
  }
  if (state == last) return;
  const bool target_changed = !last_targets.empty() && targets != last_targets;
  last = state;
  last_targets = targets;
  ++dumps;
  next = now + std::chrono::milliseconds(500);
  REXLOG_INFO("split screen: view changed");
  debug::Command(base, "splitcams");
  debug::Scenes(base, false, false, nullptr, false);
  if (capture && target_changed && captures < 4) {
    ++captures;
    capture_at = now + std::chrono::seconds(1);
  }
}

// Log the local players when their number, controllers, actors or pawns change.
void Tick(uint8_t* base) {
  game_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  static const bool on = bots::SplitScreen();
  if (!on) return;
  if (const uint32_t p = Player(base, 0); Readable(p)) WatchWorldRendering(base, p);
  WatchCameras(base);
  auto cls = [&](uint32_t o) {
    return Readable(o) ? menu_pointer::NameOf(base, LoadU32(base, LoadU32(base, o + 0x34) + 0x2C)) : "None";
  };
  const uint32_t count = PlayerCount(base);
  std::vector<uint32_t> state = {count};
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t p = Player(base, i);
    state.insert(state.end(), {p, Readable(p) ? LoadU32(base, p + 0x60) : 0, Readable(p) ? LoadU32(base, p + 0x40) : 0,
                               PawnOf(base, p)});
  }
  static std::vector<uint32_t> seen;
  if (state == seen) return;
  seen = state;
  for (uint32_t i = 0; i < count && i < 4; ++i) {
    const uint32_t p = Player(base, i);
    if (!Readable(p)) continue;
    const uint32_t actor = LoadU32(base, p + 0x40), pawn = PawnOf(base, p);
    const uint32_t hud_prop = Readable(actor) ? FindFunction(base, actor, "myHUD") : 0;
    const uint32_t hud = hud_prop ? LoadU32(base, actor + LoadU32(base, hud_prop + 0x64)) : 0;
    REXLOG_INFO("split screen: player {} {:08X} controller id {} origin {} {} size {} {} actor {:08X} {} pawn {:08X} {} "
                "hud {:08X} {}",
                i, p, LoadU32(base, p + 0x60), menu_pointer::LoadF32(base, p + 0x68),
                menu_pointer::LoadF32(base, p + 0x6C), menu_pointer::LoadF32(base, p + 0x70),
                menu_pointer::LoadF32(base, p + 0x74), actor, cls(actor), pawn, cls(pawn), hud, cls(hud));
  }
}
}  // namespace split_screen

// Debug aids for UI and engine work (console commands through BOTS_EXEC / BOTS_EXEC_FILE, next to "peek"):
//   props <hex object> [tag]  every property of the object's class chain: class, name, property type, offset, the word
//                             there as hex and as a float
//   uiscenes [tree|full|stack] the scene client's active scenes: name, owner LocalPlayer, SceneRenderMode (1 = drawn in
//                             the owner's share of the screen), Position, CurrentViewportSize, and each scene's top-level
//                             widgets with RenderBounds (scene pixels, before the split-screen draw shift). "tree" walks
//                             every widget (Children, and HudWidget_BigNumber Digits) with the bHidden flags word,
//                             Opacity and C1UI_HudWidget OpacityAlpha; "full" adds the client's and each scene's vtable,
//                             the GetViewport* UFunction records and the props of every top-level widget. "stack"
//                             lists each scene's RenderStack (+0x19C, the widgets it draws, in order) with each
//                             widget's owner.
//   traceimage <name>         log the first renders of UIImages of that name per scene, with the guest call chain
//                             (also BOTS_TRACE_UIIMAGE)
//   uiprops <widget name>     the props of every widget of that name in any active scene (Children and Digits, at any
//                             depth), tagged with the scene's owner LocalPlayer.
//   words <hex addr> <count> [tag]  raw guest words from addr, as hex and as floats
//   renderdiff                the next split-screen-shifted scene draw logs every word of the scene and its widgets
//                             (first 0x1600 bytes) that differs after the draw and the shift's restore: what drawing
//                             changed for good
//   splitcams                 each local player's view target, cinematic flags, camera fade and POV
//   poke <hex addr> <hex word> write one guest word (logs the old value)
//   splitpp share|own         player 2's LocalPlayer.PlayerPostProcess: player 1's chain, or back to its own
//   gfxcapture                record the next frame's D3D calls (gfx/capture.cpp; launch with BOTS_GFX_CAPTURE=1e9)
//   traceeasel [n]            the next n (40) easel post-renders (C1UI_Easel / SfUI_Easel canvases: button bar prompts)
//                             log the widget, the canvas rectangle GetPosition gave and each draw's position and origin
// LogGuestStack is for hooks: the guest call chain at that point.
namespace debug {
using menu_pointer::Readable;

std::string trace_image = [] {
  const char* v = std::getenv("BOTS_TRACE_UIIMAGE");
  return std::string(v ? v : "");
}();
std::unordered_map<uint32_t, int> trace_image_seen;  // renders logged, by scene
bool render_diff = false;
int trace_easel = 0;

const char* NameOf(uint8_t* base, uint32_t object) {
  return Readable(object) ? menu_pointer::NameOf(base, LoadU32(base, object + 0x2C)) : "None";
}

void Props(uint8_t* base, uint32_t object, const std::string& tag) {
  if (!Readable(object)) {
    REXLOG_WARN("props: {:08X} is not readable", object);
    return;
  }
  REXLOG_INFO("props {:08X} {} ({})", object, NameOf(base, object), NameOf(base, LoadU32(base, object + 0x34)));
  for (uint32_t cls = LoadU32(base, object + 0x34), depth = 0; Readable(cls) && depth < 12;
       cls = LoadU32(base, cls + 0x3C), ++depth)
    for (uint32_t f = LoadU32(base, cls + 0x50), n = 0; Readable(f) && n < 4096; f = LoadU32(base, f + 0x40), ++n) {
      const char* type = NameOf(base, LoadU32(base, f + 0x34));
      if (!std::strstr(type, "Property")) continue;
      const uint32_t off = LoadU32(base, f + 0x64), v = LoadU32(base, object + off);
      if (std::string_view(type) == "BoolProperty")  // UBoolProperty +0x84 BitMask (+0x68 is the next-property link)
        REXLOG_INFO("  {} {}.{} {} +{:X} = {:08X} mask {:08X} -> {}", tag, NameOf(base, cls), NameOf(base, f), type,
                    off, v, LoadU32(base, f + 0x84), (v & LoadU32(base, f + 0x84)) != 0);
      else
        REXLOG_INFO("  {} {}.{} {} +{:X} = {:08X} ({})", tag, NameOf(base, cls), NameOf(base, f), type, off, v,
                    menu_pointer::LoadF32(base, object + off));
    }
}

std::string Words(uint8_t* base, uint32_t from, uint32_t to) {
  std::string out;
  for (uint32_t a = from; a < to; a += 4) out += fmt::format(" {:08X}", LoadU32(base, a));
  return out;
}

// One line per widget, its Children and Digits below it.
void Tree(uint8_t* base, uint32_t w, int depth) {
  using split_screen::Prop;
  if (!Readable(w) || depth > 10) return;
  auto f = [&](uint32_t object, const char* name) {
    const uint32_t off = Prop(base, object, name);
    return off == 0xFFFFFFFF ? -1.f : menu_pointer::LoadF32(base, object + off);
  };
  const uint32_t hidden = Prop(base, w, "bHidden");
  REXLOG_INFO("  {:>{}}{:08X} {} ({}) bounds {} {} {} {} flags {:08X} opacity {} alpha {}", "", 2 * depth, w,
              NameOf(base, w), NameOf(base, LoadU32(base, w + 0x34)), menu_pointer::LoadF32(base, w + 0x204),
              menu_pointer::LoadF32(base, w + 0x208), menu_pointer::LoadF32(base, w + 0x20C),
              menu_pointer::LoadF32(base, w + 0x210), hidden == 0xFFFFFFFF ? 0 : LoadU32(base, w + hidden),
              f(w, "Opacity"), f(w, "OpacityAlpha"));
  const uint32_t co = Prop(base, w, "Children");
  if (co != 0xFFFFFFFF)
    for (uint32_t k = 0, n = LoadU32(base, w + co + 4); k < n && k < 64; ++k)
      Tree(base, LoadU32(base, LoadU32(base, w + co) + 4 * k), depth + 1);
  if (const uint32_t digits = Prop(base, w, "Digits"); digits != 0xFFFFFFFF)
    for (uint32_t d = 0; d < 7; ++d) Tree(base, LoadU32(base, w + digits + 4 * d), depth + 1);
}

// Props of every widget named `name` below `w`.
void FindProps(uint8_t* base, uint32_t w, const char* name, const std::string& tag, int depth) {
  using split_screen::Prop;
  if (!Readable(w) || depth > 10) return;
  if (!std::strcmp(NameOf(base, w), name)) Props(base, w, tag);
  const uint32_t co = Prop(base, w, "Children");
  if (co != 0xFFFFFFFF)
    for (uint32_t k = 0, n = LoadU32(base, w + co + 4); k < n && k < 64; ++k)
      FindProps(base, LoadU32(base, LoadU32(base, w + co) + 4 * k), name, tag, depth + 1);
}

void Scenes(uint8_t* base, bool full, bool tree, const char* find = nullptr, bool stack = false) {
  using split_screen::Prop;
  auto f = [&](uint32_t a) { return menu_pointer::LoadF32(base, a); };
  const uint32_t engine = LoadU32(base, kGEngine);
  const uint32_t viewport = Readable(engine) ? LoadU32(base, engine + 0x2E4) : 0;
  const uint32_t ui = Readable(viewport) ? LoadU32(base, viewport + Prop(base, viewport, "UIController")) : 0;
  const uint32_t client = Readable(ui) ? LoadU32(base, ui + Prop(base, ui, "SceneClient")) : 0;
  if (!Readable(client)) {
    REXLOG_WARN("uiscenes: no scene client yet");
    return;
  }
  const uint32_t off = Prop(base, client, "ActiveScenes");
  const uint32_t scenes = LoadU32(base, client + off), count = LoadU32(base, client + off + 4);
  REXLOG_INFO("uiscenes: client {:08X} {} vt {:08X}, {} active", client, NameOf(base, LoadU32(base, client + 0x34)),
              LoadU32(base, client), count);
  if (full) REXLOG_INFO("uiscenes: client vt{}", Words(base, LoadU32(base, client), LoadU32(base, client) + 400));
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    const uint32_t sc = LoadU32(base, scenes + 4 * i);
    if (!Readable(sc)) continue;
    if (find) {
      FindProps(base, sc, find, fmt::format("{:08X}", LoadU32(base, sc + Prop(base, sc, "PlayerOwner"))), 0);
      continue;
    }
    const uint32_t pos = sc + Prop(base, sc, "Position"), size = sc + Prop(base, sc, "CurrentViewportSize");
    // "flags word": the word holding the scene's bDisableWorldRendering (and its neighbouring bools).
    const uint32_t dwr = split_screen::FindFunction(base, sc, "bDisableWorldRendering");
    REXLOG_INFO("uiscene {} {:08X} {} owner {:08X} mode {} pos {} {} {} {} vsize {} {} flags word {:08X}", i, sc,
                NameOf(base, sc), LoadU32(base, sc + Prop(base, sc, "PlayerOwner")),
                Raw(base, sc + Prop(base, sc, "SceneRenderMode"))[0], f(pos), f(pos + 4), f(pos + 8), f(pos + 12),
                f(size), f(size + 4), dwr ? LoadU32(base, sc + LoadU32(base, dwr + 0x64)) : 0);
    if (stack) {
      const uint32_t rs = Prop(base, sc, "RenderStack");
      for (uint32_t k = 0, n = LoadU32(base, sc + rs + 4); k < n && k < 512; ++k) {
        const uint32_t w = LoadU32(base, LoadU32(base, sc + rs) + 4 * k);
        REXLOG_INFO("  render {} {:08X} {} ({}) in {}", k, w, NameOf(base, w), NameOf(base, LoadU32(base, w + 0x34)),
                    NameOf(base, Readable(w) ? LoadU32(base, w + 0x17C) : 0));
      }
      continue;
    }
    if (full) {
      REXLOG_INFO("  vt {:08X}:{}", LoadU32(base, sc), Words(base, LoadU32(base, sc), LoadU32(base, sc) + 800));
      for (const char* name : {"GetViewportOrigin", "GetViewportSize", "GetViewportOffset", "GetViewportScale"})
        if (const uint32_t fn = split_screen::FindFunction(base, sc, name))
          REXLOG_INFO("  fn {} {:08X}:{}", name, fn, Words(base, fn + 0x80, fn + 0xC0));
    }
    const uint32_t co = Prop(base, sc, "Children");
    const uint32_t kids = LoadU32(base, sc + co), nk = LoadU32(base, sc + co + 4);
    for (uint32_t k = 0; k < nk && k < 64; ++k) {
      const uint32_t w = LoadU32(base, kids + 4 * k);
      if (!Readable(w)) continue;
      if (tree) {
        Tree(base, w, 1);
        continue;
      }
      REXLOG_INFO("  widget {:08X} {} ({}) bounds {} {} {} {}", w, NameOf(base, w), NameOf(base, LoadU32(base, w + 0x34)),
                  f(w + 0x204), f(w + 0x208), f(w + 0x20C), f(w + 0x210));
      if (full) Props(base, w, fmt::format("{}.{}", NameOf(base, sc), NameOf(base, w)));
    }
  }
}

bool Command(uint8_t* base, const std::string& cmd) {
  if (cmd == "uiscenes" || cmd == "uiscenes full" || cmd == "uiscenes tree" || cmd == "uiscenes stack")
    return Scenes(base, cmd == "uiscenes full", cmd == "uiscenes tree", nullptr, cmd == "uiscenes stack"), true;
  if (cmd.rfind("traceimage ", 0) == 0) {
    trace_image = cmd.substr(11);
    trace_image_seen.clear();
    return true;
  }
  if (cmd == "renderdiff") return render_diff = true;
  if (cmd == "gfxcapture") return bots::gfx::RequestCapture(), true;
  if (cmd.rfind("hybrid ", 0) == 0) return bots::hybrid::Command(cmd);
  // "mkexec <function>": flag a function of the local players' controller or pawn classes exec (UFunction +0x94
  // FunctionFlags, FUNC_Exec 0x200) so tests can run it from the console, e.g. "p2:GotoStateReviving".
  if (cmd.rfind("mkexec ", 0) == 0) {
    const std::string name = cmd.substr(7);
    for (uint32_t i = 0, n = split_screen::PlayerCount(base); i < n && i < 4; ++i) {
      const uint32_t player = split_screen::Player(base, i);
      const uint32_t controller = Readable(player) ? LoadU32(base, player + 0x40) : 0;
      for (const uint32_t object : {controller, split_screen::PawnOf(base, player)}) {
        const uint32_t fn = Readable(object) ? split_screen::FindFunction(base, object, name.c_str()) : 0;
        if (!fn || std::string_view(split_screen::ClassName(base, fn)) != "Function") continue;
        StoreU32(base, fn + 0x94, LoadU32(base, fn + 0x94) | 0x200);
        REXLOG_INFO("mkexec: {}.{} ({:08X}) is exec", split_screen::ClassName(base, object), name, fn);
      }
    }
    return true;
  }
  if (cmd == "traceeasel" || cmd.rfind("traceeasel ", 0) == 0)
    return trace_easel = cmd.size() > 11 ? std::atoi(cmd.c_str() + 11) : 40, true;
  if (cmd.rfind("words ", 0) == 0) {
    char* end = nullptr;
    const uint32_t addr = static_cast<uint32_t>(std::strtoul(cmd.c_str() + 6, &end, 16));
    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(std::strtoul(end, &end, 0)), 1024);
    std::string tag = end ? end : "";
    tag.erase(0, tag.find_first_not_of(' '));
    for (uint32_t k = 0; k < count && Readable(addr + 4 * k); ++k)
      REXLOG_INFO("{} {:08X} +{:X} = {:08X} ({})", tag, addr + 4 * k, 4 * k, LoadU32(base, addr + 4 * k),
                  menu_pointer::LoadF32(base, addr + 4 * k));
    return true;
  }
  if (cmd.rfind("poke ", 0) == 0) {
    char* end = nullptr;
    const uint32_t addr = static_cast<uint32_t>(std::strtoul(cmd.c_str() + 5, &end, 16));
    const uint32_t value = static_cast<uint32_t>(std::strtoul(end, &end, 16));
    if (!Readable(addr)) {
      REXLOG_INFO("poke: {:08X} not readable", addr);
      return true;
    }
    REXLOG_INFO("poke {:08X}: {:08X} -> {:08X}", addr, LoadU32(base, addr), value);
    StoreU32(base, addr, value);
    return true;
  }
  if (cmd == "splitcams") {
    // Each local player's view: controller ViewTarget, cinematic flags, PlayerCamera fade and its view target / POV.
    auto cls = [&](uint32_t o) {
      return Readable(o) ? menu_pointer::NameOf(base, LoadU32(base, LoadU32(base, o + 0x34) + 0x2C)) : "None";
    };
    auto f32 = [&](uint32_t a) { return menu_pointer::LoadF32(base, a); };
    for (uint32_t i = 0, n = split_screen::PlayerCount(base); i < n && i < 4; ++i) {
      const uint32_t pc = LoadU32(base, split_screen::Player(base, i) + 0x40);
      if (!Readable(pc)) continue;
      auto at = [&](uint32_t o, const char* name) { return split_screen::Prop(base, o, name); };
      const uint32_t vt = LoadU32(base, pc + at(pc, "ViewTarget")), cam = LoadU32(base, pc + at(pc, "PlayerCamera"));
      REXLOG_INFO("splitcams: player {} pc {:08X} {} ViewTarget {:08X} {} cinematic word {:08X}", i, pc, cls(pc), vt,
                  cls(vt), LoadU32(base, pc + at(pc, "bCinematicMode")));
      if (!Readable(cam)) continue;
      const uint32_t v = cam + at(cam, "ViewTarget");
      REXLOG_INFO("splitcams: player {} camera {:08X} {} fading word {:08X} FadeAmount {} FadeColor {:08X} target "
                  "{:08X} {} POV {} {} {} FOV {} aspect {}",
                  i, cam, cls(cam), LoadU32(base, cam + at(cam, "bEnableFading")), f32(cam + at(cam, "FadeAmount")),
                  LoadU32(base, cam + at(cam, "FadeColor")), LoadU32(base, v), cls(LoadU32(base, v)), f32(v + 8),
                  f32(v + 12), f32(v + 16), f32(v + 32), f32(v + 36));
    }
    return true;
  }
  if (cmd == "splitpp share" || cmd == "splitpp own") {
    // LocalPlayer.PlayerPostProcess (+0x78): player 2's own post-process chain, or player 1's.
    static uint32_t own = 0;
    const uint32_t p1 = split_screen::Player(base, 0), p2 = split_screen::Player(base, 1);
    if (!Readable(p1) || !Readable(p2)) {
      REXLOG_INFO("splitpp: no player 2");
      return true;
    }
    if (!own || LoadU32(base, p2 + 0x78) != LoadU32(base, p1 + 0x78)) own = LoadU32(base, p2 + 0x78);
    StoreU32(base, p2 + 0x78, cmd == "splitpp share" ? LoadU32(base, p1 + 0x78) : own);
    REXLOG_INFO("splitpp: player 2's chain {:08X} (player 1's {:08X}, own {:08X})", LoadU32(base, p2 + 0x78),
                LoadU32(base, p1 + 0x78), own);
    return true;
  }
  if (cmd.rfind("uiprops ", 0) == 0) return Scenes(base, false, false, cmd.c_str() + 8), true;
  if (cmd.rfind("props ", 0) == 0) {
    char* end = nullptr;
    const uint32_t object = static_cast<uint32_t>(std::strtoul(cmd.c_str() + 6, &end, 16));
    std::string tag = end ? end : "";
    tag.erase(0, tag.find_first_not_of(' '));
    return Props(base, object, tag), true;
  }
  return false;
}

// The guest call chain from r1 (PPC back chain: the word at a frame's stack pointer is the caller's stack pointer; the
// return address is saved 8 bytes below that). Bounded by the thread's stack (KPCR at r13: +0x70 stack base, the high
// end, +0x74 its low end): the outermost frame's back chain points at the base, and the page above it isn't mapped.
void LogGuestStack(PPCContext& ctx, uint8_t* base, const char* tag, int frames) {
  const uint32_t pcr = ctx.r13.u32;  // outside Readable()'s range
  uint32_t top = pcr ? LoadU32(base, pcr + 0x70) : 0, bottom = pcr ? LoadU32(base, pcr + 0x74) : 0;
  if (!(bottom <= ctx.r1.u32 && ctx.r1.u32 < top)) top = bottom = 0;  // unknown stack: the return address only
  std::string chain = fmt::format(" {:08X}", static_cast<uint32_t>(ctx.lr));
  for (uint32_t sp = ctx.r1.u32, n = 0; n < uint32_t(frames) && sp + 4 <= top; ++n) {
    const uint32_t next = LoadU32(base, sp);
    if (next <= sp || next > top || next - 8 < bottom) break;
    chain += fmt::format(" {:08X}", LoadU32(base, next - 8));
    sp = next;
  }
  REXLOG_INFO("stack {} [{:08X}-{:08X}]:{}", tag, bottom, top, chain);
}
}  // namespace debug

// Debug aid: BOTS_TRACE_UIIMAGE=<widget name> (or the console command "traceimage <name>", which also restarts the
// count) logs the first renders of UIImages of that name (UIImage::Render_Widget, sub_8281D990: r3 the image, r4 the
// canvas) in each scene (UIObject +0x180 OwnerScene), with the guest call chain.
extern "C" void __imp__sub_8281D990(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_8281D990(PPCContext& __restrict ctx, uint8_t* base) {
  if (!debug::trace_image.empty() && debug::trace_image == debug::NameOf(base, ctx.r3.u32)) {
    const uint32_t scene = LoadU32(base, ctx.r3.u32 + 0x180);
    if (debug::trace_image_seen[scene]++ < 4)
      debug::LogGuestStack(ctx, base, fmt::format("{} {:08X} scene {:08X} owner {:08X}", debug::trace_image,
                                                  ctx.r3.u32, scene, LoadU32(base, scene + 0x174)).c_str());
  }
  __imp__sub_8281D990(ctx, base);
}

extern "C" void sub_82E2E860(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2E860(ctx, base);
  bots::instr::HudTick();
  split_screen::Tick(base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) {
    kbm->NoteHudTick();
    kbm->SetDriving(Driving(base));
  }
}

// BOTS_KEY_TEST=<secs>:<key>,d|u[;...] (test only): press / release a keyboard key (kbm binding names: W, Space, LMB,
// ...) at that time, counted from the first frame.
static void KeyTest(bots::MouseKeyboardDriver& kbm, float dt) {
  struct Key {
    float at;
    uint16_t vk;
    bool down;
  };
  static std::vector<Key> plan = [] {
    std::vector<Key> out;
    const char* v = std::getenv("BOTS_KEY_TEST");
    for (const char* p = v ? v : ""; *p;) {
      float at = 0;
      char name[32] = {}, action = 0;
      if (std::sscanf(p, "%f:%31[^,],%c", &at, name, &action) < 3) break;
      out.push_back({at, uint16_t(rex::ui::ParseVirtualKey(name)), action == 'd'});
      p = std::strchr(p, ';');
      if (!p) break;
      ++p;
    }
    if (!out.empty()) REXLOG_INFO("BOTS_KEY_TEST: {} key events", out.size());
    return out;
  }();
  static float clock = 0;
  static size_t current = 0;
  if (current >= plan.size()) return;
  clock += dt;
  for (; current < plan.size() && clock >= plan[current].at; ++current) {
    REXLOG_INFO("BOTS_KEY_TEST: key {:#x} {} at {:.1f} s", plan[current].vk, plan[current].down ? "down" : "up", clock);
    kbm.TestKey(plan[current].vk, plan[current].down);
  }
}

// Vibration setting. The game's two XInputSetState wrappers (user, XINPUT_VIBRATION*) both end in XamInputSetState;
// with vibration off the request is zeroed (so any rumble in progress stops too).
namespace {
void MuteVibration(uint8_t* base, uint32_t vibration) {
  static std::atomic<int> logged{-1};
  const bool on = REXCVAR_GET(vibration);
  if (logged.exchange(int(on)) != int(on)) REXLOG_INFO("vibration: {}", on ? "on" : "off");
  if (!on && vibration) std::memset(Raw(base, vibration), 0, 4);
}
}  // namespace

extern "C" void sub_82445388(PPCContext& __restrict ctx, uint8_t* base) {
  MuteVibration(base, ctx.r4.u32);
  __imp__sub_82445388(ctx, base);
}

extern "C" void sub_82445450(PPCContext& __restrict ctx, uint8_t* base) {
  MuteVibration(base, ctx.r4.u32);
  __imp__sub_82445450(ctx, base);
}

// Controller feel (GitHub #19, #20, #21). Each local player's PlayerController.PlayerInput (a C1PlayerInput) is a
// new object after every chapter load, and its config fields start at the Coalesced.ini values, so the writes are
// redone whenever they read back differently.
//   - bEnableMouseSmoothing is cleared for everyone. UE3's SmoothMouse expects many mouse samples per frame; we send
//     one, and it then scales each frame's movement by dt / (running mean frame time), so the turn wobbles with
//     frame-time noise (measured: ratio sd 0.18; exactly 1.00 with it off).
//   - Stick acceleration off: MaxPowerTurn 0 (no PowerTurn: 2.6x at full tilt) and a huge LagAmount (no easing; see
//     input_math.h for why not 0 or 1).
//   - Stick response linear: TurnPower 1 (yaw is squared as shipped; pitch is already linear).
namespace {
namespace feel {
struct Fields {
  uint32_t smooth_off = 0, smooth_mask = 0, turn_power = 0, max_power_turn = 0, lag_amount = 0;
  bool found = false;
};

float F(uint8_t* base, uint32_t at) { return menu_pointer::LoadF32(base, at); }
void SetF(uint8_t* base, uint32_t at, float v) { StoreU32(base, at, std::bit_cast<uint32_t>(v)); }

void Apply(uint8_t* base, float dt) {
  static const bool trace = [] { const char* v = std::getenv("BOTS_FEEL_LOG"); return v && *v == '1'; }();
  static Fields f;
  static bool have_original = false;
  static float original_turn_power = 0, original_max_power_turn = 0, original_lag_amount = 0;
  static uint32_t logged_pin[4] = {};
  const bool accel = REXCVAR_GET(stick_acceleration);
  const bool linear = REXCVAR_GET(stick_response) == "linear";
  for (uint32_t i = 0, n = std::min<uint32_t>(split_screen::PlayerCount(base), 4); i < n; ++i) {
    const uint32_t player = split_screen::Player(base, i);
    const uint32_t pc = debug::Readable(player) ? LoadU32(base, player + 0x40) : 0;
    if (!debug::Readable(pc)) continue;
    const uint32_t po = split_screen::Prop(base, pc, "PlayerInput");
    if (po == 0xFFFFFFFF) continue;
    const uint32_t pin = LoadU32(base, pc + po);
    if (!debug::Readable(pin)) continue;
    if (!f.found) {
      const uint32_t sm = split_screen::FindFunction(base, pin, "bEnableMouseSmoothing");
      const uint32_t tp = split_screen::Prop(base, pin, "TurnPower");
      const uint32_t mp = split_screen::Prop(base, pin, "MaxPowerTurn");
      const uint32_t la = split_screen::Prop(base, pin, "LagAmount");
      if (!sm || tp == 0xFFFFFFFF || mp == 0xFFFFFFFF || la == 0xFFFFFFFF) continue;  // not a C1PlayerInput (yet)
      // +0x64 UProperty Offset, +0x84 UBoolProperty BitMask.
      f = {LoadU32(base, sm + 0x64), LoadU32(base, sm + 0x84), tp, mp, la, true};
      REXLOG_INFO("controller feel: PlayerInput fields found (smoothing +{:X} mask {:08X}, TurnPower +{:X}, "
                  "MaxPowerTurn +{:X}, LagAmount +{:X})", f.smooth_off, f.smooth_mask, tp, mp, la);
    }
    // The shipped values are the ini's, the same for every instance: remember them before the first write so a
    // setting changed back to the original can be restored.
    if (!have_original) {
      original_turn_power = F(base, pin + f.turn_power);
      original_max_power_turn = F(base, pin + f.max_power_turn);
      original_lag_amount = F(base, pin + f.lag_amount);
      have_original = true;
      REXLOG_INFO("controller feel: shipped TurnPower {}, MaxPowerTurn {}, LagAmount {}", original_turn_power,
                  original_max_power_turn, original_lag_amount);
    }
    if (const uint32_t word = LoadU32(base, pin + f.smooth_off); word & f.smooth_mask)
      StoreU32(base, pin + f.smooth_off, word & ~f.smooth_mask);
    const float want_power = linear ? 1.0f : original_turn_power;
    const float want_max = accel ? original_max_power_turn : 0.0f;
    const float want_lag = accel ? original_lag_amount : bots::input::kNoLagAmount;
    if (F(base, pin + f.turn_power) != want_power) SetF(base, pin + f.turn_power, want_power);
    if (F(base, pin + f.max_power_turn) != want_max) SetF(base, pin + f.max_power_turn, want_max);
    if (F(base, pin + f.lag_amount) != want_lag) SetF(base, pin + f.lag_amount, want_lag);
    if (trace && i == 0) {
      // Per frame: dt, yaw (PlayerController.Rotation.Yaw), then the stick as the script sees it and what it makes.
      const uint32_t ro = split_screen::Prop(base, pc, "Rotation");
      const int32_t yaw = ro != 0xFFFFFFFF ? int32_t(LoadU32(base, pc + ro + 4)) : 0;
      auto prop = [&](const char* name) {
        const uint32_t o = split_screen::Prop(base, pin, name);
        return o != 0xFFFFFFFF ? F(base, pin + o) : -999.f;
      };
      REXLOG_INFO("FEELLOG dt={:.6f} yaw={} raw={:.4f} aTurn={:.4f} powerturn={:.4f} lagprop={:.4f} smooth={}", dt, yaw,
                  prop("RawJoyLookRight"), prop("aTurn"), prop("PowerTurn"), prop("LagProportion"),
                  (LoadU32(base, pin + f.smooth_off) & f.smooth_mask) != 0);
    }
    if (logged_pin[i] != pin) {
      logged_pin[i] = pin;
      REXLOG_INFO("controller feel: player {} PlayerInput {:08X}: mouse smoothing off, stick acceleration {}, stick "
                  "response {}", i, pin, accel ? "on" : "off", linear ? "linear" : "original");
    }
  }
}
}  // namespace feel
}  // namespace

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
  // ProcessInput also runs on a second guest thread during level loads (B-109). Everything below keeps statics or
  // drains the keyboard/mouse driver, so one thread at a time; the other skips the extras for that frame.
  static std::mutex mutex;
  std::unique_lock lock(mutex, std::try_to_lock);
  if (!lock) return;
  MemoryDumpTick(base, dt);
  press_start::Tick(base, dt);
  quit_option::Tick(ctx, base);
  float dx = 0, dy = 0;
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) {
    KeyTest(*kbm, dt);
    kbm->TakeMouseDelta(dx, dy);
    static std::vector<bool> enter;
    kbm->TakeEnterEvents(enter);
    menu_pointer::Tick(ctx, base, *kbm, vp, dt, enter);
    quit_option::TakeEnter(base, enter);
    for (const bool down : enter) InputKey(ctx, base, vp, 0, kKeyEnter, down ? 0 : 1, 1.0f, false);
  }
  if (dx) InputAxis(ctx, base, vp, 0, kKeyMouseX, dx, dt, false);
  if (dy) InputAxis(ctx, base, vp, 0, kKeyMouseY, dy, dt, false);
  feel::Apply(base, dt);
  if (!test.on) return;
  if (test.dx) InputAxis(ctx, base, vp, test.cid, kKeyMouseX, test.dx, dt, false);
  if (test.dy) InputAxis(ctx, base, vp, test.cid, kKeyMouseY, test.dy, dt, false);
}

// BOTS_TRAVEL_TRACE=1 (diagnostics): log the front end's map launch path. C1UIAction_CreateAndLaunchOnlineGame
// LaunchMap (0x82FA6250) -> GameEngine SetClientTravel (0x827CF678, TravelURL +0x46C) -> GameEngine::Tick
// (0x827DB2C0) -> Browse (0x827DA828). A request that never reaches Browse means Tick is stuck.
extern "C" void __imp__sub_82FA6250(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827CF678(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827DA828(PPCContext& __restrict ctx, uint8_t* base);

namespace {
bool TravelTrace() {
  static const bool on = [] {
    const char* v = std::getenv("BOTS_TRAVEL_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

// FString {TCHAR* data, int count, int max}, big-endian UTF-16; ASCII only.
std::string FStr(uint8_t* base, uint32_t address) {
  const uint32_t data = LoadU32(base, address), count = LoadU32(base, address + 4);
  std::string out;
  if (!data || count > 512) return out;
  for (uint32_t i = 0; i + 1 < count; ++i) out += char(Raw(base, data + 2 * i)[1]);
  return out;
}
}  // namespace

extern "C" void sub_82FA6250(PPCContext& __restrict ctx, uint8_t* base) {
  if (TravelTrace()) REXLOG_INFO("travel trace: LaunchMap({:08X})", ctx.r3.u32);
  __imp__sub_82FA6250(ctx, base);
}

extern "C" void sub_827CF678(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t engine = ctx.r3.u32, type = ctx.r5.u32;
  __imp__sub_827CF678(ctx, base);
  if (bots::instr::Enabled()) bots::instr::Travel(FStr(base, engine + 0x46C));
  if (TravelTrace())
    REXLOG_INFO("travel trace: SetClientTravel -> TravelURL '{}' type {}", FStr(base, engine + 0x46C), type);
}

// B-108: session-create failures are silent in the game (C1UIAction_CreateAndLaunchOnlineGame.OnGameCreated(false)
// does nothing with a profile signed in, so New Story just hangs on the launch screen). Log every failure path of
// UOnlineSubsystemLive's create completion, sub_82BF2AD8(this, ?, result, ?): a nonzero incoming result, the LAN
// beacon start (sub_82BE76B0, only when GameSettings bShouldAdvertise) failing, or bIsListPlay (E_NOTIMPL).
extern "C" void __imp__sub_82BF2AD8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82BE76B0(PPCContext& __restrict ctx, uint8_t* base);

extern "C" void sub_82BF2AD8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t result = ctx.r5.u32;
  const uint32_t settings = LoadU32(base, ctx.r3.u32 + 0xC0);
  const uint32_t flags = settings ? LoadU32(base, settings + 0x8C) : 0;
  if (result && result != 997)  // 997 ERROR_IO_PENDING: still running
    REXLOG_ERROR("online: CreateOnlineGame failed, result {:08X} (New Story / co-op launch will not proceed)", result);
  else if (!result && !(flags & 0x80000000u) && (flags & 0x00100000u))
    REXLOG_ERROR("online: CreateOnlineGame failed, bIsListPlay is not implemented (E_NOTIMPL)");
  else if (!result)
    REXLOG_INFO("online: CreateOnlineGame completed (settings flags {:08X})", flags);
  __imp__sub_82BF2AD8(ctx, base);
}

extern "C" void sub_82BE76B0(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82BE76B0(ctx, base);
  if (ctx.r3.u32) REXLOG_ERROR("online: LAN beacon start failed, result {:08X} (session create fails)", ctx.r3.u32);
}

extern "C" void sub_827DA828(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t engine = ctx.r3.u32;
  if (TravelTrace())
    REXLOG_INFO("travel trace: Browse enter, TravelURL '{}' LastURL map '{}'", FStr(base, engine + 0x46C),
                FStr(base, engine + 0x3D8 + 0x1C));
  if (bots::instr::Enabled()) bots::instr::BrowseBegin(FStr(base, engine + 0x46C));
  __imp__sub_827DA828(ctx, base);
  bots::instr::BrowseEnd();
  if (TravelTrace())
    REXLOG_INFO("travel trace: Browse -> {} , TravelURL '{}'", ctx.r3.u32, FStr(base, engine + 0x46C));
}

// Split screen: where each player's UI scenes (the HUD) draw. C1GameUISceneClient's GetViewportSize (sub_828A6C58)
// gives a scene that renders per player (UIScene +0x174 PlayerOwner, +0x1F1 SceneRenderMode 1, SPLITRENDER_PlayerOwner)
// its owner's share of the screen, but GetViewportOrigin (sub_828A6C40) is a stub answering (0, 0), so every player's
// HUD was laid out, and drawn, from the top left. Answering the real origin there doesn't work: the layout code adds it
// more than once along docking chains. So layout keeps origin (0, 0), and while C1GameUISceneClient renders such a
// scene (sub_82F899D8: r3 the client, r4 the canvas, r5 the scene) its widgets' draw rectangles (UIObject +0x204
// RenderBounds[4], +0x214 RenderBoundsVertices[4] Vector2D) move by the owner's LocalPlayer Origin (+0x68) / Size
// (+0x70) times the scene's viewport size (UIScene +0x1F4 CurrentViewportSize), and are put back afterwards.
namespace split_screen {
thread_local float draw_dx = 0, draw_dy = 0;  // the split-screen scene being drawn's shift, (0, 0) outside one
// A face of a widget's Position, moved with its draw rectangle (see Shift).
struct Moved {
  uint32_t address, was, moved;  // the face's bits before and after the move
  float by;
};
// The widget's draw rectangle, saved, then moved. Its Position (UIScreenObject +0x3C UIScreenValue_Bounds: Value[4]
// L T R B, then ScaleType[4] bytes) moves too where it is relative to the viewport or the scene, and the move is taken
// back out afterwards (Unshift) - drawing can write Position from the moved rectangle: the HUD's jackpot counters
// (cash, score) set their digits' Position each frame, which without this drifted 360 px down a frame, off screen.
void Shift(uint8_t* base, uint32_t w, float dx, float dy, float view_w, float view_h,
           std::vector<std::pair<uint32_t, uint32_t>>& saved, std::vector<Moved>& moved) {
  for (uint32_t k = 0; k < 12; ++k) {  // RenderBounds L T R B, then four (x, y) vertices
    const uint32_t a = w + 0x204 + 4 * k;
    const uint32_t bits = LoadU32(base, a);
    saved.emplace_back(a, bits);
    StoreU32(base, a, std::bit_cast<uint32_t>(std::bit_cast<float>(bits) + ((k < 4 ? k : k - 4) % 2 ? dy : dx)));
  }
  for (uint32_t face = 0; face < 4; ++face) {
    const bool y = face % 2;
    const float d = y ? dy : dx, extent = y ? view_h : view_w;
    float by = 0;
    switch (Raw(base, w + 0x4C)[face]) {  // EPositionEvalType
      case 1: case 2: by = d; break;                             // PixelViewport, PixelScene
      case 4: case 6: by = extent > 0 ? d / extent : 0; break;   // PercentageViewport, PercentageScene
      default: break;                                            // owner-relative: the owner moves too
    }
    if (by == 0) continue;
    const uint32_t a = w + 0x3C + 4 * face, was = LoadU32(base, a);
    const uint32_t bits = std::bit_cast<uint32_t>(std::bit_cast<float>(was) + by);
    StoreU32(base, a, bits);
    moved.push_back({a, was, bits, by});
  }
}
void Unshift(uint8_t* base, const std::vector<Moved>& moved) {
  for (const auto& m : moved) {  // untouched faces get their exact bits back (+by -by can round)
    const uint32_t now = LoadU32(base, m.address);
    StoreU32(base, m.address, now == m.moved ? m.was : std::bit_cast<uint32_t>(std::bit_cast<float>(now) - m.by));
  }
}
// Every widget the scene draws, recursively: its Children, and HudWidget_BigNumber's Digits[7] (the cash and score
// digits, which are its Children too in the HUD scenes seen so far).
void Collect(uint8_t* base, uint32_t object, uint32_t children_offset, std::vector<uint32_t>& out, int depth) {
  auto add = [&](uint32_t w) {
    if (!menu_pointer::Readable(w) || std::find(out.begin(), out.end(), w) != out.end()) return;
    out.push_back(w);
    if (depth < 8) Collect(base, w, children_offset, out, depth + 1);
  };
  const uint32_t kids = LoadU32(base, object + children_offset), count = LoadU32(base, object + children_offset + 4);
  for (uint32_t i = 0; i < count && i < 256; ++i) add(LoadU32(base, kids + 4 * i));
  static std::unordered_map<uint32_t, uint32_t> digits_offset;  // by class
  const uint32_t cls = LoadU32(base, object + 0x34);
  auto it = digits_offset.find(cls);
  if (it == digits_offset.end()) it = digits_offset.emplace(cls, Prop(base, object, "Digits")).first;
  if (it->second != 0xFFFFFFFF)
    for (uint32_t d = 0; d < 7; ++d) add(LoadU32(base, object + it->second + 4 * d));
}
}  // namespace split_screen
extern "C" void __imp__sub_82F899D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82F899D8(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool on = bots::SplitScreen();
  const uint32_t scene = ctx.r5.u32;
  float dx = 0, dy = 0;
  if (on && menu_pointer::Readable(scene) && Raw(base, scene + 0x1F1)[0] == 1) {
    if (const uint32_t player = LoadU32(base, scene + 0x174); menu_pointer::Readable(player)) {
      const float sx = menu_pointer::LoadF32(base, player + 0x70), sy = menu_pointer::LoadF32(base, player + 0x74);
      if (sx > 0) dx = menu_pointer::LoadF32(base, player + 0x68) / sx * menu_pointer::LoadF32(base, scene + 0x1F4);
      if (sy > 0) dy = menu_pointer::LoadF32(base, player + 0x6C) / sy * menu_pointer::LoadF32(base, scene + 0x1F8);
    }
  }
  // Debug aid: BOTS_UI_SKIP_PLAYER=<n> (0-3) draws none of local player n's UI scenes, to tell the players' HUDs apart.
  static const int skip = [] {
    const char* v = std::getenv("BOTS_UI_SKIP_PLAYER");
    return v && *v ? std::atoi(v) : -1;
  }();
  if (skip >= 0 && menu_pointer::Readable(scene) && LoadU32(base, scene + 0x174) &&
      LoadU32(base, scene + 0x174) == split_screen::Player(base, uint32_t(skip)))
    return;
  if (dx == 0 && dy == 0) return __imp__sub_82F899D8(ctx, base);
  static uint32_t children_offset = split_screen::Prop(base, scene, "Children");
  static std::vector<uint32_t> widgets;
  static std::vector<std::pair<uint32_t, uint32_t>> saved;
  static std::vector<split_screen::Moved> moved;
  widgets.clear();
  saved.clear();
  moved.clear();
  split_screen::Collect(base, scene, children_offset, widgets, 0);
  const bool diff = std::exchange(debug::render_diff, false);
  constexpr uint32_t kDiffBytes = 0x1600;
  std::vector<uint8_t> before;
  if (diff)
    for (const uint32_t w : widgets) before.insert(before.end(), Raw(base, w), Raw(base, w) + kDiffBytes);
  const float view_w = menu_pointer::LoadF32(base, scene + 0x1F4), view_h = menu_pointer::LoadF32(base, scene + 0x1F8);
  for (const uint32_t w : widgets) split_screen::Shift(base, w, dx, dy, view_w, view_h, saved, moved);
  split_screen::draw_dx = dx;
  split_screen::draw_dy = dy;
  __imp__sub_82F899D8(ctx, base);
  split_screen::draw_dx = split_screen::draw_dy = 0;
  for (const auto& [a, bits] : saved) StoreU32(base, a, bits);
  split_screen::Unshift(base, moved);
  if (diff) {
    int logged = 0;
    for (size_t i = 0; i < widgets.size(); ++i)
      for (uint32_t o = 0; o < kDiffBytes; o += 4) {
        uint32_t was;
        std::memcpy(&was, &before[i * kDiffBytes + o], 4);
        was = __builtin_bswap32(was);
        if (const uint32_t now = LoadU32(base, widgets[i] + o); now != was && logged++ < 200)
          REXLOG_INFO("renderdiff {:08X} {} +{:X}: {:08X} ({}) -> {:08X} ({})", widgets[i],
                      debug::NameOf(base, widgets[i]), o, was, std::bit_cast<float>(was), now,
                      std::bit_cast<float>(now));
      }
    REXLOG_INFO("renderdiff: scene {:08X} shift {} {}, {} widgets, {} words changed", scene, dx, dy, widgets.size(),
                logged);
  }
}

// Split screen: easels (C1UI_Easel / SfUI_Easel: button bars, also inside dialogs) draw in their post-render
// (sub_82DDAE08: r3 the widget, r4 the FCanvas), and the jitter labels (HudWidget_JitterForegroundLabel: the pause
// menu's header and options) in their Render_Widget (sub_82F05818; the dark copies behind them, HudWidget_JitterLabel,
// sub_82F05158). Both hand the FCanvas helpers (DrawString
// sub_82790FA8, DrawTile sub_82790150: f1 X, f2 Y) positions they worked out themselves from the layout, not the
// RenderBounds the scene draw moved, so player 2's prompts and pause menu drew over player 1's half. Inside a
// split-screen scene draw, those helpers add the scene's shift for them. (The easel's UCanvas origin, +64 OrgX / +68
// OrgY from GetPosition(face, EVALPOS_PixelViewport), sub_82844190, isn't it: the positions are already absolute.)
namespace split_screen {
thread_local uint32_t easel = 0;  // the easel drawing, 0 if none
thread_local bool tracing_easel = false;
thread_local bool own_positions = false;      // a widget drawing from positions of its own (jitter labels)
void EaselOrigin(PPCContext& ctx, uint8_t* base) {
  if (!easel && !own_positions) return;
  if (tracing_easel)
    REXLOG_INFO("traceeasel:   draw at {} {} (f3 {} f4 {}: DrawTile size, DrawString scale) + {} {}", ctx.f1.f64,
                ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, draw_dx, draw_dy);
  ctx.f1.f64 = double(float(ctx.f1.f64) + draw_dx);
  ctx.f2.f64 = double(float(ctx.f2.f64) + draw_dy);
}
}  // namespace split_screen
extern "C" void __imp__sub_82790FA8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82790FA8(PPCContext& __restrict ctx, uint8_t* base) {
  split_screen::EaselOrigin(ctx, base);
  __imp__sub_82790FA8(ctx, base);
}
extern "C" void __imp__sub_82790150(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82790150(PPCContext& __restrict ctx, uint8_t* base) {
  split_screen::EaselOrigin(ctx, base);
  __imp__sub_82790150(ctx, base);
}
extern "C" void __imp__sub_82F05818(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82F05818(PPCContext& __restrict ctx, uint8_t* base) {
  const bool was = std::exchange(split_screen::own_positions, true);
  __imp__sub_82F05818(ctx, base);
  split_screen::own_positions = was;
}
// HudWidget_JitterLabel's (MenuJitterLabel: the dark copy behind each pause menu label) Render_Widget, the same way.
extern "C" void __imp__sub_82F05158(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82F05158(PPCContext& __restrict ctx, uint8_t* base) {
  const bool was = std::exchange(split_screen::own_positions, true);
  __imp__sub_82F05158(ctx, base);
  split_screen::own_positions = was;
}
extern "C" void __imp__sub_82844190(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82844190(PPCContext& __restrict ctx, uint8_t* base) {
  if (!split_screen::tracing_easel || ctx.lr < 0x82DDAE08 || ctx.lr >= 0x82DDB0A0)
    return __imp__sub_82844190(ctx, base);
  const uint32_t face = ctx.r4.u8;
  __imp__sub_82844190(ctx, base);
  REXLOG_INFO("traceeasel:   face {}: {}", face, ctx.f1.f64);
}
// Ultrawide (B-164): the Music Player (C1UI_MusicPlayer, front end and pause menu) lays itself out in 720-line pixels,
// so on a shorter back buffer (548 lines at 21:9) its bottom, the button bar included, was cut off. While it draws,
// the FCanvas's top transform is scaled by GuestHeight / 720 about the screen's middle, so the whole panel (frame,
// lists, text, prompts) shows at its own proportions; music_player::FitPointer maps the pointer back for its hit
// tests. FCanvas +0x24 TransformStack: TArray of {FMatrix Matrix (row vectors); DWORD MatrixCRC}, 0x50 bytes each.
// Draws are batched under the top entry and a new batch starts when its CRC changes, so the CRC changes with it.
namespace easel_fit {
constexpr uint32_t kTransformStack = 0x24, kEntry = 0x50;  // FMatrix is 16-byte aligned: 0x44 padded

// Scales the top transform of FCanvas `canvas` by `fit`; returns the entry (0 if none) and its old words in `saved`.
uint32_t Apply(uint8_t* base, uint32_t canvas, float fit, uint32_t (&saved)[17]) {
  const uint32_t data = LoadU32(base, canvas + kTransformStack);
  const uint32_t count = LoadU32(base, canvas + kTransformStack + 4);
  if (!menu_pointer::Readable(data) || count == 0 || count > 64) return 0;
  const uint32_t top = data + (count - 1) * kEntry;
  for (uint32_t i = 0; i < 17; ++i) saved[i] = LoadU32(base, top + 4 * i);
  float m[16];
  for (uint32_t i = 0; i < 16; ++i) m[i] = std::bit_cast<float>(saved[i]);
  // S * M with S = rows (fit,0,0,0) (0,fit,0,0) (0,0,1,0) (640 (1 - fit),0,0,1): x' = 640 + (x - 640) fit, y' = y fit.
  const float dx = 640.0f * (1.0f - fit);
  for (uint32_t c = 0; c < 4; ++c) {
    const float row0 = m[c], row1 = m[4 + c];
    m[12 + c] += dx * row0;
    m[c] = fit * row0;
    m[4 + c] = fit * row1;
  }
  for (uint32_t i = 0; i < 16; ++i) StoreU32(base, top + 4 * i, std::bit_cast<uint32_t>(m[i]));
  StoreU32(base, top + 64, saved[16] ^ 0xB1640000u);
  return top;
}

void Restore(uint8_t* base, uint32_t top, const uint32_t (&saved)[17]) {
  for (uint32_t i = 0; i < 17; ++i) StoreU32(base, top + 4 * i, saved[i]);
}
}  // namespace easel_fit

extern "C" void __imp__sub_82DDAE08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82DDAE08(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t w = ctx.r3.u32;
  const bool trace = debug::trace_easel > 0;
  if (trace) {
    --debug::trace_easel;
    REXLOG_INFO("traceeasel: {:08X} {} ({}) full viewport {}", w, debug::NameOf(base, w),
                debug::NameOf(base, LoadU32(base, w + 0x34)), LoadU32(base, w + 0x410) >> 31);
  }
  const uint32_t outer = std::exchange(split_screen::easel, w);
  const bool was = std::exchange(split_screen::tracing_easel, trace);
  const float fit = menu_pointer::music_player::Fit(base, w);
  uint32_t saved[17];
  const uint32_t fitted = fit != 1 ? easel_fit::Apply(base, ctx.r4.u32, fit, saved) : 0;
  __imp__sub_82DDAE08(ctx, base);
  if (fitted) easel_fit::Restore(base, fitted, saved);
  split_screen::tracing_easel = was;
  split_screen::easel = outer;
}

// ULocalPlayer::Exec (sub_825D67A8: r3 the player's FExec at LocalPlayer +60, r4 the command, UTF-16): every console
// command a player runs, the UI's included (menus run execs such as QuitToMainMenu as their player). Logged, apart
// from our own (RunPlayerCommand logs those), as a map of what the menus do. With split screen, player 2 quitting from
// their pause menu would end the game for both (QuitToMainMenu ends with "Disconnect"); they leave it instead.
namespace split_screen {
bool running_ours = false;  // set by RunPlayerCommand around its exec
}
extern "C" void __imp__sub_825D67A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_825D67A8(PPCContext& __restrict ctx, uint8_t* base) {
  if (split_screen::running_ours) return __imp__sub_825D67A8(ctx, base);
  std::string cmd;
  for (uint32_t a = ctx.r4.u32; menu_pointer::Readable(a & ~3u) && cmd.size() < 200; a += 2) {
    const uint16_t c = uint16_t(Raw(base, a)[0] << 8 | Raw(base, a)[1]);
    if (!c) break;
    cmd.push_back(c < 0x80 ? char(c) : '?');
  }
  const uint32_t player = ctx.r3.u32 - 60;
  uint32_t index = 0;
  while (index < 4 && split_screen::Player(base, index) != player) ++index;
  REXLOG_INFO("exec (player {}): {}", index, cmd);
  static const bool split = bots::SplitScreen();
  auto lower = [](std::string t) {
    for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
    return t;
  };
  if (split && index >= 1 && index < 4 && lower(cmd).rfind("quittomainmenu", 0) == 0) {
    split_screen::join.leave = true;
    ctx.r3.u64 = 1;  // handled
    return;
  }
  __imp__sub_825D67A8(ctx, base);
}

// Split screen: the final post-process pass (sub_82D7E850: r3 the effect, r5 the view) sets the back buffer as its
// target and clears it to black, once per view - stock UE3 clears only for the first view (bDeferClear), but the game
// never shipped split screen, so the second view's clear wiped the first's finished image. Only the view at the top
// left (origin 0,0, which every layout has; view +0x34 X, +0x38 Y) clears now. And with two players nobody does: the
// back buffer shares EDRAM with the scene colour, and the first view's clear wiped the second view's scene alpha (its
// scene depth) before that view's final pass read it, so player 2's view came out fogged blue, more so the farther
// apart the players were (seen on real pads; bots-replay with BOTS_REPLAY_SKIP on the clear draw). Two views fill the
// screen between them, so nothing needs the clear then.
namespace split_screen {
thread_local uint32_t final_pass_view = 0;
}
extern "C" void __imp__sub_82D7E850(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82D7E850(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t saved = split_screen::final_pass_view;
  split_screen::final_pass_view = ctx.r5.u32;
  __imp__sub_82D7E850(ctx, base);
  split_screen::final_pass_view = saved;
}
extern "C" void __imp__sub_827BBC10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827BBC10(PPCContext& __restrict ctx, uint8_t* base) {
  if (const uint32_t view = split_screen::final_pass_view;
      view && ctx.lr == 0x82D7EEC0 &&
      (LoadU32(base, view + 0x34) || LoadU32(base, view + 0x38) || split_screen::PlayerCount(base) > 1)) {
    return;
  }
  __imp__sub_827BBC10(ctx, base);
}

// Split screen: the final pass's clamp rect for its blur samples (RenderTargetClampParameter, a pixel-shader float4
// set through sub_827BCF50: r6 bytes, r7 the values) is (X - 0.5, Y - 0.5, X + SizeX - 0.5, Y + SizeY - 0.5) over the
// buffer size, so a view's min edge is the centre of the last row (or column) of the view before it. The back buffer
// shares EDRAM with the scene colour, so by the second view's pass that row holds the first view's finished 8888
// image read as HDR colour: while the camera moved, motion blur at the second view's top edge clamped onto it and
// drew a green line between the views (seen on real pads; bots-replay of a capture, its c6 is 359.5/720 for player 2).
// Move a min edge that isn't the screen's one texel into the view. Only a vector that matches the view (view +0x34
// X, +0x38 Y, +0x3C SizeX, +0x40 SizeY, ints) is touched.
extern "C" void __imp__sub_827BCF50(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827BCF50(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t view = split_screen::final_pass_view, values = ctx.r7.u32;
  if (view && ctx.r6.u32 == 16 && values >= 0x40000000u && values < 0xA0000000u && !(values & 3) &&
      split_screen::PlayerCount(base) > 1) {
    auto load = [&](uint32_t a) { const uint32_t v = LoadU32(base, a); float f; std::memcpy(&f, &v, 4); return f; };
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = load(values + 4 * i);
    float size[2];  // buffer width, height
    bool match = true;
    for (int a = 0; a < 2 && match; ++a) {
      const float origin = float(int32_t(LoadU32(base, view + 0x34 + 4 * a)));
      const float extent = float(int32_t(LoadU32(base, view + 0x3C + 4 * a)));
      size[a] = v[a] != 0.0f ? (origin - 0.5f) / v[a] : 0.0f;
      match = extent > 0.0f && size[a] >= origin + extent - 0.5f && size[a] < 16384.0f &&
              std::fabs(v[a + 2] * size[a] - (origin + extent - 0.5f)) < 0.01f;
    }
    if (match) {
      bool moved = false;
      for (int a = 0; a < 2; ++a) {
        const float origin = float(int32_t(LoadU32(base, view + 0x34 + 4 * a)));
        if (origin <= 0.0f) continue;
        moved = true;
        const float f = (origin + 0.5f) / size[a];
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        StoreU32(base, values + 4 * a, bits);
      }
      static std::atomic<int> logged{0};
      if (moved && logged.fetch_add(1) < 2) {
        REXLOG_INFO("split screen: view at {},{} keeps its blur inside it (clamp {} {} {} {}, buffer {}x{})",
                    int32_t(LoadU32(base, view + 0x34)), int32_t(LoadU32(base, view + 0x38)), v[0], v[1], v[2], v[3],
                    size[0], size[1]);
      }
    }
  }
  __imp__sub_827BCF50(ctx, base);
}
