#include <xgpu/renderer.h>
#include <xgpu/debug.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <xxhash.h>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>
#include <rex/string/buffer.h>

namespace xgpu {

bool g_trace = false;

using namespace rex::graphics;
using Translator = SpirvShaderTranslator;

namespace {

constexpr VkDeviceSize kSharedMemorySize = 512u * 1024 * 1024;
constexpr VkDeviceSize kUniformBufferSize = 128u * 1024 * 1024;
constexpr VkDeviceSize kConvertedIndexBufferSize = 32u * 1024 * 1024;
constexpr uint32_t kMaxBuiltinStrips = 65536;

constexpr uint32_t TwoTriangleStripIndexCount(uint32_t strips) {
  return 4 * strips + (std::max(strips, 1u) - 1);
}

RenderTargetKey ColorKey(const RegisterFile& regs, uint32_t index) {
  auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[index]);
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  RenderTargetKey key;
  key.base_tiles = info.color_base;
  key.format = uint32_t(info.color_format);
  key.pitch = surface.surface_pitch;
  key.msaa = uint32_t(surface.msaa_samples);
  return key;
}

RenderTargetKey DepthKey(const RegisterFile& regs) {
  auto info = regs.Get<reg::RB_DEPTH_INFO>();
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  RenderTargetKey key;
  key.depth = true;
  key.base_tiles = info.depth_base;
  key.format = uint32_t(info.depth_format);
  key.pitch = surface.surface_pitch;
  key.msaa = uint32_t(surface.msaa_samples);
  return key;
}

VkBlendFactor BlendFactor(uint32_t f, bool alpha) {
  static const VkBlendFactor kColor[17] = {
      VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  static const VkBlendFactor kAlpha[17] = {
      VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  return f < 17 ? (alpha ? kAlpha : kColor)[f] : VK_BLEND_FACTOR_ZERO;
}

VkBlendOp BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return VK_BLEND_OP_ADD;
    case xenos::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
  }
  return VK_BLEND_OP_ADD;
}

// Everything that goes into a pipeline, hashed as bytes.
struct PipelineKey {
  const void* vertex_translation;
  const void* pixel_translation;
  VkPipelineLayout layout;
  uint32_t topology, restart, cull_mode, front_face_cw, depth_clamp, samples;
  uint32_t depth_format, depth_test, depth_write, depth_compare;
  uint32_t stencil_test, stencil_front[4], stencil_back[4];
  uint32_t color_formats[4];
  uint32_t blend[4][7];  // enable, src c, dst c, op c, src a, dst a, op a
  uint32_t write_mask[4];
  uint32_t rasterizer_discard;
};

}  // namespace

bool Renderer::RenderingState::operator==(const RenderingState& o) const {
  return active == o.active && !std::memcmp(color, o.color, sizeof(color)) && depth == o.depth &&
         width == o.width && height == o.height;
}

Renderer::Renderer(vk::Context& context, const uint8_t* guest_memory, RenderTargets& render_targets,
                   TextureCache& textures, bool import_guest_memory)
    : context_(context),
      guest_memory_(guest_memory),
      render_targets_(render_targets),
      textures_(textures),
      import_guest_memory_(import_guest_memory) {}

Renderer::~Renderer() {
  VkDevice device = context_.device();
  vkDeviceWaitIdle(device);
  for (auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
  for (auto& [translation, module] : modules_) vkDestroyShaderModule(device, module, nullptr);
  for (auto& [key, layout] : pipeline_layouts_) vkDestroyPipelineLayout(device, layout.layout, nullptr);
  for (auto& [key, layout] : texture_set_layouts_) vkDestroyDescriptorSetLayout(device, layout, nullptr);
  for (VkDescriptorSetLayout layout : {layout_shared_memory_, layout_constants_, layout_empty_}) {
    if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
  }
  if (descriptor_pool_) vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
  if (persistent_pool_) vkDestroyDescriptorPool(device, persistent_pool_, nullptr);
  if (imported_memory_) {
    vkDestroyBuffer(device, shared_memory_.buffer, nullptr);
    vkFreeMemory(device, imported_memory_, nullptr);
    shared_memory_ = vk::Buffer();
  }
  context_.DestroyBuffer(shared_memory_);
  context_.DestroyBuffer(uniforms_);
  context_.DestroyBuffer(converted_indices_);
  context_.DestroyBuffer(builtin_indices_);
  context_.DestroyBuffer(dummy_storage_);
  context_.DestroyBuffer(upload_staging_);
}

bool Renderer::Initialize(std::string* error) {
  VkDevice device = context_.device();
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(context_.physical_device(), &properties);
  uniform_alignment_ = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 16);

  Translator::Features features(true);
  features.max_storage_buffer_range = properties.limits.maxStorageBufferRange;
  features.fragment_shader_sample_interlock = false;
  features.signed_zero_inf_nan_preserve_float32 = false;
  features.denorm_flush_to_zero_float32 = false;
  features.rounding_mode_rte_float32 = false;
  translator_ = std::make_unique<Translator>(features, true, true, false);
  if (Translator::GetSharedMemoryStorageBufferCountLog2(features.max_storage_buffer_range) != 0) {
    *error = "device storage buffer range below 512 MB is not supported yet";
    return false;
  }

  // Guest memory as one storage (and index) buffer.
  if (import_guest_memory_) {
    // Snapshot mode (see SyncGuestRange): device-local copy of guest memory.
    snapshot_memory_ = true;
    if (!context_.CreateBuffer(shared_memory_, kSharedMemorySize,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               false) ||
        !context_.CreateBuffer(upload_staging_, 256u * 1024 * 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
      *error = "shared memory buffers";
      return false;
    }
  } else if (false) {
    auto get_host_pointer_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
    VkMemoryHostPointerPropertiesEXT pointer_properties = {
        VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!get_host_pointer_properties ||
        get_host_pointer_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                    guest_memory_, &pointer_properties) != VK_SUCCESS) {
      *error = "VK_EXT_external_memory_host unavailable for guest memory";
      return false;
    }
    VkExternalMemoryBufferCreateInfo external_info = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.pNext = &external_info;
    buffer_info.size = kSharedMemorySize;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vkCreateBuffer(device, &buffer_info, nullptr, &shared_memory_.buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, shared_memory_.buffer, &requirements);
    VkImportMemoryHostPointerInfoEXT import_info = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import_info.pHostPointer = const_cast<uint8_t*>(guest_memory_);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = &import_info;
    alloc.allocationSize = kSharedMemorySize;
    alloc.memoryTypeIndex = context_.FindMemoryType(
        pointer_properties.memoryTypeBits & requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &alloc, nullptr, &imported_memory_) != VK_SUCCESS ||
        vkBindBufferMemory(device, shared_memory_.buffer, imported_memory_, 0) != VK_SUCCESS) {
      *error = "importing guest memory failed";
      return false;
    }
    shared_memory_.size = kSharedMemorySize;
  } else {
    if (!context_.CreateBuffer(shared_memory_, kSharedMemorySize,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true)) {
      *error = "shared memory buffer";
      return false;
    }
    std::memcpy(shared_memory_.mapped, guest_memory_, kSharedMemorySize);
  }

  context_.CreateBuffer(uniforms_, kUniformBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
  context_.CreateBuffer(converted_indices_, kConvertedIndexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        true);
  context_.CreateBuffer(dummy_storage_, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
  const uint32_t builtin_count = TwoTriangleStripIndexCount(kMaxBuiltinStrips);
  context_.CreateBuffer(builtin_indices_, builtin_count * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
  {
    uint32_t* out = static_cast<uint32_t*>(builtin_indices_.mapped);
    for (uint32_t i = 0; i < kMaxBuiltinStrips; ++i) {
      if (i) *out++ = UINT32_MAX;
      for (uint32_t j = 0; j < 4; ++j) *out++ = (i << 2) + j;
    }
  }

  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding bindings[Translator::kConstantBufferCount] = {};
  VkDescriptorSetLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  // Set 0: shared memory (binding 0), ZPD counter (binding 1).
  for (uint32_t i = 0; i < 2; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = stages;
  }
  layout_info.bindingCount = 2;
  layout_info.pBindings = bindings;
  vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &layout_shared_memory_);
  // Set 1: constant buffers.
  for (uint32_t i = 0; i < Translator::kConstantBufferCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = stages;
  }
  layout_info.bindingCount = Translator::kConstantBufferCount;
  vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &layout_constants_);
  layout_info.bindingCount = 0;
  vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &layout_empty_);

  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 65536},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 65536},
  };
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 32768;
  pool_info.poolSizeCount = 4;
  pool_info.pPoolSizes = pool_sizes;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
    *error = "descriptor pool";
    return false;
  }
  VkDescriptorPoolSize persistent_sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
  VkDescriptorPoolCreateInfo persistent_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  persistent_info.maxSets = 2;
  persistent_info.poolSizeCount = 1;
  persistent_info.pPoolSizes = persistent_sizes;
  vkCreateDescriptorPool(device, &persistent_info, nullptr, &persistent_pool_);
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = persistent_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &layout_shared_memory_;
  vkAllocateDescriptorSets(device, &alloc, &shared_memory_set_);
  VkDescriptorBufferInfo buffer_infos[2] = {{shared_memory_.buffer, 0, VK_WHOLE_SIZE},
                                            {dummy_storage_.buffer, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = shared_memory_set_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffer_infos[i];
  }
  vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  return true;
}

