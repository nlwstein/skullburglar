#include "shaders.h"

#include <cstdio>

#include <xxhash.h>

namespace bots::gfx::vk {

namespace rg = rex::graphics;

// Host render targets: no fragment shader interlock, no 2x MSAA tricks (the game renders without MSAA).
ShaderCache::ShaderCache(const rex::ui::vulkan::VulkanDevice* device)
    : device_(device), translator_(rg::SpirvShaderTranslator::Features(device), false, false, false) {}

ShaderCache::Shader* ShaderCache::Get(rg::xenos::ShaderType type, const uint32_t* ucode, uint32_t dwords) {
  // Vertex and pixel microcode can be identical; keep them apart.
  const uint64_t hash = XXH3_64bits(ucode, size_t(dwords) * 4) ^ uint64_t(type);
  auto& slot = shaders_[hash];
  if (!slot) {
    slot = std::make_unique<Shader>(device_, type, hash, ucode, dwords);
    slot->AnalyzeUcode(disasm_);
  }
  return slot.get();
}

ShaderCache::Translation* ShaderCache::Translate(Shader& shader, uint64_t modification) {
  auto* translation = static_cast<Translation*>(shader.GetOrCreateTranslation(modification));
  if (!translation->is_translated() && !translator_.TranslateAnalyzedShader(*translation)) {
    std::fprintf(stderr, "native renderer: shader %016llX translation failed\n",
                 (unsigned long long)shader.ucode_data_hash());
  }
  return translation->is_valid() && translation->GetOrCreateShaderModule() ? translation : nullptr;
}

}  // namespace bots::gfx::vk
