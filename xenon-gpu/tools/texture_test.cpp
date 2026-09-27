// xgpu-texture-test: loads every texture a captured frame's draws reference
// through the native TextureCache (GPU upload) and dumps CPU-decoded base
// levels as PPM for visual checks.
//
// Usage: xgpu-texture-test <capture dir> <out dir> [max dumps]
#include <xgpu/capture.h>
#include <xgpu/pm4.h>
#include <xgpu/render_targets.h>
#include <xgpu/textures.h>
#include <xgpu/vk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>

using namespace xgpu;
using namespace rex::graphics;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <capture dir> <out dir> [max dumps]\n", argv[0]);
    return 1;
  }
  const uint32_t max_dumps = argc > 3 ? uint32_t(std::atoi(argv[3])) : 40;
  Capture capture;
  if (!capture.Load(argv[1])) {
    std::fprintf(stderr, "failed to load capture\n");
    return 1;
  }

  // Collect the distinct texture fetch constants referenced by the frame's
  // draws (all 32 slots of every draw's register file).
  std::map<std::vector<uint32_t>, uint32_t> fetches;  // dwords -> first draw
  auto collect = [&](const uint32_t* regs, uint32_t draw) {
    for (uint32_t i = 0; i < 32; ++i) {
      const uint32_t* f = regs + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i * 6;
      if ((f[0] & 3) != uint32_t(xenos::FetchConstantType::kTexture)) continue;
      fetches.emplace(std::vector<uint32_t>(f, f + 6), draw);
    }
  };
  collect(capture.registers().data(), 0);
  pm4::FrontEnd front_end(capture.memory_end(), capture.registers());
  if (!capture.reads().empty()) front_end.UseRecordedReads(&capture.reads());
  front_end.on_draw = [&](const pm4::Draw& draw) {
    if (!draw.is_resolve) collect(draw.registers, draw.index);
  };
  for (const RingSegment& segment : capture.ring()) front_end.ExecuteRingSegment(segment);
  std::printf("%zu distinct texture fetch constants\n", fetches.size());

  xgpu::vk::Context context;
  std::string error;
  if (!context.Initialize(&error)) {
    std::fprintf(stderr, "vulkan: %s\n", error.c_str());
    return 1;
  }
  RenderTargets render_targets(context);
  auto cache = std::make_unique<TextureCache>(context, capture.memory_end(), render_targets);
  auto regs = std::make_unique<RegisterFile>();
  std::memset(regs->values, 0, sizeof(regs->values));

  // GPU path: load every fetch constant through GetView/GetSampler.
  std::map<std::string, uint32_t> format_counts;
  uint32_t null_views = 0;
  bool ok = context.Submit([&](VkCommandBuffer cmd) {
    for (const auto& [dwords, draw] : fetches) {
      std::memcpy(&regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], dwords.data(), 24);
      xenos::xe_gpu_texture_fetch_t fetch = regs->GetTextureFetch(0);
      format_counts[FormatInfo::Get(GetBaseFormat(fetch.format))->name]++;
      SpirvShader::TextureBinding binding = {};
      binding.fetch_constant = 0;
      switch (fetch.dimension) {
        case xenos::DataDimension::k3D:
          binding.dimension = xenos::FetchOpDimension::k3DOrStacked;
          break;
        case xenos::DataDimension::kCube:
          binding.dimension = xenos::FetchOpDimension::kCube;
          break;
        default:
          binding.dimension = xenos::FetchOpDimension::k2D;
          break;
      }
      for (uint32_t is_signed = 0; is_signed < 2; ++is_signed) {
        binding.is_signed = is_signed;
        if (!cache->GetView(cmd, *regs, binding)) ++null_views;
      }
      SpirvShader::SamplerBinding sampler = {};
      sampler.fetch_constant = 0;
      sampler.mag_filter = sampler.min_filter = sampler.mip_filter =
          xenos::TextureFilter::kUseFetchConst;
      sampler.aniso_filter = xenos::AnisoFilter::kUseFetchConst;
      if (!cache->GetSampler(*regs, sampler)) std::printf("null sampler!\n");
      SpirvShaderTranslator::SystemConstants constants = {};
      cache->WriteSystemConstants(*regs, 1, constants);
    }
  });
  cache->EndFrame();
  std::printf("GPU load: submit %s, loaded %u, unsupported %u, resolved hits %u, VK_NULL views %u\n",
              ok ? "ok" : "FAILED", cache->loaded_count(), cache->unsupported_count(),
              cache->resolved_hits(), null_views);
  for (const auto& [name, count] : format_counts) std::printf("  %-24s %u\n", name.c_str(), count);

  // CPU decode dumps: prefer larger textures, one per (format, tiling) first.
  std::vector<std::pair<uint64_t, std::vector<uint32_t>>> order;
  for (const auto& [dwords, draw] : fetches) {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, dwords.data(), 24);
    uint64_t area = uint64_t(fetch.size_2d.width + 1) * (fetch.size_2d.height + 1);
    order.push_back({area, dwords});
  }
  std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::set<std::pair<uint32_t, uint32_t>> seen;
  std::vector<std::vector<uint32_t>> picks;
  for (int pass = 0; pass < 2 && picks.size() < max_dumps; ++pass) {
    for (auto& [area, dwords] : order) {
      if (picks.size() >= max_dumps) break;
      xenos::xe_gpu_texture_fetch_t fetch;
      std::memcpy(&fetch, dwords.data(), 24);
      auto sig = std::make_pair(uint32_t(GetBaseFormat(fetch.format)),
                                uint32_t(fetch.tiled) | (uint32_t(fetch.dimension) << 1));
      bool first = seen.insert(sig).second;
      if (pass == 0 ? first : std::find(picks.begin(), picks.end(), dwords) == picks.end()) {
        picks.push_back(dwords);
      }
    }
  }
  uint32_t index = 0;
  for (const auto& dwords : picks) {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, dwords.data(), 24);
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::string description;
    bool decoded = cache->DebugDecode(fetch, w, h, rgba, &description);
    std::printf("[%02u] %s -> %s\n", index, description.c_str(), decoded ? "decoded" : "skipped");
    if (!decoded) {
      ++index;
      continue;
    }
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/tex_%02u_%s_%ux%u.ppm", argv[2], index,
                  FormatInfo::Get(GetBaseFormat(fetch.format))->name, w, h);
    FILE* file = std::fopen(path, "wb");
    if (file) {
      std::fprintf(file, "P6\n%u %u\n255\n", w, h);
      for (size_t i = 0; i < size_t(w) * h; ++i) std::fwrite(&rgba[i * 4], 1, 3, file);
      std::fclose(file);
    }
    ++index;
  }
  // All mip levels of a few mipmapped (packed) textures of different formats.
  std::set<uint32_t> mip_formats;
  for (const auto& dwords : picks) {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, dwords.data(), 24);
    uint32_t format = uint32_t(GetBaseFormat(fetch.format));
    if (fetch.mip_max_level == 0 || !fetch.mip_address || !mip_formats.insert(format).second) {
      continue;
    }
    for (uint32_t level = 0; level <= fetch.mip_max_level; ++level) {
      uint32_t w = 0, h = 0;
      std::vector<uint8_t> rgba;
      if (!cache->DebugDecode(fetch, w, h, rgba, nullptr, level)) continue;
      char path[1024];
      std::snprintf(path, sizeof(path), "%s/mip_%s_%08X_L%02u_%ux%u.ppm", argv[2],
                    FormatInfo::Get(GetBaseFormat(fetch.format))->name,
                    uint32_t(fetch.base_address) << 12, level, w, h);
      FILE* file = std::fopen(path, "wb");
      if (!file) continue;
      std::fprintf(file, "P6\n%u %u\n255\n", w, h);
      for (size_t i = 0; i < size_t(w) * h; ++i) std::fwrite(&rgba[i * 4], 1, 3, file);
      std::fclose(file);
    }
  }
  return ok ? 0 : 1;
}
