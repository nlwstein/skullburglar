// Test range (B-299). Opt-in (BOTS_RANGE_DIR), no effect when unset.
//
// Console commands (BOTS_EXEC, BOTS_EXEC_FILE or the instr control channel's `exec`):
//   range spawn <Class> <n> <dist> <spacing> [yaw [dz]]
//        Summons n copies of C1GameNonNative.<Class> in a row `dist` units ahead of where the player stands now,
//        facing `yaw` (default: the player's own), `spacing` apart, `dz` above (or below) the player's height. `Summon` pawns have no controller, so they stand
//        still and never attack. The player ends up back where they started. Needs EnableCheats first.
//   range settle <Class> let that class's floating summoned pawns fall to the floor (`range spawn` does it)
//   range log <name>     close the current log and start <BOTS_RANGE_DIR>/<name>.jsonl
//   range mark <text>    write a marker line into the log
//   range where          log the player's position and yaw
//
// Damage log, one JSON line per event, from a hook on the script VM (ProcessInternal):
//   {"ev":"hit",  "t":ms,"victim":..,"cls":..,"dmg":..,"type":..,"bone":..,"head":0|1,"hp0":..,"hp1":..,"dead":0|1,
//    "lrt":victim LastRenderTime,"now":WorldInfo.TimeSeconds,"lod":..,"lod_old":..,"nokin":..,"dist":..}
//   {"ev":"shot", "t":ms,"actor":..,"bone":..}      a weapon's ProcessInstantHit: what the trace hit and where
#include "test_range.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string_view>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "range_math.h"

extern "C" void __imp__sub_82686B28(PPCContext& __restrict ctx, uint8_t* base);

namespace bots::testrange {
namespace {

const char* Dir() {
  static const char* d = [] {
    const char* v = std::getenv("BOTS_RANGE_DIR");
    return v && *v ? v : nullptr;
  }();
  return d;
}
const bool g_on = Dir() != nullptr;

constexpr uint32_t kGEngine = 0x8380D380;
constexpr uint32_t kNames = 0x837F98A0;
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

struct Field {
  uint32_t offset = 0xFFFFFFFF, mask = 0, prop = 0;
  bool ok() const { return offset != 0xFFFFFFFF; }
};
// A property of a class / struct / function (walks the super chain; functions have none).
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
bool PropNum(uint32_t o, const char* name, double& out) {
  const Field f = Prop(o, name);
  if (!f.ok()) return false;
  out = !std::strcmp(ClassName(f.prop), "FloatProperty") ? double(F32(o + f.offset)) : double(int32_t(U32(o + f.offset)));
  return true;
}
double Num(uint32_t o, const char* name, double dflt = -1) {
  double v;
  return PropNum(o, name, v) ? v : dflt;
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
// A StructProperty's member, as an offset from the start of the struct value.
Field Member(const Field& s, const char* name) {
  if (!s.ok() || std::strcmp(ClassName(s.prop), "StructProperty")) return {};
  return FindProp(U32(s.prop + 0x84), name);
}

std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) o += (c == '"' || c == '\\') ? std::string("_") : std::string(1, c);
  return o;
}

// ---- log -----------------------------------------------------------------------------------------------------
std::ofstream g_log;
const auto g_t0 = std::chrono::steady_clock::now();
long Ms() { return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count()); }

void OpenLog(const std::string& name) {
  if (g_log.is_open()) g_log.close();
  g_log.open(std::string(Dir()) + "/" + name + ".jsonl", std::ios::app);
}
void Line(const std::string& s) {
  if (!g_log.is_open()) OpenLog("range");
  g_log << s << '\n';
  g_log.flush();
}

