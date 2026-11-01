#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace crackdown::native {

// The game's precompiled command buffers hold raw GPU packets that no Direct3D hook sees: 41-64% of city draws. Each
// draw in one carries its complete state, and the census saw none change, so a list is decoded once and replayed from
// compact records after that. See docs/native-rendering-survey.md.
struct BakedDraw {
  uint32_t pipeline;  // Interned render state, with the shaders.
  uint32_t textures;  // Interned fetch constants.
  uint32_t dynamic;   // Interned viewport, scissor and references.
  uint32_t vertex_shader;
  uint32_t vertex_shader_dwords;
  uint32_t pixel_shader;
  uint32_t literals;  // Guest address of the shader's literal constants.
  uint16_t literal_register;
  uint16_t literal_dwords;
  uint32_t index_buffer;
  uint32_t index_info;
  uint32_t initiator;
  uint32_t index_offset;
  uint32_t booleans;  // Bool constants 0-15 of each stage, as XenosRecomp's shaders read them.
  uint32_t depth_control;
  uint32_t fetch_constants;  // Index of this draw's 32 fetch constants in BakedList::fetch_data.
  uint64_t mask;  // Low half: tiles. High half: which pass this draw belongs to.
  uint32_t first_constant_write;
  uint32_t constant_writes;
};

// Float constants a list writes itself, in dwords of the ALU constant file. Applied in order before the draw they
// belong to; constants a list doesn't write come from the device.
struct BakedConstantWrite {
  uint16_t first_dword;
  uint16_t dwords;
  uint32_t data;
};

struct BakedList {
  std::vector<BakedDraw> draws;
  std::vector<BakedConstantWrite> constant_writes;
  std::vector<uint32_t> constant_data;
  std::vector<uint32_t> fetch_data;
};

struct DecodeStats {
  uint64_t packets = 0;
  uint64_t unknown_packets = 0;
  uint64_t truncated_packets = 0;
};

class BakedLibrary {
 public:
  // Decodes one list of host-order packets. Keep the result: this is the slow path, and it interns as it goes.
  BakedList Decode(const uint32_t* words, size_t count);

  // The 32 fetch constants of a texture set, six dwords each.
  const uint32_t* texture_set(uint32_t id) const { return texture_set_data_.data() + size_t(id) * 192; }

  size_t pipeline_count() const { return pipelines_.size(); }
  size_t texture_set_count() const { return texture_sets_.size(); }
  size_t dynamic_state_count() const { return dynamic_states_.size(); }
  const DecodeStats& stats() const { return stats_; }

 private:
  std::unordered_map<uint64_t, uint32_t> pipelines_;
  std::unordered_map<uint64_t, uint32_t> texture_sets_;
  std::unordered_map<uint64_t, uint32_t> dynamic_states_;
  std::vector<uint32_t> texture_set_data_;
  std::vector<uint32_t> registers_;
  DecodeStats stats_;
};

// Tiles collapse in native rendering, so any tile bit passes. The high half has to match the pass the game selected.
inline bool DrawPasses(const BakedDraw& draw, uint32_t pass_select) {
  const uint32_t tiles = uint32_t(draw.mask);
  const uint32_t passes = uint32_t(draw.mask >> 32);
  return tiles != 0 || (passes & pass_select) != 0;
}

}  // namespace crackdown::native
