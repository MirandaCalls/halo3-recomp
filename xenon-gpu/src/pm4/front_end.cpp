#include <xgpu/pm4.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace xgpu::pm4 {
namespace {

uint32_t Swap(uint32_t v) { return __builtin_bswap32(v); }

constexpr uint32_t kSetBinMaskLo = 0x60, kSetBinMaskHi = 0x61, kSetBinSelectLo = 0x62,
                   kSetBinSelectHi = 0x63, kSetBinMask = 0x50, kSetBinSelect = 0x51;
constexpr uint32_t kIndirectBuffer = 0x3F, kIndirectBufferPfd = 0x37;
constexpr uint32_t kDrawIndx = 0x22, kDrawIndx2 = 0x36;
constexpr uint32_t kSetConstant = 0x2D, kSetConstant2 = 0x55, kSetShaderConstants = 0x56;
constexpr uint32_t kLoadAluConstant = 0x2F, kImLoad = 0x27, kImLoadImmediate = 0x2B;
constexpr uint32_t kXeSwap = 0x64;

// SET_CONSTANT / LOAD_ALU_CONSTANT type -> register base.
constexpr uint32_t kConstantBases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};

}  // namespace

FrontEnd::FrontEnd(const uint8_t* memory, const std::vector<uint32_t>& initial_registers)
    : memory_(memory), registers_(initial_registers) {
  registers_.resize(kRegisterCount, 0);
}

std::vector<uint32_t> FrontEnd::Fetch(uint32_t kind, uint32_t address, uint32_t dwords) {
  std::vector<uint32_t> host(dwords);
  if (reads_) {
    // Reads are in execution order; the capture may also contain reads from
    // stream parts that aren't replayed (before the first recorded ring
    // segment), so look a little ahead and skip those.
    for (size_t i = next_read_; i < reads_->size() && i < next_read_ + 256; ++i) {
      const RecordedRead& read = (*reads_)[i];
      if (read.kind == kind && read.address == address && read.dwords.size() >= dwords) {
        next_read_ = i + 1;
        ++stats_.recorded_reads_used;
        for (uint32_t j = 0; j < dwords; ++j) host[j] = Swap(read.dwords[j]);
        return host;
      }
    }
    ++stats_.recorded_read_mismatches;
  }
  for (uint32_t i = 0; i < dwords; ++i) host[i] = Capture::ReadBE(memory_, address + i * 4);
  return host;
}

void FrontEnd::ExecuteRingSegment(const RingSegment& segment) {
  std::vector<uint32_t> host(segment.dwords.size());
  for (size_t i = 0; i < host.size(); ++i) host[i] = Swap(segment.dwords[i]);
  Reader reader{host.data(), host.size()};
  Execute(reader, 0);
}

void FrontEnd::Execute(Reader& reader, int depth) {
  while (reader.remaining()) {
    if (!ExecutePacket(reader, depth)) break;
  }
}

void FrontEnd::ExecuteIndirect(uint32_t address, uint32_t dwords, int depth) {
  if (depth > 8) return;
  ++stats_.indirect_buffers;
  std::vector<uint32_t> host = Fetch(1, address, dwords);
  Reader reader{host.data(), host.size()};
  Execute(reader, depth + 1);
}

void FrontEnd::WriteRegister(uint32_t index, uint32_t value) {
  if (index < registers_.size()) registers_[index] = value;
  ++stats_.register_writes;
}

void FrontEnd::LoadShader(bool pixel, uint32_t address, const uint32_t* host_dwords, uint32_t count) {
  // Hash the guest byte order, like Xenia's shader storage.
  std::vector<uint32_t> guest(count);
  for (uint32_t i = 0; i < count; ++i) guest[i] = Swap(host_dwords[i]);
  uint64_t hash = XXH3_64bits(guest.data(), count * 4);
  (pixel ? pixel_shader_hash_ : vertex_shader_hash_) = hash;
  if (on_shader_load) on_shader_load(ShaderLoad{pixel, address, count, hash, guest.data()});
}

