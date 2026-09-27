// xenon-gpu: native draw path.
//
// Consumes the draws reported by the PM4 front end and rasterizes them with
// Vulkan into the native render targets (xgpu::RenderTargets): guest shaders
// are translated to SPIR-V with Xenia's translator, the guest's fixed-function
// state becomes pipeline state, vertex data is fetched by the translated
// shaders from guest memory bound as a storage buffer, and textures come from
// xgpu::TextureCache.
#pragma once

#include <xgpu/vk.h>  // first: see vk.h

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/register_file.h>
#include <xgpu/pm4.h>
#include <xgpu/render_targets.h>
#include <xgpu/textures.h>

namespace xgpu {

class Renderer {
 public:
  // guest_memory: 512 MB physical memory image (uploaded as the shared memory
  // storage buffer, also used as the index buffer).
  // With `import_guest_memory`, the guest memory is imported in place
  // (VK_EXT_external_memory_host: shaders see live guest memory, and memexport
  // writes land in it); otherwise it's copied once (replaying a capture).
  Renderer(vk::Context& context, const uint8_t* guest_memory, RenderTargets& render_targets,
           TextureCache& textures, bool import_guest_memory = false);
  ~Renderer();

  bool Initialize(std::string* error);

  rex::graphics::SpirvShader* OnShaderLoad(const pm4::ShaderLoad& load);
  // Records a (non-resolve) draw.
  void Draw(VkCommandBuffer cmd, const pm4::Draw& draw);
  // Full resolve parameters for the current register state (including the
  // source rectangle from the resolve draw's vertices in guest memory).
  ResolveInfo GetResolveInfo(const uint32_t* registers);

  // Ends the current dynamic rendering scope (before resolves / transfers).
  void EndRendering(VkCommandBuffer cmd);
  // Recycles per-frame allocations; call after the frame's commands completed.
  void EndFrame();

  struct Stats {
    uint32_t drawn = 0, skipped_no_raster = 0, skipped_no_shader = 0, skipped_translation = 0,
             skipped_primitive = 0, skipped_no_target = 0, skipped_pipeline = 0, passes = 0, memexport_only = 0;
    uint32_t pipelines = 0, shaders = 0;
  };
  const Stats& stats() const { return stats_; }

 private:
  struct PipelineLayout {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_vertex = VK_NULL_HANDLE, textures_pixel = VK_NULL_HANDLE;
  };
  struct RenderingState {
    bool active = false;
    VkImageView color[4] = {};
    VkImageView depth = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool operator==(const RenderingState& o) const;
  };

  rex::graphics::SpirvShader* GetShader(uint64_t hash, bool pixel);
  VkShaderModule GetModule(rex::graphics::Shader::Translation& translation);
  VkDescriptorSetLayout GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers);
  const PipelineLayout* GetPipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp, uint32_t sp);
  // Bump allocation in the per-frame uniform buffer; returns the offset.
  VkDeviceSize AllocateUniform(const void* data, size_t size);

  vk::Context& context_;
  const uint8_t* guest_memory_;
  RenderTargets& render_targets_;
  TextureCache& textures_;
  rex::graphics::RegisterFile regs_;

  std::unique_ptr<rex::graphics::SpirvShaderTranslator> translator_;
  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::SpirvShader>> shaders_;
  std::unordered_map<rex::graphics::Shader::Translation*, VkShaderModule> modules_;

  vk::Buffer shared_memory_;
  vk::Buffer uniforms_;
  VkDeviceSize uniforms_used_ = 0;
  VkDeviceSize uniform_alignment_ = 256;
  vk::Buffer converted_indices_;
  VkDeviceSize converted_indices_used_ = 0;
  vk::Buffer builtin_indices_;  // two-triangle strips with primitive restart
  vk::Buffer dummy_storage_;

  VkDescriptorSetLayout layout_shared_memory_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_constants_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_empty_ = VK_NULL_HANDLE;
  std::map<std::tuple<bool, uint32_t, uint32_t>, VkDescriptorSetLayout> texture_set_layouts_;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, PipelineLayout> pipeline_layouts_;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;  // per frame
  VkDescriptorPool persistent_pool_ = VK_NULL_HANDLE;
  bool import_guest_memory_ = false;
  VkDeviceMemory imported_memory_ = VK_NULL_HANDLE;
  // Live mode: guest memory ranges are snapshotted into the device buffer at
  // draw time (the guest reuses dynamic buffers as soon as the command
  // processor has parsed a draw, long before the frame executes).
  bool snapshot_memory_ = false;
  vk::Buffer upload_staging_;
  VkDeviceSize upload_staging_used_ = 0;
  // Ranges whose current contents are in the device buffer: start -> (size,
  // hash). Uploading a range drops every overlapping entry (their device copy
  // may now differ).
  std::map<uint32_t, std::pair<uint32_t, uint64_t>> uploaded_ranges_;
  uint32_t uploaded_max_size_ = 0;
  // Memexport destinations: start -> (end, last frame written).
  std::map<uint32_t, std::pair<uint32_t, uint64_t>> gpu_written_;
  uint64_t frame_index_ = 0;
  bool upload_staging_overflowed_ = false;
  bool shared_memory_cleared_ = false;
  void SyncGuestRange(VkCommandBuffer cmd, uint32_t address, uint32_t size);

 public:
  // Live mode with a page-tracking shared memory (ReXGlue SharedMemory): the
  // renderer asks it for ranges before use and reports memexport writes; the
  // shared memory uploads invalid pages through UploadGuestRange.
  std::function<bool(uint32_t start, uint32_t length)> request_range;
  std::function<void(uint32_t start, uint32_t length)> range_written_by_gpu;
  // Records a copy of guest memory into the shared memory buffer (into the
  // command buffer of the draw being recorded).
  bool UploadGuestRange(uint32_t address, uint32_t size);
  VkBuffer shared_memory_buffer() const { return shared_memory_.buffer; }

 private:
  VkCommandBuffer current_cmd_ = VK_NULL_HANDLE;
  std::vector<vk::Buffer> extra_staging_;
  VkDescriptorSet shared_memory_set_ = VK_NULL_HANDLE;

  std::unordered_map<std::string, VkPipeline> pipelines_;

  RenderingState rendering_;
  Stats stats_;
};

}  // namespace xgpu
