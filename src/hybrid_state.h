// Hybrid renderer research spike: state shared between the game's hooks (src/hybrid.cpp) and the native renderer
// plugin (src/gfx/), which has no access to the game's symbols.
//
// The game owns the one instance and publishes its address in the process environment (BOTS_HYBRID_STATE, hex)
// during static initialization, before any plugin loads (as prompt_state.h does).
//
// Per frame: the game thread writes the frame's camera (from UE3's FSceneView) into cameras[seq % kCameras] and
// bumps `published`. When the 3D scene is suppressed, the guest render thread writes a marker into the GPU command
// stream at the point the scene would have been drawn: a type-0 write of `seq` to register kMarkerRegister. The
// native command processor sees it in stream order and has the renderer draw the host scene for that camera into the
// back buffer before the HUD's draws, which follow in the same stream.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bots {

// UE3's camera for one frame, as the scene renderer would have used it. Matrices are UE3 FMatrix as stored: 16 floats,
// row-major, row vectors (v' = v * M, translation in row 3); world space is UE3's (left-handed, Z up, cm).
struct HybridCamera {
  float view[16];
  float proj[16];
  float view_proj[16];
  float inv_view_proj[16];
  float location[3];      // view origin (world)
  float fov;              // horizontal, degrees (from the projection)
  int32_t rotation[3];    // pitch, yaw, roll in UE3 units (65536 = 360 degrees), from the view matrix
  float rect[4];          // view X, Y, SizeX, SizeY in back-buffer pixels
  float time;             // ViewFamily CurrentWorldTime
  uint32_t seq;
};

// Fill 4 (n1, hybrid_fill.cpp): the static meshes UE3 has attached to its scene, read from the live engine objects
// on the game thread (scene_feed.cpp) and drawn by an external renderer library. Assets are named by UE3 path; the
// strings are interned by the game and never freed, so the renderer may key its caches on the pointers.
// B-188: the lighting feed as fixed-layout binary records (no JSON). The layouts are the same as lightfeed::bin in the
// renderer and N1Feed* in n1ffi.h (sizes asserted on both sides); the game fills them straight from guest memory.
inline constexpr uint32_t kHybridNone = 0xFFFFFFFFu;
struct HybridLight {  // 80 bytes
  uint32_t kind;      // 0 point, 1 spot, 2 directional, 3 spherical harmonic, 4 sky
  uint32_t flags;     // 1 enabled, 2 baked, 4 casts dynamic shadows, 8 lights static, 16 lights dynamic
  uint32_t env;       // index in HybridLighting::paths (the owner's DynamicLightEnvironmentComponent) or kHybridNone
  float radius, falloff, inner_deg, outer_deg;
  float pos[3];       // LightToWorld translation (UE3 axes)
  float axis[3];      // LightToWorld Z row
  float color[3];     // LightColor / 255 * Brightness
  float lower[3];     // LowerColor / 255 * LowerBrightness
  int32_t sh;         // index of its 27 floats (R, G, B x 9) in HybridLighting::sh, or -1
};
struct HybridFog {  // 32 bytes
  uint32_t present;
  float color[3];
  float density, height, start, extinction;
};
struct HybridPost {  // 152 bytes; NaN = property absent
  uint32_t present, flags, material_effect;
  float uber_bloom_scale, uber_bloom_threshold, ls_bloom_scale, ls_bloom_threshold;
  float balance[9];
  float desaturation, shadows[3], midtones[3], highlights[3];
  float tone_min, tone_max, tone_scale;
  float god[9];
};
static_assert(sizeof(HybridLight) == 80 && sizeof(HybridFog) == 32 && sizeof(HybridPost) == 152);
struct HybridLighting {
  bool has_fog_post = false;  // this update carries fog and post (sent only when they change)
  std::vector<HybridLight> lights;
  std::vector<float> sh;
  std::vector<const char*> paths;  // interned, stable
  HybridFog fog = {};
  HybridPost post = {};
  const char* post_material = nullptr;  // the post chain's material effect (interned), not in `paths`
  void Clear() {
    has_fog_post = false;
    lights.clear();
    sh.clear();
    paths.clear();
  }
};

