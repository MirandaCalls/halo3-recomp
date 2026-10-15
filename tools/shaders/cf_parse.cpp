// Xenos control-flow parsing (layout per Xenia's ucode.h).
//
// Control-flow instructions are 48 bits wide, packed two per 3-dword slot at
// the start of a program. Exec-type instructions run `count` ALU/fetch slots
// starting at slot `address`; the first such address marks where control flow
// ends. alloc instructions reveal the stage: only vertex shaders allocate a
// position export.
#include "ucode.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace xgpu::ucode {
namespace {

constexpr uint32_t kMaxSlots = 4096;

bool IsExec(uint32_t op) {
  return op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 13 || op == 14;
}
bool IsExecEnd(uint32_t op) { return op == 2 || op == 4 || op == 6 || op == 14; }
bool IsValidFetchOpcode(uint32_t op) {
  return op == 0 || op == 1 || (op >= 16 && op <= 19) || (op >= 24 && op <= 26);
}

}  // namespace

template <typename Load>
std::optional<ProgramInfo> Parse(Load load, size_t word_count) {
  uint32_t cf_end = UINT32_MAX;  // first slot holding ALU/fetch instructions
  uint32_t end_slot = 0;
  bool seen_end = false;
  bool alloc_position = false, alloc_other = false;
  struct Clause { uint32_t addr, count, sequence; };
  std::vector<Clause> clauses;
  for (uint32_t slot = 0; slot < cf_end; ++slot) {
    if (slot >= kMaxSlots || size_t(slot + 1) * 3 > word_count) {
      return std::nullopt;
    }
    const uint32_t w0 = load(slot * 3), w1 = load(slot * 3 + 1), w2 = load(slot * 3 + 2);
    const uint32_t instr[2][2] = {{w0, w1 & 0xFFFF}, {(w1 >> 16) | (w2 << 16), w2 >> 16}};
    for (const auto& cf : instr) {
      const uint32_t d0 = cf[0], d1 = cf[1];
      const uint32_t op = (d1 >> 12) & 0xF;
      if (IsExec(op)) {
        const uint32_t addr = d0 & 0xFFF, count = (d0 >> 12) & 0x7;
        if ((count == 0 && !IsExecEnd(op)) || addr == 0 || addr > kMaxSlots) {
          return std::nullopt;
        }
        cf_end = std::min(cf_end, addr);
        end_slot = std::max(end_slot, addr + count);
        if (count) clauses.push_back({addr, count, (d0 >> 16) & 0xFFF});
        seen_end |= IsExecEnd(op);
      } else if (op == 12) {  // alloc
        const uint32_t type = (d1 >> 9) & 0x3;
        (type == 1 ? alloc_position : alloc_other) = true;
      }
    }
    if (seen_end && cf_end == UINT32_MAX) {
      return std::nullopt;
    }
  }
  if (!seen_end || cf_end == UINT32_MAX || end_slot <= cf_end) {
    return std::nullopt;
  }
  // Real programs lay their exec clauses out back to back: sorted by address
  // they tile [cf_end, end_slot) exactly. Random data practically never does.
  std::sort(clauses.begin(), clauses.end(),
            [](const Clause& a, const Clause& b) { return a.addr < b.addr; });
  uint32_t next = cf_end;
  for (const Clause& c : clauses) {
    if (c.addr == next) {
      next = c.addr + c.count;
    } else if (c.addr + c.count > next || c.addr < cf_end) {
      // Not back to back (a duplicate clause, e.g. from a loop, is fine only
      // if it re-runs an already-covered range).
      return std::nullopt;
    }
  }
  if (next != end_slot) {
    return std::nullopt;
  }
  // Every instruction slot must decode: sequence bit 0 marks fetch slots.
  if (size_t(end_slot) * 3 > word_count) {
    return std::nullopt;
  }
  uint64_t export_targets = 0;  // bit per export register index written
  for (const Clause& c : clauses) {
    for (uint32_t i = 0; i < c.count; ++i) {
      const uint32_t slot = c.addr + i;
      const uint32_t w0 = load(slot * 3), w2 = load(slot * 3 + 2);
      if ((c.sequence >> (i * 2)) & 1) {
        if (!IsValidFetchOpcode(w0 & 0x1F)) return std::nullopt;
      } else {
        const uint32_t scalar = w0 >> 26, vector = (w2 >> 24) & 0x1F;
        if (scalar > 50 || scalar == 41 || vector > 29) return std::nullopt;
        if ((w0 >> 15) & 1) {
          export_targets |= uint64_t(1) << (w0 & 0x3F);  // vector_dest
        }
      }
    }
  }
  ProgramInfo info;
  info.dword_count = (end_slot + 1) * 3;
  if (info.dword_count > word_count) {
    return std::nullopt;
  }
  info.stage = alloc_position ? Stage::kVertex : (alloc_other ? Stage::kPixel : Stage::kUnknown);
  // Exports must target registers the stage has: memexport (32-37) for both,
  // interpolators 0-15 / position 62 / point size 63 for vertex shaders,
  // colors 0-3 / depth 61 for pixel shaders.
  const uint64_t memexport = uint64_t(0x3F) << 32;
  const uint64_t allowed = info.stage == Stage::kVertex
                               ? memexport | 0xFFFFull | (uint64_t(3) << 62)
                               : memexport | 0xFull | (uint64_t(1) << 61);
  if (export_targets & ~allowed) {
    return std::nullopt;
  }
  return info;
}

std::optional<ProgramInfo> ParseProgram(const uint32_t* words, size_t word_count) {
  return Parse([words](size_t i) { return words[i]; }, word_count);
}

std::optional<ProgramInfo> ParseProgramBigEndian(const uint8_t* data, size_t word_count) {
  return Parse(
      [data](size_t i) {
        const uint8_t* p = data + i * 4;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      },
      word_count);
}

}  // namespace xgpu::ucode
