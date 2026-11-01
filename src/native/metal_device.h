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
  std::string last_error;
  uint64_t gpu_ns = 0;
  uint64_t worst_gpu_ns = 0;
};

// One vertex fetch of a shader, as pullElement reads it. See tools/shader-trial/pull_vertices.py.
struct MetalPullElement {
  uint32_t packed;
  int32_t offset;
  float scale;
  uint32_t select;
};

enum class MetalPrimitive { kTriangleList, kTriangleStrip };

enum class MetalTextureFormat {
  kRgba8,
  kRgba8Srgb,
  kBc1,
  kBc1Srgb,
  kBc2,
  kBc2Srgb,
  kBc3,
  kBc3Srgb,
  kBc5,
  kR8,
  kRg8,
  kB5G6R5,
  kRgb10A2,
  kR16,
  kRg16,
  kRgba16,
  kR16Float,
  kRg16Float,
  kRgba16Float,
  kR32Float,
  kRg32Float,
  kRgba32Float,
};

struct MetalTextureDesc {
  MetalTextureFormat format = MetalTextureFormat::kRgba8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t levels = 1;
  bool cube = false;
  uint8_t swizzle[4] = {0, 1, 2, 3};  // Xenos's: 0-3 pick a component, 4 is zero and 5 is one.
};

enum class MetalAddressMode { kRepeat, kMirrorRepeat, kClamp, kMirrorClamp };

struct MetalSamplerDesc {
  MetalAddressMode address_u = MetalAddressMode::kRepeat;
  MetalAddressMode address_v = MetalAddressMode::kRepeat;
  bool nearest = false;
};

struct MetalViewport {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float near_depth = 0.0f;
  float far_depth = 1.0f;
  bool operator==(const MetalViewport&) const = default;
};

// What a pass starts by clearing. The rest carries on from what the target holds.
struct MetalClear {
  bool color = false;
  bool depth = false;
  float color_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  double depth_value = 1.0;
};

// Copies part of a render target into a resolve texture. The source can run past the target's edge.
struct MetalCopy {
  int target = 0;
  bool depth = false;
  int resolve = 0;
  uint32_t level = 0;
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
  int32_t dest_x = 0;
  int32_t dest_y = 0;
};

struct MetalDraw {
  int vertex_shader = 0;
  int pixel_shader = -1;  // -1 is plain grey.
  int debug_id = -1;  // Paints the draw flat, in a colour that gives its number.
  uint32_t depth_control = 0;
  uint32_t stencil_front = 0;
  uint32_t stencil_back = 0;
  uint32_t mode_control = 0;
  float depth_bias_slope = 0.0f;
  uint32_t blend_control = 0;
  uint32_t color_mask = 0;
  bool alpha_test = false;
  const int* textures = nullptr;  // Descriptors the pixel shader reads.
  uint32_t texture_count = 0;
  MetalViewport viewport;
  MetalPrimitive primitive = MetalPrimitive::kTriangleList;
  size_t vertex_constants = 0;  // Into the frame's memory, and so are the rest.
  size_t pixel_constants = 0;
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

  // Loads a pixel shader and returns a handle for MetalDraw::pixel_shader, or -1.
  virtual int AddPixelShader(const std::filesystem::path& library, std::string& error) = 0;

  // Adds a texture and returns its descriptor, or -1. Descriptor 0 is a dummy. Fill gives a mip level its data, as one row
  // of blocks after another. Only a cube has more than face 0.
  virtual int AddTexture(const MetalTextureDesc& desc) = 0;
  virtual void FillTexture(int descriptor, uint32_t level, uint32_t face, uint32_t width, uint32_t height, const void* data,
                           size_t bytes_per_row) = 0;
  virtual int AddSampler(const MetalSamplerDesc& desc) = 0;

  // A texture render targets get copied into, and a descriptor for reading one. It reads colour bytes in the order guest
  // memory would hold them, so a fetch constant's swizzle means what it did there.
  virtual int AddResolve(uint32_t width, uint32_t height, uint32_t levels, bool depth) = 0;
  virtual int ViewResolve(int resolve, bool gamma, const uint8_t* swizzle) = 0;

  // A stack of depth maps, read as a 3D texture, and a descriptor for it.
  virtual int AddStack(uint32_t width, uint32_t height, uint32_t depth) = 0;

  // Opens a window that shows every frame from here on.
  virtual void ShowWindow() = 0;

  // A render target, with a colour part only if asked for: shadow maps are depth alone. Returns a handle.
  virtual int AddTarget(uint32_t width, uint32_t height, bool has_color) = 0;

  // Records and submits one frame. encode gets that frame's memory and calls BeginPass, Draw and ShowTarget. If capture
  // isn't empty, waits for the GPU and writes the shown target there as a PNG.
  virtual MetalFrameTiming RecordFrame(const std::function<void(uint8_t* frame)>& encode,
                                       const std::filesystem::path& capture) = 0;

  // Ends any open pass and starts one on a target.
  virtual void BeginPass(int target, const MetalClear& clear) = 0;

  virtual void Draw(const MetalDraw& draw) = 0;

  // Ends any open pass, so the next draw needs a BeginPass.
  virtual void CopyTarget(const MetalCopy& copy) = 0;
  virtual void CopyDepthToStack(int target, int stack, uint32_t slice) = 0;

  // Which colour target this frame's window and capture show, and whether it goes through the gamma ramp on the way, as
  // the Xenos's display does.
  virtual void ShowTarget(int target, bool gamma) = 0;

  // The 256 entries of the ramp, as 10-bit blue, green and red.
  virtual void SetGammaRamp(const uint32_t* ramp) = 0;

  // Puts alpha in the colour channels of captures, for looking at what a target keeps there.
  virtual void CaptureAlpha(bool alpha) = 0;

  // Copies a range out through the GPU's view of guest memory and compares it with the CPU's. Waits for the GPU.
  virtual bool MatchesGuestMemory(uint32_t address, uint32_t bytes) = 0;

  virtual MetalGpuTotals gpu_totals() const = 0;
};

}  // namespace crackdown::native
