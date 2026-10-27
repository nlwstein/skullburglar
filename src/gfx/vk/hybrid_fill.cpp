// Hybrid renderer research spike, renderer side (src/hybrid.cpp has the game side): stands in for a separate scene
// renderer. When the game's 3D scene is suppressed, the guest render thread writes a marker (hybrid_state.h) into the
// GPU stream where the scene would have been drawn; native.cpp passes it here, and before the next colour draw (the
// HUD's first) the colour target gets a host-drawn "scene" for the frame's camera:
//   fill 1  a flat test colour over the view rectangle
//   fill 2  a sky gradient and a ground grid (UE3 world units: 1 m lines, 10 m axes) cast through the frame's
//           inverse view-projection, so camera motion shows that the camera is the game's
//   fill 3  the grid lines only, over the game's own scene (not suppressed): lines that stay put on the world as the
//           camera moves check the camera against what the game draws
//   fill 4  the n1 renderer (an external library, built with -DBOTS_N1FFI_DIR=<dir with include/n1ffi.h and
//           libn1ffi.a>): the static meshes the game publishes each frame (HybridScene, scene_feed.cpp Live) drawn
//           from disc assets with the frame's camera, recorded into this command buffer. Environment:
//             BOTS_N1_COOKED   the cooked packages (default game/C1Game/CookedXenon)
//             BOTS_N1_CACHE    a writable cache directory (default cache/n1)
//             BOTS_N1_THREADS  asset loader threads (default 2); BOTS_N1_BUDGET uploads per frame (default 8)
//             BOTS_N1_LOG      seconds between stats lines (default 5)
// It records into the frame's command buffer like a format transfer: its own one-attachment rendering, so the game's
// pass simply reopens for the HUD.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include <glslang/Public/ShaderLang.h>

#include "../../hybrid_state.h"
#include "../../instr_state.h"
#include "renderer.h"

#ifdef BOTS_N1FFI
#include <n1ffi.h>
#endif

namespace bots::gfx::vk {
namespace {

// gl_FragCoord -> NDC in the view rectangle (D3D: +y up) -> two depths through UE3's InvViewProjection (row vectors,
// stored row-major: as a GLSL mat4 it is the transpose, so M * v is UE3's v * M) -> a world ray.
constexpr const char* kFillShader = R"(
layout(push_constant) uniform Fill {
  mat4 inv_view_proj;
  vec4 rect;     // x, y, width, height (host pixels)
  vec4 origin;   // camera location (world), ground height
} fill;
layout(location = 0) out vec4 color;
vec3 Unproject(vec2 ndc, float z) {
  vec4 p = fill.inv_view_proj * vec4(ndc, z, 1.0);
  return p.xyz / p.w;
}
float Lines(vec2 p, float spacing, float width) {
  vec2 g = abs(fract(p / spacing - 0.5) - 0.5) * spacing;
  vec2 w = fwidth(p) * width;
  vec2 l = 1.0 - smoothstep(vec2(0.0), w, g);
  return max(l.x, l.y);
}
void main() {
  vec2 uv = (gl_FragCoord.xy - fill.rect.xy) / fill.rect.zw;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec3 a = Unproject(ndc, 0.0), b = Unproject(ndc, 0.5);
  vec3 dir = normalize(b - a);
  vec3 sky = mix(vec3(0.78, 0.66, 0.50), vec3(0.25, 0.45, 0.75), clamp(dir.z * 2.0, 0.0, 1.0));
  float line = 0.0;
  vec3 ground = sky;
  if (dir.z < -1e-4) {
    float t = (fill.origin.w - a.z) / dir.z;
    vec2 p = a.xy + dir.xy * t;
    float fade = exp(-t / 20000.0);
    float minor = Lines(p, 100.0, 1.0), major = Lines(p, 1000.0, 2.0);
    line = max(minor * 0.5, major) * fade;
    ground = mix(vec3(0.42, 0.36, 0.28), sky, 1.0 - fade);
    // world axes through the origin: X red, Y green
    float ax = 1.0 - smoothstep(0.0, fwidth(p.y) * 3.0, abs(p.y));
    float ay = 1.0 - smoothstep(0.0, fwidth(p.x) * 3.0, abs(p.x));
    ground = mix(ground, vec3(0.9, 0.1, 0.1), ax * fade);
    ground = mix(ground, vec3(0.1, 0.9, 0.1), ay * fade);
    line = max(line, max(ax, ay) * fade);
  }
#if MODE == 3
  if (line < 0.3) discard;
  color = vec4(0.1, 1.0, 1.0, 1.0);
#else
  color = vec4(mix(ground, vec3(1.0), line * (dir.z < 0.0 ? 0.8 : 0.0)), 1.0);
#endif
}
)";

struct FillConstants {
  float inv_view_proj[16];
  float rect[4];
  float origin[4];
};

}  // namespace

void Renderer::QueueSceneFill(uint32_t seq) {
  capture_frame_ = false;  // B-220: the scene's marker ends a capture frame's scene
  // B-210 diagnostics (BOTS_HYBRID_DROP_LOG=<n>): a marker replaced before any draw reached it drew no scene.
  static int drop_log = [] { const char* v = std::getenv("BOTS_HYBRID_DROP_LOG"); return v ? std::atoi(v) : 0; }();
  if (scene_fill_seq_ && drop_log > 0) {
    --drop_log;
    REXLOG_INFO("hybrid fill: marker {} drew no scene: no draw before marker {}", scene_fill_seq_, seq);
  }
  scene_fill_seq_ = seq;
}

