// Test instrumentation (BOTS_INSTR): a machine-readable status stream and a control channel, so unattended tests,
// benchmarks and external agents can see what is on screen, press buttons and wait for states instead of timers.
// Switches are in instr.h. Off by default: with BOTS_INSTR unset nothing here runs, and every hook is one branch.
//
// What it costs when on: the game thread samples guest memory every 200 ms (every frame while a macro is steering
// the pad, every 250 ms with BOTS_INSTR_WORLD); the GPU thread adds one histogram increment per frame; a status
// thread builds the status line at BOTS_INSTR_HZ; a log sink matches a few substrings per log line.
//
// Control channel: TCP, one text command per line, one JSON line back per command (plus pushed lines after `watch`).
//   status                     the status line now (schema below)
//   watch [hz]                 push status lines at hz (default BOTS_INSTR_HZ) and every event; `watch 0` stops
//   hist                       the cumulative frame-time histogram (0.5 ms buckets): take two and subtract
//   press BTN[+BTN..] [ms]     press and release (default 150 ms; at least 3 game polls); returns once released
//   hold BTN[+BTN..] | release BTN[+BTN..]|all     LT / RT here pull the trigger fully
//   stick L|R x y [ms]         x, y in -1..1 (y up); without ms it stays until changed
//   trigger L|R v [ms]         v in 0..1
//   pad <hex buttons> [ms]
//   exec <console command>     queued for the game thread, as BOTS_EXEC (LoadChapter, God, hybrid ...)
//   wait [-t secs] <cond>      returns once the condition holds (default timeout 60 s). <cond>: clauses over the flat
//                              status keys (`keys` lists them) joined by && and ||, ops == != >= <= > < ~= (contains)
//                              e.g. `wait -t 300 phase==in_play && game.levels~=Ambush`
//   keys                       the flat status keys and their values
//   shot <path.png>            the presented frame as PNG, plus <path.png>.json (the status line at that moment)
//   burst <dir> <count>        <count> consecutive presented frames (native renderer; PPM + TSV, as BOTS_CAPTURE_BURST)
//   dualshot <tag>             hybrid dual render: NNNN-<tag>.port.png / .n1.png in BOTS_HYBRID_DUAL_DIR (needs
//                              BOTS_HYBRID_DUAL=1, which changes what is drawn: not for benchmarks)
//   macro move_to x y [z] [ms] [radius] | path_to x y z|objective|hotspot [ms] [radius] [exact] | aim_at x y z [ms]
//         | aim_nearest [ms] | turn <yaw deg> [ms] | fire [ms] | stop
//                              high-level actions steered every frame from the player state (see docs). path_to plans
//                              a route over the game's collision (a grid search, a few ms per frame), then walks it,
//                              pressing B when it stands still (weak planks) and replanning around what blocks it. An
//                              unreachable goal walks to the nearest reachable point (`found` with partial, ends
//                              `near`) unless `exact`. `path` events: planning, found, smash, replan, arrived, near,
//                              no_path, stuck, stopped, timeout
//   enemies [n]                nearest pawns now (position, distance, bearing, health, class, foe, line of sight)
//   trace x1 y1 z1 x2 y2 z2 [extent] | floor x y [z]    one world collision trace / the floor under a point
//   pathdrop [uu] | pathflags [N]                   path_to's deepest planned drop (260) / its trace flags
//   inspect pawn|weapon|pc|world|game|camera|engine|0xADDR [filter]   an object's properties (discovery aid)
//   objs CLASS [n] | elems 0xOBJ PROP [stride] [n]      live objects of a class / an array's elements
//   log [n] [filter]           the last log lines
//   quit                       quit the game cleanly;  close: close this connection;  help
//
// Status line (type "status", v 1): t (s since start), pid, phase (boot, precompile, loading, hung, dialog, paused,
// movie, cutscene, front_end, in_play, dead, menu), phase_s, game {..., objective {...}, hotspot {...}, prompt {...}}, precompile
// {...}, render {..., n1 {...}}, health {...}, input {..., human, path {...}}, cpu [...]. Events (type "event"): phase
// changes, loads, LoadChapter, presenter failures, hitches, builds in play, errors, macro ends, path progress,
// human_input (keyboard / mouse events the channel didn't send), captures. docs: the project's instrumentation notes.
#include "instr.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <array>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <spdlog/sinks/base_sink.h>

#include "app_quit.h"
#include "instr_state.h"
#include "mnk_input.h"
#include "thread_stats.h"

// UWorld::SingleLineCheck(FCheckResult& Hit, AActor* Source, const FVector& End, const FVector& Start, DWORD
// TraceFlags, const FVector& Extent, ...): the collision query behind Actor.FastTrace / Trace (called from
// AActor::execFastTrace at 0x82665AF4). Game thread only.
extern "C" void sub_82677C00(PPCContext& __restrict ctx, uint8_t* base);

