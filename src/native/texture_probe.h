#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/system/xmemory.h>

namespace crackdown::native::textures {

// Phase 2's go or no-go: can a native renderer learn cheaply when a texture changes? Most textures sit raw in guest
// memory with no creation call. With crackdown_texture_probe on, this tracks every texture the game binds and three
// signals that one changed: the game's cache flush, CPU locks and resolves. A slow rotating hash of the real memory is
// the truth to compare against, and a change no signal announced counts as a miss. The emulator's page write watch is
// scored separately, armed just before each hash, so a change it didn't report is a real gap in it. It runs through
// normal play too, writing texture-probe-<time>.json to the logs folder every 30 seconds.
bool Enabled();

void Install(rex::memory::Memory* memory, uint8_t* membase, const std::filesystem::path& output_dir);
void OnGuestFrame();
void OnDraw(uint32_t device);
void OnFetchSet(uint32_t set, const uint32_t* fetch_dwords);
void OnCacheFlush(uint32_t start, uint32_t end);
void OnLock(uint32_t texture);
void OnResolve(uint32_t texture);

}  // namespace crackdown::native::textures
