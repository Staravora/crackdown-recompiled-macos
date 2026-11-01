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
// the thread falls behind, a frame is dropped and counted. Draws use the game's real vertex shaders and geometry with a
// plain grey pixel shader, since textures aren't there yet. Baked and direct draws go in the order the game made them.
// Every pass goes to its own target, and the colour target with the most draws is shown. Writes
// native-shadow-<time>.json and .png to the logs folder, and crackdown_native_shadow_window shows the picture live.
enum class DirectDraw { kCaptured, kRingWrapped, kNoDrawPacket };

#if defined(__APPLE__)
bool Enabled();
void Install(rex::memory::Memory* memory, const std::filesystem::path& shader_dir, const std::filesystem::path& output_dir);
void OnGuestFrame();
void OnClear();
// vertex_code holds a direct draw's vertex shader, whose copy in guest memory may not last until the frame is drawn.
void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* vertex_constants,
             uint32_t color_target, uint32_t depth_target, std::shared_ptr<const std::vector<uint8_t>> vertex_code = nullptr);
void CountDirectDraw(DirectDraw outcome);
#else
inline bool Enabled() {
  return false;
}
inline void Install(rex::memory::Memory*, const std::filesystem::path&, const std::filesystem::path&) {}
inline void OnGuestFrame() {}
inline void OnClear() {}
inline void Capture(std::shared_ptr<const std::vector<BakedList>>, uint32_t, const uint32_t*, uint32_t, uint32_t,
                    std::shared_ptr<const std::vector<uint8_t>> = nullptr) {}
inline void CountDirectDraw(DirectDraw) {}
#endif

}  // namespace crackdown::native::shadow
