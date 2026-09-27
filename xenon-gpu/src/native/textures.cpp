// xenon-gpu: native textures and samplers (see textures.h).
//
// GPU loading (the normal path) follows VulkanTextureCache::
// LoadTextureDataFromResidentMemoryImpl: Xenia's texture_load_* compute
// shaders read the guest blocks from the shared memory buffer (set 1, bound
// whole, guest_offset = absolute address), untile / endian swap / convert them
// into a scratch buffer (set 0) in the host layout, then the scratch buffer is
// copied to the image (packed mip tails are loaded whole and copied out by
// offset).
//
// The CPU loader (fallback, tools, DebugDecode) does the same on the CPU:
// blocks are read from the guest memory image (big-endian, tiled or linear),
// endian-swapped like the load shaders do (XeEndianSwap16 for 16bpb,
// XeEndianSwap32 for everything bigger, nothing for 8bpb), converted to the
// host format (BC formats stay compressed) and uploaded through a staging
// buffer. Host formats and swizzles follow VulkanTextureCache::
// kBestHostFormats so the SPIR-V translator's view of the data (swizzled
// signs, host swizzle through the view component mapping) is the same as in
// Xenia.
#include <xgpu/textures.h>
#include <xgpu/debug.h>

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/thread/mutex.h>

namespace xgpu {

using namespace rex::graphics;
using xenos::TextureFormat;

namespace {

constexpr uint32_t kGuestMemoryMask = 0x1FFFFFFF;
constexpr uint32_t kGuestMemorySize = 0x20000000;
constexpr uint8_t kSwizzledSignsUnsigned = 0;

// How guest blocks become host data.
enum class Convert : uint8_t {
  kCopy,            // bytes as-is after the endian swap
  kR5G6B5,          // -> VK_FORMAT_R5G6B5_UNORM_PACK16 (R/B swapped)
  kR5G5B5A1,        // -> VK_FORMAT_A1R5G5B5_UNORM_PACK16
  kR4G4B4A4,        // -> VK_FORMAT_B4G4R4A4_UNORM_PACK16
  kR5G5B6,          // k_6_5_5 -> R5G6B5 with an RBGA swizzle
  kR11G11B10,       // k_10_11_11 -> RGBA16 UNORM
  kR10G11B11,       // k_11_11_10 -> RGBA16 UNORM
  kDepthUnorm,      // k_24_8 -> R32_SFLOAT
  kDepthFloat,      // k_24_8_FLOAT -> R32_SFLOAT
  kDXT3AToR8,       // k_DXT3A (4x4 blocks of 4-bit values) -> R8 texels
  kCTX1ToR8G8,      // k_CTX1 (4x4 blocks, two 8:8 endpoints) -> R8G8 texels
};

struct HostFormat {
  VkFormat format_unsigned = VK_FORMAT_UNDEFINED;
  VkFormat format_signed = VK_FORMAT_UNDEFINED;
  uint32_t swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  Convert convert = Convert::kCopy;
  // True if the host format is block-compressed (4x4 blocks like the guest).
  bool host_compressed = false;
  // Bytes per host block (compressed) or per host texel (uncompressed).
  uint32_t host_bytes = 0;
  bool supported() const {
    return format_unsigned != VK_FORMAT_UNDEFINED || format_signed != VK_FORMAT_UNDEFINED;
  }
};

constexpr uint32_t Swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
  return x | (y << 3) | (z << 6) | (w << 9);
}
constexpr uint32_t kRRRR = Swz(0, 0, 0, 0), kRGGG = Swz(0, 1, 1, 1), kRGBB = Swz(0, 1, 2, 2),
                   kRGBA = Swz(0, 1, 2, 3);

HostFormat MakeHostFormat(TextureFormat format) {
  HostFormat f;
  auto set = [&](VkFormat u, VkFormat s, uint32_t swizzle, uint32_t bytes,
                 Convert convert = Convert::kCopy, bool compressed = false) {
    f.format_unsigned = u;
    f.format_signed = s;
    f.swizzle = swizzle;
    f.host_bytes = bytes;
    f.convert = convert;
    f.host_compressed = compressed;
  };
  switch (format) {
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
      set(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, kRRRR, 1);
      break;
    case TextureFormat::k_1_5_5_5:
      set(VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_UNDEFINED, kRGBA, 2, Convert::kR5G5B5A1);
      break;
    case TextureFormat::k_5_6_5:
      set(VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_UNDEFINED, kRGBB, 2, Convert::kR5G6B5);
      break;
    case TextureFormat::k_6_5_5:
      set(VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_UNDEFINED, Swz(0, 2, 1, 1), 2,
          Convert::kR5G5B6);
      break;
    case TextureFormat::k_8_8_8_8:
      set(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, kRGBA, 4);
      break;
    case TextureFormat::k_2_10_10_10:
      set(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_UNDEFINED, kRGBA, 4);
      break;
    case TextureFormat::k_8_8:
      set(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, kRGGG, 2);
      break;
    case TextureFormat::k_4_4_4_4:
      set(VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_UNDEFINED, kRGBA, 2, Convert::kR4G4B4A4);
      break;
    case TextureFormat::k_10_11_11:
      set(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED, kRGBB, 8, Convert::kR11G11B10);
      break;
    case TextureFormat::k_11_11_10:
      set(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_UNDEFINED, kRGBB, 8, Convert::kR10G11B11);
      break;
    case TextureFormat::k_DXT1:
      set(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED, kRGBA, 8, Convert::kCopy, true);
      break;
    case TextureFormat::k_DXT2_3:
      set(VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_UNDEFINED, kRGBA, 16, Convert::kCopy, true);
      break;
    case TextureFormat::k_DXT4_5:
      set(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED, kRGBA, 16, Convert::kCopy, true);
      break;
    case TextureFormat::k_DXN:
      set(VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED, kRGGG, 16, Convert::kCopy, true);
      break;
    case TextureFormat::k_DXT5A:
      set(VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, kRRRR, 8, Convert::kCopy, true);
      break;
    case TextureFormat::k_DXT3A:
      set(VK_FORMAT_R8_UNORM, VK_FORMAT_UNDEFINED, kRRRR, 1, Convert::kDXT3AToR8);
      break;
    case TextureFormat::k_CTX1:
      set(VK_FORMAT_R8G8_UNORM, VK_FORMAT_UNDEFINED, kRGGG, 2, Convert::kCTX1ToR8G8);
      break;
    case TextureFormat::k_24_8:
      set(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, kRRRR, 4, Convert::kDepthUnorm);
      break;
    case TextureFormat::k_24_8_FLOAT:
      set(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, kRRRR, 4, Convert::kDepthFloat);
      break;
    case TextureFormat::k_16:
      set(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, kRRRR, 2);
      break;
    case TextureFormat::k_16_16:
      set(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, kRGGG, 4);
      break;
    case TextureFormat::k_16_16_16_16:
      set(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, kRGBA, 8);
      break;
    case TextureFormat::k_16_FLOAT:
      set(VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, kRRRR, 2);
      break;
    case TextureFormat::k_16_16_FLOAT:
      set(VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, kRGGG, 4);
      break;
    case TextureFormat::k_16_16_16_16_FLOAT:
      set(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, kRGBA, 8);
      break;
    case TextureFormat::k_32_FLOAT:
      set(VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, kRRRR, 4);
      break;
    case TextureFormat::k_32_32_FLOAT:
      set(VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, kRGGG, 8);
      break;
    case TextureFormat::k_32_32_32_32_FLOAT:
      set(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, kRGBA, 16);
      break;
    default:
      // Unsupported (CTX1, YUV, 32-bit integer, MPEG, interlaced, EDRAM...):
      // leave undefined; swizzle as in kBestHostFormats for the constants.
      switch (format) {
        case TextureFormat::k_Cr_Y1_Cb_Y0_REP:
        case TextureFormat::k_Y1_Cr_Y0_Cb_REP:
        case TextureFormat::k_32_32_32_FLOAT:
          f.swizzle = kRGBB;
          break;
        case TextureFormat::k_16_16_EDRAM:
        case TextureFormat::k_32_32:
        case TextureFormat::k_32_AS_8_8:
        case TextureFormat::k_16_16_MPEG:
        case TextureFormat::k_32_AS_8_8_INTERLACED:
        case TextureFormat::k_16_16_MPEG_INTERLACED:
          f.swizzle = kRGGG;
          break;
        case TextureFormat::k_16_16_16_16_EDRAM:
        case TextureFormat::k_8_8_8_8_A:
        case TextureFormat::k_32_32_32_32:
        case TextureFormat::k_DXT3A_AS_1_1_1_1:
        case TextureFormat::k_8_8_8_8_GAMMA_EDRAM:
        case TextureFormat::k_2_10_10_10_FLOAT_EDRAM:
          f.swizzle = kRGBA;
          break;
        default:
          f.swizzle = kRRRR;
          break;
      }
      break;
  }
  return f;
}

const HostFormat& GetHostFormat(TextureFormat format) {
  static const auto table = [] {
    std::vector<HostFormat> t(64);
    for (uint32_t i = 0; i < 64; ++i) t[i] = MakeHostFormat(TextureFormat(i));
    return t;
  }();
  return table[uint32_t(format) & 63];
}

uint32_t GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t c = (guest_swizzle >> (3 * i)) & 0b111;
    uint32_t h = c >= xenos::XE_GPU_TEXTURE_SWIZZLE_0 ? (c & 0b101)
                                                      : (host_format_swizzle >> (3 * c)) & 0b111;
    host_swizzle |= h << (3 * i);
  }
  return host_swizzle;
}

VkComponentSwizzle ComponentSwizzle(uint32_t swizzle, uint32_t index) {
  uint32_t c = (swizzle >> (3 * index)) & 0b111;
  if (c == index) return VK_COMPONENT_SWIZZLE_IDENTITY;
  switch (c) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    case 5: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
}

