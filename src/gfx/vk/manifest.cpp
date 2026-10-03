// Pipeline manifest and warm-up (precompile), see Renderer::LoadManifest.
//
// File: header {magic "BOTSPM02", record size, feature class}, then chunks, appended as pipelines are created:
//   'S' u64 hash, u32 type, u32 dwords, ucode (big-endian, as the guest has it)   a shader, before its first use
//   'P' record                                                                     a pipeline
// A record is the two shaders (ShaderCache hash + translation modification; pixel hash 0 = none), the binding counts
// (LayoutKey) and the PipelineKey bytes from `topology` on. Everything in it is guest state except what the device
// features below decide (render-target formats are fixed per guest format), so manifests are interchangeable between
// GPUs of the same feature class and can be merged into one master per class (in practice: MoltenVK, and the rest).
// Bump the magic when the record layout or the meaning of the translator's modification bits changes. A header for
// another class or record size starts the file over. A truncated last chunk (a killed run) is ignored.
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>

#include "renderer.h"
#include "shaders.h"

namespace bots::gfx::vk {

namespace {

constexpr char kMagic[8] = {'B', 'O', 'T', 'S', 'P', 'M', '0', '2'};

// The fixed part of a record; the PipelineKey state bytes follow it.
struct RecordHeader {
  uint64_t vs, vs_mod, ps, ps_mod;
  uint32_t layout[4];
};

uint32_t ByteSwap(uint32_t v) { return __builtin_bswap32(v); }

struct Header {
  char magic[8];
  uint32_t record_size;
  uint32_t features;  // the device features that go into keys: depth clamp, clip and cull distances (draw.cpp)
};

}  // namespace

void Renderer::LoadManifest(const std::filesystem::path& path) {
  if (manifest_) return;  // loaded already (the cache path is given twice at startup)
  if (const char* v = std::getenv("BOTS_GFX_PRECOMPILE"); v && *v) warm_enabled_ = *v != '0';
  const size_t state_bytes = sizeof(PipelineKey) - offsetof(PipelineKey, topology);
  const auto& props = device_->properties();
  Header header{{},
                uint32_t(sizeof(RecordHeader) + state_bytes),
                uint32_t(props.depthClamp) | uint32_t(props.shaderClipDistance) << 1 |
                    uint32_t(props.shaderCullDistance) << 2};
  std::memcpy(header.magic, kMagic, sizeof(kMagic));

  // Reads a manifest into manifest_shaders_ / manifest_records_ / warm_ (`own`: this machine's file); returns the size
  // up to the last complete chunk, or 0 when the file is missing or of another class.
  auto read = [&](const std::filesystem::path& file, bool own, size_t& size) -> size_t {
    std::vector<char> data;
    if (std::FILE* f = std::fopen(file.string().c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      const long length = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (length > 0) {
        data.resize(size_t(length));
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
      }
      std::fclose(f);
    }
    size = data.size();
    if (data.size() < sizeof(Header) || std::memcmp(data.data(), &header, sizeof(Header)) != 0) return 0;
    size_t at = sizeof(Header);
    while (at < data.size()) {
      const char tag = data[at];
      if (tag == 'S' && at + 17 <= data.size()) {
        uint64_t hash;
        uint32_t type, dwords;
        std::memcpy(&hash, &data[at + 1], 8);
        std::memcpy(&type, &data[at + 9], 4);
        std::memcpy(&dwords, &data[at + 13], 4);
        if (at + 17 + size_t(dwords) * 4 > data.size()) break;
        ManifestShader& shader = manifest_shaders_[hash];
        shader.type = type;
        shader.ucode.resize(dwords);
        std::memcpy(shader.ucode.data(), &data[at + 17], size_t(dwords) * 4);
        shader.in_file |= own;
        at += 17 + size_t(dwords) * 4;
      } else if (tag == 'P' && at + 1 + header.record_size <= data.size()) {
        std::string record(&data[at + 1], header.record_size);
        auto [it, added] = manifest_records_.emplace(record, own);
        it->second |= own;
        if (added) warm_.push_back({std::move(record)});
        at += 1 + header.record_size;
      } else {
        break;
      }
    }
    return at;
  };

  size_t size = 0;
  const size_t end = read(path, true, size);
  const bool valid = end != 0;
  const size_t own = manifest_records_.size();
  // The master's records are warmed too; this machine's manifest gets only what it runs (from RecordPipeline).
  size_t master_size = 0;
  const auto master = std::filesystem::path(path).replace_extension().replace_extension(".master.bin");
  const bool have_master = read(master, false, master_size) != 0;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (valid) {
    // Drop a torn tail so appends start on a chunk boundary.
    if (end < size) std::filesystem::resize_file(path, end, ec);
    manifest_ = std::fopen(path.string().c_str(), "ab");
  } else {
    manifest_ = std::fopen(path.string().c_str(), "wb");
    if (manifest_) {
      std::fwrite(&header, sizeof(header), 1, manifest_);
      std::fflush(manifest_);
    }
  }
  if (!warm_enabled_) warm_.clear();
  warm_start_ = std::chrono::steady_clock::now();
  REXLOG_INFO("gfx native: pipeline manifest {}: {} pipelines{}, {} shaders, class {}{}", path.string(), own,
              have_master ? fmt::format(" + {} from the master", manifest_records_.size() - own) : std::string(),
              manifest_shaders_.size(), header.features,
              warm_enabled_ ? "" : " (warm-up off: BOTS_GFX_PRECOMPILE=0)");
  if (master_size && !have_master) REXLOG_WARN("gfx native: {} is for another device class; not used", master.string());
}

void Renderer::RecordPipeline(const PipelineKey& key) {
  if (!manifest_) return;
  const auto* vt = static_cast<const ShaderCache::Translation*>(key.vs);
  const auto* pt = static_cast<const ShaderCache::Translation*>(key.ps);
  LayoutKey layout{};
  for (const auto& [k, v] : pipeline_layouts_) {
    if (v.layout == key.layout) layout = k;
  }
  RecordHeader head{};
  head.vs = vt->shader().ucode_data_hash();
  head.vs_mod = vt->modification();
  if (pt) head.ps = pt->shader().ucode_data_hash(), head.ps_mod = pt->modification();
  std::copy(layout.begin(), layout.end(), head.layout);
  std::string record(reinterpret_cast<const char*>(&head), sizeof(head));
  record.append(reinterpret_cast<const char*>(&key) + offsetof(PipelineKey, topology),
                sizeof(PipelineKey) - offsetof(PipelineKey, topology));
  bool& in_file = manifest_records_[record];
  if (in_file) return;
  in_file = true;
  for (const auto* t : {vt, pt}) {
    if (!t) continue;
    const auto& shader = t->shader();
    ManifestShader& m = manifest_shaders_[shader.ucode_data_hash()];
    if (m.in_file) continue;
    m.in_file = true;
    m.type = uint32_t(shader.type());
    m.ucode.resize(shader.ucode_dword_count());
    for (size_t i = 0; i < shader.ucode_dword_count(); ++i) m.ucode[i] = ByteSwap(shader.ucode_dwords()[i]);
    const uint64_t hash = shader.ucode_data_hash();
    const uint32_t dwords = uint32_t(m.ucode.size());
    std::fputc('S', manifest_);
    std::fwrite(&hash, 8, 1, manifest_);
    std::fwrite(&m.type, 4, 1, manifest_);
    std::fwrite(&dwords, 4, 1, manifest_);
    std::fwrite(m.ucode.data(), 4, dwords, manifest_);
  }
  std::fputc('P', manifest_);
  std::fwrite(record.data(), 1, record.size(), manifest_);
  std::fflush(manifest_);  // runs are often killed rather than shut down
}

// From EndFrame (the GPU thread): starts warm-up pipelines while the background queue is short, and builds the ones
// whose translations have finished. Translations and pipelines both run on the workers behind live work.
void Renderer::PumpWarmup() {
  if (warm_done_ >= warm_.size() || !shaders_) return;
  Workers* workers = shaders_->workers();
  if (!workers) return;
  const size_t cap = 2 * workers->count();
  auto shader_of = [&](uint64_t hash) -> ShaderCache::Shader* {
    auto it = manifest_shaders_.find(hash);
    if (it == manifest_shaders_.end()) return nullptr;
    ManifestShader& m = it->second;
    if (!m.shader) {
      m.shader = shaders_->Get(rex::graphics::xenos::ShaderType(m.type), m.ucode.data(), uint32_t(m.ucode.size()));
    }
    return m.shader->ucode_data_hash() == hash ? m.shader : nullptr;
  };
  // Each call looks at what's started but not done, then starts more; done entries move warm_done_ forward.
  for (size_t i = 0; i < warm_.size() && (i < warm_next_ || workers->background_queued() < cap); ++i) {
    WarmPipeline& warm = warm_[i];
    if (warm.done) continue;
    if (!warm.started) {
      if (i < warm_next_) continue;
      warm.started = true;
      warm_next_ = i + 1;
    }
    RecordHeader head;
    std::memcpy(&head, warm.record.data(), sizeof(head));
    auto finish = [&] { warm.done = true, ++warm_done_; };
    ShaderCache::Shader* vs = shader_of(head.vs);
    ShaderCache::Shader* ps = head.ps ? shader_of(head.ps) : nullptr;
    if (!vs || (head.ps && !ps)) {
      finish();
      continue;
    }
    ShaderCache::Translation *vt = nullptr, *pt = nullptr;
    const Async vstate = shaders_->Request(*vs, head.vs_mod, vt, false, true);
    const Async pstate = ps ? shaders_->Request(*ps, head.ps_mod, pt, false, true) : Async::kReady;
    if (vstate == Async::kFailed || pstate == Async::kFailed) {
      finish();
      continue;
    }
    if (vstate != Async::kReady || pstate != Async::kReady) continue;
    const LayoutKey layout_key{head.layout[0], head.layout[1], head.layout[2], head.layout[3]};
    const PipelineLayout* layout = GetPipelineLayout(layout_key);
    if (!layout) {
      finish();
      continue;
    }
    PipelineKey key;
    std::memset(&key, 0, sizeof(key));
    key.vs = vt;
    key.ps = pt;
    key.layout = layout->layout;
    std::memcpy(reinterpret_cast<char*>(&key) + offsetof(PipelineKey, topology), warm.record.data() + sizeof(head),
                sizeof(PipelineKey) - offsetof(PipelineKey, topology));
    bool pending = false;
    GetPipeline(key, false, pending, true);
    if (!pending) finish();  // ready, failed, or created live meanwhile
  }
  if (warm_done_ >= warm_.size() && !warm_logged_) {
    warm_logged_ = true;
    REXLOG_INFO("gfx native: pipeline warm-up done: {} pipelines in {:.1f} s", warm_.size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - warm_start_).count());
  }
}

}  // namespace bots::gfx::vk
