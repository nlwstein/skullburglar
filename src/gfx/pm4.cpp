#include "pm4.h"

#include <memory>

namespace bots::gfx {
namespace {

namespace rg = rex::graphics;

constexpr uint32_t kAluBase = 0x4000, kFetchBase = 0x4800, kBoolBase = 0x4900, kLoopBase = 0x4908,
                   kRegistersBase = 0x2000;

uint32_t BE(const uint32_t* p) { return __builtin_bswap32(*p); }

uint32_t ConstantBase(uint32_t type) {
  switch (type) {
    case 0: return kAluBase;
    case 1: return kFetchBase;
    case 2: return kBoolBase;
    case 3: return kLoopBase;
    case 4: return kRegistersBase;
    default: return UINT32_MAX;
  }
}

enum Opcode : uint32_t {
  kDrawIndx = 0x22,
  kImLoad = 0x27,
  kImLoadImmediate = 0x2B,
  kSetConstant = 0x2D,
  kLoadAluConstant = 0x2F,
  kDrawIndx2 = 0x36,
  kIndirectBufferPfd = 0x37,
  kIndirectBuffer = 0x3F,
  kSetConstant2 = 0x55,
  kSetShaderConstants = 0x56,
};

}  // namespace

Pm4Executor::Pm4Executor(PhysicalMemory memory, Pm4Sink* sink)
    : memory_(std::move(memory)), sink_(sink), regs_(std::make_unique<rg::RegisterFile>()) {}

void Pm4Executor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= rg::RegisterFile::kRegisterCount) return;
  regs_->values[index] = value;
  ++stats_.register_writes;
  constant_writes_.Note(index);
}

void Pm4Executor::Execute(const uint8_t* data, size_t bytes) {
  const auto* d = reinterpret_cast<const uint32_t*>(data);
  const size_t n = bytes / 4;
  for (size_t i = 0; i < n;) {
    const uint32_t h = BE(d + i);
    packet_offset_ = i * 4;
    ++stats_.packets;
    switch (h >> 30) {
      case 0: {
        const uint32_t count = ((h >> 16) & 0x3FFF) + 1, base = h & 0x7FFF;
        const bool one_register = (h >> 15) & 1;
        for (uint32_t k = 0; k < count && i + 1 + k < n; ++k) {
          WriteRegister(one_register ? base : base + k, BE(d + i + 1 + k));
        }
        i += 1 + count;
        break;
      }
      case 1:
        if (i + 2 < n) {
          WriteRegister(h & 0x7FF, BE(d + i + 1));
          WriteRegister((h >> 11) & 0x7FF, BE(d + i + 2));
        }
        i += 3;
        break;
      case 2:
        i += 1;
        break;
      default: {
        const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
        if (i + 1 + count <= n) Type3((h >> 8) & 0x7F, d + i + 1, count);
        i += 1 + count;
        break;
      }
    }
  }
}

void Pm4Executor::Type3(uint32_t opcode, const uint32_t* body, uint32_t count) {
  switch (opcode) {
    case kSetConstant: {
      const uint32_t offset_type = BE(body);
      const uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
      if (base == UINT32_MAX) break;
      for (uint32_t k = 1; k < count; ++k) WriteRegister(base + (offset_type & 0x7FF) + k - 1, BE(body + k));
      break;
    }
    case kSetConstant2:
    case kSetShaderConstants: {
      const uint32_t index = BE(body) & 0xFFFF;
      for (uint32_t k = 1; k < count; ++k) WriteRegister(index + k - 1, BE(body + k));
      break;
    }
    case kLoadAluConstant: {
      const uint32_t address = BE(body) & 0x3FFFFFFF, offset_type = BE(body + 1), dwords = BE(body + 2) & 0xFFF;
      const uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
      const auto* src = reinterpret_cast<const uint32_t*>(memory_(address & 0x1FFFFFFF));
      if (base == UINT32_MAX || !src) break;
      for (uint32_t k = 0; k < dwords; ++k) WriteRegister(base + (offset_type & 0x7FF) + k, BE(src + k));
      break;
    }
    case kImLoad: {
      const uint32_t addr_type = BE(body), start_size = BE(body + 1);
      ShaderLoad& load = (addr_type & 3) == 0 ? vs_ : ps_;
      load = {addr_type & ~3u, start_size & 0xFFFF, nullptr};
      break;
    }
    case kImLoadImmediate: {
      const uint32_t type = BE(body), start_size = BE(body + 1);
      ShaderLoad& load = type == 0 ? vs_ : ps_;
      load = {0, start_size & 0xFFFF, body + 2};
      break;
    }
    case kDrawIndx:
    case kDrawIndx2: {
      // DRAW_INDX: viz query token, initiator, [DMA base, DMA size]. DRAW_INDX_2: initiator, [inline indices].
      const uint32_t* p = body + (opcode == kDrawIndx ? 1 : 0);
      const uint32_t left = count - uint32_t(p - body);
      if (!left) break;
      Draw draw;
      draw.draw_indx_2 = opcode == kDrawIndx2;
      draw.constant_writes = constant_writes_.writes();
      draw.initiator.value = BE(p);
      WriteRegister(uint32_t(rg::XE_GPU_REG_VGT_DRAW_INITIATOR), draw.initiator.value);
      const auto source = draw.initiator.source_select;
      if (source == rex::graphics::xenos::SourceSelect::kDMA && left >= 3) {
        rg::reg::VGT_DMA_SIZE size;
        size.value = BE(p + 2);
        draw.index_base = BE(p + 1);
        draw.index_words = size.num_words;
        draw.index_endian = size.swap_mode;
        WriteRegister(uint32_t(rg::XE_GPU_REG_VGT_DMA_BASE), draw.index_base);
        WriteRegister(uint32_t(rg::XE_GPU_REG_VGT_DMA_SIZE), BE(p + 2));
      } else if (source == rex::graphics::xenos::SourceSelect::kImmediate) {
        draw.immediate_indices = p + 1;
        draw.immediate_dwords = left - 1;
      }
      ++stats_.draws;
      if (sink_) sink_->OnDraw(*regs_, vs_, ps_, draw);
      break;
    }
    case kIndirectBuffer:
    case kIndirectBufferPfd:
      ++stats_.skipped_indirect;
      break;
    default:
      ++stats_.unknown_packets;
      if (sink_) sink_->OnEvent(opcode, body, count);
      break;
  }
}

}  // namespace bots::gfx
