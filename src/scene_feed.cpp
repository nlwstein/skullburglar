// Scene feed (research spike for a renderer that draws the UE3 scene itself instead of replaying the GPU stream).
//
// Reads, on the game thread once per frame, every primitive component attached to the renderer's scene (UE3's
// UPrimitiveComponent with a non-null SceneInfo) from the live object table, plus the player's camera, and writes
// one JSON file per frame. Every field is found by name through UE3's own reflection data (UProperty +0x64 Offset),
// so nothing below depends on a class layout except the object header and the reflection records themselves.
//
// Object layout (this build, as used elsewhere in src/): UObject +0x04 Index, +0x28 Outer, +0x2C Name (FName: index,
// number), +0x34 Class; UField +0x3C SuperField, +0x40 Next; UStruct +0x50 Children; UProperty +0x64 Offset;
// UBoolProperty +0x84 BitMask. FName::Names is a TArray<FNameEntry*> at 0x837F98A0, an entry's text at +8.
// The object table (UObject::GObjObjects, a TArray<UObject*>) is found once by scanning the image's data for an
// array whose entries' Index fields count up from 0.
//
// Commands (BOTS_EXEC / BOTS_EXEC_FILE; output under BOTS_SCENEFEED_DIR, default "scenefeed"):
//   scenefeed dump <frames> [every]  write frame_<n>.json for the next <frames> frames (every <every>th frame)
//   scenefeed time <frames> [cached] walk without writing and log the cost per stage; "cached" re-reads a primitive
//                                    list found by a full table walk every 64 frames (the incremental-tracking bound)
//   scenefeed census                 log every primitive component class: instances, attached, hidden, rendered
//   scenefeed draws <frames>         ask the native renderer to dump its next <frames> frames of draws (VS constants
//                                    and pass info) next to the JSON, for matching draws to primitives offline
//   scenefeed emitters <frames>      research: write each particle component's emitter instances raw (see DumpEmitters)
//
// JSON (one object per file):
//   frame, time (WorldInfo.TimeSeconds), real_time, host_us (steady clock, microseconds; the renderer's draw dumps
//   use the same clock), camera {location[3], rotation[3] (UE3 units, 65536 = 360 degrees: pitch, yaw, roll), fov
//   (horizontal, degrees), view[16], proj[16]}, primitives [ {kind, class, name (component path), owner, owner_class,
//   mesh, transform[16], materials[], bounds {origin[3], extent[3], radius}, hidden, rendered, last_render_time,
//   lod, bones[[16]...], emitters[...]} ], skeletons {mesh: {names[], parents[]}}, lights [...], fog [...], post {...}.
//   emitters (particle primitives only; one per FParticleEmitterInstance): {index, type (sprite, mesh, beam, trail or
//   other, from the LOD level's TypeDataModule class), type_class, vtable, emitter (ParticleEmitter path), material,
//   screen_alignment (EParticleScreenAlignment), local_space, sub_images [h, v], sub_uv_method, mesh, mesh_alignment,
//   override_material (mesh emitters only), location[3], active, max, stride, particles [[location 3, old_location 3,
//   size 3, rotation (radians), color 4 (linear RGBA), relative_time (0..1 of the lifetime), payload words...]]}.
//   A sprite particle's quad is centred on location (or old_location, depending on the material's motion blur);
//   a mesh particle's transform is translation(location) with the mesh emitter's rotation and size.
//   lights (every attached LightComponent: world lights and the ones DynamicLightEnvironments make for characters
//   and movers), fog (attached HeightFogComponent and FogVolumeDensityComponent): {class, name, props}, where props
//   holds every script property below ActorComponent, by name, as reflection describes it (see Members): for lights
//   LightToWorld (row 2, its Z axis, is the direction light travels: the owner's forward axis), Brightness, LightColor [R, G, B, A],
//   Radius, FalloffExponent, CastShadows, CastDynamicShadows, LightEnvironment, LightShadowMode, Inner/OuterConeAngle
//   (spot), LowerBrightness/LowerColor (sky), WorldSpaceIncidentLighting (spherical-harmonic environment lights).
//   post: {chain [{class, name, props}] (the local player's PlayerPostProcess effects, e.g. SfUberPostProcessEffect,
//   SfGodRaysEffect, MaterialEffect), chain_name, current (LocalPlayer.CurrentPPInfo: LastSettings, the blended
//   PostProcessSettings the renderer uses, and LastVolumeUsed), world_default (WorldInfo.DefaultPostProcessSettings),
//   camera_settings / camera_override_alpha when the camera class has them}. Enum values are written by name.
//   Matrices are UE3 FMatrix as stored: 16 floats, row-major, row vectors (v' = v * M, translation in row 3).
//   World space is UE3's: left-handed, Z up, 1 unit = 1 cm by convention.
#include "scene_feed.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "hybrid_state.h"

namespace bots::scenefeed {
namespace {
uint8_t* g_base = nullptr;

uint8_t* Raw(uint32_t address) { return g_base + (uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u)); }
uint32_t U32(uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Raw(address), 4);
  return __builtin_bswap32(v);
}
float F32(uint32_t address) { return std::bit_cast<float>(U32(address)); }
bool Readable(uint32_t address) { return address >= 0x40000000u && address < 0xA0000000u && !(address & 3); }

constexpr uint32_t kNames = 0x837F98A0, kGEngine = 0x8380D380;
constexpr uint32_t kImageDataFrom = 0x83000000, kImageDataTo = 0x839C0000;

const char* NameText(uint32_t index) {
  const uint32_t names = U32(kNames);
  if (!Readable(names) || index >= U32(kNames + 4)) return "?";
  const uint32_t entry = U32(names + 4 * index);
  return Readable(entry) ? reinterpret_cast<const char*>(Raw(entry + 8)) : "?";
}

std::string ObjName(uint32_t object) {
  if (!Readable(object)) return "None";
  std::string out = NameText(U32(object + 0x2C));
  if (const uint32_t number = U32(object + 0x30)) out += "_" + std::to_string(number - 1);
  return out;
}

const char* ClassName(uint32_t object) {
  const uint32_t cls = Readable(object) ? U32(object + 0x34) : 0;
  return Readable(cls) ? NameText(U32(cls + 0x2C)) : "None";
}

// Package.Group.Object
std::string Path(uint32_t object) {
  if (!Readable(object)) return "";
  uint32_t chain[16];
  int n = 0;
  for (uint32_t o = object; Readable(o) && n < 16; o = U32(o + 0x28)) chain[n++] = o;
  std::string out;
  for (int i = n - 1; i >= 0; --i) {
    if (!out.empty()) out += '.';
    out += ObjName(chain[i]);
  }
  return out;
}

constexpr uint32_t kBoolMask = 0x84;  // UBoolProperty BitMask (the record is 0x88 bytes)

struct Field {
  uint32_t offset = 0xFFFFFFFF, mask = 0;
  bool ok() const { return offset != 0xFFFFFFFF; }
};

// A property of the class or its superclasses, by name.
Field FindProp(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + 0x50), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) {
        Field out{U32(f + 0x64), 0};
        if (!std::strcmp(ClassName(f), "BoolProperty")) {
          out.mask = U32(f + kBoolMask);
        }
        return out;
      }
  return {};
}

bool ClassChainHas(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    if (!std::strcmp(NameText(U32(cls + 0x2C)), name)) return true;
  return false;
}

enum class Kind : uint8_t { kNone, kStatic, kSkeletal, kBsp, kTerrain, kParticle, kDecal, kSpeedTree, kSprite, kOther };
const char* KindName(Kind k) {
  switch (k) {
    case Kind::kStatic: return "static";
    case Kind::kSkeletal: return "skeletal";
    case Kind::kBsp: return "bsp";
    case Kind::kTerrain: return "terrain";
    case Kind::kParticle: return "particle";
    case Kind::kDecal: return "decal";
    case Kind::kSpeedTree: return "speedtree";
    case Kind::kSprite: return "sprite";
    case Kind::kOther: return "other";
    default: return "none";
  }
}

// Non-primitive components the feed also reports: lights and fog.
enum class Extra : uint8_t { kNone, kLight, kFog };

struct ClassInfo {
  Kind kind = Kind::kNone;  // kNone: not a PrimitiveComponent
  Extra extra = Extra::kNone;
  const char* name = "";
};

// Offsets, resolved from the first instance of each class family seen.
struct Offsets {
  bool primitive = false, mesh = false, static_mesh = false, skeletal = false, particle = false, decal = false;
  Field scene_info, local_to_world, bounds, hidden_game, last_render_time, owner, cull_dist, cached_cull, materials,
      static_mesh_ref,
      forced_lod, skeletal_mesh_ref, space_bases, predicted_lod, template_ref, decal_material;
  bool actor = false;
  Field actor_hidden;
  bool skel_asset = false;
  Field ref_skeleton, skel_materials;
};
Offsets g_off;

std::unordered_map<uint32_t, ClassInfo> g_classes;

const ClassInfo& Classify(uint32_t cls) {
  auto it = g_classes.find(cls);
  if (it != g_classes.end()) return it->second;
  ClassInfo info;
  info.name = Readable(cls) ? NameText(U32(cls + 0x2C)) : "None";
  if (ClassChainHas(cls, "LightComponent")) info.extra = Extra::kLight;
  else if (ClassChainHas(cls, "HeightFogComponent") || ClassChainHas(cls, "FogVolumeDensityComponent"))
    info.extra = Extra::kFog;
  if (ClassChainHas(cls, "PrimitiveComponent")) {
    if (ClassChainHas(cls, "SkeletalMeshComponent")) info.kind = Kind::kSkeletal;
    else if (ClassChainHas(cls, "StaticMeshComponent")) info.kind = Kind::kStatic;
    else if (ClassChainHas(cls, "ModelComponent")) info.kind = Kind::kBsp;
    else if (ClassChainHas(cls, "TerrainComponent")) info.kind = Kind::kTerrain;
    else if (ClassChainHas(cls, "ParticleSystemComponent")) info.kind = Kind::kParticle;
    else if (ClassChainHas(cls, "DecalComponent")) info.kind = Kind::kDecal;
    else if (ClassChainHas(cls, "SpeedTreeComponent")) info.kind = Kind::kSpeedTree;
    else if (ClassChainHas(cls, "SpriteComponent")) info.kind = Kind::kSprite;
    else info.kind = Kind::kOther;
    Offsets& o = g_off;
    if (!o.primitive) {
      o.primitive = true;
      o.scene_info = FindProp(cls, "SceneInfo");
      o.local_to_world = FindProp(cls, "LocalToWorld");
      o.bounds = FindProp(cls, "Bounds");
      o.hidden_game = FindProp(cls, "HiddenGame");
      o.last_render_time = FindProp(cls, "LastRenderTime");
      o.owner = FindProp(cls, "Owner");
      o.cull_dist = FindProp(cls, "CullDistance");
      o.cached_cull = FindProp(cls, "CachedCullDistance");
      REXLOG_INFO("scenefeed: PrimitiveComponent CullDistance +{:X} CachedCullDistance +{:X}", o.cull_dist.offset,
                  o.cached_cull.offset);
      REXLOG_INFO("scenefeed: PrimitiveComponent SceneInfo +{:X} LocalToWorld +{:X} Bounds +{:X} HiddenGame +{:X}/{:08X} "
                  "LastRenderTime +{:X} Owner +{:X}",
                  o.scene_info.offset, o.local_to_world.offset, o.bounds.offset, o.hidden_game.offset,
                  o.hidden_game.mask, o.last_render_time.offset, o.owner.offset);
    }
    if (!o.mesh && ClassChainHas(cls, "MeshComponent")) {
      o.mesh = true;
      o.materials = FindProp(cls, "Materials");
      REXLOG_INFO("scenefeed: MeshComponent Materials +{:X}", o.materials.offset);
    }
    if (!o.static_mesh && info.kind == Kind::kStatic) {
      o.static_mesh = true;
      o.static_mesh_ref = FindProp(cls, "StaticMesh");
      o.forced_lod = FindProp(cls, "ForcedLodModel");
      REXLOG_INFO("scenefeed: StaticMeshComponent StaticMesh +{:X} ForcedLodModel +{:X}", o.static_mesh_ref.offset,
                  o.forced_lod.offset);
    }
    if (!o.skeletal && info.kind == Kind::kSkeletal) {
      o.skeletal = true;
      o.skeletal_mesh_ref = FindProp(cls, "SkeletalMesh");
      o.space_bases = FindProp(cls, "SpaceBases");
      o.predicted_lod = FindProp(cls, "PredictedLODLevel");
      REXLOG_INFO("scenefeed: SkeletalMeshComponent SkeletalMesh +{:X} SpaceBases +{:X} PredictedLODLevel +{:X}",
                  o.skeletal_mesh_ref.offset, o.space_bases.offset, o.predicted_lod.offset);
    }
    if (!o.particle && info.kind == Kind::kParticle) {
      o.particle = true;
      o.template_ref = FindProp(cls, "Template");
    }
    if (!o.decal && info.kind == Kind::kDecal) {
      o.decal = true;
      o.decal_material = FindProp(cls, "DecalMaterial");
    }
  }
  return g_classes.emplace(cls, info).first->second;
}

// UObject::GObjObjects: {data, count, max} whose entries' Index (+0x04) equal their position.
uint32_t g_objects = 0;
bool Mapped(uint32_t address) { return address >= 0x40000000u && !(address & 3); }
bool FindObjectTable() {
  if (g_objects) return true;
  static int tries = 0;
  if (tries++ % 300) return false;  // a scan costs a few ms: retry every 300 calls
  // The engine object's Index field: where in the header the object's own table position sits.
  for (uint32_t a = kImageDataFrom; a + 12 <= kImageDataTo; a += 4) {
    const uint32_t data = U32(a), count = U32(a + 4), max = U32(a + 8);
    if (!Mapped(data) || count < 1000 || count > 4000000 || max < count || max > 8000000) continue;
    int good = 0;
    for (uint32_t i = 1; i < 64; ++i) {
      const uint32_t o = U32(data + 4 * i);
      if (!o) continue;
      if (!Mapped(o) || U32(o + 0x04) != i) {
        good = -1;
        break;
      }
      ++good;
    }
    if (good >= 32) {
      g_objects = a;
      REXLOG_INFO("scenefeed: object table at {:08X} ({} objects, data {:08X})", a, count, data);
      return true;
    }
  }
  const uint32_t e = U32(kGEngine);
  std::string words;
  for (uint32_t i = 0; i < 0x40; i += 4) words += fmt::format(" {:08X}", U32(e + i));
  REXLOG_WARN("scenefeed: object table not found; GEngine {:08X} header:{}", e, words);
  return false;
}

