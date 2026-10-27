// Hybrid renderer research spike: where a separate renderer for the 3D scene plugs into UE3, and what turning the
// game's own scene rendering off saves. Off unless asked for (BOTS_HYBRID_* below, or the "hybrid" console command).
//
// The insertion point, found by structure (names are compiled out of this build):
//   sub_825D4408  UGameViewportClient::Draw (strings "CanvasObject", "ScreenShot", "MovieFrame"). Builds the view
//                 family on its stack, one FSceneView per local player (the CalcSceneView loop over GamePlayers),
//                 then, unless the viewport client's world rendering is disabled (+0x6C, mask 0x10000000), calls:
//   sub_82BB3230  BeginRenderingViewFamily(FCanvas*, FSceneViewFamily*) (game thread). Flushes the canvas, builds
//                 an FSceneRenderer (0x110 bytes, ctor sub_82BB2A70) and enqueues FDrawSceneCommand (vtable
//                 0x82112D18, DescribeCommand sub_82BA3510 returns L"FDrawSceneCommand"), or with no scene
//                 FDrawSceneCommandPP (0x82112D24, post-process only); without a rendering thread it runs inline.
//   sub_82BB3128  RenderViewFamily_RenderThread(FSceneRenderer*) (render thread, FDrawSceneCommand::Execute
//                 sub_82BB3208): picks hit-proxy / special paths by show flags, else calls
//   sub_82BB1E78  FSceneRenderer::Render(): the whole 3D frame (visibility, occlusion queries, depth, base pass,
//                 lights, shadows, translucency, post process) up to the final pass that writes the back buffer.
//                 Then the renderer is destroyed (sub_82BB2F70) and freed.
// Everything the viewport client draws after BeginRenderingViewFamily (HUD, UI scenes, subtitles) goes through the
// canvas and separate render commands, so skipping Render() leaves them drawing as before.
//
// FSceneViewFamily (this build): +0x00 Views (TArray<const FSceneView*>: data, count, max), +0x0C RenderTarget,
// +0x10 Scene, +0x14 ShowFlags (64-bit), +0x1C CurrentWorldTime, +0x20 DeltaWorldTime (floats). FSceneRenderer keeps a
// copy of the family at +0x04 (its +0x18 show-flag test and sub_82BB1958's +0x20 time load are family +0x14 / +0x1C).
// FSceneView: see kViewMatrix below.
//
// Environment (all optional):
//   BOTS_HYBRID_SUPPRESS=1   don't render the 3D scene (FSceneRenderer::Render skipped); HUD, menus, movies stay
//                        =2  the same, but keep Render's visibility pass (LastRenderTime stays current)
//   BOTS_HYBRID_AB_MODE=1|2  the suppression BOTS_HYBRID_AB toggles to (default 1)
//   BOTS_HYBRID_FILL=0..3    at the scene's place in the GPU stream the renderer draws: nothing / a test colour / a
//                            world-space test grid from the frame's camera / (scene not suppressed) the grid's lines
//                            over the game's own scene (needs the native renderer; src/gfx/vk/hybrid_fill.cpp)
//   BOTS_HYBRID_AB=<s>       toggle suppression every <s> seconds (interleaved A/B in one run)
//   BOTS_HYBRID_CAMERA=<n>   log the camera (N2's JSON shape) every <n> frames
//   BOTS_HYBRID_DUAL=1       parity harness: the game draws its scene as usual (no suppression, no A/B) and n1 (fill
//                            4) draws the same frame offscreen; pictures of both on request (hybrid_fill.cpp)
// Console (BOTS_EXEC): "hybrid suppress 0|1|2", "hybrid fill 0..3", "hybrid camera", "hybrid dumpview",
// "hybrid dualshot <tag>" (one port + n1 pair under BOTS_HYBRID_DUAL).
//
// Mode 2 also hooks sub_82D3E358, UE3's GetQueryResult(query, out, bWait): with the drawing skipped no occlusion
// query is ever issued, so InitViews' waits would never return. It answers "visible" there and for two real frames
// after mode 2 ends.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

