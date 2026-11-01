#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crackdown::native {

// The game's precompiled command buffers hold raw GPU packets that no Direct3D hook sees: 41-64% of city draws. Each
// draw in one carries its complete state, and the census saw none change, so a list is decoded once and replayed from
// compact records after that. See docs/native-rendering-survey.md.
// A shader's literal constants, loaded from beside its code. One shader can have more than one block of them.
struct BakedLiteral {
  uint32_t address;
  uint16_t first_dword;  // In the ALU constant file: vertex constants, then pixel.
  uint16_t dwords;
};

inline constexpr uint32_t kMaxLiteralsPerDraw = 6;

struct BakedDraw {
  uint32_t pipeline;  // Interned render state, with the shaders.
  uint32_t textures;  // Interned fetch constants.
  uint32_t dynamic;   // Interned viewport, scissor and references.
  uint32_t vertex_shader;
  uint32_t vertex_shader_dwords;
  uint32_t pixel_shader;
  uint32_t pixel_shader_dwords;
  BakedLiteral literals[kMaxLiteralsPerDraw];
  uint32_t literal_count;
  uint32_t index_buffer;
  uint32_t index_info;
  uint32_t initiator;
  uint32_t index_offset;
  uint32_t booleans;  // Bool constants 0-15 of each stage, as XenosRecomp's shaders read them.
  uint32_t depth_control;
  uint32_t blend_control;
  uint32_t color_control;
  uint32_t color_mask;
  uint32_t alpha_reference;
  uint32_t stencil_front;  // Reference, read mask and write mask.
  uint32_t stencil_back;
  uint32_t mode_control;  // Culling and winding.
  uint32_t surface_info;
  uint32_t color_info;
  uint32_t depth_info;
  uint32_t viewport[6];  // Scale and offset: x, then y, then z.
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
  uint64_t dropped_literals = 0;
  uint64_t ring_desyncs = 0;
};

class BakedLibrary {
 public:
  static constexpr uint32_t kRegisterBase = 0x2000;
  static constexpr uint32_t kFloatConstantRegister = 0x4000;
  static constexpr uint32_t kLoopConstantRegister = 0x4908;

  // Decodes one list of host-order packets. Keep the result: this is the slow path, and it interns as it goes.
  BakedList Decode(const uint32_t* words, size_t count);

  // Reads the next stretch of D3D's ring, which holds only what changed, so state carries over from the call before.
  // Everything the GPU would do to its registers is done here, including loads from `physical_memory`, which is guest
  // physical memory. Calls on_draw for each draw, with its 32 fetch constants, and returns how many words it used: a packet
  // that doesn't fit is left for next time.
  using RingDraw = std::function<void(const BakedDraw& draw, const uint32_t* fetch_constants)>;
  size_t DecodeRing(const uint32_t* words, size_t count, const uint8_t* physical_memory, const RingDraw& on_draw);

  // Whether a vertex shader has loaded since this was last asked.
  bool TakeVertexShaderLoaded() { return std::exchange(vertex_shader_loaded_, false); }

  // The vertex shader's code, in host order, if it came inside the ring and not from an address.
  const std::vector<uint32_t>& inline_vertex_shader() const { return inline_vertex_shader_; }

  // The float constants DecodeRing has seen, vertex then pixel, as the GPU would hold them.
  const uint32_t* float_constants() const { return registers_.data() + (kFloatConstantRegister - kRegisterBase); }

  // The 32 loop constants, the same way.
  const uint32_t* loop_constants() const { return registers_.data() + (kLoopConstantRegister - kRegisterBase); }

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
  size_t DecodeWords(const uint32_t* words, size_t count, BakedList& list, const uint8_t* physical_memory,
                     const RingDraw* on_draw);
  void AddLiteral(const uint32_t* data);
  void SetConstants(const uint32_t* data, uint32_t body);
  void LoadConstants(const uint32_t* data, const uint8_t* physical_memory);

  std::vector<uint32_t> registers_;
  BakedDraw state_{};
  bool literals_open_[2] = {};
  bool vertex_shader_loaded_ = false;
  std::vector<uint32_t> inline_vertex_shader_;
  uint64_t mask_ = ~0ull;
  DecodeStats stats_;
};

// Tiles collapse in native rendering, so any tile bit passes. The high half has to match the pass the game selected.
inline bool DrawPasses(const BakedDraw& draw, uint32_t pass_select) {
  const uint32_t tiles = uint32_t(draw.mask);
  const uint32_t passes = uint32_t(draw.mask >> 32);
  return tiles != 0 || (passes & pass_select) != 0;
}

}  // namespace crackdown::native