// TextureCache::GetIntegerScaleBits from the Xenia code.
uint32_t GetIntegerScaleBits(TextureFormat guest_format, uint32_t num_format,
                             uint32_t guest_swizzle, uint8_t swizzled_signs) {
  const FormatInfo& format_info = *FormatInfo::Get(guest_format);
  if (!format_info.fixed) return 0;
  if (!num_format) return swizzled_signs == kSwizzledSignsUnsigned ? UINT32_C(1) << 24 : 0;
  uint32_t last_stored_component = 0;
  for (uint32_t i = 1; i < 4; ++i) {
    if (format_info.component_bits[i]) last_stored_component = i;
  }
  uint32_t scale_bits = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t source_component = (guest_swizzle >> (i * 3)) & 0b111;
    if (source_component >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) continue;
    source_component = std::min(source_component, last_stored_component);
    xenos::TextureSign sign = xenos::TextureSign((swizzled_signs >> (i * 2)) & 0b11);
    uint8_t width = format_info.component_bits[source_component];
    if (!width || width > 16 || sign == xenos::TextureSign::kGamma) continue;
    uint32_t component_scale = uint32_t(width - 1);
    if (sign == xenos::TextureSign::kSigned) {
      component_scale |= UINT32_C(1) << 4;
    } else if (sign == xenos::TextureSign::kUnsignedBiased) {
      component_scale |= UINT32_C(1) << 5;
    }
    scale_bits |= component_scale << (i * 6);
  }
  return scale_bits;
}

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v > 1) {
    v >>= 1;
    ++r;
  }
  return r;
}

inline uint32_t Swap32(uint32_t v, xenos::Endian endian) {
  if (endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32) {
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  }
  if (endian == xenos::Endian::k8in32 || endian == xenos::Endian::k16in32) {
    v = (v << 16) | (v >> 16);
  }
  return v;
}

inline uint16_t Expand(uint32_t v, uint32_t bits) {
  // Bit replication to 16 bits.
  uint32_t r = v << (16 - bits);
  for (uint32_t filled = bits; filled < 16; filled += bits) r |= r >> bits;
  return uint16_t(r);
}

VkImageViewType ViewTypeFor(xenos::FetchOpDimension dimension) {
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked: return VK_IMAGE_VIEW_TYPE_3D;
    case xenos::FetchOpDimension::kCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    default: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  }
}

bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                             xenos::DataDimension resource_dimension) {
  switch (binding_dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
      return resource_dimension == xenos::DataDimension::k1D ||
             resource_dimension == xenos::DataDimension::k2DOrStacked ||
             resource_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::k3DOrStacked:
      return resource_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::kCube:
      return resource_dimension == xenos::DataDimension::kCube;
    default:
      return false;
  }
}

struct TextureKey {
  uint32_t base_page = 0, mip_page = 0;
  uint32_t dimension = 0;
  uint32_t width = 0, height = 0, depth = 0;  // depth or array size
  uint32_t pitch = 0;
  uint32_t mip_max_level = 0;
  uint32_t tiled = 0, packed_mips = 0;
  uint32_t format = 0;  // base format
  uint32_t endianness = 0;
  auto Tie() const {
    return std::tie(base_page, mip_page, dimension, width, height, depth, pitch, mip_max_level, tiled,
                    packed_mips, format, endianness);
  }
  bool operator<(const TextureKey& o) const { return Tie() < o.Tie(); }
  xenos::DataDimension dim() const { return xenos::DataDimension(dimension); }
  TextureFormat fmt() const { return TextureFormat(format); }
};

struct FetchInfo {
  bool valid = false;
  TextureKey key;
  uint8_t swizzled_signs = kSwizzledSignsUnsigned;
};

// TextureCache::BindingInfoFromFetchConstant (without wide 1D remapping).
FetchInfo DescribeFetch(const xenos::xe_gpu_texture_fetch_t& fetch) {
  FetchInfo info;
  if (fetch.type != xenos::FetchConstantType::kTexture) return info;
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, nullptr,
                                                 &mip_max_level);
  if (base_page == 0 && mip_page == 0) return info;
  if (fetch.dimension == xenos::DataDimension::k1D) {
    if (fetch.tiled || fetch.packed_mips || width_minus_1 >= 8192) return info;
  }
  TextureKey& key = info.key;
  key.base_page = base_page;
  key.mip_page = mip_page;
  key.dimension = uint32_t(fetch.dimension);
  key.width = width_minus_1 + 1;
  key.height = height_minus_1 + 1;
  key.depth = depth_minus_1 + 1;
  key.pitch = fetch.pitch;
  key.mip_max_level = mip_max_level;
  key.tiled = fetch.tiled;
  key.packed_mips = fetch.packed_mips;
  key.format = uint32_t(GetBaseFormat(fetch.format));
  key.endianness = uint32_t(fetch.endianness);
  info.valid = true;
  info.swizzled_signs = texture_util::SwizzleSigns(fetch);
  return info;
}

// Host data for a texture, ready for vkCmdCopyBufferToImage.
struct HostData {
  std::vector<uint8_t> bytes;
  std::vector<VkBufferImageCopy> regions;
  uint32_t width = 0, height = 0, depth = 1, layers = 1, levels = 1;
};

// Loads (untiles, swaps, converts) all levels of a texture from guest memory.
// levels_limit: load at most levels 0..levels_limit (for debug decoding).
bool BuildHostData(const uint8_t* memory, const TextureKey& key, const HostFormat& hf,
                   HostData& out, uint32_t levels_limit = UINT32_MAX) {
  const FormatInfo* fi = FormatInfo::Get(key.fmt());
  const uint32_t bw = fi->block_width, bh = fi->block_height;
  const uint32_t bpb = fi->bytes_per_block();
  if (!bpb || bpb > 16) return false;
  const uint32_t bpb_log2 = Log2Floor(bpb);
  const bool is_3d = key.dim() == xenos::DataDimension::k3D;
  const uint32_t width = key.width;
  const uint32_t height = key.dim() == xenos::DataDimension::k1D ? 1 : key.height;
  const uint32_t depth = is_3d ? key.depth : 1;
  const uint32_t layers = is_3d ? 1 : key.depth;
  const xenos::Endian endian = xenos::Endian(key.endianness);

  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      key.dim(), key.pitch, width, height, key.depth, key.tiled != 0, key.fmt(),
      key.packed_mips != 0, key.base_page != 0, key.mip_max_level);

  out.width = width;
  out.height = height;
  out.depth = depth;
  out.layers = layers;
  out.levels = key.mip_max_level + 1;
  out.bytes.clear();
  out.regions.clear();

  // Host geometry per guest block.
  const bool decompress = bw > 1 && !hf.host_compressed;  // DXT3A, CTX1
  const uint32_t host_rows_per_block = decompress ? bh : 1;
  const uint32_t host_bytes_per_block_x = decompress ? bw * hf.host_bytes : hf.host_bytes;

  const uint32_t level_first = key.base_page ? 0 : 1;
  const uint32_t level_last = std::min(key.mip_max_level, levels_limit);
  uint8_t block[16];
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const uint32_t stored = std::min(level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& gl =
        level == 0 ? layout.base : layout.mips[stored];
    const uint32_t src_base =
        level == 0 ? key.base_page << 12 : (key.mip_page << 12) + layout.mip_offsets_bytes[stored];
    uint32_t ox = 0, oy = 0, oz = 0;
    if (level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, depth, key.fmt(), level, ox, oy, oz);
    }
    const uint32_t lw = std::max(width >> level, 1u);
    const uint32_t lh = std::max(height >> level, 1u);
    const uint32_t ld = std::max(depth >> level, 1u);
    const uint32_t wb = (lw + bw - 1) / bw, hb = (lh + bh - 1) / bh;
    const uint32_t pitch_blocks = gl.row_pitch_bytes / bpb;
    const uint32_t z_stride_rows = gl.z_slice_stride_block_rows;

    const size_t host_row_bytes = size_t(wb) * host_bytes_per_block_x;
    const size_t host_rows = size_t(hb) * host_rows_per_block;
    const size_t host_slice_bytes = host_row_bytes * host_rows;  // one Z slice
    const size_t level_offset = (out.bytes.size() + 15) & ~size_t(15);
    out.bytes.resize(level_offset + host_slice_bytes * ld * layers, 0);

    for (uint32_t layer = 0; layer < layers; ++layer) {
      const uint32_t layer_base = src_base + layer * gl.array_slice_stride_bytes;
      for (uint32_t z = 0; z < ld; ++z) {
        uint8_t* host_slice =
            out.bytes.data() + level_offset + host_slice_bytes * (size_t(layer) * ld + z);
        for (uint32_t y = 0; y < hb; ++y) {
          for (uint32_t x = 0; x < wb; ++x) {
            const int32_t gx = int32_t(x + ox), gy = int32_t(y + oy), gz = int32_t(z + oz);
            uint32_t offset;
            if (key.tiled) {
              offset = is_3d ? uint32_t(texture_util::GetTiledOffset3D(gx, gy, gz, pitch_blocks,
                                                                       z_stride_rows, bpb_log2))
                             : uint32_t(texture_util::GetTiledOffset2D(gx, gy, pitch_blocks,
                                                                       bpb_log2));
            } else {
              offset = (uint32_t(gx) + pitch_blocks * (uint32_t(gy) + z_stride_rows * uint32_t(gz)))
                       << bpb_log2;
            }
            const uint32_t address = (layer_base + offset) & kGuestMemoryMask;
            if (address + bpb > kGuestMemorySize) {
              std::memset(block, 0, bpb);
            } else {
              std::memcpy(block, memory + address, bpb);
            }
            // Endian swap.
            if (bpb == 2) {
              if (endian == xenos::Endian::k8in16) std::swap(block[0], block[1]);
            } else if (bpb >= 4) {
              for (uint32_t i = 0; i < bpb; i += 4) {
                uint32_t v;
                std::memcpy(&v, block + i, 4);
                v = Swap32(v, endian);
                std::memcpy(block + i, &v, 4);
              }
            }
            uint8_t* dst = host_slice + size_t(y) * host_rows_per_block * host_row_bytes +
                           size_t(x) * host_bytes_per_block_x;
            switch (hf.convert) {
              case Convert::kCopy:
                std::memcpy(dst, block, bpb);
                break;
              case Convert::kR5G6B5:
              case Convert::kR5G5B5A1:
              case Convert::kR4G4B4A4:
              case Convert::kR5G5B6: {
                uint16_t v;
                std::memcpy(&v, block, 2);
                uint32_t p = v, r = 0;
                if (hf.convert == Convert::kR5G6B5) {
                  r = (p & 0x07E0) | ((p & 0x001F) << 11) | ((p & 0xF800) >> 11);
                } else if (hf.convert == Convert::kR5G5B5A1) {
                  r = (p & 0x83E0) | ((p & 0x001F) << 10) | ((p & 0x7C00) >> 10);
                } else if (hf.convert == Convert::kR4G4B4A4) {
                  r = ((p & 0x0FFF) << 4) | ((p & 0xF000) >> 12);
                } else {
                  r = ((p & 0x001F) << 11) | ((p & 0xFFE0) >> 5);
                }
                uint16_t o = uint16_t(r);
                std::memcpy(dst, &o, 2);
                break;
              }
              case Convert::kR11G11B10:
              case Convert::kR10G11B11: {
                uint32_t v;
                std::memcpy(&v, block, 4);
                uint16_t o[4];
                if (hf.convert == Convert::kR11G11B10) {
                  o[0] = Expand(v & 2047, 11);
                  o[1] = Expand((v >> 11) & 2047, 11);
                  o[2] = Expand((v >> 22) & 1023, 10);
                } else {
                  o[0] = Expand(v & 1023, 10);
                  o[1] = Expand((v >> 10) & 2047, 11);
                  o[2] = Expand((v >> 21) & 2047, 11);
                }
                o[3] = 0xFFFF;
                std::memcpy(dst, o, 8);
                break;
              }
              case Convert::kDepthUnorm:
              case Convert::kDepthFloat: {
                uint32_t v;
                std::memcpy(&v, block, 4);
                float f = hf.convert == Convert::kDepthUnorm
                              ? float(v >> 8) * (1.0f / 16777215.0f)
                              : xenos::Float20e4To32(v >> 8);
                std::memcpy(dst, &f, 4);
                break;
              }
              case Convert::kDXT3AToR8: {
                uint32_t d[2];
                std::memcpy(d, block, 8);
                for (uint32_t row = 0; row < 4; ++row) {
                  uint32_t bits = (d[row >> 1] >> ((row & 1) * 16)) & 0xFFFF;
                  for (uint32_t i = 0; i < 4; ++i) {
                    uint32_t a = (bits >> (i * 4)) & 0xF;
                    dst[row * host_row_bytes + i] = uint8_t(a | (a << 4));
                  }
                }
                break;
              }
              case Convert::kCTX1ToR8G8: {
                uint32_t d[2];
                std::memcpy(d, block, 8);
                uint32_t e[4][2];  // 0, 1, 2/3 0 + 1/3 1, 1/3 0 + 2/3 1; (R, G)
                e[0][0] = (d[0] >> 8) & 0xFF;
                e[0][1] = d[0] & 0xFF;
                e[1][0] = d[0] >> 24;
                e[1][1] = (d[0] >> 16) & 0xFF;
                for (uint32_t c = 0; c < 2; ++c) {
                  e[2][c] = (2 * e[0][c] + e[1][c]) / 3;
                  e[3][c] = (e[0][c] + 2 * e[1][c]) / 3;
                }
                for (uint32_t t = 0; t < 16; ++t) {
                  uint32_t code = (d[1] >> (2 * t)) & 3;
                  uint8_t* texel = dst + (t >> 2) * host_row_bytes + (t & 3) * 2;
                  texel[0] = uint8_t(e[code][0]);
                  texel[1] = uint8_t(e[code][1]);
                }
                break;
              }
            }
          }
        }
      }
    }

    VkBufferImageCopy region = {};
    region.bufferOffset = level_offset;
    region.bufferRowLength = wb * bw;
    region.bufferImageHeight = hb * bh;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
    region.imageExtent = {lw, lh, ld};
    out.regions.push_back(region);
  }
  return !out.regions.empty();
}