#ifdef BOTS_N1FFI
namespace {
struct N1Host {
  N1Renderer* r = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool failed = false;
  uint32_t budget = 8;
  HybridScene scene;  // the newest one taken from the game
  std::unordered_map<const char*, uint32_t> meshes, materials, lights, skel_meshes, envs, pmaterials, pmeshes;
  std::unordered_map<const char*, uint32_t> tree_comps, tree_assets;
  std::vector<N1Tree> trees;
  std::vector<N1Emitter> emitters;
  std::vector<N1Instance> instances;
  std::vector<N1Decal> decals;
  std::vector<N1SkelInstance> skeletal;
  std::vector<uint32_t> skel_materials;
  std::vector<uint32_t> material_handles;
  // Stats since the last log line.
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
  double log_seconds = 5, host_ms = 0, record_ms = 0, build_ms = 0, feed_ms = 0, lighting_ms = 0, particles_ms = 0;
  uint64_t frames = 0, draws = 0, triangles = 0, scenes = 0, light_updates = 0, verified = 0, verify_bad = 0;
  uint32_t cam_seq = 0;  // the camera n1 drew the last frame with
  // Waits for the marker's own scene (BOTS_N1_SCENE_WAIT_MS) and frames still drawn from an older one, per window.
  double wait_ms = 0;
  uint64_t waits = 0, wait_timeouts = 0, drawn_behind = 0;
  bool wait_off = false;
  // n1_record wall time: worst frame and frames over 20 ms in the window (a compile on the render thread shows here).
  double record_max_ms = 0;
  uint64_t record_over_20 = 0;
  const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  // The startup step (n1_precompile_*): when it began, and whether its result has been logged.
  std::chrono::steady_clock::time_point precompile_started{};
  bool precompile_logged = false;
  N1Stats last{};
  // B-199 level preload: the load counters as of the last note (for per-frame deltas), when n1 last drew a scene,
  // packages handed to n1, and the knobs: render-thread ms per loading-screen frame for loads (BOTS_N1_LOAD_MS), the
  // longest hold at level start (BOTS_N1_PRELOAD_HOLD_S, 0: none), the gap that counts as a level start.
  N1Stats prev{};
  std::chrono::steady_clock::time_point last_scene{};
  bool any_scene = false;
  uint32_t packages = 0, holds = 0;
  double load_ms = 4, hold_s = 30, gap_s = 0.5;
};

// Folds the load counters' change since the last call into the frame note.
void NoteLoads(N1Host* host, const N1Stats& st, Renderer::N1FrameNote& note) {
  const N1Stats& p = host->prev;
  note.applied += st.applied - p.applied;
  note.apply_ms += double(st.apply_ms - p.apply_ms);
  note.apply_max_ms = std::max(note.apply_max_ms, double(st.apply_max_ms) > double(p.apply_max_ms) ? double(st.apply_max_ms) : 0.0);
  note.lazy += (st.lazy_meshes + st.lazy_materials + st.lazy_lights + st.lazy_skel + st.lazy_particles + st.lazy_trees) -
               (p.lazy_meshes + p.lazy_materials + p.lazy_lights + p.lazy_skel + p.lazy_particles + p.lazy_trees);
  note.late_skips += st.late_skips - p.late_skips;
  note.textures += st.textures_added - p.textures_added;
  note.meshes += st.meshes_added - p.meshes_added;
  note.bytes += st.upload_bytes - p.upload_bytes;
  host->prev.applied = st.applied, host->prev.apply_ms = st.apply_ms, host->prev.apply_max_ms = st.apply_max_ms;
  host->prev.lazy_meshes = st.lazy_meshes, host->prev.lazy_materials = st.lazy_materials;
  host->prev.lazy_lights = st.lazy_lights, host->prev.lazy_skel = st.lazy_skel;
  host->prev.lazy_particles = st.lazy_particles, host->prev.lazy_trees = st.lazy_trees;
  host->prev.late_skips = st.late_skips, host->prev.textures_added = st.textures_added;
  host->prev.meshes_added = st.meshes_added, host->prev.upload_bytes = st.upload_bytes;
}

// Hands the packages the game opened since the last call to n1 (hybrid.cpp's CreateFile hook).
void TakePackages(N1Host* host, HybridState* state) {
  if (!state || !host->r) return;
  std::vector<std::string> pkgs;
  {
    std::lock_guard lock(state->package_mutex);
    pkgs.swap(state->packages);
  }
  for (const std::string& p : pkgs) host->packages += uint32_t(n1_preload_package(host->r, p.c_str()) > 0);
}
uint32_t Handle(std::unordered_map<const char*, uint32_t>& map, const char* path, N1Renderer* r,
                uint32_t (*make)(N1Renderer*, const char*)) {
  if (!path) return N1_NONE;
  auto it = map.find(path);
  if (it == map.end()) it = map.emplace(path, make(r, path)).first;
  return it->second;
}
const char* EnvOr(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return v && *v ? v : fallback;
}
}  // namespace

// Makes the n1 renderer for colour target `format` (the guest's 8-bit colour target; DrawSceneN1 remakes it if the
// first scene arrives in another). Called at start-up so the startup step can run before the first level.
void* Renderer::EnsureN1(VkFormat format) {
  auto* host = static_cast<N1Host*>(n1_host_);
  if (host) return host;
  host = new N1Host;
  n1_host_ = host;
  host->budget = uint32_t(std::atoi(EnvOr("BOTS_N1_BUDGET", "8")));
  host->load_ms = std::atof(EnvOr("BOTS_N1_LOAD_MS", "4"));
  host->hold_s = std::atof(EnvOr("BOTS_N1_PRELOAD_HOLD_S", "30"));
  host->log_seconds = std::atof(EnvOr("BOTS_N1_LOG", "5"));
  N1Init init{};
  init.get_instance_proc_addr = device_->vulkan_instance()->functions().vkGetInstanceProcAddr;
  init.instance = device_->vulkan_instance()->instance();
  init.physical_device = device_->physical_device();
  init.device = device_->device();
  init.color_format = format;
  init.cooked_dir = EnvOr("BOTS_N1_COOKED", "game/C1Game/CookedXenon");
  init.cache_dir = EnvOr("BOTS_N1_CACHE", "cache/n1");
  init.loader_threads = uint32_t(std::atoi(EnvOr("BOTS_N1_THREADS", "2")));
  const auto t0 = std::chrono::steady_clock::now();
  host->r = n1_create(&init);
  host->format = format;
  n1_tree_r_ = host->r;
  if (!host->r) {
    host->failed = true;
    REXLOG_ERROR("hybrid n1: n1_create failed: {}", n1_last_error());
  } else {
    REXLOG_INFO("hybrid n1: renderer up in {:.1f} ms (target format {})",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), int(format));
  }
  return host;
}

// The startup step's share of the warm-up (renderer.h warmup_total/done): starts it once the hybrid scene is on, then
// follows its progress. Called from PumpWarmup every frame on the GPU thread.
void Renderer::PumpN1Warmup() {
  if (!n1_warm_started_) {
    HybridState* state = FindHybridState();
    if (!state || state->fill.load(std::memory_order_relaxed) != 4) return;
    n1_warm_started_ = true;
    auto* host = static_cast<N1Host*>(EnsureN1(VK_FORMAT_R8G8B8A8_UNORM));
    if (host && host->r) {
      n1_precompile_start(host->r);
      host->precompile_started = std::chrono::steady_clock::now();
    }
  }
  auto* host = static_cast<N1Host*>(n1_host_);
  if (!host || !host->r || host->precompile_logged) return;
  uint32_t done = 0, total = 0;
  const uint32_t state = n1_precompile_progress(host->r, &done, &total);
  if (InstrState* is = FindInstrState()) {
    is->n1_warm_state.store(state, std::memory_order_relaxed);
    is->n1_warm_done.store(done, std::memory_order_relaxed);
    is->n1_warm_total.store(total, std::memory_order_relaxed);
    if (state == 2)
      is->n1_warm_ms.store(uint64_t(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                              host->precompile_started).count()),
                           std::memory_order_relaxed);
  }
  if (state == 1) {
    n1_warm_total_ = std::max<size_t>(total, done + 1), n1_warm_done_ = done;
  } else if (state == 2) {
    n1_warm_total_ = n1_warm_done_ = 0;
    host->precompile_logged = true;
    REXLOG_INFO("hybrid n1: startup step done in {:.1f} s",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - host->precompile_started).count());
  }
}

double Renderer::n1_seconds_since_resume() const {
  if (n1_resumed_.time_since_epoch().count() == 0) return -1;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - n1_resumed_).count();
}

// B-199: from EndFrame (GPU thread), every frame. Hands opened packages to n1; on a frame without a scene (a loading
// screen, a menu, a movie) applies n1's finished loads for up to BOTS_N1_LOAD_MS and records their texture copies
// into this frame's commands, so a level's assets come in while it loads instead of after it starts.
void Renderer::PumpN1Uploads() {
  auto* host = static_cast<N1Host*>(n1_host_);
  if (!host || !host->r || host->failed) return;
  TakePackages(host, FindHybridState());
  if (n1_drawn_ != n1_drawn_pumped_) {  // n1_record ran this frame and applied loads itself
    n1_drawn_pumped_ = n1_drawn_;
    return;
  }
  N1Stats st{};
  n1_load_stats(host->r, &st);
  if (!st.preload_pending && !st.copies_pending && !st.preload_scanning) return;
  uint64_t completed = stats_.flushes;
  for (const Submission& s : submissions_)
    if (s.pending) completed = std::min(completed, s.serial - 1);
  BeginCommands(kGpuDraw);
  EndRendering("n1 upload");
  const auto t0 = std::chrono::steady_clock::now();
  const int32_t n = n1_upload(host->r, cmd_, stats_.flushes + 1, completed, uint32_t(host->load_ms * 1000), &st);
  n1_note_.upload_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (n < 0) REXLOG_WARN("hybrid n1: n1_upload failed: {}", n1_last_error());
  NoteLoads(host, st, n1_note_);
}

