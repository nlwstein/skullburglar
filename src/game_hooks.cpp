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
#include <cstring>
#include <vector>
#include <cmath>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "app_quit.h"
#include "mnk_input.h"
#include "prompt_state.h"

extern "C" void __imp__sub_82E1AC28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823598E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_823501E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82BC86F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2EB38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2EBB8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82E2E860(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_828291E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82853F50(PPCContext& __restrict ctx, uint8_t* base);

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
  // "peek <hex addr> [count]": log guest words (pointers the game fills in at runtime).
  if (cmd.rfind("peek ", 0) == 0) {
    char* end = nullptr;
    uint32_t addr = static_cast<uint32_t>(std::strtoul(cmd.c_str() + 5, &end, 16)) & ~3u;
    int count = std::max(1, std::min(64, static_cast<int>(std::strtol(end, nullptr, 10))));
    for (int i = 0; i < count; ++i, addr += 4) REXLOG_INFO("BOTS_EXEC peek {:08X} = {:08X}", addr, LoadU32(base, addr));
    return;
  }
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
  if (state && ctx.r3.u32 == 0) quit_option::FilterPad(base, state);
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
// button" (title screen). While keyboard/mouse prompts are shown it reads "Press ENTER" (kbm_confirm). The config
// cache's value and any labels already built from it are FStrings {data, count, max}; the new text is shorter, so it
// is written into the same buffers (and back for a controller). The guest heap is rescanned only when the prompt
// device changes; labels built later copy the config value, which is already right.
namespace press_start {
constexpr char16_t kPad[] = u"Press START button", kKeyboard[] = u"Press ENTER";
constexpr uint32_t kPadCount = std::size(kPad), kKeyboardCount = std::size(kKeyboard);  // FString count: with the null

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

void Rewrite(uint8_t* base, bool keyboard) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const auto pad = Utf16Be(kPad, kPadCount), key = Utf16Be(kKeyboard, kKeyboardCount);
  const std::vector<uint8_t>& from = keyboard ? pad : key;
  const std::vector<uint8_t>& to = keyboard ? key : pad;
  const uint32_t from_count = keyboard ? kPadCount : kKeyboardCount, to_count = keyboard ? kKeyboardCount : kPadCount;
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
  REXLOG_INFO("prompts: PressStart text -> '{}' ({} strings, {:.0f} ms)", keyboard ? "Press ENTER" : "Press START button",
              strings, ms);
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
  static bool rewritten = false;
  if (!state) return;
  const uint32_t generation = state->generation.load(std::memory_order_relaxed);
  if (generation == seen) return;
  seen = generation;
  const bool keyboard = state->keyboard_mouse.load(std::memory_order_relaxed) != 0;
  if (!keyboard && !rewritten) return;
  Rewrite(base, keyboard);
  rewritten = true;
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

// BOTS_POINTER_TEST=<secs>:<x>,<y>[,d|u][;...] (test only): put the pointer at UI pixel x,y (1280x720) from that
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
  py = plan[current - 1].y / 720.0f;
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
  pointer_x = px * 1280.0f, pointer_y = py * 720.0f;
  if (!n) return;
  if (focused && IsA(base, focused, n->music_player)) {
    dragging = 0;
    music_player::Tick(base, kbm, focused, pointer_x, pointer_y, dt, moves != seen_moves, enter);
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
  // Window -> UI pixels (the UI is laid out at 1280x720 and scaled to the window).
  const float x = px * 1280.0f, y = py * 720.0f;
  const Widget* hovered = nullptr;
  for (const auto& w : widgets) {
    // Rows: the label's box is narrow, so accept the whole row height left of the right edge plus a margin.
    if (y >= w.top - 4 && y <= w.bottom + 4 && x >= w.left - 200 && x <= w.right + 200) hovered = &w;
  }
  // SetFocus can refuse (seen once while the main menu was opening), so keep asking for up to a second after a move
  // while the pointer stays on that widget. Not longer: pad/keyboard navigation must be able to move focus away.
  static uint32_t want = 0;
  static float want_age = 0;
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
// wrap-around, labelled "Quit". Choosing it (A / START, Enter, a click) quits the app; those presses are kept from the
// game, since the copy also carries its archetype's actions.
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
  SetLabel(ctx, base, LoadU32(base, widget + kMyJitter), "Quit");
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
// reads "Quit" with keyboard and mouse, where clicking it quits (the pointer is free there), and "Press B to quit" with a
// pad. B quits on either (on the keyboard, the keys bound to B). Off while the attract movie plays over the screen
// (C1TitleScreen +0x260 bInMovie), where B stops the movie as before.
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
  static int shown = -1;     // the text it shows: 1 keyboard/mouse, 0 pad
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
  const int keyboard = prompts && prompts->keyboard_mouse.load(std::memory_order_relaxed) ? 1 : 0;
  if (keyboard != shown) {
    shown = keyboard;
    SetLabel(ctx, base, label, keyboard ? "Quit" : "Press B to quit");
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
  static std::atomic<bool> held{true};  // a press that began before Quit was there doesn't count
  const uint16_t mask = focused ? (kA | kStart) : title::active ? kB : 0;
  if (!mask) {
    held = true;
    return;
  }
  uint8_t* s = Raw(base, state);
  const uint16_t buttons = uint16_t(s[4] << 8 | s[5]);
  const bool pressed = buttons & mask;
  if (pressed && !held) {
    REXLOG_INFO("{}: Quit chosen (controller)", focused ? "main menu" : "press start");
    bots::RequestQuit();
  }
  held = pressed;
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
}

extern "C" void sub_82E2EBB8(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2EBB8(ctx, base);
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) kbm->SetPaused(false);
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
  return Readable(pawn) && menu_pointer::IsA(base, pawn, n->driving_vehicle);
}

extern "C" void sub_82E2E860(PPCContext& __restrict ctx, uint8_t* base) {
  __imp__sub_82E2E860(ctx, base);
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
  __imp__sub_827DA828(ctx, base);
  if (TravelTrace())
    REXLOG_INFO("travel trace: Browse -> {} , TravelURL '{}'", ctx.r3.u32, FStr(base, engine + 0x46C));
}