namespace bots::instr {
using rex::X_RESULT;
using rex::X_STATUS;
namespace {

constexpr double kPi = 3.14159265358979323846;
uint64_t NowUs() { return InstrState::NowUs(); }
const uint64_t g_start_us = NowUs();
double Secs(uint64_t us) { return double(int64_t(us - g_start_us)) / 1e6; }

// ---------------------------------------------------------------------------------------------------------------
// Switches

struct Config {
  bool on = false;
  int port = 47100;
  std::string bind = "127.0.0.1";
  std::string log;
  double hz = 2;
  bool world = false;
  double cpu_secs = 5;
  std::vector<std::string> dialogs;  // scene class/tag substrings that count as a dialog
};

const Config& Cfg() {
  static const Config c = [] {
    Config c;
    const char* v = std::getenv("BOTS_INSTR");
    c.on = v && *v && std::strcmp(v, "0") != 0;
    if (!c.on) return c;
    if (int p = std::atoi(v); p > 1) c.port = p;
    if (const char* b = std::getenv("BOTS_INSTR_BIND"); b && *b) c.bind = b;
    if (const char* l = std::getenv("BOTS_INSTR_LOG"); l && *l) c.log = l;
    if (const char* h = std::getenv("BOTS_INSTR_HZ"); h && *h) c.hz = std::clamp(std::atof(h), 0.1, 30.0);
    if (const char* w = std::getenv("BOTS_INSTR_WORLD"); w && *w && *w != '0') c.world = true;
    if (const char* s = std::getenv("BOTS_INSTR_CPU"); s && *s) c.cpu_secs = std::max(0.0, std::atof(s));
    std::string list = "MessageBox,Dialog,ErrorMessage,Popup,Confirm,Prompt,Warning";
    if (const char* d = std::getenv("BOTS_INSTR_DIALOGS"); d && *d) list = d;
    std::stringstream ss(list);
    for (std::string item; std::getline(ss, item, ',');)
      if (!item.empty()) c.dialogs.push_back(item);
    return c;
  }();
  return c;
}

const bool g_enabled = Cfg().on;

// The renderer plugin's counters (instr_state.h), published before any plugin loads.
InstrState g_state;
const bool g_state_published = [] {
  if (!Cfg().on) return false;
  char value[32];
  std::snprintf(value, sizeof(value), "%llx", static_cast<unsigned long long>(uintptr_t(&g_state)));
#ifdef _WIN32
  SetEnvironmentVariableA(kInstrStateEnv, value);
#else
  setenv(kInstrStateEnv, value, 1);
#endif
  return true;
}();

// ---------------------------------------------------------------------------------------------------------------
// Guest memory (game thread only). UObject +0x28 Outer, +0x2C Name, +0x30 Number, +0x34 Class; UStruct +0x3C
// SuperStruct, +0x50 Children; UField +0x40 Next; UProperty +0x64 Offset; UBoolProperty +0x84 BitMask.

constexpr uint32_t kGEngine = 0x8380D380;
constexpr uint32_t kNames = 0x837F98A0;  // FName::Names: TArray<FNameEntry*>, name text at entry +8

uint8_t* g_base = nullptr;

uint8_t* Raw(uint32_t a) { return g_base + (uint64_t(a) + (a >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Raw(a), 4);
  return __builtin_bswap32(v);
}
float F32(uint32_t a) {
  const uint32_t v = U32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
bool Readable(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u && !(a & 3); }

const char* NameText(uint32_t index) {
  const uint32_t names = U32(kNames);
  if (!Readable(names) || index >= U32(kNames + 4)) return "?";
  const uint32_t entry = U32(names + 4 * index);
  return Readable(entry) ? reinterpret_cast<const char*>(Raw(entry + 8)) : "?";
}

std::string ObjName(uint32_t o) {
  if (!Readable(o)) return "None";
  std::string out = NameText(U32(o + 0x2C));
  if (const uint32_t number = U32(o + 0x30)) out += "_" + std::to_string(number - 1);
  return out;
}

const char* ClassName(uint32_t o) {
  const uint32_t cls = Readable(o) ? U32(o + 0x34) : 0;
  return Readable(cls) ? NameText(U32(cls + 0x2C)) : "None";
}

bool ClassChainHas(uint32_t o, std::string_view part) {
  uint32_t cls = Readable(o) ? U32(o + 0x34) : 0;
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    if (std::string_view(NameText(U32(cls + 0x2C))).find(part) != std::string_view::npos) return true;
  return false;
}

uint32_t Outermost(uint32_t o) {
  for (int n = 0; Readable(o) && Readable(U32(o + 0x28)) && n < 16; ++n) o = U32(o + 0x28);
  return o;
}

struct Field {
  uint32_t offset = 0xFFFFFFFF, mask = 0, prop = 0;
  bool ok() const { return offset != 0xFFFFFFFF; }
};

Field FindProp(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + 0x50), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) {
        Field out{U32(f + 0x64), 0, f};
        if (!std::strcmp(ClassName(f), "BoolProperty")) out.mask = U32(f + 0x84);
        return out;
      }
  return {};
}

// By (class, name): property offsets never change while the class exists.
Field Prop(uint32_t object, const char* name) {
  static std::map<std::pair<uint32_t, std::string>, Field> cache;
  if (!Readable(object)) return {};
  const uint32_t cls = U32(object + 0x34);
  if (!Readable(cls)) return {};
  auto key = std::make_pair(cls, std::string(name));
  auto it = cache.find(key);
  if (it == cache.end()) it = cache.emplace(key, FindProp(cls, name)).first;
  return it->second;
}

uint32_t PropObj(uint32_t o, const char* name) {
  const Field f = Prop(o, name);
  const uint32_t v = f.ok() ? U32(o + f.offset) : 0;
  return Readable(v) ? v : 0;
}
bool PropInt(uint32_t o, const char* name, int32_t& out) {
  const Field f = Prop(o, name);
  if (!f.ok()) return false;
  out = int32_t(U32(o + f.offset));
  return true;
}
bool PropFloat(uint32_t o, const char* name, float& out) {
  const Field f = Prop(o, name);
  if (!f.ok()) return false;
  out = F32(o + f.offset);
  return true;
}
bool PropBool(uint32_t o, const char* name) {
  const Field f = Prop(o, name);
  return f.ok() && f.mask && (U32(o + f.offset) & f.mask);
}
bool PropVec(uint32_t o, const char* name, float* v) {
  const Field f = Prop(o, name);
  if (!f.ok()) return false;
  for (int i = 0; i < 3; ++i) v[i] = F32(o + f.offset + 4 * i);
  return true;
}
// A member of a struct type (UStructProperty +0x84 is the struct), e.g. C1SeqVar_Objective.Objective.WaypointActor.
Field StructMember(uint32_t object, const char* struct_prop, const char* member) {
  const Field s = Prop(object, struct_prop);
  if (!s.ok() || std::strcmp(ClassName(s.prop), "StructProperty")) return {};
  static std::map<std::pair<uint32_t, std::string>, Field> cache;
  const uint32_t st = U32(s.prop + 0x84);
  auto key = std::make_pair(st, std::string(member));
  auto it = cache.find(key);
  if (it == cache.end()) it = cache.emplace(key, FindProp(st, member)).first;
  Field f = it->second;
  if (f.ok()) f.offset += s.offset;
  return f;
}
// An FString (TArray<TCHAR>, big-endian UTF-16) at `at`, as ASCII-ish text.
std::string FStr(uint32_t at, size_t max = 120) {
  const uint32_t data = U32(at), count = U32(at + 4);
  std::string s;
  for (uint32_t i = 0; Readable(data & ~3u) && i + 1 < count && i < max; ++i) {
    const uint8_t* c = Raw(data + 2 * i);
    s += c[0] ? '?' : char(c[1]);
  }
  return s;
}
void W32(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(a), &v, 4);
}
void WF32(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  W32(a, v);
}

// ---------------------------------------------------------------------------------------------------------------
// Collision traces: a call into the game's own UWorld::SingleLineCheck, on the game thread, from inside the input
// poll hook whose guest context GameTick receives. The call runs on a frame carved below the current guest stack
// pointer, and the whole context is restored afterwards.

constexpr uint32_t kGWorld = 0x8381D238;  // GWorld (UWorld*), as AActor::execFastTrace loads it
// FastTrace's flags: movers, volumes, level geometry, stop at any hit, terrain; no pawns.
constexpr uint32_t kTraceWorld = 0x2286;

PPCContext* g_ctx = nullptr;  // set while GameTick runs (game thread)
std::atomic<uint64_t> g_traces{0};

struct Hit {
  bool hit = false;
  float loc[3] = {}, normal[3] = {}, time = 1;
  uint32_t actor = 0;
};

// False when no trace could be made (not on the game thread inside GameTick, or no world).
bool Trace(const float start[3], const float end[3], const float extent[3], uint32_t source, Hit& out,
           uint32_t flags = kTraceWorld) {
  if (!g_ctx || !g_base) return false;
  const uint32_t world = U32(kGWorld);
  if (!Readable(world)) return false;
  PPCContext& ctx = *g_ctx;
  const PPCContext saved = ctx;
  const uint32_t sp = (saved.r1.u32 - 0x1000) & ~0xFu;  // callee frames go below sp; our buffers sit above it
  const uint32_t hit = sp + 0x400, v_end = sp + 0x480, v_start = sp + 0x490, v_ext = sp + 0x4A0;
  for (uint32_t i = 0; i < 0x80; i += 4) W32(hit + i, 0);
  WF32(hit + 0x20, 1.0f);  // Time
  W32(hit + 0x24, 0xFFFFFFFFu);  // Item
  W32(hit + 0x34, 0xFFFFFFFFu);
  W32(hit + 0x50, 0xFFFFFFFFu);
  for (int i = 0; i < 3; ++i)
    WF32(v_end + 4 * i, end[i]), WF32(v_start + 4 * i, start[i]), WF32(v_ext + 4 * i, extent ? extent[i] : 0.0f);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = world;
  ctx.r4.u64 = hit;
  ctx.r5.u64 = source;
  ctx.r6.u64 = v_end;
  ctx.r7.u64 = v_start;
  ctx.r8.u64 = flags;
  ctx.r9.u64 = v_ext;
  ctx.r10.u64 = 0;
  sub_82677C00(ctx, g_base);
  ++g_traces;
  out.actor = U32(hit + 4);
  out.hit = out.actor != 0;
  for (int i = 0; i < 3; ++i) out.loc[i] = F32(hit + 8 + 4 * i), out.normal[i] = F32(hit + 0x14 + 4 * i);
  out.time = F32(hit + 0x20);
  ctx = saved;
  return true;
}

// The floor under (x, y), searching from z_from down to z_to. NaN when there is none (or no trace could be made).
float Ground(float x, float y, float z_from, float z_to, uint32_t source) {
  const float a[3] = {x, y, z_from}, b[3] = {x, y, z_to};
  Hit h;
  if (!Trace(a, b, nullptr, source, h) || !h.hit) return std::nanf("");
  return h.loc[2];
}

// ---------------------------------------------------------------------------------------------------------------
// The game-thread snapshot

struct Enemy {
  float pos[3];
  float dist, bearing, pitch;  // bearing: degrees from the camera's view, + right; pitch: + up
  int32_t health;
  std::string cls;
  bool player;
  int team;  // 1 the player's team, 0 another team (enemy), -1 no team found
  bool foe;  // worth shooting: another team, or no team but a turret / insurgent (team -1 is mostly scenery)
  int los = -1;  // line of sight from the camera: 1 clear, 0 blocked, -1 not checked
};

// Team -1 pawns are mostly scenery (helicopter drop-offs, props with health); turrets and insurgents are the
// exceptions seen so far.
bool IsFoe(const Enemy& e) {
  if (e.player || e.team == 1) return false;
  return e.team == 0 || e.cls.find("Turret") != std::string::npos || e.cls.find("Insurgent") != std::string::npos;
}

struct Objective {
  bool present = false;
  float pos[3] = {};
  std::string label, actor, source;  // source: list (ObjectiveList) or marker (ObjectiveMarkers only)
  int priority = 0, state = 0, open = 0;  // EObjectivePriority (1 primary), EObjectiveState (1 incomplete)
};

// The nearest G-Unit hot spot (GUnitMarker): the glowing spots where the Action button (A) climbs down, kicks a door,
// boosts a partner... `kind` is its material without "_MIC" (DropDown, ...). Level progress often needs one.
struct Hotspot {
  bool present = false;
  float pos[3] = {};
  std::string actor, kind;
  int count = 0;  // activatable hot spots in the world
  struct Near {
    float pos[3], d2;
    std::string actor, kind;
  };
  std::vector<Near> nearest;  // the 4 nearest, nearest first (when one does nothing, try the next)
};

struct Snap {
  uint64_t at_us = 0;
  std::string map, game_class;
  float world_time = 0;
  bool world_paused = false, cinematic = false, title = false, title_movie = false;
  bool has_pc = false, has_pawn = false;
  float pos[3] = {}, vel[3] = {};
  int32_t rot[3] = {};
  int32_t health = 0, health_max = 0, ammo = -1;
  std::string pawn_class, weapon, ammo_prop;
  bool cam_ok = false;
  float cam[3] = {}, fov = 0;
  int32_t cam_rot[3] = {};
  std::vector<std::string> scenes;  // bottom to top: Class or Class/Tag
  bool enemies_valid = false;
  std::vector<Enemy> enemies;  // nearest first
  int pawns = 0;
  std::string levels;            // visible streaming levels
  int levels_pending = 0;        // streaming levels asked for but not loaded / shown yet
  int objectives = 0;
  bool checkpoint_pending = false;
  Objective objective;
  Hotspot hotspot;
  int32_t clip = -1, clip_max = -1, reserve = -1;  // the primary fire mode's AmmoInClip / MaxAmmoInClip; carried ammo
  bool dead = false;                                // the death screen is up (C1PlayerController.DeathScreenActive)
  bool prompt_counterkill = false, prompt_tutorial = false;  // the counter-kill button prompt / a tutorial box
};

std::mutex g_snap_mutex;
Snap g_snap;

double UnrealDeg(int32_t r) {
  double d = double(r & 0xFFFF) * 360.0 / 65536.0;
  return d > 180 ? d - 360 : d;
}
double Wrap180(double d) {
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
}

void ReadScenes(Snap& s) {
  const uint32_t engine = U32(kGEngine);
  const uint32_t viewport = Readable(engine) ? U32(engine + 0x2E4) : 0;
  const uint32_t ui = Readable(viewport) ? U32(viewport + 0x5C) : 0;
  const uint32_t client = Readable(ui) ? U32(ui + 0x78) : 0;
  if (!Readable(client)) return;
  const uint32_t scenes = U32(client + 0x100);
  const int32_t count = int32_t(U32(client + 0x104));
  for (int32_t i = 0; Readable(scenes) && i < count && i < 64; ++i) {
    const uint32_t scene = U32(scenes + 4 * i);
    if (!Readable(scene)) continue;
    std::string name = ClassName(scene);
    const Field tag = Prop(scene, "SceneTag");
    if (tag.ok()) {
      const std::string t = NameText(U32(scene + tag.offset));
      if (t != "None" && t != name) name += "/" + t;
    }
    if (!std::strcmp(ClassName(scene), "C1TitleScreen")) {
      s.title = true;
      s.title_movie = U32(scene + 0x260) & 0x80000000u;
    }
    s.scenes.push_back(std::move(name));
  }
}

// A pawn's TeamInfo: its PlayerReplicationInfo's Team, or its controller's.
uint32_t TeamOf(uint32_t pawn) {
  uint32_t team = PropObj(PropObj(pawn, "PlayerReplicationInfo"), "Team");
  if (!team) team = PropObj(PropObj(PropObj(pawn, "Controller"), "PlayerReplicationInfo"), "Team");
  return team;
}

void ReadEnemies(Snap& s, uint32_t world_info, uint32_t own_pawn) {
  s.enemies_valid = true;
  const uint32_t own_team = own_pawn ? TeamOf(own_pawn) : 0;
  const uint32_t own_driver = own_pawn ? PropObj(own_pawn, "Driver") : 0;
  const float* from = s.cam_ok ? s.cam : s.pos;
  const double yaw = s.cam_ok ? UnrealDeg(s.cam_rot[1]) : UnrealDeg(s.rot[1]);
  const double pitch = s.cam_ok ? UnrealDeg(s.cam_rot[0]) : 0;
  uint32_t p = PropObj(world_info, "PawnList");
  for (int n = 0; p && n < 512; ++n, p = PropObj(p, "NextPawn")) {
    ++s.pawns;
    if (p == own_pawn || p == own_driver) continue;
    Enemy e{};
    if (!PropVec(p, "Location", e.pos)) continue;
    PropInt(p, "Health", e.health);
    if (e.health <= 0) continue;
    const float dx = e.pos[0] - from[0], dy = e.pos[1] - from[1], dz = e.pos[2] - from[2];
    const double flat = std::sqrt(double(dx) * dx + double(dy) * dy);
    e.dist = float(std::sqrt(flat * flat + double(dz) * dz));
    e.bearing = float(Wrap180(std::atan2(dy, dx) * 180 / kPi - yaw));
    e.pitch = float(std::atan2(dz, flat) * 180 / kPi - pitch);
    e.cls = ClassName(p);
    const uint32_t controller = PropObj(p, "Controller");
    e.player = controller && ClassChainHas(controller, "PlayerController");
    const uint32_t team = TeamOf(p);
    e.team = !team ? -1 : team == own_team ? 1 : 0;
    e.foe = IsFoe(e);
    s.enemies.push_back(std::move(e));
  }
  std::sort(s.enemies.begin(), s.enemies.end(), [](const Enemy& a, const Enemy& b) { return a.dist < b.dist; });
  if (s.enemies.size() > 16) s.enemies.resize(16);
  // Line of sight from the camera to the nearest few foes (one world trace each; pawns don't block).
  int checked = 0;
  for (auto& e : s.enemies) {
    if (!e.foe || checked == 4) continue;
    ++checked;
    Hit h;
    if (Trace(from, e.pos, nullptr, own_pawn, h)) e.los = h.hit ? 0 : 1;
  }
}

// The objective the HUD's marker points at: the first incomplete entry of C1PlayerController.ObjectiveList with a
// waypoint (primary first), else the first ObjectiveMarkers actor. The list stays empty until the level's script adds
// one (in 11010: after the counter-kill tutorial).
void ReadObjective(Snap& s, uint32_t pc) {
  Objective& ob = s.objective;
  if (const Field list = Prop(pc, "ObjectiveList"); list.ok()) {
    const uint32_t data = U32(pc + list.offset), count = U32(pc + list.offset + 4);
    int best = 1 << 20;
    for (uint32_t i = 0; Readable(data) && i < count && i < 64; ++i) {
      const uint32_t o = U32(data + 4 * i);
      const Field st = Prop(o, "ObjectiveState");
      if (!Readable(o) || !st.ok()) continue;
      const int state = *Raw(o + st.offset);
      if (state != 1) continue;  // OBJECTIVE_Incomplete
      ++ob.open;
      const uint32_t var = PropObj(o, "ObjectiveVar");
      if (!var) continue;
      const Field wa = StructMember(var, "Objective", "WaypointActor");
      const Field pr = StructMember(var, "Objective", "ObjectivePriority");
      const uint32_t actor = wa.ok() ? U32(var + wa.offset) : 0;
      const int prio = pr.ok() ? *Raw(var + pr.offset) : 0;
      float pos[3] = {};
      const bool located = Readable(actor) && PropVec(actor, "Location", pos);
      const int rank = (located ? 0 : 100) + (prio == 1 ? 0 : 10 + prio) + int(i);
      if (rank >= best) continue;
      best = rank;
      ob.present = located;
      std::memcpy(ob.pos, pos, sizeof(pos));
      ob.priority = prio, ob.state = state;
      ob.actor = located ? ObjName(actor) : "";
      ob.source = "list";
      if (const Field d = StructMember(var, "Objective", "Description"); d.ok()) ob.label = FStr(var + d.offset);
    }
  }
  if (ob.present) return;
  if (const Field m = Prop(pc, "ObjectiveMarkers"); m.ok() && U32(pc + m.offset + 4)) {
    const uint32_t actor = U32(U32(pc + m.offset));  // HighlightedObject {Actor Actor; HighlightInfo Info}
    if (Readable(actor) && PropVec(actor, "Location", ob.pos)) ob.present = true, ob.actor = ObjName(actor), ob.source = "marker";
  }
}

template <typename Fn>
void ForEachObject(const char* cls, Fn&& fn);

uint32_t ObjectTable();
// <o> is still the live object at object-table position <index>. Reads only the table, never <o>: an actor kept from
// an earlier sample may have been freed by a level unload into memory that is no longer mapped.
bool AliveAt(uint32_t o, uint32_t index) {
  const uint32_t t = ObjectTable();
  return t && index < U32(t + 4) && U32(U32(t) + 4 * index) == o;
}

// GUnitMarker actors are found by a walk of the object table (a few ms), so the list is cached and refreshed every
// 5 s or on a map change; the per-sample work is only reading the cached actors. A streamed level can unload its
// markers between scans with the map name unchanged, so each is checked by its object-table slot before it is read.
void ReadHotspot(Snap& s) {
  struct Marker {
    uint32_t actor, index;
  };
  static std::vector<Marker> markers;
  static uint64_t next_scan = 0;
  static std::string scanned_map;
  if (s.at_us >= next_scan || s.map != scanned_map) {
    markers.clear();
    ForEachObject("GUnitMarker", [](uint32_t o) {
      const std::string n = ObjName(o);
      if (n.rfind("Default__", 0) != 0 && n.find("Arc") == std::string::npos) markers.push_back({o, U32(o + 4)});
    });
    next_scan = s.at_us + 5000000;
    scanned_map = s.map;
  }
  Hotspot& h = s.hotspot;
  float best = 0;
  for (const auto [o, index] : markers) {
    float p[3];
    if (!AliveAt(o, index) || std::strcmp(ClassName(o), "GUnitMarker") || !PropBool(o, "bIsActivatable") || PropBool(o, "bHidden") ||
        PropBool(o, "bDestroyed") || !PropVec(o, "Location", p))
      continue;
    ++h.count;
    const float dx = p[0] - s.pos[0], dy = p[1] - s.pos[1], dz = p[2] - s.pos[2];
    const float d = dx * dx + dy * dy + dz * dz;
    std::string kind = ObjName(PropObj(o, "SharedMaterialInstance_base"));
    if (const size_t k = kind.rfind("_MIC"); k != std::string::npos) kind.resize(k);
    h.nearest.push_back({{p[0], p[1], p[2]}, d, ObjName(o), kind});
    if (h.present && d >= best) continue;
    h.present = true, best = d;
    std::memcpy(h.pos, p, sizeof(p));
    h.actor = ObjName(o);
    h.kind = kind;
  }
  std::sort(h.nearest.begin(), h.nearest.end(), [](const auto& a, const auto& b) { return a.d2 < b.d2; });
  if (h.nearest.size() > 4) h.nearest.resize(4);
}

uint32_t LocalController() {
  const uint32_t engine = U32(kGEngine);
  if (!Readable(engine)) return 0;
  const uint32_t players = PropObj(engine, "GamePlayers");
  const Field count = Prop(engine, "GamePlayers");
  if (!players || !count.ok() || !U32(engine + count.offset + 4)) return 0;
  const uint32_t lp = U32(players);
  return Readable(lp) ? PropObj(lp, "Actor") : 0;
}

void Sample(Snap& s, bool enemies) {
  s.at_us = NowUs();
  ReadScenes(s);
  const uint32_t pc = LocalController();
  if (!pc) return;
  s.has_pc = true;
  const uint32_t world_info = PropObj(pc, "WorldInfo");
  if (world_info) {
    s.map = ObjName(Outermost(world_info));
    PropFloat(world_info, "TimeSeconds", s.world_time);
    s.world_paused = PropObj(world_info, "Pauser") != 0;
    s.game_class = ClassName(PropObj(world_info, "Game"));
    const Field levels = Prop(world_info, "StreamingLevels");
    const uint32_t data = levels.ok() ? U32(world_info + levels.offset) : 0;
    const uint32_t count = levels.ok() ? U32(world_info + levels.offset + 4) : 0;
    for (uint32_t i = 0; Readable(data) && i < count && i < 256; ++i) {
      const uint32_t ls = U32(data + 4 * i);
      if (!Readable(ls)) continue;
      const bool visible = PropBool(ls, "bIsVisible"), loaded = PropObj(ls, "LoadedLevel") != 0;
      if ((PropBool(ls, "bShouldBeLoaded") && !loaded) || (PropBool(ls, "bShouldBeVisible") && !visible) ||
          PropBool(ls, "bHasLoadRequestPending"))
        ++s.levels_pending;
      if (visible) {
        const Field name = Prop(ls, "PackageName");
        if (name.ok()) s.levels += (s.levels.empty() ? "" : ",") + std::string(NameText(U32(ls + name.offset)));
      }
    }
  }
  s.checkpoint_pending = PropBool(pc, "pending_load_checkpoint");
  ReadObjective(s, pc);
  s.dead = PropBool(pc, "DeathScreenActive");
  if (const uint32_t hud = PropObj(pc, "myHUD")) {
    float opacity = 0;
    const uint32_t button = PropObj(hud, "CounterKillButton");
    s.prompt_counterkill = button && !PropBool(button, "bHidden") && PropFloat(button, "Opacity", opacity) && opacity > 0.05f;
    // The tutorial box (HudWidget_Tutorial) fades in and out by Opacity; its bActive stays false.
    const uint32_t tut = PropObj(hud, "TutorialWindow");
    opacity = 0;
    s.prompt_tutorial = tut && !PropBool(tut, "bHidden") && PropFloat(tut, "Opacity", opacity) && opacity > 0.05f;
  }
  if (const Field list = Prop(pc, "ObjectiveList"); list.ok()) s.objectives = int32_t(U32(pc + list.offset + 4));
  s.cinematic = PropBool(pc, "bCinematicMode");
  if (const uint32_t camera = PropObj(pc, "PlayerCamera")) {
    const Field cache = Prop(camera, "CameraCache");
    if (cache.ok()) {
      const uint32_t pov = camera + cache.offset + 4;  // TCameraCache {float TimeStamp; TPOV POV}
      for (int i = 0; i < 3; ++i) s.cam[i] = F32(pov + 4 * i), s.cam_rot[i] = int32_t(U32(pov + 12 + 4 * i));
      s.fov = F32(pov + 24);
      s.cam_ok = true;
    }
  }
  const uint32_t pawn = PropObj(pc, "Pawn");
  if (pawn) {
    s.has_pawn = true;
    s.pawn_class = ClassName(pawn);
    PropVec(pawn, "Location", s.pos);
    PropVec(pawn, "Velocity", s.vel);
    if (const Field r = Prop(pawn, "Rotation"); r.ok())
      for (int i = 0; i < 3; ++i) s.rot[i] = int32_t(U32(pawn + r.offset + 4 * i));
    PropInt(pawn, "Health", s.health);
    if (!PropInt(pawn, "HealthMax", s.health_max)) PropInt(pawn, "MaxHealth", s.health_max);
    if (const uint32_t weapon = PropObj(pawn, "Weapon")) {
      s.weapon = ClassName(weapon);
      static const char* kAmmo[] = {"firemode_ammocount", "AmmoCount", "CurrentAmmo", "AmmoInClip", "ClipAmmo", "Ammo"};
      for (const char* name : kAmmo)
        if (PropInt(weapon, name, s.ammo)) {
          s.ammo_prop = name;
          break;
        }
      // Clip and carried ammo: the primary fire mode's AmmoInClip, and the inventory items of its AmmoType class
      // (CurrentAmount). The HUD shows them as "clip|reserve".
      const Field modes = Prop(weapon, "FireModeList");
      const uint32_t mode = modes.ok() && U32(weapon + modes.offset + 4) ? U32(U32(weapon + modes.offset)) : 0;
      if (Readable(mode) && PropInt(mode, "AmmoInClip", s.clip)) {
        PropInt(mode, "MaxAmmoInClip", s.clip_max);
        if (const uint32_t ammo_class = PropObj(mode, "AmmoType")) {
          s.reserve = 0;
          uint32_t item = PropObj(PropObj(pawn, "InvManager"), "InventoryChain");
          for (int n = 0; item && n < 64; ++n, item = PropObj(item, "Inventory")) {
            int32_t amount = 0;
            if (U32(item + 0x34) == ammo_class && PropInt(item, "CurrentAmount", amount)) s.reserve += amount;
          }
        }
      }
    }
  }
  if (enemies && world_info) ReadEnemies(s, world_info, pawn);
  if (enemies && pawn) ReadHotspot(s);
}

// ---------------------------------------------------------------------------------------------------------------
// Shared counters (any thread)

struct Counters {
  std::atomic<uint64_t> ticks{0}, last_tick_us{0}, last_hud_us{0};
  std::atomic<bool> paused{false}, in_browse{false};
  std::atomic<uint64_t> browse_start_us{0}, loads{0};
  std::atomic<uint64_t> presenter_failures{0}, hitches{0}, builds_in_play{0}, builds_logged{0}, errors{0},
      warnings{0};
  std::atomic<uint64_t> audio_frames{0}, audio_silent{0}, audio_underruns{0};
  std::atomic<double> guest_fps{-1};
  std::atomic<bool> macro_active{false};
  std::atomic<bool> in_play{false};  // the last computed phase was in_play (for the log sink's counters)
  std::atomic<bool> port_warm_done{false}, n1_warm_done{false};  // their "done" log lines were seen
  std::mutex text_mutex;
  std::string chapter, pending_chapter, last_url, last_exec;
  std::atomic<uint64_t> last_load_end_us{0};
  double last_load_s = 0;
};
Counters g;

// Events: queued from any thread (the log sink must never block or log), flushed by the status thread.
std::mutex g_event_mutex;
std::deque<std::string> g_events;
uint64_t g_event_drops = 0;

std::string Escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\t': out += "\\t"; break;
      default:
        if (uint8_t(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// kind and a body of extra JSON members (without braces), e.g. "\"to\":\"in_play\"".
void Event(std::string_view kind, const std::string& body = {}) {
  char head[96];
  std::snprintf(head, sizeof(head), "{\"type\":\"event\",\"t\":%.3f,\"kind\":\"", Secs(NowUs()));
  std::string line = head;
  line += kind;
  line += '"';
  if (!body.empty()) line += "," + body;
  line += '}';
  std::lock_guard lock(g_event_mutex);
  if (g_events.size() > 2000) {
    ++g_event_drops;
    return;
  }
  g_events.push_back(std::move(line));
}

std::string Str(std::string_view key, std::string_view value) {
  return "\"" + std::string(key) + "\":\"" + Escape(value) + "\"";
}

// ---------------------------------------------------------------------------------------------------------------
// Log watcher

std::mutex g_ring_mutex;
std::deque<std::string> g_ring;  // the last log lines
constexpr size_t kRing = 400;

class LogSink final : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const std::string_view text(msg.payload.data(), msg.payload.size());
    {
      std::lock_guard lock(g_ring_mutex);
      if (g_ring.size() >= kRing) g_ring.pop_front();
      g_ring.emplace_back(text);
    }
    const auto has = [&](std::string_view s) { return text.find(s) != std::string_view::npos; };
    const std::string_view brief = text.substr(0, 400);
    if (has("pipeline warm-up done")) g.port_warm_done = true;
    if (has("hybrid n1: startup step done")) g.n1_warm_done = true;
    if (msg.level >= spdlog::level::err) {
      const uint64_t n = ++g.errors;
      if (n <= 50 || n % 100 == 0) Event("error", Str("text", brief));
    } else if (msg.level == spdlog::level::warn) {
      ++g.warnings;
    }
    if (has("Failed to submit") || has("Failed to create a swapchain") || has("DEVICE_LOST") ||
        has("device lost")) {
      ++g.presenter_failures;
      Event("presenter_failure", Str("text", brief));
    } else if (text.rfind("gfx hitch:", 0) == 0) {
      ++g.hitches;
      Event("hitch", Str("text", brief));
    } else if (has("pipeline built during play")) {
      ++g.builds_logged;
      if (g.in_play) ++g.builds_in_play, Event("build_in_play", Str("text", brief));
    } else if (text.rfind("audio: ", 0) == 0 && has("silent fills")) {
      unsigned long long frames = 0, silent = 0, underruns = 0;
      if (std::sscanf(std::string(text).c_str(), "audio: %llu frames played, %llu silent fills in %llu underruns",
                      &frames, &silent, &underruns) == 3) {
        g.audio_frames += frames, g.audio_silent += silent, g.audio_underruns += underruns;
        if (underruns && g.in_play) Event("audio_underrun", "\"underruns\":" + std::to_string(underruns));
      }
    } else if (text.rfind("guest fps ", 0) == 0) {
      g.guest_fps = std::atof(std::string(text.substr(10, 16)).c_str());
    } else if (has("hybrid n1 compile:") || has("startup step done") || has("pipeline warm-up done") ||
               has("capture burst: wrote") || has("dualshot") || text.rfind("BOTS_EXEC: ", 0) == 0) {
      Event("log", Str("text", brief));
    }
  }
  void flush_() override {}
};

// ---------------------------------------------------------------------------------------------------------------
// Game-thread jobs (inspect, enemies on request)

struct Job {
  std::function<std::string()> fn;
  std::string result;
  bool done = false;
};
std::mutex g_job_mutex;
std::condition_variable g_job_cv;
std::deque<std::shared_ptr<Job>> g_jobs;
std::atomic<bool> g_jobs_pending{false};

// Runs fn on the game thread at its next frame; empty if the game thread doesn't get there within `timeout`.
std::string OnGameThread(std::function<std::string()> fn, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto job = std::make_shared<Job>();
  job->fn = std::move(fn);
  std::unique_lock lock(g_job_mutex);
  g_jobs.push_back(job);
  g_jobs_pending = true;
  if (!g_job_cv.wait_for(lock, timeout, [&] { return job->done; })) {
    g_jobs.erase(std::remove(g_jobs.begin(), g_jobs.end(), job), g_jobs.end());
    return {};
  }
  return job->result;
}

void RunJobs() {
  std::deque<std::shared_ptr<Job>> jobs;
  {
    std::lock_guard lock(g_job_mutex);
    jobs.swap(g_jobs);
    g_jobs_pending = false;
  }
  for (auto& job : jobs) {
    std::string r = job->fn();
    std::lock_guard lock(g_job_mutex);
    job->result = std::move(r);
    job->done = true;
  }
  g_job_cv.notify_all();
}

// UObject::GObjObjects, a TArray<UObject*> in the image's data whose entries' Index (+0x04) equal their position (as
// in scene_feed.cpp). Found on first use: only discovery commands and the navigation graph walk the table.
uint32_t ObjectTable() {
  static uint32_t table = 0;
  if (table) return table;
  for (uint32_t a = 0x83000000; a + 12 <= 0x839C0000; a += 4) {
    const uint32_t data = U32(a), count = U32(a + 4), max = U32(a + 8);
    if (!Readable(data) || count < 1000 || count > 4000000 || max < count || max > 8000000) continue;
    int good = 0;
    for (uint32_t i = 1; i < 64 && good >= 0; ++i)
      if (const uint32_t o = U32(data + 4 * i)) good = Readable(o) && U32(o + 4) == i ? good + 1 : -1;
    if (good >= 32) return table = a;
  }
  return 0;
}

// Calls fn(object) for every live object whose class chain has a class named exactly `cls`.
template <typename Fn>
void ForEachObject(const char* cls, Fn&& fn) {
  const uint32_t t = ObjectTable();
  if (!t) return;
  const uint32_t data = U32(t), count = U32(t + 4);
  std::unordered_map<uint32_t, bool> is;  // class object -> matches
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = U32(data + 4 * i), c = Readable(o) ? U32(o + 0x34) : 0;
    if (!Readable(c)) continue;
    auto it = is.find(c);
    if (it == is.end()) {
      bool m = false;
      for (uint32_t k = c, d = 0; Readable(k) && d < 32 && !m; k = U32(k + 0x3C), ++d) m = !std::strcmp(NameText(U32(k + 0x2C)), cls);
      it = is.emplace(c, m).first;
    }
    if (it->second) fn(o);
  }
}