struct Camera {
  float location[3] = {};
  int32_t rotation[3] = {};
  float fov = 0;
  float view[16] = {}, proj[16] = {};
  bool ok = false;
};

void Mul(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}

// UE3's view and projection for a POV (FSceneView as ULocalPlayer::CalcSceneView builds it): ViewMatrix =
// Translation(-Location) * InverseRotation(Rotation) * axis swap (UE3 X forward, Y right, Z up -> view Z forward,
// X right, Y up); ProjectionMatrix = FPerspectiveMatrix(FOV/2, 1280, 720, near 10) with an infinite far plane.
void CameraMatrices(Camera& cam) {
  constexpr double k = 3.14159265358979323846 * 2 / 65536;
  const double p = cam.rotation[0] * k, y = cam.rotation[1] * k, r = cam.rotation[2] * k;
  const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
  // FRotationMatrix rows: X, Y, Z axes.
  const double m[3][3] = {{cp * cy, cp * sy, sp},
                          {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
                          {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
  float t[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -cam.location[0], -cam.location[1], -cam.location[2], 1};
  float inv[16] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) inv[i * 4 + j] = float(m[j][i]);
  inv[15] = 1;
  const float swap[16] = {0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
  float tmp[16];
  Mul(t, inv, tmp);
  Mul(tmp, swap, cam.view);
  const float half = float(cam.fov * 3.14159265358979323846 / 360), near_z = 10;
  std::memset(cam.proj, 0, sizeof(cam.proj));
  cam.proj[0] = 1 / std::tan(half);
  cam.proj[5] = 1280.0f / 720.0f / std::tan(half);
  cam.proj[10] = 1;  // infinite far plane (UE3 uses 1 - Z_PRECISION)
  cam.proj[11] = 1;
  cam.proj[14] = -near_z;
}

uint32_t Engine() { return U32(kGEngine); }

bool ReadCamera(Camera& cam, uint32_t& world_info_out) {
  static Field players, actor, player_camera, camera_cache;
  const uint32_t engine = Engine();
  if (!Readable(engine)) return false;
  if (!players.ok()) players = FindProp(U32(engine + 0x34), "GamePlayers");
  if (!players.ok() || !U32(engine + players.offset + 4)) return false;
  const uint32_t lp = U32(U32(engine + players.offset));
  if (!Readable(lp)) return false;
  if (!actor.ok()) actor = FindProp(U32(lp + 0x34), "Actor");
  const uint32_t pc = actor.ok() ? U32(lp + actor.offset) : 0;
  if (!Readable(pc)) return false;
  if (!player_camera.ok()) player_camera = FindProp(U32(pc + 0x34), "PlayerCamera");
  const uint32_t camera = player_camera.ok() ? U32(pc + player_camera.offset) : 0;
  static Field world_info;
  if (!world_info.ok()) world_info = FindProp(U32(pc + 0x34), "WorldInfo");
  world_info_out = world_info.ok() ? U32(pc + world_info.offset) : 0;
  if (!Readable(camera)) return false;
  if (!camera_cache.ok()) camera_cache = FindProp(U32(camera + 0x34), "CameraCache");
  if (!camera_cache.ok()) return false;
  const uint32_t pov = camera + camera_cache.offset + 4;  // TCameraCache {float TimeStamp; TPOV POV}
  for (int i = 0; i < 3; ++i) cam.location[i] = F32(pov + 4 * i);
  for (int i = 0; i < 3; ++i) cam.rotation[i] = int32_t(U32(pov + 12 + 4 * i));
  cam.fov = F32(pov + 24);
  CameraMatrices(cam);
  cam.ok = true;
  return true;
}

struct Primitive {
  uint32_t component;
  Kind kind;
};

struct Times {
  double enumerate = 0, extract = 0, json = 0, write = 0;
  uint32_t primitives = 0, objects = 0;
};

// Lights and fog components attached to the world (ActorComponent.bAttached), found by the same walk.
struct Extras {
  std::vector<uint32_t> lights, fog;
};
Extras g_extras;

bool Attached(uint32_t component) {
  static std::unordered_map<uint32_t, Field> by_class;
  const uint32_t cls = U32(component + 0x34);
  auto it = by_class.find(cls);
  if (it == by_class.end()) it = by_class.emplace(cls, FindProp(cls, "bAttached")).first;
  return it->second.ok() && (U32(component + it->second.offset) & it->second.mask);
}

// Every attached primitive component in the object table.
void Enumerate(std::vector<Primitive>& out, Times& t) {
  out.clear();
  g_extras.lights.clear();
  g_extras.fog.clear();
  if (!FindObjectTable()) return;
  const uint32_t data = U32(g_objects), count = U32(g_objects + 4);
  t.objects = count;
  uint32_t last_cls = 0;
  const ClassInfo* last = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = U32(data + 4 * i);
    if (!o) continue;
    const uint32_t cls = U32(o + 0x34);
    if (cls != last_cls) last_cls = cls, last = &Classify(cls);
    if (last->extra != Extra::kNone) {
      if (Attached(o)) (last->extra == Extra::kLight ? g_extras.lights : g_extras.fog).push_back(o);
      continue;
    }
    if (last->kind == Kind::kNone) continue;
    if (!g_off.scene_info.ok() || !U32(o + g_off.scene_info.offset)) continue;  // not attached to a scene
    out.push_back({o, last->kind});
  }
}

// JSON has no NaN or infinity (skeletal SpaceBases hold NaN for bones a LOD doesn't update): write null.
void Float(std::string& s, bool comma, float v) {
  if (comma) s += ',';
  if (std::isfinite(v))
    fmt::format_to(std::back_inserter(s), "{:.7g}", v);
  else
    s += "null";
}
void Floats(std::string& s, uint32_t address, int n) {
  s += '[';
  for (int i = 0; i < n; ++i) Float(s, i, F32(address + 4 * i));
  s += ']';
}
void FloatsHost(std::string& s, const float* v, int n) {
  s += '[';
  for (int i = 0; i < n; ++i) Float(s, i, v[i]);
  s += ']';
}
void Str(std::string& s, std::string_view v) {
  s += '"';
  for (char c : v) {
    if (c == '"' || c == '\\') s += '\\';
    if (uint8_t(c) >= 0x20) s += c;
  }
  s += '"';
}

// FMeshBone stride in SkeletalMesh.RefSkeleton (declared array<int> in script; the native element is larger).
// Found once: the stride at which every entry's name is a valid FName index and its parent index is in range.
uint32_t g_bone_stride = 0, g_bone_parent = 0;

void Skeleton(std::string& s, uint32_t mesh) {
  if (!g_off.ref_skeleton.ok()) return;
  const uint32_t data = U32(mesh + g_off.ref_skeleton.offset), count = U32(mesh + g_off.ref_skeleton.offset + 4);
  if (!Readable(data) || !count || count > 1024) return;
  const uint32_t name_count = U32(kNames + 4);
  if (!g_bone_stride) {
    for (uint32_t stride = 16; stride <= 128 && !g_bone_stride; stride += 4)
      for (uint32_t parent = 8; parent + 4 <= stride && !g_bone_stride; parent += 4) {
        bool ok = U32(data + parent) == 0;  // the root is its own parent (0)
        for (uint32_t b = 1; b < count && ok; ++b)
          ok = U32(data + b * stride) < name_count && U32(data + b * stride + parent) < b;
        if (ok && count > 4) g_bone_stride = stride, g_bone_parent = parent;
      }
    REXLOG_INFO("scenefeed: FMeshBone stride {} parent +{} (from {} bones)", g_bone_stride, g_bone_parent, count);
    if (!g_bone_stride) return;
  }
  s += "{\"names\":[";
  for (uint32_t b = 0; b < count; ++b) {
    if (b) s += ',';
    Str(s, NameText(U32(data + b * g_bone_stride)));
  }
  s += "],\"parents\":[";
  for (uint32_t b = 0; b < count; ++b)
    fmt::format_to(std::back_inserter(s), "{}{}", b ? "," : "", int32_t(U32(data + b * g_bone_stride + g_bone_parent)));
  s += "]}";
}

bool IsObject(uint32_t p);

// A property of <object>'s class by name, cached per (class, name).
Field Prop(uint32_t object, const char* name) {
  static std::unordered_map<uint64_t, Field> cache;
  const uint32_t cls = U32(object + 0x34);
  const uint64_t key = (uint64_t(cls) << 32) ^ std::hash<std::string_view>{}(name);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  return cache.emplace(key, FindProp(cls, name)).first->second;
}
uint32_t PropU32(uint32_t object, const char* name, uint32_t fallback = 0) {
  const Field f = Prop(object, name);
  return f.ok() ? U32(object + f.offset) : fallback;
}
bool PropBool(uint32_t object, const char* name) {
  const Field f = Prop(object, name);
  return f.ok() && (U32(object + f.offset) & f.mask);
}
uint8_t PropByte(uint32_t object, const char* name) {
  const Field f = Prop(object, name);
  return f.ok() ? *Raw(object + f.offset) : 0;
}

// Every script property of an object (or struct) as JSON, from the reflection records alone. UProperty: +0x44
// ArrayDim, +0x48 ElementSize, +0x64 Offset; the subclass's first field (+0x84) is UBoolProperty BitMask,
// UStructProperty Struct, UObjectProperty PropertyClass, UArrayProperty Inner or UByteProperty Enum (UEnum: +0x44
// Names, a TArray<FName>). Vector, Vector2D, Rotator, Color, LinearColor and Matrix are written as arrays (Color as
// [R, G, B, A] bytes); other structs as objects; dynamic arrays up to 64 elements. Properties of <stop> and its
// superclasses are left out.
void Value(std::string& s, uint32_t prop, uint32_t address, int depth);

void Members(std::string& s, uint32_t structure, uint32_t address, int depth, const char* stop = "Object") {
  s += '{';
  bool first = true;
  for (int level = 0; Readable(structure) && level < 16; structure = U32(structure + 0x3C), ++level) {
    if (!std::strcmp(NameText(U32(structure + 0x2C)), stop)) break;
    for (uint32_t f = U32(structure + 0x50), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n) {
      if (!std::strstr(ClassName(f), "Property")) continue;
      if (!first) s += ',';
      first = false;
      Str(s, NameText(U32(f + 0x2C)));
      s += ':';
      const uint32_t dim = U32(f + 0x44), size = U32(f + 0x48), at = address + U32(f + 0x64);
      if (dim > 1 && dim <= 64) {
        s += '[';
        for (uint32_t k = 0; k < dim; ++k) {
          if (k) s += ',';
          Value(s, f, at + k * size, depth);
        }
        s += ']';
      } else {
        Value(s, f, at, depth);
      }
    }
  }
  s += '}';
}

void Value(std::string& s, uint32_t prop, uint32_t address, int depth) {
  const std::string_view type = ClassName(prop);
  const uint32_t sub = U32(prop + 0x84);
  if (type == "FloatProperty") {
    Float(s, false, F32(address));
  } else if (type == "IntProperty") {
    fmt::format_to(std::back_inserter(s), "{}", int32_t(U32(address)));
  } else if (type == "BoolProperty") {
    s += (U32(address) & sub) ? "true" : "false";
  } else if (type == "ByteProperty") {
    const uint8_t v = *Raw(address);
    const uint32_t names = IsObject(sub) ? U32(sub + 0x44) : 0;
    if (Readable(names) && v < U32(sub + 0x48)) Str(s, NameText(U32(names + 8 * v)));  // enum value name
    else fmt::format_to(std::back_inserter(s), "{}", v);
  } else if (type == "NameProperty") {
    Str(s, NameText(U32(address)));
  } else if (type == "ObjectProperty" || type == "ComponentProperty" || type == "ClassProperty") {
    const uint32_t o = U32(address);
    if (IsObject(o)) Str(s, Path(o));
    else s += "null";
  } else if (type == "StrProperty") {
    const uint32_t data = U32(address), n = U32(address + 4);
    std::string text;
    for (uint32_t k = 0; Readable(data & ~3u) && k + 1 < n && k < 256; ++k) {
      const uint16_t c = uint16_t(__builtin_bswap16(*reinterpret_cast<const uint16_t*>(Raw(data + 2 * k))));
      text += c < 0x80 ? char(c) : '?';
    }
    Str(s, text);
  } else if (type == "StructProperty" && Readable(sub) && depth < 6) {
    const std::string_view name = NameText(U32(sub + 0x2C));
    auto members = [&](std::initializer_list<const char*> names) {
      s += '[';
      bool first = true;
      for (const char* m : names) {
        const Field f = FindProp(sub, m);
        if (!first) s += ',';
        first = false;
        if (!f.ok()) s += "null";
        else if (name == "Color") fmt::format_to(std::back_inserter(s), "{}", *Raw(address + f.offset));
        else if (name == "Rotator") fmt::format_to(std::back_inserter(s), "{}", int32_t(U32(address + f.offset)));
        else Float(s, false, F32(address + f.offset));
      }
      s += ']';
    };
    if (name == "Vector") members({"X", "Y", "Z"});
    else if (name == "Vector2D") members({"X", "Y"});
    else if (name == "Rotator") members({"Pitch", "Yaw", "Roll"});
    else if (name == "Color" || name == "LinearColor") members({"R", "G", "B", "A"});
    else if (name == "Matrix") Floats(s, address, 16);
    else Members(s, sub, address, depth + 1);
  } else if (type == "ArrayProperty" && Readable(sub) && depth < 6) {
    const uint32_t data = U32(address), n = U32(address + 4), size = U32(sub + 0x48);
    s += '[';
    for (uint32_t k = 0; Readable(data) && k < n && k < 64 && size && size <= 0x1000; ++k) {
      if (k) s += ',';
      Value(s, sub, data + k * size, depth + 1);
    }
    s += ']';
  } else {
    s += "null";  // delegates, interfaces, native pointers, maps
  }
}

// One object: {class, name (path), props {...}}.
void ObjectJson(std::string& s, uint32_t object, const char* stop) {
  fmt::format_to(std::back_inserter(s), "{{\"class\":\"{}\",\"name\":", ClassName(object));
  Str(s, Path(object));
  s += ",\"props\":";
  Members(s, U32(object + 0x34), object, 0, stop);
  s += '}';
}

// The UProperty record of <object>'s property <name> (0 if none).
uint32_t PropRecord(uint32_t object, const char* name) {
  for (uint32_t cls = U32(object + 0x34), depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + 0x50), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) return f;
  return 0;
}

// ,"<key>":<object.name> when the object has that property.
void NamedValue(std::string& s, const char* key, uint32_t object, const char* name) {
  if (!IsObject(object)) return;
  const uint32_t f = PropRecord(object, name);
  if (!f) return;
  fmt::format_to(std::back_inserter(s), ",\"{}\":", key);
  Value(s, f, object + U32(f + 0x64), 0);
}

// The light properties the live renderer reads; dumping every property of every light costs milliseconds a frame.
constexpr const char* kLiveLightProps[] = {
    "LightToWorld", "Brightness", "LightColor", "LightAffectsClassification", "LightingChannels",
    "UseDirectLightMap", "LowerBrightness", "LowerColor", "WorldSpaceIncidentLighting", "LightEnvironment",
    "bEnabled", "Radius", "FalloffExponent", "InnerConeAngle", "OuterConeAngle", "CastShadows",
    "CastDynamicShadows", "LightShadowMode"};

// One light as ObjectJson writes it, with only kLiveLightProps (property records cached per class).
void CompactLightJson(std::string& s, uint32_t light) {
  static std::unordered_map<uint32_t, std::vector<uint32_t>> records;
  auto [it, fresh] = records.try_emplace(U32(light + 0x34));
  if (fresh)
    for (const char* name : kLiveLightProps)
      if (const uint32_t f = PropRecord(light, name)) it->second.push_back(f);
  // No "name": the renderer keys lights by their LightEnvironment, and Path() is the costly part.
  fmt::format_to(std::back_inserter(s), "{{\"class\":\"{}\",\"props\":{{", ClassName(light));
  bool first = true;
  for (const uint32_t f : it->second) {
    if (!first) s += ',';
    first = false;
    const char* name = NameText(U32(f + 0x2C));
    Str(s, name);
    s += ':';
    if (!std::strcmp(name, "LightingChannels")) {  // a struct of ~30 bools; the renderer reads two
      static const Field st = FindProp(U32(f + 0x84), "Static"), dy = FindProp(U32(f + 0x84), "Dynamic");
      const uint32_t at = light + U32(f + 0x64);
      fmt::format_to(std::back_inserter(s), "{{\"Static\":{},\"Dynamic\":{}}}",
                     st.ok() && (U32(at + st.offset) & st.mask), dy.ok() && (U32(at + dy.offset) & dy.mask));
    } else {
      Value(s, f, light + U32(f + 0x64), 0);
    }
  }
  s += "}}";
}

// Lights, fog and post-processing for the frame (see the format notes at the top). `live`: the renderer's cut: only
// the light properties it reads, and fog and post only when `fog_post` (they rarely change).
void LightsPostJson(std::string& s, uint32_t world_info, bool live = false, bool fog_post = true) {
  s += ",\"lights\":[";
  bool first = true;
  for (uint32_t l : g_extras.lights) {
    if (!IsObject(l)) continue;
    // The renderer skips baked or disabled level point and spot lights (the lightmaps have them): leave them out.
    if (live && !IsObject(PropU32(l, "LightEnvironment"))) {
      const std::string_view cls = ClassName(l);
      if ((cls == "PointLightComponent" || cls == "SpotLightComponent") &&
          (!PropBool(l, "bEnabled") || PropBool(l, "UseDirectLightMap") ||
           PropByte(l, "LightAffectsClassification") == 2 /* LAC_STATIC_AFFECTING */))
        continue;
    }
    if (!first) s += ",\n";
    first = false;
    if (live) CompactLightJson(s, l);
    else ObjectJson(s, l, "ActorComponent");
  }
  s += ']';
  if (!fog_post) return;
  s += ",\"fog\":[";
  first = true;
  for (uint32_t f : g_extras.fog) {
    if (!IsObject(f)) continue;
    if (!first) s += ",\n";
    first = false;
    ObjectJson(s, f, "ActorComponent");
  }
  s += "],\"post\":{\"chain\":[";
  const uint32_t engine = Engine();
  const uint32_t players = IsObject(engine) ? PropRecord(engine, "GamePlayers") : 0;
  const uint32_t lp = players && U32(engine + U32(players + 0x64) + 4) ? U32(U32(engine + U32(players + 0x64))) : 0;
  const uint32_t chain = IsObject(lp) ? PropU32(lp, "PlayerPostProcess") : 0;
  if (IsObject(chain)) {
    const Field effects = Prop(chain, "Effects");
    const uint32_t data = effects.ok() ? U32(chain + effects.offset) : 0, n = effects.ok() ? U32(chain + effects.offset + 4) : 0;
    first = true;
    for (uint32_t i = 0; Readable(data) && i < n && i < 32; ++i) {
      const uint32_t e = U32(data + 4 * i);
      if (!IsObject(e)) continue;
      if (!first) s += ",\n";
      first = false;
      ObjectJson(s, e, "Object");
    }
  }
  s += ']';
  if (IsObject(chain)) {
    s += ",\"chain_name\":";
    Str(s, Path(chain));
  }
  NamedValue(s, "current", lp, "CurrentPPInfo");
  const uint32_t pc = IsObject(lp) ? PropU32(lp, "Actor") : 0;
  const uint32_t camera = IsObject(pc) ? PropU32(pc, "PlayerCamera") : 0;
  NamedValue(s, "camera_override_alpha", camera, "CamOverridePostProcessAlpha");
  NamedValue(s, "camera_settings", camera, "CamPostProcessSettings");
  NamedValue(s, "world_default", world_info, "DefaultPostProcessSettings");
  s += '}';
}

// FParticleEmitterInstance (native, this build; found by dumping instances and matching the sprite vertex data):
//   +0x00 vtable, +0x10 CurrentLODLevel (UParticleLODLevel), +0x20 Location (FVector), +0x30 ParticleData,
//   +0x34 ParticleIndices (WORD[]), +0xC0 ParticleStride, +0xC4 ActiveParticles, +0xC8 MaxActiveParticles,
//   +0x130 the material the emitter draws with. Particles: ParticleData + ParticleIndices[i] * ParticleStride for
//   i < ActiveParticles. FBaseParticle (0x80 bytes): +0x00 OldLocation, +0x0C RelativeTime, +0x10 Location,
//   +0x1C OneOverMaxLifetime, +0x20 BaseVelocity, +0x2C Rotation, +0x30 Velocity, +0x3C BaseRotationRate,
//   +0x40 BaseSize, +0x4C RotationRate, +0x50 Size, +0x5C Flags, +0x60 Color (FLinearColor), +0x70 BaseColor;
//   module payloads follow from +0x80.
constexpr uint32_t kEmitLod = 0x10, kEmitLocation = 0x20, kEmitData = 0x30, kEmitIndices = 0x34, kEmitStride = 0xC0,
                   kEmitActive = 0xC4, kEmitMax = 0xC8, kEmitMaterial = 0x130;

void Emitters(std::string& s, uint32_t component) {
  static Field instances;
  if (!instances.ok()) instances = FindProp(U32(component + 0x34), "EmitterInstances");
  if (!instances.ok()) return;
  const uint32_t data = U32(component + instances.offset), n = U32(component + instances.offset + 4);
  s += ",\"emitters\":[";
  bool first = true;
  for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
    const uint32_t inst = U32(data + 4 * i);
    if (!Readable(inst)) continue;
    const uint32_t lod = U32(inst + kEmitLod);
    if (!IsObject(lod)) continue;
    const uint32_t required = PropU32(lod, "RequiredModule"), type_data = PropU32(lod, "TypeDataModule");
    const char* type = "sprite";
    std::string type_class = IsObject(type_data) ? ClassName(type_data) : "";
    if (type_class.find("Mesh") != std::string::npos) type = "mesh";
    else if (type_class.find("Beam") != std::string::npos) type = "beam";
    else if (type_class.find("Trail") != std::string::npos) type = "trail";
    else if (!type_class.empty()) type = "other";
    const uint32_t active = U32(inst + kEmitActive), stride = U32(inst + kEmitStride);
    if (!first) s += ',';
    first = false;
    fmt::format_to(std::back_inserter(s), "{{\"index\":{},\"type\":\"{}\",\"type_class\":\"{}\",\"vtable\":{},"
                   "\"emitter\":", i, type, type_class, U32(inst));
    Str(s, Path(U32(lod + 0x28)));  // the LOD level's outer: the ParticleEmitter
    const uint32_t material = U32(inst + kEmitMaterial);
    s += ",\"material\":";
    Str(s, IsObject(material) ? Path(material) : (IsObject(required) ? Path(PropU32(required, "Material")) : ""));
    if (IsObject(required))
      fmt::format_to(std::back_inserter(s),
                     ",\"screen_alignment\":{},\"local_space\":{},\"sub_images\":[{},{}],\"sub_uv_method\":{}",
                     PropByte(required, "ScreenAlignment"), PropBool(required, "bUseLocalSpace"),
                     int32_t(PropU32(required, "SubImages_Horizontal", 1)),
                     int32_t(PropU32(required, "SubImages_Vertical", 1)), PropByte(required, "InterpolationMethod"));
    if (IsObject(type_data) && std::string_view(type) == "mesh") {
      s += ",\"mesh\":";
      Str(s, Path(PropU32(type_data, "Mesh")));
      fmt::format_to(std::back_inserter(s), ",\"mesh_alignment\":{},\"override_material\":{}",
                     PropByte(type_data, "MeshAlignment"), PropBool(type_data, "bOverrideMaterial"));
    }
    s += ",\"location\":";
    Floats(s, inst + kEmitLocation, 3);
    fmt::format_to(std::back_inserter(s), ",\"active\":{},\"max\":{},\"stride\":{},\"particles\":[", active,
                   U32(inst + kEmitMax), stride);
    const uint32_t pdata = U32(inst + kEmitData), pindex = U32(inst + kEmitIndices);
    if (Readable(pdata) && Readable(pindex & ~3u) && stride >= 0x80 && stride <= 0x1000 && active <= 4096) {
      for (uint32_t k = 0; k < active; ++k) {
        const uint16_t slot = uint16_t(__builtin_bswap16(*reinterpret_cast<const uint16_t*>(Raw(pindex + 2 * k))));
        const uint32_t p = pdata + slot * stride;
        if (k) s += ',';
        // [location 3, old_location 3, size 3, rotation, color 4, relative_time, payload...]
        s += '[';
        for (uint32_t off : {0x10u, 0x14u, 0x18u, 0x00u, 0x04u, 0x08u, 0x50u, 0x54u, 0x58u, 0x2Cu, 0x60u, 0x64u, 0x68u,
                             0x6Cu, 0x0Cu})
          Float(s, off != 0x10u, F32(p + off));
        for (uint32_t off = 0x80; off < stride; off += 4) Float(s, true, F32(p + off));
        s += ']';
      }
    }
    s += "]}";
  }
  s += "]";
}

