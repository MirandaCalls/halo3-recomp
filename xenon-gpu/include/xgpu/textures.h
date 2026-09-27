// xenon-gpu: native textures and samplers for guest draws.
//
// Guest textures are described by 6-dword texture fetch constants
// (xenos::xe_gpu_texture_fetch_t) at register XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0
// + fetch_constant * 6. The cache turns them into Vulkan images:
// - normally on the GPU, from the shared memory buffer (the GPU copy of guest
//   physical memory, which also holds resolve and memexport output), with
//   Xenia's texture_load_* compute shaders (untile, endian swap, format
//   conversion; DXT stays BC compressed) and a buffer -> image copy;
// - on the CPU from the guest memory image (same conversions) when no shared
//   memory buffer is set (tools) or a format has no load shader;
// - or, when the fetch exactly matches the most recent resolve to its base
//   address (same size, compatible format, not overwritten since), by using
//   the native resolved image from RenderTargets directly.
//
// Invalidation: with a rex::graphics::SharedMemory (live game), textures watch
// their guest ranges (CPU writes, and GPU writes reported through
// RangeWrittenByGpu) and reload on next use. Without one (replay), the guest
// memory content is hashed on first use in each frame, and InvalidateRange
// reports GPU writes (resolves).
//
// Views and samplers must match what SpirvShaderTranslator declares for the
// binding (see VulkanTextureCache::GetActiveBindingOrNullImageView and
// VulkanTextureCache::GetSamplerParameters / UseSampler in the Xenia code):
// dimension 1D/2D -> 2D array view, 3D -> 3D view, cube -> cube view; signed
// vs unsigned views for is_signed bindings.
#pragma once

#include <xgpu/vk.h>  // first: see vk.h

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <xgpu/render_targets.h>

namespace rex::graphics {
class SharedMemory;
}

namespace xgpu {

class TextureCache {
 public:
  // guest_memory: the 512 MB physical memory snapshot (big-endian guest data).
  TextureCache(vk::Context& context, const uint8_t* guest_memory, RenderTargets& render_targets);
  ~TextureCache();

  // Enables GPU loading from the shared memory buffer (512 MB, storage buffer
  // usage). request_range (optional) is called for every guest range a load
  // reads before recording it (uploads CPU-written pages; null = the buffer
  // is always complete, as in replay). shared_memory (optional) enables
  // page-watch invalidation instead of per-frame content hashing. Call before
  // any GetView; call with VK_NULL_HANDLE / nullptr before destroying the
  // SharedMemory (removes the watches).
  void SetSharedMemory(VkBuffer buffer, std::function<bool(uint32_t start, uint32_t length)> request_range,
                       rex::graphics::SharedMemory* shared_memory = nullptr);

  // Marks textures overlapping guest physical [start, start + length) as
  // outdated (reloaded on next use). For GPU writes to the shared memory
  // buffer when there is no SharedMemory to report them (replay resolves).
  void InvalidateRange(uint32_t start, uint32_t length);

  // Returns a view for a translator texture binding, loading / converting the
  // texture if needed (upload commands are recorded into `cmd`, outside any
  // render pass). Never returns VK_NULL_HANDLE: a null (black/zero) texture of
  // the right view type is returned for unsupported or invalid fetch constants.
  // The image is left in SHADER_READ_ONLY_OPTIMAL layout.
  VkImageView GetView(VkCommandBuffer cmd, const rex::graphics::RegisterFile& regs,
                      const rex::graphics::SpirvShader::TextureBinding& binding);

  // Returns a sampler matching the binding's filter overrides and the fetch
  // constant's clamp / filter / border / LOD state.
  VkSampler GetSampler(const rex::graphics::RegisterFile& regs,
                       const rex::graphics::SpirvShader::SamplerBinding& binding);

  // Fills the texture-related system constants for the textures in
  // `used_texture_mask`: texture_swizzled_signs, texture_swizzles (when the
  // swizzle is not applied through the view's component mapping),
  // textures_resolved, textures_resolution_scaled (0), and
  // texture_integer_scale_bits.
  void WriteSystemConstants(const rex::graphics::RegisterFile& regs, uint32_t used_texture_mask,
                            rex::graphics::SpirvShaderTranslator::SystemConstants& constants);

  // Frees / recycles the staging and load scratch memory used by the uploads
  // recorded so far (call after the command buffers they were recorded into
  // have completed). Also done by the destructor.
  void EndFrame();

  // Debug / testing: loads one mip level (array layer 0) of the texture a
  // fetch constant describes on the CPU (same untiling / conversion path as
  // GetView) and decodes it to RGBA8 (BC formats are decompressed). Returns
  // false for invalid or unsupported fetch constants. `description` gets a
  // short summary (format, size, tiling, address).
  bool DebugDecode(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, uint32_t& width,
                   uint32_t& height, std::vector<uint8_t>& rgba, std::string* description,
                   uint32_t level = 0) const;

  // Stats for reports.
  uint32_t loaded_count() const;
  uint32_t resolved_hits() const;
  uint32_t unsupported_count() const;

 private:
  VkImageView GetViewImpl(VkCommandBuffer cmd, const rex::graphics::RegisterFile& regs,
                          const rex::graphics::SpirvShader::TextureBinding& binding);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xgpu