#include <time.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "hybrid_state.h"
#include "scene_feed.h"

extern "C" void __imp__sub_82BB3230(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82BB3128(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82BB1E78(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82BAD738(PPCContext& __restrict ctx, uint8_t* base);  // FSceneRenderer::InitViews
extern "C" void sub_82BB1958(PPCContext& __restrict ctx, uint8_t* base);  // per visible primitive: LastRenderTime
extern "C" void sub_8231FA50(PPCContext& __restrict ctx, uint8_t* base);  // D3D: make ring space, returns ring ptr
extern "C" void __imp__sub_82D3E358(PPCContext& __restrict ctx, uint8_t* base);  // GetQueryResult(q, out, bWait)

namespace bots::hybrid {
namespace {

// The one HybridState, published for the renderer plugin before main (hybrid_state.h).
HybridState g_state;
const bool g_state_published = [] {
  char value[32];
  std::snprintf(value, sizeof(value), "%llx", static_cast<unsigned long long>(uintptr_t(&g_state)));
#ifdef _WIN32
  SetEnvironmentVariableA(kHybridStateEnv, value);
#else
  setenv(kHybridStateEnv, value, 1);
#endif
  return true;
}();

constexpr uint32_t kDeviceGlobal = 0x838191B0;  // the XDK D3D device pointer UE3's RHI loads

uint8_t* Raw(uint8_t* base, uint32_t address) {
  return base + (uint64_t(address) + (address >= 0xE0000000u ? 0x1000u : 0u));
}
uint32_t U32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Raw(base, address), 4);
  return __builtin_bswap32(v);
}
float F32(uint8_t* base, uint32_t address) {
  const uint32_t v = U32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void Store(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Raw(base, address), &value, 4);
}
bool Readable(uint32_t a) { return a >= 0x40000000u && a < 0xFFFF0000u && !(a & 3); }

int EnvInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::atoi(v) : fallback;
}

double ThreadCpuMs() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
  auto t = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
  return t(k) + t(u);
#else
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) / 1e6;
#endif
}

std::atomic<int> g_suppress{0};  // 0 render, 1 skip Render(), 2 skip it but keep its visibility pass
int g_suppress_mode = 1;         // what A/B toggles to
std::atomic<bool> g_dump_view{false}, g_log_camera{false};
int g_camera_every = 0;
int g_ab_seconds = 0;

// FSceneView (this build), from a dump of a live view (BOTS_HYBRID_CAMERA, "hybrid dumpview") cross-checked against
// UE3's field order: +0x00 Family, +0x04 State, +0x24 X, +0x28 Y, +0x2C SizeX, +0x30 SizeY (floats),
// +0x34..+0x40 RenderTargetX/Y/SizeX/SizeY (ints), then FMatrix ViewMatrix +0x50, ProjectionMatrix +0x90,
// ViewProjectionMatrix +0x110, InvProjectionMatrix +0x150, InvViewProjectionMatrix +0x190, FVector4 ViewOrigin +0x1D0.
constexpr uint32_t kFamilyTime = 0x1C;
constexpr uint32_t kViewRect = 0x24, kViewMatrix = 0x50, kProjMatrix = 0x90, kViewProjMatrix = 0x110,
                   kInvViewProjMatrix = 0x190, kViewOrigin = 0x1D0;

void ReadMatrix(uint8_t* base, uint32_t at, float* m) {
  for (int i = 0; i < 16; ++i) m[i] = F32(base, at + 4 * i);
}

// A UE3 perspective projection (row vectors, z' = w): rejects a view whose layout isn't the one above.
bool LooksLikeProjection(const float* m) {
  return m[0] > 0.1f && m[0] < 20 && m[5] > 0.1f && m[5] < 40 && std::fabs(m[11] - 1) < 1e-4f &&
         std::fabs(m[15]) < 1e-4f && std::fabs(m[1]) < 1e-4f && std::fabs(m[4]) < 1e-4f;
}