// A material as n1 names it: the first cooked material up its Parent chain, plus the run-time instances' parameters.
std::string MaterialPath(uint32_t m, bool params = false, bool* runtime = nullptr);

std::string FrameJson(uint32_t frame, const Camera& cam, uint32_t world_info, const std::vector<Primitive>& prims,
                      int64_t host_us) {
  static Field time_seconds, real_time;
  if (Readable(world_info) && !time_seconds.ok()) {
    time_seconds = FindProp(U32(world_info + 0x34), "TimeSeconds");
    real_time = FindProp(U32(world_info + 0x34), "RealTimeSeconds");
  }
  const float now = time_seconds.ok() && Readable(world_info) ? F32(world_info + time_seconds.offset) : 0;
  std::string s;
  s.reserve(1 << 20);
  fmt::format_to(std::back_inserter(s), "{{\"frame\":{},\"time\":{:.4f},\"real_time\":{:.4f},\"host_us\":{},", frame,
                 now, real_time.ok() && Readable(world_info) ? F32(world_info + real_time.offset) : 0.f, host_us);
  s += "\"camera\":{";
  if (cam.ok) {
    s += "\"location\":";
    FloatsHost(s, cam.location, 3);
    fmt::format_to(std::back_inserter(s), ",\"rotation\":[{},{},{}],\"fov\":{:.5g},\"view\":", cam.rotation[0],
                   cam.rotation[1], cam.rotation[2], cam.fov);
    FloatsHost(s, cam.view, 16);
    s += ",\"proj\":";
    FloatsHost(s, cam.proj, 16);
  }
  s += "},\"primitives\":[\n";
  std::unordered_map<uint32_t, bool> skeletons;
  bool first = true;
  for (const Primitive& p : prims) {
    const uint32_t c = p.component;
    if (!first) s += ",\n";
    first = false;
    fmt::format_to(std::back_inserter(s), "{{\"kind\":\"{}\",\"class\":\"{}\",\"name\":", KindName(p.kind),
                   ClassName(c));
    Str(s, Path(c));
    const uint32_t owner = g_off.owner.ok() ? U32(c + g_off.owner.offset) : 0;
    s += ",\"owner\":";
    Str(s, Readable(owner) ? ObjName(owner) : "");
    s += ",\"owner_class\":";
    Str(s, Readable(owner) ? ClassName(owner) : "");
    uint32_t mesh = 0;
    if (p.kind == Kind::kStatic && g_off.static_mesh_ref.ok()) mesh = U32(c + g_off.static_mesh_ref.offset);
    if (p.kind == Kind::kSkeletal && g_off.skeletal_mesh_ref.ok()) mesh = U32(c + g_off.skeletal_mesh_ref.offset);
    if (p.kind == Kind::kParticle && g_off.template_ref.ok()) mesh = U32(c + g_off.template_ref.offset);
    s += ",\"mesh\":";
    Str(s, Path(mesh));
    s += ",\"transform\":";
    Floats(s, c + g_off.local_to_world.offset, 16);
    s += ",\"materials\":[";
    if (g_off.materials.ok() && (p.kind == Kind::kStatic || p.kind == Kind::kSkeletal)) {
      const uint32_t data = U32(c + g_off.materials.offset), n = U32(c + g_off.materials.offset + 4);
      for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
        if (i) s += ',';
        Str(s, MaterialPath(U32(data + 4 * i), true));  // as the live feed passes it
      }
    } else if (p.kind == Kind::kDecal && g_off.decal_material.ok()) {
      Str(s, Path(U32(c + g_off.decal_material.offset)));
    }
    s += "]";
    if (g_off.bounds.ok()) {  // FBoxSphereBounds {Origin, BoxExtent, SphereRadius}
      const uint32_t b = c + g_off.bounds.offset;
      s += ",\"bounds\":{\"origin\":";
      Floats(s, b, 3);
      s += ",\"extent\":";
      Floats(s, b + 12, 3);
      fmt::format_to(std::back_inserter(s), ",\"radius\":{:.6g}}}", F32(b + 24));
    }
    bool hidden = g_off.hidden_game.ok() && (U32(c + g_off.hidden_game.offset) & g_off.hidden_game.mask);
    if (Readable(owner)) {
      if (!g_off.actor) {
        g_off.actor = true;
        g_off.actor_hidden = FindProp(U32(owner + 0x34), "bHidden");
      }
      if (g_off.actor_hidden.ok()) hidden |= (U32(owner + g_off.actor_hidden.offset) & g_off.actor_hidden.mask) != 0;
    }
    const float lrt = g_off.last_render_time.ok() ? F32(c + g_off.last_render_time.offset) : -1;
    fmt::format_to(std::back_inserter(s), ",\"hidden\":{},\"last_render_time\":{:.4f},\"rendered\":{}", hidden, lrt,
                   lrt >= now - 0.05f);
    if (p.kind == Kind::kParticle) Emitters(s, c);
    if (p.kind == Kind::kStatic && g_off.forced_lod.ok())
      fmt::format_to(std::back_inserter(s), ",\"lod\":{}", int32_t(U32(c + g_off.forced_lod.offset)));
    if (p.kind == Kind::kSkeletal) {
      if (g_off.predicted_lod.ok())
        fmt::format_to(std::back_inserter(s), ",\"lod\":{}", int32_t(U32(c + g_off.predicted_lod.offset)));
      if (g_off.space_bases.ok()) {
        const uint32_t data = U32(c + g_off.space_bases.offset), n = U32(c + g_off.space_bases.offset + 4);
        s += ",\"bones\":[";
        for (uint32_t i = 0; Readable(data) && i < n && i < 1024; ++i) {
          if (i) s += ',';
          Floats(s, data + 64 * i, 16);
        }
        s += "]";
      }
      if (Readable(mesh)) skeletons[mesh] = true;
    }
    s += "}";
  }
  s += "\n],\"skeletons\":{";
  first = true;
  for (const auto& [mesh, _] : skeletons) {
    if (!g_off.skel_asset) {
      g_off.skel_asset = true;
      g_off.ref_skeleton = FindProp(U32(mesh + 0x34), "RefSkeleton");
      g_off.skel_materials = FindProp(U32(mesh + 0x34), "Materials");
      REXLOG_INFO("scenefeed: SkeletalMesh RefSkeleton +{:X} Materials +{:X}", g_off.ref_skeleton.offset,
                  g_off.skel_materials.offset);
    }
    if (!first) s += ",\n";
    first = false;
    Str(s, Path(mesh));
    s += ':';
    const size_t before = s.size();
    Skeleton(s, mesh);
    if (s.size() == before) s += "null";
  }
  s += "}";
  LightsPostJson(s, world_info);
  s += "}\n";
  return s;
}

