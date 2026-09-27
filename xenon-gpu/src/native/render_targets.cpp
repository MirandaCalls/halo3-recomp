#include <xgpu/render_targets.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

#include <xgpu/glsl.h>

#include <xxhash.h>

#include <rex/graphics/xenos.h>
#include <rex/math.h>

namespace xgpu {

static RenderTargetKey CanonicalKey(const RenderTargetKey& key);

std::string RenderTargetKey::Describe() const {
  char buf[96];
  if (format & 0x80000000u) {
    std::snprintf(buf, sizeof(buf), "color @%u pitch %u vkfmt %u %ux", base_tiles, pitch,
                  format & 0x7FFFFFFFu, 1u << msaa);
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "%s @%u pitch %u fmt %u %ux", depth ? "depth" : "color", base_tiles,
                pitch, format, 1u << msaa);
  return buf;
}

RenderTargets::~RenderTargets() {
  VkDevice device = context_.device();
  if (device) vkDeviceWaitIdle(device);
  EndFrame();
  for (auto& [key, view] : views_) vkDestroyImageView(device, view, nullptr);
  for (auto& [key, image] : targets_) context_.DestroyImage(image);
  for (auto& [address, image] : resolved_) context_.DestroyImage(image);
  for (auto& [key, pipeline] : resolve_pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
  for (VkShaderModule module : {resolve_vs_, resolve_fs_, resolve_fs_ms_}) {
    if (module) vkDestroyShaderModule(device, module, nullptr);
  }
  if (resolve_layout_) vkDestroyPipelineLayout(device, resolve_layout_, nullptr);
  for (VkPipeline pipeline : {write_pipeline_, write_pipeline_ms_}) {
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
  }
  for (VkShaderModule module : {write_cs_, write_cs_ms_}) {
    if (module) vkDestroyShaderModule(device, module, nullptr);
  }
  if (write_layout_) vkDestroyPipelineLayout(device, write_layout_, nullptr);
  for (auto& [key, pipeline] : transfer_pipelines_) vkDestroyPipeline(device, pipeline, nullptr);
  for (VkShaderModule module : {transfer_fs_, transfer_fs_ms_}) {
    if (module) vkDestroyShaderModule(device, module, nullptr);
  }
  if (transfer_layout_) vkDestroyPipelineLayout(device, transfer_layout_, nullptr);
  if (write_set_layout_) vkDestroyDescriptorSetLayout(device, write_set_layout_, nullptr);
  if (resolve_set_layout_) vkDestroyDescriptorSetLayout(device, resolve_set_layout_, nullptr);
  if (resolve_pool_) vkDestroyDescriptorPool(device, resolve_pool_, nullptr);
  if (resolve_sampler_) vkDestroySampler(device, resolve_sampler_, nullptr);
}

namespace {

constexpr char kResolveVertexShader[] = R"(#version 450
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Copies the source render target into the destination, like the Xenos
// resolve: sample selection / averaging, exponent bias, red/blue swap; the
// destination attachment format does the format conversion.
constexpr char kResolveFragmentShader[] = R"(#version 450
layout(set = 0, binding = 0) uniform SOURCE_TYPE source;
layout(push_constant) uniform Constants {
  float scale;
  int sample_select;
  int swap_rb;
  int is_depth;
  ivec2 offset;
  int alias_mode;
} pc;
layout(location = 0) out vec4 result;
#define ALIAS_MODE pc.alias_mode
SAMPLE_FETCH
void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + pc.offset;
  vec4 value = Resolved(coord, pc.sample_select, pc.is_depth != 0);
  if (pc.is_depth == 0) value *= pc.scale;
  if (pc.swap_rb != 0) value = value.bgra;
  result = value;
}
)";

// Sample access shared by the resolve shaders. EDRAM holds a 4x MSAA surface
// of width W in the same tiles as a 1x surface of width 2W (each pixel's 2x2
// samples side by side), and titles use that: Reach renders HDR at 1x and
// resolves it as 4x for a free 2x downsample. ALIAS_MODE 1: the source is the
// 1x surface, read as 4x; 2: the source is the 4x surface, read as 1x.
constexpr char kSampleFetch[] = R"(
vec4 FetchSample(ivec2 coord, int s) {
#ifdef MULTISAMPLED
  if (ALIAS_MODE == 2) return texelFetch(source, coord >> 1, ((coord.y & 1) << 1) | (coord.x & 1));
  return texelFetch(source, coord, min(s, textureSamples(source) - 1));
#else
  if (ALIAS_MODE == 1) return texelFetch(source, coord * 2 + ivec2(s & 1, s >> 1), 0);
  return texelFetch(source, coord, 0);
#endif
}
int SampleCount() {
#ifdef MULTISAMPLED
  return ALIAS_MODE == 2 ? 1 : textureSamples(source);
#else
  return ALIAS_MODE == 1 ? 4 : 1;
#endif
}
vec4 Resolved(ivec2 coord, int sample_select, bool depth) {
  int samples = SampleCount();
  if (samples == 1 || depth) return FetchSample(coord, 0);
  if (sample_select < 4) return FetchSample(coord, min(sample_select, samples - 1));
  if (sample_select == 4) return 0.5 * (FetchSample(coord, 0) + FetchSample(coord, min(1, samples - 1)));
  if (sample_select == 5) {
    return 0.5 * (FetchSample(coord, min(2, samples - 1)) + FetchSample(coord, min(3, samples - 1)));
  }
  vec4 value = vec4(0.0);
  for (int i = 0; i < samples; ++i) value += FetchSample(coord, i);
  return value / float(samples);
}
)";

// Resolve to guest memory: packs each texel of the resolved rectangle in the
// destination format, applies the endian swap, and stores it at its Xbox 360
// tiled address in the shared memory buffer (the same bits a real resolve
// would leave in memory).
constexpr char kResolveWriteShader[] = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform SOURCE_TYPE source;
layout(set = 0, binding = 1) buffer SharedMemory { uint data[]; } shared_memory;
layout(push_constant) uniform Constants {
  uint dest_base;
  uint pitch;
  uint bpp_log2;
  uint format;
  uint number;
  uint endian;
  uint swap_rb;
  float scale;
  ivec2 source_origin;
  ivec2 dest_origin;
  uvec2 size;
  int sample_select;
  uint depth_mode;  // 0 color, 1 24-bit unorm depth, 2 20e4 float depth
  int alias_mode;
} pc;
#define ALIAS_MODE pc.alias_mode
SAMPLE_FETCH

