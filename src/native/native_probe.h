#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace crackdown::native::probe {

// With crackdown_native_probe on, a benchmark's recording window decodes and replays the game's baked command buffers
// from live guest memory and times it, drawing nothing. The guest code still runs unchanged, so this measures what the
// native layer would cost on the thread that runs them.
bool Enabled();

void Begin();
void OnGuestFrame();
void OnRunCommandBuffer(uint32_t device, uint32_t buffer, uint32_t predication, const uint8_t* base);
void Write(const std::filesystem::path& path, const std::string& label);

}  // namespace crackdown::native::probe