// ---- Debug decoding (CPU) ----

void DecodeColor565(uint16_t c, uint8_t* rgb) {
  rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  rgb[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes a BC1-style color block into 16 RGBA texels.
void DecodeBC1Color(const uint8_t* b, uint8_t out[16][4], bool allow_punchthrough) {
  uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t pal[4][4];
  DecodeColor565(c0, pal[0]);
  DecodeColor565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  if (c0 > c1 || !allow_punchthrough) {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
    pal[2][3] = 255;
    pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
  }
  uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) |
                 (uint32_t(b[7]) << 24);
  for (int t = 0; t < 16; ++t) std::memcpy(out[t], pal[(idx >> (2 * t)) & 3], 4);
}

void DecodeBC4(const uint8_t* b, uint8_t out[16]) {
  uint8_t a0 = b[0], a1 = b[1];
  uint8_t pal[8];
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) out[t] = pal[(bits >> (3 * t)) & 7];
}

float HalfToFloat(uint16_t h) {
  uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) {
    v = std::ldexp(float(m), -24);
  } else if (e == 31) {
    v = m ? NAN : INFINITY;
  } else {
    v = std::ldexp(float(m | 1024), int(e) - 25);
  }
  return s ? -v : v;
}

uint8_t FloatToU8(float f) {
  if (!(f > 0.0f)) return 0;
  if (f >= 1.0f) return 255;
  return uint8_t(f * 255.0f + 0.5f);
}

// Converts a host texel (uncompressed host format) to RGBA8 (before swizzle).
void HostTexelToRGBA(VkFormat format, const uint8_t* p, uint8_t* o) {
  o[0] = o[1] = o[2] = 0;
  o[3] = 255;
  switch (format) {
    case VK_FORMAT_R8_UNORM: o[0] = p[0]; break;
    case VK_FORMAT_R8G8_UNORM: o[0] = p[0]; o[1] = p[1]; break;
    case VK_FORMAT_R8G8B8A8_UNORM: std::memcpy(o, p, 4); break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
      uint32_t v;
      std::memcpy(&v, p, 4);
      o[0] = uint8_t((v & 1023) >> 2);
      o[1] = uint8_t(((v >> 10) & 1023) >> 2);
      o[2] = uint8_t(((v >> 20) & 1023) >> 2);
      o[3] = uint8_t((v >> 30) * 85);
      break;
    }
    case VK_FORMAT_R5G6B5_UNORM_PACK16: {
      uint16_t v;
      std::memcpy(&v, p, 2);
      DecodeColor565(v, o);
      break;
    }
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: {
      uint16_t v;
      std::memcpy(&v, p, 2);
      o[0] = uint8_t(((v >> 10) & 31) * 255 / 31);
      o[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
      o[2] = uint8_t((v & 31) * 255 / 31);
      o[3] = (v & 0x8000) ? 255 : 0;
      break;
    }
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16: {
      uint16_t v;
      std::memcpy(&v, p, 2);
      o[2] = uint8_t(((v >> 12) & 15) * 17);
      o[1] = uint8_t(((v >> 8) & 15) * 17);
      o[0] = uint8_t(((v >> 4) & 15) * 17);
      o[3] = uint8_t((v & 15) * 17);
      break;
    }
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16B16A16_UNORM: {
      uint32_t n = format == VK_FORMAT_R16_UNORM ? 1 : format == VK_FORMAT_R16G16_UNORM ? 2 : 4;
      for (uint32_t i = 0; i < n; ++i) o[i] = p[2 * i + 1];
      break;
    }
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SFLOAT: {
      uint32_t n = format == VK_FORMAT_R16_SFLOAT ? 1 : format == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
      for (uint32_t i = 0; i < n; ++i) o[i] = FloatToU8(HalfToFloat(uint16_t(p[2 * i] | (p[2 * i + 1] << 8))));
      break;
    }
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
      uint32_t n = format == VK_FORMAT_R32_SFLOAT ? 1 : format == VK_FORMAT_R32G32_SFLOAT ? 2 : 4;
      for (uint32_t i = 0; i < n; ++i) {
        float f;
        std::memcpy(&f, p + 4 * i, 4);
        o[i] = FloatToU8(f);
      }
      break;
    }
    default: break;
  }
}

uint32_t HostTexelBytes(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
  }
}

// ---- GPU texture loading (Xenia's texture_load_* compute shaders) ----

namespace shaders {
#include "src/graphics/shaders/vulkan_spirv/texture_load_128bpb_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_16bpb_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_32bpb_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_64bpb_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_8bpb_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_ctx1_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_depth_float_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_depth_unorm_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_dxt3a_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_r10g11b11_rgba16_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_r11g11b10_rgba16_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "src/graphics/shaders/vulkan_spirv/texture_load_r5g6b5_b5g6r5_cs.h"
}  // namespace shaders

// Subset of TextureCache::LoadShaderIndex used by the host formats above.
enum LoadShader : uint32_t {
  kLoad8bpb,
  kLoad16bpb,
  kLoad32bpb,
  kLoad64bpb,
  kLoad128bpb,
  kLoadR5G5B5A1ToB5G5R5A1,
  kLoadR5G6B5ToB5G6R5,
  kLoadR5G5B6ToB5G6R5WithRBGASwizzle,
  kLoadRGBA4ToARGB4,
  kLoadR10G11B11ToRGBA16,
  kLoadR11G11B10ToRGBA16,
  kLoadDXT3A,
  kLoadCTX1,
  kLoadDepthUnorm,
  kLoadDepthFloat,
  kLoadCount,
  kLoadNone = kLoadCount,
};

struct LoadShaderDesc {
  const uint32_t* code;
  size_t code_size;
  // TextureCache::LoadShaderInfo::bytes_per_host_block / guest_x_blocks_per_thread_log2.
  uint32_t bytes_per_host_block;
  uint32_t guest_x_blocks_per_thread_log2;
};

#define XGPU_LOAD_SHADER(name, bytes, x_log2) {shaders::name, sizeof(shaders::name), bytes, x_log2}
const LoadShaderDesc kLoadShaders[kLoadCount] = {
    XGPU_LOAD_SHADER(texture_load_8bpb_cs, 1, 4),
    XGPU_LOAD_SHADER(texture_load_16bpb_cs, 2, 4),
    XGPU_LOAD_SHADER(texture_load_32bpb_cs, 4, 3),
    XGPU_LOAD_SHADER(texture_load_64bpb_cs, 8, 2),
    XGPU_LOAD_SHADER(texture_load_128bpb_cs, 16, 1),
    XGPU_LOAD_SHADER(texture_load_r5g5b5a1_b5g5r5a1_cs, 2, 4),
    XGPU_LOAD_SHADER(texture_load_r5g6b5_b5g6r5_cs, 2, 4),
    XGPU_LOAD_SHADER(texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs, 2, 4),
    XGPU_LOAD_SHADER(texture_load_r4g4b4a4_a4r4g4b4_cs, 2, 4),
    XGPU_LOAD_SHADER(texture_load_r10g11b11_rgba16_cs, 8, 3),
    XGPU_LOAD_SHADER(texture_load_r11g11b10_rgba16_cs, 8, 3),
    XGPU_LOAD_SHADER(texture_load_dxt3a_cs, 1, 2),
    XGPU_LOAD_SHADER(texture_load_ctx1_cs, 2, 2),
    XGPU_LOAD_SHADER(texture_load_depth_unorm_cs, 4, 3),
    XGPU_LOAD_SHADER(texture_load_depth_float_cs, 4, 3),
};
#undef XGPU_LOAD_SHADER

