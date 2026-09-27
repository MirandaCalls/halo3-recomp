// xgpu-replay: replay a captured GPU frame on the native renderer (headless).
//
// Milestone 2 skeleton: render targets are native Vulkan images, resolves are
// image copies into resolved textures, clears are image clears. Draws create
// the render targets they use but are not rasterized yet. The front buffer
// named by the frame's XE_SWAP is written as a PPM image.
//
// Usage: xgpu-replay <capture dir> <out.ppm>
#include <xgpu/capture.h>
#include <xgpu/pm4.h>
#include <xgpu/render_targets.h>
#include <xgpu/renderer.h>
#include <xgpu/textures.h>
#include <xgpu/vk.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

using namespace xgpu;

namespace {

RenderTargetKey ColorKey(const uint32_t* r, uint32_t index) {
  static const uint32_t kInfo[4] = {pm4::reg::RB_COLOR_INFO, pm4::reg::RB_COLOR1_INFO,
                                    pm4::reg::RB_COLOR2_INFO, pm4::reg::RB_COLOR3_INFO};
  const uint32_t info = r[kInfo[index]];
  RenderTargetKey key;
  key.base_tiles = info & 0xFFF;
  key.format = (info >> 16) & 0xF;
  key.pitch = r[pm4::reg::RB_SURFACE_INFO] & 0x3FFF;
  key.msaa = (r[pm4::reg::RB_SURFACE_INFO] >> 16) & 3;
  return key;
}

RenderTargetKey DepthKey(const uint32_t* r) {
  const uint32_t info = r[pm4::reg::RB_DEPTH_INFO];
  RenderTargetKey key;
  key.depth = true;
  key.base_tiles = info & 0xFFF;
  key.format = (info >> 16) & 1;
  key.pitch = r[pm4::reg::RB_SURFACE_INFO] & 0x3FFF;
  key.msaa = (r[pm4::reg::RB_SURFACE_INFO] >> 16) & 3;
  return key;
}

// Rows a target can have: bounded by the scissor and by what fits in the
// 2048 EDRAM tiles (80x16 samples each) after its base.
uint32_t TargetHeight(const uint32_t* r, const RenderTargetKey& key) {
  uint32_t scissor = (r[0x2082] >> 16) & 0x7FFF;  // PA_SC_WINDOW_SCISSOR_BR
  uint32_t samples_x = key.msaa >= 2 ? 2 : 1, samples_y = key.msaa >= 1 ? 2 : 1;
  uint32_t bpp_factor = (!key.depth && (key.format == 5 || key.format == 7 || key.format == 15)) ? 2 : 1;
  uint32_t tiles_per_row = ((key.pitch * samples_x + 79) / 80) * bpp_factor;
  if (!tiles_per_row || key.base_tiles >= 2048) return scissor;
  uint32_t edram_rows = (2048 - key.base_tiles) / tiles_per_row * 16 / samples_y;
  return std::min(scissor, edram_rows);
}

// Tracks which render target last wrote each EDRAM tile, to count handoffs
// where the same EDRAM is used through a different target (format / pitch)
// without a clear in between - data the native model must convert.
struct EdramOwnership {
  std::vector<int32_t> owner = std::vector<int32_t>(2048, -1);
  std::map<RenderTargetKey, int32_t> ids;
  uint32_t handoffs = 0, draws_with_handoff = 0;
  int32_t Id(const RenderTargetKey& key) {
    auto it = ids.find(key);
    return it != ids.end() ? it->second : (ids[key] = int32_t(ids.size()));
  }
  // Marks tiles [base, base + count) as owned by key; returns handoffs.
  uint32_t Use(const RenderTargetKey& key, uint32_t tiles, bool clears) {
    const int32_t id = Id(key);
    uint32_t found = 0;
    for (uint32_t t = key.base_tiles; t < std::min<uint32_t>(2048, key.base_tiles + tiles); ++t) {
      if (!clears && owner[t] >= 0 && owner[t] != id) ++found;
      owner[t] = id;
    }
    return found;
  }
};

uint32_t TargetTiles(const RenderTargetKey& key, uint32_t height) {
  uint32_t samples_x = key.msaa >= 2 ? 2 : 1, samples_y = key.msaa >= 1 ? 2 : 1;
  uint32_t bpp_factor = (!key.depth && (key.format == 5 || key.format == 7 || key.format == 15)) ? 2 : 1;
  return ((key.pitch * samples_x + 79) / 80) * bpp_factor * ((height * samples_y + 15) / 16);
}

bool WriteImage(xgpu::vk::Context& context, const xgpu::vk::Image& source, const char* path) {
  xgpu::vk::Image rgba;
  if (!context.CreateImage(rgba, VK_FORMAT_R8G8B8A8_UNORM, source.width, source.height,
                           VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    return false;
  }
  xgpu::vk::Buffer staging;
  context.CreateBuffer(staging, VkDeviceSize(source.width) * source.height * 4,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  xgpu::vk::Image src = source;  // layout tracking copy
  context.Submit([&](VkCommandBuffer cmd) {
    xgpu::vk::Context::Transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    xgpu::vk::Context::Transition(cmd, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(cmd, src.image, src.layout, rgba.image, rgba.layout, 1, &blit, VK_FILTER_NEAREST);
    xgpu::vk::Context::Transition(cmd, rgba, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {source.width, source.height, 1};
    vkCmdCopyImageToBuffer(cmd, rgba.image, rgba.layout, staging.buffer, 1, &copy);
  });
  // Binary PPM (RGB).
  bool ok = false;
  if (FILE* f = std::fopen(path, "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", source.width, source.height);
    const uint8_t* pixels = static_cast<const uint8_t*>(staging.mapped);
    // XGPU_ALPHA: write alpha instead of blue, to inspect it.
    static const bool alpha = std::getenv("XGPU_ALPHA") != nullptr;
    for (uint32_t i = 0; i < source.width * source.height; ++i) {
      uint8_t rgb[3] = {pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + (alpha ? 3 : 2)]};
      std::fwrite(rgb, 1, 3, f);
    }
    ok = std::fclose(f) == 0;
  }
  context.DestroyBuffer(staging);
  context.DestroyImage(rgba);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <capture dir> <out.ppm>\n", argv[0]);
    return 1;
  }
  Capture capture;
  if (!capture.Load(argv[1])) {
    std::fprintf(stderr, "failed to load capture %s\n", argv[1]);
    return 1;
  }
  xgpu::vk::Context context;
  std::string error;
  if (!context.Initialize(&error)) {
    std::fprintf(stderr, "Vulkan: %s\n", error.c_str());
    return 1;
  }
  std::printf("device: %s\n", context.device_name().c_str());

  RenderTargets render_targets(context);
  if (!render_targets.Initialize(&error)) {
    std::fprintf(stderr, "render targets: %s\n", error.c_str());
    return 1;
  }
  TextureCache textures(context, capture.memory_end(), render_targets);
  Renderer renderer(context, capture.memory_end(), render_targets, textures);
  if (!renderer.Initialize(&error)) {
    std::fprintf(stderr, "renderer: %s\n", error.c_str());
    return 1;
  }
  // Textures load from the renderer's copy of guest memory on the GPU, which
  // resolves also write (captures have no CPU writes during replay).
  textures.SetSharedMemory(renderer.shared_memory_buffer(), nullptr);
  render_targets.SetSharedMemoryBuffer(renderer.shared_memory_buffer());
  pm4::FrontEnd front_end(capture.memory_end(), capture.registers());
  if (!capture.reads().empty()) front_end.UseRecordedReads(&capture.reads());

  uint32_t frontbuffer = 0, fb_width = 0, fb_height = 0, depth_resolves = 0;
  EdramOwnership ownership;
  std::map<std::pair<RenderTargetKey, RenderTargetKey>, uint32_t> handoff_pairs;
  context.Submit([&](VkCommandBuffer cmd) {
    // Debug: XGPU_STOP_AFTER=N ignores draws and resolves after draw N.
    const char* stop_env = std::getenv("XGPU_STOP_AFTER");
    const uint32_t stop_after = stop_env ? uint32_t(std::strtoul(stop_env, nullptr, 0)) : UINT32_MAX;
    front_end.on_draw = [&](const pm4::Draw& draw) {
      if (draw.index > stop_after) return;
      const uint32_t* r = draw.registers;
      if (draw.is_resolve) {
        renderer.EndRendering(cmd);
        ResolveInfo info = renderer.GetResolveInfo(r);
        if (info.source.depth) ++depth_resolves;
        if (std::getenv("XGPU_TRACE")) {
          std::printf("resolve %4u src %s -> %08X %ux%u fmt %u clear c%u d%u rect %d,%d-%d,%d +%d bias %d "
                      "swap %u\n",
                      draw.index, info.source.Describe().c_str(), info.dest_address, info.dest_width,
                      info.dest_height, info.dest_format, info.clear_color, info.clear_depth, info.rect_x0,
                      info.rect_y0, info.rect_x1, info.rect_y1, info.dest_offset_x, info.dest_exp_bias,
                      info.dest_swap);
        }
        render_targets.Resolve(cmd, info);
        if (info.memory_write) textures.InvalidateRange(info.extent_start, info.extent_length);
        // A resolve with clear re-initializes the source EDRAM.
        if (info.clear_color || info.clear_depth) {
          RenderTargetKey cleared = info.clear_depth && !info.source.depth ? info.depth_target : info.source;
          ownership.Use(cleared, TargetTiles(cleared, info.dest_height), true);
          if (info.clear_color && !info.source.depth) {
            ownership.Use(info.source, TargetTiles(info.source, info.dest_height), true);
          }
        }
        return;
      }
      // Make sure every render target this draw uses exists.
      const uint32_t mode = r[pm4::reg::RB_MODECONTROL] & 7;
      uint32_t handoffs = 0;
      auto use = [&](const RenderTargetKey& key) {
        const uint32_t height = TargetHeight(r, key);
        render_targets.Get(key, height);
        // Only the previous owner of the first tile is reported per pair.
        const int32_t previous = ownership.owner[std::min<uint32_t>(key.base_tiles, 2047)];
        uint32_t found = ownership.Use(key, TargetTiles(key, height), false);
        if (found && previous >= 0) {
          for (const auto& [other, id] : ownership.ids) {
            if (id == previous) ++handoff_pairs[{other, key}];
          }
        }
        handoffs += found;
      };
      if (mode == 4) {
        for (uint32_t i = 0; i < 4; ++i) {
          if ((r[0x2104] >> (i * 4)) & 0xF) use(ColorKey(r, i));  // RB_COLOR_MASK
        }
      }
      if (mode == 4 || mode == 5) use(DepthKey(r));
      if (handoffs) {
        ++ownership.draws_with_handoff;
        ownership.handoffs += handoffs;
      }
      renderer.Draw(cmd, draw);
    };
    front_end.on_shader_load = [&](const pm4::ShaderLoad& load) { renderer.OnShaderLoad(load); };
    front_end.on_swap = [&](uint32_t address, uint32_t width, uint32_t height) {
      frontbuffer = address;
      fb_width = width;
      fb_height = height;
    };
    for (const RingSegment& segment : capture.ring()) front_end.ExecuteRingSegment(segment);
    // Debug: XGPU_REPEAT=N replays the frame N more times on the same render
    // targets (frame-to-frame state: accumulation, feedback, clears).
    if (const char* repeat = std::getenv("XGPU_REPEAT")) {
      for (int i = std::atoi(repeat); i > 0; --i) {
        pm4::FrontEnd again(capture.memory_end(), capture.registers());
        if (!capture.reads().empty()) again.UseRecordedReads(&capture.reads());
        again.on_draw = front_end.on_draw;
        again.on_shader_load = front_end.on_shader_load;
        again.on_swap = front_end.on_swap;
        for (const RingSegment& segment : capture.ring()) again.ExecuteRingSegment(segment);
      }
    }
    renderer.EndRendering(cmd);
  });
  render_targets.EndFrame();

  const Renderer::Stats& rs = renderer.stats();
  std::printf("renderer: %u drawn in %u passes, %u pipelines, %u shaders; skipped: %u no raster, %u no "
              "shader, %u translation, %u primitive, %u no target, %u pipeline\n",
              rs.drawn, rs.passes, rs.pipelines, rs.shaders, rs.skipped_no_raster, rs.skipped_no_shader,
              rs.skipped_translation, rs.skipped_primitive, rs.skipped_no_target, rs.skipped_pipeline);
  std::printf("textures: %u loaded, %u resolved hits, %u unsupported\n", textures.loaded_count(),
              textures.resolved_hits(), textures.unsupported_count());
  const pm4::Stats& s = front_end.stats();
  std::printf("%llu draws, %llu resolves (%u depth, not copied yet); %zu native render targets, %zu "
              "resolved textures\n",
              (unsigned long long)s.draws, (unsigned long long)s.resolves, depth_resolves,
              render_targets.render_target_count(), render_targets.resolved_count());
  for (const auto& [key, image] : render_targets.targets()) {
    std::printf("  RT %-40s -> %ux%u\n", key.Describe().c_str(), image.width, image.height);
  }
  std::printf("EDRAM handoffs: %u draws start using EDRAM last written through another target\n",
              ownership.draws_with_handoff);
  for (const auto& [pair, n] : handoff_pairs) {
    std::printf("  %-36s -> %-36s x%u\n", pair.first.Describe().c_str(), pair.second.Describe().c_str(), n);
  }
  if (const char* dump = std::getenv("XGPU_DUMP")) {
    // Debug: every single-sampled color render target and resolved texture.
    char path[512];
    for (const auto& [key, image] : render_targets.targets()) {
      if (key.depth || image.samples != VK_SAMPLE_COUNT_1_BIT) continue;
      std::snprintf(path, sizeof(path), "%s/rt_%04u_p%u_f%u.ppm", dump, key.base_tiles, key.pitch, key.format);
      WriteImage(context, image, path);
    }
    for (const auto& [address, image] : render_targets.resolved()) {
      std::snprintf(path, sizeof(path), "%s/resolve_%08X.ppm", dump, address);
      WriteImage(context, image, path);
    }
  }
  std::printf("front buffer 0x%08X %ux%u\n", frontbuffer, fb_width, fb_height);
  const xgpu::vk::Image* output = render_targets.FindResolved(frontbuffer);
  if (!output) {
    std::printf("front buffer was not produced by a resolve in this frame\n");
    return 2;
  }
  if (!WriteImage(context, *output, argv[2])) return 3;
  std::printf("wrote %s (%ux%u)\n", argv[2], output->width, output->height);
  return 0;
}
