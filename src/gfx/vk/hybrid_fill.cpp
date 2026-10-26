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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include <glslang/Public/ShaderLang.h>

#include "../../hybrid_state.h"
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

void Renderer::QueueSceneFill(uint32_t seq) { scene_fill_seq_ = seq; }

#ifdef BOTS_N1FFI
namespace {
struct N1Host {
  N1Renderer* r = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool failed = false;
  uint32_t budget = 8;
  HybridScene scene;  // the newest one taken from the game
  std::unordered_map<const char*, uint32_t> meshes, materials, lights, skel_meshes, envs, pmaterials, pmeshes;
  std::vector<N1Emitter> emitters;
  std::vector<N1Instance> instances;
  std::vector<N1SkelInstance> skeletal;
  std::vector<uint32_t> skel_materials;
  std::vector<uint32_t> material_handles;
  // Stats since the last log line.
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
  double log_seconds = 5, host_ms = 0, record_ms = 0, build_ms = 0, feed_ms = 0, lighting_ms = 0, particles_ms = 0;
  uint64_t frames = 0, draws = 0, triangles = 0, scenes = 0;
  N1Stats last{};
};
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

bool Renderer::DrawSceneN1(Image& target, const HybridCamera& cam, const VkRect2D& area) {
  HybridState* state = FindHybridState();
  auto* host = static_cast<N1Host*>(n1_host_);
  if (!host) {
    host = new N1Host;
    n1_host_ = host;
    host->budget = uint32_t(std::atoi(EnvOr("BOTS_N1_BUDGET", "8")));
    host->log_seconds = std::atof(EnvOr("BOTS_N1_LOG", "5"));
    N1Init init{};
    init.get_instance_proc_addr = device_->vulkan_instance()->functions().vkGetInstanceProcAddr;
    init.instance = device_->vulkan_instance()->instance();
    init.physical_device = device_->physical_device();
    init.device = device_->device();
    init.color_format = target.format;
    init.cooked_dir = EnvOr("BOTS_N1_COOKED", "game/C1Game/CookedXenon");
    init.cache_dir = EnvOr("BOTS_N1_CACHE", "cache/n1");
    init.loader_threads = uint32_t(std::atoi(EnvOr("BOTS_N1_THREADS", "2")));
    const auto t0 = std::chrono::steady_clock::now();
    host->r = n1_create(&init);
    host->format = target.format;
    if (!host->r) {
      host->failed = true;
      REXLOG_ERROR("hybrid n1: n1_create failed: {}", n1_last_error());
    } else {
      REXLOG_INFO("hybrid n1: renderer up in {:.1f} ms (target format {}, {}x{})",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                  int(target.format), target.width, target.height);
    }
  }
  if (host->failed) return false;
  if (target.format != host->format) {
    REXLOG_WARN("hybrid n1: target format {} is not the renderer's {}; skipped", int(target.format), int(host->format));
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(state->scene_mutex);
    if (state->scene.frame > host->scene.frame) std::swap(host->scene, state->scene), ++host->scenes;
  }
  // Lights, fog and post: handed over when the game built a new set (every BOTS_N1_FEED_EVERY game frames).
  if (!host->scene.lighting.empty()) {
    const auto f0 = std::chrono::steady_clock::now();
    if (n1_set_feed(host->r, host->scene.lighting.data(), uint32_t(host->scene.lighting.size())) != 0)
      REXLOG_WARN("hybrid n1: n1_set_feed failed: {}", n1_last_error());
    host->feed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
    host->lighting_ms += host->scene.lighting_ms;
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
    out.sub_method = in.sub_method, out.local_space = in.local_space;
    out.material = Handle(host->pmaterials, in.material, host->r, n1_pmaterial);
    out.mesh = Handle(host->pmeshes, in.mesh, host->r, n1_pmesh);
    out.count = in.count, out.row_offset = in.row_offset, out.row_floats = in.row_floats;
    std::memcpy(out.origin, in.origin, sizeof(out.origin));
    std::memcpy(out.transform, in.transform, sizeof(out.transform));
  }
  n1_set_particles(host->r, host->emitters.data(), uint32_t(host->emitters.size()), host->scene.particle_rows.data(),
                   uint32_t(host->scene.particle_rows.size()));
  n1_set_skeletal(host->r, host->skeletal.data(), uint32_t(host->skeletal.size()), host->scene.bones.data(),
                  uint32_t(host->scene.bones.size()), host->skel_materials.data(),
                  uint32_t(host->skel_materials.size()));
  N1Camera camera{};
  std::memcpy(camera.view, cam.view, sizeof(camera.view));
  std::memcpy(camera.proj, cam.proj, sizeof(camera.proj));
  std::memcpy(camera.location, cam.location, sizeof(camera.location));
  // UE3's infinite perspective: proj[14] = -near * proj[10].
  camera.near_plane = cam.proj[10] != 0 ? -cam.proj[14] / cam.proj[10] : 5.0f;
  N1Target t{};
  t.view = target.view;
  t.layout = VK_IMAGE_LAYOUT_GENERAL;
  t.width = target.width, t.height = target.height;
  t.x = area.offset.x, t.y = area.offset.y, t.w = area.extent.width, t.h = area.extent.height;
  t.clear = 1;  // the host covers the whole view every frame
  t.clear_color[0] = 0.32f, t.clear_color[1] = 0.40f, t.clear_color[2] = 0.50f, t.clear_color[3] = 1.0f;
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
    return false;
  }
  target.Use(uint32_t(t.x) + t.w, uint32_t(t.y) + t.h);
  host->frames++, host->draws += st.draws, host->triangles += st.triangles, host->last = st;
  host->host_ms += std::chrono::duration<double, std::milli>(t2 - t0).count();
  host->record_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
  host->build_ms += host->scene.build_ms;
  host->particles_ms += host->scene.particles_ms;
  const double secs = std::chrono::duration<double>(t2 - host->since).count();
  if (host->log_seconds > 0 && secs >= host->log_seconds) {
    const double n = double(host->frames);
    REXLOG_INFO("hybrid n1: {:.1f} s, {} frames ({} scenes): {} instances ({} skipped) -> {:.0f} draws, {:.0f}k tris "
                "per frame; render thread {:.3f} ms/frame ({:.3f} in n1_record), game thread walk {:.3f} ms; meshes "
                "{} ready {} pending {} missing, {} loading, {} textures ({} unsupported); skinned {} drawn {} culled {} "
                "skipped, {} meshes ready; {} shaders {} pipelines ({} draws waiting), feed {} lights {} envs ({} "
                "instances bound, {} handles), lighting json {:.3f} ms game / {:.3f} ms parse (totals); particles {} emitters "
                "{} sprites {} meshes ({} dropped, {} skipped, {}/{} materials ready/failed), game thread {:.3f} ms",
                secs, host->frames, host->scenes, st.instances, st.skipped, host->draws / n, host->triangles / n / 1000,
                host->host_ms / n, host->record_ms / n, host->build_ms / n, st.meshes_ready, st.meshes_pending,
                st.meshes_missing, st.loading, st.textures, st.textures_unsupported, st.skel_drawn, st.skel_culled,
                st.skel_skipped, st.skel_meshes_ready, st.shaders, st.pipelines, st.pipelines_pending, st.dyn_lights,
                st.light_envs, st.env_bound, st.env_handles, host->lighting_ms, host->feed_ms, st.particle_emitters,
                st.particle_sprites, st.particle_meshes, st.particle_dropped, st.particle_skipped,
                st.particle_mats_ready, st.particle_mats_failed, host->particles_ms / n);
    host->since = t2, host->frames = host->draws = host->triangles = host->scenes = 0;
    host->host_ms = host->record_ms = host->build_ms = host->feed_ms = host->lighting_ms = host->particles_ms = 0;
  }
  return true;
}
#endif

void Renderer::DestroySceneFill() {
#ifdef BOTS_N1FFI
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
  if (!mode || cam.seq != seq) return false;  // overwritten: the game ran 64 frames ahead
  // The view rectangle in host pixels; a target smaller than it isn't the back buffer.
  const uint32_t x0 = Host(uint32_t(cam.rect[0])), y0 = Host(uint32_t(cam.rect[1]));
  const uint32_t x1 = Host(uint32_t(cam.rect[0] + cam.rect[2])), y1 = Host(uint32_t(cam.rect[1] + cam.rect[3]));
  if (x1 <= x0 || y1 <= y0 || target.width < x1 || target.height < y1) return false;

  const VkRect2D area{{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}};
  if (mode == 4) {
#ifdef BOTS_N1FFI
    if (!DrawSceneN1(target, cam, area)) return false;
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
