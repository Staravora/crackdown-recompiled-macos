#include "texture_cache.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <vector>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace crackdown::native {
namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

using rex::graphics::FormatInfo;

constexpr size_t kFetchDwords = 6;
constexpr uint32_t kDummyDescriptor = 0;

std::optional<MetalTextureFormat> MetalFormatOf(xenos::TextureFormat format, bool gamma) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
      return gamma ? MetalTextureFormat::kRgba8Srgb : MetalTextureFormat::kRgba8;
    case xenos::TextureFormat::k_DXT1:
      return gamma ? MetalTextureFormat::kBc1Srgb : MetalTextureFormat::kBc1;
    case xenos::TextureFormat::k_DXT2_3:
      return gamma ? MetalTextureFormat::kBc2Srgb : MetalTextureFormat::kBc2;
    case xenos::TextureFormat::k_DXT4_5:
      return gamma ? MetalTextureFormat::kBc3Srgb : MetalTextureFormat::kBc3;
    case xenos::TextureFormat::k_DXN:
      return MetalTextureFormat::kBc5;
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      return MetalTextureFormat::kR8;
    case xenos::TextureFormat::k_8_8:
      return MetalTextureFormat::kRg8;
    case xenos::TextureFormat::k_5_6_5:
      return MetalTextureFormat::kB5G6R5;
    case xenos::TextureFormat::k_2_10_10_10:
      return MetalTextureFormat::kRgb10A2;
    case xenos::TextureFormat::k_16:
      return MetalTextureFormat::kR16;
    case xenos::TextureFormat::k_16_16:
      return MetalTextureFormat::kRg16;
    case xenos::TextureFormat::k_16_16_16_16:
      return MetalTextureFormat::kRgba16;
    case xenos::TextureFormat::k_16_FLOAT:
      return MetalTextureFormat::kR16Float;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return MetalTextureFormat::kRg16Float;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return MetalTextureFormat::kRgba16Float;
    case xenos::TextureFormat::k_32_FLOAT:
      return MetalTextureFormat::kR32Float;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return MetalTextureFormat::kRg32Float;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return MetalTextureFormat::kRgba32Float;
    default:
      return std::nullopt;
  }
}

void SwapBlock(uint8_t* block, uint32_t bytes, xenos::Endian endian) {
  if (endian == xenos::Endian::k8in16) {
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
      std::swap(block[i], block[i + 1]);
    }
  } else if (endian == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(block[i], block[i + 3]);
      std::swap(block[i + 1], block[i + 2]);
    }
  } else if (endian == xenos::Endian::k16in32) {
    for (uint32_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(block[i], block[i + 2]);
      std::swap(block[i + 1], block[i + 3]);
    }
  }
}

uint32_t BlocksOf(uint32_t texels, uint32_t level, uint32_t block_size) {
  return (std::max(texels >> level, 1u) + block_size - 1) / block_size;
}

uint64_t HashFetch(const uint32_t* fetch) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < kFetchDwords; ++i) {
    hash = (hash ^ fetch[i]) * 0x100000001B3ull;
  }
  return hash;
}

MetalAddressMode AddressModeOf(xenos::ClampMode clamp) {
  switch (clamp) {
    case xenos::ClampMode::kRepeat:
      return MetalAddressMode::kRepeat;
    case xenos::ClampMode::kMirroredRepeat:
      return MetalAddressMode::kMirrorRepeat;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return MetalAddressMode::kMirrorClamp;
    default:
      return MetalAddressMode::kClamp;
  }
}

}  // namespace

TextureCache::TextureCache(MetalDevice& device, const uint8_t* guest_memory, size_t guest_bytes)
    : device_(device), guest_memory_(guest_memory), guest_bytes_(guest_bytes) {}

int TextureCache::FindTexture(const uint32_t* fetch) {
  const uint64_t key = HashFetch(fetch);
  const auto found = textures_.find(key);
  if (found != textures_.end()) {
    return found->second;
  }
  const size_t budget_before = upload_budget_;
  const int descriptor = Load(fetch);
  // Out of budget isn't an answer, so it gets asked again next frame.
  if (budget_before != 0 && (descriptor != int(kDummyDescriptor) || upload_budget_ != 0)) {
    textures_[key] = descriptor;
  }
  return descriptor;
}