// Properties of an object: name, type and (for scalar types) value.
// `cls` is the object's class, or for a struct value at `o`, the struct (UStructProperty +0x84 Struct).
std::string Inspect(uint32_t o, const std::string& filter, uint32_t cls = 0) {
  if (!Readable(o & ~3u)) return "{\"ok\":false,\"error\":\"no such object\"}";
  const bool is_struct = cls != 0;
  std::string out = "{\"ok\":true,\"object\":\"" + Escape(is_struct ? std::string(NameText(U32(cls + 0x2C))) : ObjName(o)) +
                    "\",\"class\":\"" + Escape(is_struct ? "struct" : ClassName(o)) + "\",\"props\":[";
  bool first = true;
  if (!cls) cls = U32(o + 0x34);
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + 0x50), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n) {
      const std::string type = ClassName(f);
      if (type == "Function" || type == "State" || type == "Enum" || type == "Const" || type == "ScriptStruct" ||
          type == "Struct")
        continue;
      const std::string name = NameText(U32(f + 0x2C));
      if (!filter.empty() && name.find(filter) == std::string::npos) continue;
      const uint32_t at = o + U32(f + 0x64);
      std::string value;
      char buf[64];
      if (type == "IntProperty") {
        value = std::to_string(int32_t(U32(at)));
      } else if (type == "FloatProperty") {
        std::snprintf(buf, sizeof(buf), "%g", F32(at));
        value = buf;
      } else if (type == "BoolProperty") {
        value = (U32(at) & U32(f + 0x84)) ? "true" : "false";
      } else if (type == "ByteProperty") {
        value = std::to_string(*Raw(at));
      } else if (type == "ObjectProperty" || type == "ClassProperty" || type == "ComponentProperty") {
        const uint32_t v = U32(at);
        char addr[16];
        std::snprintf(addr, sizeof(addr), "@%08X", v);
        value = Readable(v) ? "\"" + Escape(ObjName(v)) + ":" + Escape(ClassName(v)) + addr + "\"" : "null";
      } else if (type == "NameProperty") {
        value = "\"" + Escape(NameText(U32(at))) + "\"";
      } else if (type == "StrProperty") {
        const uint32_t data = U32(at), count = U32(at + 4);
        std::string s;
        for (uint32_t i = 0; Readable(data & ~3u) && i + 1 < count && i < 128; ++i) s += char(Raw(data + 2 * i)[1]);
        value = "\"" + Escape(s) + "\"";
      } else if (type == "StructProperty") {
        std::snprintf(buf, sizeof(buf), "\"[%g %g %g]\"", F32(at), F32(at + 4), F32(at + 8));
        value = buf;  // the first three floats: right for vectors, raw for anything else
      } else if (type == "ArrayProperty") {
        value = std::to_string(int32_t(U32(at + 4)));
      }
      if (!first) out += ',';
      first = false;
      out += "[\"" + Escape(name) + "\",\"" + type + "\"" + (value.empty() ? "" : "," + value) + "]";
    }
  return out + "]}";
}

// ---------------------------------------------------------------------------------------------------------------
// path_to. The game's AI walks its own navmeshes, and the UE3 path network is empty (NavigationPoints without edges),
// so paths come from a grid search over the game's collision: A* on 100-unit cells whose nodes are floor points (a
// downward trace), joined when the step up is climbable and a pawn-sized box sweeps between them. The search runs on
// the game thread for a few ms per frame (BOTS_INSTR_PATH_MS). The pad then walks the waypoints, skips ahead whenever
// a later one is in clear reach, and replans (avoiding the step that failed) when it stops making progress.

constexpr float kCell = 100, kZBucket = 80, kMaxRise = 110, kBodyZ = 90, kReach = 80;
// The deepest step down a path takes (`pathdrop N`). Long drops cost extra, so stairs win when there are any.
std::atomic<float> g_max_drop{260};
constexpr float kBodyExtent[3] = {22, 22, 40};
// Walkability traces: the world plus volumes and blocking volumes (stairs are often a BlockingVolume ramp over a mesh
// without collision). Floor traces take the nearest hit, not the first one found (no StopAtAnyHit, 0x200).
std::atomic<uint32_t> g_walk_flags{kTraceWorld | 0x08 | 0x40};
uint32_t FloorFlags() { return g_walk_flags.load(std::memory_order_relaxed) & ~0x200u; }
constexpr int kMaxExpansions = 30000, kMaxReplans = 3;
constexpr uint64_t kPlanLimitUs = 20000000, kStuckUs = 2500000, kSkipUs = 150000;

// The floor under (x, y) between z_from and z_to; NaN for none, a start inside geometry, or a slope too steep.
float Floor(float x, float y, float z_from, float z_to, uint32_t source) {
  const float a[3] = {x, y, z_from}, b[3] = {x, y, z_to};
  Hit h;
  if (!Trace(a, b, nullptr, source, h, FloorFlags()) || !h.hit || h.time < 0.01f || h.normal[2] < 0.6f)
    return std::nanf("");
  return h.loc[2];
}

// A pawn-sized box from floor point a to floor point b, at body height above the higher of the two: true when clear.
bool Sweep(const float a[3], const float b[3], uint32_t source) {
  const float z = std::max(a[2], b[2]) + kBodyZ;
  const float s[3] = {a[0], a[1], z}, e[3] = {b[0], b[1], z};
  Hit h;
  return Trace(s, e, kBodyExtent, source, h, g_walk_flags.load(std::memory_order_relaxed)) && !h.hit;
}

float Dist(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}
float DistXY(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0], dy = a[1] - b[1];
  return std::sqrt(dx * dx + dy * dy);
}

struct PathState {
  // The request (macro)
  bool active = false;
  bool near_ok = true;  // when the goal can't be reached, walk to the reachable point nearest it ("near")
  float goal[3] = {}, radius = 150;
  uint64_t gen = 0;
  // Progress, for the status and events
  std::string state = "idle";  // planning, following; then arrived, near, no_path, stuck, timeout, stopped
  bool partial = false;        // following a path to the nearest reachable point, not to the goal
  bool done = false;           // reached a final state: the macro ends
  std::vector<std::array<float, 3>> wps;
  size_t i = 0;
  int replans = 0, expansions = 0;
  int no_floor = 0, too_high = 0, swept_out = 0;  // rejected steps, this search
  float left = 0;  // units along the remaining waypoints
  // The search (game thread)
  struct Node {
    float p[3];
    uint64_t parent;
    float g;
    bool closed;
  };
  std::unordered_map<uint64_t, Node> nodes;
  std::unordered_map<uint64_t, float> floors;  // (cell, z bucket searched from) -> floor z
  std::vector<std::pair<float, uint64_t>> open;  // a heap on f
  uint64_t start_key = 0, plan_start_us = 0;
  float goal_floor = 0, start_floor = 0;
  std::vector<uint64_t> blocked;  // cell pairs (xy) whose step failed when walked
  // Following
  float check_pos[3] = {};
  uint64_t check_us = 0, skip_us = 0;
  bool smashed = false;      // already tried melee (B) at this standstill: weak planks and crates break to it
  uint64_t smash_until = 0;  // Steer holds B until then
};
std::mutex g_path_mutex;
PathState g_path;

uint64_t CellXY(int ix, int iy) { return uint64_t(uint32_t(ix + (1 << 20)) & 0x1FFFFF) << 21 | (uint32_t(iy + (1 << 20)) & 0x1FFFFF); }
uint64_t NodeKey(int ix, int iy, float z) {
  return CellXY(ix, iy) << 20 | (uint32_t(int(std::lround(z / kZBucket)) + (1 << 19)) & 0xFFFFF);
}
int CellOf(float v) { return int(std::lround(v / kCell)); }
uint64_t EdgeKey(int ax, int ay, int bx, int by) {
  const uint64_t a = CellXY(ax, ay), b = CellXY(bx, by);
  return std::min(a, b) * 0x9E3779B97F4A7C15ull ^ std::max(a, b);
}