void DumpView(uint8_t* base, uint32_t family, uint32_t view) {
  REXLOG_INFO("hybrid: view family {:08X}:", family);
  for (uint32_t o = 0; o < 0x60; o += 16)
    REXLOG_INFO("hybrid:   fam +{:03X}: {:08X} {:08X} {:08X} {:08X}", o, U32(base, family + o),
                U32(base, family + o + 4), U32(base, family + o + 8), U32(base, family + o + 12));
  REXLOG_INFO("hybrid: view {:08X}:", view);
  for (uint32_t o = 0; o < 0x400; o += 16)
    REXLOG_INFO("hybrid:   view +{:03X}: {:08X} {:08X} {:08X} {:08X} | {:.5g} {:.5g} {:.5g} {:.5g}", o,
                U32(base, view + o), U32(base, view + o + 4), U32(base, view + o + 8), U32(base, view + o + 12),
                F32(base, view + o), F32(base, view + o + 4), F32(base, view + o + 8), F32(base, view + o + 12));
}

void Mul(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}

bool Invert(const float* m, float* out) {
  double inv[16], a[16];
  for (int i = 0; i < 16; ++i) a[i] = m[i];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
           a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
           a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
           a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
            a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
           a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
           a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
           a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
            a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
           a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
           a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
            a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
            a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
           a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
           a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
            a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
            a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
  const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (std::fabs(det) < 1e-30) return false;
  for (int i = 0; i < 16; ++i) out[i] = float(inv[i] / det);
  return true;
}

