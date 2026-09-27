// rexgpu-xgpu: ReXGlue GPU plugin that renders with the xenon-gpu native
// renderer instead of Xenia's EDRAM emulation.
//
// ReXGlue's CommandProcessor base still runs the PM4 ring (register writes,
// shader loads, predication, waits, interrupts); this plugin's command
// processor turns the draws, resolves and swaps it reports into native Vulkan
// work on the presenter's device. Select with --gpu_plugin=xgpu.
#include <xgpu/debug.h>
#include <xgpu/render_targets.h>
#include <xgpu/renderer.h>
#include <xgpu/textures.h>
#include <xgpu/vk.h>  // before any ReXGlue header, see vk.h

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>
#include <string_view>

#include <xxhash.h>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/shared_memory.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

namespace xgpu {

using namespace rex::graphics;

// ReXGlue's page-tracking shared memory (write watches on guest physical
// memory, valid/GPU-written page bits); uploads go through the renderer.
class NativeSharedMemory : public SharedMemory {
 public:
  NativeSharedMemory(rex::memory::Memory& memory, Renderer& renderer)
      : SharedMemory(memory), renderer_(renderer) {}
  ~NativeSharedMemory() override { ShutdownCommon(); }
  bool Initialize() { return InitializeCommon(); }

 protected:
  bool UploadRanges(const std::pair<uint32_t, uint32_t>* ranges, uint32_t count) override {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t start = ranges[i].first << page_size_log2();
      const uint32_t length = ranges[i].second << page_size_log2();
      if (!length) continue;
      MakeRangeValid(start, length, false);
      if (!renderer_.UploadGuestRange(start, length)) return false;
    }
    return true;
  }

