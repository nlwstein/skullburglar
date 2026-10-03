// In-process consumer of the Xbox D3D runtime's PM4 (native renderer).
//
// The runtime writes almost only type-0 register writes, IM_LOAD (shader microcode by physical address) and
// draws. Pm4Executor applies them to a Xenos register file and hands every draw, with the state it sees, to a
// sink. It runs synchronously: there is no ring, no fence and no second thread. Indirect buffers are not
// followed. The runtime uses them for two things: predicated tiling (one replay per tile of draws already
// executed inline; the native renderer draws each once over the whole target), and a hardware pipeline flush
// (WAIT_UNTIL, SQ_GPR_MANAGEMENT, 24 point draws with colour writes off) with no visible output.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

namespace bots::gfx {

// Resolves a GPU physical address (0..0x1FFFFFFF) to host memory.
using PhysicalMemory = std::function<const uint8_t*(uint32_t physical)>;

struct ShaderLoad {
  uint32_t address = 0;  // physical
  uint32_t dwords = 0;
  const uint32_t* inline_ucode = nullptr;  // IM_LOAD_IMMEDIATE: microcode inside the packet (big-endian)
};

struct Draw {
  rex::graphics::reg::VGT_DRAW_INITIATOR initiator{};
  uint32_t index_base = 0;   // physical, for DMA indices
  uint32_t index_words = 0;  // VGT_DMA_SIZE.num_words: DMA index buffer length in indices (may exceed the count)
  rex::graphics::xenos::Endian index_endian{};
  const uint32_t* immediate_indices = nullptr;  // source_select immediate: inline in the packet (big-endian)
  uint32_t immediate_dwords = 0;
  bool draw_indx_2 = false;
  // When each group of shader constant registers was last written (kConstantGroups entries, a counter that only
  // grows), so a renderer can reuse an upload nothing it reads has changed since. Null: unknown.
  const uint64_t* constant_writes = nullptr;
};

// Constant register groups for Draw::constant_writes: float constants 0x4000-0x47FF in blocks of 64 registers
// (16 float4 constants; group = float4 index / 16), then fetch constants 0x4800-0x48FF, then bool/loop constants
// 0x4900-0x4927.
constexpr uint32_t kFloatConstantGroups = 32;
constexpr uint32_t kFetchConstantGroup = 32;
constexpr uint32_t kBoolLoopConstantGroup = 33;
constexpr uint32_t kConstantGroups = 34;

// Fills Draw::constant_writes: call Note for every register write.
class ConstantWriteTracker {
 public:
  void Note(uint32_t index) {
    if (index < 0x4000 || index >= 0x4928) return;
    const uint32_t group = index < 0x4800 ? (index - 0x4000) >> 6
                           : index < 0x4900 ? kFetchConstantGroup
                                            : kBoolLoopConstantGroup;
    writes_[group] = ++counter_;
  }
  const uint64_t* writes() const { return writes_; }

 private:
  uint64_t writes_[kConstantGroups] = {};
  uint64_t counter_ = 0;
};

class Pm4Sink {
 public:
  virtual ~Pm4Sink() = default;
  virtual void OnDraw(const rex::graphics::RegisterFile& regs, const ShaderLoad& vs, const ShaderLoad& ps,
                      const Draw& draw) = 0;
  virtual void OnEvent(uint32_t opcode, const uint32_t* body, uint32_t dwords) {}
};

class Pm4Executor {
 public:
  Pm4Executor(PhysicalMemory memory, Pm4Sink* sink);

  // Executes big-endian PM4 dwords.
  void Execute(const uint8_t* data, size_t bytes);

  rex::graphics::RegisterFile& regs() { return *regs_; }
  const ShaderLoad& vertex_shader() const { return vs_; }
  const ShaderLoad& pixel_shader() const { return ps_; }
  // Byte offset, within the buffer being executed, of the packet being handled (valid inside sink calls).
  size_t packet_offset() const { return packet_offset_; }

  struct Stats {
    uint64_t packets = 0, draws = 0, register_writes = 0, unknown_packets = 0, skipped_indirect = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  void WriteRegister(uint32_t index, uint32_t value);
  void Type3(uint32_t opcode, const uint32_t* body, uint32_t count);

  PhysicalMemory memory_;
  Pm4Sink* sink_;
  std::unique_ptr<rex::graphics::RegisterFile> regs_;
  ShaderLoad vs_, ps_;
  ConstantWriteTracker constant_writes_;
  Stats stats_;
  size_t packet_offset_ = 0;
};

}  // namespace bots::gfx
