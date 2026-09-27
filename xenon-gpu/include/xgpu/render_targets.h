// xenon-gpu: native render targets.
//
// Instead of emulating the Xenos EDRAM (a 10 MB tiled memory that render
// targets are views into), every render target configuration the title uses
// (EDRAM base, pitch, format, MSAA) is a real Vulkan image, and a resolve
// (EDRAM -> texture memory copy) produces a "resolved texture" image keyed by
// its destination address, which later draws can sample directly without a
// round trip through guest memory.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <xgpu/vk.h>

namespace xgpu {

struct RenderTargetKey {
  uint32_t base_tiles = 0;  // EDRAM base in 80x16-sample tiles
  uint32_t pitch = 0;       // pixels
  uint32_t format = 0;      // Xenos color RT format, or depth format
  uint32_t msaa = 0;        // log2 samples
  bool depth = false;
  bool operator==(const RenderTargetKey& o) const {
    return base_tiles == o.base_tiles && pitch == o.pitch && format == o.format && msaa == o.msaa &&
           depth == o.depth;
  }
  bool operator<(const RenderTargetKey& o) const {
    return std::tie(base_tiles, pitch, format, msaa, depth) <
           std::tie(o.base_tiles, o.pitch, o.format, o.msaa, o.depth);
  }
  std::string Describe() const;
};

struct ResolveInfo {
  RenderTargetKey source;
  uint32_t dest_address = 0;
  uint32_t dest_width = 0, dest_height = 0;
  uint32_t dest_format = 0;  // Xenos texture ColorFormat
  bool clear_color = false, clear_depth = false;
  bool has_depth_target = false;
  RenderTargetKey depth_target;  // bound depth buffer (cleared by clear_depth)
  uint32_t clear_color_value = 0, clear_color_value_lo = 0, clear_depth_value = 0;
  int32_t dest_exp_bias = 0;   // RB_COPY_DEST_INFO: color scaled by 2^bias
  uint32_t sample_select = 0;  // RB_COPY_CONTROL copy_sample_select (6 = average all)
  bool dest_swap = false;      // swap red and blue
  // Source rectangle in render target pixels (from the resolve draw's
  // vertices); the destination receives it at the same position plus
  // dest_offset_x (base address macro tile phase). has_rect false: whole
  // destination from the origin.
  bool has_rect = false;
  int32_t rect_x0 = 0, rect_y0 = 0, rect_x1 = 0, rect_y1 = 0;
  int32_t dest_offset_x = 0;
  // Guest memory write (shared memory buffer, Xbox tiled layout): enabled for
  // 32 / 64 bpp destinations when the extent is known.
  bool memory_write = false;
  uint32_t dest_pitch_aligned = 0;  // texels, multiple of 32
  uint32_t dest_bpp_log2 = 2;
  uint32_t dest_endian = 0;         // xenos::Endian128
  uint32_t dest_number = 0;         // xenos::SurfaceNumberFormat
  bool depth_float24 = false;
  uint32_t extent_start = 0, extent_length = 0;
};

// Keys and resolve parameters from the Xenos register file (dword array).
RenderTargetKey ColorTargetKey(const uint32_t* regs, uint32_t index);
RenderTargetKey DepthTargetKey(const uint32_t* regs);
ResolveInfo ResolveInfoFromRegisters(const uint32_t* regs);

class RenderTargets {
 public:
  explicit RenderTargets(vk::Context& context) : context_(context) {}
  ~RenderTargets();

  // Creates the resolve shaders; call once before Resolve.
  bool Initialize(std::string* error);

  // Returns the image for a render target, creating it (or growing it) so it
  // is at least `height` rows tall.
  vk::Image* Get(const RenderTargetKey& key, uint32_t height);

  // Copies the source render target into the resolved texture for
  // `info.dest_address`, then applies the clears the resolve requests.
  void Resolve(VkCommandBuffer cmd, const ResolveInfo& info);

  const vk::Image* FindResolved(uint32_t address) const;
  // Mutable access (for layout transitions by the texture cache).
  vk::Image* FindResolvedMutable(uint32_t address) {
    auto it = resolved_.find(address);
    return it == resolved_.end() ? nullptr : &it->second;
  }

  // Whether the last resolve to `address` requested a red/blue swap. The swap
  // isn't baked into the image (the image is also what gets presented, and the
  // guest presents swapped resolves through a swizzled fetch); texture views of
  // it apply the swap instead.
  bool IsResolvedSwapped(uint32_t address) const {
    auto it = resolved_swapped_.find(address);
    return it != resolved_swapped_.end() && it->second;
  }