// B-218: runs at IssueDraw's ring reservation (before the draw makes its uploads and descriptor sets), the last point
// before DrawSceneFill where a Flush is safe. Remakes the renderer for a new target format (DrawSceneN1 can't: it
// needs the GPU idle) and holds for the level preload at a level start.
// BOTS_N1_HOLD_IN_DRAW=1: the old behaviour (the hold inside the draw, flushing under it), kept for the B-218 A/B.
static bool HoldInDraw() {
  static const bool on = [] { const char* v = std::getenv("BOTS_N1_HOLD_IN_DRAW"); return v && std::atoi(v) != 0; }();
  return on;
}

void Renderer::PrepareSceneN1() {
  HybridState* state = FindHybridState();
  if (!state || state->fill.load(std::memory_order_relaxed) != 4 || HoldInDraw()) return;
  // Once per marker: the draws before the one that carries the scene (depth-only, too small) all come through here.
  if (n1_prepared_seq_ == scene_fill_seq_ && n1_remake_format_ == VK_FORMAT_UNDEFINED) return;
  n1_prepared_seq_ = scene_fill_seq_;
  auto* host = static_cast<N1Host*>(n1_host_);
  if (n1_remake_format_ != VK_FORMAT_UNDEFINED) {
    const VkFormat format = n1_remake_format_;
    n1_remake_format_ = VK_FORMAT_UNDEFINED;
    if (host && host->r && host->format != format) {
      // The first scene is in another colour format than the one the renderer was made for: make it again (the
      // shader and pipeline caches make this quick). The GPU must be idle.
      REXLOG_WARN("hybrid n1: target format {} is not the renderer's {}; remaking it", int(format), int(host->format));
      Finish();
      n1_destroy(host->r);
      delete host;
      n1_host_ = host = nullptr;
      n1_warm_started_ = false;
      n1_warm_total_ = n1_warm_done_ = 0;
      host = static_cast<N1Host*>(EnsureN1(format));
      if (host->r) n1_precompile_start(host->r);
    }
  }
  if (!host || !host->r || host->failed) return;
  TakePackages(host, state);
  HoldN1Preload(host, true);
}

// B-199: the first scene after a gap (level start, end of a loading screen) waits, up to BOTS_N1_PRELOAD_HOLD_S, for
// the level preload: loads applied 8 ms at a time with a submit between (bounds staging memory). The guest waits
// with it, as for the pipeline warm-up. Streamed sublevels never hold: they load while the level plays.
// `may_flush` false (B-218: inside a draw): no hold, the scene draws with what has loaded.
void Renderer::HoldN1Preload(void* h, bool may_flush) {
  auto* host = static_cast<N1Host*>(h);
  const auto now = std::chrono::steady_clock::now();
  const bool resumed = !host->any_scene || std::chrono::duration<double>(now - host->last_scene).count() > host->gap_s;
  if (!resumed) {
    host->last_scene = now;
    return;
  }
  n1_resumed_ = now;
  N1Stats st{};
  n1_load_stats(host->r, &st);
  const uint64_t lazy0 = st.lazy_meshes + st.lazy_materials + st.lazy_lights + st.lazy_skel;
  uint32_t passes = 0;
  const bool work = st.preload_pending || st.copies_pending || st.preload_scanning;
  if (work && host->hold_s > 0 && !may_flush)
    REXLOG_WARN("hybrid n1: level resumed inside a draw: no preload hold ({} loads to go)", st.preload_pending);
  while (may_flush && host->hold_s > 0 && (st.preload_pending || st.copies_pending || st.preload_scanning)) {
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count() > host->hold_s) {
      REXLOG_WARN("hybrid n1: preload hold gave up after {:.0f} s with {} loads to go", host->hold_s,
                  st.preload_pending);
      break;
    }
    uint64_t completed = stats_.flushes;
    for (const Submission& s : submissions_)
      if (s.pending) completed = std::min(completed, s.serial - 1);
    BeginCommands(kGpuDraw);
    EndRendering("n1 preload hold");
    if (n1_upload(host->r, cmd_, stats_.flushes + 1, completed, 8000, &st) < 0) {
      REXLOG_WARN("hybrid n1: n1_upload failed: {}", n1_last_error());
      break;
    }
    Flush();
    ++passes;
    if (!st.applied || st.preload_pending) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto end = std::chrono::steady_clock::now();
  const double held = std::chrono::duration<double, std::milli>(end - now).count();
  NoteLoads(host, st, n1_note_);
  if (passes) {
    n1_note_.held = true, n1_note_.hold_ms += held;
    ++host->holds;
  }
  REXLOG_INFO("hybrid n1: level resumed: preload hold {:.0f} ms ({} passes), {} packages scanned, {} preload "
              "requests, {} loads applied, {:.1f} MB to the GPU ({} textures, {} meshes), {} lazy loads so far, "
              "{} still pending",
              held, passes, host->packages, st.preload_requests, st.applied, double(st.upload_bytes) / 1048576.0,
              st.textures_added, st.meshes_added, lazy0, st.preload_pending);
  if (passes) BeginCommands(kGpuDraw);
  // From the end of the hold: from its start, the next scene saw the hold as a gap and "resumed" again.
  host->any_scene = true, host->last_scene = end;
}

