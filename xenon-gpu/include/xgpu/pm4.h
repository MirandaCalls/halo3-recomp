// xenon-gpu: Xenos PM4 command stream front end.
//
// Walks primary ring segments and (recursively) indirect buffers, maintains
// the GPU register file, and reports the events a renderer needs. Semantics
// follow Xenia's command processor (register ranges for SET_CONSTANT /
// LOAD_ALU_CONSTANT, predication by bin mask/select, draws in copy mode being
// resolves).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <xgpu/capture.h>

namespace xgpu::pm4 {

// Register indices (dwords) used by the front end.
namespace reg {
constexpr uint32_t RB_SURFACE_INFO = 0x2000;
constexpr uint32_t RB_COLOR_INFO = 0x2001;
constexpr uint32_t RB_DEPTH_INFO = 0x2002;
constexpr uint32_t RB_COLOR1_INFO = 0x2003;
constexpr uint32_t RB_COLOR2_INFO = 0x2004;
constexpr uint32_t RB_COLOR3_INFO = 0x2005;
constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;
constexpr uint32_t RB_MODECONTROL = 0x2208;
constexpr uint32_t RB_COPY_CONTROL = 0x2318;
constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
}  // namespace reg

enum class EdramMode : uint32_t { kNoOperation = 0, kColorDepth = 4, kDepthOnly = 5, kCopy = 6 };

struct ShaderLoad {
  bool pixel;
  uint32_t address;  // 0 for immediate
  uint32_t dword_count;
  uint64_t hash;  // XXH3 of the microcode bytes (same as Xenia's shader storage)
  const uint32_t* guest_dwords;  // microcode in guest (big-endian) order, valid during the callback
};

struct Draw {
  uint32_t index;  // draw number within the frame
  uint32_t opcode;  // 0x22 DRAW_INDX or 0x36 DRAW_INDX_2
  uint32_t initiator;  // VGT_DRAW_INITIATOR
  bool indexed;
  uint32_t index_base;
  uint32_t index_count;
  uint64_t vertex_shader_hash;
  uint64_t pixel_shader_hash;
  bool is_resolve;  // RB_MODECONTROL.edram_mode == kCopy
  const uint32_t* registers;  // register file at the time of the draw
};

struct Stats {
  uint64_t type3[128] = {};
  uint64_t type0_packets = 0, register_writes = 0;
  uint64_t indirect_buffers = 0, predicated_skipped = 0;
  uint64_t draws = 0, resolves = 0;
  uint64_t recorded_reads_used = 0, recorded_read_mismatches = 0;
};

class FrontEnd {
 public:
  // `memory` is the guest physical memory image commands are read from.
  FrontEnd(const uint8_t* memory, const std::vector<uint32_t>& initial_registers);
  // Use recorded reads (in execution order) instead of the memory image for
  // indirect buffers, shader loads and constant loads.
  void UseRecordedReads(const std::vector<RecordedRead>* reads) { reads_ = reads; }

  std::function<void(const Draw&)> on_draw;
  std::function<void(const ShaderLoad&)> on_shader_load;
  // XE_SWAP (VdSwap): front buffer guest address and size.
  std::function<void(uint32_t address, uint32_t width, uint32_t height)> on_swap;

  void ExecuteRingSegment(const RingSegment& segment);
  const Stats& stats() const { return stats_; }
  const uint32_t* registers() const { return registers_.data(); }

 private:
  // Reads dwords from a host-order buffer.
  struct Reader {
    const uint32_t* data;
    size_t count;
    size_t pos = 0;
    size_t remaining() const { return count - pos; }
    uint32_t Read() { return data[pos++]; }
  };
  void Execute(Reader& reader, int depth);
  bool ExecutePacket(Reader& reader, int depth);
  void ExecuteIndirect(uint32_t address, uint32_t dwords, int depth);
  void WriteRegister(uint32_t index, uint32_t value);
  void HandleDraw(Reader& reader, uint32_t opcode, uint32_t count);
  void LoadShader(bool pixel, uint32_t address, const uint32_t* host_dwords, uint32_t count);

  // Fetches `dwords` host-order dwords at `address`, from the recorded reads
  // when available (kind must match), else from the memory image.
  std::vector<uint32_t> Fetch(uint32_t kind, uint32_t address, uint32_t dwords);

  const uint8_t* memory_;
  const std::vector<RecordedRead>* reads_ = nullptr;
  size_t next_read_ = 0;
  std::vector<uint32_t> registers_;
  uint64_t bin_mask_ = ~uint64_t(0), bin_select_ = ~uint64_t(0);
  uint64_t vertex_shader_hash_ = 0, pixel_shader_hash_ = 0;
  uint32_t draw_index_ = 0;
  Stats stats_;
};

}  // namespace xgpu::pm4