// TextureCache::kLoadGuestXThreadsPerGroupLog2 / kLoadGuestYBlocksPerGroupLog2.
constexpr uint32_t kLoadGuestXThreadsPerGroupLog2 = 2;
constexpr uint32_t kLoadGuestYBlocksPerGroupLog2 = 5;

// TextureCache::LoadConstants (push constants of the load shaders).
struct LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
};
static_assert(sizeof(LoadConstants) == 40);

// The load shader producing HostFormat data (same conversions as the CPU
// loader, see VulkanTextureCache::kBestHostFormats).
LoadShader LoadShaderFor(TextureFormat format, const HostFormat& hf) {
  switch (hf.convert) {
    case Convert::kCopy:
      switch (FormatInfo::Get(format)->bytes_per_block()) {
        case 1: return kLoad8bpb;
        case 2: return kLoad16bpb;
        case 4: return kLoad32bpb;
        case 8: return kLoad64bpb;
        case 16: return kLoad128bpb;
        default: return kLoadNone;
      }
    case Convert::kR5G6B5: return kLoadR5G6B5ToB5G6R5;
    case Convert::kR5G5B5A1: return kLoadR5G5B5A1ToB5G5R5A1;
    case Convert::kR4G4B4A4: return kLoadRGBA4ToARGB4;
    case Convert::kR5G5B6: return kLoadR5G5B6ToB5G6R5WithRBGASwizzle;
    case Convert::kR11G11B10: return kLoadR11G11B10ToRGBA16;
    case Convert::kR10G11B11: return kLoadR10G11B11ToRGBA16;
    case Convert::kDepthUnorm: return kLoadDepthUnorm;
    case Convert::kDepthFloat: return kLoadDepthFloat;
    case Convert::kDXT3AToR8: return kLoadDXT3A;
    case Convert::kCTX1ToR8G8: return kLoadCTX1;
  }
  return kLoadNone;
}

struct SamplerKey {
  uint32_t clamp_x = 0, clamp_y = 0, clamp_z = 0;
  uint32_t border_color = 0, force_bc_w_to_max = 0;
  uint32_t mag_linear = 0, min_linear = 0, mip_linear = 0;
  uint32_t aniso = 0;
  uint32_t mip_min_level = 0, mip_base_map = 0;
  auto Tie() const {
    return std::tie(clamp_x, clamp_y, clamp_z, border_color, force_bc_w_to_max, mag_linear,
                    min_linear, mip_linear, aniso, mip_min_level, mip_base_map);
  }
  bool operator<(const SamplerKey& o) const { return Tie() < o.Tie(); }
};

}  // namespace

struct TextureCache::Impl {
  const char* last_source = "null";  // for tracing
  vk::Context& context;
  const uint8_t* memory;
  RenderTargets& render_targets;
  VkDevice device;
  VkPhysicalDeviceMemoryProperties memory_properties{};
  VkPhysicalDeviceProperties properties{};

  struct Texture {
    bool ok = false;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format_unsigned = VK_FORMAT_UNDEFINED, format_signed = VK_FORMAT_UNDEFINED;
    // (format, swizzle, view type) -> view
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, VkImageView> views;
    // Needs (re)loading: set by watch callbacks (any thread, under the
    // global critical region), InvalidateRange, or a content hash change.
    std::atomic<bool> outdated{true};
    // Permanently unloadable (unsupported format / image creation failed).
    bool failed = false;
    // Guest extents (bytes) of the base level and of the mips.
    uint32_t base_size = 0, mips_size = 0;
    // SharedMemory watches (owned under the global critical region; the
    // callback clears the handle it fires for).
    void* watch_base = nullptr;
    void* watch_mips = nullptr;
    // Without SharedMemory: guest memory content hash, rechecked on first use
    // in each frame (the guest rewrites textures in place: movies, streaming,
    // reused pools).
    uint64_t content_hash = 0;
    uint64_t checked_frame = 0;
  };
  uint64_t frame = 1;

  uint64_t HashGuestTexture(const TextureKey& key) const {
    const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
        key.dim(), key.pitch, key.width, key.height, key.depth, key.tiled != 0, key.fmt(),
        key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
    const uint32_t base_size = layout.base.level_data_extent_bytes;
    const uint32_t mip_size = layout.mips_total_extent_bytes;
    uint64_t hash = 0;
    auto add = [&](uint32_t page, uint32_t size) {
      const uint32_t address = (page << 12) & 0x1FFFFFFF;
      size = std::min<uint32_t>(size, 0x20000000u - address);
      if (page && size) hash = XXH3_64bits_withSeed(memory + address, size, hash);
    };
    add(key.base_page, base_size);
    if (key.mip_max_level) add(key.mip_page, mip_size);
    return hash;
  }
  std::map<TextureKey, Texture> textures;

  // Views on resolved images: (image, memory, format, swizzle, w, h) -> view.
  std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t>, VkImageView> resolved_views;

  struct NullImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  NullImage null_2d_array, null_cube, null_3d;
  bool null_initialized = false;

  std::map<SamplerKey, VkSampler> samplers;
  std::map<VkFormat, VkFormatFeatureFlags> format_features;

  // Staging memory (freed in EndFrame).
  std::vector<vk::Buffer> staging_chunks;
  VkDeviceSize staging_used = 0;  // in the last chunk

  // GPU loading.
  VkBuffer shared_buffer = VK_NULL_HANDLE;
  std::function<bool(uint32_t, uint32_t)> request_range;
  SharedMemory* shared_memory = nullptr;
  VkDescriptorSetLayout load_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout load_layout = VK_NULL_HANDLE;
  VkDescriptorPool load_pool = VK_NULL_HANDLE;
  VkDescriptorSet source_set = VK_NULL_HANDLE;
  VkPipeline load_pipelines[kLoadCount] = {};
  bool load_pipeline_failed[kLoadCount] = {};
  // Device-local scratch buffers the load shaders write (host layout), each
  // with its storage buffer descriptor. The first is kept across frames.
  struct Scratch {
    vk::Buffer buffer;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDeviceSize used = 0;
  };
  std::vector<Scratch> scratch;
  uint32_t gpu_loads = 0, cpu_loads = 0;

  uint32_t loaded_count = 0, resolved_hits = 0;
  std::set<TextureKey> unsupported;
  std::set<std::pair<uint32_t, uint32_t>> logged_resolve_mismatch;

  Impl(vk::Context& c, const uint8_t* m, RenderTargets& rt)
      : context(c), memory(m), render_targets(rt), device(c.device()) {
    vkGetPhysicalDeviceMemoryProperties(c.physical_device(), &memory_properties);
    vkGetPhysicalDeviceProperties(c.physical_device(), &properties);
  }

  ~Impl() {
    if (std::getenv("XGPU_TEXTURE_LOG")) {
      std::fprintf(stderr, "textures: %u GPU loads, %u CPU loads\n", gpu_loads, cpu_loads);
    }
    vkDeviceWaitIdle(device);
    UnwatchAll();
    for (auto& [key, texture] : textures) DestroyTexture(texture);
    DestroyLoadResources();
    for (auto& [key, view] : resolved_views) vkDestroyImageView(device, view, nullptr);
    for (NullImage* n : {&null_2d_array, &null_cube, &null_3d}) {
      if (n->view) vkDestroyImageView(device, n->view, nullptr);
      if (n->image) vkDestroyImage(device, n->image, nullptr);
      if (n->memory) vkFreeMemory(device, n->memory, nullptr);
    }
    for (auto& [key, sampler] : samplers) vkDestroySampler(device, sampler, nullptr);
    FreeStaging();
  }

  void DestroyTexture(Texture& texture) {
    for (auto& [key, view] : texture.views) vkDestroyImageView(device, view, nullptr);
    texture.views.clear();
    if (texture.image) vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(device, texture.memory, nullptr);
    texture.image = VK_NULL_HANDLE;
    texture.memory = VK_NULL_HANDLE;
  }

  void FreeStaging() {
    for (vk::Buffer& buffer : staging_chunks) context.DestroyBuffer(buffer);
    staging_chunks.clear();
    staging_used = 0;
  }

