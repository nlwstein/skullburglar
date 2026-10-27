// Level-start streaming prefetch (B-226) and a level-streaming trace.
//
// The engine's UWorld::UpdateLevelStreaming (sub_82647688) ends with
//   if (a streaming level has a load request pending && !(WorldInfo bit +540 & 0x08000000 && TimeSeconds >= 1.0))
//     UObject::FlushAsyncLoading();
// so a level that Kismet streams in during the first second of play is loaded synchronously, in one game frame. Every
// level start does this: the persistent map's Kismet streams the player's kill-move levels (GEN_KillMove_*) and
// GEN_DeathAnimations a few frames into play (world time 0.38-0.57 s), after the loading screen is gone. Loading those
// 4-5 packages takes one 55-60 ms game frame on an M4 and 200-290 ms on a 15 W laptop. The original game runs the same
// code, so it has the same stall.
//
// The prefetch moves that load behind the loading screen without changing what the game does with it. While the
// loading screen is up, the map's own streaming requests are flushed in two batches; with the first request of the
// second batch we also request (LoadPackageAsync, no callback) the packages that earlier level starts streamed in
// during their first second of play and that are streaming levels of this world too. They load in the same flush.
// When Kismet asks for them, UpdateLevelStreaming finds the package and its world already in memory (its own "already
// loaded" path, used e.g. for a level that is streamed out and back in) and links them without a load or a flush.
// Visibility, Kismet outputs and timing in world time are unchanged; only the wall-clock moment the package's objects
// are created moves earlier.
//
// What a level start streams depends on the profile (the equipped kill moves), so the list is learned: a package that
// forced an in-play flush is added to <user data root>/levelstart_prefetch.txt and is prefetched from the next level
// start on (the first level start after a change still stalls once).
//
// Switches: BOTS_LEVELSTART_PREFETCH=0 turns the prefetch off (learning too). BOTS_STREAM_LOG=1 logs every
// LoadPackageAsync and every FlushAsyncLoading (duration, packages, world time); =2 also lists the world's streaming
// levels at each flush and once per map at world time 2 s (prefetch on and off give the same list).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

extern "C" void __imp__sub_825A4DB0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_825A4068(PPCContext& __restrict ctx, uint8_t* base);