int TiledOffset2D(int x, int y, uint pitch, uint bpb_log2) {
  int macro = ((x >> 5) + (y >> 5) * int(pitch >> 5)) << (bpb_log2 + 7);
  int micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

uint Swap32(uint v, uint endian) {
  if (endian == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (endian == 2u || endian == 4u || endian == 5u) {
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
  }
  if (endian == 3u) return (v << 16) | (v >> 16);
  return v;
}

uint Float32To20e4(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(f);
  if (u >= 0x3FFFFFF8u) return 0xFFFFFFu;
  if (u < 0x38800000u) {
    uint shift = min(113u - (u >> 23), 24u);
    u = (0x800000u | (u & 0x7FFFFFu)) >> shift;
  } else {
    u += 0xC8000000u;
  }
  u += 3u + ((u >> 3) & 1u);
  return (u >> 3) & 0xFFFFFFu;
}

uint Unorm(float v, float max_value) { return uint(clamp(v, 0.0, 1.0) * max_value + 0.5); }
uint Snorm16(float v) { return uint(int(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xFFFFu; }
uint Pack16(float v) {
  if (pc.number == 1u) return Snorm16(v);
  if (pc.number == 7u) return packHalf2x16(vec2(v, 0.0)) & 0xFFFFu;
  return Unorm(v, 65535.0);
}

void main() {
  uvec2 id = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(id, pc.size))) return;
  ivec2 coord = pc.source_origin + ivec2(id);
  vec4 value = Resolved(coord, pc.sample_select, pc.depth_mode != 0u);
  uint p0 = 0u, p1 = 0u;
  if (pc.depth_mode == 1u) {
    p0 = Unorm(value.r, 16777215.0) << 8;
  } else if (pc.depth_mode == 2u) {
    p0 = Float32To20e4(value.r * 2.0) << 8;
  } else {
    value *= pc.scale;
    if (pc.swap_rb != 0u) value = value.bgra;
    switch (pc.format) {
      case 6u:  // k_8_8_8_8
        if (pc.number == 1u) {
          ivec4 s = ivec4(round(clamp(value, -1.0, 1.0) * 127.0)) & 0xFF;
          p0 = uint(s.r) | (uint(s.g) << 8) | (uint(s.b) << 16) | (uint(s.a) << 24);
        } else {
          p0 = Unorm(value.r, 255.0) | (Unorm(value.g, 255.0) << 8) | (Unorm(value.b, 255.0) << 16) |
               (Unorm(value.a, 255.0) << 24);
        }
        break;
      case 7u:  // k_2_10_10_10
        p0 = Unorm(value.r, 1023.0) | (Unorm(value.g, 1023.0) << 10) | (Unorm(value.b, 1023.0) << 20) |
             (Unorm(value.a, 3.0) << 30);
        break;
      case 25u:  // k_16_16
        p0 = Pack16(value.r) | (Pack16(value.g) << 16);
        break;
      case 26u:  // k_16_16_16_16
        p0 = Pack16(value.r) | (Pack16(value.g) << 16);
        p1 = Pack16(value.b) | (Pack16(value.a) << 16);
        break;
      case 31u:  // k_16_16_FLOAT
        p0 = packHalf2x16(value.rg);
        break;
      case 32u:  // k_16_16_16_16_FLOAT
        p0 = packHalf2x16(value.rg);
        p1 = packHalf2x16(value.ba);
        break;
      default:  // k_32_FLOAT and others with 32 bits
        p0 = floatBitsToUint(value.r);
        break;
    }
  }
  ivec2 dest = pc.dest_origin + ivec2(id);
  uint address = pc.dest_base + uint(TiledOffset2D(dest.x, dest.y, pc.pitch, pc.bpp_log2));
  uint index = address >> 2;
  if (pc.bpp_log2 == 3u) {
    if (pc.endian == 4u) {
      shared_memory.data[index] = Swap32(p1, pc.endian);
      shared_memory.data[index + 1u] = Swap32(p0, pc.endian);
    } else {
      shared_memory.data[index] = Swap32(p0, pc.endian);
      shared_memory.data[index + 1u] = Swap32(p1, pc.endian);
    }
  } else {
    shared_memory.data[index] = Swap32(p0, pc.endian);
  }
}
)";

// EDRAM ownership transfer: reads a texel (sample) of the image last written
// through source_format, packs it into the guest's 32bpp EDRAM bits, and
// unpacks those bits as dest_format - what the other view sees in the tiles.
constexpr char kTransferShader[] = R"(#version 450
layout(set = 0, binding = 0) uniform SOURCE_TYPE source;
layout(push_constant) uniform Constants { uint source_format; uint dest_format; } pc;
layout(location = 0) out vec4 result;

uint F32To7e3(float f) {
  uint u = floatBitsToUint(clamp(f, 0.0, 31.875));
  uint biased = u < 0x3E800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23), 24u)
                                : u - (124u << 23);
  return ((biased + 0x7FFFu + ((biased >> 16) & 1u)) >> 16) & 0x3FFu;
}
float F7e3To32(uint f) {
  f &= 0x3FFu;
  if (f == 0u) return 0.0;
  uint mantissa = f & 0x7Fu, exponent = f >> 7;
  if (exponent == 0u) {
    int lz = 7 - findMSB(mantissa);
    exponent = uint(1 - lz);
    mantissa = (mantissa << uint(lz)) & 0x7Fu;
  }
  return uintBitsToFloat(((exponent + 124u) << 23) | (mantissa << 16));
}
uint U(float v, float m) { return uint(clamp(v, 0.0, 1.0) * m + 0.5); }
uint S16(float v) { return uint(int(round(clamp(v, -1.0, 1.0) * 32767.0))) & 0xFFFFu; }
float FromS16(uint b) { return max(float(int(b << 16) >> 16) / 32767.0, -1.0); }

