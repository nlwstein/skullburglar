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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

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
  Field scene_info, local_to_world, bounds, hidden_game, last_render_time, owner, materials, static_mesh_ref,
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

// Lights, fog and post-processing for the frame (see the format notes at the top).
void LightsPostJson(std::string& s, uint32_t world_info) {
  s += ",\"lights\":[";
  bool first = true;
  for (uint32_t l : g_extras.lights) {
    if (!IsObject(l)) continue;
    if (!first) s += ",\n";
    first = false;
    ObjectJson(s, l, "ActorComponent");
  }
  s += "],\"fog\":[";
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
        Str(s, Path(U32(data + 4 * i)));
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

}  // namespace bots::scenefeed
