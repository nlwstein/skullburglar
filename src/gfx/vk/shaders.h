// Guest shader microcode -> SPIR-V, through the SDK's Xenos translator.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/string/buffer.h>

namespace bots::gfx::vk {

class ShaderCache {
 public:
  using Shader = rex::graphics::vulkan::VulkanShader;
  using Translation = Shader::VulkanTranslation;

  explicit ShaderCache(const rex::ui::vulkan::VulkanDevice* device);

  // The shader whose big-endian microcode is at `ucode`, analyzed. Cached by content hash.
  Shader* Get(rex::graphics::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords);
  // The shader's translation for `modification`, translated and with a module; null if translation failed.
  Translation* Translate(Shader& shader, uint64_t modification);

  const rex::graphics::SpirvShaderTranslator& translator() const { return translator_; }

 private:
  const rex::ui::vulkan::VulkanDevice* device_;
  rex::graphics::SpirvShaderTranslator translator_;
  rex::string::StringBuffer disasm_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
};

}  // namespace bots::gfx::vk