  // Guest memory, for detecting CPU writes over resolve destinations (native
  // resolves don't write guest memory, so it only changes when the CPU puts
  // something else there, e.g. new textures after a level load).
  void SetGuestMemory(const uint8_t* memory) { guest_memory_ = memory; }
  // Whether the resolved image for `address` still represents that memory.
  bool IsResolvedCurrent(uint32_t address);
  // Shared memory buffer resolves also write to (live mode); none = only
  // the resolved images.
  void SetSharedMemoryBuffer(VkBuffer buffer) { shared_memory_buffer_ = buffer; }

  // Records that a render target was just drawn to (its EDRAM contents are
  // now in this image, not in aliases of it).
  void MarkWritten(const RenderTargetKey& guest_key);

  // EDRAM ownership: if the tiles behind this view were last written through
  // another format's image (same base / pitch / samples), converts that
  // image's bits into this one's format first. Records commands (outside a
  // render pass); returns whether it did.
  bool TransferOwnership(VkCommandBuffer cmd, const RenderTargetKey& guest_key);
  bool LastWriteIsOther(const RenderTargetKey& guest_key) const { return NewerOwner(guest_key) != nullptr; }

  // Frees temporaries used by the frame's commands (after they completed).
  void EndFrame();

  // Stats for reports.
  size_t render_target_count() const { return targets_.size(); }
  size_t resolved_count() const { return resolved_.size(); }
  const std::map<RenderTargetKey, vk::Image>& targets() const { return targets_; }
  const std::map<uint32_t, vk::Image>& resolved() const { return resolved_; }

  static VkFormat ColorRenderTargetFormat(uint32_t xenos_format);
  static VkFormat TextureFormat(uint32_t xenos_color_format);

 private:
  struct ResolvePipelineKey {
    VkFormat format;
    bool multisampled;
    bool operator<(const ResolvePipelineKey& o) const {
      return std::tie(format, multisampled) < std::tie(o.format, o.multisampled);
    }
  };
  VkPipeline GetResolvePipeline(VkFormat format, bool multisampled);
  VkImageView GetView(vk::Image& image, VkFormat format, VkImageAspectFlags aspect);

  vk::Context& context_;
  VkShaderModule resolve_vs_ = VK_NULL_HANDLE, resolve_fs_ = VK_NULL_HANDLE,
                 resolve_fs_ms_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout resolve_set_layout_ = VK_NULL_HANDLE;
  VkShaderModule write_cs_ = VK_NULL_HANDLE, write_cs_ms_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout write_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout write_layout_ = VK_NULL_HANDLE;
  VkPipeline write_pipeline_ = VK_NULL_HANDLE, write_pipeline_ms_ = VK_NULL_HANDLE;
  VkBuffer shared_memory_buffer_ = VK_NULL_HANDLE;
  void WriteResolveToMemory(VkCommandBuffer cmd, const ResolveInfo& info, vk::Image& source,
                            VkImageView source_view, int32_t alias_mode);
  VkPipelineLayout resolve_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool resolve_pool_ = VK_NULL_HANDLE;  // per frame
  VkSampler resolve_sampler_ = VK_NULL_HANDLE;
  std::map<ResolvePipelineKey, VkPipeline> resolve_pipelines_;
  // (image id, format, aspect) -> view
  std::map<std::tuple<uint64_t, uint32_t, uint32_t>, VkImageView> views_;
  std::vector<VkImageView> frame_views_to_free_;
  std::map<RenderTargetKey, vk::Image> targets_;
  std::map<uint32_t, vk::Image> resolved_;
  std::map<uint32_t, bool> resolved_swapped_;
  std::map<uint32_t, uint64_t> resolved_memory_hash_;
  std::map<RenderTargetKey, uint64_t> last_write_;  // canonical key -> sequence
  std::map<RenderTargetKey, uint32_t> last_format_;  // canonical key -> guest format last written
  VkShaderModule transfer_fs_ = VK_NULL_HANDLE, transfer_fs_ms_ = VK_NULL_HANDLE;
  VkPipelineLayout transfer_layout_ = VK_NULL_HANDLE;
  std::map<std::tuple<uint32_t, uint32_t>, VkPipeline> transfer_pipelines_;  // (format, samples)
  VkPipeline GetTransferPipeline(VkFormat format, VkSampleCountFlagBits samples);
  const RenderTargetKey* NewerOwner(const RenderTargetKey& guest_key) const;
  uint64_t write_sequence_ = 0;
  uint64_t LastWrite(const RenderTargetKey& guest_key) const;
  const uint8_t* guest_memory_ = nullptr;
  uint64_t MemoryFingerprint(uint32_t address, uint32_t size) const;
  std::vector<vk::Image> frame_temporaries_;
};

}  // namespace xgpu