struct State {
  int dump_left = 0, dump_every = 1, time_left = 0, emitters_left = 0;
  bool cached = false;
  uint32_t frame = 0, written = 0;
  float last_real_time = -1;
  std::vector<Primitive> prims;
  uint32_t prims_age = 1u << 30;
  std::vector<Times> times;
  std::string dir = [] {
    const char* v = std::getenv("BOTS_SCENEFEED_DIR");
    return std::string(v && *v ? v : "scenefeed");
  }();
};
State g_state;

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

void ReportTimes() {
  auto& v = g_state.times;
  if (v.empty()) return;
  auto stat = [&](double Times::*m, const char* name) {
    std::vector<double> x;
    for (auto& t : v) x.push_back(t.*m);
    std::sort(x.begin(), x.end());
    REXLOG_INFO("scenefeed time: {:<9} median {:.3f} ms  p90 {:.3f}  max {:.3f}  ({} frames)", name, x[x.size() / 2],
                x[x.size() * 9 / 10], x.back(), x.size());
  };
  stat(&Times::enumerate, "enumerate");
  stat(&Times::extract, "extract");
  stat(&Times::json, "json");
  stat(&Times::write, "write");
  REXLOG_INFO("scenefeed time: {} primitives of {} objects (last frame)", v.back().primitives, v.back().objects);
  v.clear();
}

void Census() {
  if (!FindObjectTable()) {
    REXLOG_WARN("scenefeed census: object table not found");
    return;
  }
  struct Count {
    uint32_t instances = 0, attached = 0, hidden = 0, rendered = 0;
    Kind kind;
  };
  std::unordered_map<std::string, Count> by_class;
  uint32_t world_info = 0;
  Camera cam;
  ReadCamera(cam, world_info);
  static Field time_seconds;
  if (Readable(world_info) && !time_seconds.ok()) time_seconds = FindProp(U32(world_info + 0x34), "TimeSeconds");
  const float now = time_seconds.ok() ? F32(world_info + time_seconds.offset) : 0;
  const uint32_t data = U32(g_objects), count = U32(g_objects + 4);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t o = U32(data + 4 * i);
    if (!o) continue;
    const ClassInfo& ci = Classify(U32(o + 0x34));
    if (ci.kind == Kind::kNone) continue;
    Count& c = by_class[ci.name];
    c.kind = ci.kind;
    ++c.instances;
    if (!U32(o + g_off.scene_info.offset)) continue;
    ++c.attached;
    if (U32(o + g_off.hidden_game.offset) & g_off.hidden_game.mask) ++c.hidden;
    if (F32(o + g_off.last_render_time.offset) >= now - 0.05f) ++c.rendered;
  }
  REXLOG_INFO("scenefeed census: {} objects, time {:.3f}", count, now);
  for (const auto& [name, c] : by_class)
    REXLOG_INFO("scenefeed census: {:<34} {:<9} instances {:>6} attached {:>6} hidden {:>5} rendered {:>5}", name,
                KindName(c.kind), c.instances, c.attached, c.hidden, c.rendered);
}

// Is <p> a live UObject (its Index points back at it in the object table)?
bool IsObject(uint32_t p) {
  if (!Readable(p) || !g_objects) return false;
  const uint32_t index = U32(p + 0x04);
  return index < U32(g_objects + 4) && U32(U32(g_objects) + 4 * index) == p;
}

// Research dump for the native particle structures: every attached ParticleSystemComponent's EmitterInstances,
// raw (guest memory, big-endian), plus whatever their first 0x200 bytes point at. emitters_<frame>.bin holds
// regions {u32 address, u32 length, bytes}; emitters_<frame>.json indexes components and names the UObjects those
// pointers hit.
void DumpEmitters(uint32_t frame, const std::vector<Primitive>& prims) {
  static Field instances;
  std::string json = fmt::format("{{\"frame\":{},\"components\":[\n", frame);
  std::string bin;
  std::unordered_map<uint32_t, bool> done;
  auto region = [&](uint32_t a, uint32_t len) {
    if (!Readable(a) || done.count(a)) return;
    done[a] = true;
    const uint32_t h[2] = {a, len};
    bin.append(reinterpret_cast<const char*>(h), 8);
    bin.append(reinterpret_cast<const char*>(Raw(a)), len);
  };
  std::unordered_map<uint32_t, std::string> objects;
  bool first = true;
  for (const Primitive& p : prims) {
    if (p.kind != Kind::kParticle) continue;
    const uint32_t c = p.component;
    if (!instances.ok()) instances = FindProp(U32(c + 0x34), "EmitterInstances");
    if (!instances.ok()) return;
    const uint32_t data = U32(c + instances.offset), n = U32(c + instances.offset + 4);
    if (!first) json += ",\n";
    first = false;
    json += fmt::format("{{\"component\":{},\"template\":", c);
    Str(json, Path(g_off.template_ref.ok() ? U32(c + g_off.template_ref.offset) : 0));
    json += ",\"name\":";
    Str(json, Path(c));
    json += ",\"transform\":";
    Floats(json, c + g_off.local_to_world.offset, 16);
    json += ",\"instances\":[";
    for (uint32_t i = 0; Readable(data) && i < n && i < 64; ++i) {
      const uint32_t inst = U32(data + 4 * i);
      json += fmt::format("{}{}", i ? "," : "", inst);
      if (!Readable(inst)) continue;
      region(inst, 0x400);
      for (uint32_t k = 0; k < 0x200; k += 4) {
        const uint32_t q = U32(inst + k);
        if (!Readable(q) || q == inst) continue;
        if (IsObject(q)) {
          objects[q] = Path(q) + " (" + ClassName(q) + ")";
          continue;
        }
        region(q, 0x2000);
      }
    }
    json += "]}";
  }
  json += "\n],\"objects\":{";
  first = true;
  for (const auto& [a, name] : objects) {
    json += fmt::format("{}\"{}\":", first ? "" : ",", a);
    first = false;
    Str(json, name);
  }
  json += "}}\n";
  char stem[64];
  std::snprintf(stem, sizeof(stem), "/emitters_%06u", frame);
  std::ofstream(g_state.dir + stem + ".json", std::ios::binary) << json;
  std::ofstream(g_state.dir + stem + ".bin", std::ios::binary) << bin;
}

void RequestDraws(int frames) {
  std::error_code ec;
  std::filesystem::create_directories(g_state.dir, ec);
  std::ofstream(g_state.dir + "/draws.req") << frames << "\n";
  REXLOG_INFO("scenefeed: asked the renderer for {} frames of draws ({}/draws.req)", frames, g_state.dir);
}
}  // namespace

bool Command(uint8_t* base, const std::string& cmd) {
  if (cmd.rfind("scenefeed", 0) != 0) return false;
  g_base = base;
  char what[32] = {}, extra[32] = {};
  int n = 0, every = 1;
  std::sscanf(cmd.c_str(), "scenefeed %31s %d %31s", what, &n, extra);
  const std::string w = what;
  if (w == "dump") {
    if (std::isdigit(uint8_t(extra[0]))) every = std::max(1, std::atoi(extra));
    g_state.dump_left = std::max(1, n), g_state.dump_every = every;
    std::error_code ec;
    std::filesystem::create_directories(g_state.dir, ec);
    REXLOG_INFO("scenefeed: dumping {} frames (every {}) to {}", g_state.dump_left, every, g_state.dir);
  } else if (w == "time") {
    g_state.time_left = std::max(1, n), g_state.cached = std::string_view(extra) == "cached";
  } else if (w == "census") {
    Census();
  } else if (w == "emitters") {
    g_state.emitters_left = std::max(1, n);
    std::error_code ec;
    std::filesystem::create_directories(g_state.dir, ec);
  } else if (w == "draws") {
    RequestDraws(std::max(1, n));
  } else {
    REXLOG_WARN("scenefeed: unknown command '{}'", cmd);
  }
  return true;
}

void Tick(uint8_t* base) {
  State& st = g_state;
  if (!st.dump_left && !st.time_left && !st.emitters_left) return;
  g_base = base;
  // Once per game frame: the input poll runs more than once a frame, so only a new RealTimeSeconds counts.
  Camera cam;
  uint32_t world_info = 0;
  const auto t0 = std::chrono::steady_clock::now();
  ReadCamera(cam, world_info);
  static Field real_time;
  if (Readable(world_info) && !real_time.ok()) real_time = FindProp(U32(world_info + 0x34), "RealTimeSeconds");
  if (!real_time.ok() || !Readable(world_info)) return;
  const float rt = F32(world_info + real_time.offset);
  if (rt == st.last_real_time) return;
  st.last_real_time = rt;
  ++st.frame;

  Times t;
  const bool dumping = st.dump_left > 0;
  const bool full = dumping || !st.cached || ++st.prims_age >= 64;
  if (full) {
    Enumerate(st.prims, t);
    st.prims_age = 0;
  } else {
    // Incremental-tracking stand-in: drop entries that were detached or reused since the last full walk.
    std::erase_if(st.prims, [](const Primitive& p) {
      return !Readable(p.component) || Classify(U32(p.component + 0x34)).kind != p.kind ||
             !U32(p.component + g_off.scene_info.offset);
    });
  }
  const auto t1 = std::chrono::steady_clock::now();
  t.enumerate = Ms(t0, t1);
  t.primitives = uint32_t(st.prims.size());
  if (!t.objects && g_objects) t.objects = U32(g_objects + 4);
  const int64_t host_us =
      std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();

  if (st.emitters_left > 0) {
    if (!full) Enumerate(st.prims, t);
    DumpEmitters(st.frame, st.prims);
    if (!--st.emitters_left) REXLOG_INFO("scenefeed: emitter dump finished (frame {})", st.frame);
    if (!dumping && !st.time_left) return;
  }
  if (dumping) {
    if ((st.frame % st.dump_every) == 0) {
      const auto j0 = std::chrono::steady_clock::now();
      const std::string json = FrameJson(st.frame, cam, world_info, st.prims, host_us);
      const auto j1 = std::chrono::steady_clock::now();
      char name[64];
      std::snprintf(name, sizeof(name), "/frame_%06u.json", st.frame);
      std::ofstream(st.dir + name, std::ios::binary) << json;
      const auto j2 = std::chrono::steady_clock::now();
      t.json = Ms(j0, j1), t.write = Ms(j1, j2);
      ++st.written;
      --st.dump_left;
      st.times.push_back(t);
      if (!st.dump_left) {
        REXLOG_INFO("scenefeed: wrote {} frames to {} (last: {} primitives)", st.written, st.dir, t.primitives);
        ReportTimes();
      }
    }
    return;
  }
  // Timing only: read every field the JSON would carry, without formatting.
  const auto e0 = std::chrono::steady_clock::now();
  volatile float sink = 0;
  for (const Primitive& p : st.prims) {
    const uint32_t c = p.component;
    for (int i = 0; i < 16; ++i) sink = sink + F32(c + g_off.local_to_world.offset + 4 * i);
    for (int i = 0; i < 7; ++i) sink = sink + F32(c + g_off.bounds.offset + 4 * i);
    sink = sink + float(U32(c + g_off.hidden_game.offset) & 1) + F32(c + g_off.last_render_time.offset);
    if (g_off.owner.ok()) sink = sink + float(U32(c + g_off.owner.offset) & 1);
    if (p.kind == Kind::kSkeletal && g_off.space_bases.ok()) {
      const uint32_t data = U32(c + g_off.space_bases.offset), n = U32(c + g_off.space_bases.offset + 4);
      for (uint32_t i = 0; Readable(data) && i < n && i < 1024; ++i)
        for (int k = 0; k < 16; ++k) sink = sink + F32(data + 64 * i + 4 * k);
    }
  }
  t.extract = Ms(e0, std::chrono::steady_clock::now());
  st.times.push_back(t);
  if (!--st.time_left) {
    REXLOG_INFO("scenefeed time ({}):", st.cached ? "cached list, full walk every 64 frames" : "full walk every frame");
    ReportTimes();
  }
}


