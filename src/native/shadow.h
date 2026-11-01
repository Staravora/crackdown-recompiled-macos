#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/system/xmemory.h>

#include "baked_buffer.h"

namespace crackdown::native::shadow {

// Phase 2's shadow mode. With crackdown_native_shadow on, a thread of its own records every frame's baked draws in
// Metal, off screen, while the emulator goes on presenting. The game's threads only hand runs over and never wait: if
// the thread falls behind, a frame is dropped and counted. Draws use the game's real shaders, geometry and textures, and
// a draw with no matching pixel shader comes out plain grey. Baked and direct draws go in the order the game made them.
// Every pass goes to its own target, and the last colour target drawn to is shown. Writes
// native-shadow-<time>.json and .png to the logs folder, and crackdown_native_shadow_window shows the picture live.
// The surfaces a draw goes to, and their size.
struct Targets {
  uint32_t color = 0;
  uint32_t depth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t surface_height = 0;  // What the surface holds at once: one tile, on a tiled one.
};

// A copy the game asks for, out of its current surfaces and into a texture object.
struct ResolveCopy {
  uint32_t texture = 0;
  uint32_t flags = 0;
  bool has_rect = false;
  std::array<int32_t, 4> rect{};  // Left, top, right and bottom. Without one, as much as the texture holds.
  int32_t dest_x = 0;
  int32_t dest_y = 0;
  uint32_t level = 0;
  std::array<float, 4> clear_color{};  // For a resolve that clears as well, as flags say.
  float clear_depth = 0.0f;
  uint32_t clear_stencil = 0;
};

// The vertex shaders a run of baked lists uses, by address and size, as they stood when it ran. Binding rewrites a shader
// where it sits to suit a new vertex format, so what's there a frame later may be for some other draw.
using VertexCode = std::unordered_map<uint64_t, std::vector<uint8_t>>;

#if defined(__APPLE__)
bool Enabled();
void Install(rex::memory::Memory* memory, const std::filesystem::path& shader_dir, const std::filesystem::path& output_dir);
void OnGuestFrame();
void OnClear();
// vertex_code holds a direct draw's vertex shader, whose copy in guest memory may not last until the frame is drawn.
void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* float_constants,
             const uint32_t* loop_constants, const Targets& targets,
             std::shared_ptr<const std::vector<uint8_t>> vertex_code = nullptr,
             std::shared_ptr<const VertexCode> list_code = nullptr);
void CountDirectDraw();
void OnResolve(const ResolveCopy& copy, const Targets& targets, const uint8_t* base);
void SetRingDesyncs(uint64_t desyncs);
void SetGammaRamp(const uint32_t* ramp);
#else
inline bool Enabled() {
  return false;
}
inline void Install(rex::memory::Memory*, const std::filesystem::path&, const std::filesystem::path&) {}
inline void OnGuestFrame() {}
inline void OnClear() {}
inline void Capture(std::shared_ptr<const std::vector<BakedList>>, uint32_t, const uint32_t*, const uint32_t*, const Targets&,
                    std::shared_ptr<const std::vector<uint8_t>> = nullptr, std::shared_ptr<const VertexCode> = nullptr) {}
inline void CountDirectDraw() {}
inline void OnResolve(const ResolveCopy&, const Targets&, const uint8_t*) {}
inline void SetRingDesyncs(uint64_t) {}
inline void SetGammaRamp(const uint32_t*) {}
#endif

}  // namespace crackdown::native::shadow
