#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/system/xmemory.h>

#include "baked_buffer.h"

namespace crackdown::native::shadow {

// Phase 2's shadow mode. With crackdown_native_shadow on, a thread of its own records every frame's baked draws in
// Metal, off screen, while the emulator goes on presenting. The game's threads only hand runs over and never wait: if
// the thread falls behind, a frame is dropped and counted. Draws use the game's real shaders, geometry and textures, and
// a draw with no matching pixel shader comes out plain grey. Baked and direct draws go in the order the game made them.
// Every pass goes to its own target, and the colour target with the most draws is shown. Writes
// native-shadow-<time>.json and .png to the logs folder, and crackdown_native_shadow_window shows the picture live.
// The surfaces a draw goes to, and their size.
struct Targets {
  uint32_t color = 0;
  uint32_t depth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

#if defined(__APPLE__)
bool Enabled();
void Install(rex::memory::Memory* memory, const std::filesystem::path& shader_dir, const std::filesystem::path& output_dir);
void OnGuestFrame();
void OnClear();
// vertex_code holds a direct draw's vertex shader, whose copy in guest memory may not last until the frame is drawn.
void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* vertex_constants,
             const Targets& targets, std::shared_ptr<const std::vector<uint8_t>> vertex_code = nullptr);
void CountDirectDraw();
void SetRingDesyncs(uint64_t desyncs);
#else
inline bool Enabled() {
  return false;
}
inline void Install(rex::memory::Memory*, const std::filesystem::path&, const std::filesystem::path&) {}
inline void OnGuestFrame() {}
inline void OnClear() {}
inline void Capture(std::shared_ptr<const std::vector<BakedList>>, uint32_t, const uint32_t*, const Targets&,
                    std::shared_ptr<const std::vector<uint8_t>> = nullptr) {}
inline void CountDirectDraw() {}
inline void SetRingDesyncs(uint64_t) {}
#endif

}  // namespace crackdown::native::shadow