// The camera of a view, from its matrices (so it is exactly what the scene renderer would have used).
bool ReadCamera(uint8_t* base, uint32_t family, uint32_t view, HybridCamera& cam) {
  ReadMatrix(base, view + kViewMatrix, cam.view);
  ReadMatrix(base, view + kProjMatrix, cam.proj);
  if (!LooksLikeProjection(cam.proj)) return false;
  // The game's own products, so the host renderer uses exactly the scene renderer's matrices.
  ReadMatrix(base, view + kViewProjMatrix, cam.view_proj);
  ReadMatrix(base, view + kInvViewProjMatrix, cam.inv_view_proj);
  for (int i = 0; i < 4; ++i) cam.rect[i] = F32(base, view + kViewRect + 4 * i);
  static bool checked = false;  // once: our product and inverse agree with the stored ones
  if (!checked) {
    checked = true;
    float vp[16], inv[16];
    Mul(cam.view, cam.proj, vp);
    Invert(vp, inv);
    double dvp = 0, dinv = 0;
    for (int i = 0; i < 16; ++i) {
      dvp = std::max(dvp, double(std::fabs(vp[i] - cam.view_proj[i]) / (1 + std::fabs(cam.view_proj[i]))));
      dinv = std::max(dinv, double(std::fabs(inv[i] - cam.inv_view_proj[i]) / (1 + std::fabs(cam.inv_view_proj[i]))));
    }
    REXLOG_INFO("hybrid: FSceneView check: view*proj vs stored ViewProjection rel. err {:.2g}, inverse {:.2g}; "
                "ViewOrigin ({:.1f}, {:.1f}, {:.1f})", dvp, dinv, F32(base, view + kViewOrigin),
                F32(base, view + kViewOrigin + 4), F32(base, view + kViewOrigin + 8));
  }
  // View = Translation(-L) * R: row 3 = -L * R, so L = -row3 * R^T (R orthonormal).
  const float* v = cam.view;
  for (int i = 0; i < 3; ++i) cam.location[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
  // R = InverseRotation(Rotation) * axis swap: its columns 0, 1, 2 are the camera's right, up and forward (world).
  const double fx = v[2], fy = v[6], fz = v[10], rz = v[8], uz = v[9];
  constexpr double k = 32768.0 / 3.14159265358979323846;
  const double pitch = std::atan2(fz, std::hypot(fx, fy)), yaw = std::atan2(fy, fx), roll = std::atan2(-rz, uz);
  cam.rotation[0] = int32_t(std::lround(pitch * k)) & 0xFFFF;
  cam.rotation[1] = int32_t(std::lround(yaw * k)) & 0xFFFF;
  cam.rotation[2] = int32_t(std::lround(roll * k)) & 0xFFFF;
  cam.fov = float(2 * std::atan(1.0 / cam.proj[0]) * 180 / 3.14159265358979323846);
  return true;
}

std::string CameraJson(const HybridCamera& c) {
  std::string s = "{\"seq\":" + std::to_string(c.seq);
  auto arr = [&](const char* name, const float* f, int n) {
    s += ",\"";
    s += name;
    s += "\":[";
    char b[32];
    for (int i = 0; i < n; ++i) std::snprintf(b, sizeof(b), "%s%.7g", i ? "," : "", f[i]), s += b;
    s += ']';
  };
  arr("view", c.view, 16);
  arr("proj", c.proj, 16);
  char b[96];
  std::snprintf(b, sizeof(b), ",\"fov\":%.5g", c.fov);
  s += b;
  arr("location", c.location, 3);
  std::snprintf(b, sizeof(b), ",\"rotation\":[%d,%d,%d]", c.rotation[0], c.rotation[1], c.rotation[2]);
  s += b;
  arr("rect", c.rect, 4);
  std::snprintf(b, sizeof(b), ",\"time\":%.4f", c.time);
  s += b;
  return s + "}";
}

// Render-command order is game-thread order: the render thread pops the camera the game thread pushed for the same
// view family.
std::mutex g_fifo_mutex;
std::deque<uint32_t> g_fifo;  // seqs of view families with a scene, not yet rendered
thread_local uint32_t t_render_seq = 0;
thread_local bool t_suppressed_visibility = false;  // answer GetQueryResult at once (mode 2 and just after)
std::atomic<uint64_t> g_queries_answered{0};

struct Stats {
  uint64_t families = 0, scene_families = 0, renders = 0, suppressed = 0, markers = 0, by_site[4] = {};
  double render_ms = 0;               // inside FSceneRenderer::Render (wall)
  double visibility_ms = 0;           // suppress 2: InitViews + LastRenderTime pass (wall)
  double rt_cpu_ms = 0, gt_cpu_ms = 0;  // thread CPU between consecutive frames on the render / game thread
  uint64_t rt_frames = 0, gt_frames = 0;
};
std::mutex g_stats_mutex;
Stats g_stats;
double g_rt_last = -1, g_gt_last = -1;
std::chrono::steady_clock::time_point g_report{}, g_ab_next{};

void Report(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(g_stats_mutex);
  if (g_report == std::chrono::steady_clock::time_point{}) g_report = now;
  const double secs = std::chrono::duration<double>(now - g_report).count();
  if (secs < 10) return;
  const Stats& s = g_stats;
  const HybridState& st = g_state;
  REXLOG_INFO(
      "hybrid: {:.1f} s, suppress {}, fill {}: {:.1f} fps (view families), render thread CPU {:.2f} ms/frame, "
      "FSceneRenderer::Render {:.2f} ms/frame ({} run, {} skipped, visibility {:.2f} ms/frame, {} queries answered), game thread CPU {:.2f} ms/frame; sites "
      "{}/{}/{}/{}, markers {} written {} seen {} filled",
      secs, g_suppress.load(), st.fill.load(), double(s.families) / secs,
      s.rt_frames ? s.rt_cpu_ms / double(s.rt_frames) : 0.0, s.renders ? s.render_ms / double(s.renders) : 0.0,
      s.renders, s.suppressed, s.suppressed ? s.visibility_ms / double(s.suppressed) : 0.0,
      g_queries_answered.exchange(0), s.gt_frames ? s.gt_cpu_ms / double(s.gt_frames) : 0.0, s.by_site[0], s.by_site[1],
      s.by_site[2], s.by_site[3], s.markers, st.markers_seen.load(), st.fills_drawn.load());
  g_stats = {};
  g_report = now;
}

// A type-0 register write of `value` to the marker register, appended to the D3D device's ring (as the runtime's own
// inline packet writers do: +0x30 is the last written dword, +0x38 the limit; sub_8231FA50 makes room).
void EmitMarker(PPCContext& ctx, uint8_t* base, uint32_t value) {
  const uint32_t dev = U32(base, kDeviceGlobal);
  if (!Readable(dev)) return;
  uint32_t ring = U32(base, dev + 0x30);
  if (ring > U32(base, dev + 0x38)) {
    PPCContext saved = ctx;
    ctx.r3.u64 = dev;
    sub_8231FA50(ctx, base);
    ring = ctx.r3.u32;
    ctx = saved;
  }
  Store(base, ring + 4, kHybridMarkerRegister);  // type 0, one register
  Store(base, ring + 8, value);
  Store(base, dev + 0x30, ring + 8);
}

}  // namespace