int TextureCache::Load(const uint32_t* fetch_dwords) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != xenos::DataDimension::k2DOrStacked ||
      fetch.stacked) {
    ++textures_skipped_;
    return kDummyDescriptor;
  }
  const xenos::TextureFormat format = xenos::TextureFormat(uint32_t(fetch.format));
  const std::optional<MetalTextureFormat> metal_format =
      MetalFormatOf(format, fetch.sign_x == xenos::TextureSign::kGamma);
  const FormatInfo* info = FormatInfo::Get(format);
  if (!metal_format || !info) {
    ++textures_skipped_;
    return kDummyDescriptor;
  }

  const uint32_t width = uint32_t(fetch.size_2d.width) + 1, height = uint32_t(fetch.size_2d.height) + 1;
  const uint32_t block_width = info->block_width, block_height = info->block_height;
  const uint32_t block_bytes = info->bits_per_pixel * block_width * block_height / 8;
  if (!block_bytes) {
    ++textures_skipped_;
    return kDummyDescriptor;
  }
  const uint32_t mip_address = uint32_t(fetch.mip_address) << 12;
  const uint32_t full_levels = std::bit_width(std::max(width, height));
  const uint32_t levels = mip_address ? std::min(uint32_t(fetch.mip_max_level) + 1, full_levels) : 1;

  size_t needed = 0;
  for (uint32_t level = 0; level < levels; ++level) {
    needed += size_t(BlocksOf(width, level, block_width)) * BlocksOf(height, level, block_height) * block_bytes;
  }
  if (needed > upload_budget_) {
    upload_budget_ = 0;
    return kDummyDescriptor;
  }

  MetalTextureDesc desc;
  desc.format = *metal_format;
  desc.width = width;
  desc.height = height;
  desc.levels = levels;
  for (uint32_t i = 0; i < 4; ++i) {
    desc.swizzle[i] = uint8_t((fetch.swizzle >> (i * 3)) & 7);
  }
  const int descriptor = device_.AddTexture(desc);
  if (descriptor < 0) {
    ++textures_skipped_;
    return kDummyDescriptor;
  }

  const texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1, fetch.tiled,
                                          format, fetch.packed_mips, true, levels - 1);
  const uint32_t block_bytes_log2 = std::bit_width(block_bytes) - 1;
  const size_t base_address = (size_t(uint32_t(fetch.base_address)) << 12) & (guest_bytes_ - 1);
  const size_t mips_address = size_t(mip_address) & (guest_bytes_ - 1);
  std::vector<uint8_t> output;
  for (uint32_t level = 0; level < levels; ++level) {
    // A level in the packed tail shares the tail's storage, at an offset within it.
    const uint32_t stored = std::min(level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& guest = stored ? layout.mips[stored] : layout.base;
    const size_t address = stored ? mips_address + layout.mip_offsets_bytes[stored] : base_address;
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (fetch.packed_mips) {
      texture_util::GetPackedMipOffset(width, height, 1, format, level, offset_x, offset_y, offset_z);
    }
    const uint32_t blocks_x = BlocksOf(width, level, block_width), blocks_y = BlocksOf(height, level, block_height);
    const uint32_t pitch_blocks = guest.row_pitch_bytes / block_bytes;
    output.resize(size_t(blocks_x) * blocks_y * block_bytes);
    for (uint32_t y = 0; y < blocks_y; ++y) {
      for (uint32_t x = 0; x < blocks_x; ++x) {
        const size_t offset = fetch.tiled ? size_t(texture_util::GetTiledOffset2D(int32_t(x + offset_x), int32_t(y + offset_y),
                                                                                  pitch_blocks, block_bytes_log2))
                                          : size_t(y + offset_y) * guest.row_pitch_bytes + size_t(x + offset_x) * block_bytes;
        uint8_t* target = output.data() + (size_t(y) * blocks_x + x) * block_bytes;
        if (address + offset + block_bytes <= guest_bytes_) {
          std::memcpy(target, guest_memory_ + address + offset, block_bytes);
          SwapBlock(target, block_bytes, xenos::Endian(uint32_t(fetch.endianness)));
        }
      }
    }
    device_.FillTexture(descriptor, level, std::max(width >> level, 1u), std::max(height >> level, 1u), output.data(),
                        size_t(blocks_x) * block_bytes);
  }
  upload_budget_ -= needed;
  ++textures_loaded_;
  return descriptor;
}

int TextureCache::FindSampler(const uint32_t* fetch_dwords) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  MetalSamplerDesc desc;
  desc.address_u = AddressModeOf(fetch.clamp_x);
  desc.address_v = AddressModeOf(fetch.clamp_y);
  desc.nearest = fetch.mag_filter == xenos::TextureFilter::kPoint && fetch.min_filter == xenos::TextureFilter::kPoint;
  const uint32_t key = uint32_t(desc.address_u) | uint32_t(desc.address_v) << 4 | uint32_t(desc.nearest) << 8;
  const auto found = samplers_.find(key);
  if (found != samplers_.end()) {
    return found->second;
  }
  const int descriptor = device_.AddSampler(desc);
  samplers_[key] = descriptor;
  return descriptor;
}

}  // namespace crackdown::native