std::string PathEvent(const PathState& ps, const char* state, std::string extra = {}) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "\"state\":\"%s\",\"replans\":%d,\"expansions\":%d,\"goal\":[%.0f,%.0f,%.0f]", state,
                ps.replans, ps.expansions, ps.goal[0], ps.goal[1], ps.goal[2]);
  return buf + (extra.empty() ? "" : "," + extra);
}

void PathFinish(PathState& ps, const char* state, std::string extra = {}) {
  ps.state = state;
  ps.done = true;
  Event("path", PathEvent(ps, state, std::move(extra)));
}

void PathBeginPlan(PathState& ps, const float pos[3], uint32_t pawn) {
  ps.nodes.clear();
  ps.floors.clear();
  ps.open.clear();
  ps.wps.clear();
  ps.i = 0;
  ps.expansions = ps.no_floor = ps.too_high = ps.swept_out = 0;
  ps.partial = false;
  ps.state = "planning";
  ps.plan_start_us = NowUs();
  ps.start_floor = Floor(pos[0], pos[1], pos[2], pos[2] - 400, pawn);
  if (std::isnan(ps.start_floor)) ps.start_floor = pos[2] - kBodyZ;
  ps.goal_floor = Floor(ps.goal[0], ps.goal[1], ps.goal[2] + 60, ps.goal[2] - 600, pawn);
  if (std::isnan(ps.goal_floor)) ps.goal_floor = ps.goal[2];
  const float p[3] = {pos[0], pos[1], ps.start_floor};
  ps.start_key = NodeKey(CellOf(p[0]), CellOf(p[1]), p[2]);
  ps.nodes[ps.start_key] = {{p[0], p[1], p[2]}, 0, 0, false};
  const float goal[3] = {ps.goal[0], ps.goal[1], ps.goal_floor};
  ps.open.push_back({Dist(p, goal), ps.start_key});
  Event("path", PathEvent(ps, "planning"));
}

// Waypoints from the start to node `key` (then `last`, if given); starts following them.
void PathAdopt(PathState& ps, uint64_t key, const float* last) {
  std::vector<std::array<float, 3>> back;
  if (last) back.push_back({last[0], last[1], last[2]});
  for (uint64_t k = key; k && k != ps.start_key; k = ps.nodes[k].parent) {
    const auto& n = ps.nodes[k];
    back.push_back({n.p[0], n.p[1], n.p[2]});
  }
  ps.wps.assign(back.rbegin(), back.rend());
  ps.i = 0;
  ps.state = "following";
  ps.check_us = 0;
  ps.smashed = false;
}

// One frame's share of the search. True when it ended (found or not).
bool PathSearch(PathState& ps, uint32_t pawn, uint64_t budget_us) {
  const uint64_t t0 = NowUs();
  const float goal[3] = {ps.goal[0], ps.goal[1], ps.goal_floor};
  const auto cmp = [](const auto& a, const auto& b) { return a.first > b.first; };
  while (NowUs() - t0 < budget_us) {
    if (ps.open.empty() || ps.expansions >= kMaxExpansions || NowUs() - ps.plan_start_us > kPlanLimitUs) {
      // What the search reached, for debugging: the explored floor points nearest the goal.
      std::vector<std::array<float, 3>> seen;
      for (const auto& [k, n] : ps.nodes)
        if (n.closed) seen.push_back({n.p[0], n.p[1], n.p[2]});
      std::sort(seen.begin(), seen.end(),
                [&](const auto& x, const auto& y) { return Dist(x.data(), goal) < Dist(y.data(), goal); });
      std::string pts = "[";
      for (size_t k = 0; k < seen.size() && k < 12; ++k) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s[%.0f,%.0f,%.0f]", k ? "," : "", seen[k][0], seen[k][1], seen[k][2]);
        pts += buf;
      }
      char extra[160];
      std::snprintf(extra, sizeof(extra), "\"no_floor\":%d,\"too_high\":%d,\"blocked\":%d,\"nearest\":", ps.no_floor,
                    ps.too_high, ps.swept_out);
      const std::string why = (ps.open.empty() ? Str("why", "exhausted") : Str("why", "limit")) + "," + extra + pts + "]";
      // The goal is out of reach (another floor with no way found, behind a door the script hasn't opened): head for
      // the explored point nearest it instead, if that gets us anywhere.
      uint64_t best = 0;
      float best_d = 0;
      for (const auto& [k, n] : ps.nodes)
        if (n.closed && (!best || Dist(n.p, goal) < best_d)) best = k, best_d = Dist(n.p, goal);
      const auto& start = ps.nodes[ps.start_key];
      if (!ps.near_ok || !best || best == ps.start_key || Dist(ps.nodes[best].p, start.p) < kReach) {
        PathFinish(ps, "no_path", why);
        return true;
      }
      PathAdopt(ps, best, nullptr);
      ps.partial = true;
      Event("path", PathEvent(ps, "found", "\"partial\":true,\"n\":" + std::to_string(ps.wps.size()) + "," + why));
      return true;
    }
    std::pop_heap(ps.open.begin(), ps.open.end(), cmp);
    const uint64_t key = ps.open.back().second;
    ps.open.pop_back();
    auto& node = ps.nodes[key];
    if (node.closed) continue;
    node.closed = true;
    ++ps.expansions;
    const float a[3] = {node.p[0], node.p[1], node.p[2]};
    const float ag = node.g;
    // Done: within reach of the goal and the step to it is clear.
    if (DistXY(a, goal) <= std::max(ps.radius, kCell * 0.8f) && std::fabs(a[2] - goal[2]) < 120 &&
        (DistXY(a, goal) < 40 || Sweep(a, goal, pawn))) {
      PathAdopt(ps, key, goal);
      std::string pts = "[";
      for (const auto& w : ps.wps) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%s[%.0f,%.0f,%.0f]", pts.size() > 1 ? "," : "", w[0], w[1], w[2]);
        pts += buf;
      }
      char extra[96];
      std::snprintf(extra, sizeof(extra), "\"n\":%zu,\"ms\":%.0f,\"traces\":%llu,", ps.wps.size(),
                    double(NowUs() - ps.plan_start_us) / 1000, static_cast<unsigned long long>(g_traces.load()));
      Event("path", PathEvent(ps, "found", extra + std::string("\"waypoints\":") + pts + "]"));
      return true;
    }
    const int ax = CellOf(a[0]), ay = CellOf(a[1]);
    const int az = int(std::lround(a[2] / kZBucket));
    for (int d = 0; d < 8; ++d) {
      static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
      const int bx = ax + kDx[d], by = ay + kDy[d];
      if (std::find(ps.blocked.begin(), ps.blocked.end(), EdgeKey(ax, ay, bx, by)) != ps.blocked.end()) continue;
      const float x = bx * kCell, y = by * kCell;
      const uint64_t fkey = CellXY(bx, by) << 20 | uint32_t(az + (1 << 19));
      float fz;
      if (auto it = ps.floors.find(fkey); it != ps.floors.end()) {
        fz = it->second;
      } else {
        fz = Floor(x, y, a[2] + kMaxRise + 20, a[2] - g_max_drop.load(std::memory_order_relaxed), pawn);
        ps.floors[fkey] = fz;
      }
      if (std::isnan(fz) || fz - a[2] > kMaxRise) {
        ++(std::isnan(fz) ? ps.no_floor : ps.too_high);
        continue;
      }
      const uint64_t bkey = NodeKey(bx, by, fz);
      auto it = ps.nodes.find(bkey);
      const float b[3] = {x, y, fz};
      const float g = ag + Dist(a, b) + std::max(0.0f, a[2] - fz - kMaxRise) * 2;
      if (it != ps.nodes.end() && (it->second.closed || it->second.g <= g)) continue;
      if (!Sweep(a, b, pawn)) {
        ++ps.swept_out;
        continue;
      }
      ps.nodes[bkey] = {{x, y, fz}, key, g, false};
      ps.open.push_back({g + 1.2f * Dist(b, goal), bkey});
      std::push_heap(ps.open.begin(), ps.open.end(), cmp);
    }
  }
  return false;
}

// Game thread, every frame while a path_to macro runs.
void PathTick(uint64_t now) {
  std::lock_guard lock(g_path_mutex);
  PathState& ps = g_path;
  if (!ps.active || ps.done) return;
  const uint32_t pc = LocalController();
  const uint32_t pawn = pc ? PropObj(pc, "Pawn") : 0;
  float pos[3];
  if (!pawn || !PropVec(pawn, "Location", pos)) return;  // no pawn (cutscene, death): wait
  static const uint64_t budget = [] {
    const char* v = std::getenv("BOTS_INSTR_PATH_MS");
    return uint64_t(std::clamp(v && *v ? std::atof(v) : 4.0, 0.5, 50.0) * 1000);
  }();
  if (ps.state == "planning" && ps.nodes.empty()) PathBeginPlan(ps, pos, pawn);
  if (ps.state == "planning") {
    PathSearch(ps, pawn, budget);
    return;
  }
  if (ps.state != "following") return;
  const float feet[3] = {pos[0], pos[1], pos[2] - kBodyZ};
  const float goal[3] = {ps.goal[0], ps.goal[1], ps.goal_floor};
  if (DistXY(pos, ps.goal) <= ps.radius && std::fabs(feet[2] - goal[2]) < 160) return PathFinish(ps, "arrived");
  while (ps.i < ps.wps.size() && DistXY(pos, ps.wps[ps.i].data()) < kReach && std::fabs(feet[2] - ps.wps[ps.i][2]) < 160)
    ++ps.i;
  // Skip ahead to the farthest of the next few waypoints that is in clear reach.
  if (now - ps.skip_us > kSkipUs && ps.i + 1 < ps.wps.size()) {
    ps.skip_us = now;
    const float here = Floor(pos[0], pos[1], pos[2], pos[2] - 400, pawn);
    const float from[3] = {pos[0], pos[1], std::isnan(here) ? feet[2] : here};
    for (size_t j = std::min(ps.wps.size() - 1, ps.i + 5); j > ps.i; --j) {
      const float* w = ps.wps[j].data();
      if (std::fabs(w[2] - from[2]) > kMaxRise) continue;
      const float mid[2] = {(from[0] + w[0]) / 2, (from[1] + w[1]) / 2};
      const float mz = Floor(mid[0], mid[1], std::max(from[2], w[2]) + kMaxRise, std::min(from[2], w[2]) - 120, pawn);
      if (std::isnan(mz) || !Sweep(from, w, pawn)) continue;
      ps.i = j;
      break;
    }
  }
  if (ps.i >= ps.wps.size() && ps.partial) return PathFinish(ps, "near");
  if (ps.i >= ps.wps.size()) {
    // Out of waypoints short of the goal (a moved goal, or a drop): plan again from here.
    if (ps.replans >= kMaxReplans) return PathFinish(ps, "stuck", Str("why", "end of path"));
    ++ps.replans;
    Event("path", PathEvent(ps, "replan", Str("why", "end of path")));
    ps.nodes.clear();
    ps.state = "planning";
    return;
  }
  float left = DistXY(pos, ps.wps[ps.i].data());
  for (size_t j = ps.i + 1; j < ps.wps.size(); ++j) left += Dist(ps.wps[j - 1].data(), ps.wps[j].data());
  ps.left = left;
  if (!ps.check_us) {
    ps.check_us = now;
    std::memcpy(ps.check_pos, pos, sizeof(pos));
  } else if (now - ps.check_us > kStuckUs) {
    if (DistXY(pos, ps.check_pos) < 30 && !ps.smashed) {
      // Standing still: first try melee, which breaks weak planks and crates in the way.
      ps.smashed = true;
      ps.smash_until = now + 250000;
      ps.check_us = now;
      Event("path", PathEvent(ps, "smash"));
      return;
    }
    if (DistXY(pos, ps.check_pos) < 30) {
      const float* w = ps.wps[ps.i].data();
      ps.blocked.push_back(EdgeKey(CellOf(pos[0]), CellOf(pos[1]), CellOf(w[0]), CellOf(w[1])));
      if (ps.replans >= kMaxReplans) return PathFinish(ps, "stuck", Str("why", "no progress"));
      ++ps.replans;
      Event("path", PathEvent(ps, "replan", Str("why", "no progress")));
      ps.nodes.clear();
      ps.state = "planning";
      ps.check_us = 0;
      return;
    }
    ps.check_us = now;
    ps.smashed = false;
    std::memcpy(ps.check_pos, pos, sizeof(pos));
  }
}

void PathStart(const float goal[3], float radius, bool near_ok) {
  std::lock_guard lock(g_path_mutex);
  PathState& ps = g_path;
  ps = PathState{};
  ps.active = true;
  ps.near_ok = near_ok;
  std::memcpy(ps.goal, goal, sizeof(ps.goal));
  ps.radius = radius;
  ps.state = "planning";
}

void PathStop(const char* how) {
  std::lock_guard lock(g_path_mutex);
  PathState& ps = g_path;
  if (ps.active && !ps.done) PathFinish(ps, how);
  ps.active = false;
}

// ---------------------------------------------------------------------------------------------------------------
// The control channel's pad

constexpr uint16_t kButtonMasks[] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000};
constexpr const char* kButtonNames[] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS",
                                        "RS", "LB",   "RB",   "A",     "B",    "X",    "Y"};

bool ParseButtons(const std::string& text, uint16_t& mask) {
  mask = 0;
  std::stringstream ss(text);
  for (std::string b; std::getline(ss, b, '+');) {
    for (auto& c : b) c = char(std::toupper(uint8_t(c)));
    bool found = false;
    for (size_t i = 0; i < std::size(kButtonNames); ++i)
      if (b == kButtonNames[i]) mask |= kButtonMasks[i], found = true;
    if (b == "DPAD_UP") mask |= 1, found = true;
    if (!found) return false;
  }
  return mask != 0;
}

enum class MacroKind { kNone, kMoveTo, kAimAt, kAimNearest, kTurn, kPathTo };
const char* MacroName(MacroKind k) {
  switch (k) {
    case MacroKind::kMoveTo: return "move_to";
    case MacroKind::kAimAt: return "aim_at";
    case MacroKind::kAimNearest: return "aim_nearest";
    case MacroKind::kTurn: return "turn";
    case MacroKind::kPathTo: return "path_to";
    default: return "none";
  }
}

struct Press {
  uint16_t mask;
  uint32_t ms;
  bool started = false, released = false;
  uint64_t start_us = 0;
  int polls = 0, gap_polls = 0;
  uint64_t id;
};

struct PadState {
  std::mutex m;
  std::condition_variable cv;
  uint16_t held = 0;
  std::deque<Press> presses;
  uint64_t next_press = 1, done_press = 0;  // ids
  float stick[2][2] = {};
  uint64_t stick_until[2] = {};
  float trig[2] = {};
  uint64_t trig_until[2] = {};
  // Macro
  MacroKind macro = MacroKind::kNone;
  float target[3] = {};
  float radius = 120, yaw_target = 0;
  uint64_t macro_until = 0, fire_until = 0;
  // Last output, for the status
  uint16_t out_buttons = 0;
  int16_t out_stick[4] = {};
  uint8_t out_trig[2] = {};
  uint64_t polls = 0;
};
PadState g_pad;

// Tunables for the steering (UE3 yaw and pitch: 65536 = 360 degrees).
// The game's response is steep: about 3 deg/s at 0.3 deflection, 30 at 0.6, over 200 at full (and nothing under the
// XInput dead zone, 0.27). So: full deflection beyond 20 deg off, never under 0.45, stop within 1.5 deg.
constexpr double kAimFullDeg = 20;
constexpr double kAimMinStick = 0.45, kAimTolDeg = 1.5;
constexpr double kYawSign = 1, kPitchSign = 1;  // right stick +x turns right (yaw up), +y looks up (pitch up)

void EndMacro(PadState& p, const char* how) {
  if (p.macro == MacroKind::kNone) return;
  if (p.macro == MacroKind::kPathTo) PathStop(how);
  Event("macro", Str("name", MacroName(p.macro)) + "," + Str("end", how));
  p.macro = MacroKind::kNone;
  g.macro_active = p.fire_until != 0;
}

uint16_t g_steer_buttons = 0;  // buttons a macro presses this poll (path_to's melee at a standstill)

