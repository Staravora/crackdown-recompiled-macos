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

// D3D's ring holds what the GPU will do, in order, as only what changed. Shadow mode reads it as a stream, which picks up
// every draw once. D3D breaks it into segments around the data it keeps in the same memory, so OnRingSegmentEnding has to
// run before one closes.
void AfterDraw(uint32_t device, const uint8_t* base);
void OnRingSegmentEnding(uint32_t device, const uint8_t* base);
void OnRunCommandBuffer(uint32_t device, uint32_t buffer, uint32_t predication, const uint8_t* base);
void Write(const std::filesystem::path& path, const std::string& label);

}  // namespace crackdown::native::probe