// Hybrid fill 4: publish this frame's static and skeletal meshes to the n1 renderer (hybrid_state.h HybridScene). A cached
// primitive list, re-walked every BOTS_N1_RESCAN frames (default 16; the walk costs about 1.5 ms), each entry checked
// every frame as the timing mode does. Paths are interned per component and rebuilt when its mesh or materials change.
namespace {
struct LiveComponent {
  uint32_t name = 0, outer = 0, mesh = 0, mats = 0, mat_count = 0;
  const char* mesh_path = nullptr;
  const char* light_path = nullptr;
  uint32_t dle = 0;
  const char* env_path = nullptr;
  bool runtime = false;  // a material was made at run time (MaterialPath): re-read its parameters every frame
  std::vector<const char*> materials;
  std::vector<uint64_t> fingerprints;  // per material, when `runtime` (MaterialFingerprint)
};
// What an emitter's LOD level says about how to draw it, cached per LOD level (the property reads walk class records).
struct LiveEmitter {
  uint32_t name = 0, outer = 0, material = 0, type_data = 0;
  int kind = -1;  // 0 sprite, 1 mesh, -1 not drawn (beam, trail, other)
  uint32_t align = 0, sub_h = 1, sub_v = 1, sub_method = 0, local_space = 0;
  const char* material_path = nullptr;
  const char* mesh_path = nullptr;
};
// A SpeedTreeComponent: the paths the renderer needs, cached until the component's tree changes.
struct LiveTree {
  uint32_t tree = 0;
  const char* component_path = nullptr;
  const char* asset_path = nullptr;
};
struct LiveState {
  std::vector<Primitive> prims;
  std::vector<Primitive> particle_prims;
  std::vector<Primitive> tree_prims;
  std::unordered_map<uint32_t, LiveTree> trees;
  bool trees_on = [] {
    const char* v = std::getenv("BOTS_N1_TREES");
    return !(v && !std::strncmp(v, "off", 3));
  }();
  std::unordered_map<uint32_t, LiveEmitter> emitters;
  bool particles = [] {
    const char* v = std::getenv("BOTS_N1_PARTICLES");
    return !(v && (v[0] == '0'));
  }();
  uint32_t age = 1u << 30, rescan = [] {
    const char* v = std::getenv("BOTS_N1_RESCAN");
    uint32_t every = uint32_t(v && *v ? std::max(1, std::atoi(v)) : 16);
    // Particle components come and go within a few frames (muzzle flashes): look for them more often.
    const char* pv = std::getenv("BOTS_N1_PRESCAN");
    const char* on = std::getenv("BOTS_N1_PARTICLES");
    if (on && on[0] == '0') return every;
    return std::min(every, uint32_t(pv && *pv ? std::max(1, std::atoi(pv)) : 4));
  }();
  float last_real_time = -1;
  uint64_t frame = 0;
  uint32_t lighting_builds = 0, env_stamp = 0;
  bool json_feed = [] {
    const char* v = std::getenv("BOTS_N1_FEED");
    return v && !std::strcmp(v, "json");
  }();
  bool verify_feed = [] {
    const char* v = std::getenv("BOTS_N1_FEED_VERIFY");
    return v && v[0] == '1';
  }();
  HybridLighting next_light, last_light;  // the build in progress; the last state the renderer was given
  HybridFog last_fog = {};
  HybridPost last_post = {};
  const char* last_material = nullptr;
  bool have_fog_post = false;
  uint32_t feed_every = [] {
    const char* v = std::getenv("BOTS_N1_FEED_EVERY");
    return uint32_t(v && *v ? std::max(0, std::atoi(v)) : 2);
  }();
  std::unordered_map<uint32_t, LiveComponent> components;
  std::unordered_map<std::string, std::unique_ptr<char[]>> strings;
  HybridScene building;
  // Tracking (BOTS_N1_TRACK, default on): the lists come from the FScene::Add* hooks plus a slow sliced walk, not from
  // a whole-table walk every few frames. `known` holds every component on one of the lists.
  bool track = [] {
    const char* v = std::getenv("BOTS_N1_TRACK");
    return !(v && v[0] == '0');
  }();
  uint32_t slice = [] {  // the sliced walk covers the table in this many frames
    const char* v = std::getenv("BOTS_N1_TRACK_SLICES");
    return uint32_t(v && *v ? std::max(1, std::atoi(v)) : 64);
  }();
  bool tracking = false;  // the first full walk has run
  uint32_t walk_pos = 0;
  std::unordered_set<uint32_t> known;
  std::vector<uint32_t> lights, fog;
};
LiveState g_live;

// Components the game added to a scene (FScene::AddPrimitive / AddLight / AddHeightFog / AddFogVolume, hooked below)
// since the feed last looked. Filled on the game thread; the mutex only guards against an add from another thread.
std::mutex g_added_mutex;
std::vector<uint32_t> g_added;

const char* Intern(const std::string& v) {
  if (v.empty()) return nullptr;
  auto it = g_live.strings.find(v);
  if (it == g_live.strings.end()) {
    auto copy = std::make_unique<char[]>(v.size() + 1);
    std::memcpy(copy.get(), v.c_str(), v.size() + 1);
    it = g_live.strings.emplace(v, std::move(copy)).first;
  }
  return it->second.get();
}

// Intern a material name. Run-time instances carry their parameters ("<path>?..."), and some change them per use
// (tracer smoke: a start time and length per shot), so each variant is a new n1 material that is never freed. Cap
// the variants: past kVariantsPerBase for one cooked material, or kVariantsTotal in all, new ones fall back to the
// cooked material's own values.
const char* InternMaterial(const std::string& v) {
  constexpr size_t kVariantsPerBase = 64, kVariantsTotal = 4096;
  static std::unordered_map<std::string, size_t> per_base;
  static size_t total = 0;
  const size_t q = v.find('?');
  if (q == std::string::npos || g_live.strings.count(v)) return Intern(v);
  size_t& n = per_base[v.substr(0, q)];
  if (n >= kVariantsPerBase || total >= kVariantsTotal) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("scenefeed: material variant cap reached ({}); using cooked values", v);
    }
    return Intern(v.substr(0, q));
  }
  ++n, ++total;
  return Intern(v);
}

// Material instances the game makes at run time: in the Transient package (weapon pickups, tinted characters) or
// under a level's actors ("<Map>.TheWorld.PersistentLevel.<Actor>.MaterialInstanceConstant_N": tracer smoke,
// vehicle paint). No cooked file holds them.
bool RuntimeObject(const std::string& path) {
  return path.starts_with("Transient.") || path.find(".TheWorld.") != std::string::npos;
}

// One parameter array of a MaterialInstanceConstant (ScalarParameterValues, VectorParameterValues,
// TextureParameterValues), appended as "&<kind>:<name>=<value>" unless an outer instance already set that name.
void InstanceParams(std::string& out, std::vector<uint32_t>& seen, uint32_t mic, const char* array, char kind) {
  const uint32_t rec = PropRecord(mic, array);
  if (!rec) return;
  const uint32_t inner = U32(rec + 0x84);  // UArrayProperty Inner: a StructProperty
  const uint32_t st = Readable(inner) ? U32(inner + 0x84) : 0;
  if (!Readable(st)) return;
  const uint32_t size = U32(inner + 0x48);
  const Field name = FindProp(st, "ParameterName"), value = FindProp(st, "ParameterValue");
  if (!name.ok() || !value.ok() || !size || size > 0x200) return;
  const uint32_t at = mic + U32(rec + 0x64), data = U32(at), n = U32(at + 4);
  for (uint32_t k = 0; Readable(data) && k < n && k < 32; ++k) {
    const uint32_t e = data + k * size, nm = U32(e + name.offset);
    const uint32_t key = (uint32_t(kind) << 24) ^ nm;
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
    seen.push_back(key);
    fmt::format_to(std::back_inserter(out), "&{}:{}=", kind, NameText(nm));
    if (kind == 's') {
      fmt::format_to(std::back_inserter(out), "{:.4g}", F32(e + value.offset));
    } else if (kind == 'v') {
      const uint32_t v = e + value.offset;
      fmt::format_to(std::back_inserter(out), "{:.4g},{:.4g},{:.4g},{:.4g}", F32(v), F32(v + 4), F32(v + 8), F32(v + 12));
    } else {
      const uint32_t t = U32(e + value.offset);
      if (IsObject(t)) out += Path(t);
    }
  }
}

// A material's path for the renderer: the first cooked material up the Parent chain. With `params`, a run-time
// instance's own parameter values follow after '?' ("<path>?&s:Name=v&v:Name=r,g,b,a&t:Name=<texture>"; n1's
// matir applies them over the cooked chain), so per-instance looks (paint, tint, fades) survive. `runtime` is set
// when the material was made at run time, as its parameters can change from frame to frame.
std::string MaterialPath(uint32_t m, bool params, bool* runtime) {
  std::string extra;
  std::vector<uint32_t> seen;
  for (int depth = 0; depth < 8 && IsObject(m); ++depth) {
    std::string p = Path(m);
    if (!RuntimeObject(p)) return extra.empty() ? p : p + "?" + extra;
    if (runtime) *runtime = true;
    if (params && !std::strcmp(ClassName(m), "MaterialInstanceConstant")) {
      InstanceParams(extra, seen, m, "ScalarParameterValues", 's');
      InstanceParams(extra, seen, m, "VectorParameterValues", 'v');
      InstanceParams(extra, seen, m, "TextureParameterValues", 't');
    }
    m = PropU32(m, "Parent");
  }
  return {};
}

// Everything MaterialPath(m, true) reads, hashed without building any strings: each object up the Parent chain (its
// address, name, outer and class) and, for a MaterialInstanceConstant, the raw bytes of its three parameter arrays
// (names, values and texture references). Equal fingerprints give the same MaterialPath, so a run-time instance's
// path is rebuilt only when one of these changed. The property records are looked up once per class. Texture
// parameters are hashed by address: a texture freed and another loaded at the same address while the instance
// keeps pointing at it would be missed (BOTS_N1_MIC_CHECK=1 counts any mismatch).
uint64_t MaterialFingerprint(uint32_t m) {
  struct Layout {
    bool mic = false;
    uint32_t offset[3] = {}, size[3] = {};  // size 0: the class has no such array
  };
  static std::unordered_map<uint32_t, Layout> layouts;
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  for (int depth = 0; depth < 8 && IsObject(m); ++depth) {
    const uint32_t cls = U32(m + 0x34);
    mix(m), mix(U32(m + 0x2C)), mix(U32(m + 0x28)), mix(cls);
    auto [it, fresh] = layouts.try_emplace(cls);
    Layout& l = it->second;
    if (fresh && !std::strcmp(ClassName(m), "MaterialInstanceConstant")) {
      l.mic = true;
      const char* arrays[3] = {"ScalarParameterValues", "VectorParameterValues", "TextureParameterValues"};
      for (int k = 0; k < 3; ++k) {
        const uint32_t rec = PropRecord(m, arrays[k]);
        const uint32_t inner = rec ? U32(rec + 0x84) : 0;
        const uint32_t size = Readable(inner) ? U32(inner + 0x48) : 0;
        if (size && size <= 0x200) l.offset[k] = U32(rec + 0x64), l.size[k] = size;
      }
    }
    if (l.mic) {
      for (int k = 0; k < 3; ++k) {
        if (!l.size[k]) continue;
        const uint32_t at = m + l.offset[k], data = U32(at), n = std::min<uint32_t>(U32(at + 4), 32);
        mix(data), mix(n);
        if (!Readable(data)) continue;
        const uint8_t* bytes = Raw(data);
        for (uint32_t i = 0, end = n * l.size[k]; i < end; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
      }
    }
    m = PropU32(m, "Parent");
  }
  return h;
}
}  // namespace