bool Command(const std::string& cmd) {
  auto arg = [&](const char* prefix) -> int {
    return std::atoi(cmd.c_str() + std::strlen(prefix));
  };
  if (cmd.rfind("hybrid suppress ", 0) == 0) {
    g_suppress = arg("hybrid suppress ");
  } else if (cmd.rfind("hybrid fill ", 0) == 0) {
    g_state.fill = uint32_t(arg("hybrid fill "));
  } else if (cmd == "hybrid camera") {
    g_log_camera = true;
  } else if (cmd == "hybrid dumpview") {
    g_dump_view = true;
  } else if (cmd.rfind("hybrid dualshot", 0) == 0) {
    // The tag names the files; keep it to [A-Za-z0-9_-].
    std::string tag = cmd.size() > 16 ? cmd.substr(16) : "shot";
    for (char& c : tag)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    std::snprintf(g_state.dual_tag, sizeof(g_state.dual_tag), "%s", tag.c_str());
    g_state.dual_shots.fetch_add(1, std::memory_order_release);
  } else {
    return false;
  }
  REXLOG_INFO("hybrid: {} -> suppress {}, fill {}", cmd, g_suppress.load(), g_state.fill.load());
  return true;
}

}  // namespace bots::hybrid

using namespace bots::hybrid;
using bots::HybridCamera;
using bots::HybridState;

