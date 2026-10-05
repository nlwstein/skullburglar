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
  uint32_t option = 0, toggle = 0, slider = 0, image = 0, left = 0, right = 0;
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
                                                      {"Left", &n.left},              {"Right", &n.right}};
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

// Per frame. `enter` holds the click/confirm Enter edges for this frame; the ones that land on a toggle or slider are
// taken out of it and become Left/Right taps.
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
    if (!Readable(focus) || !LoadU32(base, scene + 0x98)) continue;
    const uint32_t f = LoadU32(base, focus);
    if (!Readable(f)) continue;
    if (KindOf(base, n, f) == Kind::kNone) {
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

void Tick(PPCContext& ctx, uint8_t* base, bots::MouseKeyboardDriver& kbm, uint32_t vp, float dt, std::vector<bool>& enter) {
  static uint32_t seen_moves = 0, dragging = 0;
  static bool click_taken = false;  // the current click went to a toggle/slider: its release is ours too
  const Names* n = GetNames(base);
  uint32_t focused = 0;
  const uint32_t scene = n ? MenuScene(base, *n, focused) : 0;
  kbm.SetMenuScene(scene != 0);
  float px, py;
  uint32_t moves;
  if (!TestPointer(dt, px, py, moves, enter) && !kbm.Pointer(px, py, moves)) {
    dragging = 0;
    return;
  }
  if (!n) return;
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
  if (moves != seen_moves) {
    seen_moves = moves;
    if (hovered && hovered->object != focused && !dragging) Focus(ctx, base, hovered->object);
  }

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
  // ProcessInput also runs on a second guest thread during level loads (B-109). Everything below keeps statics or
  // drains the keyboard/mouse driver, so one thread at a time; the other skips the extras for that frame.
  static std::mutex mutex;
  std::unique_lock lock(mutex, std::try_to_lock);
  if (!lock) return;
  MemoryDumpTick(base, dt);
  press_start::Tick(base, dt);
  float dx = 0, dy = 0;
  if (auto* kbm = bots::MouseKeyboardDriver::instance()) {
    kbm->TakeMouseDelta(dx, dy);
    static std::vector<bool> enter;
    kbm->TakeEnterEvents(enter);
    menu_pointer::Tick(ctx, base, *kbm, vp, dt, enter);
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