bool Renderer::DrawSceneN1(Image& target, const HybridCamera& cam, const VkRect2D& area) {
  HybridState* state = FindHybridState();
  auto* host = static_cast<N1Host*>(n1_host_);
  if (host && host->r && target.format != host->format) {
    if (scene_mid_draw_) {
      // B-218: remaking needs the GPU idle (a Finish), which can't happen inside a draw: PrepareSceneN1 does it at
      // the next draw's reservation; this scene is skipped.
      n1_remake_format_ = target.format;
      return false;
    }
    n1_remake_format_ = target.format;
    PrepareSceneN1();
    host = static_cast<N1Host*>(n1_host_);
  }
  if (!host) {
    host = static_cast<N1Host*>(EnsureN1(target.format));
    if (host->r) n1_precompile_start(host->r);
  }
  if (host->failed) return false;
  TakePackages(host, state);
  HoldN1Preload(host, !scene_mid_draw_ || HoldInDraw());
  const auto t0 = std::chrono::steady_clock::now();
  {
    // The marker can reach the render thread before the game has published its frame's scene (the feed runs at the next
    // tick's input poll). The older scene would then be drawn: with the marker's camera everything riding the camera
    // (the Humvee, 50 on the gun) jumps back a frame and forward again, the doubling; with the scene's own camera the
    // last frame repeats and the next is skipped. Wait briefly for it (BOTS_N1_SCENE_WAIT_MS, default 6, 0 off). After a
    // timeout (menus, a stalled feed) no more waits until a scene catches up with its marker.
    static const int wait_ms = std::atoi(EnvOr("BOTS_N1_SCENE_WAIT_MS", "6"));
    std::unique_lock lock(state->scene_mutex);
    const auto behind = [&] { return std::max(state->scene.cam_seq, host->scene.cam_seq) < cam.seq; };
    if (wait_ms > 0 && !host->wait_off && host->scene.frame && behind()) {
      const auto w0 = std::chrono::steady_clock::now();
      const bool got = state->scene_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return !behind(); });
      host->wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
      ++host->waits, host->wait_timeouts += !got, host->wait_off = !got;
    }
    if (state->scene.frame > host->scene.frame) {
      std::swap(host->scene, state->scene), ++host->scenes;
      host->lighting_ms += host->scene.lighting_ms;  // every build, changed or not
    }
  }
  // Lights, fog and post: handed over when the game built a new set (every BOTS_N1_FEED_EVERY game frames). Binary
  // records (B-188); the JSON text only for BOTS_N1_FEED=json, and both compared with BOTS_N1_FEED_VERIFY=1.
  if (host->scene.light_pending) {
    const auto f0 = std::chrono::steady_clock::now();
    HybridLighting& l = host->scene.light;
    std::vector<const char*> paths = l.paths;
    N1FeedBin fb{};
    static_assert(sizeof(N1FeedLight) == sizeof(HybridLight) && sizeof(N1FeedFog) == sizeof(HybridFog) &&
                  sizeof(N1FeedPost) == sizeof(HybridPost));
    fb.version = N1_FEED_VERSION;
    fb.flags = l.has_fog_post ? N1_FEED_FOG_POST : 0;
    fb.light_count = uint32_t(l.lights.size());
    fb.sh_count = uint32_t(l.sh.size() / 27);
    fb.lights = reinterpret_cast<const N1FeedLight*>(l.lights.data());
    fb.sh = l.sh.data();
    std::memcpy(&fb.fog, &l.fog, sizeof(fb.fog));
    std::memcpy(&fb.post, &l.post, sizeof(fb.post));
    if (l.has_fog_post && !l.post_materials.empty()) {  // consecutive paths, the count in post flags bits 24..27
      fb.post.material_effect = uint32_t(paths.size());
      fb.post.flags |= uint32_t(l.post_materials.size()) << 24;
      paths.insert(paths.end(), l.post_materials.begin(), l.post_materials.end());
    }
    fb.path_count = uint32_t(paths.size());
    fb.paths = paths.data();
    if (n1_set_feed_bin(host->r, &fb) != 0) REXLOG_WARN("hybrid n1: n1_set_feed_bin failed: {}", n1_last_error());
    if (!host->scene.lighting.empty()) {  // BOTS_N1_FEED_VERIFY=1
      const int diffs = n1_check_feed(host->scene.lighting.data(), uint32_t(host->scene.lighting.size()), &fb);
      ++host->verified;
      if (diffs != 0 && host->verify_bad++ < 8)
        REXLOG_WARN("hybrid n1: feed verify: {} differences: {}", diffs, n1_last_error());
    }
    host->feed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
    host->light_updates++;
    host->scene.light_pending = false;
    host->scene.lighting.clear();
  } else if (!host->scene.lighting.empty()) {
    const auto f0 = std::chrono::steady_clock::now();
    if (n1_set_feed(host->r, host->scene.lighting.data(), uint32_t(host->scene.lighting.size())) != 0)
      REXLOG_WARN("hybrid n1: n1_set_feed failed: {}", n1_last_error());
    host->feed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
    host->light_updates++;
    host->scene.lighting.clear();
  }
  // Handles by interned path (the game never frees the strings). The light first: it names the level package that
  // mesh and material lookups then prefer.
  host->instances.clear();
  host->material_handles.clear();
  for (const HybridScene::Instance& in : host->scene.instances) {
    N1Instance& out = host->instances.emplace_back();
    std::memcpy(out.transform, in.transform, sizeof(out.transform));
    out.light = Handle(host->lights, in.light, host->r, n1_light);
    out.mesh = Handle(host->meshes, in.mesh, host->r, n1_mesh);
    out.env = Handle(host->envs, in.env, host->r, n1_env);
    out.mat_offset = uint32_t(host->material_handles.size());
    out.mat_count = in.mat_count;
    for (uint32_t i = 0; i < in.mat_count; ++i)
      host->material_handles.push_back(Handle(host->materials, host->scene.materials[in.mat_offset + i], host->r,
                                              n1_material));
  }
  // B-228 decals: receivers are indices into `instances` (one per scene instance, same order).
  static_assert(sizeof(N1Decal) == 192, "N1Decal layout");
  host->decals.clear();
  for (const HybridScene::Decal& in : host->scene.decals) {
    N1Decal& out = host->decals.emplace_back();
    out = {};
    out.material = Handle(host->materials, in.material, host->r, n1_material);
    out.id = in.id;
    out.receiver_offset = in.receiver_offset, out.receiver_count = in.receiver_count;
    std::memcpy(out.planes, in.planes, sizeof(out.planes));
    std::memcpy(out.location, in.location, sizeof(out.location));
    std::memcpy(out.tangent, in.tangent, sizeof(out.tangent));
    std::memcpy(out.binormal, in.binormal, sizeof(out.binormal));
    std::memcpy(out.normal, in.normal, sizeof(out.normal));
    out.width = in.width, out.height = in.height, out.backface_angle = in.backface_angle, out.flags = in.flags;
    std::memcpy(out.tile, in.tile, sizeof(out.tile));
    std::memcpy(out.offset, in.offset, sizeof(out.offset));
  }
  n1_set_decals(host->r, host->decals.data(), uint32_t(host->decals.size()), host->scene.decal_receivers.data(),
                uint32_t(host->scene.decal_receivers.size()));
  // Skinned primitives: bones are passed as published (n1 rebuilds unset ones from the reference pose).
  host->skeletal.clear();
  host->skel_materials.clear();
  for (const HybridScene::Skel& in : host->scene.skeletal) {
    N1SkelInstance& out = host->skeletal.emplace_back();
    std::memcpy(out.transform, in.transform, sizeof(out.transform));
    std::memcpy(out.bounds, in.bounds, sizeof(out.bounds));
    out.mesh = Handle(host->skel_meshes, in.mesh, host->r, n1_skel_mesh);
    out.env = Handle(host->envs, in.env, host->r, n1_env);
    out.bone_offset = in.bone_offset, out.bone_count = in.bone_count;
    out.mat_offset = uint32_t(host->skel_materials.size());
    out.mat_count = in.mat_count;
    for (uint32_t i = 0; i < in.mat_count; ++i)
      host->skel_materials.push_back(Handle(host->materials, host->scene.materials[in.mat_offset + i], host->r,
                                            n1_material));
  }
  // Particles: emitters by interned material and mesh path; the rows are the game's copy of the particle data.
  host->emitters.clear();
  for (const HybridScene::Emitter& in : host->scene.emitters) {
    N1Emitter& out = host->emitters.emplace_back();
    out = {};
    out.kind = in.kind, out.align = in.align, out.sub_h = in.sub_h, out.sub_v = in.sub_v;
    out.sub_method = in.sub_method, out.local_space = in.local_space, out.lock_axis = in.lock_axis;
    out.material = Handle(host->pmaterials, in.material, host->r, n1_pmaterial);
    out.mesh = Handle(host->pmeshes, in.mesh, host->r, n1_pmesh);
    out.count = in.count, out.row_offset = in.row_offset, out.row_floats = in.row_floats;
    std::memcpy(out.origin, in.origin, sizeof(out.origin));
    std::memcpy(out.transform, in.transform, sizeof(out.transform));
  }
  n1_set_particles(host->r, host->emitters.data(), uint32_t(host->emitters.size()), host->scene.particle_rows.data(),
                   uint32_t(host->scene.particle_rows.size()));
  host->trees.clear();
  for (const HybridScene::Tree& in : host->scene.trees) {
    N1Tree& out = host->trees.emplace_back();
    out = {};
    std::memcpy(out.transform, in.transform, sizeof(out.transform));
    out.component = Handle(host->tree_comps, in.component, host->r, n1_tree_component);
    out.asset = Handle(host->tree_assets, in.asset, host->r, n1_tree_asset);
    std::memcpy(out.rotation, in.rotation, sizeof(out.rotation));
  }
  n1_set_trees(host->r, host->trees.data(), uint32_t(host->trees.size()));
  n1_set_skeletal(host->r, host->skeletal.data(), uint32_t(host->skeletal.size()), host->scene.bones.data(),
                  uint32_t(host->scene.bones.size()), host->skel_materials.data(),
                  uint32_t(host->skel_materials.size()));
  if (host->scene.cam_seq >= cam.seq) host->wait_off = false;
  host->drawn_behind += host->scene.cam_seq < cam.seq;
  // The scene's own camera (HybridScene::cam_seq) when the ring still holds it; BOTS_N1_SCENE_CAMERA=0: the marker's.
  static const bool scene_camera = std::atoi(EnvOr("BOTS_N1_SCENE_CAMERA", "1")) != 0;
  const HybridCamera* view_cam = &cam;
  if (scene_camera && host->scene.cam_seq) {
    const HybridCamera& sc = state->cameras[host->scene.cam_seq % HybridState::kCameras];
    if (sc.seq == host->scene.cam_seq) view_cam = &sc;
  }
  host->cam_seq = view_cam->seq;
  n1_set_time(host->r, view_cam->time);  // materials' Time is the game's clock (tracer fades compare with it)
  N1Camera camera{};
  std::memcpy(camera.view, view_cam->view, sizeof(camera.view));
  std::memcpy(camera.proj, view_cam->proj, sizeof(camera.proj));
  std::memcpy(camera.location, view_cam->location, sizeof(camera.location));
  // UE3's infinite perspective: proj[14] = -near * proj[10].
  camera.near_plane = view_cam->proj[10] != 0 ? -view_cam->proj[14] / view_cam->proj[10] : 5.0f;
  N1Target t{};
  t.view = target.view;
  t.layout = VK_IMAGE_LAYOUT_GENERAL;
  t.width = target.width, t.height = target.height;
  t.x = area.offset.x, t.y = area.offset.y, t.w = area.extent.width, t.h = area.extent.height;
  t.clear = 1;  // the host covers the whole view every frame
  // BOTS_N1_CLEAR="r,g,b": what n1 clears the view to (the parity harness uses a colour no scene has, to find pixels n1
  // left empty).
  static const std::array<float, 3> clear = [] {
    std::array<float, 3> c{0.32f, 0.40f, 0.50f};
    if (const char* v = std::getenv("BOTS_N1_CLEAR")) std::sscanf(v, "%f,%f,%f", &c[0], &c[1], &c[2]);
    return c;
  }();
  t.clear_color[0] = clear[0], t.clear_color[1] = clear[1], t.clear_color[2] = clear[2], t.clear_color[3] = 1.0f;
  // Serials: this command buffer is submitted as stats_.flushes + 1; everything older than the oldest submission
  // not yet waited for is done.
  uint64_t completed = stats_.flushes;
  for (const Submission& s : submissions_)
    if (s.pending) completed = std::min(completed, s.serial - 1);

  BeginCommands(kGpuDraw);
  EndRendering("hybrid n1");
  Image* const write_image = &target;
  Access({}, {&write_image, 1});
  if (gpu_timing_) GpuLabel(kGpuDraw, "hybrid n1");
  const auto t1 = std::chrono::steady_clock::now();
  N1Stats st{};
  const int32_t rc = n1_record(host->r, cmd_, &t, &camera, host->instances.data(), uint32_t(host->instances.size()),
                               host->material_handles.data(), uint32_t(host->material_handles.size()),
                               stats_.flushes + 1, completed, host->budget, &st);
  const auto t2 = std::chrono::steady_clock::now();
  bound_pipeline_ = VK_NULL_HANDLE;
  dynamic_valid_ = false;
  if (rc != 0) {
    REXLOG_ERROR("hybrid n1: n1_record failed: {}", n1_last_error());
    host->failed = true;
    if (InstrState* is = FindInstrState()) is->n1_failed.store(1, std::memory_order_relaxed);
    return false;
  }
  target.Use(uint32_t(t.x) + t.w, uint32_t(t.y) + t.h);
  {
    const uint32_t cap = n1_tree_end_frame(host->r);
    n1_tree_active_ = cap > 0;
    // Also 0 when a session ends: left at its last count, the game went on drawing its own scene for up to 30 more
    // frames (35-70 ms each on a UHD 620).
    if (state && (cap > 0 || state->capture_frames.load(std::memory_order_relaxed) > 0)) state->capture_frames.store(int(cap));
  }
  {
    const N1Stats& p = host->last;
    N1FrameNote& note = n1_note_;
    ++note.scenes;
    note.record_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
    note.late_pipelines += st.late_pipelines - p.late_pipelines;
    note.late_shaders += st.late_shaders - p.late_shaders;
    note.draws_no_pipe += uint32_t(st.draws_no_pipe - p.draws_no_pipe);
    note.fallback_draws += uint32_t(st.fallback_draws - p.fallback_draws);
    note.tree_capture |= n1_tree_active_;
    NoteLoads(host, st, note);
  }
  host->frames++, host->draws += st.draws, host->triangles += st.triangles, host->last = st;
  if (InstrState* is = FindInstrState()) {  // test instrumentation (instr_state.h)
    constexpr auto r = std::memory_order_relaxed;
    is->n1_frames.fetch_add(1, r);
    is->n1_draws.store(st.draws, r), is->n1_instances.store(st.instances, r), is->n1_skipped.store(st.skipped, r);
    is->n1_particles.store(st.particle_sprites + st.particle_meshes, r);
    is->n1_particle_dropped.fetch_add(st.particle_dropped, r);
    is->n1_meshes_pending.store(st.meshes_pending, r), is->n1_loading.store(st.loading, r);
    is->n1_pipelines_live.store(uint32_t(st.pipelines_live), r), is->n1_shaders_live.store(uint32_t(st.shaders_live), r);
    is->n1_late_pipelines.store(uint32_t(st.late_pipelines), r), is->n1_late_shaders.store(uint32_t(st.late_shaders), r);
    is->n1_build_failures.store(uint32_t(st.build_failures), r);
    is->n1_partial_frames.store(uint32_t(st.partial_frames), r);
    is->n1_draws_no_pipe.store(uint64_t(st.draws_no_pipe), r), is->n1_fallback_draws.store(uint64_t(st.fallback_draws), r);
    const uint32_t us = uint32_t(std::chrono::duration<double, std::micro>(t2 - t1).count());
    if (us > is->n1_record_worst_us.load(r)) is->n1_record_worst_us.store(us, r);
  }
  ++n1_drawn_, n1_scene_frame_ = host->scene.frame, n1_marker_seq_ = cam.seq, n1_view_seq_ = view_cam->seq;
  n1_scene_cam_ = host->scene.cam_seq;
  host->host_ms += std::chrono::duration<double, std::milli>(t2 - t0).count();
  host->record_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
  {
    const double ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    host->record_max_ms = std::max(host->record_max_ms, ms);
    host->record_over_20 += ms > 20.0;
  }
  host->build_ms += host->scene.build_ms;
  host->particles_ms += host->scene.particles_ms;
  const double secs = std::chrono::duration<double>(t2 - host->since).count();
  if (host->log_seconds > 0 && secs >= host->log_seconds) {
    const double n = double(host->frames);
    REXLOG_INFO("hybrid n1: {:.1f} s, {} frames ({} scenes): {} instances ({} skipped) -> {:.0f} draws, {:.0f}k tris "
                "per frame; render thread {:.3f} ms/frame ({:.3f} in n1_record), game thread walk {:.3f} ms; meshes "
                "{} ready {} pending {} missing, {} loading, {} textures ({} unsupported); skinned {} drawn {} culled {} "
                "skipped, {} meshes ready; {} shaders {} pipelines ({} draws waiting), feed {} lights {} envs ({} "
                "instances bound, {} handles), lighting {} updates, {:.3f} ms game / {:.3f} ms feed (totals; {} verified); particles {} emitters "
                "{} sprites {} meshes ({} dropped, {} skipped, {}/{} materials ready/failed), game thread {:.3f} ms; trees {} placements {} drawn "
                "({} assets ready, {} uncaptured, {} failed, {} captured this run)",
                secs, host->frames, host->scenes, st.instances, st.skipped, host->draws / n, host->triangles / n / 1000,
                host->host_ms / n, host->record_ms / n, host->build_ms / n, st.meshes_ready, st.meshes_pending,
                st.meshes_missing, st.loading, st.textures, st.textures_unsupported, st.skel_drawn, st.skel_culled,
                st.skel_skipped, st.skel_meshes_ready, st.shaders, st.pipelines, st.pipelines_pending, st.dyn_lights,
                st.light_envs, st.env_bound, st.env_handles, host->light_updates, host->lighting_ms, host->feed_ms, host->verified, st.particle_emitters,
                st.particle_sprites, st.particle_meshes, st.particle_dropped, st.particle_skipped,
                st.particle_mats_ready, st.particle_mats_failed, host->particles_ms / n, st.tree_placements, st.tree_instances,
                st.tree_assets_ready, st.tree_assets_uncaptured, st.tree_assets_failed, st.tree_captured);
    if (st.decal_builds || st.decals)
      REXLOG_INFO("hybrid n1 decals: {} this frame -> {} pieces drawn, {} cached; {} built since start ({} tris), {:.3f} ms "
                  "this frame", st.decals, st.decal_draws, st.decal_cached, st.decal_builds, st.decal_triangles,
                  st.decal_build_ms);
    REXLOG_INFO("hybrid n1 compile: t={:.1f} s: built {}+{} shaders and {}+{} pipelines (startup+play), {} late "
                "shaders and {} late pipelines, {} failed; {:.1f} s in the driver (worst {:.0f} ms), spirv cache "
                "{}/{}; render thread: {} draws skipped, {} on the stand-in, {} partial frames, backlog {}; n1_record "
                "worst {:.1f} ms, {} frames over 20 ms",
                std::chrono::duration<double>(t2 - host->started).count(), st.shaders_startup, st.shaders_live,
                st.pipelines_startup, st.pipelines_live, st.late_shaders, st.late_pipelines, st.build_failures,
                double(st.pipeline_ms) / 1e3, double(st.pipeline_max_ms), st.spv_hits, st.spv_hits + st.spv_misses,
                st.draws_no_pipe, st.fallback_draws, st.partial_frames, st.compile_backlog, host->record_max_ms,
                host->record_over_20);
    REXLOG_INFO("hybrid n1 sync: {} frames: {} waited for their scene ({:.2f} ms each, {} timed out), {} drawn from an "
                "older scene", host->frames, host->waits, host->waits ? host->wait_ms / double(host->waits) : 0.0,
                host->wait_timeouts, host->drawn_behind);
    host->wait_ms = 0, host->waits = host->wait_timeouts = host->drawn_behind = 0;
    REXLOG_INFO("hybrid n1 loads: t={:.1f} s: lazy {} meshes {} materials {} lights {} skeletal {} particle {} tree; {} "
                "late skips; {} material variants; {} preload requests ({} packages, {} pending, {} scans running); {} applied in {:.1f} ms "
                "(worst {:.1f} ms); {:.1f} MB to the GPU, {} textures, {} meshes; {} copies waiting ({:.1f} MB staging)",
                std::chrono::duration<double>(t2 - host->started).count(), st.lazy_meshes, st.lazy_materials,
                st.lazy_lights, st.lazy_skel, st.lazy_particles, st.lazy_trees, st.late_skips, st.variant_materials, st.preload_requests,
                host->packages, st.preload_pending, st.preload_scanning, st.applied, double(st.apply_ms),
                double(st.apply_max_ms), double(st.upload_bytes) / 1048576.0, st.textures_added, st.meshes_added,
                st.copies_pending, double(st.staging_bytes) / 1048576.0);
    host->record_max_ms = 0, host->record_over_20 = 0;
    host->since = t2, host->frames = host->draws = host->triangles = host->scenes = 0;
    host->host_ms = host->record_ms = host->build_ms = host->feed_ms = host->lighting_ms = host->particles_ms = 0;
    host->light_updates = host->verified = 0;
  }
  return true;
}