// BeginRenderingViewFamily(FCanvas* r3, FSceneViewFamily* r4), game thread.
namespace bots::hybrid {
void InstallOpenHooks();  // B-199, end of file
}
extern "C" void sub_82BB3230(PPCContext& __restrict ctx, uint8_t* base) {
  bots::hybrid::InstallOpenHooks();
  static const bool init = [] {
    g_suppress = EnvInt("BOTS_HYBRID_SUPPRESS", 0);
    if (g_suppress) g_suppress_mode = g_suppress;
    if (const int m = EnvInt("BOTS_HYBRID_AB_MODE", 0)) g_suppress_mode = m;
    g_state.fill = uint32_t(EnvInt("BOTS_HYBRID_FILL", 0));
    g_camera_every = EnvInt("BOTS_HYBRID_CAMERA", 0);
    g_ab_seconds = EnvInt("BOTS_HYBRID_AB", 0);
    if (EnvInt("BOTS_HYBRID_DUAL", 0)) {
      // Both renderers in one frame: the game's own scene stays, so nothing is suppressed or toggled.
      g_state.dual = 1;
      g_state.fill = 4;
      g_suppress = 0;
      g_ab_seconds = 0;
    }
    REXLOG_INFO("hybrid: BeginRenderingViewFamily hooked: suppress {}, fill {}, camera log every {} frames, A/B {} s",
                g_suppress.load(), g_state.fill.load(), g_camera_every, g_ab_seconds);
    return true;
  }();
  (void)init;
  const auto now = std::chrono::steady_clock::now();
  if (g_ab_seconds > 0) {
    if (g_ab_next == std::chrono::steady_clock::time_point{}) g_ab_next = now + std::chrono::seconds(g_ab_seconds);
    if (now >= g_ab_next) {
      g_ab_next = now + std::chrono::seconds(g_ab_seconds);
      g_suppress = g_suppress.load() ? 0 : g_suppress_mode;
      REXLOG_INFO("hybrid: A/B -> suppress {}", g_suppress.load());
      std::lock_guard lock(g_stats_mutex);
      g_stats = {};  // each report covers one setting
      g_report = now;
    }
  }
  Report(now);

  const uint32_t family = ctx.r4.u32;
  const uint32_t views = U32(base, family), count = U32(base, family + 4), scene = U32(base, family + 0x10);
  const uint32_t site = ctx.lr == 0x825D51F8 ? 0 : ctx.lr == 0x825D5990 ? 1 : ctx.lr == 0x825D5D30 ? 2 : 3;
  {
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.families, ++g_stats.by_site[site];
    if (scene) ++g_stats.scene_families;
    const double cpu = ThreadCpuMs();
    if (g_gt_last >= 0) g_stats.gt_cpu_ms += cpu - g_gt_last, ++g_stats.gt_frames;
    g_gt_last = cpu;
  }
  static uint32_t logged_sites = 0;
  if (!(logged_sites & (1u << site))) {
    logged_sites |= 1u << site;
    REXLOG_INFO("hybrid: view family from site {} (lr {:08X}): {} views, scene {:08X}, show flags {:08X}{:08X}", site,
                uint32_t(ctx.lr), count, scene, U32(base, family + 0x14), U32(base, family + 0x18));
  }
  if (count && Readable(views)) {
    const uint32_t view = U32(base, views);
    if (Readable(view)) {
      static bool dumped = false;
      if (!dumped && scene) dumped = true, DumpView(base, family, view);
      if (g_dump_view.exchange(false)) DumpView(base, family, view);
      if (scene) {
        const uint32_t seq = g_state.published.load(std::memory_order_relaxed) + 1;
        HybridCamera& cam = g_state.cameras[seq % HybridState::kCameras];
        if (ReadCamera(base, family, view, cam)) {
          cam.seq = seq;
          cam.time = F32(base, family + kFamilyTime);
          g_state.published.store(seq, std::memory_order_release);
          if (g_log_camera.exchange(false) || (g_camera_every > 0 && seq % uint32_t(g_camera_every) == 0))
            REXLOG_INFO("hybrid: camera {}", CameraJson(cam));
          {
            std::lock_guard lock(g_fifo_mutex);
            g_fifo.push_back(seq);
            if (g_fifo.size() > 16) g_fifo.pop_front();
          }
          // The frame's scene, published before the render command that draws it exists: the render thread can no
          // longer reach this camera's marker first and draw the previous frame's scene (doubling, stutter).
          if (bots::scenefeed::LiveAtDraw()) bots::scenefeed::Live(base);
        }
      }
    }
  }
  __imp__sub_82BB3230(ctx, base);
}

// RenderViewFamily_RenderThread(FSceneRenderer* r3), render thread (or the game thread without one).
extern "C" void sub_82BB3128(PPCContext& __restrict ctx, uint8_t* base) {
  {
    std::lock_guard lock(g_fifo_mutex);
    t_render_seq = g_fifo.empty() ? 0 : g_fifo.front();
    if (!g_fifo.empty()) g_fifo.pop_front();
  }
  {
    std::lock_guard lock(g_stats_mutex);
    const double cpu = ThreadCpuMs();
    if (g_rt_last >= 0) g_stats.rt_cpu_ms += cpu - g_rt_last, ++g_stats.rt_frames;
    g_rt_last = cpu;
  }
  __imp__sub_82BB3128(ctx, base);
}