uint Pack(vec4 v, uint f) {
  switch (f) {
    case 0u: case 1u: return U(v.r, 255.0) | (U(v.g, 255.0) << 8) | (U(v.b, 255.0) << 16) | (U(v.a, 255.0) << 24);
    case 2u: case 10u: return U(v.r, 1023.0) | (U(v.g, 1023.0) << 10) | (U(v.b, 1023.0) << 20) | (U(v.a, 3.0) << 30);
    case 3u: case 12u: return F32To7e3(v.r) | (F32To7e3(v.g) << 10) | (F32To7e3(v.b) << 20) | (U(v.a, 3.0) << 30);
    case 4u: return S16(v.r) | (S16(v.g) << 16);
    case 6u: return packHalf2x16(v.rg);
    default: return floatBitsToUint(v.r);
  }
}
vec4 Unpack(uint b, uint f) {
  switch (f) {
    case 0u: case 1u: return vec4(b & 255u, (b >> 8) & 255u, (b >> 16) & 255u, b >> 24) / 255.0;
    case 2u: case 10u:
      return vec4(vec3(b & 1023u, (b >> 10) & 1023u, (b >> 20) & 1023u) / 1023.0, float(b >> 30) / 3.0);
    case 3u: case 12u:
      return vec4(F7e3To32(b), F7e3To32(b >> 10), F7e3To32(b >> 20), float(b >> 30) / 3.0);
    case 4u: return vec4(FromS16(b & 0xFFFFu), FromS16(b >> 16), 0.0, 1.0);
    case 6u: return vec4(unpackHalf2x16(b), 0.0, 1.0);
    default: return vec4(uintBitsToFloat(b), 0.0, 0.0, 1.0);
  }
}
void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy);
#ifdef MULTISAMPLED
  vec4 value = texelFetch(source, coord, gl_SampleID);
#else
  vec4 value = texelFetch(source, coord, 0);
#endif
  result = Unpack(Pack(value, pc.source_format), pc.dest_format);
}
)";

struct WriteConstants {
  uint32_t dest_base, pitch, bpp_log2, format, number, endian, swap_rb;
  float scale;
  int32_t source_origin[2], dest_origin[2];
  uint32_t size[2];
  int32_t sample_select;
  uint32_t depth_mode;
  int32_t alias_mode;
};

struct ResolveConstants {
  float scale;
  int32_t sample_select;
  int32_t swap_rb;
  int32_t is_depth;
  int32_t offset[2];
  int32_t alias_mode;
};

VkShaderModule CreateModule(VkDevice device, const std::vector<uint32_t>& spirv) {
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = spirv.size() * 4;
  info.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  vkCreateShaderModule(device, &info, nullptr, &module);
  return module;
}

// Host format of a resolve destination (matches the texture cache's host
// format for the same guest texture format), and the signed alias if any.
void ResolveDestFormats(uint32_t f, VkFormat& unsigned_format, VkFormat& signed_format) {
  signed_format = VK_FORMAT_UNDEFINED;
  switch (f) {
    case 6:  // k_8_8_8_8
      unsigned_format = VK_FORMAT_R8G8B8A8_UNORM;
      signed_format = VK_FORMAT_R8G8B8A8_SNORM;
      return;
    case 7: unsigned_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32; return;  // k_2_10_10_10
    case 25:  // k_16_16
      unsigned_format = VK_FORMAT_R16G16_UNORM;
      signed_format = VK_FORMAT_R16G16_SNORM;
      return;
    case 26:  // k_16_16_16_16
      unsigned_format = VK_FORMAT_R16G16B16A16_UNORM;
      signed_format = VK_FORMAT_R16G16B16A16_SNORM;
      return;
    case 31: unsigned_format = VK_FORMAT_R16G16_SFLOAT; return;        // k_16_16_FLOAT
    case 32: unsigned_format = VK_FORMAT_R16G16B16A16_SFLOAT; return;  // k_16_16_16_16_FLOAT
    case 22:  // k_24_8
    case 23:  // k_24_8_FLOAT
    case 36:  // k_32_FLOAT
      unsigned_format = VK_FORMAT_R32_SFLOAT;
      return;
    default: unsigned_format = VK_FORMAT_R8G8B8A8_UNORM; return;
  }
}

}  // namespace