struct HybridScene {
  struct Instance {
    float transform[16];  // LocalToWorld, UE3 FMatrix as stored
    const char* mesh;     // StaticMesh path
    const char* light;    // the component's path (its baked vertex light), or null
    uint32_t mat_offset, mat_count;  // range in `materials` (Materials overrides; null entries keep the mesh's)
    const char* env;      // the owner's DynamicLightEnvironmentComponent path (LightEnvironment), or null
  };
  struct Skel {
    float transform[16];  // LocalToWorld
    const char* mesh;     // SkeletalMesh path
    uint32_t bone_offset, bone_count;  // range in `bones`, 16 floats each (SpaceBases, component space)
    uint32_t mat_offset, mat_count;    // range in `materials`
    float bounds[4];                   // world bounding sphere: centre, radius
    const char* env;                   // DynamicLightEnvironmentComponent path, or null
  };
  // One particle emitter instance (sprite or mesh) with its live particles in `particle_rows`: `count` rows of
  // `row_floats` floats from `row_offset`, each [location 3, old_location 3, size 3, rotation, colour 4,
  // relative_time, payload...] in UE3 units (guest values; local-space rows are moved to world space by the
  // renderer with `transform`). No JSON: the rows are copied straight from guest memory.
  struct Emitter {
    uint32_t kind;  // 0 sprite, 1 mesh
    uint32_t align, sub_h, sub_v, sub_method, local_space;  // EParticleScreenAlignment, sub-image grid, interpolation
    const char* material;  // the material the emitter draws with
    const char* mesh;      // mesh emitters: StaticMesh path
    uint32_t count, row_offset, row_floats;
    float origin[3];       // emitter location (UE3), for far-to-near ordering
    float transform[16];   // the component's LocalToWorld
  };
  // One SpeedTree placement (B-187): the component's LocalToWorld (scale and translation; the actor's rotation is
  // kept separately), the component's path (its baked vertex light), the tree asset's path and the rotation in UE3
  // units (pitch, yaw, roll).
  struct Tree {
    float transform[16];
    const char* component;
    const char* asset;
    int32_t rotation[3];
  };
  std::vector<Tree> trees;
  std::vector<Emitter> emitters;
  std::vector<float> particle_rows;
  float particles_ms = 0;  // game-thread cost of reading them
  uint64_t frame = 0;  // game frames counted by the feed; 0: nothing published yet
  std::vector<Instance> instances;
  std::vector<Skel> skeletal;
  std::vector<float> bones;
  std::vector<const char*> materials;
  // The frame's lights, fog and post chain (B-188). `light` is set when the game built a new lighting state that
  // differs from the last one it published (every BOTS_N1_FEED_EVERY frames at most; fog and post only when they
  // changed); the renderer takes it (clears `light_pending`) and the game keeps an unconsumed update across frames.
  // `lighting` is the same data as the scene feed's JSON, built only with BOTS_N1_FEED=json (the pre-B-188 path, kept
  // for A/B measurement) or BOTS_N1_FEED_VERIFY=1 (the renderer checks both against each other).
  bool light_pending = false;
  HybridLighting light;
  std::string lighting;
  float lighting_ms = 0;  // game-thread cost of building the lighting update
  float build_ms = 0;  // game-thread cost of this frame's walk
  uint32_t walked = 0;  // primitives looked at
};

struct HybridState {
  static constexpr uint32_t kCameras = 64;
  // What the renderer does at a marker: 0 nothing (the back buffer keeps whatever it held), 1 clear to a test colour,
  // 2 a world-space test grid drawn with the frame's camera, 3 the grid's lines over the game's scene, 4 the
  // static meshes of `scene` drawn by the external n1 renderer (BOTS_N1_* in hybrid_fill.cpp).
  std::atomic<uint32_t> fill{0};
  std::atomic<uint32_t> published{0};  // seq of the newest camera
  // B-187: frames the game should draw its own scene for although the port suppresses it (mode 2), so the SpeedTree
  // capture hook sees the trees' draws. The renderer sets it when a tree asset has no geometry cache yet; Render()
  // counts it down, and the n1 marker is emitted in those frames so the capture session advances.
  std::atomic<int> capture_frames{0};
  std::atomic<uint64_t> markers_seen{0}, fills_drawn{0};  // renderer side, for the log
  HybridCamera cameras[kCameras] = {};
  // Fill 4: the newest scene. The game swaps its finished frame in, the renderer swaps it out (both under the lock).
  std::mutex scene_mutex;
  HybridScene scene;
};

// The register the marker writes: unknown to the Xenos register table (no side effects in the SDK's command
// processor) and never written by the D3D runtime.
inline constexpr uint32_t kHybridMarkerRegister = 0x0030;

inline constexpr const char* kHybridStateEnv = "BOTS_HYBRID_STATE";

// The game's instance, or null (another host, e.g. bots-replay). Looked up once.
inline HybridState* FindHybridState() {
  static HybridState* state = [] {
    char value[32] = {};
#ifdef _WIN32
    if (!GetEnvironmentVariableA(kHybridStateEnv, value, sizeof(value))) return static_cast<HybridState*>(nullptr);
#else
    const char* v = std::getenv(kHybridStateEnv);
    if (!v) return static_cast<HybridState*>(nullptr);
    for (size_t i = 0; v[i] && i + 1 < sizeof(value); ++i) value[i] = v[i];
#endif
    return reinterpret_cast<HybridState*>(uintptr_t(std::strtoull(value, nullptr, 16)));
  }();
  return state;
}

}  // namespace bots