  VkFormatFeatureFlags Features(VkFormat format) {
    auto it = format_features.find(format);
    if (it != format_features.end()) return it->second;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(context.physical_device(), format, &props);
    format_features[format] = props.optimalTilingFeatures;
    return props.optimalTilingFeatures;
  }
  bool Sampleable(VkFormat format) {
    return format == VK_FORMAT_UNDEFINED ||
           (Features(format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
  }

  uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & flags) == flags) {
        return i;
      }
    }
    return UINT32_MAX;
  }

  bool CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory_out) {
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
      image = VK_NULL_HANDLE;
      return false;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex =
        FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &alloc, nullptr, &memory_out) != VK_SUCCESS) {
      vkDestroyImage(device, image, nullptr);
      image = VK_NULL_HANDLE;
      memory_out = VK_NULL_HANDLE;
      return false;
    }
    vkBindImageMemory(device, image, memory_out, 0);
    return true;
  }

  static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                      VkImageLayout new_layout) {
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
  }

  // Suballocates host-visible staging memory; returns the buffer and offset.
  bool AllocateStaging(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset, void*& mapped) {
    constexpr VkDeviceSize kChunkSize = 64 * 1024 * 1024;
    size = (size + 15) & ~VkDeviceSize(15);
    if (staging_chunks.empty() || staging_used + size > staging_chunks.back().size) {
      vk::Buffer chunk;
      if (!context.CreateBuffer(chunk, std::max(kChunkSize, size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                true)) {
        return false;
      }
      staging_chunks.push_back(chunk);
      staging_used = 0;
    }
    vk::Buffer& chunk = staging_chunks.back();
    buffer = chunk.buffer;
    offset = staging_used;
    mapped = static_cast<uint8_t*>(chunk.mapped) + staging_used;
    staging_used += size;
    return true;
  }

  // ---- Watches ----

  static void WatchCallback(const std::unique_lock<std::recursive_mutex>& /*global_lock*/, void* /*context*/,
                            void* data, uint64_t argument, bool /*invalidated_by_gpu*/) {
    Texture& texture = *static_cast<Texture*>(data);
    (argument ? texture.watch_mips : texture.watch_base) = nullptr;
    texture.outdated.store(true, std::memory_order_release);
  }

  void Unwatch(Texture& texture) {
    if (!shared_memory) return;
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    if (texture.watch_base) shared_memory->UnwatchMemoryRange(texture.watch_base);
    if (texture.watch_mips) shared_memory->UnwatchMemoryRange(texture.watch_mips);
    texture.watch_base = texture.watch_mips = nullptr;
  }

  void UnwatchAll() {
    if (!shared_memory) return;
    for (auto& [key, texture] : textures) {
      Unwatch(texture);
      texture.outdated.store(true, std::memory_order_release);
    }
  }

  // Installs the watches after a load if the loaded ranges are still valid in
  // the shared memory (else the texture stays outdated and reloads next use).
  void Watch(Texture& texture, const TextureKey& key, bool base, bool mips) {
    auto lock = rex::thread::global_critical_region::AcquireDirect();
    const uint32_t base_address = key.base_page << 12, mips_address = key.mip_page << 12;
    if ((base && !shared_memory->IsRangeValid(base_address, (texture.base_size + 15) & ~15u)) ||
        (mips && !shared_memory->IsRangeValid(mips_address, (texture.mips_size + 15) & ~15u))) {
      texture.outdated.store(true, std::memory_order_release);
      return;
    }
    if (base) {
      texture.watch_base = shared_memory->WatchMemoryRange(base_address, texture.base_size, WatchCallback,
                                                           this, &texture, 0);
    }
    if (mips) {
      texture.watch_mips = shared_memory->WatchMemoryRange(mips_address, texture.mips_size, WatchCallback,
                                                           this, &texture, 1);
    }
    texture.outdated.store(false, std::memory_order_release);
  }

  // ---- GPU load resources ----

  bool InitLoadResources() {
    if (load_layout) return true;
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &load_set_layout) != VK_SUCCESS) {
      return false;
    }
    // Set 0: destination (scratch), set 1: source (shared memory).
    VkDescriptorSetLayout sets[2] = {load_set_layout, load_set_layout};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LoadConstants)};
    VkPipelineLayoutCreateInfo pipeline_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 2;
    pipeline_layout_info.pSetLayouts = sets;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &load_layout) != VK_SUCCESS) {
      load_layout = VK_NULL_HANDLE;
      return false;
    }
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256};
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 256;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    return vkCreateDescriptorPool(device, &pool_info, nullptr, &load_pool) == VK_SUCCESS;
  }

  VkDescriptorSet AllocateBufferSet(VkBuffer buffer, VkDeviceSize range) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = load_pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &load_set_layout;
    if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo buffer_info = {buffer, 0, range};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return set;
  }

  void SetSource(VkBuffer buffer) {
    if (source_set) {
      vkFreeDescriptorSets(device, load_pool, 1, &source_set);
      source_set = VK_NULL_HANDLE;
    }
    shared_buffer = buffer;
    if (!buffer || !InitLoadResources()) return;
    if (properties.limits.maxStorageBufferRange < kGuestMemorySize) {
      std::fprintf(stderr, "textures: maxStorageBufferRange < 512 MB, loading textures on the CPU\n");
      return;
    }
    source_set = AllocateBufferSet(buffer, kGuestMemorySize);
  }

  VkPipeline GetLoadPipeline(LoadShader shader) {
    if (load_pipelines[shader] || load_pipeline_failed[shader]) return load_pipelines[shader];
    const LoadShaderDesc& desc = kLoadShaders[shader];
    VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = desc.code_size;
    module_info.pCode = desc.code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &module_info, nullptr, &module) == VK_SUCCESS) {
      VkComputePipelineCreateInfo info = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      info.stage.module = module;
      info.stage.pName = "main";
      info.layout = load_layout;
      if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &load_pipelines[shader]) !=
          VK_SUCCESS) {
        load_pipelines[shader] = VK_NULL_HANDLE;
      }
      vkDestroyShaderModule(device, module, nullptr);
    }
    if (!load_pipelines[shader]) {
      load_pipeline_failed[shader] = true;
      std::fprintf(stderr, "textures: failed to create texture load pipeline %u\n", uint32_t(shader));
    }
    return load_pipelines[shader];
  }

  // Suballocates device-local scratch memory for load shader output.
  Scratch* AllocateScratch(VkDeviceSize size, VkDeviceSize& offset) {
    constexpr VkDeviceSize kChunkSize = 64 * 1024 * 1024;
    size = (size + 255) & ~VkDeviceSize(255);
    if (scratch.empty() || scratch.back().used + size > scratch.back().buffer.size) {
      if (std::max(kChunkSize, size) > properties.limits.maxStorageBufferRange) return nullptr;
      Scratch chunk;
      if (!context.CreateBuffer(chunk.buffer, std::max(kChunkSize, size),
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                false)) {
        return nullptr;
      }
      chunk.set = AllocateBufferSet(chunk.buffer.buffer, chunk.buffer.size);
      if (!chunk.set) {
        context.DestroyBuffer(chunk.buffer);
        return nullptr;
      }
      scratch.push_back(chunk);
    }
    Scratch& chunk = scratch.back();
    offset = chunk.used;
    chunk.used += size;
    return &chunk;
  }

  void RecycleScratch() {
    // Keep the first chunk (the common case fits), free the others.
    for (size_t i = scratch.size(); i-- > 1;) {
      vkFreeDescriptorSets(device, load_pool, 1, &scratch[i].set);
      context.DestroyBuffer(scratch[i].buffer);
    }
    if (scratch.size() > 1) scratch.resize(1);
    if (!scratch.empty()) scratch[0].used = 0;
  }

  void DestroyLoadResources() {
    for (Scratch& chunk : scratch) context.DestroyBuffer(chunk.buffer);
    scratch.clear();
    for (VkPipeline& pipeline : load_pipelines) {
      if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    if (load_pool) vkDestroyDescriptorPool(device, load_pool, nullptr);
    if (load_layout) vkDestroyPipelineLayout(device, load_layout, nullptr);
    if (load_set_layout) vkDestroyDescriptorSetLayout(device, load_set_layout, nullptr);
    load_pool = VK_NULL_HANDLE;
    load_layout = VK_NULL_HANDLE;
    load_set_layout = VK_NULL_HANDLE;
    source_set = VK_NULL_HANDLE;
  }

  void InitNullImages(VkCommandBuffer cmd) {
    if (null_initialized) return;
    null_initialized = true;
    struct Desc {
      NullImage* image;
      VkImageType type;
      VkImageViewType view_type;
      uint32_t layers;
      VkImageCreateFlags flags;
    } descs[] = {
        {&null_2d_array, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 0},
        {&null_cube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6,
         VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
        {&null_3d, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0},
    };
    for (const Desc& d : descs) {
      VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.flags = d.flags;
      info.imageType = d.type;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = d.layers;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      if (!CreateImage(info, d.image->image, d.image->memory)) continue;
      Barrier(cmd, d.image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue zero = {};
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d.layers};
      vkCmdClearColorImage(cmd, d.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                           &range);
      Barrier(cmd, d.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = d.image->image;
      view.viewType = d.view_type;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.subresourceRange = range;
      vkCreateImageView(device, &view, nullptr, &d.image->view);
    }
  }

  VkImageView NullView(VkCommandBuffer cmd, VkImageViewType type) {
    InitNullImages(cmd);
    switch (type) {
      case VK_IMAGE_VIEW_TYPE_CUBE: return null_cube.view;
      case VK_IMAGE_VIEW_TYPE_3D: return null_3d.view;
      default: return null_2d_array.view;
    }
  }

  // Returns the resolved image to sample directly for this fetch constant, or
  // null to load from memory. Only exact matches with the most recent resolve
  // to the base address (the resolve also wrote memory, so both agree).
  vk::Image* FindCompatibleResolved(const FetchInfo& info) {
    const TextureKey& key = info.key;
    if (!key.base_page) return nullptr;
    vk::Image* image = render_targets.FindResolvedMutable(key.base_page << 12);
    if (!image || !image->image) return nullptr;
    // The CPU put something else there since the resolve: use memory.
    if (!render_targets.IsResolvedCurrent(key.base_page << 12)) return nullptr;
    // Resolved images are created mutable, so any uncompressed host format
    // with the same texel size views the same bits (the guest reinterprets
    // resolve output, e.g. 8_8_8_8 as 2_10_10_10, depth as 24_8).
    const HostFormat& hf = GetHostFormat(key.fmt());
    const bool compatible =
        key.dim() == xenos::DataDimension::k2DOrStacked && key.depth == 1 && key.tiled &&
        (key.mip_max_level == 0 || key.mip_page == 0) && hf.supported() && !hf.host_compressed &&
        FormatInfo::Get(key.fmt())->block_width == 1 &&
        HostTexelBytes(hf.format_unsigned != VK_FORMAT_UNDEFINED ? hf.format_unsigned : hf.format_signed) ==
            HostTexelBytes(image->format) &&
        key.width == image->width && key.height == image->height;
    if (!compatible) {
      if (std::getenv("XGPU_TEXTURE_LOG") &&
          logged_resolve_mismatch.insert({key.base_page, key.format}).second) {
        std::fprintf(stderr,
                     "textures: resolved image at %08X (%ux%u vk format %d) doesn't match fetch "
                     "(%s %ux%ux%u dim %u mips %u); loading memory instead\n",
                     key.base_page << 12, image->width, image->height, int(image->format),
                     FormatInfo::Get(key.fmt())->name, key.width, key.height, key.depth, key.dimension,
                     key.mip_max_level);
      }
      return nullptr;
    }
    return image;
  }

  // Records the GPU load of all levels of `texture` from the shared memory
  // buffer (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl without
  // resolution scaling). The image is left in TRANSFER_DST_OPTIMAL.
  bool LoadGpu(VkCommandBuffer cmd, Texture& texture, const TextureKey& key, const HostFormat& hf) {
    const LoadShader shader = LoadShaderFor(key.fmt(), hf);
    if (shader == kLoadNone || !source_set) return false;
    const VkPipeline pipeline = GetLoadPipeline(shader);
    if (!pipeline) return false;
    const LoadShaderDesc& info = kLoadShaders[shader];

    const FormatInfo* fi = FormatInfo::Get(key.fmt());
    const uint32_t bw = fi->block_width, bh = fi->block_height, bpb = fi->bytes_per_block();
    const bool is_3d = key.dim() == xenos::DataDimension::k3D;
    const uint32_t width = key.width;
    const uint32_t height = key.dim() == xenos::DataDimension::k1D ? 1 : key.height;
    const uint32_t depth = is_3d ? key.depth : 1;
    const uint32_t array_size = is_3d ? 1 : key.depth;
    const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
        key.dim(), key.pitch, width, height, key.depth, key.tiled != 0, key.fmt(), key.packed_mips != 0,
        key.base_page != 0, key.mip_max_level);

    const bool load_base = key.base_page != 0;
    const bool load_mips = key.mip_max_level != 0 && key.mip_page != 0;
    if (!load_base && !load_mips) return false;
    const uint32_t level_first = load_base ? 0 : 1;
    const uint32_t level_last = load_mips ? key.mip_max_level : 0;
    const uint32_t level_packed = layout.packed_level;
    const uint32_t level_stored_first = std::min(level_first, level_packed);
    const uint32_t level_stored_last = std::min(level_last, level_packed);
    // With the packed tail at level 0, iteration 0 is the base tail and 1 the
    // mips tail; otherwise the iteration is the stored level.
    uint32_t loop_first, loop_last;
    if (level_packed == 0) {
      loop_first = uint32_t(level_first != 0);
      loop_last = uint32_t(level_last != 0);
    } else {
      loop_first = level_stored_first;
      loop_last = level_stored_last;
    }

    const uint32_t host_block_width = hf.host_compressed ? bw : 1;
    const uint32_t host_block_height = hf.host_compressed ? bh : 1;
    uint32_t host_x_blocks_per_thread = 1u << info.guest_x_blocks_per_thread_log2;
    if (!hf.host_compressed) host_x_blocks_per_thread *= bw;
    struct HostLayout {
      VkDeviceSize offset_bytes = 0, slice_size_bytes = 0;
      uint32_t x_pitch_blocks = 0, y_pitch_blocks = 0;
    };
    HostLayout host_base, host_mips[xenos::kTextureMaxMips];
    VkDeviceSize host_size = 0;
    auto level_extent = [&](bool is_base, uint32_t level, uint32_t& w, uint32_t& h, uint32_t& d) {
      if (level == level_packed) {
        const texture_util::TextureGuestLayout::Level& packed = is_base ? layout.base : layout.mips[level];
        w = packed.x_extent_blocks * bw;
        h = packed.y_extent_blocks * bh;
        d = packed.z_extent;
      } else {
        w = std::max(width >> level, 1u);
        h = std::max(height >> level, 1u);
        d = std::max(depth >> level, 1u);
      }
    };
    for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
      const bool is_base = loop == 0;
      const uint32_t level = level_packed == 0 ? 0 : loop;
      HostLayout& hl = is_base ? host_base : host_mips[level];
      hl.offset_bytes = host_size;
      uint32_t w, h, d;
      level_extent(is_base, level, w, h, d);
      const uint32_t x_blocks = (w + host_block_width - 1) / host_block_width;
      hl.x_pitch_blocks = (x_blocks + host_x_blocks_per_thread - 1) / host_x_blocks_per_thread *
                          host_x_blocks_per_thread;
      hl.y_pitch_blocks = (h + host_block_height - 1) / host_block_height;
      hl.slice_size_bytes =
          VkDeviceSize(info.bytes_per_host_block) * hl.x_pitch_blocks * hl.y_pitch_blocks * d;
      host_size += hl.slice_size_bytes * array_size;
    }
    VkDeviceSize scratch_offset = 0;
    Scratch* chunk = AllocateScratch(host_size, scratch_offset);
    if (!chunk || scratch_offset + host_size > UINT32_MAX) return false;

    // Make GPU writes to the shared memory (uploads, resolves, memexport)
    // visible to the load shaders.
    VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory_barrier, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDescriptorSet sets[2] = {chunk->set, source_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, load_layout, 0, 2, sets, 0, nullptr);
    LoadConstants constants = {};
    constants.is_tiled_3d_endian_scale = uint32_t(key.tiled != 0) | (uint32_t(is_3d) << 1) |
                                         (key.endianness << 2) | (1u << 4) | (1u << 7);
    const uint32_t x_blocks_per_group_log2 = kLoadGuestXThreadsPerGroupLog2 + info.guest_x_blocks_per_thread_log2;
    for (uint32_t loop = loop_first; loop <= loop_last; ++loop) {
      const bool is_base = loop == 0;
      const uint32_t level = level_packed == 0 ? 0 : loop;
      const texture_util::TextureGuestLayout::Level& gl = is_base ? layout.base : layout.mips[level];
      constants.guest_offset = (is_base ? key.base_page : key.mip_page) << 12;
      if (!is_base) constants.guest_offset += layout.mip_offsets_bytes[level];
      constants.guest_pitch_aligned = gl.row_pitch_bytes / bpb;
      constants.guest_z_stride_block_rows_aligned = gl.z_slice_stride_block_rows;
      uint32_t w, h, d;
      level_extent(is_base, level, w, h, d);
      constants.size_blocks[0] = (w + bw - 1) / bw;
      constants.size_blocks[1] = (h + bh - 1) / bh;
      constants.size_blocks[2] = d;
      constants.height_texels = h;
      const uint32_t groups_x =
          (constants.size_blocks[0] + (1u << x_blocks_per_group_log2) - 1) >> x_blocks_per_group_log2;
      const uint32_t groups_y =
          (constants.size_blocks[1] + (1u << kLoadGuestYBlocksPerGroupLog2) - 1) >> kLoadGuestYBlocksPerGroupLog2;
      const HostLayout& hl = is_base ? host_base : host_mips[level];
      constants.host_offset = uint32_t(scratch_offset + hl.offset_bytes);
      constants.host_pitch = info.bytes_per_host_block * hl.x_pitch_blocks;
      for (uint32_t slice = 0; slice < array_size; ++slice) {
        vkCmdPushConstants(cmd, load_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(cmd, groups_x, groups_y, constants.size_blocks[2]);
        constants.guest_offset += gl.array_slice_stride_bytes;
        constants.host_offset += uint32_t(hl.slice_size_bytes);
      }
    }

    VkBufferMemoryBarrier buffer_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    buffer_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    buffer_barrier.srcQueueFamilyIndex = buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_barrier.buffer = chunk->buffer.buffer;
    buffer_barrier.offset = scratch_offset;
    buffer_barrier.size = host_size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         1, &buffer_barrier, 0, nullptr);

    std::vector<VkBufferImageCopy> regions;
    for (uint32_t level = level_first; level <= level_last; ++level) {
      const HostLayout& hl = level != 0 ? host_mips[std::min(level, level_packed)] : host_base;
      VkBufferImageCopy region = {};
      region.bufferOffset = scratch_offset + hl.offset_bytes;
      if (level >= level_packed) {
        uint32_t ox, oy, oz;
        texture_util::GetPackedMipOffset(width, height, depth, key.fmt(), level, ox, oy, oz);
        if (!hf.host_compressed) {
          ox *= bw;
          oy *= bh;
        }
        region.bufferOffset += VkDeviceSize(info.bytes_per_host_block) *
                               (ox + VkDeviceSize(hl.x_pitch_blocks) * (oy + VkDeviceSize(hl.y_pitch_blocks) * oz));
      }
      region.bufferRowLength = hl.x_pitch_blocks * host_block_width;
      region.bufferImageHeight = hl.y_pitch_blocks * host_block_height;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, array_size};
      region.imageExtent.width = std::min(std::max(width >> level, 1u), region.bufferRowLength);
      region.imageExtent.height = std::min(std::max(height >> level, 1u), region.bufferImageHeight);
      region.imageExtent.depth = std::max(depth >> level, 1u);
      regions.push_back(region);
    }
    Barrier(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, chunk->buffer.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           uint32_t(regions.size()), regions.data());
    return true;
  }

  // CPU load from the guest memory image through staging. Leaves the image in
  // TRANSFER_DST_OPTIMAL.
  bool LoadCpu(VkCommandBuffer cmd, Texture& texture, const TextureKey& key, const HostFormat& hf) {
    HostData data;
    if (!BuildHostData(memory, key, hf, data)) return false;
    VkBuffer staging;
    VkDeviceSize staging_offset;
    void* mapped;
    if (!AllocateStaging(data.bytes.size(), staging, staging_offset, mapped)) return false;
    std::memcpy(mapped, data.bytes.data(), data.bytes.size());
    for (VkBufferImageCopy& region : data.regions) region.bufferOffset += staging_offset;
    Barrier(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           uint32_t(data.regions.size()), data.regions.data());
    return true;
  }

  bool CreateTextureImage(Texture& texture, const TextureKey& key, const HostFormat& hf) {
    const bool is_3d = key.dim() == xenos::DataDimension::k3D;
    const bool is_cube = key.dim() == xenos::DataDimension::kCube;
    texture.format_unsigned = hf.format_unsigned;
    texture.format_signed = hf.format_signed;
    VkFormat formats[2] = {hf.format_unsigned != VK_FORMAT_UNDEFINED ? hf.format_unsigned : hf.format_signed,
                           VK_FORMAT_UNDEFINED};
    if (hf.format_unsigned != VK_FORMAT_UNDEFINED && hf.format_signed != VK_FORMAT_UNDEFINED &&
        hf.format_signed != hf.format_unsigned) {
      formats[1] = hf.format_signed;
    }
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageFormatListCreateInfo format_list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (formats[1] != VK_FORMAT_UNDEFINED) {
      info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      format_list.viewFormatCount = 2;
      format_list.pViewFormats = formats;
      info.pNext = &format_list;
    }
    if (is_cube) info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = formats[0];
    info.extent = {key.width, key.dim() == xenos::DataDimension::k1D ? 1 : key.height, is_3d ? key.depth : 1};
    info.mipLevels = key.mip_max_level + 1;
    info.arrayLayers = is_3d ? 1 : key.depth;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!CreateImage(info, texture.image, texture.memory)) {
      std::fprintf(stderr, "textures: failed to create %ux%ux%u image (%s)\n", info.extent.width,
                   info.extent.height, info.extent.depth, FormatInfo::Get(key.fmt())->name);
      return false;
    }
    return true;
  }

  Texture* LoadTexture(VkCommandBuffer cmd, const TextureKey& key) {
    auto [it, inserted] = textures.try_emplace(key);
    Texture& texture = it->second;
    if (!inserted) {
      if (texture.failed) return nullptr;
      if (!shared_memory && texture.checked_frame != frame) {
        texture.checked_frame = frame;
        if (HashGuestTexture(key) != texture.content_hash) texture.outdated.store(true);
      }
      if (!texture.outdated.load(std::memory_order_acquire)) return texture.ok ? &texture : nullptr;
    }
    // (Re)load. The image is reused (written in place, ordered after earlier
    // reads by the layout barrier), so views recorded earlier stay valid.
    Unwatch(texture);
    const HostFormat& hf = GetHostFormat(key.fmt());
    auto fail = [&]() -> Texture* {
      texture.failed = true;
      texture.outdated.store(false);
      unsupported.insert(key);
      return nullptr;
    };
    if (!hf.supported() || !Sampleable(hf.format_unsigned) || !Sampleable(hf.format_signed)) return fail();
    if (key.dim() == xenos::DataDimension::kCube && key.width != key.height) return fail();
    const FormatInfo* fi = FormatInfo::Get(key.fmt());
    if (!fi->bytes_per_block() || fi->bytes_per_block() > 16) return fail();
    if (inserted) {
      const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
          key.dim(), key.pitch, key.width, key.dim() == xenos::DataDimension::k1D ? 1 : key.height, key.depth,
          key.tiled != 0, key.fmt(), key.packed_mips != 0, key.base_page != 0, key.mip_max_level);
      texture.base_size = key.base_page ? layout.base.level_data_extent_bytes : 0;
      texture.mips_size = key.mip_max_level && key.mip_page ? layout.mips_total_extent_bytes : 0;
    }
    if (!texture.image && !CreateTextureImage(texture, key, hf)) return fail();
    if (!shared_memory) {
      texture.content_hash = HashGuestTexture(key);
      texture.checked_frame = frame;
    }
    // Make the ranges current in the shared memory buffer.
    if (shared_buffer && request_range) {
      if (texture.base_size) request_range(key.base_page << 12, (texture.base_size + 15) & ~15u);
      if (texture.mips_size) request_range(key.mip_page << 12, (texture.mips_size + 15) & ~15u);
    }
    bool loaded = false;
    static const bool force_cpu = std::getenv("XGPU_CPU_TEXTURES") != nullptr;  // debug
    if (shared_buffer && !force_cpu && LoadGpu(cmd, texture, key, hf)) {
      loaded = true;
      ++gpu_loads;
    } else if (LoadCpu(cmd, texture, key, hf)) {
      loaded = true;
      ++cpu_loads;
    }
    if (!loaded) {
      if (!texture.ok) return fail();
      // Keep the previous contents; retry next use.
      return &texture;
    }
    Barrier(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture.ok = true;
    ++loaded_count;
    if (shared_memory) {
      Watch(texture, key, texture.base_size != 0, texture.mips_size != 0);
    } else {
      texture.outdated.store(false);
    }
    return &texture;
  }

  void InvalidateRange(uint32_t start, uint32_t length) {
    start &= kGuestMemoryMask;
    const uint64_t end = uint64_t(start) + length;
    auto overlaps = [&](uint32_t page, uint32_t size) {
      const uint64_t address = uint64_t(page) << 12;
      return size && address < end && address + size > start;
    };
    for (auto& [key, texture] : textures) {
      if (overlaps(key.base_page, texture.base_size) || overlaps(key.mip_page, texture.mips_size)) {
        texture.outdated.store(true, std::memory_order_release);
      }
    }
  }

  VkImageView TextureView(Texture& texture, VkFormat format, uint32_t swizzle,
                          VkImageViewType type) {
    auto view_key = std::make_tuple(uint32_t(format), swizzle, uint32_t(type));
    auto it = texture.views.find(view_key);
    if (it != texture.views.end()) return it->second;
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = texture.image;
    info.viewType = type;
    info.format = format;
    info.components = {ComponentSwizzle(swizzle, 0), ComponentSwizzle(swizzle, 1),
                       ComponentSwizzle(swizzle, 2), ComponentSwizzle(swizzle, 3)};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                             VK_REMAINING_ARRAY_LAYERS};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    texture.views[view_key] = view;
    return view;
  }

  VkImageView ResolvedView(vk::Image& image, VkFormat format, uint32_t swizzle) {
    auto key = std::make_tuple(image.id, uint32_t(format), swizzle, image.width, image.height);
    auto it = resolved_views.find(key);
    if (it != resolved_views.end()) return it->second;
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image.image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = format;
    info.components = {ComponentSwizzle(swizzle, 0), ComponentSwizzle(swizzle, 1),
                       ComponentSwizzle(swizzle, 2), ComponentSwizzle(swizzle, 3)};
    info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    resolved_views[key] = view;
    return view;
  }
};