namespace {
// B-188: the lighting feed as binary records (hybrid_state.h HybridLighting), read from the same guest objects
// CompactLightJson / LightsPostJson walk, with the same per-field defaults the renderer applies to a missing JSON
// property. Property records are looked up once per class.
struct FieldEx {
  uint32_t offset = 0xFFFFFFFF, mask = 0, sub = 0;  // sub: the struct class of a struct property, the enum of a byte one
  bool ok() const { return offset != 0xFFFFFFFF; }
};

FieldEx FindEx(uint32_t cls, const char* name) {
  for (int depth = 0; Readable(cls) && depth < 16; cls = U32(cls + 0x3C), ++depth)
    for (uint32_t f = U32(cls + 0x50), n = 0; Readable(f) && n < 4096; f = U32(f + 0x40), ++n)
      if (!std::strcmp(NameText(U32(f + 0x2C)), name)) {
        FieldEx out{U32(f + 0x64), 0, U32(f + kBoolMask)};
        if (!std::strcmp(ClassName(f), "BoolProperty")) out.mask = out.sub;
        return out;
      }
  return {};
}

// A member of a struct property: the struct's class is the property's `sub`.
FieldEx Member(const FieldEx& prop, const char* name) { return prop.ok() ? FindEx(prop.sub, name) : FieldEx{}; }

float FieldF(uint32_t base, const FieldEx& f, float fallback) { return f.ok() ? F32(base + f.offset) : fallback; }
bool FieldB(uint32_t base, const FieldEx& f) { return f.ok() && (U32(base + f.offset) & f.mask); }

// A Color struct's R, G, B members (bytes).
struct ColorLayout {
  FieldEx r, g, b;
  void Init(const FieldEx& prop) { r = Member(prop, "R"), g = Member(prop, "G"), b = Member(prop, "B"); }
  void Bytes(uint32_t base, const FieldEx& prop, float out[3]) const {
    const uint32_t at = base + prop.offset;
    out[0] = r.ok() ? float(*Raw(at + r.offset)) : 0.f;
    out[1] = g.ok() ? float(*Raw(at + g.offset)) : 0.f;
    out[2] = b.ok() ? float(*Raw(at + b.offset)) : 0.f;
  }
};

constexpr uint32_t kLfEnabled = 1, kLfBaked = 2, kLfCast = 4, kLfStatic = 8, kLfDynamic = 16;

struct LightLayout {
  int kind = -1;  // HybridLight::kind, -1: not a class the renderer reads
  FieldEx l2w, brightness, color, affects, channels, direct, lower_b, lower_c, wsil, env, enabled, radius, falloff,
      inner, outer, cast, ch_static, ch_dynamic;
  ColorLayout color_l, lower_l;
  FieldEx sh_r, sh_g, sh_b, sh_rv, sh_gv, sh_bv;
};

const LightLayout& LayoutFor(uint32_t light) {
  static std::unordered_map<uint32_t, LightLayout> layouts;
  const uint32_t cls = U32(light + 0x34);
  auto [it, fresh] = layouts.try_emplace(cls);
  LightLayout& L = it->second;
  if (!fresh) return L;
  const std::string_view name = ClassName(light);
  L.kind = name == "PointLightComponent"                  ? 0
           : name == "SpotLightComponent"                 ? 1
           : name == "DirectionalLightComponent"          ? 2
           : name == "SphericalHarmonicLightComponent"    ? 3
           : name == "SkyLightComponent"                  ? 4
                                                          : -1;
  if (L.kind < 0) return L;
  L.l2w = FindEx(cls, "LightToWorld"), L.brightness = FindEx(cls, "Brightness"), L.color = FindEx(cls, "LightColor");
  L.affects = FindEx(cls, "LightAffectsClassification"), L.channels = FindEx(cls, "LightingChannels");
  L.direct = FindEx(cls, "UseDirectLightMap"), L.lower_b = FindEx(cls, "LowerBrightness");
  L.lower_c = FindEx(cls, "LowerColor"), L.wsil = FindEx(cls, "WorldSpaceIncidentLighting");
  L.env = FindEx(cls, "LightEnvironment"), L.enabled = FindEx(cls, "bEnabled"), L.radius = FindEx(cls, "Radius");
  L.falloff = FindEx(cls, "FalloffExponent"), L.inner = FindEx(cls, "InnerConeAngle");
  L.outer = FindEx(cls, "OuterConeAngle"), L.cast = FindEx(cls, "CastDynamicShadows");
  L.ch_static = Member(L.channels, "Static"), L.ch_dynamic = Member(L.channels, "Dynamic");
  L.color_l.Init(L.color), L.lower_l.Init(L.lower_c);
  L.sh_r = Member(L.wsil, "R"), L.sh_g = Member(L.wsil, "G"), L.sh_b = Member(L.wsil, "B");
  L.sh_rv = Member(L.sh_r, "V"), L.sh_gv = Member(L.sh_g, "V"), L.sh_bv = Member(L.sh_b, "V");
  return L;
}

// A DynamicLightEnvironmentComponent's path, cached per object (re-read when the slot's name or outer changes).
struct DleInfo {
  uint32_t name = 0, outer = 0;
  const char* path = nullptr;
  uint32_t stamp = 0, index = 0;
};
std::unordered_map<uint32_t, DleInfo> g_dle;

uint32_t EnvIndex(HybridLighting& out, uint32_t dle, uint32_t stamp) {
  DleInfo& d = g_dle[dle];
  if (!d.path || d.name != U32(dle + 0x2C) || d.outer != U32(dle + 0x28)) {
    d.name = U32(dle + 0x2C), d.outer = U32(dle + 0x28);
    d.path = Intern(Path(dle));
    d.stamp = 0;
  }
  if (d.stamp != stamp) {
    d.stamp = stamp;
    d.index = uint32_t(out.paths.size());
    out.paths.push_back(d.path);
  }
  return d.index;
}

// The renderer's cut of the feed's lights (see LightsPostJson): baked or disabled level point and spot lights are
// the lightmaps' business and left out.
void LightsBin(HybridLighting& out, uint32_t stamp) {
  for (uint32_t l : g_extras.lights) {
    if (!IsObject(l)) continue;
    const LightLayout& L = LayoutFor(l);
    if (L.kind < 0) continue;
    const uint32_t dle = L.env.ok() ? U32(l + L.env.offset) : 0;
    const bool has_env = IsObject(dle);
    const bool baked = FieldB(l, L.direct) || (L.affects.ok() && *Raw(l + L.affects.offset) == 2);
    if (!has_env && (L.kind == 0 || L.kind == 1) && (!FieldB(l, L.enabled) || baked)) continue;
    HybridLight& h = out.lights.emplace_back();
    std::memset(&h, 0, sizeof(h));
    h.kind = uint32_t(L.kind);
    h.flags = (FieldB(l, L.enabled) ? kLfEnabled : 0) | (baked ? kLfBaked : 0) | (FieldB(l, L.cast) ? kLfCast : 0) |
              (!L.channels.ok() || (L.ch_static.ok() && (U32(l + L.channels.offset + L.ch_static.offset) & L.ch_static.mask))
                   ? kLfStatic
                   : 0) |
              (!L.channels.ok() || (L.ch_dynamic.ok() && (U32(l + L.channels.offset + L.ch_dynamic.offset) & L.ch_dynamic.mask))
                   ? kLfDynamic
                   : 0);
    h.env = has_env ? EnvIndex(out, dle, stamp) : kHybridNone;
    h.radius = FieldF(l, L.radius, 0.f), h.falloff = FieldF(l, L.falloff, 2.f);
    h.inner_deg = FieldF(l, L.inner, 0.f), h.outer_deg = FieldF(l, L.outer, 44.f);
    if (L.l2w.ok()) {
      const uint32_t m = l + L.l2w.offset;
      for (int k = 0; k < 3; ++k) h.pos[k] = F32(m + 4 * (12 + k)), h.axis[k] = F32(m + 4 * (8 + k));
    }
    const float brightness = FieldF(l, L.brightness, 1.f);
    float c[3] = {0, 0, 0};
    if (L.color.ok()) L.color_l.Bytes(l, L.color, c);
    for (int k = 0; k < 3; ++k) h.color[k] = (c[k] / 255.f) * brightness;
    const float lower_b = FieldF(l, L.lower_b, 0.f);
    float lc[3] = {0, 0, 0};
    if (L.lower_c.ok()) L.lower_l.Bytes(l, L.lower_c, lc);
    for (int k = 0; k < 3; ++k) h.lower[k] = (lc[k] / 255.f) * lower_b;
    h.sh = -1;
    if (L.wsil.ok() && L.sh_rv.ok() && L.sh_gv.ok() && L.sh_bv.ok()) {
      const uint32_t w = l + L.wsil.offset;
      h.sh = int32_t(out.sh.size() / 27);
      for (const auto& [m, v] : {std::pair{&L.sh_r, &L.sh_rv}, {&L.sh_g, &L.sh_gv}, {&L.sh_b, &L.sh_bv}})
        for (int k = 0; k < 9; ++k) out.sh.push_back(F32(w + m->offset + v->offset + 4 * k));
    }
  }
}

void FogBin(HybridFog& fog) {
  std::memset(&fog, 0, sizeof(fog));
  for (uint32_t f : g_extras.fog) {
    if (!IsObject(f) || std::strcmp(ClassName(f), "HeightFogComponent")) continue;
    const uint32_t cls = U32(f + 0x34);
    static std::unordered_map<uint32_t, std::array<FieldEx, 7>> layouts;
    auto [it, fresh] = layouts.try_emplace(cls);
    auto& L = it->second;
    if (fresh) {
      const char* names[7] = {"bEnabled", "LightColor", "LightBrightness", "Density", "Height", "StartDistance",
                              "ExtinctionDistance"};
      for (int i = 0; i < 7; ++i) L[i] = FindEx(cls, names[i]);
    }
    if (!FieldB(f, L[0])) return;  // the first height fog decides, enabled or not
    fog.present = 1;
    float c[3] = {0, 0, 0};
    if (L[1].ok()) {
      ColorLayout cl;
      cl.Init(L[1]);
      cl.Bytes(f, L[1], c);
    }
    const float k = FieldF(f, L[2], 1.f);
    for (int i = 0; i < 3; ++i) fog.color[i] = (c[i] / 255.f) * k, fog.light_color[i] = c[i] / 255.f;
    fog.density = FieldF(f, L[3], 0.f), fog.height = FieldF(f, L[4], 0.f), fog.start = FieldF(f, L[5], 0.f);
    fog.extinction = FieldF(f, L[6], 1e8f);
    return;
  }
}

// Three numbers of a Color (bytes), Vector or LinearColor struct property, as the JSON writes them.
void Triple(uint32_t base, const FieldEx& prop, float out[3]) {
  out[0] = out[1] = out[2] = std::nanf("");
  if (!prop.ok()) return;
  const std::string_view type = NameText(U32(prop.sub + 0x2C));
  const uint32_t at = base + prop.offset;
  if (type == "Color") {
    const char* m[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; ++i)
      if (const FieldEx f = FindEx(prop.sub, m[i]); f.ok()) out[i] = float(*Raw(at + f.offset));
  } else {
    const bool vec = type == "Vector";
    const char* m[3] = {vec ? "X" : "R", vec ? "Y" : "G", vec ? "Z" : "B"};
    for (int i = 0; i < 3; ++i)
      if (const FieldEx f = FindEx(prop.sub, m[i]); f.ok()) out[i] = F32(at + f.offset);
  }
}

enum : uint32_t { kPfUber = 1, kPfGod = 2, kPfLsBloom = 4, kPfLsBalance = 8, kPfLsScene = 16, kPfLsTone = 32, kPfLast = 64, kPfUberDiffKernel = 128 };

// The player's post chain and blended settings (LightsPostJson's `post`), as raw numbers.
void PostBin(HybridLighting& out) {
  HybridPost& p = out.post;
  std::memset(&p, 0, sizeof(p));
  const float nan = std::nanf("");
  p.material_effect = kHybridNone;
  p.uber_bloom_scale = p.uber_bloom_threshold = p.ls_bloom_scale = p.ls_bloom_threshold = nan;
  p.desaturation = p.tone_min = p.tone_max = p.tone_scale = nan;
  for (float& v : p.balance) v = 255.f;
  for (int i = 0; i < 3; ++i) p.shadows[i] = p.midtones[i] = p.highlights[i] = nan;
  for (float& v : p.god) v = nan;
  for (float& v : p.uber_dof) v = nan;
  p.present = 1;
  out.post_material = nullptr;
  const uint32_t engine = Engine();
  const uint32_t players = IsObject(engine) ? PropRecord(engine, "GamePlayers") : 0;
  const uint32_t lp = players && U32(engine + U32(players + 0x64) + 4) ? U32(U32(engine + U32(players + 0x64))) : 0;
  const uint32_t chain = IsObject(lp) ? PropU32(lp, "PlayerPostProcess") : 0;
  if (IsObject(chain)) {
    const Field effects = Prop(chain, "Effects");
    const uint32_t data = effects.ok() ? U32(chain + effects.offset) : 0, n = effects.ok() ? U32(chain + effects.offset + 4) : 0;
    for (uint32_t i = 0; Readable(data) && i < n && i < 32; ++i) {
      const uint32_t e = U32(data + 4 * i);
      if (!IsObject(e)) continue;
      const uint32_t cls = U32(e + 0x34);
      const std::string_view name = ClassName(e);
      if (name == "SfUberPostProcessEffect") {
        p.flags |= kPfUber;
        p.uber_bloom_scale = FieldF(e, FindEx(cls, "BloomScale"), nan);
        p.uber_bloom_threshold = FieldF(e, FindEx(cls, "BloomThreshold"), nan);
        const char* dof[7] = {"BloomKernelSize", "BlurKernelSize",    "FalloffExponent", "MaxNearBlurAmount",
                              "MaxFarBlurAmount", "FocusInnerRadius", "FocusDistance"};
        for (int k = 0; k < 7; ++k) p.uber_dof[k] = FieldF(e, FindEx(cls, dof[k]), nan);
        if (FieldB(e, FindEx(cls, "UseDifferentKernelForBloom"))) p.flags |= kPfUberDiffKernel;
      } else if (name == "MaterialEffect") {
        const FieldEx m = FindEx(cls, "Material");
        const uint32_t mat = m.ok() ? U32(e + m.offset) : 0;
        out.post_material = IsObject(mat) ? Intern(Path(mat)) : nullptr;
      } else if (name == "SfGodRaysEffect") {
        if (!FieldB(e, FindEx(cls, "Enabled"))) continue;
        p.flags |= kPfGod;
        p.god[0] = FieldF(e, FindEx(cls, "Exposure"), nan);
        p.god[1] = FieldF(e, FindEx(cls, "DecayPerSample"), nan);
        p.god[2] = FieldF(e, FindEx(cls, "RayMagnitude"), nan);
        float c[3];
        Triple(e, FindEx(cls, "ModulateRayColour"), c);
        for (int k = 0; k < 3; ++k) p.god[3 + k] = std::isnan(c[k]) ? 255.f : c[k];
        p.god[6] = FieldF(e, FindEx(cls, "vTextureSpaceLightPointX"), nan);
        p.god[7] = FieldF(e, FindEx(cls, "vTextureSpaceLightPointY"), nan);
      }
    }
  }
  if (!IsObject(lp)) return;
  const FieldEx cur = FindEx(U32(lp + 0x34), "CurrentPPInfo");
  const FieldEx last = Member(cur, "LastSettings");
  if (!last.ok()) return;
  const uint32_t s = lp + cur.offset + last.offset, sc = last.sub;
  p.flags |= kPfLast;
  if (FieldB(s, FindEx(sc, "bEnableBloom"))) p.flags |= kPfLsBloom;
  p.ls_bloom_scale = FieldF(s, FindEx(sc, "Bloom_Scale"), nan);
  p.ls_bloom_threshold = FieldF(s, FindEx(sc, "Bloom_Threshold"), nan);
  if (FieldB(s, FindEx(sc, "bEnableColourBalanceEffect"))) {
    p.flags |= kPfLsBalance;
    const char* names[3] = {"ColourBalance_Shadows", "ColourBalance_Midtones", "ColourBalance_Highlights"};
    for (int i = 0; i < 3; ++i) {
      float c[3];
      Triple(s, FindEx(sc, names[i]), c);
      for (int k = 0; k < 3; ++k) p.balance[3 * i + k] = std::isnan(c[k]) ? 255.f : c[k];
    }
  }
  if (FieldB(s, FindEx(sc, "bEnableSceneEffect"))) {
    p.flags |= kPfLsScene;
    p.desaturation = FieldF(s, FindEx(sc, "Scene_Desaturation"), nan);
    Triple(s, FindEx(sc, "Scene_Shadows"), p.shadows);
    Triple(s, FindEx(sc, "Scene_MidTones"), p.midtones);
    Triple(s, FindEx(sc, "Scene_HighLights"), p.highlights);
  }
  if (FieldB(s, FindEx(sc, "bEnableToneMappingEffect"))) p.flags |= kPfLsTone;
  p.tone_min = FieldF(s, FindEx(sc, "ToneMapping_MinLuminosity"), nan);
  p.tone_max = FieldF(s, FindEx(sc, "ToneMapping_MaxLuminosity"), nan);
  p.tone_scale = FieldF(s, FindEx(sc, "ToneMapping_BrightnessScale"), nan);
}

bool SameLights(const HybridLighting& a, const HybridLighting& b) {
  return a.lights.size() == b.lights.size() && a.sh == b.sh && a.paths == b.paths &&
         (a.lights.empty() || !std::memcmp(a.lights.data(), b.lights.data(), a.lights.size() * sizeof(HybridLight)));
}
}  // namespace

