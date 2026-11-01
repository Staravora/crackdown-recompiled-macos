#include "baked_buffer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>

namespace crackdown::native {
namespace {

constexpr uint32_t kRegisterEnd = 0x5003;

constexpr uint32_t kSurfaceInfo = 0x2000;
constexpr uint32_t kColorInfo = 0x2001;
constexpr uint32_t kDepthInfo = 0x2002;
constexpr uint32_t kIndexOffset = 0x2102;
constexpr uint32_t kColorMask = 0x2104;
constexpr uint32_t kAlphaReference = 0x210E;
constexpr uint32_t kStencilBack = 0x210C;
constexpr uint32_t kStencilFront = 0x210D;
constexpr uint32_t kMode = 0x2205;
constexpr uint32_t kViewportControl = 0x2206;
constexpr uint32_t kGammaIndex = 0x1922;
constexpr uint32_t kGammaColor = 0x1925;
constexpr uint32_t kPolygonOffset = 0x2380;
constexpr uint32_t kProgramControl = 0x2180;
constexpr uint32_t kInterpolatorControl = 0x2182;
constexpr uint32_t kViewport = 0x210F;
constexpr uint32_t kDepthControl = 0x2200;
constexpr uint32_t kBlendControl = 0x2201;
constexpr uint32_t kColorControl = 0x2202;
constexpr uint32_t kModeControl = 0x2208;
constexpr uint32_t kFloatConstants = BakedLibrary::kFloatConstantRegister;
constexpr uint32_t kVertexConstantDwords = 1024;
constexpr uint32_t kFetchConstants = 0x4800;
constexpr uint32_t kFetchDwords = 32 * 6;
constexpr uint32_t kBoolConstants = 0x4900;
constexpr uint32_t kPixelBoolConstants = 0x4904;
constexpr uint32_t kLoopConstants = BakedLibrary::kLoopConstantRegister;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;

constexpr uint32_t kOpDraw = 0x22;
constexpr uint32_t kOpShaderLoad = 0x27;
constexpr uint32_t kOpShaderLoadInline = 0x2B;
constexpr uint32_t kOpSetConstant = 0x2D;
constexpr uint32_t kOpLoadConstants = 0x2F;
constexpr uint32_t kOpWaitRegister = 0x3C;
constexpr uint32_t kOpBinMaskLow = 0x60;
constexpr uint32_t kOpBinMaskHigh = 0x61;

constexpr uint32_t kSetConstantRegisters = 4;
constexpr uint32_t kMaxShaderDwords = 4096 * 3;

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

// Where each kind of constant lives in the register file.
uint32_t ConstantBase(uint32_t type) {
  constexpr uint32_t kBases[] = {kFloatConstants, kFetchConstants, kBoolConstants, kLoopConstants,
                                 BakedLibrary::kRegisterBase};
  return type < std::size(kBases) ? kBases[type] : 0;
}

}  // namespace

// Literals come just before their shader, so a stage's load opens a new set, and loading the shader closes it.
void BakedLibrary::AddLiteral(const uint32_t* data) {
  const uint32_t first_dword = data[1] & 0x7FF;
  const bool pixel = first_dword >= kVertexConstantDwords;
  if (!literals_open_[pixel]) {
    literals_open_[pixel] = true;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < state_.literal_count; ++i) {
      if ((state_.literals[i].first_dword >= kVertexConstantDwords) != pixel) {
        state_.literals[kept++] = state_.literals[i];
      }
    }
    state_.literal_count = kept;
  }
  if (state_.literal_count < kMaxLiteralsPerDraw) {
    state_.literals[state_.literal_count++] = {data[0] & 0x3FFFFFFF, uint16_t(first_dword), uint16_t(data[2] & 0xFFF)};
  } else {
    ++stats_.dropped_literals;
  }
}

BakedList BakedLibrary::Decode(const uint32_t* words, size_t count) {
  registers_.assign(kRegisterEnd - kRegisterBase, 0);
  state_ = {};
  literals_open_[0] = literals_open_[1] = false;
  mask_ = ~0ull;
  BakedList list;
  DecodeWords(words, count, list, nullptr, nullptr);
  return list;
}

size_t BakedLibrary::DecodeRing(const uint32_t* words, size_t count, const uint8_t* physical_memory,
                                const RingDraw& on_draw) {
  if (registers_.empty()) {
    registers_.assign(kRegisterEnd - kRegisterBase, 0);
  }
  BakedList unused;
  return DecodeWords(words, count, unused, physical_memory, &on_draw);
}

void BakedLibrary::SetConstants(const uint32_t* data, uint32_t body) {
  const uint32_t base = ConstantBase((data[0] >> 16) & 0xFF);
  if (!base) {
    ++stats_.unknown_packets;
    return;
  }
  const uint32_t first = base + (data[0] & 0x7FF);
  for (uint32_t i = 0; i + 1 < body && first + i < kRegisterEnd; ++i) {
    registers_[first + i - kRegisterBase] = data[1 + i];
  }
}

void BakedLibrary::LoadConstants(const uint32_t* data, const uint8_t* physical_memory) {
  const uint32_t base = ConstantBase((data[1] >> 16) & 0xFF);
  const uint32_t address = data[0] & kPhysicalMask;
  const uint32_t dwords = data[2] & 0xFFF;
  if (!base || uint64_t(address) + dwords * 4ull > uint64_t(kPhysicalMask) + 1) {
    ++stats_.unknown_packets;
    return;
  }
  const uint32_t first = base + (data[1] & 0x7FF);
  for (uint32_t i = 0; i < dwords && first + i < kRegisterEnd; ++i) {
    uint32_t value;
    std::memcpy(&value, physical_memory + address + size_t(i) * 4, sizeof(value));
    registers_[first + i - kRegisterBase] = std::byteswap(value);
  }
}

