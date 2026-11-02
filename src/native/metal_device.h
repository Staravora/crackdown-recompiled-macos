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
  uint64_t queued_ns = 0;      // From each frame's commit until the GPU starts on it.
  uint64_t generation_ns = 0;  // GPU time of frame generation's own command buffers, also in gpu_ns.
  uint64_t resampled_copies = 0;  // Copies between a target and a resolve drawn at different scales.
  uint64_t dropped_copies = 0;    // Copies that couldn't be resampled.
  uint64_t dropped_stack_copies = 0;  // Depth from a scaled target for a stack, which is the guest's size.
  uint64_t dropped_reads = 0;         // ReadResolve of a scaled resolve, whose readers want the guest's size.
};

// What a resolve texture holds. Float is for the targets the water's FFT runs on.
enum class MetalResolveFormat { kColor, kFloat, kDepth };

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
  uint32_t max_anisotropy = 1;
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

// Clears part of a target, in order with its draws.
struct MetalRectClear {
  int target = 0;
  bool color = false;
  bool depth = false;
  float color_value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float depth_value = 1.0f;
  uint32_t stencil_value = 0;
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
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
  uint32_t scissor_top = 0;  // Rows of the target it can touch, across its whole width.
  uint32_t scissor_bottom = UINT32_MAX;
  uint32_t blend_control = 0;
  float blend_constant[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t color_mask = 0;
  bool alpha_test = false;
  bool alpha_to_coverage = false;  // Used on a multisampled target only.
  bool colorless = false;  // Runs no pixel shader and leaves colour alone, as the Xenos's depth-only mode does.
  int query = -1;  // The slot of the frame's query results that counts the samples this passes.
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

// What a frame leaves behind once the GPU is done with it.
struct MetalFrameResults {
  const uint64_t* query_samples = nullptr;  // What each query slot counted.
  const uint8_t* readback = nullptr;        // Pixels from ReadResolve, as it placed them.
  // From CopyPicture, in the order they were made: BGRA8, picture_width * 4 bytes a row.
  std::array<const uint8_t*, 2> pictures{};
  uint32_t picture_count = 0;
  uint32_t picture_width = 0;
  uint32_t picture_height = 0;
  // Frame generation finishes after the rest of the frame and reports its pictures alone, with nothing else set.
  bool pictures_only = false;
};

// The camera a frame was drawn with, for working out how each pixel moved since the last.
struct MetalCamera {
  std::array<float, 16> view_projection{};           // Rows, as the game's shaders take it: clip = rows · world.
  std::array<float, 16> inverse_view_projection{};
  std::array<float, 16> previous_view_projection{};
  float field_of_view = 0.0f;  // Vertical, in degrees.
  float aspect = 0.0f;
  float near_plane = 0.0f;
  float far_plane = 0.0f;
  float delta_time = 0.0f;
  bool reset = false;  // No usable frame before this one.
};

// The only part of the native layer that touches Metal, so the rest stays plain C++. Everything but gpu_totals() is for
// one thread.
class MetalDevice {
 public:
  static constexpr int kFramesInFlight = 3;
  static constexpr uint32_t kMaxQueries = 1024;
  static constexpr size_t kReadbackBytes = 1 << 20;

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
  // memory would hold them, so a fetch constant's swizzle means what it did there. Scale is as AddTarget's: the size is in
  // the guest's pixels, and the texture holds scale times as many each way.
  virtual int AddResolve(uint32_t width, uint32_t height, uint32_t levels, MetalResolveFormat format, float scale) = 0;
  virtual int ViewResolve(int resolve, bool gamma, const uint8_t* swizzle) = 0;

  // A stack of depth maps, read as a 3D texture, and a descriptor for it. Scale is as AddTarget's, each way across a slice.
  virtual int AddStack(uint32_t width, uint32_t height, uint32_t depth, float scale) = 0;

  // Opens a window that shows every frame from here on.
  virtual void ShowWindow() = 0;

  // A render target, with a colour part only if asked for: shadow maps are depth alone. With samples above 1 it's
  // multisampled, and reads of it see the samples resolved. Returns a handle. With scale above 1 it's drawn at that many
  // times the guest's pixels each way. Everything else that names its pixels, viewports, scissors, clears and copies, still
  // does so in the guest's, and the device scales them.
  virtual int AddTarget(uint32_t width, uint32_t height, bool has_color, bool float_color, uint32_t samples, float scale) = 0;

  // Records and submits one frame. encode gets that frame's memory and calls BeginPass, Draw and ShowTarget. If capture
  // isn't empty, waits for the GPU and writes the shown target there as a PNG. finished runs on a Metal thread when the GPU
  // is done.
  virtual MetalFrameTiming RecordFrame(const std::function<void(uint8_t* frame)>& encode, const std::filesystem::path& capture,
                                       const std::function<void(const MetalFrameResults&)>& finished) = 0;

  // Ends any open pass and starts one on a target.
  virtual void BeginPass(int target, const MetalClear& clear) = 0;

  virtual void Draw(const MetalDraw& draw) = 0;

  // Ends any open pass, so the next draw needs a BeginPass.
  virtual void CopyTarget(const MetalCopy& copy) = 0;
  virtual void ClearRect(const MetalRectClear& clear) = 0;
  virtual void CopyDepthToStack(int target, int stack, uint32_t slice) = 0;

  // Queues a resolve's pixels for MetalFrameResults::readback, at width * 4 bytes a row, as Metal holds them: each is the
  // reverse of the four bytes in guest memory. Returns where in it they'll be, or SIZE_MAX if they don't fit. Ends any open
  // pass.
  virtual size_t ReadResolve(int resolve) = 0;

  // Queues a resolve's pixels for MetalFrameResults::pictures, for showing outside Metal, upscaled scale times by MetalFX if
  // scale is above 1. They stay put until the frame slot comes round again, kFramesInFlight frames on. Ends any open pass.
  virtual bool CopyPicture(int resolve, uint32_t scale) = 0;

  // Frame generation. SnapshotScene keeps a target's colour and depth as they stand, before the HUD goes on. Then
  // InterpolatePicture has MetalFX make the frame halfway between the last snapshot and this one, with the HUD from the
  // finished picture in resolve, and queues it as a picture ahead of the real one. False if it couldn't.
  virtual bool SnapshotScene(int target) = 0;
  // A scene draw that moves on its own, drawn again into the motion with its vertex constants from a frame ago, at
  // previous_vertex_constants in that frame's memory.
  virtual void AddMotionDraw(const MetalDraw& draw, size_t previous_vertex_constants) = 0;
  virtual bool InterpolatePicture(int resolve, const MetalCamera& camera, uint32_t scale) = 0;

  // Writes a resolve into guest memory at byte offset at, as the Xenos lays out a 32-bit texture: by 32x32 tiles if
  // tiled, and with each pixel's bytes reversed if swap. Pitch is in pixels. A scaled resolve goes down to the guest's size
  // on the way. Ends any open pass.
  virtual bool WriteResolveToGuest(int resolve, size_t at, uint32_t pitch, bool tiled, bool swap) = 0;

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