SpirvShader* Renderer::OnShaderLoad(const pm4::ShaderLoad& load) {
  auto it = shaders_.find(load.hash);
  if (it != shaders_.end()) return it->second.get();
  auto shader = std::make_unique<SpirvShader>(
      load.pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, load.hash, load.guest_dwords,
      load.dword_count, std::endian::big);
  SpirvShader* result = shader.get();
  shaders_.emplace(load.hash, std::move(shader));
  ++stats_.shaders;
  return result;
}

SpirvShader* Renderer::GetShader(uint64_t hash, bool pixel) {
  auto it = shaders_.find(hash);
  if (it == shaders_.end()) return nullptr;
  SpirvShader* shader = it->second.get();
  if ((shader->type() == xenos::ShaderType::kPixel) != pixel) return nullptr;
  if (!shader->is_ucode_analyzed()) {
    rex::string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
  }
  return shader;
}

VkShaderModule Renderer::GetModule(Shader::Translation& translation) {
  auto it = modules_.find(&translation);
  if (it != modules_.end()) return it->second;
  const std::vector<uint8_t>& spirv = translation.translated_binary();
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = spirv.size();
  info.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
  VkShaderModule module = VK_NULL_HANDLE;
  vkCreateShaderModule(context_.device(), &info, nullptr, &module);
  modules_[&translation] = module;
  return module;
}

VkDescriptorSetLayout Renderer::GetTextureSetLayout(bool vertex, uint32_t textures, uint32_t samplers) {
  if (!textures && !samplers) return layout_empty_;
  auto key = std::make_tuple(vertex, textures, samplers);
  auto it = texture_set_layouts_.find(key);
  if (it != texture_set_layouts_.end()) return it->second;
  std::vector<VkDescriptorSetLayoutBinding> bindings(textures + samplers);
  for (uint32_t i = 0; i < textures + samplers; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  info.bindingCount = uint32_t(bindings.size());
  info.pBindings = bindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(context_.device(), &info, nullptr, &layout);
  return texture_set_layouts_[key] = layout;
}

const Renderer::PipelineLayout* Renderer::GetPipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp,
                                                            uint32_t sp) {
  auto key = std::make_tuple(tv, sv, tp, sp);
  auto it = pipeline_layouts_.find(key);
  if (it != pipeline_layouts_.end()) return &it->second;
  PipelineLayout layout;
  layout.textures_vertex = GetTextureSetLayout(true, tv, sv);
  layout.textures_pixel = GetTextureSetLayout(false, tp, sp);
  VkDescriptorSetLayout sets[4] = {layout_shared_memory_, layout_constants_, layout.textures_vertex,
                                   layout.textures_pixel};
  VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  info.setLayoutCount = 4;
  info.pSetLayouts = sets;
  vkCreatePipelineLayout(context_.device(), &info, nullptr, &layout.layout);
  return &(pipeline_layouts_[key] = layout);
}

VkDeviceSize Renderer::AllocateUniform(const void* data, size_t size) {
  VkDeviceSize offset = (uniforms_used_ + uniform_alignment_ - 1) & ~(uniform_alignment_ - 1);
  if (offset + size > uniforms_.size) offset = 0;  // wrap (single frame: should not happen)
  if (data) {
    std::memcpy(static_cast<uint8_t*>(uniforms_.mapped) + offset, data, size);
  } else {
    std::memset(static_cast<uint8_t*>(uniforms_.mapped) + offset, 0, size);
  }
  uniforms_used_ = offset + size;
  return offset;
}