// ---- the VM hook ---------------------------------------------------------------------------------------------
struct FnInfo {
  enum Kind { None, TakeDamage, InstantHit } kind = None;
  Field damage, damage_type, hit_info, causer, instigator, impact;
};
FnInfo& Classify(uint32_t fn) {
  static std::unordered_map<uint32_t, FnInfo> cache;
  auto it = cache.find(fn);
  if (it != cache.end()) return it->second;
  FnInfo info;
  const char* n = Readable(fn) ? NameText(U32(fn + 0x2C)) : "?";
  if (!std::strcmp(n, "TakeDamage")) {
    info.kind = FnInfo::TakeDamage;
    info.damage = FindProp(fn, "Damage");
    info.damage_type = FindProp(fn, "DamageType");
    info.hit_info = FindProp(fn, "HitInfo");
    info.causer = FindProp(fn, "DamageCauser");
    info.instigator = FindProp(fn, "InstigatedBy");
  } else if (!std::strcmp(n, "ProcessInstantHit")) {
    info.kind = FnInfo::InstantHit;
    info.impact = FindProp(fn, "Impact");
  }
  return cache.emplace(fn, info).first->second;
}

std::string BoneOf(uint32_t locals, const Field& info) {
  const Field bone = Member(info, "BoneName");
  if (!info.ok() || !bone.ok()) return "?";
  return NameText(U32(locals + info.offset + bone.offset));
}

uint32_t g_shot = 0;    // the fire mode whose outermost ProcessInstantHit is running
uint32_t g_victim = 0;  // the pawn whose outermost TakeDamage is running (the super chain re-enters it)
std::unordered_map<uint32_t, bots::range::Kill> g_kills;

void OnTakeDamage(uint32_t pawn, uint32_t locals, const FnInfo& fi, void (*run)(void*), void* arg) {
  const double hp0 = Num(pawn, "Health");
  const std::string bone = BoneOf(locals, fi.hit_info);
  run(arg);
  const double hp1 = Num(pawn, "Health");
  const double dmg = fi.damage.ok() ? double(int32_t(U32(locals + fi.damage.offset))) : -1;
  const uint32_t dtype = fi.damage_type.ok() ? U32(locals + fi.damage_type.offset) : 0;
  const uint32_t causer = fi.causer.ok() ? U32(locals + fi.causer.offset) : 0;
  const uint32_t mesh = PropObj(pawn, "Mesh"), world = PropObj(pawn, "WorldInfo");
  const bool head = bone.find("Head") != std::string::npos || bone.find("head") != std::string::npos;
  float p[3] = {0, 0, 0}, q[3] = {0, 0, 0};
  PropVec(pawn, "Location", p);
  const double dist = Readable(causer) && PropVec(causer, "Location", q)
                          ? std::sqrt(double(p[0] - q[0]) * (p[0] - q[0]) + double(p[1] - q[1]) * (p[1] - q[1]) +
                                      double(p[2] - q[2]) * (p[2] - q[2]))
                          : -1;
  const bool dead = hp1 <= 0;
  bots::range::Note(g_kills[pawn], float(dmg), head, dead);
  char buf[768];
  std::snprintf(buf, sizeof(buf),
                "{\"ev\":\"hit\",\"t\":%ld,\"victim\":\"%s\",\"cls\":\"%s\",\"dmg\":%.0f,\"type\":\"%s\",\"bone\":\"%s\",\"head\":%d,"
                "\"hp0\":%.0f,\"hp1\":%.0f,\"dead\":%d,\"lrt\":%.3f,\"now\":%.3f,\"lod\":%.0f,\"lod_old\":%.0f,\"nokin\":%d,"
                "\"mesh_lrt\":%.3f,\"causer\":\"%s\",\"dist\":%.0f,\"vpos\":[%.0f,%.0f,%.0f]}",
                Ms(), Esc(ObjName(pawn)).c_str(), ClassName(pawn), dmg, Esc(ObjName(dtype)).c_str(), Esc(bone).c_str(),
                head ? 1 : 0, hp0, hp1, dead ? 1 : 0, Num(pawn, "LastRenderTime"), Num(world, "TimeSeconds"),
                Num(mesh, "PredictedLODLevel"), Num(mesh, "OldPredictedLODLevel"),
                PropBool(mesh, "bNotUpdatingKinematicDueToDistance") ? 1 : 0, Num(mesh, "LastRenderTime"),
                Esc(ClassName(causer)).c_str(), dist, p[0], p[1], p[2]);
  Line(buf);
}