size_t BakedLibrary::DecodeWords(const uint32_t* words, size_t count, BakedList& list, const uint8_t* physical_memory,
                                 const RingDraw* on_draw) {
  // A list is self-contained. The ring is everything the GPU does, in order, so registers are all there is to go on.
  const bool from_ring = on_draw != nullptr;
  const auto reg = [this](uint32_t address) -> uint32_t& { return registers_[address - kRegisterBase]; };

  uint32_t first_constant_write = 0;
  uint32_t last_texture_set = ~0u;

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
      // D3D never writes this packet type, so the ring has been misread. Reading on would turn data into register writes.
      if (from_ring) {
        ++stats_.ring_desyncs;
        return count;
      }
      at += 3;
      continue;
    }
    if (at + 1 + body > count) {
      if (from_ring) {
        return at;
      }
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
        } else if (address == kGammaIndex) {
          gamma_index_ = data[i] % kGammaEntries;
        } else if (address == kGammaColor) {
          gamma_ramp_[gamma_index_] = data[i];
          gamma_index_ = (gamma_index_ + 1) % kGammaEntries;
          ++gamma_writes_;
        }
      }
      at += 1 + body;
      continue;
    }

    switch ((header >> 8) & 0x7F) {
      case kOpBinMaskLow:
        mask_ = (mask_ & 0xFFFFFFFF00000000ull) | data[0];
        break;
      case kOpBinMaskHigh:
        mask_ = (mask_ & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
        break;
      case kOpSetConstant:
        // In lists these only move the tile's window offset, which collapses along with the tiles.
        if (from_ring) {
          SetConstants(data, body);
        } else if (((data[0] >> 16) & 0xFF) != kSetConstantRegisters) {
          ++stats_.unknown_packets;
        }
        break;
      case kOpLoadConstants:
        if (from_ring) {
          LoadConstants(data, physical_memory);
        } else {
          AddLiteral(data);
        }
        break;
      case kOpShaderLoad:
        // More than the hardware's 4096 instructions means these words aren't really a packet.
        if ((data[1] & 0xFFFF) > kMaxShaderDwords) {
          ++stats_.unknown_packets;
          break;
        }
        if ((data[0] & 3) == 0) {
          state_.vertex_shader = data[0] & ~3u;
          state_.vertex_shader_dwords = data[1] & 0xFFFF;
          literals_open_[0] = false;
          vertex_shader_loaded_ = true;
          inline_vertex_shader_.clear();
        } else {
          state_.pixel_shader = data[0] & ~3u;
          state_.pixel_shader_dwords = data[1] & 0xFFFF;
          literals_open_[1] = false;
        }
        break;
      case kOpShaderLoadInline:
        // The code follows in the packet. A list can keep it, but nothing here reads that yet, so those draws count as
        // having no shader.
        if ((data[0] & 3) == 0) {
          const uint32_t dwords = data[1] & 0xFFFF;
          const bool usable = from_ring && dwords + 2 <= body;
          state_.vertex_shader = 0;
          state_.vertex_shader_dwords = usable ? dwords : 0;
          literals_open_[0] = false;
          vertex_shader_loaded_ = true;
          inline_vertex_shader_.clear();
          if (usable) {
            inline_vertex_shader_.assign(data + 2, data + 2 + dwords);
          }
        } else {
          state_.pixel_shader = 0;
          state_.pixel_shader_dwords = 0;
          literals_open_[1] = false;
        }
        break;
      case kOpWaitRegister:
        // A cache flush handshake, which a native renderer doesn't have.
        break;
      case kOpDraw: {
        BakedDraw draw = state_;
        draw.mask = mask_;
        draw.initiator = data[1];
        // A draw with no index buffer ends at the initiator.
        draw.index_buffer = body > 2 ? data[2] : 0;
        draw.index_info = body > 3 ? data[3] : 0;
        draw.index_offset = reg(kIndexOffset);
        draw.booleans = (reg(kBoolConstants) & 0xFFFF) | (reg(kPixelBoolConstants) << 16);
        draw.depth_control = reg(kDepthControl);
        draw.blend_control = reg(kBlendControl);
        draw.color_control = reg(kColorControl);
        draw.color_mask = reg(kColorMask);
        draw.alpha_reference = reg(kAlphaReference);
        draw.stencil_front = reg(kStencilFront);
        draw.stencil_back = reg(kStencilBack);
        draw.mode_control = reg(kMode);
        draw.viewport_control = reg(kViewportControl);
        std::copy_n(&reg(kPolygonOffset), 4, draw.polygon_offset);
        draw.surface_info = reg(kSurfaceInfo);
        draw.color_info = reg(kColorInfo);
        draw.depth_info = reg(kDepthInfo);
        std::copy_n(&reg(kViewport), 6, draw.viewport);

        if (from_ring) {
          (*on_draw)(draw, &reg(kFetchConstants));
          break;
        }

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
        if (draw.textures != last_texture_set) {
          last_texture_set = draw.textures;
          list.fetch_data.insert(list.fetch_data.end(), &reg(kFetchConstants), &reg(kFetchConstants) + kFetchDwords);
        }
        draw.fetch_constants = uint32_t(list.fetch_data.size() / kFetchDwords) - 1;

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
  return at;
}

}  // namespace crackdown::native