// Dual render (BOTS_HYBRID_DUAL=1, the parity harness). The game draws its own scene as usual; the marker comes after
// it, so `target` holds the port's finished scene (post-processed, before the HUD). n1 then draws the same frame (the
// same feed and the marker's camera) into an offscreen image of the target's size and format, and on request both are
// copied out and written as PNG with a JSON sidecar. Both pictures are post-process output in the target's encoding
// (n1 resolves its linear RGBA16F scene through post and sRGB-encodes for a UNORM target, as the port does), so they
// compare directly. Environment:
//   BOTS_HYBRID_DUAL_DIR     where pictures go (default "dual"; must exist)
//   BOTS_HYBRID_DUAL_EVERY   also take a pair every <s> seconds (default 0: only on "hybrid dualshot <tag>")
//   BOTS_HYBRID_DUAL_SETTLE  frames n1 must have nothing loading or waiting (meshes, pipelines, particle materials)
//                            and the port no draw waiting on a pipeline, before a shot (default 30)
//   BOTS_HYBRID_DUAL_WAIT    seconds to wait for that before shooting anyway, marked "settled": false (default 30)
namespace {
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}
// 8-bit RGB as PNG, uncompressed (stored deflate blocks): no image library in the build, and the harness converts or
// recompresses afterwards.
bool WritePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(w * 3 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (n == 0) break;
  }
  const uint32_t adler = (b << 16) | a;
  for (int k = 3; k >= 0; --k) z.push_back(uint8_t(adler >> (8 * k)));
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
    uint8_t head[8] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
    std::memcpy(head + 4, type, 4);
    std::fwrite(head, 1, 8, f);
    if (n) std::fwrite(data, 1, n, f);
    const uint32_t crc = Crc32(data, n, Crc32(head + 4, 4));
    const uint8_t tail[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    std::fwrite(tail, 1, 4, f);
  };
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::fwrite(sig, 1, 8, f);
  const uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                            uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                            8, 2, 0, 0, 0};
  chunk("IHDR", ihdr, sizeof(ihdr));
  chunk("IDAT", z.data(), z.size());
  chunk("IEND", nullptr, 0);
  return std::fclose(f) == 0;
}
float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
  uint32_t bits;
  if (exp == 0) {
    const float v = std::ldexp(float(man), -24);
    return sign ? -v : v;
  }
  bits = exp == 31 ? sign | 0x7F800000u | (man << 13) : sign | ((exp + 112) << 23) | (man << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
// Bytes per texel of the formats a back buffer can have; 0: not supported.
uint32_t DualTexel(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return 8;
    default:
      return 0;
  }
}
void DualToRgb(VkFormat f, const uint8_t* src, size_t texels, std::vector<uint8_t>& rgb) {
  rgb.resize(texels * 3);
  auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  for (size_t i = 0; i < texels; ++i) {
    uint8_t* o = &rgb[i * 3];
    if (f == VK_FORMAT_R16G16B16A16_SFLOAT) {
      uint16_t h[4];
      std::memcpy(h, src + i * 8, 8);
      for (int k = 0; k < 3; ++k) o[k] = byte(HalfToFloat(h[k]));
      continue;
    }
    uint32_t p;
    std::memcpy(&p, src + i * 4, 4);
    if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
      for (int k = 0; k < 3; ++k) o[k] = byte(float((p >> (10 * k)) & 0x3FF) / 1023.0f);
    } else {
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(p >> (8 * k));
      if (f == VK_FORMAT_B8G8R8A8_UNORM) std::swap(o[0], o[2]);
    }
  }
}
std::string Floats(const float* v, size_t n) {
  std::string s = "[";
  for (size_t i = 0; i < n; ++i) s += (i ? "," : "") + std::to_string(v[i]);
  return s + "]";
}
struct DualState {
  void* image = nullptr;  // Renderer::Image, n1's offscreen target
  uint32_t seen = 0;      // dual_shots handled
  bool want = false;
  std::string tag;
  std::chrono::steady_clock::time_point asked, next_auto, start = std::chrono::steady_clock::now();
  uint32_t settled = 0;   // consecutive settled frames
  uint64_t port_pending = 0;
  uint32_t index = 0;
  double every = 0, wait = 30;
  uint32_t settle = 30;
  std::string dir;
  struct Pending {
    VkBuffer buffer[2] = {};
    VmaAllocation alloc[2] = {};
    void* map[2] = {};
    uint64_t serial = 0;
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::string stem, json;
  };
  std::vector<Pending> pending;
};
}  // namespace

