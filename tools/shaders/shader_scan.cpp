// xgpu-shader-scan: find every Xenos shader program in arbitrary game files.
//
// Scans each file at 4-byte granularity for a plausible Xenos control-flow
// program (see src/ucode/cf_parse.cpp) and writes the unique programs as a
// Xenia/ReXGlue shader storage file (.xsh), hashed the same way the runtime
// hashes them (XXH3-64 over the microcode as stored in guest memory), plus a
// CSV index of where each one was found.
//
// Usage: xgpu-shader-scan --out corpus.xsh --index corpus.csv FILE...
#include "ucode.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Found {
  std::vector<uint8_t> ucode;  // guest (big-endian) bytes
  xgpu::ucode::Stage stage;
  std::string file;
  uint64_t offset;
};

uint32_t LoadBe(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_path, index_path;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
      out_path = argv[++i];
    } else if (!std::strcmp(argv[i], "--index") && i + 1 < argc) {
      index_path = argv[++i];
    } else {
      files.push_back(argv[i]);
    }
  }
  if (out_path.empty() || files.empty()) {
    std::fprintf(stderr, "usage: %s --out corpus.xsh [--index corpus.csv] FILE...\n", argv[0]);
    return 1;
  }

  std::unordered_map<uint64_t, Found> found;
  size_t total_hits = 0;
  for (const std::string& path : files) {
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) || st.st_size < 12) {
      if (fd >= 0) close(fd);
      continue;
    }
    size_t size = size_t(st.st_size);
    auto* data = static_cast<const uint8_t*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (data == MAP_FAILED) continue;
    madvise(const_cast<uint8_t*>(data), size, MADV_SEQUENTIAL);
    size_t file_hits = 0;
    for (size_t off = 0; off + 12 <= size;) {
      // Cheap prefilter: the first control-flow instruction must be an alloc,
      // exec or nop; most offsets fail here.
      const uint32_t op_a = (LoadBe(data + off + 4) >> 12) & 0xF;
      if (op_a != 12 && op_a != 1 && op_a != 2 && op_a != 0) {
        off += 4;
        continue;
      }
      auto info = xgpu::ucode::ParseProgramBigEndian(data + off, (size - off) / 4);
      if (!info || info->stage == xgpu::ucode::Stage::kUnknown) {
        off += 4;
        continue;
      }
      const size_t bytes = size_t(info->dword_count) * 4;
      uint64_t hash = XXH3_64bits(data + off, bytes);
      if (!found.count(hash)) {
        found[hash] = Found{std::vector<uint8_t>(data + off, data + off + bytes), info->stage, path, off};
      }
      ++file_hits;
      off += 4;
    }
    munmap(const_cast<uint8_t*>(data), size);
    total_hits += file_hits;
    std::fprintf(stderr, "%s: %zu programs\n", path.c_str(), file_hits);
  }

  FILE* out = std::fopen(out_path.c_str(), "wb");
  if (!out) {
    std::perror(out_path.c_str());
    return 1;
  }
  const uint32_t header[2] = {0x48534558u, __builtin_bswap32(0x20201219u)};  // 'XESH', version
  std::fwrite(header, sizeof(header), 1, out);
  FILE* index = index_path.empty() ? nullptr : std::fopen(index_path.c_str(), "w");
  if (index) std::fprintf(index, "hash,stage,dwords,file,offset\n");
  for (const auto& [hash, f] : found) {
    const uint32_t bits = uint32_t(f.ucode.size() / 4) | (f.stage == xgpu::ucode::Stage::kPixel ? 0x80000000u : 0u);
    std::fwrite(&hash, sizeof(hash), 1, out);
    std::fwrite(&bits, sizeof(bits), 1, out);
    std::fwrite(f.ucode.data(), f.ucode.size(), 1, out);
    if (index) {
      std::fprintf(index, "%016llX,%s,%zu,%s,%llu\n", (unsigned long long)hash,
                   f.stage == xgpu::ucode::Stage::kPixel ? "ps" : "vs", f.ucode.size() / 4,
                   f.file.c_str(), (unsigned long long)f.offset);
    }
  }
  std::fclose(out);
  if (index) std::fclose(index);
  std::fprintf(stderr, "%zu program hits, %zu unique -> %s\n", total_hits, found.size(), out_path.c_str());
  return 0;
}
