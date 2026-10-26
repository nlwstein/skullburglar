// Hybrid renderer research spike, renderer side (src/hybrid.cpp has the game side): stands in for a separate scene
// renderer. When the game's 3D scene is suppressed, the guest render thread writes a marker (hybrid_state.h) into the
// GPU stream where the scene would have been drawn; native.cpp passes it here, and before the next colour draw (the
// HUD's first) the colour target gets a host-drawn "scene" for the frame's camera:
//   fill 1  a flat test colour over the view rectangle
//   fill 2  a sky gradient and a ground grid (UE3 world units: 1 m lines, 10 m axes) cast through the frame's
//           inverse view-projection, so camera motion shows that the camera is the game's
//   fill 3  the grid lines only, over the game's own scene (not suppressed): lines that stay put on the world as the
//           camera moves check the camera against what the game draws
// It records into the frame's command buffer like a format transfer: its own one-attachment rendering, so the game's
// pass simply reopens for the HUD.
#include <cstdlib>
#include <cstring>
#include <string>

#include <glslang/Public/ShaderLang.h>

#include "../../hybrid_state.h"
#include "renderer.h"

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

void Renderer::DestroySceneFill() {
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

  BeginCommands(kGpuDraw);
  EndRendering("hybrid fill");
  Image* const write_image = &target;
  Access({}, {&write_image, 1});
  const VkRect2D area{{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}};
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
