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
// the thread falls behind, a frame is dropped and counted. Draws are placeholders until the real shaders and textures
// arrive, and direct draws aren't captured yet. Writes native-shadow-<time>.json to the logs folder every 30 seconds.
#if defined(__APPLE__)
bool Enabled();
void Install(rex::memory::Memory* memory, const std::filesystem::path& output_dir);
void OnGuestFrame();
void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* camera);
#else
inline bool Enabled() {
  return false;
}
inline void Install(rex::memory::Memory*, const std::filesystem::path&) {}
inline void OnGuestFrame() {}
inline void Capture(std::shared_ptr<const std::vector<BakedList>>, uint32_t, const uint32_t*) {}
#endif

}  // namespace crackdown::native::shadow