// Player pawn: GEngine -> GamePlayers[0] -> Actor (PlayerController) -> Pawn.
uint32_t PlayerController() {
  const uint32_t engine = U32(kGEngine);
  if (!Readable(engine)) return 0;
  const uint32_t players = U32(engine + 728);
  if (!Readable(players) || !U32(engine + 732)) return 0;
  return PropObj(U32(players), "Actor");
}
bool Yaw(uint32_t o, int32_t& yaw) {
  const Field f = Prop(o, "Rotation");
  if (!f.ok()) return false;
  yaw = int32_t(U32(o + f.offset + 4));
  return true;
}

}  // namespace

bool Enabled() { return g_on; }

bool Command(uint8_t* base, const std::string& cmd, std::vector<std::string>& expand) {
  if (cmd.rfind("range ", 0) != 0) return false;
  if (!g_on) {
    REXLOG_WARN("range: BOTS_RANGE_DIR is not set, ignored: {}", cmd);
    return true;
  }
  g_base = base;
  const std::string rest = cmd.substr(6);
  if (rest.rfind("log ", 0) == 0) {
    OpenLog(rest.substr(4));
  } else if (rest.rfind("mark", 0) == 0) {
    Line("{\"ev\":\"mark\",\"t\":" + std::to_string(Ms()) + ",\"text\":\"" + Esc(rest.size() > 5 ? rest.substr(5) : "") + "\"}");
  } else if (rest.rfind("settle ", 0) == 0) {
    // `Summon` leaves its pawn floating (Physics None) until the first hit drops it to the floor, which moves the
    // target mid-trial: let it fall now (Physics Falling; it lands as Walking) so every hit sees the same pawn.
    const std::string klass = rest.substr(7);
    const uint32_t world = PropObj(PlayerController(), "WorldInfo");
    const Field physics = Prop(PropObj(PlayerController(), "Pawn"), "Physics");
    int settled = 0;
    for (uint32_t p = PropObj(world, "PawnList"), n = 0; p && n < 512; p = PropObj(p, "NextPawn"), ++n) {
      const Field f = Prop(p, "Physics");
      if (klass != ClassName(p) || !f.ok() || !physics.ok() || *Raw(p + f.offset) != 0) continue;
      *Raw(p + f.offset) = 2;
      ++settled;
    }
    REXLOG_INFO("range: {} {} pawns set falling", settled, klass);
  } else if (rest.rfind("setloc ", 0) == 0) {
    // setloc x y z [yaw_degrees]: teleport the player (BugItGo). A spot inside the floor is refused by the engine: add ~30 to z.
    float x = 0, y = 0, z = 0, yaw_deg = 0;
    const int got = std::sscanf(rest.c_str(), "setloc %f %f %f %f", &x, &y, &z, &yaw_deg);
    if (got < 3) {
      REXLOG_WARN("range: setloc x y z [yaw_degrees]");
      return true;
    }
    const int yaw_units = int(yaw_deg * 65536.0f / 360.0f);
    expand.push_back(bots::range::BugItGo(bots::range::Spot{x, y, z}, yaw_units));
  } else if (rest == "where" || rest.rfind("spawn ", 0) == 0) {
    const uint32_t pc = PlayerController(), pawn = PropObj(pc, "Pawn");
    float pos[3] = {0, 0, 0};
    int32_t yaw = 0;
    if (!pawn || !PropVec(pawn, "Location", pos) || !Yaw(pc, yaw)) {
      REXLOG_WARN("range: no player pawn yet");
      return true;
    }
    if (rest == "where") {
      REXLOG_INFO("range: player at {:.1f} {:.1f} {:.1f} yaw {}", pos[0], pos[1], pos[2], yaw);
      return true;
    }
    char klass[96] = {0};
    int n = 1;
    float dist = 600, spacing = 150, dz = 0;
    int yaw_arg = yaw;
    if (std::sscanf(rest.c_str(), "spawn %95s %d %f %f %d %f", klass, &n, &dist, &spacing, &yaw_arg, &dz) < 1) return true;
    yaw = yaw_arg;
    n = n < 1 ? 1 : (n > 16 ? 16 : n);
    const bots::range::Spot origin{pos[0], pos[1], pos[2]}, level{pos[0], pos[1], pos[2] + dz};
    for (int i = 0; i < n; ++i) {
      const auto target = bots::range::TargetPos(level, yaw, dist, spacing, i, n);
      expand.push_back(bots::range::BugItGo(bots::range::SummonStand(target, yaw), yaw));
      expand.push_back(std::string("Summon C1GameNonNative.") + klass);
    }
    expand.push_back(bots::range::BugItGo(origin, yaw));
    expand.push_back("Walk");
    expand.push_back(std::string("range settle ") + klass);
  }
  return true;
}

}  // namespace bots::testrange