 private:
  Renderer& renderer_;
};

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, rex::system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}

 protected:
  bool SetupContext() override {
    auto* provider = static_cast<const rex::ui::vulkan::VulkanProvider*>(graphics_system_->provider());
    rex::ui::vulkan::VulkanDevice* device = provider->vulkan_device();
    const uint32_t queue_family = device->queue_family_graphics_compute();
    // Own queue when the device has a second one: the presenter holds queue 0
    // across vkQueuePresentKHR, which can block indefinitely (Wayland FIFO
    // while the window isn't shown). Work is awaited on the CPU before the
    // guest output refresher returns, so ordering with the presenter holds.
    const uint32_t queue_index = device->queue_families()[queue_family].queues.size() >= 2 ? 1 : 0;
    std::string error;
    if (!context_.InitializeExternal(
            device->vulkan_instance()->instance(), device->physical_device(), device->device(), queue_family,
            [device, queue_family, queue_index](const std::function<void(VkQueue)>& use) {
              auto acquisition = device->AcquireQueue(queue_family, queue_index);
              use(acquisition.queue());
            },
            &error)) {
      REXLOG_ERROR("xgpu: Vulkan context: {}", error);
      return false;
    }
    const uint8_t* guest_memory = memory_->TranslatePhysical<const uint8_t*>(0);
    render_targets_ = std::make_unique<RenderTargets>(context_);
    if (!render_targets_->Initialize(&error)) {
      REXLOG_ERROR("xgpu: render targets: {}", error);
      return false;
    }
    render_targets_->SetGuestMemory(guest_memory);
    textures_ = std::make_unique<TextureCache>(context_, guest_memory, *render_targets_);
    renderer_ = std::make_unique<Renderer>(context_, guest_memory, *render_targets_, *textures_, true);
    if (!renderer_->Initialize(&error)) {
      REXLOG_ERROR("xgpu: renderer: {}", error);
      return false;
    }
    shared_memory_ = std::make_unique<NativeSharedMemory>(*memory_, *renderer_);
    if (!shared_memory_->Initialize()) {
      REXLOG_ERROR("xgpu: shared memory initialization failed");
      return false;
    }
    render_targets_->SetSharedMemoryBuffer(renderer_->shared_memory_buffer());
    renderer_->request_range = [this](uint32_t start, uint32_t length) {
      return shared_memory_->RequestRange(start, length);
    };
    renderer_->range_written_by_gpu = [this](uint32_t start, uint32_t length) {
      shared_memory_->RangeWrittenByGpu(start, length);
    };
    // Textures load from the shared memory buffer on the GPU; page watches
    // (CPU writes, resolves, memexport) mark them for reloading.
    textures_->SetSharedMemory(
        renderer_->shared_memory_buffer(),
        [this](uint32_t start, uint32_t length) { return shared_memory_->RequestRange(start, length); },
        shared_memory_.get());
    REXLOG_INFO("xgpu: native renderer on {} (queue {})", context_.device_name(), queue_index);
    return true;
  }

  void ShutdownContext() override {
    context_.FlushFrame();
    if (renderer_) {
      renderer_->request_range = nullptr;
      renderer_->range_written_by_gpu = nullptr;
    }
    // Remove the texture watches before the SharedMemory goes away.
    if (textures_) textures_->SetSharedMemory(VK_NULL_HANDLE, nullptr, nullptr);
    shared_memory_.reset();
    context_.DestroyBuffer(readback_buffer_);
    renderer_.reset();
    textures_.reset();
    render_targets_.reset();
  }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address, const uint32_t* host_address,
                     uint32_t dword_count) override {
    pm4::ShaderLoad load;
    load.pixel = shader_type == xenos::ShaderType::kPixel;
    load.address = guest_address;
    load.dword_count = dword_count;
    load.hash = XXH3_64bits(host_address, size_t(dword_count) * 4);
    load.guest_dwords = host_address;
    return renderer_->OnShaderLoad(load);
  }

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override {
    (void)prim_type;
    (void)index_buffer_info;
    (void)major_mode_explicit;
    const RegisterFile& regs = *register_file_;
    if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) return IssueCopy();
    pm4::Draw draw{};
    draw.index = draw_index_++;
    draw.initiator = regs[XE_GPU_REG_VGT_DRAW_INITIATOR];
    draw.index_count = index_count;
    draw.vertex_shader_hash = active_vertex_shader() ? active_vertex_shader()->ucode_data_hash() : 0;
    draw.pixel_shader_hash = active_pixel_shader() ? active_pixel_shader()->ucode_data_hash() : 0;
    draw.registers = regs.values;
    renderer_->Draw(context_.frame_command_buffer(), draw);
    return true;
  }

  bool IssueCopy() override {
    VkCommandBuffer cmd = context_.frame_command_buffer();
    renderer_->EndRendering(cmd);
    const ResolveInfo info = renderer_->GetResolveInfo(register_file_->values);
    // The resolve overwrites part of its extent's pages: make them valid first
    // (uploads recorded before the resolve), then mark them GPU-written.
    if (info.memory_write) shared_memory_->RequestRange(info.extent_start, info.extent_length);
    if (g_trace) {
      std::printf("resolve src %s -> %08X %ux%u fmt %u rect %d,%d-%d,%d +%d bias %d swap %u mem %u\n",
                  info.source.Describe().c_str(), info.dest_address, info.dest_width, info.dest_height,
                  info.dest_format, info.rect_x0, info.rect_y0, info.rect_x1, info.rect_y1, info.dest_offset_x,
                  info.dest_exp_bias, info.dest_swap, info.memory_write);
    }
    render_targets_->Resolve(cmd, info);
    if (info.memory_write) {
      shared_memory_->RangeWrittenByGpu(info.extent_start, info.extent_length);
      // The CPU may read resolve output (Reach computes auto-exposure from
      // tiny luminance resolves): copy small ones back into guest RAM at the
      // end of the frame, like Xenia's readback resolve.
      static const uint32_t readback_max = [] {
        const char* v = std::getenv("XGPU_READBACK_MAX");
        return v ? uint32_t(std::strtoul(v, nullptr, 0)) : 1u << 20;
      }();
      if (info.extent_length <= readback_max) readback_ranges_.push_back({info.extent_start, info.extent_length});
    }
    return true;
  }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    EndZPDFrame();
    VkCommandBuffer cmd = context_.frame_command_buffer();
    renderer_->EndRendering(cmd);
    rex::ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
    vk::Image* source = render_targets_->FindResolvedMutable(frontbuffer_ptr & 0x1FFFFFFF);
    if (!source) source = render_targets_->FindResolvedMutable(frontbuffer_ptr);
    if (presenter && source && frontbuffer_width && frontbuffer_height) {
      const uint32_t width = std::min(frontbuffer_width, source->width);
      const uint32_t height = std::min(frontbuffer_height, source->height);
      presenter->RefreshGuestOutput(
          width, height, width, height, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
            auto& vulkan_context =
                static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
            VkImage output = vulkan_context.image();
            vk::Context::Transition(cmd, *source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = output;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstSubresource = blit.srcSubresource;
            blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
            blit.dstOffsets[1] = blit.srcOffsets[1];
            vkCmdBlitImage(cmd, source->image, source->layout, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_NEAREST);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                                 nullptr, 1, &barrier);
            vulkan_context.SetIs8bpc(true);
            // The presenter orders its fence after this submission.
            return EndFrame();
          });
    } else if (!source && !missing_frontbuffer_logged_) {
      missing_frontbuffer_logged_ = true;
      REXLOG_WARN("xgpu: front buffer 0x{:08X} was not produced by a resolve; not presented", frontbuffer_ptr);
    }
    EndFrame();
    ++frame_count_;
    // Debug: XGPU_TRACE_FRAME=N traces every draw / resolve / texture of frame N.
    static const uint64_t trace_frame = [] {
      const char* v = std::getenv("XGPU_TRACE_FRAME");
      return v ? std::strtoull(v, nullptr, 0) : 0ull;
    }();
    // Or XGPU_TRACE_TRIGGER=<file>: traces the frame after the file appears
    // (and deletes it).
    static const char* trace_trigger = std::getenv("XGPU_TRACE_TRIGGER");
    const bool was_tracing = g_trace;
    g_trace = trace_frame && frame_count_ == trace_frame;
    if (trace_trigger && std::filesystem::exists(trace_trigger)) {
      std::error_code ec;
      std::filesystem::remove(trace_trigger, ec);
      g_trace = true;
    }
    if (g_trace) std::printf("==== xgpu trace frame %llu\n", (unsigned long long)frame_count_);
    if (was_tracing) std::fflush(stdout);
    // Debug: XGPU_DUMP_SWAP=<dir> writes the front buffer every 300 frames.
    static const char* dump_dir = std::getenv("XGPU_DUMP_SWAP");
    if (dump_dir && source && frame_count_ % 300 == 0) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/swap_%06llu.ppm", dump_dir, (unsigned long long)frame_count_);
      context_.WritePPM(*source, path);
      std::vector<uint32_t> addresses;
      for (const auto& entry : render_targets_->resolved()) addresses.push_back(entry.first);
      for (uint32_t address : addresses) {
        std::snprintf(path, sizeof(path), "%s/swap_%06llu_r%08X.ppm", dump_dir,
                      (unsigned long long)frame_count_, address);
        context_.WritePPM(*render_targets_->FindResolvedMutable(address), path);
      }
    }
    if (frame_count_ == 1 || frame_count_ % 600 == 0) {
      const Renderer::Stats& s = renderer_->stats();
      REXLOG_INFO(
          "xgpu: frame {}: {} drawn, {} pipelines, {} shaders; skipped {} no target, {} translation, {} "
          "primitive, {} pipeline; {} textures ({} unsupported)",
          frame_count_, s.drawn, s.pipelines, s.shaders, s.skipped_no_target, s.skipped_translation,
          s.skipped_primitive, s.skipped_pipeline, textures_->loaded_count(), textures_->unsupported_count());
    }
  }

 private:
  bool EndFrame() {
    if (!context_.frame_open()) return true;
    // Resolve readback: shared memory -> host-visible buffer, then guest RAM
    // once the frame has completed.
    std::vector<std::pair<uint32_t, VkDeviceSize>> readbacks;  // (guest address, buffer offset + size packed)
    std::vector<std::pair<uint32_t, uint32_t>> readback_list;
    VkDeviceSize readback_used = 0;
    if (!readback_ranges_.empty()) {
      if (!readback_buffer_.buffer) {
        context_.CreateBuffer(readback_buffer_, 32u * 1024 * 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      }
      VkCommandBuffer cmd = context_.frame_command_buffer();
      renderer_->EndRendering(cmd);
      std::vector<VkBufferCopy> copies;
      for (const auto& [start, length] : readback_ranges_) {
        if (readback_used + length > readback_buffer_.size) break;
        copies.push_back({start, readback_used, length});
        readback_list.push_back({start, uint32_t(readback_used)});
        readback_used += (length + 255) & ~255u;
      }
      if (!copies.empty()) {
        vkCmdCopyBuffer(cmd, renderer_->shared_memory_buffer(), readback_buffer_.buffer, uint32_t(copies.size()),
                        copies.data());
      }
    }
    const bool ok = context_.FlushFrame();
    if (ok) {
      for (size_t i = 0; i < readback_list.size(); ++i) {
        const uint32_t start = readback_list[i].first;
        const uint32_t length = readback_ranges_[i].second;
        std::memcpy(memory_->TranslatePhysical<uint8_t*>(start),
                    static_cast<const uint8_t*>(readback_buffer_.mapped) + readback_list[i].second, length);
      }
    }
    readback_ranges_.clear();
    renderer_->EndFrame();
    render_targets_->EndFrame();
    textures_->EndFrame();
    draw_index_ = 0;
    return ok;
  }

  vk::Context context_;
  std::unique_ptr<RenderTargets> render_targets_;
  std::unique_ptr<TextureCache> textures_;
  std::unique_ptr<Renderer> renderer_;
  std::unique_ptr<NativeSharedMemory> shared_memory_;
  std::vector<std::pair<uint32_t, uint32_t>> readback_ranges_;
  vk::Buffer readback_buffer_;
  uint32_t draw_index_ = 0;
  uint64_t frame_count_ = 0;
  bool missing_frontbuffer_logged_ = false;
};

class NativeGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "xenon-gpu native (Vulkan)"; }

 protected:
  void CreateProvider(bool with_presentation) override {
    provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
  }

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace xgpu

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-xgpu: incompatible host (ABI {})", abi_version);
    return nullptr;
  }
  return new xgpu::NativeGraphicsSystem();
}