TextureCache::TextureCache(vk::Context& context, const uint8_t* guest_memory,
                           RenderTargets& render_targets)
    : impl_(std::make_unique<Impl>(context, guest_memory, render_targets)) {}

TextureCache::~TextureCache() = default;

VkImageView TextureCache::GetView(VkCommandBuffer cmd, const RegisterFile& regs,
                                  const SpirvShader::TextureBinding& binding) {
  VkImageView result = GetViewImpl(cmd, regs, binding);
  if (g_trace) {
    const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
    const FetchInfo info = DescribeFetch(fetch);
    std::printf("  tex fc %2u %08X %s %ux%u dim %u signed %u -> %s\n", uint32_t(binding.fetch_constant),
                info.key.base_page << 12, FormatInfo::Get(info.key.fmt())->name, info.key.width, info.key.height,
                info.key.dimension, uint32_t(binding.is_signed), impl_->last_source);
  }
  return result;
}

VkImageView TextureCache::GetViewImpl(VkCommandBuffer cmd, const RegisterFile& regs,
                                      const SpirvShader::TextureBinding& binding) {
  Impl& impl = *impl_;
  impl.last_source = "null";
  const VkImageViewType view_type = ViewTypeFor(binding.dimension);
  const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
  const FetchInfo info = DescribeFetch(fetch);
  if (!info.valid || !AreDimensionsCompatible(binding.dimension, info.key.dim())) {
    return impl.NullView(cmd, view_type);
  }
  // 3D textures sampled as 2D need a special 2D view in Xenia - not supported.
  if (info.key.dim() == xenos::DataDimension::k3D &&
      view_type != VK_IMAGE_VIEW_TYPE_3D) {
    impl.unsupported.insert(info.key);
    return impl.NullView(cmd, view_type);
  }
  // The translator samples the signed view only for signed components and the
  // unsigned view only for the others.
  if (binding.is_signed ? !texture_util::IsAnySignSigned(info.swizzled_signs)
                        : !texture_util::IsAnySignNotSigned(info.swizzled_signs)) {
    return impl.NullView(cmd, view_type);
  }
  const HostFormat& hf = GetHostFormat(info.key.fmt());
  const uint32_t host_swizzle = GuestToHostSwizzle(fetch.swizzle, hf.swizzle);

  if (vk::Image* resolved = impl.FindCompatibleResolved(info)) {
    vk::Context::Transition(cmd, *resolved, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ++impl.resolved_hits;
    impl.last_source = "resolved-image";
    VkFormat view_format = binding.is_signed ? hf.format_signed : hf.format_unsigned;
    if (view_format == VK_FORMAT_UNDEFINED) view_format = hf.format_unsigned;
    uint32_t resolved_swizzle = host_swizzle;
    static const bool no_swap = std::getenv("XGPU_NO_RESOLVE_SWAP") != nullptr;  // debug
    if (!no_swap && impl.render_targets.IsResolvedSwapped(info.key.base_page << 12)) {
      resolved_swizzle = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t c = (host_swizzle >> (3 * i)) & 0b111;
        c = c == 0 ? 2 : c == 2 ? 0 : c;
        resolved_swizzle |= c << (3 * i);
      }
    }
    VkImageView view = impl.ResolvedView(*resolved, view_format, resolved_swizzle);
    return view ? view : impl.NullView(cmd, view_type);
  }

  Impl::Texture* texture = impl.LoadTexture(cmd, info.key);
  if (!texture) return impl.NullView(cmd, view_type);
  impl.last_source = "memory";
  VkFormat format = binding.is_signed ? texture->format_signed : texture->format_unsigned;
  if (format == VK_FORMAT_UNDEFINED) return impl.NullView(cmd, view_type);
  VkImageView view = impl.TextureView(*texture, format, host_swizzle, view_type);
  return view ? view : impl.NullView(cmd, view_type);
}

