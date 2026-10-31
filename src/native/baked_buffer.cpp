#include "baked_buffer.h"

#include <algorithm>

namespace crackdown::native {
namespace {

constexpr uint32_t kRegisterBase = 0x2000;
constexpr uint32_t kRegisterEnd = 0x5003;

constexpr uint32_t kSurfaceInfo = 0x2000;
constexpr uint32_t kColorInfo = 0x2001;
constexpr uint32_t kDepthInfo = 0x2002;
constexpr uint32_t kIndexOffset = 0x2102;
constexpr uint32_t kColorMask = 0x2104;
constexpr uint32_t kProgramControl = 0x2180;
constexpr uint32_t kInterpolatorControl = 0x2182;
constexpr uint32_t kDepthControl = 0x2200;
constexpr uint32_t kModeControl = 0x2208;
constexpr uint32_t kFloatConstants = 0x4000;
constexpr uint32_t kFetchConstants = 0x4800;
constexpr uint32_t kFetchDwords = 32 * 6;

constexpr uint32_t kOpDraw = 0x22;
constexpr uint32_t kOpShaderLoad = 0x27;
constexpr uint32_t kOpSetConstant = 0x2D;
constexpr uint32_t kOpLoadConstants = 0x2F;
constexpr uint32_t kOpWaitRegister = 0x3C;
constexpr uint32_t kOpBinMaskLow = 0x60;
constexpr uint32_t kOpBinMaskHigh = 0x61;

constexpr uint32_t kSetConstantRegisters = 4;

uint64_t Mix(uint64_t hash, uint32_t value) {
  return (hash ^ value) * 0x100000001B3ull;
}

uint64_t HashRange(uint64_t hash, const uint32_t* words, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    hash = Mix(hash, words[i]);
  }
  return hash;
}

uint32_t Intern(std::unordered_map<uint64_t, uint32_t>& table, uint64_t hash) {
  return table.try_emplace(hash, uint32_t(table.size())).first->second;
}

}  // namespace

BakedList BakedLibrary::Decode(const uint32_t* words, size_t count) {
  registers_.assign(kRegisterEnd - kRegisterBase, 0);
  const auto reg = [this](uint32_t address) -> uint32_t& { return registers_[address - kRegisterBase]; };

  BakedList list;
  // Lists without a mask packet run in every tile.
  uint64_t mask = ~0ull;
  BakedDraw state{};
  uint32_t first_constant_write = 0;

  size_t at = 0;
  while (at < count) {
    const uint32_t header = words[at];
    const uint32_t kind = header >> 30;
    const uint32_t body = ((header >> 16) & 0x3FFF) + 1;
    ++stats_.packets;

    if (kind == 2) {
      ++at;
      continue;
    }
    if (kind == 1) {
      ++stats_.unknown_packets;
      at += 3;
      continue;
    }
    if (at + 1 + body > count) {
      ++stats_.truncated_packets;
      break;
    }
    const uint32_t* data = words + at + 1;

    if (kind == 0) {
      const uint32_t base = header & 0x7FFF;
      const bool one_register = (header >> 15) & 1;
      if (base >= kFloatConstants && base < kFetchConstants) {
        const uint32_t dwords = one_register ? 1 : body;
        list.constant_writes.push_back({uint16_t(base - kFloatConstants), uint16_t(dwords),
                                        uint32_t(list.constant_data.size())});
        // A repeating write leaves only its last value.
        list.constant_data.insert(list.constant_data.end(), data + (one_register ? body - 1 : 0), data + body);
      }
      for (uint32_t i = 0; i < body; ++i) {
        const uint32_t address = base + (one_register ? 0 : i);
        if (address >= kRegisterBase && address < kRegisterEnd) {
          reg(address) = data[i];
        }
      }
      at += 1 + body;
      continue;
    }

    switch ((header >> 8) & 0x7F) {
      case kOpBinMaskLow:
        mask = (mask & 0xFFFFFFFF00000000ull) | data[0];
        break;
      case kOpBinMaskHigh:
        mask = (mask & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
        break;
      case kOpSetConstant:
        // These only move the tile's window offset, which collapses along with the tiles.
        if (((data[0] >> 16) & 0xFF) != kSetConstantRegisters) {
          ++stats_.unknown_packets;
        }
        break;
      case kOpLoadConstants:
        state.literals = data[0] & 0x3FFFFFFF;
        state.literal_register = uint16_t((data[1] & 0x7FF) / 4);
        state.literal_dwords = uint16_t(data[2] & 0xFFF);
        break;
      case kOpShaderLoad:
        ((data[0] & 3) == 0 ? state.vertex_shader : state.pixel_shader) = data[0] & ~3u;
        break;
      case kOpWaitRegister:
        // A cache flush handshake, which a native renderer doesn't have.
        break;
      case kOpDraw: {
        BakedDraw draw = state;
        draw.mask = mask;
        draw.initiator = data[1];
        draw.index_buffer = data[2];
        draw.index_info = data[3];
        draw.index_offset = reg(kIndexOffset);

        // Formats and sample counts only: the EDRAM bases and pitches aren't pipeline state.
        uint64_t pipeline = 0xCBF29CE484222325ull;
        pipeline = Mix(pipeline, draw.vertex_shader);
        pipeline = Mix(pipeline, draw.pixel_shader);
        pipeline = Mix(pipeline, reg(kSurfaceInfo) & 0x30000);
        pipeline = Mix(pipeline, reg(kColorInfo) & 0xF0000);
        pipeline = Mix(pipeline, reg(kDepthInfo) & 0x10000);
        pipeline = Mix(pipeline, reg(kColorMask));
        pipeline = HashRange(pipeline, &reg(kProgramControl), kInterpolatorControl - kProgramControl + 1);
        pipeline = HashRange(pipeline, &reg(kDepthControl), kModeControl - kDepthControl + 1);
        pipeline = Mix(pipeline, draw.initiator & 0x3F);
        draw.pipeline = Intern(pipelines_, pipeline);

        const size_t known_sets = texture_sets_.size();
        draw.textures =
            Intern(texture_sets_, HashRange(0xCBF29CE484222325ull, &reg(kFetchConstants), kFetchDwords));
        if (texture_sets_.size() != known_sets) {
          texture_set_data_.insert(texture_set_data_.end(), &reg(kFetchConstants),
                                   &reg(kFetchConstants) + kFetchDwords);
        }

        uint64_t dynamic = 0xCBF29CE484222325ull;
        dynamic = HashRange(dynamic, &reg(0x200E), 2);    // Screen scissor.
        dynamic = HashRange(dynamic, &reg(0x2081), 2);    // Window scissor.
        dynamic = HashRange(dynamic, &reg(0x2105), 4);    // Blend colour.
        dynamic = HashRange(dynamic, &reg(0x210D), 8);    // Stencil, alpha and viewport.
        draw.dynamic = Intern(dynamic_states_, dynamic);

        draw.first_constant_write = first_constant_write;
        draw.constant_writes = uint32_t(list.constant_writes.size()) - first_constant_write;
        first_constant_write = uint32_t(list.constant_writes.size());

        list.draws.push_back(draw);
        break;
      }
      default:
        ++stats_.unknown_packets;
        break;
    }
    at += 1 + body;
  }
  return list;
}

}  // namespace crackdown::native