ResolveInfo Renderer::GetResolveInfo(const uint32_t* registers) {
  ResolveInfo info = ResolveInfoFromRegisters(registers);
  std::memcpy(regs_.values, registers, sizeof(regs_.values));
  const RegisterFile& regs = regs_;
  const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2) return info;
  const uint32_t vertex_address = (fetch.address << 2) & 0x1FFFFFFF;
  if (vertex_address > kSharedMemorySize - 24) return info;
  const float half_pixel =
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  int32_t fixed[6];
  for (uint32_t i = 0; i < 6; ++i) {
    float value;
    std::memcpy(&value, guest_memory_ + vertex_address + i * 4, 4);
    value = xenos::GpuSwap(value, fetch.endian) + half_pixel;
    fixed[i] = int32_t(std::lrint(std::clamp(value, -32768.0f, 32767.0f) * 256.0f));
  }
  int32_t x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  int32_t y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  int32_t x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
  int32_t y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    x0 += window_offset.window_x_offset;
    x1 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset;
    y1 += window_offset.window_y_offset;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, false);
  const int32_t scissor_right = int32_t(scissor.offset[0] + scissor.extent[0]);
  const int32_t scissor_bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
  x0 = std::clamp(x0, int32_t(scissor.offset[0]), scissor_right);
  y0 = std::clamp(y0, int32_t(scissor.offset[1]), scissor_bottom);
  x1 = std::clamp(x1, int32_t(scissor.offset[0]), scissor_right);
  y1 = std::clamp(y1, int32_t(scissor.offset[1]), scissor_bottom);
  const int32_t align = int32_t(xenos::kResolveAlignmentPixels);
  x0 &= ~(align - 1);
  y0 &= ~(align - 1);
  x1 = (x1 + align - 1) & ~(align - 1);
  y1 = (y1 + align - 1) & ~(align - 1);
  const int32_t pitch_aligned = int32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) & ~(align - 1);
  x1 = std::min(x1, pitch_aligned);
  x0 = std::min(x0, x1);
  y0 = std::min(y0, y1);
  info.has_rect = true;
  info.rect_x0 = x0;
  info.rect_y0 = y0;
  info.rect_x1 = x1;
  info.rect_y1 = y1;

  // Destination: the rectangle lands at the same coordinates in the texture
  // at the copy base; a base not aligned to a 4 KB tile row moves it right.
  auto copy_dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  uint32_t bits_per_pixel = 32;
  if (!info.source.depth) {
    const FormatInfo* format_info = FormatInfo::Get(xenos::TextureFormat(copy_dest_info.copy_dest_format));
    if (format_info) bits_per_pixel = format_info->bits_per_pixel;
  }
  const uint32_t bpp_log2 = rex::log2_floor(std::max(bits_per_pixel >> 3, 1u));
  if (!copy_dest_info.copy_dest_array) {
    const uint32_t macro_tile_bytes_log2 = 2 * xenos::kTextureTileWidthHeightLog2 + bpp_log2;
    const uint32_t phase =
        (info.dest_address & (xenos::kTextureSubresourceAlignmentBytes - 1)) >> macro_tile_bytes_log2;
    info.dest_address -= phase << macro_tile_bytes_log2;
    info.dest_offset_x = int32_t(phase << xenos::kTextureTileWidthHeightLog2);
  }
  // Guest memory destination (tiled), as the hardware writes it.
  info.dest_pitch_aligned = (regs.Get<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch + 31) & ~31u;
  info.dest_bpp_log2 = bpp_log2;
  info.dest_endian = uint32_t(copy_dest_info.copy_dest_endian);
  info.dest_number = uint32_t(copy_dest_info.copy_dest_number);
  info.depth_float24 =
      info.source.depth && regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
  if ((bpp_log2 == 2 || bpp_log2 == 3) && !copy_dest_info.copy_dest_array && x1 > x0 && y1 > y0) {
    const uint32_t dx0 = uint32_t(x0 + info.dest_offset_x), dx1 = uint32_t(x1 + info.dest_offset_x);
    info.extent_start = info.dest_address + texture_util::GetTiledAddressLowerBound2D(
                                                dx0, uint32_t(y0), info.dest_pitch_aligned, bpp_log2);
    const uint32_t end = info.dest_address + texture_util::GetTiledAddressUpperBound2D(
                                                 dx1, uint32_t(y1), info.dest_pitch_aligned, bpp_log2);
    if (end > info.extent_start && end <= kSharedMemorySize) {
      info.extent_length = end - info.extent_start;
      info.memory_write = true;
    }
  }

  // The image keeps the guest's destination size (textures sample it with
  // normalized coordinates); only the phase offset widens it.
  info.dest_width += uint32_t(info.dest_offset_x);
  if (info.source.depth) {
    info.dest_format = regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                           ? uint32_t(xenos::TextureFormat::k_24_8_FLOAT)
                           : uint32_t(xenos::TextureFormat::k_24_8);
  }
  return info;
}

bool Renderer::UploadGuestRange(uint32_t address, uint32_t size) {
  VkCommandBuffer cmd = current_cmd_ ? current_cmd_ : context_.frame_command_buffer();
  address &= 0x1FFFFFFF;
  size = std::min<uint32_t>(size, uint32_t(kSharedMemorySize) - address);
  if (!size) return true;
  if (!shared_memory_cleared_) {
    shared_memory_cleared_ = true;
    EndRendering(cmd);
    vkCmdFillBuffer(cmd, shared_memory_.buffer, 0, VK_WHOLE_SIZE, 0);
  }
  // Staging: the main buffer, then extra chunks for big frames (freed at the
  // end of the frame).
  vk::Buffer* staging = &upload_staging_;
  VkDeviceSize offset = upload_staging_used_;
  if (offset + size > upload_staging_.size) {
    vk::Buffer chunk;
    if (!context_.CreateBuffer(chunk, std::max<VkDeviceSize>(size, 64u * 1024 * 1024),
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
      return false;
    }
    extra_staging_.push_back(chunk);
    staging = &extra_staging_.back();
    offset = 0;
  } else {
    upload_staging_used_ = (offset + size + 15) & ~VkDeviceSize(15);
  }
  EndRendering(cmd);
  std::memcpy(static_cast<uint8_t*>(staging->mapped) + offset, guest_memory_ + address, size);
  VkBufferCopy copy = {offset, address, size};
  vkCmdCopyBuffer(cmd, staging->buffer, shared_memory_.buffer, 1, &copy);
  VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = shared_memory_.buffer;
  barrier.offset = address;
  barrier.size = size;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 1, &barrier, 0, nullptr);
  return true;
}

