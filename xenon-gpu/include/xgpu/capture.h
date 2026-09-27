// xenon-gpu: loading GPU frame captures written by the runtime
// (AUTOPORT_GPU_CAPTURE, see ReXGlue command_processor.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xgpu {

constexpr size_t kRegisterCount = 0x5003;
constexpr size_t kPhysicalMemorySize = 0x20000000;

// A block of memory the command processor read during the frame, as it was
// at that moment (indirect buffers, shader microcode, constant loads).
struct RecordedRead {
  uint32_t kind;  // 1 indirect buffer, 2 IM_LOAD, 3 LOAD_ALU_CONSTANT
  uint32_t address;
  std::vector<uint32_t> dwords;  // guest (big-endian) order
};

struct RingSegment {
  uint32_t ring_address;
  uint32_t ring_size_bytes;
  uint32_t read_index;
  uint32_t write_index;
  std::vector<uint32_t> dwords;  // guest (big-endian) order, as in memory
};

class Capture {
 public:
  ~Capture();
  bool Load(const std::string& directory);

  // Guest physical memory at frame start / end (big-endian contents).
  const uint8_t* memory_start() const { return memory_start_; }
  const uint8_t* memory_end() const { return memory_end_; }
  const std::vector<uint32_t>& registers() const { return registers_; }
  const std::vector<RingSegment>& ring() const { return ring_; }
  const std::vector<RecordedRead>& reads() const { return reads_; }

  // Big-endian dword at a GPU physical address (masked to 512 MB).
  static uint32_t ReadBE(const uint8_t* memory, uint32_t address) {
    const uint8_t* p = memory + (address & (kPhysicalMemorySize - 1));
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

 private:
  const uint8_t* memory_start_ = nullptr;
  const uint8_t* memory_end_ = nullptr;
  std::vector<uint32_t> registers_;
  std::vector<RingSegment> ring_;
  std::vector<RecordedRead> reads_;
};

}  // namespace xgpu