VkSampler TextureCache::GetSampler(const RegisterFile& regs,
                                   const SpirvShader::SamplerBinding& binding) {
  Impl& impl = *impl_;
  const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerKey key;
  xenos::ClampMode clamp[3];
  texture_util::GetClampModesForDimension(fetch, clamp[0], clamp[1], clamp[2]);
  bool uses_border = false;
  for (xenos::ClampMode& c : clamp) {
    if (c == xenos::ClampMode::kClampToHalfway) c = xenos::ClampMode::kClampToEdge;
    // No mirror-once-to-border/halfway in Vulkan; mirror-clamp-to-edge needs
    // samplerMirrorClampToEdge, which the context doesn't enable.
    if (c == xenos::ClampMode::kMirrorClampToEdge || c == xenos::ClampMode::kMirrorClampToHalfway ||
        c == xenos::ClampMode::kMirrorClampToBorder) {
      c = xenos::ClampMode::kMirroredRepeat;
    }
    uses_border |= xenos::ClampModeUsesBorder(c);
  }
  key.clamp_x = uint32_t(clamp[0]);
  key.clamp_y = uint32_t(clamp[1]);
  key.clamp_z = uint32_t(clamp[2]);
  if (uses_border) {
    key.border_color = uint32_t(fetch.border_color);
    key.force_bc_w_to_max = fetch.force_bc_w_to_max;
  }

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                 ? fetch.aniso_filter
                                 : binding.aniso_filter;
  key.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  key.min_linear = min_filter == xenos::TextureFilter::kLinear;
  key.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  key.mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  uint32_t base_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page,
                                                 nullptr, &mip_min_level, &mip_max_level);
  if (key.mip_base_map && base_page != 0) mip_min_level = 0;
  key.mip_min_level = mip_min_level;

  // Max anisotropy supported by the device (xenos::AnisoFilter is log2 + 1).
  uint32_t max_aniso = uint32_t(xenos::AnisoFilter::kMax_16_1);
  while (max_aniso > uint32_t(xenos::AnisoFilter::kMax_1_1) &&
         float(1u << (max_aniso - 1)) > impl.properties.limits.maxSamplerAnisotropy) {
    --max_aniso;
  }
  if (aniso != xenos::AnisoFilter::kDisabled && uint32_t(aniso) > max_aniso) {
    aniso = xenos::AnisoFilter(max_aniso);
  }

  // Point sampling if the host format can't be linearly filtered.
  if (key.mag_linear || key.min_linear || key.mip_linear ||
      aniso != xenos::AnisoFilter::kDisabled) {
    bool filterable = false;
    FetchInfo info = DescribeFetch(fetch);
    if (info.valid) {
      const HostFormat& hf = GetHostFormat(info.key.fmt());
      auto linear = [&](VkFormat f) {
        return f == VK_FORMAT_UNDEFINED ||
               (impl.Features(f) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
      };
      filterable = hf.supported() &&
                   (!texture_util::IsAnySignNotSigned(info.swizzled_signs) ||
                    linear(hf.format_unsigned)) &&
                   (!texture_util::IsAnySignSigned(info.swizzled_signs) ||
                    linear(hf.format_signed));
    }
    if (!filterable) {
      key.mag_linear = key.min_linear = key.mip_linear = 0;
      aniso = xenos::AnisoFilter::kDisabled;
    }
  }
  if (aniso != xenos::AnisoFilter::kDisabled) {
    key.mag_linear = key.min_linear = key.mip_linear = 1;
  }
  key.aniso = uint32_t(aniso);

  auto it = impl.samplers.find(key);
  if (it != impl.samplers.end()) return it->second;

  static constexpr VkSamplerAddressMode kAddressModes[] = {
      VK_SAMPLER_ADDRESS_MODE_REPEAT,
      VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
  };
  VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = key.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = key.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = key.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = kAddressModes[key.clamp_x];
  info.addressModeV = kAddressModes[key.clamp_y];
  info.addressModeW = kAddressModes[key.clamp_z];
  // LOD bias is applied by the translated shaders.
  if (key.aniso != uint32_t(xenos::AnisoFilter::kDisabled)) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = float(1u << (key.aniso - uint32_t(xenos::AnisoFilter::kMax_1_1)));
  }
  info.minLod = float(key.mip_min_level);
  info.maxLod = key.mip_base_map ? info.minLod + 0.25f : VK_LOD_CLAMP_NONE;
  switch (xenos::BorderColor(key.border_color)) {
    case xenos::BorderColor::k_ABGR_White:
      info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
      break;
    default:
      // YCbCr blacks would need VK_EXT_custom_border_color.
      info.borderColor = key.force_bc_w_to_max ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                               : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      break;
  }
  VkSampler sampler = VK_NULL_HANDLE;
  if (vkCreateSampler(impl.device, &info, nullptr, &sampler) != VK_SUCCESS) {
    std::fprintf(stderr, "textures: vkCreateSampler failed\n");
    sampler = VK_NULL_HANDLE;
  }
  impl.samplers[key] = sampler;
  return sampler;
}