void Steer(PadState& p, uint64_t now, float out_stick[4]) {
  Snap s;
  {
    std::lock_guard lock(g_snap_mutex);
    s.cam_ok = g_snap.cam_ok, s.has_pawn = g_snap.has_pawn;
    std::memcpy(s.cam, g_snap.cam, sizeof(s.cam));
    std::memcpy(s.cam_rot, g_snap.cam_rot, sizeof(s.cam_rot));
    std::memcpy(s.pos, g_snap.pos, sizeof(s.pos));
    s.enemies_valid = g_snap.enemies_valid;
    // aim_nearest's target: the nearest foe in sight, else the nearest foe.
    const Enemy* pick = nullptr;
    for (const auto& e : g_snap.enemies)
      if (e.foe && (!pick || (e.los == 1 && pick->los != 1))) pick = &e;
    if (pick) s.enemies.push_back(*pick);
  }
  if (p.macro_until && now >= p.macro_until) {
    EndMacro(p, p.macro == MacroKind::kAimNearest ? "done" : "timeout");
    return;
  }
  if (!s.cam_ok || !s.has_pawn) return;
  const double yaw = UnrealDeg(s.cam_rot[1]), pitch = UnrealDeg(s.cam_rot[0]);
  auto aim = [&](double want_yaw, double want_pitch, bool use_pitch) {
    const double dy = Wrap180(want_yaw - yaw), dp = use_pitch ? Wrap180(want_pitch - pitch) : 0;
    auto stick = [](double d) {
      if (std::fabs(d) < kAimTolDeg) return 0.0;
      const double v = std::clamp(d / kAimFullDeg, -1.0, 1.0);
      return std::fabs(v) < kAimMinStick ? std::copysign(kAimMinStick, v) : v;
    };
    out_stick[2] = float(kYawSign * stick(dy));
    out_stick[3] = float(kPitchSign * stick(dp));
    return std::fabs(dy) < kAimTolDeg && std::fabs(dp) < kAimTolDeg;
  };
  switch (p.macro) {
    case MacroKind::kMoveTo: {
      const double dx = p.target[0] - s.pos[0], dy = p.target[1] - s.pos[1];
      if (std::sqrt(dx * dx + dy * dy) <= p.radius) return EndMacro(p, "done");
      const double rel = (std::atan2(dy, dx) * 180 / kPi - yaw) * kPi / 180;
      out_stick[0] = float(std::sin(rel));
      out_stick[1] = float(std::cos(rel));
      break;
    }
    case MacroKind::kAimAt: {
      const double dx = p.target[0] - s.cam[0], dy = p.target[1] - s.cam[1], dz = p.target[2] - s.cam[2];
      if (aim(std::atan2(dy, dx) * 180 / kPi, std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180 / kPi, true))
        EndMacro(p, "done");
      break;
    }
    case MacroKind::kAimNearest: {
      if (s.enemies.empty()) break;
      const Enemy& e = s.enemies.front();
      aim(yaw + e.bearing, pitch + e.pitch, true);
      break;
    }
    case MacroKind::kTurn:
      if (aim(p.yaw_target, 0, false)) EndMacro(p, "done");
      break;
    case MacroKind::kPathTo: {
      float w[3];
      std::string ended;
      {
        std::lock_guard lock(g_path_mutex);
        const PathState& ps = g_path;
        if (ps.done || !ps.active) ended = ps.state;
        else if (ps.state != "following" || ps.i >= ps.wps.size()) break;  // planning: stand still
        else std::memcpy(w, ps.wps[ps.i].data(), sizeof(w));
        if (now < ps.smash_until) g_steer_buttons |= 0x2000;  // B
      }
      if (!ended.empty()) return EndMacro(p, ended.c_str());  // outside the lock: EndMacro takes it
      const double dx = w[0] - s.pos[0], dy = w[1] - s.pos[1];
      const double want = std::atan2(dy, dx) * 180 / kPi;
      const double rel = (want - yaw) * kPi / 180;
      out_stick[0] = float(std::sin(rel));
      out_stick[1] = float(std::cos(rel));
      // Face the way we walk (the game moves slower sideways and backwards); leave small errors alone.
      if (std::fabs(Wrap180(want - yaw)) > 12) aim(want, 0, false);
      break;
    }
    default:
      break;
  }
}

class InstrInputDriver final : public rex::input::InputDriver {
 public:
  InstrInputDriver() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "Instrumentation";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_SUCCESS;
    std::memset(out, 0, sizeof(*out));
    auto& p = g_pad;
    std::lock_guard lock(p.m);
    const uint64_t now = NowUs();
    ++p.polls;
    uint16_t buttons = p.held;
    // Presses run in order; one that shares a button with an earlier one waits until that one is released.
    uint16_t busy = 0;
    bool finished = false;
    for (auto& e : p.presses) {
      if (e.mask & busy) {
        busy |= e.mask;
        continue;
      }
      busy |= e.mask;
      if (!e.started) e.started = true, e.start_us = now;
      if (!e.released) {
        buttons |= e.mask;
        if (++e.polls >= 3 && now >= e.start_us + uint64_t(e.ms) * 1000) e.released = true;
      } else if (++e.gap_polls >= 3) {
        e.mask = 0;  // done
        finished = true;
      }
    }
    while (!p.presses.empty() && !p.presses.front().mask) p.done_press = p.presses.front().id, p.presses.pop_front();
    if (finished) {
      for (auto it = p.presses.begin(); it != p.presses.end();)
        it = it->mask ? std::next(it) : p.presses.erase(it);
      p.cv.notify_all();
    }
    float st[4] = {p.stick[0][0], p.stick[0][1], p.stick[1][0], p.stick[1][1]};
    for (int i = 0; i < 2; ++i)
      if (p.stick_until[i] && now >= p.stick_until[i]) p.stick[i][0] = p.stick[i][1] = 0, p.stick_until[i] = 0;
    float tr[2] = {p.trig[0], p.trig[1]};
    for (int i = 0; i < 2; ++i)
      if (p.trig_until[i] && now >= p.trig_until[i]) p.trig[i] = 0, p.trig_until[i] = 0;
    g_steer_buttons = 0;
    if (p.macro != MacroKind::kNone) Steer(p, now, st);
    buttons |= g_steer_buttons;
    if (p.fire_until) {
      if (now < p.fire_until) {
        tr[1] = 1;
      } else {
        p.fire_until = 0;
        Event("macro", Str("name", "fire") + "," + Str("end", "done"));
        g.macro_active = p.macro != MacroKind::kNone;
      }
    }
    auto axis = [](float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767)); };
    out->gamepad.buttons = buttons;
    out->gamepad.left_trigger = uint8_t(std::lround(std::clamp(tr[0], 0.0f, 1.0f) * 255));
    out->gamepad.right_trigger = uint8_t(std::lround(std::clamp(tr[1], 0.0f, 1.0f) * 255));
    out->gamepad.thumb_lx = axis(st[0]);
    out->gamepad.thumb_ly = axis(st[1]);
    out->gamepad.thumb_rx = axis(st[2]);
    out->gamepad.thumb_ry = axis(st[3]);
    uint64_t sig = buttons | uint64_t(out->gamepad.left_trigger) << 16 | uint64_t(out->gamepad.right_trigger) << 24 |
                   uint64_t(uint16_t(out->gamepad.thumb_lx)) << 32 | uint64_t(uint16_t(out->gamepad.thumb_ly)) << 48;
    sig ^= (uint64_t(uint16_t(out->gamepad.thumb_rx)) << 7) ^ (uint64_t(uint16_t(out->gamepad.thumb_ry)) << 29);
    if (sig != last_sig_) last_sig_ = sig, ++packet_;
    out->packet_number = packet_;
    p.out_buttons = buttons;
    p.out_stick[0] = out->gamepad.thumb_lx, p.out_stick[1] = out->gamepad.thumb_ly;
    p.out_stick[2] = out->gamepad.thumb_rx, p.out_stick[3] = out->gamepad.thumb_ry;
    p.out_trig[0] = out->gamepad.left_trigger, p.out_trig[1] = out->gamepad.right_trigger;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  static constexpr auto kDevice = static_cast<rex::input::DeviceId>(0x494E5354);  // 'INST'
  uint64_t last_sig_ = 0;
  uint32_t packet_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Status

std::function<rex::system::IGraphicsSystem*()> g_graphics;

std::mutex g_cmd_mutex;
std::deque<std::string> g_commands;

// A JSON object writer that also records every scalar under its dotted key (for `wait` and `keys`).
class Json {
 public:
  explicit Json(std::map<std::string, std::string>* flat) : flat_(flat) { s_ = "{"; }
  Json& Obj(const char* key) {
    Key(key);
    s_ += '{';
    path_.push_back(key);
    first_ = true;
    return *this;
  }
  Json& End() {
    s_ += '}';
    path_.pop_back();
    first_ = false;
    return *this;
  }
  Json& Num(const char* key, double v, const char* fmt = "%.3f") {
    char buf[48];
    if (!std::isfinite(v)) v = 0;
    std::snprintf(buf, sizeof(buf), fmt, v);
    return Raw(key, buf, buf);
  }
  Json& Int(const char* key, int64_t v) {
    const std::string t = std::to_string(v);
    return Raw(key, t, t);
  }
  Json& Bool(const char* key, bool v) { return Raw(key, v ? "true" : "false", v ? "1" : "0"); }
  Json& Str(const char* key, std::string_view v) { return Raw(key, "\"" + Escape(v) + "\"", std::string(v)); }
  // A raw JSON value, not recorded in the flat map.
  Json& Value(const char* key, const std::string& json) {
    Key(key);
    s_ += json;
    return *this;
  }
  std::string Done() { return s_ + "}"; }

 private:
  Json& Raw(const char* key, const std::string& json, const std::string& flat) {
    Key(key);
    s_ += json;
    if (flat_) {
      std::string k;
      for (const auto& p : path_) k += p + ".";
      (*flat_)[k + key] = flat;
    }
    return *this;
  }
  void Key(const char* key) {
    if (!first_) s_ += ',';
    first_ = false;
    s_ += '"';
    s_ += key;
    s_ += "\":";
  }
  std::string s_;
  std::vector<std::string> path_;
  bool first_ = true;
  std::map<std::string, std::string>* flat_;
};

struct PhaseInfo {
  std::string name = "boot";
  uint64_t since_us = g_start_us;
  std::map<std::string, double> first;  // first entry, seconds since start
  double precompile_done_s = -1;
};
std::mutex g_status_mutex;  // BuildStatus's own state
PhaseInfo g_phase;
uint32_t g_prev_hist[InstrState::kBuckets] = {};
uint64_t g_prev_hist_us = 0;
std::string g_cpu_json = "[]";
std::map<uint64_t, ThreadCpu> g_prev_cpu;
uint64_t g_prev_cpu_us = 0;
uint64_t g_seq = 0;

bool ContainsAny(const std::string& s, const std::vector<std::string>& parts) {
  for (const auto& p : parts)
    if (s.find(p) != std::string::npos) return true;
  return false;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

bool FrontEndMap(const std::string& map) {
  const std::string m = Lower(map);
  return m.find("frontend") != std::string::npos || m.find("front_end") != std::string::npos ||
         m.find("mainmenu") != std::string::npos || m == "entry";
}

struct PrecompileInfo {
  uint32_t port_done = 0, port_total = 0, n1_done = 0, n1_total = 0, n1_state = 0, gfx_done = 0, gfx_total = 0;
  bool hold = false, done = false;
};

PrecompileInfo Precompile() {
  PrecompileInfo p;
  p.port_done = g_state.warm_done, p.port_total = g_state.warm_total;
  p.n1_done = g_state.n1_warm_done, p.n1_total = g_state.n1_warm_total, p.n1_state = g_state.n1_warm_state;
  p.hold = g_state.warm_hold != 0;
  if (auto* gfx = g_graphics ? g_graphics() : nullptr) gfx->GetShaderPrecompileProgress(p.gfx_done, p.gfx_total);
  // The native renderer logs the end of its warm-up (and of n1's startup step, when the hybrid's n1 fill is on);
  // the totals alone can't tell "not started" from "nothing to do". Other renderers: their background progress.
  static const bool n1_expected = [] {
    const char* v = std::getenv("BOTS_HYBRID_FILL");
    return v && std::atoi(v) == 4;
  }();
  const bool port = g.port_warm_done || (g_state.warm_loaded && p.port_done >= p.port_total);
  // The native renderer publishes nothing before its first frame, so its 0/0 then means "not started".
  static const bool native = rex::cvar::GetFlagByName("gpu_plugin") == "native";
  if (g_state.frames > 0) p.done = !p.hold && port && (!n1_expected || g.n1_warm_done || p.n1_state == 2);
  else if (!native)
    p.done = g.ticks > 0 && (p.gfx_total == 0 || p.gfx_done >= p.gfx_total);
  return p;
}

// What the game is doing, most specific first.
std::string ComputePhase(const Snap& s, const PrecompileInfo& pc, uint64_t now, std::string* dialog) {
  // The game thread can stamp a tick after `now` was read: clamp, or the unsigned age wraps and reads as a hang.
  const auto age = [now](uint64_t at) { return at > now ? uint64_t(0) : now - at; };
  const uint64_t ticks = g.ticks, tick_age = age(g.last_tick_us), hud_age = age(g.last_hud_us);
  if (pc.hold) return "precompile";
  if (!ticks || (!g_state.frames && !s.has_pc && s.scenes.empty())) return "boot";
  if (g.in_browse) return "loading";
  // The game thread stopped: briefly while a level streams in; for good when the guest waits on something that never
  // comes (e.g. a system UI the port doesn't show). A real load that long still has in_browse set.
  if (tick_age > 10000000) return "hung";
  if (tick_age > 1500000) return "loading";
  // A dialog is the top scene (scenes below the top stay in the stack while hidden). Overlays don't count.
  for (auto it = s.scenes.rbegin(); it != s.scenes.rend(); ++it) {
    if (it->find("ButtonBar") != std::string::npos) continue;
    if (ContainsAny(*it, Cfg().dialogs)) {
      if (dialog) *dialog = *it;
      return "dialog";
    }
    break;
  }
  if (g.paused || s.world_paused) return "paused";
  if (s.title_movie) return "movie";
  if (s.title || FrontEndMap(s.map) || s.game_class.find("Entry") != std::string::npos) return "front_end";
  if (s.dead) return "dead";
  if (!s.has_pawn) return "menu";
  if (s.cinematic) return "cutscene";
  if (hud_age < 500000) return "in_play";
  return "cutscene";
}

void SampleCpu(uint64_t now) {
  if (Cfg().cpu_secs <= 0 || now - g_prev_cpu_us < uint64_t(Cfg().cpu_secs * 1e6)) return;
  auto cur = SampleThreadCpu();
  if (g_prev_cpu_us) {
    const double wall = double(now - g_prev_cpu_us) / 1e6;
    std::vector<std::pair<double, std::string>> rows;
    double total = 0;
    for (const auto& [tid, t] : cur) {
      auto it = g_prev_cpu.find(tid);
      const double used = t.seconds - (it != g_prev_cpu.end() ? it->second.seconds : 0);
      total += used;
      if (used / wall >= 0.02) rows.emplace_back(used / wall * 100, t.name.empty() ? std::to_string(tid) : t.name);
    }
    std::sort(rows.rbegin(), rows.rend());
    if (rows.size() > 8) rows.resize(8);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "{\"total\":%.0f,\"threads\":[", total / wall * 100);
    std::string j = buf;
    for (size_t i = 0; i < rows.size(); ++i) {
      std::snprintf(buf, sizeof(buf), "%s[\"", i ? "," : "");
      j += buf + Escape(rows[i].second);
      std::snprintf(buf, sizeof(buf), "\",%.0f]", rows[i].first);
      j += buf;
    }
    g_cpu_json = j + "]}";
  }
  g_prev_cpu = std::move(cur);
  g_prev_cpu_us = now;
}

// Percentile from a bucket histogram (bucket upper edge, ms).
double Percentile(const uint32_t* h, uint64_t total, double q) {
  if (!total) return 0;
  const uint64_t want = uint64_t(std::ceil(q * double(total)));
  uint64_t seen = 0;
  for (uint32_t i = 0; i < InstrState::kBuckets; ++i)
    if ((seen += h[i]) >= want) return (i + 1) * InstrState::kBucketMs;
  return InstrState::kBuckets * InstrState::kBucketMs;
}