namespace {
// The frame's particle emitters: every drawable emitter instance of the attached ParticleSystemComponents, with its
// live particles copied out of guest memory (see FParticleEmitterInstance above for the layout).
// UE3 skips a primitive whose bounds centre is farther from the view than its cull distance (CullDistance, or the
// CachedCullDistance a cull-distance volume gave it; the smaller when both are set; later engines call these
// MaxDrawDistance), though the game still ticks it: distant fires, props and decals the level designers cut.
bool BeyondDrawDistance(uint32_t c, const Camera& cam) {
  if (!cam.ok || !g_off.bounds.ok()) return false;
  float limit = 0;
  for (const Field* f : {&g_off.cull_dist, &g_off.cached_cull}) {
    if (!f->ok()) continue;
    const float v = F32(c + f->offset);
    if (v > 0 && std::isfinite(v) && (limit == 0 || v < limit)) limit = v;
  }
  if (limit == 0) return false;
  const uint32_t b = c + g_off.bounds.offset;
  const float dx = F32(b) - cam.location[0], dy = F32(b + 4) - cam.location[1], dz = F32(b + 8) - cam.location[2];
  return dx * dx + dy * dy + dz * dz > limit * limit;
}

// Tracking: keep the primitive, light and fog lists without walking the whole object table (about 110k objects,
// 3-4 ms on a laptop CPU) every few frames. A component enters the lists when the game adds it to a scene (the hooks at
// the end of this file) or when a slow walk of 1/BOTS_N1_TRACK_SLICES of the table per frame finds it attached (a
// safety net for anything attached another way, such as a light that is attached while disabled). Every entry is
// checked every frame exactly as before (still readable, same class kind, still attached) and dropped when it fails,
// so a detached or freed component leaves the lists on the next frame. The lists are kept in object-table order (by
// UObject Index), the order the full walk produced. The first frame does one full walk.
bool TrackAdd(LiveState& st, uint32_t o) {
  if (!IsObject(o) || st.known.count(o)) return false;
  const ClassInfo& ci = Classify(U32(o + 0x34));
  if (ci.extra != Extra::kNone) {
    if (!Attached(o)) return false;
    (ci.extra == Extra::kLight ? st.lights : st.fog).push_back(o);
  } else if (ci.kind == Kind::kStatic || ci.kind == Kind::kSkeletal || (ci.kind == Kind::kParticle && st.particles) ||
             (ci.kind == Kind::kSpeedTree && st.trees_on)) {
    if (!g_off.scene_info.ok() || !U32(o + g_off.scene_info.offset)) return false;
    (ci.kind == Kind::kParticle    ? st.particle_prims
     : ci.kind == Kind::kSpeedTree ? st.tree_prims
                                   : st.prims)
        .push_back({o, ci.kind});
  } else {
    return false;
  }
  st.known.insert(o);
  return true;
}

void Track(LiveState& st) {
  bool added = false;
  if (!st.tracking) {
    Times unused;
    std::vector<Primitive> all;
    Enumerate(all, unused);
    if (!g_objects) return;
    st.tracking = true;
    st.prims.clear(), st.particle_prims.clear(), st.tree_prims.clear(), st.lights.clear(), st.fog.clear();
    st.known.clear();
    for (uint32_t o : g_extras.lights) added |= TrackAdd(st, o);
    for (uint32_t o : g_extras.fog) added |= TrackAdd(st, o);
    for (const Primitive& p : all) added |= TrackAdd(st, p.component);
    std::lock_guard lock(g_added_mutex);
    g_added.clear();
  }
  // Drop what was detached, freed or reused since the last frame.
  auto gone = [&st](const Primitive& p) {
    const bool g = !Readable(p.component) || Classify(U32(p.component + 0x34)).kind != p.kind ||
                   !U32(p.component + g_off.scene_info.offset);
    if (g) st.known.erase(p.component);
    return g;
  };
  std::erase_if(st.prims, gone);
  std::erase_if(st.particle_prims, gone);
  std::erase_if(st.tree_prims, gone);
  auto gone_extra = [&st](uint32_t o) {
    const bool g = !IsObject(o) || Classify(U32(o + 0x34)).extra == Extra::kNone || !Attached(o);
    if (g) st.known.erase(o);
    return g;
  };
  std::erase_if(st.lights, gone_extra);
  std::erase_if(st.fog, gone_extra);
  // What the game added since the last frame.
  {
    std::vector<uint32_t> fresh;
    {
      std::lock_guard lock(g_added_mutex);
      fresh.swap(g_added);
    }
    for (uint32_t o : fresh) added |= TrackAdd(st, o);
  }
  // The safety-net slice.
  if (FindObjectTable()) {
    const uint32_t data = U32(g_objects), count = U32(g_objects + 4);
    const uint32_t n = count / st.slice + 1;
    if (st.walk_pos >= count) st.walk_pos = 0;
    const uint32_t end = std::min(count, st.walk_pos + n);
    uint32_t found = 0;
    for (uint32_t i = st.walk_pos; i < end; ++i) {
      const uint32_t o = U32(data + 4 * i);
      if (o && !st.known.count(o) && TrackAdd(st, o)) ++found;
    }
    st.walk_pos = end;
    if (found) {
      added = true;
      static uint32_t logged = 0;
      if (logged++ < 20) REXLOG_INFO("scenefeed: the tracking walk found {} component(s) no hook reported", found);
    }
  }
  if (added) {
    auto by_index = [](const Primitive& a, const Primitive& b) { return U32(a.component + 4) < U32(b.component + 4); };
    auto by_index_u = [](uint32_t a, uint32_t b) { return U32(a + 4) < U32(b + 4); };
    std::sort(st.prims.begin(), st.prims.end(), by_index);
    std::sort(st.particle_prims.begin(), st.particle_prims.end(), by_index);
    std::sort(st.tree_prims.begin(), st.tree_prims.end(), by_index);
    std::sort(st.lights.begin(), st.lights.end(), by_index_u);
    std::sort(st.fog.begin(), st.fog.end(), by_index_u);
  }
  // BOTS_N1_TRACK_CHECK=<n>: every n frames compare the lists with a full walk and log the differences (testing).
  static const uint32_t check = [] {
    const char* v = std::getenv("BOTS_N1_TRACK_CHECK");
    return uint32_t(v && *v ? std::max(0, std::atoi(v)) : 0);
  }();
  static uint32_t check_age = 0;
  if (check && ++check_age >= check) {
    check_age = 0;
    Times unused;
    std::vector<Primitive> all;
    Enumerate(all, unused);  // also refills g_extras from the walk
    std::unordered_set<uint32_t> want;
    for (const Primitive& p : all)
      if (p.kind == Kind::kStatic || p.kind == Kind::kSkeletal || (p.kind == Kind::kParticle && st.particles) ||
          (p.kind == Kind::kSpeedTree && st.trees_on))
        want.insert(p.component);
    for (uint32_t o : g_extras.lights) want.insert(o);
    for (uint32_t o : g_extras.fog) want.insert(o);
    uint32_t missing = 0, extra = 0;
    for (uint32_t o : want) missing += !st.known.count(o);
    for (uint32_t o : st.known) extra += !want.count(o);
    REXLOG_INFO("scenefeed: tracking check: {} tracked, {} in a full walk, {} missing, {} extra", st.known.size(),
                want.size(), missing, extra);
  }
  // LightsPostJson reads g_extras.
  g_extras.lights = st.lights;
  g_extras.fog = st.fog;
}

void LiveParticles(LiveState& st, HybridScene& out, const Camera& cam) {
  static Field instances;
  constexpr uint32_t kMaxRows = 24576;  // particles per frame; the renderer applies its own (lower) cap
  for (const Primitive& p : st.particle_prims) {
    const uint32_t c = p.component;
    bool hidden = g_off.hidden_game.ok() && (U32(c + g_off.hidden_game.offset) & g_off.hidden_game.mask);
    const uint32_t owner = g_off.owner.ok() ? U32(c + g_off.owner.offset) : 0;
    if (Readable(owner) && g_off.actor_hidden.ok())
      hidden |= (U32(owner + g_off.actor_hidden.offset) & g_off.actor_hidden.mask) != 0;
    if (hidden || BeyondDrawDistance(c, cam)) continue;
    if (!instances.ok()) instances = FindProp(U32(c + 0x34), "EmitterInstances");
    if (!instances.ok()) return;
    const uint32_t data = U32(c + instances.offset), n = U32(c + instances.offset + 4);
    if (!Readable(data)) continue;
    for (uint32_t i = 0; i < n && i < 64; ++i) {
      const uint32_t inst = U32(data + 4 * i);
      if (!Readable(inst)) continue;
      const uint32_t active = U32(inst + kEmitActive), stride = U32(inst + kEmitStride);
      if (!active || stride < 0x80 || stride > 0x1000 || active > 4096) continue;
      const uint32_t lod = U32(inst + kEmitLod);
      if (!IsObject(lod)) continue;
      const uint32_t material = U32(inst + kEmitMaterial);
      LiveEmitter& le = st.emitters[lod];
      const uint32_t type_data0 = PropU32(lod, "TypeDataModule");
      if (le.name != U32(lod + 0x2C) || le.outer != U32(lod + 0x28) || le.material != material ||
          le.type_data != type_data0) {
        le = {};
        le.name = U32(lod + 0x2C), le.outer = U32(lod + 0x28), le.material = material, le.type_data = type_data0;
        const uint32_t required = PropU32(lod, "RequiredModule");
        const std::string type_class = IsObject(type_data0) ? ClassName(type_data0) : "";
        le.kind = type_class.empty() ? 0 : type_class.find("Mesh") != std::string::npos ? 1 : -1;
        if (IsObject(required)) {
          le.align = PropByte(required, "ScreenAlignment");
          le.local_space = PropBool(required, "bUseLocalSpace");
          le.sub_h = std::max<int32_t>(1, int32_t(PropU32(required, "SubImages_Horizontal", 1)));
          le.sub_v = std::max<int32_t>(1, int32_t(PropU32(required, "SubImages_Vertical", 1)));
          le.sub_method = PropByte(required, "InterpolationMethod");
        }
        le.material_path = Intern(IsObject(material) ? MaterialPath(material)
                                                      : (IsObject(required) ? MaterialPath(PropU32(required, "Material")) : ""));
        if (le.kind == 1) le.mesh_path = Intern(Path(PropU32(type_data0, "Mesh")));
      }
      if (le.kind < 0 || !le.material_path || (le.kind == 1 && !le.mesh_path)) continue;
      const uint32_t pdata = U32(inst + kEmitData), pindex = U32(inst + kEmitIndices);
      if (!Readable(pdata) || !Readable(pindex & ~3u)) continue;
      if (out.particle_rows.size() / 15 + active > kMaxRows) return;
      // Payload floats: all of them for sub-UV animation (the renderer finds the frame data), the rotation for mesh
      // particles, none otherwise.
      const bool subuv = (le.sub_h > 1 || le.sub_v > 1) && le.sub_method != 0;
      const uint32_t payload = subuv ? (stride - 0x80) / 4 : (le.kind == 1 ? 3 : 0);
      const uint32_t row_floats = 15 + payload;
      HybridScene::Emitter& e = out.emitters.emplace_back();
      e.kind = uint32_t(le.kind), e.align = le.align, e.sub_h = le.sub_h, e.sub_v = le.sub_v;
      e.sub_method = le.sub_method, e.local_space = le.local_space;
      e.material = le.material_path, e.mesh = le.mesh_path;
      e.count = active, e.row_offset = uint32_t(out.particle_rows.size()), e.row_floats = row_floats;
      for (int k = 0; k < 3; ++k) e.origin[k] = F32(inst + kEmitLocation + 4 * k);
      if (le.local_space)
        for (int k = 0; k < 16; ++k) e.transform[k] = F32(c + g_off.local_to_world.offset + 4 * k);
      else
        std::memset(e.transform, 0, sizeof(e.transform));
      out.particle_rows.reserve(out.particle_rows.size() + size_t(active) * row_floats);
      for (uint32_t k = 0; k < active; ++k) {
        const uint16_t slot = uint16_t(__builtin_bswap16(*reinterpret_cast<const uint16_t*>(Raw(pindex + 2 * k))));
        const uint32_t pp = pdata + slot * stride;
        for (uint32_t off : {0x10u, 0x14u, 0x18u, 0x00u, 0x04u, 0x08u, 0x50u, 0x54u, 0x58u, 0x2Cu, 0x60u, 0x64u, 0x68u,
                             0x6Cu, 0x0Cu})
          out.particle_rows.push_back(F32(pp + off));
        for (uint32_t q = 0; q < payload; ++q) out.particle_rows.push_back(F32(pp + 0x80 + 4 * q));
      }
    }
  }
}
}  // namespace