void TextureCache::WriteSystemConstants(const RegisterFile& regs, uint32_t used_texture_mask,
                                        SpirvShaderTranslator::SystemConstants& constants) {
  Impl& impl = *impl_;
  uint32_t textures_resolved = 0;
  for (uint32_t i = 0; i < 32; ++i) {
    if (!(used_texture_mask & (1u << i))) continue;
    const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(i);
    const FetchInfo info = DescribeFetch(fetch);
    const uint8_t signs = info.valid ? info.swizzled_signs : kSwizzledSignsUnsigned;
    uint32_t& signs_word = constants.texture_swizzled_signs[i >> 2];
    const uint32_t signs_shift = 8 * (i & 3);
    signs_word = (signs_word & ~(0xFFu << signs_shift)) | (uint32_t(signs) << signs_shift);

    constants.texture_integer_scale_bits[i] =
        info.valid ? GetIntegerScaleBits(fetch.format, fetch.num_format, fetch.swizzle, signs) : 0;

    // Only read by the shaders when imageViewFormatSwizzle is unavailable (the
    // swizzle is applied through the view component mapping here), but kept
    // consistent anyway.
    const uint32_t host_swizzle =
        info.valid ? GuestToHostSwizzle(fetch.swizzle, GetHostFormat(info.key.fmt()).swizzle)
                   : uint32_t(xenos::XE_GPU_TEXTURE_SWIZZLE_0000);
    uint32_t& swizzle_word = constants.texture_swizzles[i >> 1];
    const uint32_t swizzle_shift = 12 * (i & 1);
    swizzle_word = (swizzle_word & ~(0xFFFu << swizzle_shift)) | (host_swizzle << swizzle_shift);

    if (info.valid && impl.FindCompatibleResolved(info)) textures_resolved |= 1u << i;
  }
  // No resolution scaling in the native renderer.
  constants.textures_resolution_scaled = 0;
  constants.textures_resolved = textures_resolved;
}

void TextureCache::SetSharedMemory(VkBuffer buffer, std::function<bool(uint32_t, uint32_t)> request_range,
                                   SharedMemory* shared_memory) {
  Impl& impl = *impl_;
  impl.UnwatchAll();
  impl.shared_memory = shared_memory;
  impl.request_range = std::move(request_range);
  if (buffer != impl.shared_buffer) impl.SetSource(buffer);
  // Reload everything through the new path on next use.
  for (auto& [key, texture] : impl.textures) texture.outdated.store(true);
}

void TextureCache::InvalidateRange(uint32_t start, uint32_t length) { impl_->InvalidateRange(start, length); }

void TextureCache::EndFrame() {
  impl_->FreeStaging();
  impl_->RecycleScratch();
  ++impl_->frame;
}

bool TextureCache::DebugDecode(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t& width,
                               uint32_t& height, std::vector<uint8_t>& rgba,
                               std::string* description, uint32_t level) const {
  const FetchInfo info = DescribeFetch(fetch);
  if (!info.valid) return false;
  const TextureKey& key = info.key;
  const HostFormat& hf = GetHostFormat(key.fmt());
  if (description) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "%s %ux%ux%u dim%u %s endian%u pitch%u mips%u%s base %08X mip %08X swz %03X",
                  FormatInfo::Get(key.fmt())->name, key.width, key.height, key.depth,
                  key.dimension, key.tiled ? "tiled" : "linear", key.endianness, key.pitch * 32,
                  key.mip_max_level, key.packed_mips ? " packed" : "", key.base_page << 12,
                  key.mip_page << 12, uint32_t(fetch.swizzle));
    *description = buffer;
  }
  if (!hf.supported() || level > key.mip_max_level || (level == 0 && !key.base_page)) return false;
  HostData data;
  if (!BuildHostData(impl_->memory, key, hf, data, level) || data.regions.empty()) return false;
  const VkBufferImageCopy& region = data.regions.back();
  width = region.imageExtent.width;
  height = region.imageExtent.height;
  rgba.assign(size_t(width) * height * 4, 0);
  const uint8_t* base = data.bytes.data() + region.bufferOffset;
  const VkFormat format = hf.format_unsigned != VK_FORMAT_UNDEFINED ? hf.format_unsigned
                                                                    : hf.format_signed;
  if (hf.host_compressed) {
    const uint32_t wb = region.bufferRowLength / 4, hb = region.bufferImageHeight / 4;
    for (uint32_t by = 0; by < hb; ++by) {
      for (uint32_t bx = 0; bx < wb; ++bx) {
        const uint8_t* b = base + (size_t(by) * wb + bx) * hf.host_bytes;
        uint8_t texels[16][4];
        switch (format) {
          case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            DecodeBC1Color(b, texels, true);
            break;
          case VK_FORMAT_BC2_UNORM_BLOCK:
            DecodeBC1Color(b + 8, texels, false);
            for (int t = 0; t < 16; ++t) {
              uint32_t a = (b[t / 2] >> ((t & 1) * 4)) & 0xF;
              texels[t][3] = uint8_t(a * 17);
            }
            break;
          case VK_FORMAT_BC3_UNORM_BLOCK: {
            DecodeBC1Color(b + 8, texels, false);
            uint8_t a[16];
            DecodeBC4(b, a);
            for (int t = 0; t < 16; ++t) texels[t][3] = a[t];
            break;
          }
          case VK_FORMAT_BC4_UNORM_BLOCK: {
            uint8_t r[16];
            DecodeBC4(b, r);
            for (int t = 0; t < 16; ++t) {
              texels[t][0] = r[t];
              texels[t][1] = texels[t][2] = 0;
              texels[t][3] = 255;
            }
            break;
          }
          case VK_FORMAT_BC5_UNORM_BLOCK: {
            uint8_t r[16], g[16];
            DecodeBC4(b, r);
            DecodeBC4(b + 8, g);
            for (int t = 0; t < 16; ++t) {
              texels[t][0] = r[t];
              texels[t][1] = g[t];
              texels[t][2] = 0;
              texels[t][3] = 255;
            }
            break;
          }
          default:
            std::memset(texels, 0, sizeof(texels));
            break;
        }
        for (uint32_t t = 0; t < 16; ++t) {
          uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
          if (x < width && y < height) std::memcpy(&rgba[(size_t(y) * width + x) * 4], texels[t], 4);
        }
      }
    }
  } else {
    const uint32_t texel_bytes = HostTexelBytes(format);
    const size_t row_bytes = size_t(region.bufferRowLength) * texel_bytes;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        HostTexelToRGBA(format, base + y * row_bytes + size_t(x) * texel_bytes,
                        &rgba[(size_t(y) * width + x) * 4]);
      }
    }
  }
  // Apply the host format swizzle (R -> RRRR etc.) so single-channel formats
  // are viewable as grayscale.
  if (hf.swizzle != kRGBA) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
      uint8_t src[4];
      std::memcpy(src, &rgba[i], 4);
      for (uint32_t c = 0; c < 4; ++c) rgba[i + c] = src[(hf.swizzle >> (3 * c)) & 3];
    }
  }
  return true;
}

uint32_t TextureCache::loaded_count() const { return impl_->loaded_count; }
uint32_t TextureCache::resolved_hits() const { return impl_->resolved_hits; }
uint32_t TextureCache::unsupported_count() const { return uint32_t(impl_->unsupported.size()); }

}  // namespace xgpu