// The status line. window: advance the fps/percentile window (the status thread's lines); a command's `status`
// reports the window so far without resetting it. flat: the dotted keys, for `wait`.
std::string BuildStatus(bool window, std::map<std::string, std::string>* flat) {
  std::lock_guard lock(g_status_mutex);
  const uint64_t now = NowUs();
  Snap s;
  {
    std::lock_guard sl(g_snap_mutex);
    s = g_snap;
  }
  const PrecompileInfo pc = Precompile();
  std::string dialog;
  const std::string phase = ComputePhase(s, pc, now, &dialog);
  if (phase != g_phase.name) {
    Event("phase", Str("from", g_phase.name) + "," + Str("to", phase) +
                       ",\"after_s\":" + std::to_string(double(now - g_phase.since_us) / 1e6));
    g_phase.name = phase;
    g_phase.since_us = now;
  }
  g.in_play = phase == "in_play";
  if (!g_phase.first.count(phase)) g_phase.first[phase] = Secs(now);
  if (pc.done && g_phase.precompile_done_s < 0) {
    g_phase.precompile_done_s = Secs(now);
    Event("precompile_done", "\"t_s\":" + std::to_string(g_phase.precompile_done_s));
  }

  Json j(flat);
  j.Str("type", "status").Int("v", 1).Int("seq", int64_t(++g_seq)).Num("t", Secs(now));
#ifdef _WIN32
  j.Int("pid", int64_t(GetCurrentProcessId()));
#else
  j.Int("pid", int64_t(getpid()));
#endif
  j.Str("phase", phase).Num("phase_s", double(now - g_phase.since_us) / 1e6);

  // Game
  {
    std::lock_guard tl(g.text_mutex);
    j.Obj("game")
        .Int("ticks", int64_t(g.ticks))
        .Num("tick_age_ms", g.ticks ? double(now - std::min<uint64_t>(now, g.last_tick_us)) / 1000 : -1, "%.0f")
        .Num("hud_age_ms", g.last_hud_us ? double(now - std::min<uint64_t>(now, g.last_hud_us)) / 1000 : -1, "%.0f")
        .Str("map", s.map)
        .Str("game", s.game_class)
        .Num("world_time", s.world_time, "%.2f")
        .Bool("paused", g.paused || s.world_paused)
        .Bool("loading", g.in_browse)
        .Int("loads", int64_t(g.loads))
        .Num("last_load_s", g.last_load_s, "%.2f")
        .Num("since_load_s", g.last_load_end_us ? double(now - g.last_load_end_us) / 1e6 : -1, "%.1f")
        .Str("chapter", g.chapter)
        .Str("levels", s.levels)
        .Int("levels_pending", s.levels_pending)
        .Int("objectives", s.objectives)
        .Bool("checkpoint_pending", s.checkpoint_pending)
        .Bool("cinematic", s.cinematic)
        .Bool("title", s.title);
    std::string scenes;
    for (const auto& sc : s.scenes) scenes += (scenes.empty() ? "" : ">") + sc;
    j.Str("scenes", scenes).Str("scene", s.scenes.empty() ? "" : s.scenes.back()).Str("dialog", dialog);
    j.End();
  }
  j.Obj("player").Bool("present", s.has_pawn);
  if (s.has_pawn) {
    j.Num("x", s.pos[0], "%.1f").Num("y", s.pos[1], "%.1f").Num("z", s.pos[2], "%.1f");
    j.Num("speed", std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]), "%.0f");
    j.Num("yaw", UnrealDeg(s.rot[1]), "%.1f").Int("health", s.health).Int("health_max", s.health_max);
    j.Str("pawn", s.pawn_class).Str("weapon", s.weapon).Int("ammo", s.ammo);
    j.Int("clip", s.clip).Int("clip_max", s.clip_max).Int("reserve", s.reserve);
  }
  j.Bool("dead", s.dead);
  j.End();
  // The current objective (the HUD marker's target). dist / dz from the player, bearing from the camera's view.
  {
    const Objective& ob = s.objective;
    j.Obj("objective").Bool("present", ob.present).Int("open", ob.open).Str("label", ob.label);
    if (ob.present) {
      const double dx = ob.pos[0] - s.pos[0], dy = ob.pos[1] - s.pos[1];
      const double yaw = s.cam_ok ? UnrealDeg(s.cam_rot[1]) : UnrealDeg(s.rot[1]);
      j.Num("x", ob.pos[0], "%.0f").Num("y", ob.pos[1], "%.0f").Num("z", ob.pos[2], "%.0f");
      j.Num("dist", std::sqrt(dx * dx + dy * dy), "%.0f").Num("dz", ob.pos[2] - s.pos[2], "%.0f");
      j.Num("bearing", Wrap180(std::atan2(dy, dx) * 180 / kPi - yaw), "%.1f");
      j.Str("actor", ob.actor).Str("source", ob.source).Int("priority", ob.priority);
    }
    j.End();
  }
  if (s.hotspot.count) {
    const Hotspot& h = s.hotspot;
    const double dx = h.pos[0] - s.pos[0], dy = h.pos[1] - s.pos[1];
    const double yaw = s.cam_ok ? UnrealDeg(s.cam_rot[1]) : UnrealDeg(s.rot[1]);
    j.Obj("hotspot").Int("n", h.count).Str("kind", h.kind).Str("actor", h.actor);
    j.Num("x", h.pos[0], "%.0f").Num("y", h.pos[1], "%.0f").Num("z", h.pos[2], "%.0f");
    j.Num("dist", std::sqrt(dx * dx + dy * dy), "%.0f").Num("dz", h.pos[2] - s.pos[2], "%.0f");
    j.Num("bearing", Wrap180(std::atan2(dy, dx) * 180 / kPi - yaw), "%.1f");
    std::string nearest_json = "[";
    for (const auto& n : h.nearest) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s{\"actor\":\"%s\",\"kind\":\"%s\",\"x\":%.0f,\"y\":%.0f,\"z\":%.0f,\"d\":%.0f}",
                    nearest_json.size() > 1 ? "," : "", Escape(n.actor).c_str(), Escape(n.kind).c_str(), n.pos[0], n.pos[1],
                    n.pos[2], std::sqrt(n.d2));
      nearest_json += buf;
    }
    j.Value("nearest", nearest_json + "]").End();
  }
  j.Obj("prompt").Bool("counterkill", s.prompt_counterkill).Bool("tutorial", s.prompt_tutorial).End();
  if (s.cam_ok)
    j.Obj("camera")
        .Num("x", s.cam[0], "%.1f")
        .Num("y", s.cam[1], "%.1f")
        .Num("z", s.cam[2], "%.1f")
        .Num("yaw", UnrealDeg(s.cam_rot[1]), "%.1f")
        .Num("pitch", UnrealDeg(s.cam_rot[0]), "%.1f")
        .Num("fov", s.fov, "%.1f")
        .End();
  if (s.enemies_valid) {
    std::string e = "[";
    int n = 0;
    for (const auto& en : s.enemies) {
      if (n++ == 6) break;
      char buf[240];
      std::snprintf(buf, sizeof(buf),
                    "%s{\"x\":%.0f,\"y\":%.0f,\"z\":%.0f,\"dist\":%.0f,\"bearing\":%.1f,\"pitch\":%.1f,\"health\":%d,"
                    "\"player\":%s,\"team\":%d,\"foe\":%s,\"los\":%d,\"class\":\"",
                    n > 1 ? "," : "", en.pos[0], en.pos[1], en.pos[2], en.dist, en.bearing, en.pitch, en.health,
                    en.player ? "true" : "false", en.team, en.foe ? "true" : "false", en.los);
      e += buf + Escape(en.cls) + "\"}";
    }
    j.Value("enemies", e + "]");
    j.Obj("world").Int("pawns", s.pawns).Int("alive", int64_t(s.enemies.size())).End();
  }

  // Precompile
  j.Obj("precompile")
      .Bool("done", pc.done)
      .Bool("hold", pc.hold)
      .Bool("port_finished", g.port_warm_done || (g_state.warm_loaded && pc.port_done >= pc.port_total))
      .Int("warm_done", pc.port_done)
      .Int("warm_total", pc.port_total)
      .Int("n1_done", pc.n1_done)
      .Int("n1_total", pc.n1_total)
      .Int("n1_state", pc.n1_state)
      .Num("n1_secs", double(g_state.n1_warm_ms) / 1000, "%.1f")
      .Int("bg_done", pc.gfx_done)
      .Int("bg_total", pc.gfx_total)
      .Num("done_at_s", g_phase.precompile_done_s, "%.1f")
      .End();

  // Render: the frame window since the last status line.
  const uint64_t frames = g_state.frames;
  uint32_t delta[InstrState::kBuckets];
  uint64_t count = 0, over33 = 0, over50 = 0;
  for (uint32_t i = 0; i < InstrState::kBuckets; ++i) {
    const uint32_t c = g_state.hist[i].load(std::memory_order_relaxed);
    delta[i] = c - g_prev_hist[i];
    count += delta[i];
    if (i * InstrState::kBucketMs >= 33.5) over33 += delta[i];
    if (i * InstrState::kBucketMs >= 50) over50 += delta[i];
    if (window) g_prev_hist[i] = c;
  }
  const double win_s = g_prev_hist_us ? double(now - g_prev_hist_us) / 1e6 : Secs(now);
  if (window) g_prev_hist_us = now;
  const uint32_t worst = window ? g_state.worst_frame_us.exchange(0) : g_state.worst_frame_us.load();
  const uint64_t last_frame = g_state.last_frame_us;
  j.Obj("render")
      .Bool("native", FindInstrState() && frames > 0)
      .Int("frames", int64_t(frames))
      .Num("fps", win_s > 0 ? count / win_s : 0, "%.1f")
      .Num("window_s", win_s, "%.2f")
      .Num("p50_ms", Percentile(delta, count, 0.5), "%.1f")
      .Num("p90_ms", Percentile(delta, count, 0.9), "%.1f")
      .Num("p99_ms", Percentile(delta, count, 0.99), "%.1f")
      .Num("worst_ms", worst / 1000.0, "%.1f")
      .Int("over33", int64_t(over33))
      .Int("over50", int64_t(over50))
      .Num("frame_age_ms", last_frame ? double(now - std::min<uint64_t>(now, last_frame)) / 1000 : -1, "%.0f")
      .Num("guest_fps_log", g.guest_fps, "%.1f")
      .Int("draws", int64_t(g_state.draws.load()))
      .Int("pending_draws", int64_t(g_state.pending_draws.load()))
      .Int("live_pipelines", int64_t(g_state.live_pipelines.load()))
      .Int("failed_pipeline_draws", int64_t(g_state.failed_pipeline_draws.load()))
      .Int("w", g_state.render_w)
      .Int("h", g_state.render_h);
  j.Obj("n1")
      .Int("frames", int64_t(g_state.n1_frames.load()))
      .Int("draws", g_state.n1_draws)
      .Int("instances", g_state.n1_instances)
      .Int("skipped", g_state.n1_skipped)
      .Int("particles", g_state.n1_particles)
      .Int("particle_dropped", int64_t(g_state.n1_particle_dropped.load()))
      .Int("meshes_pending", g_state.n1_meshes_pending)
      .Int("loading", g_state.n1_loading)
      .Int("pipelines_live", g_state.n1_pipelines_live)
      .Int("shaders_live", g_state.n1_shaders_live)
      .Int("late_pipelines", g_state.n1_late_pipelines)
      .Int("late_shaders", g_state.n1_late_shaders)
      .Int("build_failures", g_state.n1_build_failures)
      .Int("partial_frames", g_state.n1_partial_frames)
      .Int("draws_no_pipe", int64_t(g_state.n1_draws_no_pipe.load()))
      .Int("fallback_draws", int64_t(g_state.n1_fallback_draws.load()))
      .Num("record_worst_ms", (window ? g_state.n1_record_worst_us.exchange(0) : g_state.n1_record_worst_us.load()) /
                                  1000.0, "%.2f")
      .Bool("failed", g_state.n1_failed != 0)
      .End();
  j.End();

  j.Obj("health")
      .Int("presenter_failures", int64_t(g.presenter_failures))
      .Int("hitches", int64_t(g.hitches))
      .Int("builds_in_play", int64_t(g.builds_in_play))
      .Int("builds_logged", int64_t(g.builds_logged))
      .Int("errors", int64_t(g.errors))
      .Int("warnings", int64_t(g.warnings))
      .Int("audio_underruns", int64_t(g.audio_underruns))
      .Int("audio_silent_fills", int64_t(g.audio_silent))
      .End();

  {
    auto& p = g_pad;
    std::lock_guard pl(p.m);
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%04X", p.out_buttons);
    j.Obj("input")
        .Str("buttons", hex)
        .Num("lx", p.out_stick[0] / 32767.0, "%.2f")
        .Num("ly", p.out_stick[1] / 32767.0, "%.2f")
        .Num("rx", p.out_stick[2] / 32767.0, "%.2f")
        .Num("ry", p.out_stick[3] / 32767.0, "%.2f")
        .Int("lt", p.out_trig[0])
        .Int("rt", p.out_trig[1])
        .Str("macro", MacroName(p.macro))
        .Bool("firing", p.fire_until != 0)
        .Int("queued_presses", int64_t(p.presses.size()))
        .Int("polls", int64_t(p.polls))
        .Int("human", int64_t(MouseKeyboardDriver::HumanEvents()))
        .Int("traces", int64_t(g_traces.load()));
  }
  {
    std::lock_guard pl(g_path_mutex);
    const PathState& ps = g_path;
    j.Obj("path")
        .Str("state", ps.state)
        .Int("i", int64_t(ps.i))
        .Int("n", int64_t(ps.wps.size()))
        .Num("left_m", ps.left / 100.0, "%.1f")
        .Int("replans", ps.replans)
        .Int("expansions", ps.expansions);
    if (ps.i < ps.wps.size())
      j.Num("wx", ps.wps[ps.i][0], "%.0f").Num("wy", ps.wps[ps.i][1], "%.0f").Num("wz", ps.wps[ps.i][2], "%.0f");
    j.End().End();
  }
  std::string firsts = "{";
  for (const auto& [name, t] : g_phase.first) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s\"%s\":%.2f", firsts.size() > 1 ? "," : "", name.c_str(), t);
    firsts += buf;
  }
  j.Value("phase_first_s", firsts + "}");
  SampleCpu(now);
  j.Value("cpu", g_cpu_json);
  return j.Done();
}

// ---------------------------------------------------------------------------------------------------------------
// Conditions for `wait`

bool Number(const std::string& s, double& v) {
  if (s.empty()) return false;
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end && *end == 0;
}

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(uint8_t(s.back()))) s.pop_back();
  size_t i = 0;
  while (i < s.size() && std::isspace(uint8_t(s[i]))) ++i;
  return s.substr(i);
}

bool Clause(const std::string& clause, const std::map<std::string, std::string>& flat, std::string& error) {
  static const char* kOps[] = {">=", "<=", "!=", "==", "~=", ">", "<", "="};
  size_t at = std::string::npos;
  std::string op;
  for (const char* o : kOps) {
    const size_t p = clause.find(o);
    if (p != std::string::npos && (at == std::string::npos || p < at || (p == at && std::strlen(o) > op.size())))
      at = p, op = o;
  }
  if (at == std::string::npos) {
    // A bare key: true when it is set and not 0/false/empty.
    auto it = flat.find(Trim(clause));
    return it != flat.end() && !it->second.empty() && it->second != "0";
  }
  const std::string key = Trim(clause.substr(0, at));
  std::string want = Trim(clause.substr(at + op.size()));
  if (want.size() >= 2 && (want.front() == '"' || want.front() == '\'')) want = want.substr(1, want.size() - 2);
  if (want == "true") want = "1";
  if (want == "false") want = "0";
  auto it = flat.find(key);
  if (it == flat.end()) {
    error = "unknown key " + key;
    return false;
  }
  const std::string& have = it->second;
  if (op == "~=") return have.find(want) != std::string::npos;
  double a, b;
  const bool numeric = Number(have, a) && Number(want, b);
  if (op == "==" || op == "=") return numeric ? a == b : have == want;
  if (op == "!=") return numeric ? a != b : have != want;
  if (!numeric) return false;
  if (op == ">=") return a >= b;
  if (op == "<=") return a <= b;
  if (op == ">") return a > b;
  return a < b;
}

