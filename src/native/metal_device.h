#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace crackdown::native {

struct MetalFrameTiming {
  uint64_t wait_ns = 0;    // Waiting for a frame slot the GPU has finished with.
  uint64_t encode_ns = 0;  // Starting the pass, the encode callback, ending the pass and committing.
};

struct MetalGpuTotals {
  uint64_t frames = 0;
  uint64_t errors = 0;
  uint64_t gpu_ns = 0;
  uint64_t worst_gpu_ns = 0;
};

// One vertex fetch of a shader, as pullElement reads it. See tools/shader-trial/pull_vertices.py.
struct MetalPullElement {
  uint32_t packed;
  int32_t offset;
  float scale;
};

enum class MetalPrimitive { kTriangleList, kTriangleStrip };

struct MetalDraw {
  int vertex_shader = 0;
  uint32_t depth_control = 0;
  MetalPrimitive primitive = MetalPrimitive::kTriangleList;
  size_t vertex_constants = 0;  // Into the frame's memory, and so are the rest.
  size_t shared_constants = 0;
  size_t fetch_constants = 0;
  size_t indices = 0;
  uint32_t index_count = 0;
  bool wide_indices = false;
  int32_t base_vertex = 0;
};

// The only part of the native layer that touches Metal, so the rest stays plain C++. Everything but gpu_totals() is for
// one thread.
class MetalDevice {
 public:
  static constexpr int kFramesInFlight = 3;

  // Wraps guest memory for the GPU without copying it. Compiling pipelines makes this slow, so call it off the game's
  // threads. Returns null if the Mac has no Metal device.
  static std::unique_ptr<MetalDevice> Create(uint8_t* guest_memory, size_t guest_bytes, size_t frame_bytes);

  virtual ~MetalDevice() = default;

  virtual bool guest_memory_wrapped() const = 0;

  // Loads a metallib and returns a handle for MetalDraw::vertex_shader, or -1. error says why.
  virtual int AddVertexShader(const std::filesystem::path& library, const MetalPullElement* elements, size_t count,
                              std::string& error) = 0;

  // Opens a window that shows every frame from here on.
  virtual void ShowWindow() = 0;

  // Records and submits one frame. encode gets that frame's memory and calls Draw. If capture isn't empty, waits for
  // the GPU and writes the picture there as a PNG.
  virtual MetalFrameTiming RecordFrame(const std::function<void(uint8_t* frame)>& encode,
                                       const std::filesystem::path& capture) = 0;

  virtual void Draw(const MetalDraw& draw) = 0;

  // Copies a range out through the GPU's view of guest memory and compares it with the CPU's. Waits for the GPU.
  virtual bool MatchesGuestMemory(uint32_t address, uint32_t bytes) = 0;

  virtual MetalGpuTotals gpu_totals() const = 0;
};

}  // namespace crackdown::native