bool FrontEnd::ExecutePacket(Reader& reader, int depth) {
  const uint32_t packet = reader.Read();
  if (packet == 0) return true;
  switch (packet >> 30) {
    case 0: {
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (reader.remaining() < count) return false;
      const uint32_t base = packet & 0x7FFF;
      const bool one = (packet >> 15) & 1;
      ++stats_.type0_packets;
      for (uint32_t i = 0; i < count; ++i) WriteRegister(one ? base : base + i, reader.Read());
      return true;
    }
    case 1: {
      if (reader.remaining() < 2) return false;
      WriteRegister(packet & 0x7FF, reader.Read());
      WriteRegister((packet >> 11) & 0x7FF, reader.Read());
      return true;
    }
    case 2:
      return true;
    default:
      break;
  }

  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader.remaining() < count) return false;
  const size_t end = reader.pos + count;
  ++stats_.type3[opcode];

  // Predicated packets run only if a selected bin is in the mask (tiling).
  if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == kXeSwap)) {
    ++stats_.predicated_skipped;
    reader.pos = end;
    return true;
  }

  switch (opcode) {
    case kIndirectBuffer:
    case kIndirectBufferPfd: {
      uint32_t address = reader.Read();
      uint32_t length = reader.Read() & 0xFFFFF;
      ExecuteIndirect(address, length, depth);
      break;
    }
    case kSetBinMaskLo:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader.Read();
      break;
    case kSetBinMaskHi:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(reader.Read()) << 32);
      break;
    case kSetBinSelectLo:
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | reader.Read();
      break;
    case kSetBinSelectHi:
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(reader.Read()) << 32);
      break;
    case kSetBinMask: {
      uint64_t hi = reader.Read(), lo = reader.Read();
      bin_mask_ = (hi << 32) | lo;
      break;
    }
    case kSetBinSelect: {
      uint64_t hi = reader.Read(), lo = reader.Read();
      bin_select_ = (hi << 32) | lo;
      break;
    }
    case kSetConstant: {
      uint32_t offset_type = reader.Read();
      uint32_t index = offset_type & 0x7FF, type = (offset_type >> 16) & 0xFF;
      if (type < 5) {
        for (uint32_t i = 0; i + 1 < count; ++i) WriteRegister(kConstantBases[type] + index + i, reader.Read());
      }
      break;
    }
    case kSetConstant2:
    case kSetShaderConstants: {
      uint32_t index = reader.Read() & 0xFFFF;
      for (uint32_t i = 0; i + 1 < count; ++i) WriteRegister(index + i, reader.Read());
      break;
    }
    case kLoadAluConstant: {
      uint32_t address = reader.Read() & 0x3FFFFFFF;
      uint32_t offset_type = reader.Read();
      uint32_t size = reader.Read() & 0xFFF;
      uint32_t index = offset_type & 0x7FF, type = (offset_type >> 16) & 0xFF;
      std::vector<uint32_t> data = Fetch(3, address, size);
      if (type < 5) {
        for (uint32_t i = 0; i < size; ++i) WriteRegister(kConstantBases[type] + index + i, data[i]);
      }
      break;
    }
    case kImLoad: {
      uint32_t addr_type = reader.Read();
      uint32_t start_size = reader.Read();
      uint32_t address = addr_type & ~3u, size = start_size & 0xFFFF;
      std::vector<uint32_t> host = Fetch(2, address, size);
      LoadShader((addr_type & 3) == 1, address, host.data(), size);
      break;
    }
    case kImLoadImmediate: {
      uint32_t type = reader.Read();
      uint32_t size = reader.Read() & 0xFFFF;
      if (reader.remaining() >= size) {
        LoadShader(type == 1, 0, reader.data + reader.pos, size);
      }
      break;
    }
    case kDrawIndx:
    case kDrawIndx2:
      HandleDraw(reader, opcode, count);
      break;
    case kXeSwap:
      if (count >= 4) {
        reader.Read();  // 'SWAP' signature
        uint32_t address = reader.Read(), width = reader.Read(), height = reader.Read();
        if (on_swap) on_swap(address, width, height);
      }
      break;
    default:
      break;
  }
  reader.pos = end;
  return true;
}

void FrontEnd::HandleDraw(Reader& reader, uint32_t opcode, uint32_t count) {
  uint32_t remaining = count;
  if (opcode == kDrawIndx) {
    reader.Read();  // viz query condition
    --remaining;
  }
  const uint32_t initiator = reader.Read();
  --remaining;
  WriteRegister(reg::VGT_DRAW_INITIATOR, initiator);
  Draw draw{};
  draw.index = draw_index_++;
  draw.opcode = opcode;
  draw.initiator = initiator;
  draw.index_count = initiator >> 16;
  // source_select: 0 = DMA (index buffer in memory), 1 = immediate, 2 = auto.
  if (((initiator >> 6) & 3) == 0 && remaining >= 2) {
    draw.indexed = true;
    draw.index_base = reader.Read();
    WriteRegister(0x21FA, draw.index_base);  // VGT_DMA_BASE
    WriteRegister(0x21FB, reader.Read());    // VGT_DMA_SIZE
  }
  draw.vertex_shader_hash = vertex_shader_hash_;
  draw.pixel_shader_hash = pixel_shader_hash_;
  draw.is_resolve = EdramMode(registers_[reg::RB_MODECONTROL] & 7) == EdramMode::kCopy;
  draw.registers = registers_.data();
  ++(draw.is_resolve ? stats_.resolves : stats_.draws);
  if (on_draw) on_draw(draw);
}

}  // namespace xgpu::pm4