// UE3 GetQueryResult(q, out, bWait): InitViews reads the occlusion queries the previous frame's drawing issued. With the
// drawing suppressed nothing issues them, so bWait = 1 waits forever (mode 2 hung on the first level load). Inside a
// suppressed InitViews, answer "visible" at once; the host renderer will own occlusion.
extern "C" void sub_82D3E358(PPCContext& __restrict ctx, uint8_t* base) {
  if (t_suppressed_visibility) {
    if (Readable(ctx.r4.u32)) Store(base, ctx.r4.u32, 4096);
    g_queries_answered.fetch_add(1, std::memory_order_relaxed);
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82D3E358(ctx, base);
}

// FSceneRenderer::Render(), render thread.
extern "C" void sub_82BB1E78(PPCContext& __restrict ctx, uint8_t* base) {
  // Leaving mode 2: its InitViews calls took queries for primitives that no drawing then issued, so the first real
  // frames' InitViews would wait on them forever too. Answer queries for two real frames, until fresh ones exist.
  static int last_suppress = 0, answer_frames = 0;
  int suppress_now = g_suppress.load(std::memory_order_relaxed);
  // B-187: the SpeedTree capture needs the game's own draws; let some real frames through in mode 2.
  bool capturing = false;
  if (suppress_now == 2 && g_state.capture_frames.load(std::memory_order_relaxed) > 0) {
    g_state.capture_frames.fetch_sub(1, std::memory_order_relaxed);
    suppress_now = 0;
    capturing = true;
  }
  if (last_suppress == 2 && suppress_now != 2) answer_frames = 2;
  last_suppress = suppress_now;
  if (const int suppress = suppress_now) {
    // Mode 2 keeps the visibility work Render() does around its drawing: InitViews (sub_82BAD738, Render's first call
    // after the render-target allocation: per view frustum, distance and occlusion-history culling into the views'
    // visibility maps) and, at its end, sub_82BB1958, which for each visible primitive sets
    // PrimitiveSceneInfo +0xA4, Component->LastRenderTime (+0x1B0) and Owner->LastRenderTime (+0xEC) to the family's
    // CurrentWorldTime. Gameplay reads those (skeletal updates when not rendered, particle and LOD timers, AI
    // "recently rendered"), so mode 1 freezes them; mode 2 measures what keeping them costs.
    if (suppress == 2) {
      const auto start = std::chrono::steady_clock::now();
      const uint32_t renderer = ctx.r3.u32;
      PPCContext saved = ctx;
      t_suppressed_visibility = true;
      sub_82BAD738(ctx, base);
      t_suppressed_visibility = false;
      ctx = saved;
      ctx.r3.u64 = renderer;
      sub_82BB1958(ctx, base);
      ctx = saved;
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      std::lock_guard lock(g_stats_mutex);
      g_stats.visibility_ms += ms;
    }
    bool marker = g_state.fill.load(std::memory_order_relaxed) != 0 && t_render_seq != 0;
    if (marker) EmitMarker(ctx, base, t_render_seq);
    std::lock_guard lock(g_stats_mutex);
    ++g_stats.suppressed;
    if (marker) ++g_stats.markers;
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  t_suppressed_visibility = answer_frames > 0;
  __imp__sub_82BB1E78(ctx, base);
  t_suppressed_visibility = false;
  if (answer_frames > 0) --answer_frames;
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  // Fill 3 overlays the camera's grid on the game's own scene, and dual render (or the tree capture) draws n1's copy
  // of it: the marker goes after the scene, before the HUD.
  const uint32_t fill_now = g_state.fill.load(std::memory_order_relaxed);
  const bool marker =
      (fill_now == 3 || (fill_now == 4 && (capturing || g_state.dual.load(std::memory_order_relaxed)))) &&
      t_render_seq != 0;
  if (marker) EmitMarker(ctx, base, t_render_seq);
  std::lock_guard lock(g_stats_mutex);
  g_stats.render_ms += ms, ++g_stats.renders;
  if (marker) ++g_stats.markers;
}

// B-199 level preload. UE3 opens a cooked package's .xxx when it starts loading it (synchronously for a map,
// asynchronously for a streamed sublevel), so the open comes before the level is loaded, let alone visible. The opens
// reach the kernel through UE3's I/O function table (indirect calls to the NtCreateFile / NtOpenFile import thunks; the
// XAPI CreateFile wrappers sub_8332D5E0 and friends never see them), so the thunks' dispatch-table slots are pointed at
// a wrapper that notes the path and calls the import. The package's stem is queued for the renderer; with fill 4 it has n1
// load what it holds (hybrid_fill.cpp). BOTS_N1_PRELOAD=0 turns the queueing off; opens are logged either way.
namespace bots::hybrid {
namespace {
constexpr uint32_t kNtOpenFileThunk = 0x83740094, kNtCreateFileThunk = 0x837401D4;
PPCFunc* g_nt_open = nullptr;
PPCFunc* g_nt_create = nullptr;

// r5 (both calls): OBJECT_ATTRIBUTES { RootDirectory, ANSI_STRING* ObjectName, Attributes }.
void NotePackageOpen(PPCContext& ctx, uint8_t* base) {
  const uint32_t attrs = ctx.r5.u32;
  if (!Readable(attrs)) return;
  const uint32_t name = U32(base, attrs + 4);
  if (!Readable(name)) return;
  uint16_t len;
  std::memcpy(&len, Raw(base, name), 2);
  len = __builtin_bswap16(len);
  const uint32_t buf = U32(base, name + 4);
  if (len <= 4 || len >= 260 || !buf || !Readable(buf & ~3u)) return;
  std::string_view path(reinterpret_cast<const char*>(Raw(base, buf)), len);
  auto lower = [](char c) { return char(std::tolower(uint8_t(c))); };
  if (path[len - 4] != '.' || lower(path[len - 3]) != 'x' || lower(path[len - 2]) != 'x' || lower(path[len - 1]) != 'x')
    return;
  const size_t slash = path.find_last_of("\\/");
  std::string stem(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
  stem.resize(stem.size() - 4);
  static const bool preload = EnvInt("BOTS_N1_PRELOAD", 1) != 0;
  const uint32_t k = g_state.opened.fetch_add(1, std::memory_order_relaxed);
  // Queued whatever the fill mode (it is set later, and can change); only an n1 renderer takes them.
  bool queued = false;
  if (preload) {
    std::lock_guard lock(g_state.package_mutex);
    if ((queued = g_state.packages.size() < 4096)) g_state.packages.push_back(stem);
  }
  REXLOG_INFO("preload: package open #{} {}{}", k, stem, queued ? "" : " (not queued)");
}
void NtOpenFileHook(PPCContext& __restrict ctx, uint8_t* base) {
  NotePackageOpen(ctx, base);
  g_nt_open(ctx, base);
}
void NtCreateFileHook(PPCContext& __restrict ctx, uint8_t* base) {
  NotePackageOpen(ctx, base);
  g_nt_create(ctx, base);
}
}  // namespace

// From OnPostLoadXexImage (before the guest runs), and again from the first game-thread hook in case the dispatch
// table wasn't ready then. Installs once.
void InstallOpenHooks() {
  static std::atomic<bool> installed{false};
  static std::mutex mutex;
  if (installed.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  if (installed.load(std::memory_order_relaxed)) return;
  auto* memory = rex::system::kernel_memory();
  if (!memory || !memory->HasAnyFunctionTable()) return;
  g_nt_open = rex::runtime::ResolveIndirectFunction(kNtOpenFileThunk);
  g_nt_create = rex::runtime::ResolveIndirectFunction(kNtCreateFileThunk);
  const bool ok = g_nt_open && g_nt_create && memory->SetFunction(kNtOpenFileThunk, NtOpenFileHook) &&
                  memory->SetFunction(kNtCreateFileThunk, NtCreateFileHook);
  installed.store(true, std::memory_order_release);
  REXLOG_INFO("preload: package-open hooks {}", ok ? "installed" : "NOT installed");
}
}  // namespace bots::hybrid