// ProcessInternal: every script function call. r3 = the object, r4 = the FFrame (+4 Node, +0x10 Locals), r5 = result.
extern "C" void sub_82686B28(PPCContext& __restrict ctx, uint8_t* base) {
  using namespace bots::testrange;
  if (!g_on) return __imp__sub_82686B28(ctx, base);
  g_base = base;
  const uint32_t self = ctx.r3.u32, frame = ctx.r4.u32;
  if (!Readable(self) || !Readable(frame)) return __imp__sub_82686B28(ctx, base);
  const uint32_t fn = U32(frame + 4), locals = U32(frame + 0x10);
  const FnInfo& fi = Classify(fn);
  if (fi.kind == FnInfo::None) return __imp__sub_82686B28(ctx, base);
  if (fi.kind == FnInfo::InstantHit) {
    const Field actor = Member(fi.impact, "HitActor"), info = Member(fi.impact, "HitInfo");
    const uint32_t hit = actor.ok() ? U32(locals + fi.impact.offset + actor.offset) : 0;
    const Field bone = Member(info, "BoneName");
    const Field loc = Member(fi.impact, "HitLocation");
    float hl[3] = {0, 0, 0};
    if (loc.ok())
      for (int i = 0; i < 3; ++i) hl[i] = F32(locals + fi.impact.offset + loc.offset + 4 * i);
    if (g_shot != self && ClassChainHas(hit, "Pawn")) {
      char buf[320];
      std::snprintf(buf, sizeof(buf),
                    "{\"ev\":\"shot\",\"t\":%ld,\"actor\":\"%s\",\"bone\":\"%s\",\"at\":[%.0f,%.0f,%.0f]}", Ms(),
                    Esc(ObjName(hit)).c_str(),
                    info.ok() && bone.ok() ? NameText(U32(locals + fi.impact.offset + info.offset + bone.offset)) : "?",
                    hl[0], hl[1], hl[2]);
      Line(buf);
    }
    const uint32_t outer = g_shot;
    g_shot = self;
    __imp__sub_82686B28(ctx, base);
    g_shot = outer;
    return;
  }
  if (g_victim == self) return __imp__sub_82686B28(ctx, base);  // a super call of the hit being logged
  if (!fi.instigator.ok() || U32(locals + fi.instigator.offset) != PlayerController() || !ClassChainHas(self, "Pawn"))
    return __imp__sub_82686B28(ctx, base);  // only what the player does to pawns
  g_victim = self;
  struct Run {
    PPCContext* ctx;
    uint8_t* base;
  } run{&ctx, base};
  OnTakeDamage(
      self, locals, fi, [](void* a) { auto* r = static_cast<Run*>(a); __imp__sub_82686B28(*r->ctx, r->base); }, &run);
  g_victim = 0;
}