namespace bots::levelstream {
namespace {

constexpr uint32_t kGWorld = 0x8381D238;          // UWorld*
constexpr uint32_t kNames = 0x837F98A0;           // FName::Names (TArray<FNameEntry*>)
constexpr uint32_t kAsyncPackages = 0x8380B18C;   // UObject::AsyncPackages (TArray<FAsyncPackage*>)
constexpr uint32_t kStreamingCallback = 0x82647568;  // UpdateLevelStreaming's AsyncLevelLoadCompletionCallback
constexpr uint32_t kLoadMapCallback = 0x827D32E8;    // the map load's completion callback
constexpr uint32_t kLoadMapFlushReturn = 0x82ECE72C;  // LoadMap's FlushAsyncLoading call returns here
constexpr uint32_t kStreamingFlushReturn = 0x82647D84;  // UpdateLevelStreaming's FlushAsyncLoading call returns here

uint8_t* Raw(uint8_t* base, uint32_t a) { return base + (uint64_t(a) + (a >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(base, a), 4);
  return __builtin_bswap32(v);
}
float F32(uint8_t* base, uint32_t a) {
  const uint32_t v = U32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

int LogLevel() {
  static const int v = [] {
    const char* e = std::getenv("BOTS_STREAM_LOG");
    return e ? std::atoi(e) : 0;
  }();
  return v;
}
bool PrefetchOn() {
  static const bool on = [] {
    const char* e = std::getenv("BOTS_LEVELSTART_PREFETCH");
    return !(e && *e == '0');
  }();
  return on;
}

std::string FString(uint8_t* base, uint32_t fstr) {
  std::string out;
  if (!fstr) return out;
  const uint32_t data = U32(base, fstr), num = U32(base, fstr + 4);
  for (uint32_t i = 0; data && i < num && i < 256; ++i) {
    const uint8_t* c = Raw(base, data + 2 * i);
    const uint16_t ch = uint16_t(c[0] << 8 | c[1]);
    if (!ch) break;
    out.push_back(ch < 0x80 ? char(ch) : '?');
  }
  return out;
}

std::string Name(uint8_t* base, uint32_t index, uint32_t number = 0) {
  const uint32_t names = U32(base, kNames);
  if (!names || index >= U32(base, kNames + 4)) return "?";
  const uint32_t entry = U32(base, names + 4 * index);
  std::string s = entry ? reinterpret_cast<const char*>(Raw(base, entry + 8)) : "?";
  if (number) s += "_" + std::to_string(number - 1);
  return s;
}

// WorldInfo = GWorld->PersistentLevel->Actors(0), as UpdateLevelStreaming reads it.
uint32_t WorldInfo(uint8_t* base) {
  const uint32_t world = U32(base, kGWorld);
  const uint32_t level = world ? U32(base, world + 80) : 0;
  if (!level || !U32(base, level + 64)) return 0;
  return U32(base, U32(base, level + 60));
}

struct WorldClock {
  bool valid = false, begun = false;
  float t = 0;
};
WorldClock Clock(uint8_t* base) {
  WorldClock c;
  if (const uint32_t info = WorldInfo(base)) {
    c.valid = true;
    c.t = F32(base, info + 844);
    c.begun = (U32(base, info + 540) & 0x08000000u) != 0;
  }
  return c;
}

// Package names of WorldInfo.StreamingLevels (+760 data, +764 count); ULevelStreaming: +60 PackageName (FName),
// +68 LoadedLevel, +96 flags.
std::vector<std::string> StreamingLevels(uint8_t* base, std::string* dump = nullptr) {
  std::vector<std::string> out;
  const uint32_t info = WorldInfo(base);
  if (!info) return out;
  const uint32_t data = U32(base, info + 760), n = U32(base, info + 764);
  for (uint32_t i = 0; data && i < n && i < 512; ++i) {
    const uint32_t ls = U32(base, data + 4 * i);
    if (!ls) continue;
    out.push_back(Name(base, U32(base, ls + 60), U32(base, ls + 64)));
    if (dump) {
      const uint32_t cls = U32(base, ls + 0x34);
      char buf[64];
      std::snprintf(buf, sizeof buf, " loaded=%d flags=%08X]", U32(base, ls + 68) ? 1 : 0, U32(base, ls + 96));
      *dump += " [" + (cls ? Name(base, U32(base, cls + 0x2C)) : std::string("?")) + " " + out.back() + buf;
    }
  }
  return out;
}

// State; the game thread is the only caller of both hooks.
std::filesystem::path g_file;
std::set<std::string> g_learned;
// Where the current map's start is. Seen on every level start: LoadMap's flush, then UpdateLevelStreaming's loading-
// screen batches (two: the first is flushed inside the map load, which then collects garbage; the second is flushed
// on the next tick), all at the same world time, then play starts and world time runs.
enum class Phase { kIdle, kMapLoaded, kFirstBatchFlushed, kDone };
Phase g_phase = Phase::kIdle;
uint32_t g_world = 0;   // the UWorld LoadMap made
float g_arm_time = 0;   // its world time while the loading screen is up

void Learn(const std::string& pkg) {
  if (!g_learned.insert(pkg).second) return;
  REXLOG_INFO("levelstart prefetch: learned {} (it was streamed in during the first second of play)", pkg);
  if (g_file.empty()) return;
  std::ofstream f(g_file, std::ios::app);
  if (f) f << pkg << "\n";
}

// LoadPackageAsync(const FString& name, callback = 0, userdata = 0) on a frame carved below the guest stack.
void Request(PPCContext& ctx, uint8_t* base, const std::string& name) {
  const PPCContext saved = ctx;
  const uint32_t sp = (saved.r1.u32 - 0x2000) & ~0xFu;  // callee frames go below sp; our buffers sit above it
  const uint32_t fstr = sp + 0x800, chars = sp + 0x810;
  const uint32_t n = uint32_t(std::min<size_t>(name.size(), 400));
  for (uint32_t i = 0; i <= n; ++i) {
    const uint16_t ch = i < n ? uint8_t(name[i]) : 0;
    uint8_t* c = Raw(base, chars + 2 * i);
    c[0] = uint8_t(ch >> 8), c[1] = uint8_t(ch);
  }
  const uint32_t words[3] = {__builtin_bswap32(chars), __builtin_bswap32(n + 1), __builtin_bswap32(n + 1)};
  std::memcpy(Raw(base, fstr), words, sizeof words);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = fstr;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  __imp__sub_825A4DB0(ctx, base);
  ctx = saved;
}

}  // namespace

void Start(const std::filesystem::path& user_root) {
  if (!PrefetchOn() || user_root.empty()) return;
  g_file = user_root / "levelstart_prefetch.txt";
  std::ifstream f(g_file);
  for (std::string line; std::getline(f, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) g_learned.insert(line);
  }
  REXLOG_INFO("levelstart prefetch: on, {} learned packages", g_learned.size());
}

// BOTS_STREAM_LOG=2: once per map, when its world time passes 2 s, list its streaming levels (loaded state and flags),
// to check that a prefetch leaves them as they'd be without it.
void Tick(uint8_t* base) {
  if (LogLevel() < 2) return;
  static bool dumped = false;
  const WorldClock c = Clock(base);
  if (!c.valid) return;
  if (c.t < 1.0f) dumped = false;  // a new map
  if (dumped || c.t < 2.0f) return;
  dumped = true;
  std::string dump;
  const size_t n = StreamingLevels(base, &dump).size();
  REXLOG_INFO("stream: at world t={:.3f}, {} streaming levels:{}", c.t, n, dump);
}

}  // namespace bots::levelstream

// UObject::LoadPackageAsync(const FString& PackageName, CompletionCallback, CallbackUserData)
extern "C" void sub_825A4DB0(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace bots::levelstream;
  const uint32_t callback = ctx.r4.u32;
  const std::string name = (LogLevel() || PrefetchOn()) ? FString(base, ctx.r3.u32) : std::string();
  const WorldClock clock = Clock(base);
  if (LogLevel())
    REXLOG_INFO("stream: LoadPackageAsync {} (callback {:08X}, world t={:.3f} begun={})", name, callback, clock.t,
                clock.begun ? 1 : 0);
  if (!PrefetchOn()) return __imp__sub_825A4DB0(ctx, base);
  if (callback == kLoadMapCallback) g_phase = Phase::kIdle, g_world = 0;  // a new map: its LoadMap flush re-arms
  const bool streaming = callback == kStreamingCallback && clock.valid && U32(base, kGWorld) == g_world;
  const bool loading = streaming && clock.t <= g_arm_time + 1e-4f;
  // A request this map's UpdateLevelStreaming will flush during play: learn it.
  if (streaming && g_phase != Phase::kIdle && !loading && clock.t < 1.0f) Learn(name);
  if (streaming && !loading && g_phase != Phase::kIdle) g_phase = Phase::kDone;
  __imp__sub_825A4DB0(ctx, base);
  if (!loading || g_phase != Phase::kFirstBatchFlushed) return;
  // The first request of the last loading-screen batch: prefetch the learned packages that are streaming levels of this
  // world, so the flush that follows loads them too. (Requested with the first batch they don't last: the map load
  // collects garbage after that batch, and nothing references them yet.)
  g_phase = Phase::kDone;
  const std::vector<std::string> levels = StreamingLevels(base);
  int n = 0;
  for (const std::string& pkg : g_learned) {
    if (pkg == name || std::find(levels.begin(), levels.end(), pkg) == levels.end()) continue;
    Request(ctx, base, pkg);
    ++n;
    if (LogLevel()) REXLOG_INFO("stream: prefetch {}", pkg);
  }
  if (n) REXLOG_INFO("levelstart prefetch: requested {} packages behind the loading screen", n);
}

// UObject::FlushAsyncLoading()
extern "C" void sub_825A4068(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace bots::levelstream;
  const uint32_t lr = uint32_t(ctx.lr);
  if (!LogLevel() && !PrefetchOn()) return __imp__sub_825A4068(ctx, base);
  const uint32_t pending = U32(base, kAsyncPackages + 4);
  const WorldClock before = Clock(base);
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_825A4068(ctx, base);
  if (PrefetchOn() && lr == kLoadMapFlushReturn) {
    // The map is loaded; the loading screen is still up and world time stands still until play starts.
    g_phase = Phase::kMapLoaded;
    g_world = U32(base, kGWorld);
    g_arm_time = Clock(base).t;
  } else if (PrefetchOn() && lr == kStreamingFlushReturn && g_phase == Phase::kMapLoaded &&
             U32(base, kGWorld) == g_world && before.t <= g_arm_time + 1e-4f) {
    g_phase = Phase::kFirstBatchFlushed;
  }
  if (!LogLevel()) return;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!pending && ms < 1.0) return;
  if (LogLevel() >= 2) {
    std::string dump;
    const size_t n = StreamingLevels(base, &dump).size();
    REXLOG_INFO("stream: {} streaming levels:{}", n, dump);
  }
  REXLOG_INFO("stream: FlushAsyncLoading {:.1f} ms, {} async packages, from {:08X}, world t={:.3f} begun={}", ms,
              pending, lr, before.t, before.begun ? 1 : 0);
}