bool Condition(const std::string& cond, const std::map<std::string, std::string>& flat, std::string& error) {
  size_t from = 0;
  while (true) {
    const size_t bar = cond.find("||", from);
    const std::string group = cond.substr(from, bar == std::string::npos ? std::string::npos : bar - from);
    bool all = true;
    size_t f = 0;
    while (true) {
      const size_t amp = group.find("&&", f);
      if (!Clause(group.substr(f, amp == std::string::npos ? std::string::npos : amp - f), flat, error)) {
        all = false;
        break;
      }
      if (amp == std::string::npos) break;
      f = amp + 2;
    }
    if (all) return true;
    if (bar == std::string::npos) return false;
    from = bar + 2;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// PNG (stored deflate: no compression library needed; ~2.7 MB for 1280x720)

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  static uint32_t table[256] = {};
  static const bool init = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    return true;
  }();
  (void)init;
  c = ~c;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

bool WritePng(const std::string& path, const rex::ui::RawImage& img) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t(img.width) * 3 + 1) * img.height);
  for (uint32_t y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const uint8_t* row = img.data.data() + y * img.stride;
    for (uint32_t x = 0; x < img.width; ++x) raw.insert(raw.end(), row + x * 4, row + x * 4 + 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last) break;
  }
  const uint32_t adler = b << 16 | a;
  for (int i = 3; i >= 0; --i) z.push_back(uint8_t(adler >> (8 * i)));
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto be32 = [&](uint32_t v) {
    uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    std::fwrite(x, 1, 4, f);
  };
  auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
    be32(uint32_t(n));
    std::vector<uint8_t> buf(type, type + 4);
    buf.insert(buf.end(), data, data + n);
    std::fwrite(buf.data(), 1, buf.size(), f);
    be32(Crc32(buf.data(), buf.size()));
  };
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::fwrite(sig, 1, 8, f);
  const uint8_t ihdr[13] = {uint8_t(img.width >> 24), uint8_t(img.width >> 16), uint8_t(img.width >> 8),
                            uint8_t(img.width),       uint8_t(img.height >> 24), uint8_t(img.height >> 16),
                            uint8_t(img.height >> 8), uint8_t(img.height),       8, 2, 0, 0, 0};
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z.data(), z.size());
  chunk("IEND", nullptr, 0);
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Connections

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kBad = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kBad = -1;
void CloseSocket(Socket s) { ::close(s); }
#endif

struct Client {
  Socket sock = kBad;
  std::mutex send_mutex;
  std::atomic<bool> alive{true};
  std::atomic<double> watch_hz{0};
  uint64_t next_watch_us = 0;
};

std::mutex g_clients_mutex;
std::vector<std::shared_ptr<Client>> g_clients;

bool Send(Client& c, const std::string& line) {
  if (!c.alive) return false;
  std::lock_guard lock(c.send_mutex);
  std::string data = line + "\n";
  size_t off = 0;
  while (off < data.size()) {
#ifdef _WIN32
    const int n = ::send(c.sock, data.data() + off, int(data.size() - off), 0);
#elif defined(MSG_NOSIGNAL)
    const ssize_t n = ::send(c.sock, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(c.sock, data.data() + off, data.size() - off, 0);
#endif
    if (n <= 0) {
      c.alive = false;
      return false;
    }
    off += size_t(n);
  }
  return true;
}

std::string Ok(const std::string& body = {}) { return "{\"ok\":true" + (body.empty() ? "" : "," + body) + "}"; }
std::string Fail(const std::string& error) { return "{\"ok\":false," + Str("error", error) + "}"; }

std::vector<std::string> Split(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  for (std::string w; ss >> w;) out.push_back(w);
  return out;
}

double Arg(const std::vector<std::string>& a, size_t i, double fallback) {
  return i < a.size() ? std::atof(a[i].c_str()) : fallback;
}

std::string Shot(const std::string& path) {
  auto* gfx = g_graphics ? g_graphics() : nullptr;
  auto* presenter = gfx ? gfx->presenter() : nullptr;
  rex::ui::RawImage img;
  if (!presenter || !presenter->CaptureGuestOutput(img) || !img.width) return Fail("no guest output");
  std::error_code ec;
  if (auto dir = std::filesystem::path(path).parent_path(); !dir.empty()) std::filesystem::create_directories(dir, ec);
  if (!WritePng(path, img)) return Fail("cannot write " + path);
  const std::string status = BuildStatus(false, nullptr);
  if (FILE* f = std::fopen((path + ".json").c_str(), "wb")) {
    std::fwrite(status.data(), 1, status.size(), f);
    std::fclose(f);
  }
  Event("shot", Str("path", path));
  return Ok(Str("path", path) + ",\"w\":" + std::to_string(img.width) + ",\"h\":" + std::to_string(img.height));
}

std::string Macro(const std::vector<std::string>& a) {
  if (a.size() < 2) return Fail("macro what?");
  auto& p = g_pad;
  const std::string& kind = a[1];
  std::lock_guard lock(p.m);
  const uint64_t now = NowUs();
  auto until = [&](size_t i, double def_ms) {
    const double ms = Arg(a, i, def_ms);
    return ms > 0 ? now + uint64_t(ms * 1000) : 0;
  };
  if (kind == "stop") {
    EndMacro(p, "stopped");
    p.fire_until = 0;
    g.macro_active = false;
    return Ok();
  }
  if (kind == "fire") {
    p.fire_until = until(2, 400);
    g.macro_active = true;
    return Ok();
  }
  if (p.macro != MacroKind::kNone) EndMacro(p, "replaced");
  if (kind == "move_to" && a.size() >= 4) {
    p.target[0] = float(Arg(a, 2, 0)), p.target[1] = float(Arg(a, 3, 0)), p.target[2] = float(Arg(a, 4, 0));
    p.macro_until = until(5, 10000);
    p.radius = float(Arg(a, 6, 120));
    p.macro = MacroKind::kMoveTo;
  } else if (kind == "aim_at" && a.size() >= 5) {
    p.target[0] = float(Arg(a, 2, 0)), p.target[1] = float(Arg(a, 3, 0)), p.target[2] = float(Arg(a, 4, 0));
    p.macro_until = until(5, 3000);
    p.macro = MacroKind::kAimAt;
  } else if (kind == "aim_nearest") {
    p.macro_until = until(2, 2000);
    p.macro = MacroKind::kAimNearest;
  } else if (kind == "path_to" && (a.size() >= 5 || (a.size() >= 3 && (a[2] == "objective" || a[2] == "hotspot")))) {
    float goal[3];
    size_t next = 5;
    if (a[2] == "objective") {
      std::lock_guard lock(g_snap_mutex);
      if (!g_snap.objective.present) return Fail("no objective");
      std::memcpy(goal, g_snap.objective.pos, sizeof(goal));
      next = 3;
    } else if (a[2] == "hotspot") {
      std::lock_guard lock(g_snap_mutex);
      if (!g_snap.hotspot.present) return Fail("no hotspot");
      std::memcpy(goal, g_snap.hotspot.pos, sizeof(goal));
      next = 3;
    } else {
      goal[0] = float(Arg(a, 2, 0)), goal[1] = float(Arg(a, 3, 0)), goal[2] = float(Arg(a, 4, 0));
    }
    p.macro_until = until(next, 120000);
    const bool exact = std::find(a.begin(), a.end(), "exact") != a.end();
    PathStart(goal, float(Arg(a, next + 1, 150)), !exact);
    p.macro = MacroKind::kPathTo;
  } else if (kind == "turn" && a.size() >= 3) {
    p.yaw_target = float(Arg(a, 2, 0));
    p.macro_until = until(3, 3000);
    p.macro = MacroKind::kTurn;
  } else {
    return Fail("macro move_to x y [z] [ms] [radius] | path_to x y z|objective|hotspot [ms] [radius] [exact] | aim_at x y z [ms] | "
                "aim_nearest [ms] | turn yaw [ms] | fire [ms] | stop");
  }
  g.macro_active = true;
  Event("macro", Str("name", kind) + "," + Str("end", "started"));
  return Ok(Str("macro", kind));
}

std::string Handle(Client& c, const std::string& line) {
  const auto a = Split(line);
  if (a.empty()) return {};
  const std::string& cmd = a[0];
  const std::string rest = line.size() > cmd.size() ? Trim(line.substr(line.find(cmd) + cmd.size())) : "";
  auto& p = g_pad;
  if (cmd == "status") return BuildStatus(false, nullptr);
  if (cmd == "keys") {
    std::map<std::string, std::string> flat;
    BuildStatus(false, &flat);
    std::string out = "{\"ok\":true,\"keys\":{";
    bool first = true;
    for (const auto& [k, v] : flat) {
      out += (first ? "\"" : ",\"") + k + "\":\"" + Escape(v) + "\"";
      first = false;
    }
    return out + "}}";
  }
  if (cmd == "watch") {
    c.watch_hz = Arg(a, 1, Cfg().hz);
    c.next_watch_us = 0;
    return Ok("\"watch_hz\":" + std::to_string(c.watch_hz.load()));
  }
  if (cmd == "hist") {
    std::string out = "{\"ok\":true,\"type\":\"hist\",\"t\":" + std::to_string(Secs(NowUs())) +
                      ",\"frames\":" + std::to_string(g_state.frames.load()) +
                      ",\"bucket_ms\":0.5,\"buckets\":[";
    bool first = true;
    for (uint32_t i = 0; i < InstrState::kBuckets; ++i)
      if (const uint32_t n = g_state.hist[i].load(std::memory_order_relaxed)) {
        out += (first ? "[" : ",[") + std::to_string(i) + "," + std::to_string(n) + "]";
        first = false;
      }
    return out + "]}";
  }
  if (cmd == "press" || cmd == "pad") {
    uint16_t mask = 0;
    if (a.size() < 2) return Fail("press BTN[+BTN] [ms]");
    if (cmd == "pad")
      mask = uint16_t(std::strtoul(a[1].c_str(), nullptr, 16));
    else if (!ParseButtons(a[1], mask))
      return Fail("unknown button " + a[1]);
    const uint32_t ms = uint32_t(std::clamp(Arg(a, 2, 150), 0.0, 60000.0));
    std::unique_lock lock(p.m);
    const uint64_t id = p.next_press++;
    p.presses.push_back(Press{mask, ms, false, false, 0, 0, 0, id});
    // Returns once it was pressed and released, or after ms + 10 s (the game isn't polling: loading, hung).
    const bool done = p.cv.wait_for(lock, std::chrono::milliseconds(ms + 10000), [&] {
      return std::none_of(p.presses.begin(), p.presses.end(), [&](const Press& e) { return e.id == id; });
    });
    if (!done) {
      p.presses.erase(std::remove_if(p.presses.begin(), p.presses.end(), [&](const Press& e) { return e.id == id; }),
                      p.presses.end());
      return Fail("not polled (game not reading the pad)");
    }
    return Ok();
  }
  if (cmd == "hold" || cmd == "release") {
    uint16_t mask = 0;
    if (a.size() < 2) return Fail(cmd + " BTN[+BTN]");
    std::lock_guard lock(p.m);
    if (cmd == "release" && a[1] == "all") {
      p.held = 0;
      p.stick[0][0] = p.stick[0][1] = p.stick[1][0] = p.stick[1][1] = 0;
      p.trig[0] = p.trig[1] = 0;
      p.presses.clear();
      p.cv.notify_all();
      EndMacro(p, "stopped");
      p.fire_until = 0;
      g.macro_active = false;
      return Ok();
    }
    // LT / RT are triggers (fully pulled while held), the rest buttons; "hold LT+A" mixes both.
    std::string buttons;
    std::stringstream ss(a[1]);
    for (std::string b; std::getline(ss, b, '+');) {
      for (auto& ch : b) ch = char(std::toupper(uint8_t(ch)));
      if (b == "LT" || b == "RT") {
        const int side = b == "RT";
        p.trig[side] = cmd == "hold" ? 1.0f : 0.0f, p.trig_until[side] = 0;
      } else {
        buttons += (buttons.empty() ? "" : "+") + b;
      }
    }
    if (!buttons.empty() && !ParseButtons(buttons, mask)) return Fail("unknown button " + a[1]);
    p.held = cmd == "hold" ? p.held | mask : p.held & ~mask;
    return Ok();
  }
  if (cmd == "stick" || cmd == "trigger") {
    if (a.size() < (cmd == "stick" ? 4u : 3u))
      return Fail(cmd == "stick" ? "stick L|R x y [ms]" : "trigger L|R v [ms]");
    const int side = (a[1] == "R" || a[1] == "r") ? 1 : 0;
    std::lock_guard lock(p.m);
    const uint64_t now = NowUs();
    if (cmd == "stick") {
      p.stick[side][0] = float(Arg(a, 2, 0)), p.stick[side][1] = float(Arg(a, 3, 0));
      const double ms = Arg(a, 4, 0);
      p.stick_until[side] = ms > 0 ? now + uint64_t(ms * 1000) : 0;
    } else {
      p.trig[side] = float(Arg(a, 2, 0));
      const double ms = Arg(a, 3, 0);
      p.trig_until[side] = ms > 0 ? now + uint64_t(ms * 1000) : 0;
    }
    return Ok();
  }
  if (cmd == "exec") {
    if (rest.empty()) return Fail("exec <command>");
    std::lock_guard lock(g_cmd_mutex);
    g_commands.push_back(rest);
    return Ok(Str("queued", rest));
  }
  if (cmd == "dualshot") {
    if (a.size() < 2) return Fail("dualshot <tag>");
    const char* dual = std::getenv("BOTS_HYBRID_DUAL");
    if (!dual || !std::atoi(dual)) return Fail("dualshot needs the game started with BOTS_HYBRID_DUAL=1");
    const char* dir = std::getenv("BOTS_HYBRID_DUAL_DIR");
    std::lock_guard lock(g_cmd_mutex);
    g_commands.push_back("hybrid dualshot " + a[1]);
    // The pair lands in BOTS_HYBRID_DUAL_DIR once n1 has finished streaming (a "dualshot" event names the files).
    return Ok(Str("dir", dir ? dir : "dual"));
  }
  if (cmd == "wait") {
    std::string cond = rest;
    double timeout = 60;
    if (a.size() >= 3 && a[1] == "-t") {
      timeout = std::atof(a[2].c_str());
      cond = Trim(rest.substr(rest.find(a[2]) + a[2].size()));
    }
    if (cond.empty()) return Fail("wait [-t secs] <cond>");
    const uint64_t start = NowUs(), deadline = start + uint64_t(timeout * 1e6);
    while (true) {
      std::map<std::string, std::string> flat;
      std::string error;
      const std::string status = BuildStatus(false, &flat);
      if (Condition(cond, flat, error))
        return "{\"ok\":true,\"waited_s\":" + std::to_string(double(NowUs() - start) / 1e6) + ",\"status\":" + status +
               "}";
      if (!error.empty()) return Fail(error);
      if (NowUs() >= deadline || !c.alive)
        return "{\"ok\":false,\"error\":\"timeout\",\"waited_s\":" + std::to_string(double(NowUs() - start) / 1e6) +
               ",\"status\":" + status + "}";
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (cmd == "shot") return a.size() < 2 ? Fail("shot <path.png>") : Shot(rest);
  if (cmd == "burst") {
    if (a.size() < 3) return Fail("burst <dir> <count>");
    if (!g_state.frames) return Fail("burst needs the native renderer");
    std::snprintf(g_state.burst_dir, sizeof(g_state.burst_dir), "%s", a[1].c_str());
    g_state.burst_count = uint32_t(std::clamp(Arg(a, 2, 30), 1.0, 600.0));
    g_state.burst_seq.fetch_add(1, std::memory_order_release);
    return Ok(Str("dir", a[1]));
  }
  if (cmd == "macro") return Macro(a);
  if (cmd == "enemies") {
    const int n = int(Arg(a, 1, 8));
    std::string r = OnGameThread([n] {
      Snap s;
      Sample(s, true);
      std::string e = "{\"ok\":true,\"pawns\":" + std::to_string(s.pawns) + ",\"enemies\":[";
      int i = 0;
      for (const auto& en : s.enemies) {
        if (i++ == n) break;
        char buf[240];
        std::snprintf(buf, sizeof(buf),
                      "%s{\"x\":%.0f,\"y\":%.0f,\"z\":%.0f,\"dist\":%.0f,\"bearing\":%.1f,\"pitch\":%.1f,\"health\":%d,"
                      "\"player\":%s,\"team\":%d,\"foe\":%s,\"los\":%d,\"class\":\"",
                      i > 1 ? "," : "", en.pos[0], en.pos[1], en.pos[2], en.dist, en.bearing, en.pitch, en.health,
                      en.player ? "true" : "false", en.team, en.foe ? "true" : "false", en.los);
        e += buf + Escape(en.cls) + "\"}";
      }
      return e + "]}";
    });
    return r.empty() ? Fail("game thread busy") : r;
  }
  if (cmd == "pathdrop") {  // pathdrop [uu]: the deepest drop path_to will plan (default 260)
    if (a.size() > 1) g_max_drop = float(std::clamp(Arg(a, 1, 260), 0.0, 2000.0));
    return Ok("\"drop\":" + std::to_string(int(g_max_drop.load())));
  }
  if (cmd == "pathflags") {  // pathflags [N]: the trace flags path_to's walkability checks use (UE3 ETraceFlags)
    if (a.size() > 1) g_walk_flags = uint32_t(std::strtoul(a[1].c_str(), nullptr, 0));
    return Ok("\"flags\":" + std::to_string(g_walk_flags.load()));
  }
  if (cmd == "trace" || cmd == "floor") {
    // trace x1 y1 z1 x2 y2 z2 [extent] [flags]: one collision trace (a box sweep with extent; UE3 ETraceFlags, default
    // the world without pawns or volumes, as Actor.FastTrace); floor x y [z]: the floor
    // under a point. Debugging aids for path_to and line of sight.
    std::vector<double> v;
    for (size_t i = 1; i < a.size(); ++i) v.push_back(Arg(a, i, 0));
    if ((cmd == "trace" && v.size() < 6) || (cmd == "floor" && v.size() < 2))
      return Fail("trace x1 y1 z1 x2 y2 z2 [extent] | floor x y [z]");
    std::string r = OnGameThread([cmd, v] {
      const uint32_t pc = LocalController();
      const uint32_t pawn = pc ? PropObj(pc, "Pawn") : 0;
      char buf[256];
      if (cmd == "floor") {
        const float z = v.size() > 2 ? float(v[2]) : 100000.0f;
        const float f = Floor(float(v[0]), float(v[1]), z, z - (v.size() > 2 ? 2000.0f : 200000.0f), pawn);
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"floor\":%s}", std::isnan(f) ? "null" : std::to_string(f).c_str());
        return std::string(buf);
      }
      const float s[3] = {float(v[0]), float(v[1]), float(v[2])}, e[3] = {float(v[3]), float(v[4]), float(v[5])};
      const float ext = v.size() > 6 ? float(v[6]) : 0;
      const float extent[3] = {ext, ext, ext};
      const uint32_t flags = v.size() > 7 ? uint32_t(v[7]) : kTraceWorld;
      Hit h;
      const uint64_t t0 = NowUs();
      if (!Trace(s, e, ext > 0 ? extent : nullptr, pawn, h, flags)) return std::string("{\"ok\":false,\"error\":\"no world\"}");
      std::snprintf(buf, sizeof(buf),
                    "{\"ok\":true,\"hit\":%s,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"nx\":%.2f,\"ny\":%.2f,\"nz\":%.2f,"
                    "\"time\":%.3f,\"us\":%llu,\"actor\":\"",
                    h.hit ? "true" : "false", h.loc[0], h.loc[1], h.loc[2], h.normal[0], h.normal[1], h.normal[2], h.time,
                    static_cast<unsigned long long>(NowUs() - t0));
      return buf + Escape(h.hit && Readable(h.actor) ? ObjName(h.actor) : "") + "\"}";
    });
    return r.empty() ? Fail("game thread busy") : r;
  }
  if (cmd == "inspect") {
    const std::string what = a.size() > 1 ? a[1] : "pawn", filter = a.size() > 2 ? a[2] : "";
    std::string r = OnGameThread([what, filter] {
      const uint32_t pc = LocalController();
      uint32_t o = 0;
      if (what == "pc") o = pc;
      else if (what == "pawn") o = PropObj(pc, "Pawn");
      else if (what == "weapon") o = PropObj(PropObj(pc, "Pawn"), "Weapon");
      else if (what == "world") o = PropObj(pc, "WorldInfo");
      else if (what == "game") o = PropObj(PropObj(pc, "WorldInfo"), "Game");
      else if (what == "camera") o = PropObj(pc, "PlayerCamera");
      else if (what == "engine") o = U32(kGEngine);
      else if (what == "hud") o = PropObj(pc, "myHUD");
      else if (what.rfind("0x", 0) == 0) o = uint32_t(std::strtoul(what.c_str() + 2, nullptr, 16));
      // "inspect OBJ Prop.": the members of struct property Prop (UStructProperty +0x84 is the struct)
      if (!filter.empty() && filter.back() == '.') {
        const Field f = Prop(o, filter.substr(0, filter.size() - 1).c_str());
        if (!f.ok() || std::strcmp(ClassName(f.prop), "StructProperty")) return Fail("no such struct property");
        return Inspect(o + f.offset, "", U32(f.prop + 0x84));
      }
      return Inspect(o, filter);
    });
    return r.empty() ? Fail("game thread busy") : r;
  }
  if (cmd == "objs") {  // objs <Class> [n]: live objects of that class (or a subclass), with locations for actors
    if (a.size() < 2) return Fail("usage: objs CLASS [n]");
    const std::string cls = a[1];
    const uint32_t want = uint32_t(Arg(a, 2, 50));
    std::string r = OnGameThread([cls, want] {
      std::string out;
      uint32_t n = 0;
      ForEachObject(cls.c_str(), [&](uint32_t o) {
        if (n++ >= want) return;
        float loc[3] = {};
        char buf[96];
        const bool has = PropVec(o, "Location", loc);
        std::snprintf(buf, sizeof(buf), "@%08X", o);
        out += std::string(n > 1 ? "," : "") + "\"" + Escape(ObjName(o)) + ":" + ClassName(o) + buf;
        if (has) std::snprintf(buf, sizeof(buf), " %.0f %.0f %.0f", loc[0], loc[1], loc[2]), out += buf;
        out += "\"";
      });
      return "{\"ok\":true,\"count\":" + std::to_string(n) + ",\"objs\":[" + out + "]}";
    }, std::chrono::seconds(20));
    return r.empty() ? Fail("game thread busy") : r;
  }
  if (cmd == "elems") {  // elems <0xobject> <ArrayProperty> [stride] [n]: an array's words, as objects where they are
    if (a.size() < 3) return Fail("usage: elems 0xOBJECT PROP [stride] [n]");
    const uint32_t o = uint32_t(std::strtoul(a[1].c_str() + (a[1].rfind("0x", 0) == 0 ? 2 : 0), nullptr, 16));
    const std::string prop = a[2];
    const uint32_t stride = uint32_t(Arg(a, 3, 4)), want = uint32_t(Arg(a, 4, 32));
    std::string r = OnGameThread([o, prop, stride, want] {
      const Field f = Readable(o) ? Prop(o, prop.c_str()) : Field{};
      if (!f.ok()) return Fail("no such array");
      const uint32_t data = U32(o + f.offset), count = U32(o + f.offset + 4);
      std::string out = "{\"ok\":true,\"count\":" + std::to_string(count) + ",\"elems\":[";
      for (uint32_t i = 0; Readable(data) && i < count && i < want; ++i) {
        std::string e;
        char buf[48];
        for (uint32_t w = 0; w < stride; w += 4) {
          const uint32_t v = U32(data + i * stride + w);
          if (Readable(v) && Readable(U32(v + 0x34)))
            e += (e.empty() ? "" : " ") + ObjName(v) + ":" + ClassName(v) + "@" + (std::snprintf(buf, sizeof(buf), "%08X", v), buf);
          else
            std::snprintf(buf, sizeof(buf), "%s%08X/%g", e.empty() ? "" : " ", v, F32(data + i * stride + w)), e += buf;
        }
        out += (i ? ",\"" : "\"") + Escape(e) + "\"";
      }
      return out + "]}";
    });
    return r.empty() ? Fail("game thread busy") : r;
  }
  if (cmd == "log") {
    const size_t n = size_t(Arg(a, 1, 30));
    const std::string filter = a.size() > 2 ? a[2] : "";
    std::vector<std::string> lines;
    {
      std::lock_guard lock(g_ring_mutex);
      for (auto it = g_ring.rbegin(); it != g_ring.rend() && lines.size() < n; ++it)
        if (filter.empty() || it->find(filter) != std::string::npos) lines.push_back(*it);
    }
    std::string out = "{\"ok\":true,\"lines\":[";
    for (size_t i = lines.size(); i-- > 0;) out += "\"" + Escape(lines[i]) + (i ? "\"," : "\"");
    return out + "]}";
  }
  if (cmd == "quit") {
    Event("quit");
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      RequestQuit();
    }).detach();
    return Ok();
  }
  if (cmd == "close") {
    c.alive = false;
    return {};
  }
  if (cmd == "help") return Ok(Str("help", "status keys watch hist press hold release stick trigger pad exec wait shot "
                                           "burst dualshot macro enemies trace floor pathdrop pathflags inspect objs elems log quit close"));
  return Fail("unknown command " + cmd);
}