bool RenderTargets::Initialize(std::string* error) {
  VkDevice device = context_.device();
  std::vector<uint32_t> spirv;
  if (!CompileGlsl(GlslStage::kVertex, kResolveVertexShader, spirv, error)) return false;
  resolve_vs_ = CreateModule(device, spirv);
  auto with_fetch = [](std::string source) {
    source.replace(source.find("SAMPLE_FETCH"), 12, kSampleFetch);
    return source;
  };
  std::string fs = with_fetch(kResolveFragmentShader);
  const std::string single =
      std::string("#version 450\n#define SOURCE_TYPE sampler2D\n") + fs.substr(fs.find('\n') + 1);
  const std::string multi = std::string("#version 450\n#define SOURCE_TYPE sampler2DMS\n#define MULTISAMPLED\n") +
                            fs.substr(fs.find('\n') + 1);
  if (!CompileGlsl(GlslStage::kFragment, single, spirv, error)) return false;
  resolve_fs_ = CreateModule(device, spirv);
  if (!CompileGlsl(GlslStage::kFragment, multi, spirv, error)) return false;
  resolve_fs_ms_ = CreateModule(device, spirv);

  VkDescriptorSetLayoutBinding binding = {};
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  vkCreateDescriptorSetLayout(device, &set_info, nullptr, &resolve_set_layout_);
  VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolveConstants)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &resolve_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  vkCreatePipelineLayout(device, &layout_info, nullptr, &resolve_layout_);
  VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 8192;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  vkCreateDescriptorPool(device, &pool_info, nullptr, &resolve_pool_);
  VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_NEAREST;
  vkCreateSampler(device, &sampler_info, nullptr, &resolve_sampler_);
  // Memory-writing resolve (compute).
  {
    std::string cs = with_fetch(kResolveWriteShader);
    const std::string body = cs.substr(cs.find('\n') + 1);
    if (!CompileGlsl(GlslStage::kCompute, "#version 450\n#define SOURCE_TYPE sampler2D\n" + body, spirv, error)) {
      return false;
    }
    write_cs_ = CreateModule(device, spirv);
    if (!CompileGlsl(GlslStage::kCompute,
                     "#version 450\n#define SOURCE_TYPE sampler2DMS\n#define MULTISAMPLED\n" + body, spirv, error)) {
      return false;
    }
    write_cs_ms_ = CreateModule(device, spirv);
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo write_set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    write_set_info.bindingCount = 2;
    write_set_info.pBindings = bindings;
    vkCreateDescriptorSetLayout(device, &write_set_info, nullptr, &write_set_layout_);
    VkPushConstantRange write_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WriteConstants)};
    VkPipelineLayoutCreateInfo write_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    write_layout_info.setLayoutCount = 1;
    write_layout_info.pSetLayouts = &write_set_layout_;
    write_layout_info.pushConstantRangeCount = 1;
    write_layout_info.pPushConstantRanges = &write_range;
    vkCreatePipelineLayout(device, &write_layout_info, nullptr, &write_layout_);
    for (int ms = 0; ms < 2; ++ms) {
      VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      info.stage.module = ms ? write_cs_ms_ : write_cs_;
      info.stage.pName = "main";
      info.layout = write_layout_;
      vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, ms ? &write_pipeline_ms_ : &write_pipeline_);
    }
  }
  {
    std::string ts = kTransferShader;
    const std::string body = ts.substr(ts.find('\n') + 1);
    if (!CompileGlsl(GlslStage::kFragment, "#version 450\n#define SOURCE_TYPE sampler2D\n" + body, spirv, error)) {
      return false;
    }
    transfer_fs_ = CreateModule(device, spirv);
    if (!CompileGlsl(GlslStage::kFragment,
                     "#version 450\n#define SOURCE_TYPE sampler2DMS\n#define MULTISAMPLED\n" + body, spirv, error)) {
      return false;
    }
    transfer_fs_ms_ = CreateModule(device, spirv);
    VkPushConstantRange range = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &resolve_set_layout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    vkCreatePipelineLayout(device, &info, nullptr, &transfer_layout_);
  }
  return resolve_vs_ && resolve_fs_ && resolve_fs_ms_ && write_pipeline_ && write_pipeline_ms_ && transfer_fs_ &&
         transfer_fs_ms_;
}

VkPipeline RenderTargets::GetTransferPipeline(VkFormat format, VkSampleCountFlagBits samples) {
  auto key = std::make_tuple(uint32_t(format), uint32_t(samples));
  auto it = transfer_pipelines_.find(key);
  if (it != transfer_pipelines_.end()) return it->second;
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = resolve_vs_;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = samples != VK_SAMPLE_COUNT_1_BIT ? transfer_fs_ms_ : transfer_fs_;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = samples;
  multisample.sampleShadingEnable = samples != VK_SAMPLE_COUNT_1_BIT;
  multisample.minSampleShading = 1.0f;
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = transfer_layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
  }
  return transfer_pipelines_[key] = pipeline;
}

static bool Is64bpp(uint32_t guest_format) { return guest_format == 5 || guest_format == 7 || guest_format == 15; }

const RenderTargetKey* RenderTargets::NewerOwner(const RenderTargetKey& guest_key) const {
  if (guest_key.depth || Is64bpp(guest_key.format)) return nullptr;
  const RenderTargetKey self = CanonicalKey(guest_key);
  // Newest other 32bpp view of the same tiles.
  const RenderTargetKey* newest = nullptr;
  uint64_t newest_write = LastWrite(guest_key);
  for (const auto& [key, sequence] : last_write_) {
    if (key.depth || key == self || key.base_tiles != self.base_tiles || key.pitch != self.pitch ||
        key.msaa != self.msaa || sequence <= newest_write) {
      continue;
    }
    auto format = last_format_.find(key);
    if (format == last_format_.end() || Is64bpp(format->second)) continue;
    newest = &key;
    newest_write = sequence;
  }
  return newest;
}

