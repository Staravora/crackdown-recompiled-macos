#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace crackdown::native {

struct MetalFrameTiming {
  uint64_t wait_ns = 0;      // Waiting for a frame slot the GPU has finished with.
  uint64_t encode_ns = 0;    // Starting the pass, the encode callback, ending the pass and committing.
};

struct MetalGpuTotals {
  uint64_t frames = 0;
  uint64_t errors = 0;
  uint64_t gpu_ns = 0;
  uint64_t worst_gpu_ns = 0;
};

// The only part of the native layer that touches Metal, so the rest stays plain C++. Everything but gpu_totals() is for
// one thread.
class MetalDevice {
 public:
  static constexpr uint32_t kPipelines = 16;
  static constexpr int kFramesInFlight = 3;

  // Wraps guest memory for the GPU without copying it. Compiling the pipelines makes this slow, so call it off the
  // game's threads. Returns null if the Mac has no Metal device.
  static std::unique_ptr<MetalDevice> Create(uint8_t* guest_memory, size_t guest_bytes, size_t constant_bytes);

  virtual ~MetalDevice() = default;

  virtual bool guest_memory_wrapped() const = 0;

  // Records and submits one frame. encode gets that frame's constant memory and calls Draw.
  virtual MetalFrameTiming RecordFrame(const std::function<void(uint8_t* constants)>& encode) = 0;

  // Placeholder draw until the real shaders arrive: real state changes and constant bindings, no pixels.
  virtual void Draw(uint32_t pipeline, size_t vertex_constants, size_t fragment_constants) = 0;

  // Copies a range out through the GPU's view of guest memory and compares it with the CPU's. Waits for the GPU.
  virtual bool MatchesGuestMemory(uint32_t address, uint32_t bytes) = 0;

  virtual MetalGpuTotals gpu_totals() const = 0;
};

}  // namespace crackdown::native