bool Renderer::DrawSceneDual(Image& target, const HybridCamera& cam, const VkRect2D& area) {
  HybridState* state = FindHybridState();
  auto* d = static_cast<DualState*>(dual_);
  if (!d) {
    d = new DualState;
    dual_ = d;
    d->dir = EnvOr("BOTS_HYBRID_DUAL_DIR", "dual");
    d->every = std::atof(EnvOr("BOTS_HYBRID_DUAL_EVERY", "0"));
    d->wait = std::atof(EnvOr("BOTS_HYBRID_DUAL_WAIT", "30"));
    d->settle = uint32_t(std::atoi(EnvOr("BOTS_HYBRID_DUAL_SETTLE", "30")));
    d->next_auto = d->start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(d->every));
    REXLOG_INFO("hybrid dual: port and n1 in one frame, pictures to {} (every {} s, settle {} frames, wait {} s)",
                d->dir, d->every, d->settle, d->wait);
  }
  const uint32_t texel = DualTexel(target.format);
  if (!texel) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("hybrid dual: no readback for target format {}", int(target.format));
    }
    return false;
  }
  auto* image = static_cast<Image*>(d->image);
  if (!image || image->width != target.width || image->height != target.height || image->format != target.format) {
    if (image) Retire(*image);  // destroyed once this submission completes
    else image = new Image, d->image = image;
    if (!CreateImage(*image, target.format, target.width, target.height, false, VK_IMAGE_VIEW_TYPE_2D, true)) {
      REXLOG_ERROR("hybrid dual: offscreen target {}x{} format {} failed", target.width, target.height,
                   int(target.format));
      delete image;
      d->image = nullptr;
      return false;
    }
  }
  if (!DrawSceneN1(*image, cam, area)) return false;
  auto* host = static_cast<N1Host*>(n1_host_);
  const N1Stats& st = host->last;

  // Finished readbacks: written once their submission has completed.
  uint64_t completed = stats_.flushes;
  for (const Submission& s : submissions_)
    if (s.pending) completed = std::min(completed, s.serial - 1);
  std::erase_if(d->pending, [&](DualState::Pending& p) {
    if (p.serial > completed) return false;
    std::vector<uint8_t> rgb;
    const char* names[2] = {".port.png", ".n1.png"};
    for (int k = 0; k < 2; ++k) {
      vmaInvalidateAllocation(allocator_, p.alloc[k], 0, VK_WHOLE_SIZE);
      DualToRgb(p.format, static_cast<const uint8_t*>(p.map[k]), size_t(p.w) * p.h, rgb);
      if (!WritePng(p.stem + names[k], rgb, p.w, p.h)) REXLOG_WARN("hybrid dual: can't write {}{}", p.stem, names[k]);
      vmaDestroyBuffer(allocator_, p.buffer[k], p.alloc[k]);
    }
    if (FILE* f = std::fopen((p.stem + ".json").c_str(), "wb")) {
      std::fwrite(p.json.data(), 1, p.json.size(), f);
      std::fclose(f);
    }
    REXLOG_INFO("hybrid dual: wrote {}.{{port,n1}}.png", p.stem);
    return true;
  });

  // Requests: the console's, and the timer's.
  const auto now = std::chrono::steady_clock::now();
  const uint32_t shots = state->dual_shots.load(std::memory_order_acquire);
  if (shots != d->seen) {
    d->seen = shots, d->want = true, d->asked = now, d->tag = state->dual_tag;
  } else if (d->every > 0 && now >= d->next_auto && !d->want) {
    d->next_auto = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(d->every));
    d->want = true, d->asked = now, d->tag = "auto";
  }
  // Settled: n1 has nothing still streaming or compiling, and the port drew everything (no draw skipped for a pipeline
  // still being made since the last frame).
  const bool port_complete = stats_.pending_draws == d->port_pending;
  d->port_pending = stats_.pending_draws;
  const bool quiet = st.meshes_pending == 0 && st.loading == 0 && st.pipelines_pending == 0 &&
                     st.particle_skipped == 0 && port_complete;
  d->settled = quiet ? d->settled + 1 : 0;
  const double waited = std::chrono::duration<double>(now - d->asked).count();
  if (!d->want || (d->settled < d->settle && waited < d->wait)) return true;
  d->want = false;

  DualState::Pending p;
  p.w = area.extent.width, p.h = area.extent.height, p.format = target.format;
  p.serial = stats_.flushes + 1;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = VkDeviceSize(p.w) * p.h * texel;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  for (int k = 0; k < 2; ++k) {
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(allocator_, &info, &alloc, &p.buffer[k], &p.alloc[k], &mapped) != VK_SUCCESS) {
      if (k) vmaDestroyBuffer(allocator_, p.buffer[0], p.alloc[0]);
      REXLOG_ERROR("hybrid dual: readback buffer failed");
      return true;
    }
    p.map[k] = mapped.pMappedData;
  }
  BeginCommands(kGpuOther);
  EndRendering("hybrid dual readback");
  Barrier();  // the port's scene and n1's are both written by now
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {area.offset.x, area.offset.y, 0};
  region.imageExtent = {p.w, p.h, 1};
  vk_.vkCmdCopyImageToBuffer(cmd_, target.image, VK_IMAGE_LAYOUT_GENERAL, p.buffer[0], 1, &region);
  vk_.vkCmdCopyImageToBuffer(cmd_, image->image, VK_IMAGE_LAYOUT_GENERAL, p.buffer[1], 1, &region);
  Barrier();  // before the HUD draws over the target

  char stem[64];
  std::snprintf(stem, sizeof(stem), "%04u-", d->index++);
  p.stem = d->dir + "/" + stem + d->tag;
  const double t = std::chrono::duration<double>(now - d->start).count();
  std::string& j = p.json;
  j = "{\"tag\":\"" + d->tag + "\",\"index\":" + std::to_string(d->index - 1) + ",\"wall\":" + std::to_string(t) +
      ",\"waited\":" + std::to_string(waited) + ",\"settled\":" + (d->settled >= d->settle ? "true" : "false") +
      ",\"seq\":" + std::to_string(cam.seq) + ",\"scene_frame\":" + std::to_string(host->scene.frame) + ",\"scene_cam_seq\":" + std::to_string(host->cam_seq) +
      ",\"world_time\":" + std::to_string(cam.time) + ",\"camera\":{\"view\":" + Floats(cam.view, 16) +
      ",\"proj\":" + Floats(cam.proj, 16) + ",\"location\":" + Floats(cam.location, 3) +
      ",\"rotation\":[" + std::to_string(cam.rotation[0]) + "," + std::to_string(cam.rotation[1]) + "," +
      std::to_string(cam.rotation[2]) + "],\"fov\":" + std::to_string(cam.fov) + ",\"rect\":" + Floats(cam.rect, 4) +
      "},\"target\":{\"width\":" + std::to_string(target.width) + ",\"height\":" + std::to_string(target.height) +
      ",\"format\":" + std::to_string(int(target.format)) + ",\"area\":[" + std::to_string(area.offset.x) + "," +
      std::to_string(area.offset.y) + "," + std::to_string(p.w) + "," + std::to_string(p.h) + "]},\"n1\":{" +
      "\"instances\":" + std::to_string(st.instances) + ",\"draws\":" + std::to_string(st.draws) +
      ",\"triangles\":" + std::to_string(st.triangles) + ",\"meshes_ready\":" + std::to_string(st.meshes_ready) +
      ",\"meshes_pending\":" + std::to_string(st.meshes_pending) +
      ",\"meshes_missing\":" + std::to_string(st.meshes_missing) + ",\"loading\":" + std::to_string(st.loading) +
      ",\"textures_unsupported\":" + std::to_string(st.textures_unsupported) +
      ",\"skel_drawn\":" + std::to_string(st.skel_drawn) + ",\"skel_culled\":" + std::to_string(st.skel_culled) +
      ",\"skel_skipped\":" + std::to_string(st.skel_skipped) +
      ",\"skel_meshes_ready\":" + std::to_string(st.skel_meshes_ready) + ",\"culled\":" + std::to_string(st.culled) +
      ",\"pipelines_pending\":" + std::to_string(st.pipelines_pending) +
      ",\"light_envs\":" + std::to_string(st.light_envs) + ",\"dyn_lights\":" + std::to_string(st.dyn_lights) +
      ",\"particle_emitters\":" + std::to_string(st.particle_emitters) +
      ",\"particle_sprites\":" + std::to_string(st.particle_sprites) +
      ",\"particle_skipped\":" + std::to_string(st.particle_skipped) +
      ",\"scene_instances\":" + std::to_string(host->scene.instances.size()) +
      ",\"scene_skeletal\":" + std::to_string(host->scene.skeletal.size()) +
      ",\"scene_emitters\":" + std::to_string(host->scene.emitters.size()) + "}}\n";
  d->pending.push_back(std::move(p));
  return true;
}
#endif