void ClientThread(std::shared_ptr<Client> c) {
  std::string buf;
  char chunk[1024];
  while (c->alive) {
#ifdef _WIN32
    const int n = ::recv(c->sock, chunk, sizeof(chunk), 0);
#else
    const ssize_t n = ::recv(c->sock, chunk, sizeof(chunk), 0);
#endif
    if (n <= 0) break;
    buf.append(chunk, size_t(n));
    for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string reply = Handle(*c, line);
      if (!reply.empty()) Send(*c, reply);
      if (!c->alive) break;
    }
  }
  c->alive = false;
  CloseSocket(c->sock);
  std::lock_guard lock(g_clients_mutex);
  g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), c), g_clients.end());
}

void ServerThread() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const auto& cfg = Cfg();
  Socket server = ::socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(uint16_t(cfg.port));
  if (inet_pton(AF_INET, cfg.bind.c_str(), &addr.sin_addr) != 1) addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (server == kBad || ::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(server, 8) != 0) {
    REXLOG_ERROR("instr: cannot listen on {}:{}", cfg.bind, cfg.port);
    return;
  }
  REXLOG_INFO("instr: control channel on {}:{}", cfg.bind, cfg.port);
  while (true) {
    Socket s = ::accept(server, nullptr, nullptr);
    if (s == kBad) continue;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
#ifdef _WIN32
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{3, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    auto c = std::make_shared<Client>();
    c->sock = s;
    {
      std::lock_guard lock(g_clients_mutex);
      g_clients.push_back(c);
    }
    std::thread(ClientThread, c).detach();
  }
}

// Every 100 ms: phase changes and events; status lines at BOTS_INSTR_HZ to the log, at each watcher's rate to it.
void StatusThread() {
  FILE* log = nullptr;
  if (!Cfg().log.empty()) {
    std::error_code ec;
    if (auto dir = std::filesystem::path(Cfg().log).parent_path(); !dir.empty())
      std::filesystem::create_directories(dir, ec);
    log = std::fopen(Cfg().log.c_str(), "ab");
    if (!log) REXLOG_ERROR("instr: cannot open {}", Cfg().log);
  }
  const uint64_t period = uint64_t(1e6 / Cfg().hz);
  uint64_t next_line = NowUs();
  uint64_t human = MouseKeyboardDriver::HumanEvents();
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t now = NowUs();
    // Keyboard / mouse input the control channel didn't send: someone is at the machine (the run is contaminated).
    if (const uint64_t h = MouseKeyboardDriver::HumanEvents(); h != human) {
      Event("human_input", "\"events\":" + std::to_string(h - human) + ",\"total\":" + std::to_string(h));
      human = h;
    }
    std::vector<std::shared_ptr<Client>> clients;
    {
      std::lock_guard lock(g_clients_mutex);
      clients = g_clients;
    }
    std::string line;
    if (now >= next_line) {
      line = BuildStatus(true, nullptr);  // also turns phase changes into events
      next_line += period;
      if (next_line < now) next_line = now + period;
    } else {
      // Phase only, for prompt phase events: a cheap status without advancing the window.
      BuildStatus(false, nullptr);
    }
    std::deque<std::string> events;
    {
      std::lock_guard lock(g_event_mutex);
      events.swap(g_events);
    }
    if (log) {
      for (const auto& e : events) std::fprintf(log, "%s\n", e.c_str());
      if (!line.empty()) std::fprintf(log, "%s\n", line.c_str());
      if (!line.empty() || !events.empty()) std::fflush(log);
    }
    for (auto& c : clients) {
      const double hz = c->watch_hz;
      if (hz <= 0 || !c->alive) continue;
      for (const auto& e : events) Send(*c, e);
      if (now >= c->next_watch_us) {
        c->next_watch_us = now + uint64_t(1e6 / std::clamp(hz, 0.1, 10.0));
        Send(*c, line.empty() || hz != Cfg().hz ? BuildStatus(false, nullptr) : line);
      }
    }
  }
}

}  // namespace

bool Enabled() { return g_enabled; }

void Start(std::function<rex::system::IGraphicsSystem*()> graphics) {
  if (!g_enabled) return;
  static bool started = false;
  if (started) return;
  started = true;
  g_graphics = std::move(graphics);
  rex::AddSink(std::make_shared<LogSink>());
  std::thread(ServerThread).detach();
  std::thread(StatusThread).detach();
  REXLOG_INFO("instr: on (status {} Hz{}{}, world {})", Cfg().hz, Cfg().log.empty() ? "" : ", log ", Cfg().log,
              Cfg().world ? "on" : "off");
}

std::unique_ptr<rex::input::InputDriver> MakeInputDriver() { return std::make_unique<InstrInputDriver>(); }

void GameTick(PPCContext& ctx, uint8_t* base) {
  if (!g_enabled) return;
  g_base = base;
  g_ctx = &ctx;
  struct Clear {
    ~Clear() { g_ctx = nullptr; }
  } clear;
  const uint64_t now = NowUs();
  g.ticks.fetch_add(1, std::memory_order_relaxed);
  g.last_tick_us.store(now, std::memory_order_relaxed);
  if (g_jobs_pending.load(std::memory_order_relaxed)) RunJobs();
  static uint64_t next_sample = 0;
  const bool steering = g.macro_active.load(std::memory_order_relaxed);
  if (steering) PathTick(now);
  if (now < next_sample && !steering) return;
  const bool world = Cfg().world || steering;
  next_sample = now + (Cfg().world ? 250000 : 200000);
  Snap s;
  Sample(s, world);
  std::lock_guard lock(g_snap_mutex);
  g_snap = std::move(s);
}

void HudTick() {
  if (g_enabled) g.last_hud_us.store(NowUs(), std::memory_order_relaxed);
}

void SetPaused(bool paused) {
  if (!g_enabled) return;
  g.paused = paused;
  Event(paused ? "pause" : "unpause");
}

void BrowseBegin(const std::string& url) {
  if (!g_enabled) return;
  g.in_browse = true;
  g.browse_start_us = NowUs();
  {
    std::lock_guard lock(g.text_mutex);
    g.last_url = url;
  }
  Event("load_begin", Str("url", url));
}

void BrowseEnd() {
  if (!g_enabled) return;
  const double s = double(NowUs() - g.browse_start_us) / 1e6;
  g.in_browse = false;
  ++g.loads;
  {
    std::lock_guard lock(g.text_mutex);
    g.last_load_s = s;
    if (!g.pending_chapter.empty()) g.chapter = std::move(g.pending_chapter), g.pending_chapter.clear();
  }
  g.last_load_end_us = NowUs();
  Event("load_end", "\"secs\":" + std::to_string(s));
}

void Travel(const std::string& url) {
  if (g_enabled) Event("travel", Str("url", url));
}

void TakeCommands(std::deque<std::string>& out) {
  if (!g_enabled) return;
  std::lock_guard lock(g_cmd_mutex);
  while (!g_commands.empty()) out.push_back(std::move(g_commands.front())), g_commands.pop_front();
}

void NoteCommand(const std::string& cmd) {
  if (!g_enabled) return;
  {
    std::lock_guard lock(g.text_mutex);
    g.last_exec = cmd;
    // The chapter counts as loaded once the map load it starts has finished (BrowseEnd).
    if (Lower(cmd).rfind("loadchapter ", 0) == 0) g.pending_chapter = Trim(cmd.substr(12));
  }
  Event("exec", Str("cmd", cmd));
}

}  // namespace bots::instr
