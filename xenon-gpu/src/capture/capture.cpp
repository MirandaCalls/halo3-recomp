#include <xgpu/capture.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace xgpu {
namespace {

const uint8_t* MapFile(const std::string& path, size_t expected_size) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return nullptr;
  struct stat st {};
  if (fstat(fd, &st) || size_t(st.st_size) != expected_size) {
    close(fd);
    return nullptr;
  }
  void* data = mmap(nullptr, expected_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  return data == MAP_FAILED ? nullptr : static_cast<const uint8_t*>(data);
}

}  // namespace

Capture::~Capture() {
  if (memory_start_) munmap(const_cast<uint8_t*>(memory_start_), kPhysicalMemorySize);
  if (memory_end_) munmap(const_cast<uint8_t*>(memory_end_), kPhysicalMemorySize);
}

bool Capture::Load(const std::string& directory) {
  memory_start_ = MapFile(directory + "/memory_start.bin", kPhysicalMemorySize);
  memory_end_ = MapFile(directory + "/memory_end.bin", kPhysicalMemorySize);
  if (!memory_start_ || !memory_end_) return false;

  registers_.assign(kRegisterCount, 0);
  FILE* f = std::fopen((directory + "/registers.bin").c_str(), "rb");
  if (!f || std::fread(registers_.data(), 4, kRegisterCount, f) != kRegisterCount) {
    if (f) std::fclose(f);
    return false;
  }
  std::fclose(f);

  f = std::fopen((directory + "/ring.bin").c_str(), "rb");
  if (!f) return false;
  uint32_t header[5];
  while (std::fread(header, sizeof(header), 1, f) == 1) {
    RingSegment segment{header[0], header[1], header[2], header[3], {}};
    segment.dwords.resize(header[4]);
    if (header[4] && std::fread(segment.dwords.data(), 4, header[4], f) != header[4]) break;
    ring_.push_back(std::move(segment));
  }
  std::fclose(f);

  // Optional (older captures lack it): exact contents of what was read.
  if ((f = std::fopen((directory + "/reads.bin").c_str(), "rb"))) {
    uint32_t read_header[3];
    while (std::fread(read_header, sizeof(read_header), 1, f) == 1) {
      RecordedRead read{read_header[0], read_header[1], std::vector<uint32_t>(read_header[2])};
      if (read_header[2] && std::fread(read.dwords.data(), 4, read_header[2], f) != read_header[2]) break;
      reads_.push_back(std::move(read));
    }
    std::fclose(f);
  }
  return true;
}

}  // namespace xgpu