#ifndef BOTS_N1FFI
void* Renderer::EnsureN1(VkFormat) { return nullptr; }
void Renderer::PumpN1Warmup() {}
void Renderer::PumpN1Uploads() {}
void Renderer::PrepareSceneN1() {}
void Renderer::HoldN1Preload(void*, bool) {}
double Renderer::n1_seconds_since_resume() const { return -1; }
#endif

void Renderer::DestroySceneFill() {
  DestroyBurst();
#ifdef BOTS_N1FFI
  if (auto* d = static_cast<DualState*>(dual_)) {
    for (auto& p : d->pending)  // after Finish: never written
      for (int k = 0; k < 2; ++k) vmaDestroyBuffer(allocator_, p.buffer[k], p.alloc[k]);
    if (auto* image = static_cast<Image*>(d->image)) DestroyImage(*image), delete image;
    delete d;
    dual_ = nullptr;
  }
  if (auto* host = static_cast<N1Host*>(n1_host_)) {
    if (host->r) n1_destroy(host->r);  // after Finish: the GPU is idle
    delete host;
    n1_host_ = nullptr;
  }
#endif
  const VkDevice dev = device_->device();
  for (auto& [k, pipeline] : fill_pipelines_) vk_.vkDestroyPipeline(dev, pipeline, nullptr);
  for (VkShaderModule module : fill_modules_) vk_.vkDestroyShaderModule(dev, module, nullptr);
  if (fill_layout_) vk_.vkDestroyPipelineLayout(dev, fill_layout_, nullptr);
  fill_pipelines_.clear(), fill_modules_.clear(), fill_layout_ = VK_NULL_HANDLE;
}