void Renderer::SyncGuestRange(VkCommandBuffer cmd, uint32_t address, uint32_t size) {
  if (!snapshot_memory_ || !size) return;
  if (request_range) {
    current_cmd_ = cmd;
    request_range(address & 0x1FFFFFFF, size);
    return;
  }
  address &= 0x1FFFFFFF;
  size = std::min<uint32_t>(size, uint32_t(kSharedMemorySize) - address);
  if (!shared_memory_cleared_) {
    shared_memory_cleared_ = true;
    EndRendering(cmd);
    vkCmdFillBuffer(cmd, shared_memory_.buffer, 0, VK_WHOLE_SIZE, 0);
  }
  // Never overwrite what memexport wrote (recently) with the CPU's copy.
  for (auto it = gpu_written_.begin(); it != gpu_written_.end();) {
    if (frame_index_ - it->second.second > 120) {
      it = gpu_written_.erase(it);
      continue;
    }
    if (it->first < address + size && it->second.first > address) return;
    ++it;
  }
  const uint64_t hash = XXH3_64bits(guest_memory_ + address, size);
  auto existing = uploaded_ranges_.find(address);
  static const bool upload_always = std::getenv("XGPU_UPLOAD_ALWAYS") != nullptr;  // debug
  if (!upload_always && existing != uploaded_ranges_.end() && existing->second.first == size &&
      existing->second.second == hash) {
    return;
  }
  if (upload_staging_used_ + size > upload_staging_.size) {
    upload_staging_overflowed_ = true;
    return;
  }
  // Drop entries overlapping [address, address + size).
  const uint32_t end = address + size;
  auto it = uploaded_ranges_.lower_bound(address > uploaded_max_size_ ? address - uploaded_max_size_ : 0);
  while (it != uploaded_ranges_.end() && it->first < end) {
    if (it->first + it->second.first > address) {
      it = uploaded_ranges_.erase(it);
    } else {
      ++it;
    }
  }
  uploaded_ranges_[address] = {size, hash};
  uploaded_max_size_ = std::max(uploaded_max_size_, size);
  EndRendering(cmd);
  std::memcpy(static_cast<uint8_t*>(upload_staging_.mapped) + upload_staging_used_, guest_memory_ + address, size);
  VkBufferCopy copy = {upload_staging_used_, address, size};
  vkCmdCopyBuffer(cmd, upload_staging_.buffer, shared_memory_.buffer, 1, &copy);
  upload_staging_used_ = (upload_staging_used_ + size + 15) & ~VkDeviceSize(15);
  VkBufferMemoryBarrier barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = shared_memory_.buffer;
  barrier.offset = address;
  barrier.size = size;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void Renderer::EndFrame() {
  ++frame_index_;
  for (vk::Buffer& chunk : extra_staging_) context_.DestroyBuffer(chunk);
  extra_staging_.clear();
  current_cmd_ = VK_NULL_HANDLE;
  if (upload_staging_overflowed_) {
    std::fprintf(stderr, "xgpu: upload staging overflowed this frame\n");
    upload_staging_overflowed_ = false;
  }
  upload_staging_used_ = 0;
  uniforms_used_ = 0;
  converted_indices_used_ = 0;
  vkResetDescriptorPool(context_.device(), descriptor_pool_, 0);
}

void Renderer::EndRendering(VkCommandBuffer cmd) {
  if (!rendering_.active) return;
  vkCmdEndRendering(cmd);
  rendering_ = RenderingState();
}

void Renderer::Draw(VkCommandBuffer cmd, const pm4::Draw& draw) {
  std::memcpy(regs_.values, draw.registers, sizeof(regs_.values));
  const RegisterFile& regs = regs_;
  static const bool trace_env = std::getenv("XGPU_TRACE") != nullptr;
  const bool trace = trace_env || g_trace;
  const char* outcome = "drawn";
  struct TraceOnExit {
    const bool& enabled;
    const pm4::Draw& draw;
    const RegisterFile& regs;
    const char*& outcome;
    ~TraceOnExit() {
      if (!enabled) return;
      auto init = regs.Get<reg::VGT_DRAW_INITIATOR>();
      std::printf("draw %4u mode %u prim %2u n %5u surf %08X c0 %08X c1 %08X c2 %08X c3 %08X d %08X "
                  "mask %04X depthctl %08X blend %08X cc %08X vs %016llX ps %016llX: %s\n",
                  draw.index, uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode), uint32_t(init.prim_type),
                  uint32_t(init.num_indices), regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
                  regs[XE_GPU_REG_RB_COLOR1_INFO], regs[XE_GPU_REG_RB_COLOR2_INFO], regs[XE_GPU_REG_RB_COLOR3_INFO],
                  regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_DEPTHCONTROL],
                  regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
                  (unsigned long long)draw.vertex_shader_hash, (unsigned long long)draw.pixel_shader_hash,
                  outcome);
      if (std::getenv("XGPU_TRACE_CONSTANTS") || g_trace) {
        const float* c = reinterpret_cast<const float*>(&regs[XE_GPU_REG_SHADER_CONSTANT_256_X]);
        std::printf("  ps c0-7:");
        for (uint32_t i = 0; i < 32; ++i) std::printf(" %g", c[i]);
        std::printf("\n");
      }
    }
  } trace_on_exit{trace, draw, regs, outcome};

  const xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  SpirvShader* vertex_shader = GetShader(draw.vertex_shader_hash, false);
  if (!vertex_shader) {
    ++stats_.skipped_no_shader;
    outcome = "skipped_no_shader";
    return;
  }
  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  // Vertex shaders exporting to memory (memexport) run even without
  // rasterization - later draws consume what they write.
  const bool vs_memexport = vertex_shader->memexport_eM_written() != 0;
  bool raster_discard = false;
  if (!draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal) ||
      regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    if (!vs_memexport) {
      ++stats_.skipped_no_raster;
      outcome = "skipped_no_raster";
      return;
    }
    raster_discard = true;
  }
  SpirvShader* pixel_shader = nullptr;
  if (const char* disasm = std::getenv("XGPU_DISASM")) {  // debug: print a shader once
    static bool printed = false;
    const uint64_t want = std::strtoull(disasm, nullptr, 16);
    for (uint64_t hash : {draw.vertex_shader_hash, draw.pixel_shader_hash}) {
      if (hash == want && !printed) {
        SpirvShader* shader = GetShader(hash, hash == draw.pixel_shader_hash);
        if (shader) std::printf("%s\n", shader->ucode_disassembly().c_str());
        printed = true;
      }
    }
  }
  if (edram_mode == xenos::EdramMode::kColorDepth && !raster_discard) {
    pixel_shader = GetShader(draw.pixel_shader_hash, true);
    if (pixel_shader && !draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
      pixel_shader = nullptr;
    }
  }

  // Primitive processing.
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  if (regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select == xenos::VGTOutputPath::kTessellationEnable) {
    ++stats_.skipped_primitive;
    outcome = "skipped_primitive";
    return;
  }
  const bool dma = initiator.source_select == xenos::SourceSelect::kDMA;
  if (!dma && initiator.source_select != xenos::SourceSelect::kAutoIndex) {
    ++stats_.skipped_primitive;
    outcome = "skipped_primitive";
    return;
  }
  uint32_t guest_count = initiator.num_indices;
  if (dma) guest_count = std::min<uint32_t>(guest_count, dma_size.num_words);
  const bool index32 = initiator.index_size == xenos::IndexFormat::kInt32;
  const uint32_t index_size = index32 ? 4 : 2;
  const uint32_t index_base = (regs[XE_GPU_REG_VGT_DMA_BASE] & 0x1FFFFFFF) & ~(index_size - 1);
  xenos::Endian guest_endian = dma_size.swap_mode;
  if (!index32 && guest_endian != xenos::Endian::kNone && guest_endian != xenos::Endian::k8in16) {
    guest_endian = guest_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  const bool reset_enabled = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;

  enum class IndexSource { kNone, kGuest, kBuiltin, kConverted } index_source = IndexSource::kNone;
  Shader::HostVertexShaderType host_vs_type = Shader::HostVertexShaderType::kVertex;
  VkPrimitiveTopology topology;
  bool restart = false;
  uint32_t host_count = guest_count;
  VkDeviceSize converted_offset = 0;
  bool converted_32 = index32;
  bool shader_index_load = false;  // rectangle / point expansion with DMA indices
  switch (initiator.prim_type) {
    case xenos::PrimitiveType::kPointList:
    case xenos::PrimitiveType::kRectangleList: {
      const bool rect = initiator.prim_type == xenos::PrimitiveType::kRectangleList;
      host_vs_type = rect ? Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
                          : Shader::HostVertexShaderType::kPointListAsTriangleStrip;
      const uint32_t primitives = rect ? guest_count / 3 : guest_count;
      if (!primitives || primitives > kMaxBuiltinStrips) {
        ++stats_.skipped_primitive;
        outcome = "skipped_primitive";
      outcome = "skipped_primitive";
    outcome = "skipped_primitive";
        return;
      }
      host_count = TwoTriangleStripIndexCount(primitives);
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      restart = true;
      index_source = IndexSource::kBuiltin;
      shader_index_load = dma;
      break;
    }
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kLineLoop:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      restart = dma && reset_enabled;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleFan:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      restart = dma && reset_enabled;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      restart = dma && reset_enabled;
      break;
    case xenos::PrimitiveType::kQuadList: {
      // Quads to triangle lists; guest indices are kept in guest byte order
      // (the shader swaps them).
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      const uint32_t quads = guest_count / 4;
      host_count = quads * 6;
      converted_32 = dma ? index32 : false;
      const uint32_t size = converted_32 ? 4 : 2;
      converted_offset = (converted_indices_used_ + 3) & ~VkDeviceSize(3);
      if (!quads || converted_offset + host_count * size > converted_indices_.size) {
        ++stats_.skipped_primitive;
        outcome = "skipped_primitive";
      outcome = "skipped_primitive";
    outcome = "skipped_primitive";
        return;
      }
      uint8_t* out = static_cast<uint8_t*>(converted_indices_.mapped) + converted_offset;
      static const uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
      for (uint32_t q = 0; q < quads; ++q) {
        for (uint32_t k = 0; k < 6; ++k) {
          const uint32_t v = q * 4 + kOrder[k];
          if (dma) {
            std::memcpy(out, guest_memory_ + index_base + v * size, size);
          } else {
            const uint16_t v16 = uint16_t(v);
            std::memcpy(out, &v16, 2);
          }
          out += size;
        }
      }
      converted_indices_used_ = converted_offset + host_count * size;
      index_source = IndexSource::kConverted;
      break;
    }
    default:
      ++stats_.skipped_primitive;
      outcome = "skipped_primitive";
    outcome = "skipped_primitive";
      return;
  }
  if (!host_count) return;
  if (index_source == IndexSource::kNone && dma) index_source = IndexSource::kGuest;

  // Shader variants.
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  uint32_t ps_param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      pixel_shader ? vertex_shader->writes_interpolators() &
                         pixel_shader->GetInterpolatorInputMask(
                             sq_program_cntl, regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos)
                   : 0;
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  const uint32_t color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets()) : 0;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  Translator::Modification vs_mod(translator_->GetDefaultVertexShaderModification(
      vertex_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg), host_vs_type));
  {
    vs_mod.vertex.interpolator_mask = interpolator_mask;
    vs_mod.vertex.tessellation_mode = 0;
    const uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
    vs_mod.vertex.user_clip_plane_cull = user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena;
    vs_mod.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
    vs_mod.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
    if (host_vs_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
      vs_mod.vertex.output_point_parameters = uint32_t(ps_param_gen_pos != UINT32_MAX);
    } else {
      vs_mod.vertex.output_point_parameters = 0;
    }
    vs_mod.vertex.vertex_kill_and =
        uint32_t((vertex_shader->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
                 !pa_cl_clip_cntl.vtx_kill_or);
  }
  Translator::Modification ps_mod(0);
  if (pixel_shader) {
    ps_mod.value = translator_->GetDefaultPixelShaderModification(
        pixel_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                                regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(initiator.prim_type == xenos::PrimitiveType::kPointList);
    }
    using DepthStencilMode = Translator::Modification::DepthStencilMode;
    ps_mod.pixel.depth_stencil_mode =
        pixel_shader->implicit_early_z_write_allowed() &&
                (!pixel_shader->writes_color_target(0) ||
                 !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    ps_mod.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
    ps_mod.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
    if (pixel_shader->writes_color_target(0)) {
      auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
      if ((blend.color_comb_fcn == xenos::BlendOp::kMin || blend.color_comb_fcn == xenos::BlendOp::kMax) &&
          blend.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend.color_destblend == xenos::BlendFactor::kOne) {
        ps_mod.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
      }
      if ((blend.alpha_comb_fcn == xenos::BlendOp::kMin || blend.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          blend.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend.alpha_destblend == xenos::BlendFactor::kOne) {
        ps_mod.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
      }
    }
  }

  auto translate = [&](SpirvShader* shader, uint64_t modification) -> Shader::Translation* {
    Shader::Translation* translation = shader->GetOrCreateTranslation(modification);
    if (!translation->is_translated()) {
      try {
        translator_->TranslateAnalyzedShader(*translation);
      } catch (const std::exception&) {
      }
    }
    return translation->is_valid() ? translation : nullptr;
  };
  Shader::Translation* vs_translation = translate(vertex_shader, vs_mod.value);
  Shader::Translation* ps_translation = pixel_shader ? translate(pixel_shader, ps_mod.value) : nullptr;
  if (!vs_translation || (pixel_shader && !ps_translation)) {
    ++stats_.skipped_translation;
    outcome = "skipped_translation";
    return;
  }
  VkShaderModule vs_module = GetModule(*vs_translation);
  VkShaderModule ps_module = ps_translation ? GetModule(*ps_translation) : VK_NULL_HANDLE;
  if (!vs_module || (ps_translation && !ps_module)) {
    ++stats_.skipped_translation;
    outcome = "skipped_translation";
    return;
  }

  // Render targets.
  const uint32_t samples = 1u << uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples);
  RenderingState target;
  target.active = true;
  vk::Image* color_images[4] = {};
  vk::Image* depth_image = nullptr;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (i * 4)) & 0xF)) continue;
    color_images[i] = render_targets_.Get(ColorKey(regs, i), 0);
    if (!color_images[i]) continue;
    target.color[i] = color_images[i]->view;
    width = std::min(width, color_images[i]->width);
    height = std::min(height, color_images[i]->height);
  }
  const bool depth_used = (depth_control.z_enable || depth_control.stencil_enable) && !raster_discard;
  if (depth_used) {
    depth_image = render_targets_.Get(DepthKey(regs), 0);
    if (depth_image) {
      target.depth = depth_image->view;
      width = std::min(width, depth_image->width);
      height = std::min(height, depth_image->height);
    }
  }
  if (width == UINT32_MAX) {
    if (!vs_memexport) {
      ++stats_.skipped_no_target;
      outcome = "skipped_no_target";
      return;
    }
    raster_discard = true;
    pixel_shader = nullptr;
    width = height = 1;
  }
  if (raster_discard) {
    ++stats_.memexport_only;
    outcome = "memexport_only";
  }
  // Remember what the GPU writes, so CPU copies never overwrite it.
  if (snapshot_memory_) {
    std::vector<draw_util::MemExportRange> ranges;
    if (vs_memexport) draw_util::AddMemExportRanges(regs, *vertex_shader, ranges);
    if (pixel_shader && pixel_shader->memexport_eM_written()) {
      draw_util::AddMemExportRanges(regs, *pixel_shader, ranges);
    }
    for (const draw_util::MemExportRange& range : ranges) {
      const uint32_t start = (range.base_address_dwords << 2) & 0x1FFFFFFF;
      if (range_written_by_gpu) {
        // Make the pages valid first (the shader may write only part of it).
        if (request_range) {
          current_cmd_ = context_.frame_command_buffer();
          request_range(start, range.size_bytes);
        }
        range_written_by_gpu(start, range.size_bytes);
      } else {
        gpu_written_[start] = {start + range.size_bytes, frame_index_};
      }
    }
  }
  target.width = width;
  target.height = height;

  // Textures (may record uploads, so outside rendering).
  const auto& vs_textures = vertex_shader->GetTextureBindingsAfterTranslation();
  const auto& vs_samplers = vertex_shader->GetSamplerBindingsAfterTranslation();
  static const std::vector<SpirvShader::TextureBinding> kNoTextures;
  static const std::vector<SpirvShader::SamplerBinding> kNoSamplers;
  const auto& ps_textures = pixel_shader ? pixel_shader->GetTextureBindingsAfterTranslation() : kNoTextures;
  const auto& ps_samplers = pixel_shader ? pixel_shader->GetSamplerBindingsAfterTranslation() : kNoSamplers;
  const uint32_t used_texture_mask = vertex_shader->GetUsedTextureMaskAfterTranslation() |
                                     (pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  std::vector<VkDescriptorImageInfo> image_infos[2];
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const auto& textures = stage ? ps_textures : vs_textures;
    const auto& samplers = stage ? ps_samplers : vs_samplers;
    for (const auto& binding : textures) {
      if (rendering_.active) EndRendering(cmd);
      image_infos[stage].push_back(
          {VK_NULL_HANDLE, textures_.GetView(cmd, regs, binding), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    for (const auto& binding : samplers) {
      image_infos[stage].push_back({textures_.GetSampler(regs, binding), VK_NULL_HANDLE,
                                    VK_IMAGE_LAYOUT_UNDEFINED});
    }
  }

  const PipelineLayout* layout = GetPipelineLayout(uint32_t(vs_textures.size()), uint32_t(vs_samplers.size()),
                                                   uint32_t(ps_textures.size()), uint32_t(ps_samplers.size()));

  // Pipeline.
  auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_translation = vs_translation;
  key.pixel_translation = ps_translation;
  key.layout = layout->layout;
  key.topology = topology;
  key.restart = restart;
  if (primitive_polygonal) {
    key.cull_mode = (mode_cntl.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                    (mode_cntl.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    key.front_face_cw = mode_cntl.face != 0;
  }
  key.depth_clamp = pa_cl_clip_cntl.clip_disable;
  key.samples = raster_discard ? 1 : samples;
  key.rasterizer_discard = raster_discard;
  if (depth_image) {
    key.depth_format = depth_image->format;
    key.depth_test = 1;
    key.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    key.depth_compare = depth_control.z_enable ? uint32_t(depth_control.zfunc) : 7;
    if (depth_control.stencil_enable) {
      key.stencil_test = 1;
      key.stencil_front[0] = uint32_t(depth_control.stencilfail);
      key.stencil_front[1] = uint32_t(depth_control.stencilzpass);
      key.stencil_front[2] = uint32_t(depth_control.stencilzfail);
      key.stencil_front[3] = uint32_t(depth_control.stencilfunc);
      if (primitive_polygonal && depth_control.backface_enable) {
        key.stencil_back[0] = uint32_t(depth_control.stencilfail_bf);
        key.stencil_back[1] = uint32_t(depth_control.stencilzpass_bf);
        key.stencil_back[2] = uint32_t(depth_control.stencilzfail_bf);
        key.stencil_back[3] = uint32_t(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, sizeof(key.stencil_back));
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!color_images[i]) continue;
    key.color_formats[i] = color_images[i]->format;
    key.write_mask[i] = (color_mask >> (i * 4)) & 0xF;
    auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    uint32_t* b = key.blend[i];
    b[1] = BlendFactor(uint32_t(blend.color_srcblend), false);
    b[2] = BlendFactor(uint32_t(blend.color_destblend), false);
    b[3] = BlendOp(blend.color_comb_fcn);
    b[4] = BlendFactor(uint32_t(blend.alpha_srcblend), true);
    b[5] = BlendFactor(uint32_t(blend.alpha_destblend), true);
    b[6] = BlendOp(blend.alpha_comb_fcn);
    b[0] = !(b[1] == VK_BLEND_FACTOR_ONE && b[2] == VK_BLEND_FACTOR_ZERO && b[3] == VK_BLEND_OP_ADD &&
             b[4] == VK_BLEND_FACTOR_ONE && b[5] == VK_BLEND_FACTOR_ZERO && b[6] == VK_BLEND_OP_ADD);
  }
  std::string key_bytes(reinterpret_cast<const char*>(&key), sizeof(key));
  VkPipeline& pipeline = pipelines_[key_bytes];
  if (!pipeline) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_module;
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = topology;
    input_assembly.primitiveRestartEnable = restart;
    VkPipelineViewportStateCreateInfo viewport_state = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = key.depth_clamp;
    raster.rasterizerDiscardEnable = key.rasterizer_discard;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = key.cull_mode;
    raster.frontFace = key.front_face_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = VK_TRUE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VkSampleCountFlagBits(samples);
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    static const bool no_depth_test = std::getenv("XGPU_NO_DEPTH_TEST") != nullptr;  // debug
    depth_stencil.depthTestEnable = key.depth_test && !no_depth_test;
    depth_stencil.depthWriteEnable = key.depth_write;
    depth_stencil.depthCompareOp = VkCompareOp(key.depth_compare);
    depth_stencil.stencilTestEnable = key.stencil_test;
    auto stencil = [](const uint32_t* s) {
      VkStencilOpState state = {};
      state.failOp = VkStencilOp(s[0]);
      state.passOp = VkStencilOp(s[1]);
      state.depthFailOp = VkStencilOp(s[2]);
      state.compareOp = VkCompareOp(s[3]);
      return state;
    };
    depth_stencil.front = stencil(key.stencil_front);
    depth_stencil.back = stencil(key.stencil_back);
    VkPipelineColorBlendAttachmentState attachments[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t* b = key.blend[i];
      attachments[i].blendEnable = b[0];
      attachments[i].srcColorBlendFactor = VkBlendFactor(b[1]);
      attachments[i].dstColorBlendFactor = VkBlendFactor(b[2]);
      attachments[i].colorBlendOp = VkBlendOp(b[3]);
      attachments[i].srcAlphaBlendFactor = VkBlendFactor(b[4]);
      attachments[i].dstAlphaBlendFactor = VkBlendFactor(b[5]);
      attachments[i].alphaBlendOp = VkBlendOp(b[6]);
      attachments[i].colorWriteMask = key.write_mask[i];
    }
    VkPipelineColorBlendStateCreateInfo blend_state = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 4;
    blend_state.pAttachments = attachments;
    static const VkDynamicState kDynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(std::size(kDynamic));
    dynamic.pDynamicStates = kDynamic;
    VkFormat color_formats[4];
    for (uint32_t i = 0; i < 4; ++i) color_formats[i] = VkFormat(key.color_formats[i]);
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = VkFormat(key.depth_format);
    rendering.stencilAttachmentFormat =
        key.depth_format && key.depth_format != VK_FORMAT_D32_SFLOAT ? VkFormat(key.depth_format)
                                                                      : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = ps_module ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic;
    info.layout = layout->layout;
    if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
    } else {
      ++stats_.pipelines;
    }
  }
  if (!pipeline) {
    ++stats_.skipped_pipeline;
    outcome = "skipped_pipeline";
    return;
  }

  // Viewport and system constants.
  draw_util::GetViewportInfoArgs viewport_args{};
  viewport_args.Setup(1, 1, rex::divisors::MagicDiv(1), rex::divisors::MagicDiv(1), false, 16384, 16384,
                      true, depth_control, false, true, pixel_shader && pixel_shader->writes_depth());
  viewport_args.SetupRegisterValues(regs);
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(&viewport_args, viewport_info);

  Translator::SystemConstants sc;
  std::memset(&sc, 0, sizeof(sc));
  {
    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    uint32_t flags = 0;
    if (shader_index_load) {
      flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
      if (index32) flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    if (vte.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
    if (vte.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
    if (vte.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
    if (primitive_polygonal) flags |= Translator::kSysFlag_PrimitivePolygonal;
    if (draw_util::IsPrimitiveLine(regs)) flags |= Translator::kSysFlag_PrimitiveLine;
    flags |= uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
    if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      flags |= Translator::kSysFlag_DepthFloat24;
    }
    const xenos::CompareFunction alpha_func =
        colorcontrol.alpha_test_enable ? colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
    flags |= uint32_t(alpha_func) << Translator::kSysFlag_AlphaPassIfLess_Shift;
    for (uint32_t i = 0; i < 4; ++i) {
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      if (info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
      }
      float bias;
      const int32_t exp_bias = info.color_exp_bias;
      *reinterpret_cast<int32_t*>(&bias) = int32_t(0x3F800000) + (exp_bias << 23);
      sc.color_exp_bias[i] = bias;
    }
    sc.flags = flags;
    sc.vertex_index_load_address = shader_index_load ? index_base : 0;
    sc.vertex_index_endian =
        (dma && index_source != IndexSource::kBuiltin) || shader_index_load ? guest_endian : xenos::Endian::kNone;
    if (index_source == IndexSource::kConverted && !dma) sc.vertex_index_endian = xenos::Endian::kNone;
    sc.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
    sc.vertex_index_min = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
    sc.vertex_index_max = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
    if (!pa_cl_clip_cntl.clip_disable) {
      float* out = sc.user_clip_planes[0];
      for (uint32_t i = 0; i < 6; ++i) {
        if (!(pa_cl_clip_cntl.ucp_ena & (1u << i))) continue;
        std::memcpy(out, &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 16);
        out += 4;
      }
    }
    sc.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
    sc.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    for (uint32_t i = 0; i < 3; ++i) {
      sc.ndc_scale[i] = viewport_info.ndc_scale[i];
      sc.ndc_offset[i] = viewport_info.ndc_offset[i];
    }
    if (initiator.prim_type == xenos::PrimitiveType::kPointList) {
      auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
      auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
      sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
      sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
      sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
      sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
      sc.point_screen_diameter_to_ndc_radius[0] = 1.0f / std::max(viewport_info.xy_extent[0], 1u);
      sc.point_screen_diameter_to_ndc_radius[1] = 1.0f / std::max(viewport_info.xy_extent[1], 1u);
    }
    sc.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    sc.alpha_to_mask = colorcontrol.alpha_to_mask_enable ? (colorcontrol.value >> 24) | (1u << 8) : 0;
    sc.zpd_counter_index = UINT32_MAX;
    textures_.WriteSystemConstants(regs, used_texture_mask, sc);
  }

  // Constant buffers.
  VkDescriptorBufferInfo buffers[Translator::kConstantBufferCount];
  auto put = [&](uint32_t index, const void* data, size_t size) {
    buffers[index] = {uniforms_.buffer, AllocateUniform(data, size), size};
  };
  put(Translator::kConstantBufferSystem, &sc, sizeof(sc));
  auto pack_floats = [&](const Shader* shader, uint32_t base, uint32_t index) {
    std::vector<float> packed;
    if (shader) {
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = map.float_bitmap[i];
        while (bits) {
          const uint32_t bit = uint32_t(__builtin_ctzll(bits));
          bits &= bits - 1;
          const float* values = reinterpret_cast<const float*>(&regs[base + (i << 8) + (bit << 2)]);
          packed.insert(packed.end(), values, values + 4);
        }
      }
    }
    if (packed.empty()) packed.resize(4, 0.0f);
    put(index, packed.data(), packed.size() * sizeof(float));
  };
  pack_floats(vertex_shader, XE_GPU_REG_SHADER_CONSTANT_000_X, Translator::kConstantBufferFloatVertex);
  pack_floats(pixel_shader, XE_GPU_REG_SHADER_CONSTANT_256_X, Translator::kConstantBufferFloatPixel);
  put(Translator::kConstantBufferBoolLoop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 4 * (8 + 32));
  put(Translator::kConstantBufferFetch, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 4 * 6 * 32);
  put(Translator::kConstantBufferClipPlanes, sc.user_clip_planes, sizeof(Translator::ClipPlaneConstants));
  put(Translator::kConstantBufferTessellation, nullptr, sizeof(Translator::TessellationConstants));

  // Descriptor sets.
  VkDevice device = context_.device();
  VkDescriptorSetLayout set_layouts[3] = {layout_constants_, layout->textures_vertex, layout->textures_pixel};
  VkDescriptorSet sets[3] = {};
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = descriptor_pool_;
  alloc.descriptorSetCount = 3;
  alloc.pSetLayouts = set_layouts;
  if (vkAllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS) {
    ++stats_.skipped_pipeline;
    outcome = "skipped_pipeline";
    return;
  }
  std::vector<VkWriteDescriptorSet> writes;
  for (uint32_t i = 0; i < Translator::kConstantBufferCount; ++i) {
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[0];
    w.dstBinding = i;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &buffers[i];
    writes.push_back(w);
  }
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const uint32_t texture_count = uint32_t((stage ? ps_textures : vs_textures).size());
    for (uint32_t i = 0; i < image_infos[stage].size(); ++i) {
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets[1 + stage];
      w.dstBinding = i;
      w.descriptorCount = 1;
      w.descriptorType = i < texture_count ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
      w.pImageInfo = &image_infos[stage][i];
      writes.push_back(w);
    }
  }
  vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);

  // Snapshot the guest memory this draw reads (vertex buffers, indices).
  if (snapshot_memory_) {
    const Shader::ConstantRegisterMap& map = vertex_shader->constant_register_map();
    for (uint32_t i = 0; i < std::size(map.vertex_fetch_bitmap); ++i) {
      uint32_t bits = map.vertex_fetch_bitmap[i];
      while (bits) {
        const uint32_t bit = uint32_t(__builtin_ctz(bits));
        bits &= bits - 1;
        const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i * 32 + bit);
        if (fetch.type == xenos::FetchConstantType::kVertex) {
          SyncGuestRange(cmd, fetch.address << 2, fetch.size << 2);
        }
      }
    }
    if (dma && (index_source == IndexSource::kGuest || shader_index_load)) {
      SyncGuestRange(cmd, index_base, guest_count * index_size);
    }
  }

  // EDRAM ownership: bring in what other formats' views wrote to these tiles.
  for (uint32_t i = 0; i < 4; ++i) {
    if (!color_images[i]) continue;
    const RenderTargetKey key = ColorKey(regs, i);
    if (render_targets_.LastWriteIsOther(key)) {
      EndRendering(cmd);
      render_targets_.TransferOwnership(cmd, key);
    }
  }

  // Rendering scope.
  if (!(rendering_ == target)) {
    EndRendering(cmd);
    VkRenderingAttachmentInfo colors[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      if (!color_images[i]) continue;
      vk::Context::Transition(cmd, *color_images[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      colors[i].imageView = color_images[i]->view;
      colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth_image) {
      vk::Context::Transition(cmd, *depth_image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      depth.imageView = depth_image->view;
      depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {width, height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 4;
    info.pColorAttachments = colors;
    info.pDepthAttachment = depth_image ? &depth : nullptr;
    info.pStencilAttachment =
        depth_image && depth_image->format != VK_FORMAT_D32_SFLOAT ? &depth : nullptr;
    vkCmdBeginRendering(cmd, &info);
    rendering_ = target;
    ++stats_.passes;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, 0, 1, &shared_memory_set_, 0,
                          nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, 1, 3, sets, 0, nullptr);

  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport = {float(viewport_info.xy_offset[0]), float(viewport_info.xy_offset[1]),
                float(viewport_info.xy_extent[0]), float(viewport_info.xy_extent[1]), viewport_info.z_min,
                viewport_info.z_max};
  } else {
    viewport = {-1.0f, -1.0f, 1.0f, 1.0f, viewport_info.z_min, viewport_info.z_max};
  }
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  VkRect2D scissor_rect;
  scissor_rect.offset = {int32_t(std::min(scissor.offset[0], width)), int32_t(std::min(scissor.offset[1], height))};
  scissor_rect.extent = {std::min(scissor.extent[0], width - uint32_t(scissor_rect.offset.x)),
                         std::min(scissor.extent[1], height - uint32_t(scissor_rect.offset.y))};
  vkCmdSetScissor(cmd, 0, 1, &scissor_rect);
  {
    float slope = 0.0f, constant = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, slope, constant);
    constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
                    ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                    : draw_util::kD3D10PolygonOffsetFactorFloat24;
    slope *= xenos::kPolygonOffsetScaleSubpixelUnit;
    vkCmdSetDepthBias(cmd, constant, 0.0f, slope);
  }
  const float blend_constants[4] = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                                    regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  vkCmdSetBlendConstants(cmd, blend_constants);
  {
    auto front = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK);
    auto back = primitive_polygonal && depth_control.backface_enable
                    ? regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF)
                    : front;
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilmask);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilmask);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilwritemask);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilwritemask);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilref);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilref);
  }

  // The EDRAM behind these targets now holds this draw's output.
  for (uint32_t i = 0; i < 4; ++i) {
    if (color_images[i]) render_targets_.MarkWritten(ColorKey(regs, i));
  }
  if (depth_image && (key.depth_write || key.stencil_test)) render_targets_.MarkWritten(DepthKey(regs));

  switch (index_source) {
    case IndexSource::kNone:
      vkCmdDraw(cmd, host_count, 1, 0, 0);
      break;
    case IndexSource::kGuest:
      vkCmdBindIndexBuffer(cmd, shared_memory_.buffer, index_base,
                           index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      vkCmdDrawIndexed(cmd, host_count, 1, 0, 0, 0);
      break;
    case IndexSource::kBuiltin:
      vkCmdBindIndexBuffer(cmd, builtin_indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cmd, host_count, 1, 0, 0, 0);
      break;
    case IndexSource::kConverted:
      vkCmdBindIndexBuffer(cmd, converted_indices_.buffer, converted_offset,
                           converted_32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
      vkCmdDrawIndexed(cmd, host_count, 1, 0, 0, 0);
      break;
  }
  ++stats_.drawn;
}

}  // namespace xgpu
