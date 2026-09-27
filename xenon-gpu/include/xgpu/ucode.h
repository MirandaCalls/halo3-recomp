// xenon-gpu: Xenos shader microcode utilities.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace xgpu::ucode {

enum class Stage : uint8_t { kUnknown, kVertex, kPixel };

struct ProgramInfo {
  // Program length in dwords, matching what the guest uploads (control-flow
  // extent plus the trailing slot the D3D runtime includes).
  uint32_t dword_count = 0;
  Stage stage = Stage::kUnknown;
};

// Parses the control-flow program at the start of `words` (host-order dwords,
// i.e. already swapped from the guest's big-endian storage) and returns its
// extent and stage, or nullopt if the data is not a plausible Xenos program.
std::optional<ProgramInfo> ParseProgram(const uint32_t* words, size_t word_count);

// Same, reading big-endian guest dwords directly (e.g. from a mapped file).
std::optional<ProgramInfo> ParseProgramBigEndian(const uint8_t* data, size_t word_count);

}  // namespace xgpu::ucode