void Live(uint8_t* base) {
  HybridState* hs = FindHybridState();
  if (!hs || hs->fill.load(std::memory_order_relaxed) != 4) return;
  g_base = base;
  LiveState& st = g_live;
  Camera cam;
  uint32_t world_info = 0;
  ReadCamera(cam, world_info);
  static Field real_time;
  if (Readable(world_info) && !real_time.ok()) real_time = FindProp(U32(world_info + 0x34), "RealTimeSeconds");
  if (!real_time.ok() || !Readable(world_info)) return;
  const float rt = F32(world_info + real_time.offset);
  if (rt == st.last_real_time) return;
  st.last_real_time = rt;
  const auto t0 = std::chrono::steady_clock::now();
  if (st.track) {
    Track(st);
  } else if (++st.age >= st.rescan) {
    Times unused;
    Enumerate(st.prims, unused);
    st.particle_prims.clear();
    st.tree_prims.clear();
    for (const Primitive& p : st.prims) {
      if (st.particles && p.kind == Kind::kParticle) st.particle_prims.push_back(p);
      if (st.trees_on && p.kind == Kind::kSpeedTree) st.tree_prims.push_back(p);
    }
    std::erase_if(st.prims, [](const Primitive& p) { return p.kind != Kind::kStatic && p.kind != Kind::kSkeletal; });
    st.age = 0;
  } else {
    auto gone = [](const Primitive& p) {
      return !Readable(p.component) || Classify(U32(p.component + 0x34)).kind != p.kind ||
             !U32(p.component + g_off.scene_info.offset);
    };
    std::erase_if(st.prims, gone);
    std::erase_if(st.particle_prims, gone);
    std::erase_if(st.tree_prims, gone);
  }
  HybridScene& out = st.building;
  out.instances.clear();
  out.skeletal.clear();
  out.bones.clear();
  out.materials.clear();
  for (const Primitive& p : st.prims) {
    const uint32_t c = p.component;
    const bool skel = p.kind == Kind::kSkeletal;
    const Field& ref = skel ? g_off.skeletal_mesh_ref : g_off.static_mesh_ref;
    if (!ref.ok()) continue;
    const uint32_t mesh = U32(c + ref.offset);
    if (!Readable(mesh)) continue;
    bool hidden = g_off.hidden_game.ok() && (U32(c + g_off.hidden_game.offset) & g_off.hidden_game.mask);
    const uint32_t owner = g_off.owner.ok() ? U32(c + g_off.owner.offset) : 0;
    if (Readable(owner)) {
      if (!g_off.actor) {
        g_off.actor = true;
        g_off.actor_hidden = FindProp(U32(owner + 0x34), "bHidden");
      }
      if (g_off.actor_hidden.ok()) hidden |= (U32(owner + g_off.actor_hidden.offset) & g_off.actor_hidden.mask) != 0;
    }
    if (hidden || BeyondDrawDistance(c, cam)) continue;
    uint32_t mats = 0, mat_count = 0;
    if (g_off.materials.ok()) {
      mats = U32(c + g_off.materials.offset);
      mat_count = std::min<uint32_t>(U32(c + g_off.materials.offset + 4), 64);
      if (!Readable(mats)) mats = 0, mat_count = 0;
    }
    LiveComponent& lc = st.components[c];
    if (lc.name != U32(c + 0x2C) || lc.outer != U32(c + 0x28) || lc.mesh != mesh || lc.mats != mats ||
        lc.mat_count != mat_count) {
      lc.name = U32(c + 0x2C), lc.outer = U32(c + 0x28), lc.mesh = mesh, lc.mats = mats, lc.mat_count = mat_count;
      lc.mesh_path = Intern(Path(mesh));
      lc.light_path = Intern(Path(c));
      lc.materials.clear();
      lc.runtime = false;
      for (uint32_t i = 0; i < mat_count; ++i)
        lc.materials.push_back(InternMaterial(MaterialPath(U32(mats + 4 * i), true, &lc.runtime)));
      if (lc.runtime) {
        lc.fingerprints.resize(mat_count);
        for (uint32_t i = 0; i < mat_count; ++i) lc.fingerprints[i] = MaterialFingerprint(U32(mats + 4 * i));
      }
    } else if (lc.runtime) {  // run-time instances: their parameters (fades, tints) can change every frame
      // Rebuild a material's path only when what it is built from changed (MaterialFingerprint); building it means
      // name lookups and string formatting for every parameter, every frame.
      // BOTS_N1_MIC_CHECK=1 builds the path anyway when the fingerprint says unchanged and counts mismatches (testing).
      static const bool mic_check = [] {
        const char* v = std::getenv("BOTS_N1_MIC_CHECK");
        return v && *v && *v != '0';
      }();
      for (uint32_t i = 0; i < mat_count; ++i) {
        const uint64_t fp = MaterialFingerprint(U32(mats + 4 * i));
        if (fp == lc.fingerprints[i]) {
          if (mic_check) {
            static uint64_t checked = 0, wrong = 0;
            wrong += InternMaterial(MaterialPath(U32(mats + 4 * i), true)) != lc.materials[i];
            if (++checked % 20000 == 0)
              REXLOG_INFO("scenefeed: MIC check: {} unchanged fingerprints, {} paths differed", checked, wrong);
          }
          continue;
        }
        lc.fingerprints[i] = fp;
        lc.materials[i] = InternMaterial(MaterialPath(U32(mats + 4 * i), true));
      }
    }
    // The owner's light environment (characters, movers, pickups): lit by its composite lights in n1.
    const uint32_t dle = PropU32(c, "LightEnvironment");
    if (dle != lc.dle) lc.dle = dle, lc.env_path = IsObject(dle) ? Intern(Path(dle)) : nullptr;
    if (!lc.mesh_path) continue;
    if (skel) {  // culled by bounds in n1 (the feed's `rendered` flag flickers)
      HybridScene::Skel& sk = out.skeletal.emplace_back();
      for (int i = 0; i < 16; ++i) sk.transform[i] = F32(c + g_off.local_to_world.offset + 4 * i);
      sk.mesh = lc.mesh_path;
      sk.bone_offset = uint32_t(out.bones.size() / 16);
      sk.bone_count = 0;
      if (g_off.space_bases.ok()) {
        const uint32_t data = U32(c + g_off.space_bases.offset), n = U32(c + g_off.space_bases.offset + 4);
        if (Readable(data) && n <= 1024) {
          for (uint32_t i = 0; i < 16 * n; ++i) out.bones.push_back(F32(data + 4 * i));
          sk.bone_count = n;
        }
      }
      if (g_off.bounds.ok()) {  // FBoxSphereBounds {Origin, BoxExtent, SphereRadius}
        const uint32_t b = c + g_off.bounds.offset;
        sk.bounds[0] = F32(b), sk.bounds[1] = F32(b + 4), sk.bounds[2] = F32(b + 8), sk.bounds[3] = F32(b + 24);
      } else {
        sk.bounds[0] = sk.bounds[1] = sk.bounds[2] = 0, sk.bounds[3] = 1e30f;  // never culled
      }
      sk.env = lc.env_path;
      sk.mat_offset = uint32_t(out.materials.size());
      sk.mat_count = uint32_t(lc.materials.size());
      out.materials.insert(out.materials.end(), lc.materials.begin(), lc.materials.end());
      continue;
    }
    HybridScene::Instance& inst = out.instances.emplace_back();
    for (int i = 0; i < 16; ++i) inst.transform[i] = F32(c + g_off.local_to_world.offset + 4 * i);
    inst.mesh = lc.mesh_path;
    inst.light = lc.light_path;
    inst.env = lc.env_path;
    inst.mat_offset = uint32_t(out.materials.size());
    inst.mat_count = uint32_t(lc.materials.size());
    out.materials.insert(out.materials.end(), lc.materials.begin(), lc.materials.end());
  }
  out.walked = uint32_t(st.prims.size());
  // SpeedTree placements (B-187): the component's LocalToWorld (scale + translation), its tree asset and the actor's
  // rotation, which UE3 keeps out of the component transform for SpeedTree.
  out.trees.clear();
  for (const Primitive& p : st.tree_prims) {
    const uint32_t c = p.component;
    bool hidden = g_off.hidden_game.ok() && (U32(c + g_off.hidden_game.offset) & g_off.hidden_game.mask);
    const uint32_t owner = g_off.owner.ok() ? U32(c + g_off.owner.offset) : 0;
    if (Readable(owner)) {
      if (!g_off.actor) {
        g_off.actor = true;
        g_off.actor_hidden = FindProp(U32(owner + 0x34), "bHidden");
      }
      if (g_off.actor_hidden.ok()) hidden |= (U32(owner + g_off.actor_hidden.offset) & g_off.actor_hidden.mask) != 0;
    }
    if (hidden) continue;
    const uint32_t tree = PropU32(c, "SpeedTree");
    if (!Readable(tree)) continue;
    LiveTree& lt = st.trees[c];
    if (lt.tree != tree || !lt.component_path) {
      lt.tree = tree;
      lt.component_path = Intern(Path(c));
      lt.asset_path = Intern(Path(tree));
    }
    if (!lt.component_path || !lt.asset_path) continue;
    HybridScene::Tree& t = out.trees.emplace_back();
    for (int i = 0; i < 16; ++i) t.transform[i] = F32(c + g_off.local_to_world.offset + 4 * i);
    t.component = lt.component_path;
    t.asset = lt.asset_path;
    t.rotation[0] = t.rotation[1] = t.rotation[2] = 0;
    if (Readable(owner)) {
      const Field rot = Prop(owner, "Rotation");
      if (rot.ok())
        for (int i = 0; i < 3; ++i) t.rotation[i] = int32_t(U32(owner + rot.offset + 4 * i));
    }
  }
  out.emitters.clear();
  out.particle_rows.clear();
  out.particles_ms = 0;
  if (st.particles && !st.particle_prims.empty()) {
    const auto tp = std::chrono::steady_clock::now();
    LiveParticles(st, out, cam);
    out.particles_ms = float(Ms(tp, std::chrono::steady_clock::now()));
  }
  // Lights, fog and post for n1 (properties read live; the component list comes from the last walk). Binary records
  // (B-188) unless BOTS_N1_FEED=json; both with BOTS_N1_FEED_VERIFY=1.
  out.lighting.clear();
  out.lighting_ms = 0;
  out.light_pending = false;
  if (st.feed_every && st.frame % st.feed_every == 0) {
    const auto tl = std::chrono::steady_clock::now();
    const bool fp = st.lighting_builds++ % 16 == 0;
    if (st.json_feed || st.verify_feed) {
      out.lighting = "{\"v\":1";
      LightsPostJson(out.lighting, world_info, true, fp);
      out.lighting += '}';
    }
    if (!st.json_feed) {
      HybridLighting& nl = st.next_light;
      nl.Clear();
      LightsBin(nl, ++st.env_stamp);
      bool post_changed = false;
      if (fp) {
        FogBin(nl.fog);
        PostBin(nl);
        post_changed = st.verify_feed || !st.have_fog_post || std::memcmp(&nl.fog, &st.last_fog, sizeof(HybridFog)) ||
                       std::memcmp(&nl.post, &st.last_post, sizeof(HybridPost)) || nl.post_material != st.last_material;
      }
      if (st.verify_feed || !SameLights(nl, st.last_light) || post_changed) {
        nl.has_fog_post = post_changed;
        if (post_changed) st.last_fog = nl.fog, st.last_post = nl.post, st.last_material = nl.post_material;
        st.have_fog_post |= post_changed;
        st.last_light.lights = nl.lights, st.last_light.sh = nl.sh, st.last_light.paths = nl.paths;
        out.light = nl;
        out.light_pending = true;
      }
    }
    out.lighting_ms = float(Ms(tl, std::chrono::steady_clock::now()));
  }
  out.frame = ++st.frame;
  out.cam_seq = hs->published.load(std::memory_order_acquire);
  out.build_ms = float(Ms(t0, std::chrono::steady_clock::now()));
  std::lock_guard lock(hs->scene_mutex);
  // Keep a lighting update the renderer has not taken yet when this frame brings none, and fold an unconsumed fog and
  // post update into a newer lights-only one.
  if (out.lighting.empty() && !hs->scene.lighting.empty()) std::swap(out.lighting, hs->scene.lighting);
  if (hs->scene.light_pending) {
    if (!out.light_pending) {
      std::swap(out.light, hs->scene.light);
      out.light_pending = true;
    } else if (hs->scene.light.has_fog_post && !out.light.has_fog_post && !st.verify_feed) {  // (verify: JSON is not merged)
      out.light.has_fog_post = true;
      out.light.fog = hs->scene.light.fog, out.light.post = hs->scene.light.post;
      out.light.post_material = hs->scene.light.post_material;
    }
  }
  std::swap(hs->scene, out);
}

}  // namespace bots::scenefeed

// FScene's add functions (this build; FScene vtable at 0x82096920, found from the render commands each one enqueues,
// whose DescribeCommand returns the names below). Each is called on the game thread with the component as r4; the
// hook calls the original and then records the component for Track(). Nothing in the game's behaviour changes.
//   sub_827834D0  FScene::AddPrimitive(UPrimitiveComponent*)        FAddPrimitiveCommand
//   sub_827836F8  FScene::AddLight(ULightComponent*)                FAddLightCommand
//   sub_8277D9D0  FScene::AddHeightFog(UHeightFogComponent*)        FAddFogCommand
//   sub_82783918  FScene::AddFogVolume(UFogVolumeDensityComponent*, UPrimitiveComponent*)   FAddFogVolumeCommand
extern "C" {
void __imp__sub_827834D0(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_827836F8(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_8277D9D0(PPCContext& __restrict ctx, uint8_t* base);
void __imp__sub_82783918(PPCContext& __restrict ctx, uint8_t* base);
}
namespace {
void Added(uint32_t component) {
  std::lock_guard lock(bots::scenefeed::g_added_mutex);
  if (bots::scenefeed::g_added.size() < (1u << 20)) bots::scenefeed::g_added.push_back(component);
}
}  // namespace
#define BOTS_SCENE_ADD_HOOK(addr)                                       \
  extern "C" void sub_##addr(PPCContext& __restrict ctx, uint8_t* base) { \
    const uint32_t component = ctx.r4.u32;                              \
    __imp__sub_##addr(ctx, base);                                       \
    Added(component);                                                   \
  }
BOTS_SCENE_ADD_HOOK(827834D0)
BOTS_SCENE_ADD_HOOK(827836F8)
BOTS_SCENE_ADD_HOOK(8277D9D0)
BOTS_SCENE_ADD_HOOK(82783918)
#undef BOTS_SCENE_ADD_HOOK