bool Renderer::DrawSceneFill(Image& target) {
  const uint32_t seq = scene_fill_seq_;
  scene_fill_seq_ = 0;
  HybridState* state = FindHybridState();
  if (!state) return false;
  const uint32_t mode = state->fill.load(std::memory_order_relaxed);
  const HybridCamera& cam = state->cameras[seq % HybridState::kCameras];
  // B-210 diagnostics (BOTS_HYBRID_DROP_LOG=<n>): log the first n markers that draw no scene, and why.
  static int drop_log = [] { const char* v = std::getenv("BOTS_HYBRID_DROP_LOG"); return v ? std::atoi(v) : 0; }();
  auto dropped = [&](const char* why) {
    if (drop_log > 0) {
      --drop_log;
      REXLOG_INFO("hybrid fill: marker {} drew no scene: {} (cam seq {}, rect {},{} {}x{}, target {}x{})", seq, why,
                  cam.seq, cam.rect[0], cam.rect[1], cam.rect[2], cam.rect[3], target.width, target.height);
    }
    return false;
  };
  if (!mode || cam.seq != seq) return dropped("camera overwritten");  // the game ran 64 frames ahead
  // The view rectangle in host pixels; a target smaller than it isn't the back buffer.
  const uint32_t x0 = Host(uint32_t(cam.rect[0])), y0 = Host(uint32_t(cam.rect[1]));
  const uint32_t x1 = Host(uint32_t(cam.rect[0] + cam.rect[2])), y1 = Host(uint32_t(cam.rect[1] + cam.rect[3]));
  if (x1 <= x0 || y1 <= y0 || target.width < x1 || target.height < y1) {
    // B-210: not the back buffer. While a level fades in, the first draws after the skipped scene go to a small target
    // (the loading movie's last frames); dropping the marker there left ~0.5 s of black where the original fades the
    // level in. Keep it for the next draw that fits, unless the next frame's marker replaces it first
    // (BOTS_HYBRID_FILL_KEEP=0: the old drop).
    static const bool keep = [] { const char* v = std::getenv("BOTS_HYBRID_FILL_KEEP"); return !v || std::atoi(v) != 0; }();
    if (keep && x1 > x0 && y1 > y0) scene_fill_seq_ = seq;
    return dropped("target smaller than the view");
  }

  const VkRect2D area{{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}};
  if (mode == 4) {
#ifdef BOTS_N1FFI
    if (state->dual.load(std::memory_order_relaxed)) {
      if (!DrawSceneDual(target, cam, area)) return false;
      state->fills_drawn.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (!DrawSceneN1(target, cam, area)) return dropped("n1 declined");
    state->fills_drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    return false;
#endif
  }
  BeginCommands(kGpuDraw);
  EndRendering("hybrid fill");
  Image* const write_image = &target;
  Access({}, {&write_image, 1});
  if (mode == 1) {
    // A clear inside a one-attachment pass (vkCmdClearAttachments): the simplest stand-in.
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = area;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    BeginRendering(rendering, "hybrid fill (clear)");
    VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    clear.clearValue.color = {{0.10f, 0.35f, 0.55f, 1.0f}};
    const VkClearRect rect{area, 0, 1};
    vk_.vkCmdClearAttachments(cmd_, 1, &clear, 1, &rect);
    vk_.vkCmdEndRendering(cmd_);
    target.Use(x1, y1);
    state->fills_drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  if (!fill_layout_) {
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FillConstants)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (vk_.vkCreatePipelineLayout(device_->device(), &layout_info, nullptr, &fill_layout_) != VK_SUCCESS) return false;
  }
  VkPipeline& pipeline = fill_pipelines_[{mode, target.format}];
  if (!pipeline) {
    const VkShaderModule fs =
        CreateShaderModule(EShLangFragment, "#version 450\n#define MODE " + std::to_string(mode) + "\n" + kFillShader);
    if (!fs) return false;
    fill_modules_.push_back(fs);
    pipeline = FullscreenPipeline(fs, target.format, fill_layout_);
    if (!pipeline) return false;
  }
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = target.view;
  color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = area;
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  BeginRendering(rendering, "hybrid fill");
  if (gpu_timing_) GpuLabel(kGpuDraw, "hybrid fill " + std::to_string(mode));
  vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  bound_pipeline_ = pipeline;
  dynamic_valid_ = false;
  const VkViewport vp{float(x0), float(y0), float(x1 - x0), float(y1 - y0), 0.0f, 1.0f};
  vk_.vkCmdSetViewport(cmd_, 0, 1, &vp);
  vk_.vkCmdSetScissor(cmd_, 0, 1, &area);
  FillConstants constants{};
  std::memcpy(constants.inv_view_proj, cam.inv_view_proj, sizeof(constants.inv_view_proj));
  constants.rect[0] = float(x0), constants.rect[1] = float(y0);
  constants.rect[2] = float(x1 - x0), constants.rect[3] = float(y1 - y0);
  // The grid's plane: BOTS_HYBRID_GROUND_Z (world Z), else 2 m below the first camera drawn for, then fixed in the
  // world so it can be checked against the game's ground as the camera moves.
  static const float ground_z = [&] {
    const char* v = std::getenv("BOTS_HYBRID_GROUND_Z");
    return v && *v ? float(std::atof(v)) : cam.location[2] - 200.0f;
  }();
  constants.origin[0] = cam.location[0], constants.origin[1] = cam.location[1], constants.origin[2] = cam.location[2];
  constants.origin[3] = ground_z;
  vk_.vkCmdPushConstants(cmd_, fill_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
  vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
  vk_.vkCmdEndRendering(cmd_);
  target.Use(x1, y1);
  state->fills_drawn.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace bots::gfx::vk
