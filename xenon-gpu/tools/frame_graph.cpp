// xgpu-frame: print the render graph of a captured GPU frame.
//
// Replays the capture's command stream through the PM4 front end and groups
// draws into passes (runs with the same EDRAM render target configuration),
// listing each resolve (EDRAM -> memory copy) with its source and destination.
//
// Usage: xgpu-frame <capture dir> [--memory start|end]
#include <xgpu/capture.h>
#include <xgpu/pm4.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace xgpu;

namespace {

const char* kColorFormats[16] = {"8888",      "8888_GAMMA", "2_10_10_10", "2_10_10_10_F",
                                 "16_16",     "16x4",       "16_16_F",    "16x4_F",
                                 "?8",        "?9",         "2_10x3_AS10", "?11",
                                 "2_10F_AS16", "?13",       "32_F",       "32_32_F"};

struct RtConfig {
  uint32_t surface_info, color_info[4], depth_info, mode;
  uint32_t color_mask;  // RB_COLOR_MASK: which targets are written
  bool operator==(const RtConfig& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};

RtConfig ReadConfig(const uint32_t* r) {
  RtConfig c{};
  c.surface_info = r[pm4::reg::RB_SURFACE_INFO];
  c.color_info[0] = r[pm4::reg::RB_COLOR_INFO];
  c.color_info[1] = r[pm4::reg::RB_COLOR1_INFO];
  c.color_info[2] = r[pm4::reg::RB_COLOR2_INFO];
  c.color_info[3] = r[pm4::reg::RB_COLOR3_INFO];
  c.depth_info = r[pm4::reg::RB_DEPTH_INFO];
  c.mode = r[pm4::reg::RB_MODECONTROL] & 7;
  c.color_mask = r[0x2104];  // RB_COLOR_MASK
  return c;
}

std::string Describe(const RtConfig& c) {
  char buf[256];
  int n = std::snprintf(buf, sizeof(buf), "pitch %u msaa %ux", c.surface_info & 0x3FFF,
                        1u << ((c.surface_info >> 16) & 3));
  std::string s(buf, n);
  for (int i = 0; i < 4; ++i) {
    if (!((c.color_mask >> (i * 4)) & 0xF)) continue;
    n = std::snprintf(buf, sizeof(buf), " | c%d @%u %s", i, c.color_info[i] & 0xFFF,
                      kColorFormats[(c.color_info[i] >> 16) & 0xF]);
    s.append(buf, n);
  }
  if (c.mode == 4 || c.mode == 5) {
    n = std::snprintf(buf, sizeof(buf), " | depth @%u %s%s", c.depth_info & 0xFFF,
                      ((c.depth_info >> 16) & 1) ? "D24FS8" : "D24S8", c.mode == 5 ? " (depth only)" : "");
    s.append(buf, n);
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <capture dir> [--memory start|end]\n", argv[0]);
    return 1;
  }
  bool use_start = argc >= 4 && !std::strcmp(argv[2], "--memory") && !std::strcmp(argv[3], "start");
  Capture capture;
  if (!capture.Load(argv[1])) {
    std::fprintf(stderr, "failed to load capture %s\n", argv[1]);
    return 1;
  }
  pm4::FrontEnd front_end(use_start ? capture.memory_start() : capture.memory_end(),
                          capture.registers());
  if (!capture.reads().empty()) front_end.UseRecordedReads(&capture.reads());

  RtConfig current{};
  bool have_pass = false;
  uint32_t pass_draws = 0, pass_first = 0, passes = 0;
  auto flush_pass = [&]() {
    if (have_pass && pass_draws) {
      std::printf("pass %3u  draws %4u-%-4u (%3u)  %s\n", passes++, pass_first,
                  pass_first + pass_draws - 1, pass_draws, Describe(current).c_str());
    }
    pass_draws = 0;
  };
  front_end.on_draw = [&](const pm4::Draw& draw) {
    const uint32_t* r = draw.registers;
    if (draw.is_resolve) {
      flush_pass();
      const uint32_t copy_control = r[pm4::reg::RB_COPY_CONTROL];
      const uint32_t dest_info = r[pm4::reg::RB_COPY_DEST_INFO];
      const uint32_t dest_pitch = r[pm4::reg::RB_COPY_DEST_PITCH];
      const uint32_t src = copy_control & 7;
      const uint32_t src_info = src == 4 ? r[pm4::reg::RB_DEPTH_INFO]
                                         : r[pm4::reg::RB_COLOR_INFO + (src ? src + 1 : 0)];
      std::printf("  resolve %s @%u -> 0x%08X %ux%u fmt %u%s%s\n",
                  src == 4 ? "depth" : ("c" + std::to_string(src)).c_str(), src_info & 0xFFF,
                  r[pm4::reg::RB_COPY_DEST_BASE], dest_pitch & 0x3FFF, (dest_pitch >> 16) & 0x3FFF,
                  (dest_info >> 7) & 0x3F, (copy_control >> 8) & 1 ? " +clear color" : "",
                  (copy_control >> 9) & 1 ? " +clear depth" : "");
      have_pass = false;
      return;
    }
    RtConfig config = ReadConfig(r);
    if (!have_pass || !(config == current)) {
      flush_pass();
      current = config;
      have_pass = true;
      pass_first = draw.index;
    }
    ++pass_draws;
  };

  for (const RingSegment& segment : capture.ring()) front_end.ExecuteRingSegment(segment);
  flush_pass();

  const pm4::Stats& s = front_end.stats();
  std::printf("\n%llu draws, %llu resolves, %u passes, %llu indirect buffers, %llu register writes, "
              "%llu predicated packets skipped\n",
              (unsigned long long)s.draws, (unsigned long long)s.resolves, passes,
              (unsigned long long)s.indirect_buffers, (unsigned long long)s.register_writes,
              (unsigned long long)s.predicated_skipped);
  std::printf("recorded reads: %zu captured, %llu used, %llu mismatches\n", capture.reads().size(),
              (unsigned long long)s.recorded_reads_used,
              (unsigned long long)s.recorded_read_mismatches);
  return 0;
}