bool RenderTargets::TransferOwnership(VkCommandBuffer cmd, const RenderTargetKey& guest_key) {
  const RenderTargetKey* newest = NewerOwner(guest_key);
  if (!newest) return false;
  const RenderTargetKey self = CanonicalKey(guest_key);
  const RenderTargetKey source_key = *newest;
  const uint32_t source_format = last_format_[source_key];
  auto source_it = targets_.find(source_key);
  vk::Image* dest = Get(guest_key, 0);
  if (source_it == targets_.end() || !dest) return false;
  vk::Image& source = source_it->second;
  VkPipeline pipeline = GetTransferPipeline(dest->format, dest->samples);
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = resolve_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &resolve_set_layout_;
  if (!pipeline || vkAllocateDescriptorSets(context_.device(), &alloc, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo image_info = {resolve_sampler_, GetView(source, source.format, VK_IMAGE_ASPECT_COLOR_BIT),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);
  vk::Context::Transition(cmd, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vk::Context::Transition(cmd, *dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  const uint32_t width = std::min(source.width, dest->width), height = std::min(source.height, dest->height);
  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = dest->view;
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  vkCmdBeginRendering(cmd, &rendering);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_layout_, 0, 1, &set, 0, nullptr);
  const VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  const VkRect2D scissor = {{0, 0}, {width, height}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  const uint32_t formats[2] = {source_format, guest_key.format};
  vkCmdPushConstants(cmd, transfer_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, formats);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  last_write_[self] = ++write_sequence_;
  last_format_[self] = guest_key.format;
  return true;
}

void RenderTargets::WriteResolveToMemory(VkCommandBuffer cmd, const ResolveInfo& info, vk::Image& source,
                                         VkImageView source_view, int32_t alias_mode) {
  if (!shared_memory_buffer_ || !info.memory_write || !info.has_rect) return;
  const int32_t width = info.rect_x1 - info.rect_x0, height = info.rect_y1 - info.rect_y0;
  if (width <= 0 || height <= 0) return;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = resolve_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &write_set_layout_;
  if (vkAllocateDescriptorSets(context_.device(), &alloc, &set) != VK_SUCCESS) return;
  VkDescriptorImageInfo image_info = {resolve_sampler_, source_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo buffer_info = {shared_memory_buffer_, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet writes[2] = {};
  writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = writes[1].dstSet = set;
  writes[0].descriptorCount = writes[1].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &image_info;
  writes[1].dstBinding = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &buffer_info;
  vkUpdateDescriptorSets(context_.device(), 2, writes, 0, nullptr);
  vk::Context::Transition(cmd, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const bool multisampled = source.samples != VK_SAMPLE_COUNT_1_BIT;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, multisampled ? write_pipeline_ms_ : write_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, write_layout_, 0, 1, &set, 0, nullptr);
  WriteConstants constants = {};
  constants.dest_base = info.dest_address;
  constants.pitch = info.dest_pitch_aligned;
  constants.bpp_log2 = info.dest_bpp_log2;
  constants.format = info.dest_format;
  constants.number = info.dest_number;
  constants.endian = info.dest_endian;
  constants.swap_rb = info.dest_swap;
  constants.scale = std::ldexp(1.0f, info.dest_exp_bias);
  constants.source_origin[0] = info.rect_x0;
  constants.source_origin[1] = info.rect_y0;
  constants.dest_origin[0] = info.rect_x0 + info.dest_offset_x;
  constants.dest_origin[1] = info.rect_y0;
  const int32_t logical_width = alias_mode == 1 ? int32_t(source.width / 2)
                                : alias_mode == 2 ? int32_t(source.width * 2) : int32_t(source.width);
  const int32_t logical_height = alias_mode == 1 ? int32_t(source.height / 2)
                                 : alias_mode == 2 ? int32_t(source.height * 2) : int32_t(source.height);
  constants.size[0] = uint32_t(std::max(0, std::min(width, logical_width - info.rect_x0)));
  constants.size[1] = uint32_t(std::max(0, std::min(height, logical_height - info.rect_y0)));
  constants.sample_select = int32_t(info.sample_select);
  constants.depth_mode = info.source.depth ? (info.depth_float24 ? 2 : 1) : 0;
  constants.alias_mode = alias_mode;
  vkCmdPushConstants(cmd, write_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  vkCmdDispatch(cmd, (constants.size[0] + 7) / 8, (constants.size[1] + 7) / 8, 1);
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDEX_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                       &barrier, 0, nullptr, 0, nullptr);
}

VkPipeline RenderTargets::GetResolvePipeline(VkFormat format, bool multisampled) {
  const ResolvePipelineKey key{format, multisampled};
  auto it = resolve_pipelines_.find(key);
  if (it != resolve_pipelines_.end()) return it->second;
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = resolve_vs_;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = multisampled ? resolve_fs_ms_ : resolve_fs_;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = resolve_layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    pipeline = VK_NULL_HANDLE;
  }
  return resolve_pipelines_[key] = pipeline;
}

VkImageView RenderTargets::GetView(vk::Image& image, VkFormat format, VkImageAspectFlags aspect) {
  auto key = std::make_tuple(image.id, uint32_t(format), uint32_t(aspect));
  auto it = views_.find(key);
  if (it != views_.end()) return it->second;
  VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = image.image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.subresourceRange = {aspect, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(context_.device(), &info, nullptr, &view);
  return views_[key] = view;
}

VkFormat RenderTargets::ColorRenderTargetFormat(uint32_t f) {
  switch (f) {
    case 0: return VK_FORMAT_R8G8B8A8_UNORM;            // k_8_8_8_8
    case 1: return VK_FORMAT_R8G8B8A8_UNORM;            // k_8_8_8_8_GAMMA (PWL gamma kept encoded)
    case 2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // k_2_10_10_10
    case 3: return VK_FORMAT_R16G16B16A16_SFLOAT;       // k_2_10_10_10_FLOAT (7e3)
    case 4: return VK_FORMAT_R16G16_SNORM;              // k_16_16
    case 5: return VK_FORMAT_R16G16B16A16_SNORM;        // k_16_16_16_16
    case 6: return VK_FORMAT_R16G16_SFLOAT;             // k_16_16_FLOAT
    case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;       // k_16_16_16_16_FLOAT
    case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;  // k_2_10_10_10_AS_10_10_10_10
    case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;      // k_2_10_10_10_FLOAT_AS_16_16_16_16
    case 14: return VK_FORMAT_R32_SFLOAT;               // k_32_FLOAT
    case 15: return VK_FORMAT_R32G32_SFLOAT;            // k_32_32_FLOAT
    default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

VkFormat RenderTargets::TextureFormat(uint32_t f) {
  switch (f) {
    case 6: return VK_FORMAT_R8G8B8A8_UNORM;             // k_8_8_8_8
    case 7: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;   // k_2_10_10_10
    case 25: return VK_FORMAT_R16G16_SNORM;              // k_16_16
    case 26: return VK_FORMAT_R16G16B16A16_SNORM;        // k_16_16_16_16
    case 31: return VK_FORMAT_R16G16_SFLOAT;             // k_16_16_FLOAT
    case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;       // k_16_16_16_16_FLOAT
    case 36: return VK_FORMAT_R32_SFLOAT;                // k_32_FLOAT
    default: return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

// Rows of a render target that fit in the 2048 EDRAM tiles (80x16 samples
// each) after its base: allocating the whole range up front keeps an image
// stable for the frame no matter which scissor the draws use.
static uint32_t EdramRows(const RenderTargetKey& key) {
  const uint32_t samples_x = key.msaa >= 2 ? 2 : 1, samples_y = key.msaa >= 1 ? 2 : 1;
  const uint32_t bpp_factor =
      (!key.depth && (key.format == 5 || key.format == 7 || key.format == 15)) ? 2 : 1;
  const uint32_t tiles_per_row = ((key.pitch * samples_x + 79) / 80) * bpp_factor;
  if (!tiles_per_row || key.base_tiles >= 2048) return 16;
  return std::min<uint32_t>((2048 - key.base_tiles) / tiles_per_row * 16 / samples_y, 4096);
}

// Guest formats that are stored identically on the host (for example 8_8_8_8
// and 8_8_8_8_GAMMA, or 2_10_10_10 and 2_10_10_10_AS_10_10_10_10) are views of
// the same EDRAM data: titles write through one and resolve through the other,
// so they share an image. The key keeps the host format in place of the guest
// one.
static RenderTargetKey CanonicalKey(const RenderTargetKey& key) {
  RenderTargetKey canonical = key;
  if (!key.depth) canonical.format = 0x80000000u | uint32_t(RenderTargets::ColorRenderTargetFormat(key.format));
  return canonical;
}

vk::Image* RenderTargets::Get(const RenderTargetKey& guest_key, uint32_t height) {
  height = std::max<uint32_t>({height, EdramRows(guest_key), 16});
  const RenderTargetKey key = CanonicalKey(guest_key);
  auto it = targets_.find(key);
  if (it != targets_.end() && it->second.height >= height) return &it->second;
  if (it != targets_.end()) {
    // Grow (content is not carried over yet); the old image may still be
    // referenced by recorded commands, so it's freed at the end of the frame.
    frame_temporaries_.push_back(it->second);
    targets_.erase(it);
  }
  vk::Image image;
  VkFormat format = key.depth ? (key.format ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT)
                              : ColorRenderTargetFormat(guest_key.format);
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                            (key.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!context_.CreateImage(image, format, std::max<uint32_t>(key.pitch, 16), height,
                            VkSampleCountFlagBits(1u << key.msaa), usage)) {
    return nullptr;
  }
  return &(targets_[key] = image);
}

uint64_t RenderTargets::MemoryFingerprint(uint32_t address, uint32_t size) const {
  if (!guest_memory_) return 0;
  address &= 0x1FFFFFFF;
  size = std::min<uint32_t>(std::max(size, 4096u), 0x20000000u - address);
  // Sparse: 16 blocks of 1 KB spread over the range.
  uint64_t hash = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t offset = uint32_t(uint64_t(size - std::min(size, 1024u)) * i / 15);
    hash = XXH3_64bits_withSeed(guest_memory_ + address + offset, std::min(size, 1024u), hash);
  }
  return hash;
}

bool RenderTargets::IsResolvedCurrent(uint32_t address) {
  if (!guest_memory_) return true;
  auto it = resolved_memory_hash_.find(address);
  auto image = resolved_.find(address);
  if (it == resolved_memory_hash_.end() || image == resolved_.end()) return false;
  return MemoryFingerprint(address, image->second.width * image->second.height * 4) == it->second;
}

void RenderTargets::MarkWritten(const RenderTargetKey& guest_key) {
  const RenderTargetKey key = CanonicalKey(guest_key);
  last_write_[key] = ++write_sequence_;
  last_format_[key] = guest_key.format;
}

uint64_t RenderTargets::LastWrite(const RenderTargetKey& guest_key) const {
  auto it = last_write_.find(CanonicalKey(guest_key));
  return it != last_write_.end() ? it->second : 0;
}

void RenderTargets::EndFrame() {
  VkDevice device = context_.device();
  for (vk::Image& image : frame_temporaries_) {
    for (auto it = views_.begin(); it != views_.end();) {
      if (std::get<0>(it->first) == image.id) {
        vkDestroyImageView(device, it->second, nullptr);
        it = views_.erase(it);
      } else {
        ++it;
      }
    }
    context_.DestroyImage(image);
  }
  frame_temporaries_.clear();
  if (resolve_pool_) vkResetDescriptorPool(device, resolve_pool_, 0);
}

const vk::Image* RenderTargets::FindResolved(uint32_t address) const {
  auto it = resolved_.find(address);
  return it == resolved_.end() ? nullptr : &it->second;
}

void RenderTargets::Resolve(VkCommandBuffer cmd, const ResolveInfo& info) {
  TransferOwnership(cmd, info.source);
  vk::Image* source = Get(info.source, info.dest_height);
  if (!source || !info.dest_width || !info.dest_height) return;
  // The EDRAM may have last been written through the 1x <-> 4x MSAA alias of
  // this surface (same tiles, double / half the width): read it from there.
  int32_t alias_mode = 0;
  if (info.source.msaa == 2 || (info.source.msaa == 0 && !(info.source.pitch & 1))) {
    RenderTargetKey alias = info.source;
    alias.msaa = info.source.msaa == 2 ? 0 : 2;
    alias.pitch = info.source.msaa == 2 ? info.source.pitch * 2 : info.source.pitch / 2;
    if (LastWrite(alias) > LastWrite(info.source)) {
      if (vk::Image* alias_image = Get(alias, info.dest_height * (info.source.msaa == 2 ? 2 : 1))) {
        source = alias_image;
        alias_mode = info.source.msaa == 2 ? 1 : 2;
      }
    }
  }

  // Destination: resolved texture for this guest address, in the host format
  // the texture cache uses for the destination format (mutable, so textures
  // reading the same bits through another format of the same size can view it).
  VkFormat dest_unsigned, dest_signed;
  ResolveDestFormats(info.dest_format, dest_unsigned, dest_signed);
  vk::Image& dest = resolved_[info.dest_address];
  if (dest.image && (dest.width != info.dest_width || dest.height != info.dest_height ||
                     dest.format != dest_unsigned)) {
    frame_temporaries_.push_back(dest);
    dest = vk::Image();
  }
  if (!dest.image) {
    context_.CreateImage(dest, dest_unsigned, info.dest_width, info.dest_height, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                         VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
    // Resolves may fill only part of it; start from zero, not garbage.
    vk::Context::Transition(cmd, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue zero = {};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dest.image, dest.layout, &zero, 1, &range);
  }
  // Signed render target formats (k_16_16, k_16_16_16_16) write through the
  // signed alias so the stored bits are the guest's.
  const bool signed_source = !info.source.depth && (info.source.format == 4 || info.source.format == 5);
  const VkFormat attachment_format =
      signed_source && dest_signed != VK_FORMAT_UNDEFINED ? dest_signed : dest_unsigned;

  const bool multisampled = source->samples != VK_SAMPLE_COUNT_1_BIT;
  VkPipeline pipeline = GetResolvePipeline(attachment_format, multisampled);
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = resolve_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &resolve_set_layout_;
  if (pipeline && vkAllocateDescriptorSets(context_.device(), &alloc, &set) == VK_SUCCESS) {
    const VkImageView source_view =
        GetView(*source, source->format, info.source.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    VkDescriptorImageInfo image_info = {resolve_sampler_, source_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(context_.device(), 1, &write, 0, nullptr);

    vk::Context::Transition(cmd, *source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk::Context::Transition(cmd, dest, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // Destination rectangle = source rectangle moved by dest_offset_x,
    // clipped to both images.
    int32_t sx0 = 0, sy0 = 0, sx1 = int32_t(info.dest_width), sy1 = int32_t(info.dest_height);
    if (info.has_rect) {
      sx0 = info.rect_x0;
      sy0 = info.rect_y0;
      sx1 = info.rect_x1;
      sy1 = info.rect_y1;
    }
    const int32_t logical_width = alias_mode == 1 ? int32_t(source->width / 2)
                                  : alias_mode == 2 ? int32_t(source->width * 2) : int32_t(source->width);
    const int32_t logical_height = alias_mode == 1 ? int32_t(source->height / 2)
                                   : alias_mode == 2 ? int32_t(source->height * 2) : int32_t(source->height);
    sx1 = std::min({sx1, logical_width, int32_t(dest.width) - info.dest_offset_x});
    sy1 = std::min({sy1, logical_height, int32_t(dest.height)});
    const int32_t dx0 = std::max(sx0 + info.dest_offset_x, 0), dy0 = std::max(sy0, 0);
    const uint32_t width = sx1 > sx0 ? uint32_t(sx1 - sx0) : 0;
    const uint32_t height = sy1 > sy0 ? uint32_t(sy1 - sy0) : 0;
    VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = GetView(dest, attachment_format, VK_IMAGE_ASPECT_COLOR_BIT);
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{dx0, dy0}, {std::max(width, 1u), std::max(height, 1u)}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    if (width && height) vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout_, 0, 1, &set, 0, nullptr);
    const VkViewport viewport = {0.0f, 0.0f, float(dest.width), float(dest.height), 0.0f, 1.0f};
    const VkRect2D scissor = {{dx0, dy0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    ResolveConstants constants = {};
    constants.offset[0] = -info.dest_offset_x;
    constants.scale = std::ldexp(1.0f, info.dest_exp_bias);
    constants.sample_select = int32_t(info.sample_select);
    constants.swap_rb = 0;  // applied by texture views, see IsResolvedSwapped
    resolved_swapped_[info.dest_address] = info.dest_swap;
    resolved_memory_hash_[info.dest_address] =
        MemoryFingerprint(info.dest_address, info.dest_width * info.dest_height * 4);
    constants.is_depth = info.source.depth;
    constants.alias_mode = alias_mode;
    vkCmdPushConstants(cmd, resolve_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    if (width && height) {
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRendering(cmd);
    }
  }

  WriteResolveToMemory(cmd, info, *source,
                       GetView(*source, source->format,
                               info.source.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                       alias_mode);

  // Clears affect the resolved rectangle of the render target (EDRAM) only.
  auto clear_rect_info = [&](vk::Image& image, const VkClearValue& value, const ResolveInfo& info) {
    VkRect2D rect = {{0, 0}, {image.width, image.height}};
    if (info.has_rect) {
      const int32_t x0 = std::clamp(info.rect_x0, 0, int32_t(image.width));
      const int32_t y0 = std::clamp(info.rect_y0, 0, int32_t(image.height));
      const int32_t x1 = std::clamp(info.rect_x1, x0, int32_t(image.width));
      const int32_t y1 = std::clamp(info.rect_y1, y0, int32_t(image.height));
      rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    }
    if (!rect.extent.width || !rect.extent.height) return;
    const bool is_depth = image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
    vk::Context::Transition(cmd, image,
                            is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = image.view;
    attachment.imageLayout = image.layout;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = rect;
    rendering.layerCount = 1;
    if (is_depth) {
      rendering.pDepthAttachment = &attachment;
      if (image.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.pStencilAttachment = &attachment;
    } else {
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachments = &attachment;
    }
    vkCmdBeginRendering(cmd, &rendering);
    VkClearAttachment clear = {};
    clear.aspectMask = is_depth ? image.aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    clear.clearValue = value;
    VkClearRect clear_rect_info = {rect, 0, 1};
    vkCmdClearAttachments(cmd, 1, &clear, 1, &clear_rect_info);
    vkCmdEndRendering(cmd);
  };
  auto clear_rect = [&](vk::Image& image, const VkClearValue& value) { clear_rect_info(image, value, info); };
  if (info.clear_color && !info.source.depth) {
    // Clear value packed in the render target's format (as Xenia decodes it):
    // RB_COLOR_CLEAR is the whole value for 32bpp formats and the upper half
    // for 64bpp ones, RB_COLOR_CLEAR_LO the lower half.
    const uint32_t f = info.source.format;
    const bool is_64bpp = f == 5 || f == 7 || f == 15;
    const uint64_t v = is_64bpp ? (uint64_t(info.clear_color_value) << 32) | info.clear_color_value_lo
                                : info.clear_color_value;
    VkClearValue value{};
    float* c = value.color.float32;
    switch (f) {
      case 0:
      case 1:
        for (uint32_t j = 0; j < 4; ++j) c[j] = ((v >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
        break;
      case 2:
      case 10:
        for (uint32_t j = 0; j < 3; ++j) c[j] = ((v >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
        c[3] = ((v >> 30) & 3) * (1.0f / 3);
        break;
      case 3:
      case 12:
        for (uint32_t j = 0; j < 3; ++j) c[j] = rex::graphics::xenos::Float7e3To32(uint32_t(v >> (j * 10)) & 0x3FF);
        c[3] = ((v >> 30) & 3) * (1.0f / 3);
        break;
      case 4:
      case 5:
        for (uint32_t j = 0; j < (f == 4 ? 2u : 4u); ++j) {
          c[j] = std::max(float(int16_t(uint16_t(v >> (j * 16)))) * (1.0f / 32767.0f), -1.0f);
        }
        break;
      case 6:
      case 7:
        for (uint32_t j = 0; j < (f == 6 ? 2u : 4u); ++j) c[j] = rex::xenos_half_to_float(uint16_t(v >> (j * 16)));
        break;
      case 14:
      case 15: {
        const uint32_t lo = uint32_t(v), hi = uint32_t(v >> 32);
        std::memcpy(&c[0], f == 15 ? &hi : &lo, 4);
        if (f == 15) std::memcpy(&c[1], &lo, 4);
      } break;
      default:
        break;
    }
    clear_rect(*Get(info.source, info.dest_height), value);
    MarkWritten(info.source);
    // EDRAM is one memory: the clear also wipes every other view of the same
    // tiles (other formats at the same base / pitch, and the 1x <-> 4x MSAA
    // alias). Titles clear through one format and accumulate through another
    // (Reach clears as 8_8_8_8 and adds lighting as 7e3).
    const RenderTargetKey cleared = CanonicalKey(info.source);
    for (auto& [key, image] : targets_) {
      if (key.depth || key.base_tiles != info.source.base_tiles || key == cleared) continue;
      const bool same_view = key.pitch == info.source.pitch && key.msaa == info.source.msaa;
      const bool msaa_alias = (key.msaa == 2 && info.source.msaa == 0 && key.pitch * 2 == info.source.pitch) ||
                              (key.msaa == 0 && info.source.msaa == 2 && key.pitch == info.source.pitch * 2);
      if (!same_view && !msaa_alias) continue;
      VkClearValue zero{};
      ResolveInfo alias_info = info;
      if (msaa_alias) {
        // Rectangle in the alias's pixels.
        const int32_t num = key.msaa == 2 ? 1 : 2, den = key.msaa == 2 ? 2 : 1;
        alias_info.rect_x0 = info.rect_x0 * num / den;
        alias_info.rect_x1 = info.rect_x1 * num / den;
        alias_info.rect_y0 = info.rect_y0 * num / den;
        alias_info.rect_y1 = info.rect_y1 * num / den;
      }
      clear_rect_info(image, info.clear_color_value == 0 && info.clear_color_value_lo == 0 ? zero : value,
                      alias_info);
      last_write_[key] = write_sequence_;
    }
  }
  if (info.clear_depth) {
    vk::Image* depth = info.source.depth ? source
                       : info.has_depth_target ? Get(info.depth_target, info.dest_height)
                                               : nullptr;
    if (depth) {
      VkClearValue value{};
      const uint32_t guest_depth = (info.clear_depth_value >> 8) & 0xFFFFFF;
      const bool float24 = (info.source.depth ? info.source.format : info.depth_target.format) != 0;
      value.depthStencil.depth = float24 ? rex::graphics::xenos::Float20e4To32(guest_depth) * 0.5f
                                         : float(guest_depth) / float(0xFFFFFF);
      value.depthStencil.stencil = info.clear_depth_value & 0xFF;
      clear_rect(*depth, value);
      MarkWritten(info.source.depth ? info.source : info.depth_target);
    }
  }
}

RenderTargetKey ColorTargetKey(const uint32_t* r, uint32_t index) {
  static const uint32_t kInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};  // RB_COLOR*_INFO
  const uint32_t info = r[kInfo[index & 3]];
  RenderTargetKey key;
  key.base_tiles = info & 0xFFF;
  key.format = (info >> 16) & 0xF;
  key.pitch = r[0x2000] & 0x3FFF;  // RB_SURFACE_INFO
  key.msaa = (r[0x2000] >> 16) & 3;
  return key;
}

RenderTargetKey DepthTargetKey(const uint32_t* r) {
  const uint32_t info = r[0x2002];  // RB_DEPTH_INFO
  RenderTargetKey key;
  key.depth = true;
  key.base_tiles = info & 0xFFF;
  key.format = (info >> 16) & 1;
  key.pitch = r[0x2000] & 0x3FFF;
  key.msaa = (r[0x2000] >> 16) & 3;
  return key;
}

ResolveInfo ResolveInfoFromRegisters(const uint32_t* r) {
  ResolveInfo info;
  const uint32_t copy_control = r[0x2318];  // RB_COPY_CONTROL
  const uint32_t source = copy_control & 7;
  info.source = source == 4 ? DepthTargetKey(r) : ColorTargetKey(r, source);
  info.dest_address = r[0x2319];  // RB_COPY_DEST_BASE
  info.dest_width = r[0x231A] & 0x3FFF;  // RB_COPY_DEST_PITCH
  info.dest_height = (r[0x231A] >> 16) & 0x3FFF;
  info.dest_format = (r[0x231B] >> 7) & 0x3F;  // RB_COPY_DEST_INFO
  info.clear_color = (copy_control >> 8) & 1;
  info.clear_depth = (copy_control >> 9) & 1;
  info.has_depth_target = true;
  info.depth_target = DepthTargetKey(r);
  info.clear_depth_value = r[0x231D];     // RB_DEPTH_CLEAR
  info.clear_color_value = r[0x231E];     // RB_COLOR_CLEAR
  info.clear_color_value_lo = r[0x231F];  // RB_COLOR_CLEAR_LO
  info.dest_exp_bias = int32_t(r[0x231B] << 10) >> 26;  // RB_COPY_DEST_INFO bits 16:21
  info.sample_select = (copy_control >> 4) & 7;
  info.dest_swap = (r[0x231B] >> 24) & 1;
  return info;
}

}  // namespace xgpu
