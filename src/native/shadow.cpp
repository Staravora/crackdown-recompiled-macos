#include "shadow.h"
#include "thermal_state.h"

#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/thread/mutex.h>

#include "../updater/sha256.h"
#include "metal_device.h"
#include "texture_cache.h"

REXCVAR_DEFINE_BOOL(crackdown_native_shadow, false, "Crackdown",
                    "Draw every frame's baked geometry in Metal, off screen, beside the emulator, and write native-shadow-<time>.json");

REXCVAR_DEFINE_BOOL(crackdown_native_shadow_window, false, "Crackdown", "Show shadow mode's picture in a window of its own");

REXCVAR_DEFINE_BOOL(crackdown_native_exposure, false, "Crackdown",
                    "With shadow mode on, the game's exposure reads Metal's picture rather than the emulator's");

REXCVAR_DEFINE_BOOL(crackdown_native_pacing, false, "Crackdown",
                    "With shadow mode on, the game waits for Metal to catch up instead of Metal dropping frames");

REXCVAR_DEFINE_BOOL(crackdown_texture_sweep, true, "Crackdown",
                    "With the Metal renderer, check the textures drawn lately against game memory, a share each frame, and "
                    "reload any that changed without the memory watch noticing, which otherwise draw black or in flat colours");
REXCVAR_DEFINE_BOOL(crackdown_native_only, true, "Crackdown",
                    "Metal draws the game and the emulator draws nothing. The Mac's default renderer. Turns on shadow mode, "
                    "pacing and exposure, and Metal answers occlusion queries. Without its shaders the emulator draws instead");

REXCVAR_DEFINE_INT32(crackdown_metalfx_upscale, 1, "Crackdown",
                     "With the Metal renderer, MetalFX upscales the picture this many times before the window scales it: 1 or 2");

REXCVAR_DEFINE_INT32(crackdown_render_scale, 100, "Crackdown",
                     "With the Metal renderer, the percent of the game's 1280x720 the scene is drawn at: 100, 150 for 1920x1080 "
                     "or 200 for 2560x1440");

// Out of the launcher: in play, jumping smeared the player even with each object's own motion. Kept on the command line
// for trying it again, under a name the launcher never saved, so an old saved "on" doesn't turn it on.
REXCVAR_DEFINE_BOOL(crackdown_frame_generation_preview, false, "Crackdown",
                    "Experimental, command line only: with the Metal renderer, MetalFX makes a frame between each two the game "
                    "draws and shows it halfway. Not good enough to play with yet");

// On by default: a problem seen in play can then be examined while it's still on screen. Opening a missing file once a
// second costs nothing, and the file is removed at start so words left from an earlier run never apply.
REXCVAR_DEFINE_BOOL(crackdown_native_shadow_debug, true, "Crackdown",
                    "Poll metal-debug.txt in the logs folder, so a running game can be examined from outside");

namespace crackdown::native::shadow {
namespace {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

using Clock = std::chrono::steady_clock;

constexpr size_t kGuestBytes = size_t(1) << 29;
constexpr uint32_t kGuestMask = uint32_t(kGuestBytes - 1);
constexpr uint32_t kAluDwords = 2048;
constexpr size_t kConstantBytes = kAluDwords * 4;
constexpr size_t kPixelConstantsOffset = 1024 * 4;
constexpr size_t kFetchBytes = 192 * 4;
constexpr size_t kLoopTableOffset = 512;
constexpr uint32_t kLoopConstants = 32;
constexpr size_t kSharedBytes = kLoopTableOffset + kLoopConstants * 16;
constexpr size_t kBooleansOffset = 256;
constexpr size_t kAlphaThresholdOffset = 272;
constexpr size_t kHalfPixelOffset = 264;
constexpr size_t kScreenScaleOffset = 280;
constexpr size_t kScreenOffsetOffset = 288;
constexpr size_t kDepthOffsetOffset = 296;
constexpr size_t kPixelScaleOffset = 304;  // Guest pixels to one of the target's, for shaders that read the pixel position.
constexpr size_t kVertexTextureTableOffset = 320;
constexpr size_t kVertexSamplerTableOffset = 336;
constexpr uint32_t kFirstVertexFetch = 16;
constexpr size_t kSamplerTableOffset = 192;
constexpr std::array<size_t, 3> kTextureTableOffsets = {0, 64, 128};
constexpr size_t kFetchConstantDwords = 6;
constexpr uint32_t kTextureFetchConstant = 28;
constexpr size_t kDrawBytes = kConstantBytes + kFetchBytes + kSharedBytes;
constexpr size_t kConstantRegionBytes = 96 << 20;
constexpr size_t kIndexRegionBytes = 32 << 20;
constexpr size_t kFrameBytes = kConstantRegionBytes + kIndexRegionBytes;
constexpr size_t kPooledFrames = 3;
constexpr size_t kRunsPerFrame = 4096;
constexpr uint32_t kReportFrames = 300;
constexpr uint32_t kCaptureFrames = 300;
constexpr uint32_t kCheckFrames = 300;
constexpr uint32_t kCheckBytes = 4096;
constexpr uint32_t kMaxShaderDwords = 1 << 14;
constexpr uint32_t kMaxTargetSize = 4096;
constexpr size_t kMaxPendingQueries = 8192;
constexpr size_t kMaxQueryDisagreements = 32;
constexpr uint64_t kSmallQuerySamples = 8;
constexpr uint64_t kLargeQuerySamples = 64;
constexpr int kMaxUpscale = 2;
constexpr float kFrameGenerationFarPlane = 20000.0f;
constexpr uint32_t kQueryBlocks = 24;  // On a query object: a pointer per tile to where its counts go.
constexpr uint32_t kQueryTileCount = 144;
constexpr uint32_t kMaxQueryTiles = 30;
constexpr uint32_t kQueryBlockWords = 16;  // The end block's eight counters, then the begin block's.
constexpr uint32_t kQueryTotal = 0;
constexpr uint32_t kQueryZPass = 4;
constexpr uint32_t kDeviceWriteback = 10768;
constexpr uint32_t kDeviceFence = 10780;
constexpr auto kIdleFenceRelease = std::chrono::milliseconds(50);
// D3D's present waits for the frame before it, so the game is never more than this far ahead. Two lets Metal encode a frame
// while the GPU draws the one before.
constexpr uint64_t kMaxFramesBehind = 2;
constexpr auto kLongestHold = std::chrono::milliseconds(250);
constexpr uint32_t kReadbackCheckLocks = 30;  // Every 30th lock is compared.
constexpr size_t kKeptReadbacks = 16;
constexpr int kFarPixel = 8;
constexpr uint32_t kPixelBytesLog2 = 2;
constexpr uint64_t kExposureSkippedSamples = 600;  // The brightest few, which the game's exposure leaves out.
constexpr int kQueryMarkId = 0xFF00;  // Magenta.
constexpr std::array<double, 4> kQueryRatioBounds = {0.375, 0.75, 1.5, 3.0};

constexpr uint32_t kSourceAutoIndex = 2;
constexpr uint32_t kEdramDepthOnly = 5;
constexpr uint32_t kAluExport = 1 << 15;
constexpr uint32_t kAluWriteMasks = 0xFF0000;
constexpr uint32_t kPrimitiveTriangleList = 4;
constexpr uint32_t kPrimitiveTriangleFan = 5;
constexpr uint32_t kPrimitiveTriangleStrip = 6;
constexpr uint32_t kPrimitiveQuadList = 13;
constexpr uint64_t kHashBasis = 0xCBF29CE484222325ull;
constexpr uint64_t kHashPrime = 0x100000001B3ull;
constexpr uint32_t kStencilEnable = 1 << 0;
constexpr uint32_t kClearColorTargets = 0x0F;
constexpr uint32_t kClearDepthStencil = 0x30;
constexpr uint32_t kDepthEnable = 1 << 1;
constexpr uint32_t kDepthWrite = 1 << 2;
constexpr uint32_t kStencilWriteMask = 0xFF << 16;
constexpr uint32_t kStencilOperations = 0xFF8FF800;  // What fail, pass and depth fail do, on both faces. All zero is keep.
constexpr uint32_t kBlendDestinationShift = 8;
constexpr uint32_t kBlendFactorMask = 0x1F;
constexpr uint32_t kBlendFactorZero = 0;
constexpr uint32_t kBlendFactorOne = 1;
constexpr uint32_t kColorMaskFirstTarget = 0xF;
constexpr uint32_t kAlphaTestEnable = 1 << 3;
constexpr uint32_t kAlphaToCoverage = 1 << 4;
constexpr uint32_t kCullBothFaces = 3;
constexpr uint32_t kViewportScaleX = 1;
constexpr uint32_t kPolygonOffsetFront = 1 << 11;
constexpr uint32_t kPolygonOffsetBack = 1 << 12;
constexpr uint32_t kIndexEndianShift = 30;
constexpr uint32_t kIndexEndian16 = 1;
constexpr uint32_t kIndexEndian32 = 2;
constexpr uint32_t kResolveSourceMask = 7;
constexpr uint32_t kResolveDepth = 4;
constexpr uint32_t kResolveClearColor = 1 << 8;
constexpr uint32_t kResolveClearDepth = 1 << 9;
constexpr int32_t kSecondTileTop = 480;
constexpr int32_t kFirstTileBottom = 484;
constexpr uint32_t kFirstTileMask = 0x3;
constexpr uint32_t kSecondTileMask = 0xC;
constexpr uint32_t kBiasShift = 28;
constexpr uint32_t kHalfPictureWidth = 640;
constexpr uint32_t kHalfTileHeight = 242;
constexpr int32_t kHalfSecondTileHeight = 120;

using FetchConstant = std::array<uint32_t, kFetchConstantDwords>;

// Draws, or a resolve, which fills in only resolve, dest and targets.
struct Run {
  std::optional<ResolveCopy> resolve;
  FetchConstant dest{};
  uint64_t resolve_number = 0;  // How many resolves its texture has had, this one included.
  std::shared_ptr<const std::vector<BakedList>> lists;
  uint32_t pass_select = 0;
  uint32_t clear = 0;  // What the game cleared since the last run, as D3DCLEAR flags.
  Targets targets;
  std::shared_ptr<const std::vector<uint8_t>> vertex_code;
  std::shared_ptr<const VertexCode> list_code;
  std::array<uint32_t, kAluDwords> constants{};
  std::array<uint32_t, kLoopConstants> loops{};
};

// A query beginning or ending just before a run.
struct QueryEvent {
  size_t run;
  uint64_t id;
  uint32_t query;
  bool begin;
};

// The query a slot of a frame's results belongs to.
struct QuerySlot {
  uint64_t id;
  uint32_t query;
  float area = 1.0f;  // Pixels of the target its draws went to per guest pixel, which its count is divided by.
};

// A texture's contents after a given resolve, by how many resolves it's had and its first page, so oldest sorts first.
using ResolveKey = std::pair<uint64_t, uint32_t>;

// Where a frame's readback holds a resolve.
struct ReadbackSlot {
  ResolveKey key;
  size_t offset;
  uint32_t width;
  uint32_t height;
};

// What a frame hands the game back, in the order the slots of its results run.
struct FrameAnswers {
  std::vector<QuerySlot> queries;
  std::vector<ReadbackSlot> readbacks;
  uint32_t front_buffer = 0;  // Guest memory Metal wrote the front buffer to, by its virtual address, and how much.
  uint32_t front_buffer_bytes = 0;
  uint32_t front_width = 0;  // The guest front buffer's size, which a present names.
  uint32_t front_height = 0;
};

// The newest finished picture, for the emulator's swap to present when it takes pictures from outside (SDK patch 0016).
// Metal's buffer for it stays put until its frame slot comes round again.
std::mutex g_picture_mutex;
const uint8_t* g_picture = nullptr;
uint32_t g_picture_width = 0;
uint32_t g_picture_height = 0;
bool g_external_present = false;  // Render thread only after it's set.

// Frame generation. The emulator's swaps show nothing (SDK patch 0018), and a clock thread shows each frame's pictures:
// the made one at once and the real one half a frame later.
void (*g_present_external_frame)(uint32_t, uint32_t) = nullptr;
void (*g_set_external_present_paced)(bool) = nullptr;
bool g_frame_generation = false;  // Render thread, once it's set up.
struct PictureSet {
  std::array<const uint8_t*, 2> pictures{};
  uint32_t count = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t guest_width = 0;
  uint32_t guest_height = 0;
};
std::mutex g_present_mutex;
std::condition_variable g_present_wake;
std::optional<PictureSet> g_present_next;  // Under g_present_mutex.
// Render thread: the front buffer's size, the target last resolved into it, and the camera.
uint32_t g_front_width = 0;
uint32_t g_front_height = 0;
uint32_t g_front_page = 0;
int g_front_target = -1;
int g_next_front_target = -1;
std::optional<std::array<float, 16>> g_last_view_projection;
// Render thread: the scene's targets, the game's picture size, are drawn this many times bigger each way.
constexpr uint32_t kScaledWidth = 1280;
constexpr uint32_t kScaledHeight = 720;
constexpr int kMaxRenderScale = 200;
float g_render_scale = 1.0f;
std::vector<float> g_target_scales;  // By target.
Clock::time_point g_last_frame_time;
void (*g_found_fence_done)(uint32_t) = nullptr;  // Render thread. RenderThread passes it on to g_set_fence_done.

uint32_t PictureWidth() {
  std::lock_guard lock(g_picture_mutex);
  return g_picture_width;
}

uint32_t PictureHeight() {
  std::lock_guard lock(g_picture_mutex);
  return g_picture_height;
}

bool PictureSize(void*, uint32_t* width, uint32_t* height) {
  std::lock_guard lock(g_picture_mutex);
  *width = g_picture_width;
  *height = g_picture_height;
  return g_picture != nullptr;
}

bool FillPicture(void*, void* pixels, uint32_t row_bytes, uint32_t width, uint32_t height) {
  std::lock_guard lock(g_picture_mutex);
  if (!g_picture || width != g_picture_width || height != g_picture_height) {
    return false;
  }
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(static_cast<uint8_t*>(pixels) + size_t(y) * row_bytes, g_picture + size_t(y) * width * 4, size_t(width) * 4);
  }
  return true;
}

void ShowPicture(const uint8_t* picture, uint32_t width, uint32_t height, uint32_t guest_width, uint32_t guest_height) {
  {
    std::lock_guard lock(g_picture_mutex);
    g_picture = picture;
    g_picture_width = width;
    g_picture_height = height;
  }
  g_present_external_frame(guest_width, guest_height);
}

// Shows each frame's made picture as it arrives and its real one halfway to the next frame, by the gap between arrivals.
void PresentClock() {
  pthread_setname_np("metal-present");
  Clock::time_point last_arrival;
  double interval = 1.0 / 30.0;
  while (true) {
    PictureSet set;
    {
      std::unique_lock lock(g_present_mutex);
      g_present_wake.wait(lock, [] { return g_present_next.has_value(); });
      set = *g_present_next;
      g_present_next.reset();
    }
    const Clock::time_point arrival = Clock::now();
    if (last_arrival.time_since_epoch().count()) {
      const double gap = std::chrono::duration<double>(arrival - last_arrival).count();
      interval += (std::clamp(gap, 1.0 / 120.0, 0.1) - interval) * 0.1;
    }
    last_arrival = arrival;
    ShowPicture(set.pictures[0], set.width, set.height, set.guest_width, set.guest_height);
    if (set.count < 2) {
      continue;
    }
    // A frame that arrives sooner brings its own pictures, so the real one goes up then, rather than late.
    std::unique_lock lock(g_present_mutex);
    g_present_wake.wait_until(lock, arrival + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval / 2.0)),
                              [] { return g_present_next.has_value(); });
    lock.unlock();
    ShowPicture(set.pictures[1], set.width, set.height, set.guest_width, set.guest_height);
  }
}

// Hands the emulator's swap a source of pictures, if its plugin can take one.
void UseExternalPresent() {
  using SetSource = void (*)(bool (*)(void*, uint32_t*, uint32_t*), bool (*)(void*, void*, uint32_t, uint32_t, uint32_t), void*);
  void* plugin = dlopen("@executable_path/librexgpu-xenos.dylib", RTLD_NOLOAD | RTLD_LAZY);
  const auto set_source = plugin ? reinterpret_cast<SetSource>(dlsym(plugin, "rex_gpu_set_external_frame_source")) : nullptr;
  if (!set_source) {
    REXLOG_WARN("Native only: the GPU plugin can't take Metal's pictures, so they go through guest memory");
    return;
  }
  set_source(PictureSize, FillPicture, nullptr);
  g_external_present = true;
  REXLOG_INFO("Native only: the swap presents Metal's pictures directly");
  g_found_fence_done = reinterpret_cast<void (*)(uint32_t)>(dlsym(plugin, "rex_gpu_set_external_fence_done"));
  if (REXCVAR_GET(crackdown_frame_generation_preview)) {
    g_present_external_frame = reinterpret_cast<void (*)(uint32_t, uint32_t)>(dlsym(plugin, "rex_gpu_present_external_frame"));
    g_set_external_present_paced = reinterpret_cast<void (*)(bool)>(dlsym(plugin, "rex_gpu_set_external_present_paced"));
    if (g_present_external_frame && g_set_external_present_paced) {
      g_set_external_present_paced(true);
      g_frame_generation = true;
      std::thread(PresentClock).detach();
      REXLOG_INFO("Native only: MetalFX frame generation is on");
    } else {
      REXLOG_WARN("Native only: the GPU plugin can't pace presents, so frame generation is off");
    }
  }
  if (g_found_fence_done) {
    REXLOG_INFO("Native only: fences wait for Metal");
  } else {
    REXLOG_WARN("Native only: the GPU plugin can't hold fences for Metal, so the game may reuse memory Metal is reading");
  }
}

// What the game saw when it locked a texture, for comparing with Metal's copy.
struct GuestPixels {
  std::vector<uint8_t> bytes;
  uint32_t pitch;  // In pixels.
  bool tiled;
};

// One place in the game's fence waits, by how far Metal had got with the fence when the wait began.
struct FenceWaits {
  uint64_t count = 0;
  uint64_t ns = 0;
  uint64_t worst_ns = 0;
  uint64_t metal_done = 0;   // Metal had finished it, so it was only ever waiting on the emulator.
  uint64_t in_flight = 0;    // Handed over and not finished.
  uint64_t open_frame = 0;   // In the frame still being built, which Metal hasn't seen.
};

struct PacingTotals {
  uint64_t frames = 0;
  uint64_t fences = 0;
  uint64_t metal_ns = 0;  // Handing a frame over to the GPU finishing it.
  uint64_t worst_metal_ns = 0;
  int64_t emulator_lead = 0;  // Fences the emulator had passed beyond Metal's when Metal finished a frame, summed.
  uint64_t held_frames = 0;
  uint64_t held_ns = 0;
  // Fences let through because nothing of Metal's was pending. open: draws of the frame in progress had already been
  // captured, so the game could write over memory Metal hasn't read yet.
  uint64_t idle_releases = 0;
  uint64_t idle_releases_open = 0;
  std::map<uint32_t, FenceWaits> waits;
};

// How Metal's copy of a resolve compares with what the game read, per byte of a pixel in guest memory, summed per comparison.
struct ReadbackTotals {
  uint64_t compared = 0;
  uint64_t missed = 0;
  uint64_t written = 0;
  uint64_t late = 0;
  std::array<double, 4> guest_mean{};
  std::array<double, 4> metal_mean{};
  std::array<double, 4> difference{};  // Mean absolute.
  std::array<double, 4> far{};         // Share of pixels more than kFarPixel apart.
  double guest_exposure = 0.0;
  double metal_exposure = 0.0;
};

struct Frame {
  std::vector<Run> runs;
  std::vector<QueryEvent> queries;
  uint32_t fence = 0;  // The newest the game had issued when it handed the frame over.
  std::optional<FetchConstant> front_buffer;  // What the game swapped to the screen, as its texture header says.
  Clock::time_point handed_over;
};

struct Totals {
  uint64_t frames = 0;
  uint64_t draws = 0;
  uint64_t skipped_draws = 0;
  uint64_t no_room = 0;
  uint64_t recorded_draws = 0;
  uint64_t recorded_indices = 0;
  uint64_t no_shader_draws = 0;
  uint64_t no_target_draws = 0;
  uint64_t passes = 0;
  uint64_t other_primitive_draws = 0;
  uint64_t other_index_order_draws = 0;
  uint64_t transparent_draws = 0;
  uint64_t shaded_draws = 0;
  uint64_t needs_resolve_draws = 0;
  uint64_t native_resolves = 0;
  uint64_t missed_resolves = 0;
  uint64_t pixel_shaders_found = 0;
  uint64_t pixel_shaders_missing = 0;
  uint64_t no_color_draws = 0;
  uint64_t alpha_tested_draws = 0;
  uint64_t index_copy_ns = 0;
  uint64_t shaders_found = 0;
  uint64_t shaders_missing = 0;
  uint64_t shaders_failed = 0;
  uint64_t wait_ns = 0;
  uint64_t guest_checks = 0;
  uint64_t guest_matches = 0;
  uint64_t queries_no_room = 0;
  uint64_t front_buffers_missed = 0;
  uint64_t frames_generated = 0;
  uint64_t motion_draws = 0;
  uint64_t opening_pass_frames = 0;
  uint64_t vertex_code_changed = 0;  // Draws whose vertex shader the game rewrote between issuing and Metal drawing them.
};

// Each query's answers until both have arrived. Metal's come when its frame finishes and the emulator's when the game asks.
struct QueryAnswers {
  std::optional<uint64_t> metal;
  std::optional<uint32_t> emulator;
};

struct QueryDisagreement {
  uint32_t query;
  uint64_t metal;
  uint64_t least;
  uint32_t emulator;
  uint32_t times;
};

struct QueryAsks {
  uint64_t waiting = 0;
  uint64_t none = 0;
  uint64_t some = 0;
  uint64_t largest = 0;
};

struct QueryTotals {
  uint64_t both_none = 0;
  uint64_t both_some = 0;
  uint64_t metal_only = 0;
  uint64_t emulator_only = 0;
  uint64_t unanswered = 0;
  uint64_t metal_samples = 0;
  uint64_t emulator_samples = 0;
  std::array<uint64_t, kQueryRatioBounds.size() + 1> ratios{};  // Metal's samples over the emulator's, by kQueryRatioBounds.
  std::array<uint64_t, 3> metal_only_sizes{};  // Under kSmallQuerySamples, under kLargeQuerySamples, and the rest.
  std::vector<QueryDisagreement> disagreements;
  std::map<uint32_t, QueryAsks> asks;  // By where in the game they came from.
  std::map<std::array<uint32_t, 5>, uint64_t> locks;  // By caller, base page, format, width and height.
  uint64_t quads = 0;
};

std::atomic<bool> g_on{false};
std::mutex g_mutex;
std::condition_variable g_wake;
std::unique_ptr<Frame> g_current;
std::vector<std::unique_ptr<Frame>> g_free;
std::deque<std::unique_ptr<Frame>> g_ready;
uint64_t g_guest_frames = 0;
uint64_t g_dropped = 0;
uint64_t g_capture_ns = 0;
uint64_t g_direct_draws = 0;
uint64_t g_ring_desyncs = 0;
std::vector<FetchConstant> g_resolved_this_frame;
std::vector<FetchConstant> g_resolved_last_frame;
uint32_t g_clear_pending = 0;
std::array<uint32_t, BakedLibrary::kGammaEntries> g_gamma_ramp{};
bool g_gamma_ramp_new = false;

std::mutex g_pacing_mutex;
std::condition_variable g_frame_finished;
// With the emulator drawing nothing, its fence and read pointer writes wait for Metal (SDK patch 0017). Frames in the order
// they were handed over, by the last fence each covers, so a frame only counts as done once those before it are.
struct FrameFence {
  uint32_t fence;
  bool finished;
};
std::deque<FrameFence> g_frame_fences;  // Under g_pacing_mutex.
void (*g_set_fence_done)(uint32_t) = nullptr;
PacingTotals g_pacing_totals;
uint64_t g_frames_handed_over = 0;
uint64_t g_frames_finished = 0;
std::atomic<uint32_t> g_device{0};
std::atomic<uint8_t*> g_virtual_guest{nullptr};
std::atomic<uint32_t> g_handed_over_fence{0};
std::atomic<uint32_t> g_finished_fence{0};

std::mutex g_query_mutex;
uint64_t g_last_query = 0;
std::unordered_map<uint32_t, uint64_t> g_latest_query;  // The last issue of each query object.
std::map<uint64_t, QueryAnswers> g_query_answers;
QueryTotals g_query_totals;

std::mutex g_readback_mutex;
std::unordered_map<uint32_t, uint64_t> g_resolve_numbers;  // By first page.
std::set<std::array<uint32_t, 3>> g_read_textures;  // Page, width and height of what the game locks, which Metal copies out.
std::map<ResolveKey, std::vector<uint8_t>> g_metal_pixels;
std::map<ResolveKey, GuestPixels> g_guest_pixels;
uint64_t g_locks_seen = 0;
ReadbackTotals g_readback_totals;

// Render thread only from here down.
uint8_t* g_guest = nullptr;
uint64_t g_vertex_code_copied = 0;  // Under g_mutex: vertex shaders copied at capture that a command buffer's copies missed.
rex::memory::Memory* g_memory = nullptr;
std::filesystem::path g_path;
std::filesystem::path g_shader_dir;
std::unordered_map<std::string, std::vector<uint32_t>> g_manifest;
std::unordered_map<std::string, std::string> g_loose_manifest;
std::unordered_map<std::string, std::vector<uint32_t>> g_vertex_texture_manifest;
std::vector<std::vector<uint32_t>> g_vertex_textures;  // By handle.
// By address and size. -1 means looked at and no use.
std::unordered_map<uint64_t, int> g_vertex_shaders;
struct PixelSlot {
  uint32_t slot;
  uint32_t kind;  // 0 is 2D, 1 is 3D and 2 is cube.
};
struct PixelShader {
  int handle = -1;
  std::string name;
  std::vector<PixelSlot> slots;
  bool reads_whole_picture = false;  // By pixel position, so it wants joined-up textures even when drawn for one tile.
};
std::unordered_map<std::string, std::vector<PixelSlot>> g_pixel_manifest;
std::set<std::string> g_screen_shaders;
std::unordered_map<uint64_t, PixelShader> g_pixel_shaders;
std::unique_ptr<TextureCache> g_textures;
const PixelShader g_no_pixel_shader;
Totals g_totals;
uint64_t g_frame_number = 0;
// The small pass the game draws first in play: a 128x128 effect that feeds itself, then a 320x180 copy of the scene,
// which later draws read back. Every glitched report so far lacks it and the clean one has it, so the log says when it
// stops and comes back, to line up with the glitch. kOpeningPassGap frames without it count as stopping.
constexpr uint64_t kOpeningPassGap = 3;
bool g_opening_pass_running = false;
uint64_t g_opening_pass_since = 0;  // The frame it last started or stopped.
uint64_t g_opening_pass_last = 0;   // The last frame it ran.

bool OpeningPassTarget(const Targets& targets) {
  return targets.color && ((targets.width == 128 && targets.height == 128) || (targets.width == 320 && targets.height == 180));
}

void NoteOpeningPass(bool ran) {
  if (ran) {
    ++g_totals.opening_pass_frames;
    if (!g_opening_pass_running) {
      if (g_opening_pass_since) {
        REXLOG_INFO("Native shadow: the opening pass came back at frame {}, after {} frames without it", g_frame_number,
                    g_frame_number - g_opening_pass_last - 1);
      } else {
        REXLOG_INFO("Native shadow: the opening pass started at frame {}", g_frame_number);
      }
      g_opening_pass_running = true;
      g_opening_pass_since = g_frame_number;
    }
    g_opening_pass_last = g_frame_number;
  } else if (g_opening_pass_running && g_frame_number - g_opening_pass_last >= kOpeningPassGap) {
    REXLOG_INFO("Native shadow: the opening pass stopped after frame {}, having run for {} frames", g_opening_pass_last,
                g_opening_pass_last - g_opening_pass_since + 1);
    g_opening_pass_running = false;
    g_opening_pass_since = g_opening_pass_last;
  }
}
std::vector<uint32_t> g_encode_us;
std::array<uint32_t, kAluDwords> g_constants{};
uint32_t g_check_address = 0;
uint64_t g_quads_counted = 0;
uint32_t g_check_bytes = 0;

uint64_t Since(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

void ApplyWrites(const BakedList& list, const BakedDraw& draw) {
  for (uint32_t i = 0; i < draw.constant_writes; ++i) {
    const BakedConstantWrite& write = list.constant_writes[draw.first_constant_write + i];
    if (write.first_dword + uint32_t(write.dwords) <= kAluDwords) {
      std::memcpy(g_constants.data() + write.first_dword, list.constant_data.data() + write.data,
                  write.dwords * sizeof(uint32_t));
    }
  }
}

void ApplyLiterals(const BakedDraw& draw) {
  for (uint32_t i = 0; i < draw.literal_count; ++i) {
    const BakedLiteral& literal = draw.literals[i];
    const uint32_t address = literal.address & kGuestMask;
    if (!literal.dwords || literal.first_dword + uint32_t(literal.dwords) > kAluDwords ||
        uint64_t(address) + literal.dwords * 4ull > kGuestBytes) {
      continue;
    }
    std::memcpy(g_constants.data() + literal.first_dword, g_guest + address, size_t(literal.dwords) * 4);
    for (uint32_t k = 0; k < literal.dwords; ++k) {
      g_constants[literal.first_dword + k] = std::byteswap(g_constants[literal.first_dword + k]);
    }
  }
}

uint32_t Word(const uint8_t* code, uint32_t dword) {
  uint32_t value;
  std::memcpy(&value, code + size_t(dword) * 4, sizeof(value));
  return std::byteswap(value);
}

struct Instruction {
  uint32_t at;
  bool fetch;
};

// Each exec block's instructions. Matches exec_blocks in tools/shader-trial/pull_vertices.py.
std::vector<std::vector<Instruction>> ExecBlocks(const std::vector<uint32_t>& words) {
  std::vector<std::vector<Instruction>> blocks;
  const uint32_t dwords = uint32_t(words.size());
  uint32_t first_instruction = dwords / 3;
  for (uint32_t pair = 0; pair < first_instruction && pair * 3 + 2 < dwords; ++pair) {
    const uint32_t low = words[pair * 3], middle = words[pair * 3 + 1], high = words[pair * 3 + 2];
    const std::array<std::array<uint32_t, 2>, 2> flow = {{{low, middle & 0xFFFF}, {(middle >> 16) | (high << 16), high >> 16}}};
    for (const auto& [flow_low, flow_high] : flow) {
      const uint32_t opcode = (flow_high >> 12) & 0xF;
      const bool is_exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
      if (!is_exec) {
        continue;
      }
      const uint32_t address = flow_low & 0xFFF, count = (flow_low >> 12) & 7, sequence = (flow_low >> 16) & 0xFFF;
      first_instruction = std::min(first_instruction, address);
      std::vector<Instruction> block;
      for (uint32_t i = 0; i < count; ++i) {
        if ((address + i) * 3 + 2 < dwords) {
          block.push_back({address + i, ((sequence >> (i * 2)) & 1) != 0});
        }
      }
      blocks.push_back(std::move(block));
    }
  }
  return blocks;
}

// Each exec block's vertex fetches, as addresses.
std::vector<std::vector<uint32_t>> FetchBlocks(const std::vector<uint32_t>& words) {
  std::vector<std::vector<uint32_t>> blocks;
  for (const std::vector<Instruction>& block : ExecBlocks(words)) {
    std::vector<uint32_t> fetches;
    for (const Instruction& instruction : block) {
      if (instruction.fetch && (words[instruction.at * 3] & 0x1F) == 0) {
        fetches.push_back(instruction.at);
      }
    }
    if (!fetches.empty()) {
      blocks.push_back(std::move(fetches));
    }
  }
  return blocks;
}

std::vector<uint32_t> WordsOf(const uint8_t* code, uint32_t dwords) {
  std::vector<uint32_t> words(dwords);
  for (uint32_t i = 0; i < dwords; ++i) {
    words[i] = Word(code, i);
  }
  return words;
}

// Blanks what binding patches and sorts each block's fetches, which binding reorders by offset, so a live shader hashes
// like its shipped form. Matches masked_code in tools/shader-trial/pull_vertices.py.
std::vector<uint32_t> MaskedWords(const uint8_t* code, uint32_t dwords) {
  std::vector<uint32_t> words = WordsOf(code, dwords);
  for (const std::vector<uint32_t>& fetches : FetchBlocks(words)) {
    std::vector<std::array<uint32_t, 3>> blanked;
    for (const uint32_t at : fetches) {
      blanked.push_back({words[at * 3] & 0xC00FFFFF, words[at * 3 + 1] & 0x80000000, 0});
    }
    std::sort(blanked.begin(), blanked.end());
    for (size_t i = 0; i < fetches.size(); ++i) {
      std::copy(blanked[i].begin(), blanked[i].end(), words.begin() + fetches[i] * 3);
    }
  }
  return words;
}

std::string KeyOf(std::vector<uint32_t> words) {
  for (uint32_t& word : words) {
    word = std::byteswap(word);
  }
  const std::string_view bytes(reinterpret_cast<const char*>(words.data()), words.size() * 4);
  return trg::update::Sha256Hex(bytes).substr(0, 16);
}

std::string MaskedKey(const uint8_t* code, uint32_t dwords) {
  return KeyOf(MaskedWords(code, dwords));
}

// Binding turns off outputs the pixel shader doesn't read, which leaves instructions writing nothing, so this one blanks
// exports as well. It's the fallback for a shader that doesn't match. Matches loose_code in pull_vertices.py.
std::string LooseKey(const uint8_t* code, uint32_t dwords) {
  std::vector<uint32_t> words = MaskedWords(code, dwords);
  for (const std::vector<Instruction>& block : ExecBlocks(words)) {
    for (const Instruction& instruction : block) {
      const uint32_t word = words[instruction.at * 3];
      if (!instruction.fetch && ((word & kAluExport) || !(word & kAluWriteMasks))) {
        std::fill_n(words.begin() + instruction.at * 3, 3, 0);
      }
    }
  }
  return KeyOf(std::move(words));
}

// Which of the fetched x, y, z, w, 0 or 1 each lane of input should hold, so that a translated shader, which swizzles
// input the way the shipped ucode did, ends up with what the live ucode's swizzle asks for. Binding rewrites that to suit
// the format, such as swapping red and blue. A lane nothing reads stays as fetched. A lane the shipped ucode fixed at 0 or
// 1 reads the next lane spare, as unfreeze_constants in pull_vertices.py expects.
uint32_t SelectFor(uint32_t shipped, uint32_t live) {
  std::array<uint32_t, 4> select = {0, 1, 2, 3};
  std::array<bool, 4> taken = {};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t from = (shipped >> (i * 3)) & 7;
    if (from < 4) {
      taken[from] = true;
    }
  }
  uint32_t spare = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t from = (shipped >> (i * 3)) & 7, to = (live >> (i * 3)) & 7;
    if (from == 4 || from == 5) {
      while (taken[spare]) {
        ++spare;
      }
      taken[spare] = true;
      select[spare] = to <= 5 ? to : from;
    } else if (from < 4 && to <= 5) {
      select[from] = to;
    }
  }
  return select[0] | select[1] << 3 | select[2] << 6 | select[3] << 9;
}

// What pullElement needs from each fetch, read from the live ucode. A mini fetch shares the last full fetch's slot and
// stride, even one in an earlier block, and binding can have moved a fetch, so an element finds its own by the register it fetches to. The
// manifest holds each fetch's address, shipped swizzle and that register.
std::vector<MetalPullElement> PullElements(const uint8_t* code, uint32_t dwords, const std::vector<uint32_t>& manifest) {
  struct Fetch {
    uint32_t at;
    uint32_t slot;
    uint32_t stride;
  };
  const std::vector<uint32_t> words = WordsOf(code, dwords);
  std::vector<Fetch> fetches;
  uint32_t slot = 0, stride = 0;
  for (const std::vector<uint32_t>& block : FetchBlocks(words)) {
    for (const uint32_t at : block) {
      const uint32_t d0 = words[at * 3], d1 = words[at * 3 + 1], d2 = words[at * 3 + 2];
      if (!((d1 >> 30) & 1)) {
        slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3);
        stride = d2 & 0xFF;
      }
      fetches.push_back({at, slot, stride});
    }
  }
  std::vector<MetalPullElement> elements;
  for (size_t entry = 0; entry + 2 < manifest.size(); entry += 3) {
    const uint32_t address = manifest[entry], shipped = manifest[entry + 1], dest = manifest[entry + 2];
    const Fetch* found = nullptr;
    for (const Fetch& fetch : fetches) {
      if (((words[fetch.at * 3] >> 12) & 0x3F) == dest && (!found || fetch.at == address)) {
        found = &fetch;
      }
    }
    if (!found) {
      return {};
    }
    const uint32_t d1 = words[found->at * 3 + 1], d2 = words[found->at * 3 + 2];
    bool reads = false;
    for (uint32_t i = 0; i < 4; ++i) {
      reads |= (((d1 & 0xFFF) >> (i * 3)) & 7) <= 5;
    }
    const uint32_t flags = ((d1 >> 12) & 7) | (uint32_t(reads) << 3);
    const int32_t exponent = (int32_t(d1 << 2) >> 26);
    const int32_t offset = int32_t(d2 << 1) >> 9;
    elements.push_back({found->slot | (found->stride << 8) | (((d1 >> 16) & 0x3F) << 16) | (flags << 24), offset,
                        float(std::ldexp(1.0, exponent)), SelectFor(shipped, d1 & 0xFFF)});
  }
  return elements;
}

// Live code of a shader the translated set doesn't have, in the logs folder, for working out where it comes from.
void KeepMissingShader(const std::string& name, const uint8_t* code, uint32_t dwords) {
  const std::filesystem::path folder = g_path.parent_path() / "missing-shaders";
  std::error_code ignored;
  std::filesystem::create_directories(folder, ignored);
  std::ofstream(folder / (name + ".bin"), std::ios::binary).write(reinterpret_cast<const char*>(code), std::streamsize(dwords) * 4);
}

int FindVertexShader(MetalDevice& device, const BakedDraw& draw, const Run& run) {
  const uint32_t address = draw.vertex_shader & kGuestMask;
  if (!draw.vertex_shader_dwords || draw.vertex_shader_dwords > kMaxShaderDwords ||
      uint64_t(address) + draw.vertex_shader_dwords * 4ull > kGuestBytes) {
    return -1;
  }
  // By content: a copy in the ring has a new address each time it's made, and binding rewrites the code at an address to
  // suit a new vertex format.
  const uint8_t* code = g_guest + address;
  if (run.vertex_code) {
    code = run.vertex_code->data();
  } else if (run.list_code) {
    const auto copied = run.list_code->find(uint64_t(draw.vertex_shader) << 32 | draw.vertex_shader_dwords);
    if (copied != run.list_code->end()) {
      code = copied->second.data();
      g_totals.vertex_code_changed += std::memcmp(code, g_guest + address, copied->second.size()) != 0;
    }
  }
  uint64_t key = kHashBasis ^ draw.vertex_shader_dwords;
  for (uint32_t i = 0; i + 1 < draw.vertex_shader_dwords; i += 2) {
    uint64_t pair;
    std::memcpy(&pair, code + size_t(i) * 4, sizeof(pair));
    key = (key ^ pair) * kHashPrime;
  }
  const auto found = g_vertex_shaders.find(key);
  if (found != g_vertex_shaders.end()) {
    return found->second;
  }
  int handle = -1;
  {
    std::string masked = MaskedKey(code, draw.vertex_shader_dwords);
    if (!g_manifest.contains(masked)) {
      const auto loose = g_loose_manifest.find(LooseKey(code, draw.vertex_shader_dwords));
      if (loose != g_loose_manifest.end()) {
        masked = loose->second;
      }
    }
    const auto manifest = g_manifest.find(masked);
    if (manifest == g_manifest.end()) {
      ++g_totals.shaders_missing;
      REXLOG_INFO("Native shadow: no vertex shader {} ({} dwords at {:08X})", masked, draw.vertex_shader_dwords, address);
      KeepMissingShader("vs_" + masked, code, draw.vertex_shader_dwords);
    } else {
      const std::vector<MetalPullElement> elements = PullElements(code, draw.vertex_shader_dwords, manifest->second);
      std::string error;
      std::string layout;
      for (const MetalPullElement& element : elements) {
        layout += fmt::format(" slot {} stride {} format {} flags {:X} offset {} select {:03o}", element.packed & 0xFF, (element.packed >> 8) & 0xFF,
                              (element.packed >> 16) & 0xFF, element.packed >> 24, element.offset, element.select);
      }
      REXLOG_INFO("Native shadow: vertex shader {} at {:08X} ({} dwords):{}", masked, address, draw.vertex_shader_dwords, layout);
      handle = device.AddVertexShader(g_shader_dir / ("vs_" + masked + ".metallib"), elements.data(), elements.size(), error);
      if (handle < 0) {
        ++g_totals.shaders_failed;
        REXLOG_ERROR("Native shadow: vertex shader {}: {}", masked, error);
      } else {
        ++g_totals.shaders_found;
        g_vertex_textures.resize(size_t(handle) + 1);
        g_vertex_textures[size_t(handle)] = g_vertex_texture_manifest[masked];
      }
    }
  }
  g_vertex_shaders[key] = handle;
  return handle;
}

const PixelShader& FindPixelShader(MetalDevice& device, const BakedDraw& draw) {
  const uint64_t key = uint64_t(draw.pixel_shader) << 32 | draw.pixel_shader_dwords;
  const auto found = g_pixel_shaders.find(key);
  if (found != g_pixel_shaders.end()) {
    return found->second;
  }
  PixelShader& shader = g_pixel_shaders[key];
  const uint32_t address = draw.pixel_shader & kGuestMask;
  if (draw.pixel_shader_dwords && draw.pixel_shader_dwords <= kMaxShaderDwords &&
      uint64_t(address) + draw.pixel_shader_dwords * 4ull <= kGuestBytes) {
    const std::string_view code(reinterpret_cast<const char*>(g_guest + address), size_t(draw.pixel_shader_dwords) * 4);
    const std::string name = trg::update::Sha256Hex(code).substr(0, 16);
    const auto manifest = g_pixel_manifest.find(name);
    if (manifest == g_pixel_manifest.end()) {
      ++g_totals.pixel_shaders_missing;
      REXLOG_INFO("Native shadow: no pixel shader {} ({} dwords at {:08X})", name, draw.pixel_shader_dwords, address);
      KeepMissingShader("ps_" + name, g_guest + address, draw.pixel_shader_dwords);
    } else {
      std::string error;
      shader.handle = device.AddPixelShader(g_shader_dir / ("ps_" + name + ".metallib"), error);
      shader.name = name;
      shader.slots = manifest->second;
      shader.reads_whole_picture = g_screen_shaders.contains(name);
      if (shader.handle < 0) {
        REXLOG_ERROR("Native shadow: pixel shader {}: {}", name, error);
      } else {
        ++g_totals.pixel_shaders_found;
      }
    }
  }
  return shader;
}

// Big-endian guest indices into the frame's index region, in the Metal order. Returns false if they don't fit.
bool CopyIndices(const BakedDraw& draw, uint32_t count, bool wide, uint8_t* frame, size_t& used, size_t& offset) {
  const uint32_t address = draw.index_buffer & kGuestMask;
  const size_t bytes = size_t(count) * (wide ? 4 : 2);
  offset = kConstantRegionBytes + ((used + 3) & ~size_t(3));
  if (offset + bytes > kFrameBytes || uint64_t(address) + bytes > kGuestBytes) {
    return false;
  }
  used = offset - kConstantRegionBytes + bytes;
  const uint8_t* source = g_guest + address;
  uint8_t* target = frame + offset;
  // All this game's index buffers are big-endian. Other orders are counted in the report in case that ever changes.
  if (wide) {
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t value;
      std::memcpy(&value, source + size_t(i) * 4, 4);
      value = std::byteswap(value);
      std::memcpy(target + size_t(i) * 4, &value, 4);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t value;
      std::memcpy(&value, source + size_t(i) * 2, 2);
      value = std::byteswap(value);
      std::memcpy(target + size_t(i) * 2, &value, 2);
    }
  }
  return true;
}

struct Indices {
  size_t offset = 0;
  uint32_t count = 0;
  bool wide = false;
};

uint32_t GuestIndex(const uint8_t* source, bool wide, uint32_t i) {
  if (wide) {
    uint32_t value;
    std::memcpy(&value, source + size_t(i) * 4, 4);
    return std::byteswap(value);
  }
  uint16_t value;
  std::memcpy(&value, source + size_t(i) * 2, 2);
  return std::byteswap(value);
}

// Metal draws neither fans nor quads, so these get triangles, and a draw with no indices of its own gets a run of them.
template <typename T>
void WriteIndices(T* out, uint32_t primitive, uint32_t count, const uint8_t* source, bool source_wide) {
  const auto index = [&](uint32_t i) { return T(source ? GuestIndex(source, source_wide, i) : i); };
  size_t at = 0;
  if (primitive == kPrimitiveTriangleFan) {
    for (uint32_t i = 2; i < count; ++i) {
      out[at++] = index(0);
      out[at++] = index(i - 1);
      out[at++] = index(i);
    }
  } else if (primitive == kPrimitiveQuadList) {
    for (uint32_t i = 0; i + 3 < count; i += 4) {
      out[at++] = index(i);
      out[at++] = index(i + 1);
      out[at++] = index(i + 2);
      out[at++] = index(i);
      out[at++] = index(i + 2);
      out[at++] = index(i + 3);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      out[at++] = index(i);
    }
  }
}

bool ExpandIndices(const BakedDraw& draw, uint32_t primitive, bool automatic, uint8_t* frame, size_t& used, Indices& out) {
  const uint32_t count = draw.initiator >> 16;
  const bool wide = !automatic && ((draw.initiator >> 11) & 1);
  out.wide = wide;
  out.count = primitive == kPrimitiveTriangleFan ? (count < 2 ? 0 : (count - 2) * 3)
              : primitive == kPrimitiveQuadList  ? count / 4 * 6
                                                 : count;
  const size_t element = wide ? 4 : 2;
  const size_t bytes = size_t(out.count) * element;
  out.offset = kConstantRegionBytes + ((used + 3) & ~size_t(3));
  const uint32_t address = draw.index_buffer & kGuestMask;
  if (out.offset + bytes > kFrameBytes || (!automatic && uint64_t(address) + size_t(count) * element > kGuestBytes)) {
    return false;
  }
  used = out.offset - kConstantRegionBytes + bytes;
  const uint8_t* source = automatic ? nullptr : g_guest + address;
  if (wide) {
    WriteIndices(reinterpret_cast<uint32_t*>(frame + out.offset), primitive, count, source, wide);
  } else {
    WriteIndices(reinterpret_cast<uint16_t*>(frame + out.offset), primitive, count, source, wide);
  }
  return true;
}

struct TargetKey {
  uint32_t color;
  uint32_t depth;
  uint32_t width;
  uint32_t height;
  int tile;
  bool float_color;
  uint32_t samples;
  auto operator<=>(const TargetKey&) const = default;
};
std::map<TargetKey, int> g_targets;

MetalViewport ViewportOf(const BakedDraw& draw) {
  const std::array<float, 6> v = std::bit_cast<std::array<float, 6>>(draw.viewport);
  const float width = std::abs(v[0]) * 2.0f, height = std::abs(v[2]) * 2.0f;
  // Depth comes out as offset + scale * z, which sky objects use to keep to the far end of the range.
  return {v[1] - width / 2.0f, v[3] - height / 2.0f, width, height, v[5], v[5] + v[4]};
}

// The half-size tint layer is the one surface drawn in each tile's place in turn. Sharing a target, the second tile lands
// on top of the first, so each has its own.
bool HalfSizeTiles(const Targets& targets) {
  return targets.width == kHalfPictureWidth && targets.height == kHalfTileHeight;
}

int TileOf(const BakedDraw& draw) {
  const uint32_t tiles = uint32_t(draw.mask);
  return tiles == kFirstTileMask ? 0 : tiles == kSecondTileMask ? 1 : -1;
}

int TargetOf(MetalDevice& device, const Run& run, int tile) {
  const Targets& targets = run.targets;
  const TargetKey key = {targets.color, targets.depth, targets.width, targets.height, HalfSizeTiles(targets) ? std::max(tile, 0) : -1, targets.float_color,
                         targets.samples};
  if (!key.width || !key.height || key.width > kMaxTargetSize || key.height > kMaxTargetSize) {
    return -1;
  }
  const auto found = g_targets.find(key);
  if (found != g_targets.end()) {
    return found->second;
  }
  // Only the scene's targets are scaled. Shadow maps, reflections, bloom and exposure keep the guest's size.
  const float scale = key.width == kScaledWidth && key.height == kScaledHeight ? g_render_scale : 1.0f;
  const int target = device.AddTarget(key.width, key.height, key.color != 0, key.float_color, key.samples, scale);
  REXLOG_INFO("Native shadow: target {}x{}, colour {:08X}, depth {:08X}, float {}, samples {}, scale {}", key.width, key.height, key.color,
              key.depth, key.float_color, key.samples, scale);
  g_targets[key] = target;
  if (target >= 0) {
    g_target_scales.resize(std::max(g_target_scales.size(), size_t(target) + 1), 1.0f);
    g_target_scales[size_t(target)] = scale;
  }
  return target;
}

float ScaleOf(int target) { return target >= 0 && size_t(target) < g_target_scales.size() ? g_target_scales[size_t(target)] : 1.0f; }

bool InsideTarget(const MetalViewport& viewport, const Targets& targets) {
  return viewport.width > 0 && viewport.height > 0 && viewport.x >= -1 && viewport.y >= -1 &&
         viewport.x + viewport.width <= float(targets.width) + 1 && viewport.y + viewport.height <= float(targets.height) + 1;
}

// What a grey picture has to leave out or it paints a solid sheet: without pixel shaders, a draw that writes no depth is
// either blended over what's there or writes no colour at all, as a stencil shadow does. The sky writes neither depth nor
// blends, so it stays.
bool WritesDepth(const BakedDraw& draw) {
  return (draw.depth_control & kDepthEnable) && (draw.depth_control & kDepthWrite);
}

// A draw with no colour or depth can still leave a mark. Tunnels keep the sea out by writing stencil.
bool WritesStencil(const BakedDraw& draw) {
  return (draw.depth_control & kStencilEnable) && (draw.depth_control & kStencilOperations) && (draw.stencil_front & kStencilWriteMask);
}

bool IsBlended(const BakedDraw& draw) {
  const uint32_t source_factor = draw.blend_control & kBlendFactorMask;
  const uint32_t destination_factor = (draw.blend_control >> kBlendDestinationShift) & kBlendFactorMask;
  return source_factor != kBlendFactorOne || destination_factor != kBlendFactorZero;
}

// An occlusion query draws in depth-only mode, whatever pixel shader and mask are left over from the draw before.
bool WritesNoColor(const BakedDraw& draw) {
  return (draw.color_mask & kColorMaskFirstTarget) == 0 || draw.edram_mode == kEdramDepthOnly;
}

// A depth test that passes on greater-or-equal means reversed depth, which clears to 0.
double ClearDepthFor(const BakedDraw& draw) {
  return ((draw.depth_control >> 4) & 7) >= 4 ? 0.0 : 1.0;
}

// Live switches for examining a running game, set from metal-debug.txt: skip_blended, ids (each draw flat, in a colour
// that gives its number), no_resolves, show=<target>, skip_ps=<address>, stop_ps=<address>, which ends the frame at that
// shader's first draw, alpha, which puts alpha in a capture's colours, mark_queries, which paints the draws of queries
// the two sides disagreed on, dump=<name>, which writes live-<name>.png and .txt beside it, report, which saves a
// problem report as ⌘⇧R does, and reload_stale, which reloads every texture whose memory has changed, reported or not.
// dump, report and reload_stale happen once and are taken out of the file.
bool g_debug_skip_blended = false;
bool g_debug_ids = false;
bool g_debug_reload_stale = false;  // Once, at the start of the next frame drawn.
bool g_debug_no_resolves = false;
int g_debug_show = -1;
std::set<uint32_t> g_debug_skip_ps;
uint32_t g_debug_stop_ps = 0;
// skip_range=<shader>:<first>-<last>: skips that pixel shader's draws numbered first to last in the frame, counting from 0
// in the order they come. The numbers hold from frame to frame while the view doesn't change, so a range can be halved
// live to find which of a shader's hundreds of draws is at fault. only_range=<shader>:<first>-<last> is the other way
// round: it skips all that shader's draws but those, which finds draws that overlap, where skipping some leaves the rest.
uint32_t g_debug_range_ps = 0, g_debug_range_first = 0, g_debug_range_last = 0, g_debug_range_seen = 0;
bool g_debug_range_keep = false;
bool g_debug_stopped = false;
bool g_debug_alpha = false;
bool g_debug_no_stencil = false;
bool g_debug_no_clears = false;
bool g_debug_mark_queries = false;
bool g_debug_vertex_constants = false;  // vconst: a capture also lists each draw's first 64 vertex constants.
std::set<uint32_t> g_debug_marked;  // Queries the two sides last disagreed on.
std::ofstream* g_dump = nullptr;
int g_debug_id = 0;
std::mutex g_report_mutex;
std::filesystem::path g_report_folder;  // A report asked for, saved from the next frame.
// The frame after a report's is saved again with the sea's surface left out, the 2026-11-03 suspect for the colour wash:
// the glitch rarely lasts long enough to test by hand, but it does last a frame. These addresses held across sessions.
constexpr std::array<uint32_t, 2> kWaterPixelShaders = {0x1B150780, 0x1B153400};
std::filesystem::path g_report_followup;  // Render thread only: the report whose water-free frame is next.
bool g_skip_water = false;                // Render thread only: this frame.
std::function<void()> g_report_handler;  // Saves a whole problem report, for the report word.

// Takes g_query_mutex. Files a query once both sides have answered, and gives up on the oldest if the game never asks.
void SettleQuery(std::map<uint64_t, QueryAnswers>::iterator entry, uint32_t query) {
  const QueryAnswers& answers = entry->second;
  if (answers.metal && answers.emulator) {
    QueryTotals& totals = g_query_totals;
    const uint64_t metal = *answers.metal, emulator = *answers.emulator;
    if (metal && emulator) {
      ++totals.both_some;
      totals.metal_samples += metal;
      totals.emulator_samples += emulator;
      const double ratio = double(metal) / double(emulator);
      ++totals.ratios[size_t(std::upper_bound(kQueryRatioBounds.begin(), kQueryRatioBounds.end(), ratio) - kQueryRatioBounds.begin())];
    } else if (!metal && !emulator) {
      ++totals.both_none;
    } else {
      ++(metal ? totals.metal_only : totals.emulator_only);
      totals.metal_only_sizes[metal >= kLargeQuerySamples ? 2 : metal >= kSmallQuerySamples ? 1 : 0] += metal != 0;
      // One object per entry, and the largest are the ones worth looking at. Most are a sample or two on a silhouette.
      const auto same = std::find_if(totals.disagreements.begin(), totals.disagreements.end(), [&](const QueryDisagreement& other) { return other.query == query; });
      const auto smallest = std::min_element(totals.disagreements.begin(), totals.disagreements.end(),
                                             [](const QueryDisagreement& a, const QueryDisagreement& b) { return a.metal < b.metal; });
      if (same != totals.disagreements.end()) {
        ++same->times;
        same->metal = std::max(same->metal, metal);
        same->least = std::min(same->least, metal);
      } else if (totals.disagreements.size() < kMaxQueryDisagreements) {
        totals.disagreements.push_back({query, metal, metal, uint32_t(emulator), 1});
      } else if (smallest->metal < metal) {
        *smallest = {query, metal, metal, uint32_t(emulator), 1};
      }
    }
    g_query_answers.erase(entry);
  }
  while (g_query_answers.size() > kMaxPendingQueries) {
    g_query_answers.erase(g_query_answers.begin());
    ++g_query_totals.unanswered;
  }
}

uint32_t LoadGuestWord(uint32_t address) {
  uint8_t* const base = g_virtual_guest.load(std::memory_order_relaxed);
  if (!base) {
    return 0;
  }
  uint32_t value;
  std::memcpy(&value, rex::memory::GuestPtr<const uint8_t*>(base, address), sizeof(value));
  return std::byteswap(value);
}

// Under g_pacing_mutex. Never goes back: a frame reports the fence its present writes next, which the idle release, at
// every fence issued so far, can be one behind.
// True when it moved the game's fence on.
bool SetFenceDone(uint32_t done) {
  static bool reported = false;
  static uint32_t last = 0;
  if (g_set_fence_done && (!reported || int32_t(done - last) > 0)) {
    g_set_fence_done(done);
    reported = true;
    last = done;
    return true;
  }
  return false;
}

// Under g_pacing_mutex. Tells the emulator how far Metal has got, as far as every earlier frame is done too.
void ReportFences() {
  uint32_t done = 0;
  bool any = false;
  while (!g_frame_fences.empty() && g_frame_fences.front().finished) {
    done = g_frame_fences.front().fence;
    any = true;
    g_frame_fences.pop_front();
  }
  if (any) {
    SetFenceDone(done);
  }
}

// Nothing of Metal's is pending, so every fence issued so far can pass. open_draws: the frame in progress already has
// draws, which Metal hasn't read yet.
void ReleaseIdleFences(bool open_draws) {
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  const uint32_t fence = device ? LoadGuestWord(device + kDeviceFence) : 0;
  std::lock_guard lock(g_pacing_mutex);
  if (g_frame_fences.empty() && fence && SetFenceDone(fence - 1)) {
    ++g_pacing_totals.idle_releases;
    g_pacing_totals.idle_releases_open += open_draws;
  }
}

// Under g_pacing_mutex. A frame handed over, or one with nothing for Metal to draw, which is done at once.
void NoteFrameFence(uint32_t fence, bool finished) {
  // The device's counter is the next fence. Present writes that one right after the swap, before the next frame draws
  // anything, and its next present waits for it, so it passes once this frame is done, as it would on the Xenos.
  if (!g_set_fence_done || !fence) {
    return;
  }
  g_frame_fences.push_back({fence, finished});
  ReportFences();
}

// Any thread. Metal has finished a frame, which is the moment it could report that frame's fence to the game.
void OnFrameFinished(uint32_t fence, Clock::time_point handed_over) {
  const uint64_t metal_ns = Since(handed_over);
  g_finished_fence.store(fence, std::memory_order_relaxed);
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  const uint32_t block = device ? LoadGuestWord(device + kDeviceWriteback) : 0;
  const int32_t lead = block ? int32_t(LoadGuestWord(block) - fence) : 0;
  {
    std::lock_guard lock(g_pacing_mutex);
    PacingTotals& totals = g_pacing_totals;
    ++totals.frames;
    totals.metal_ns += metal_ns;
    totals.worst_metal_ns = std::max(totals.worst_metal_ns, metal_ns);
    totals.emulator_lead += lead;
    ++g_frames_finished;
    for (FrameFence& frame : g_frame_fences) {
      if (!frame.finished && frame.fence == fence) {
        frame.finished = true;
        break;
      }
    }
    ReportFences();
  }
  g_frame_finished.notify_all();
}

// Writes a query's answer where the GPU would: per tile, an end block of counters followed by a begin block, little-endian.
// The game sums end minus begin over the tiles, and takes the end block as not ready while its first four words still
// hold D3D's marker, so the first word goes last.
void AnswerQuery(uint32_t query, uint64_t samples) {
  const uint32_t tiles = std::min(LoadGuestWord(query + kQueryTileCount), kMaxQueryTiles);
  const uint32_t count = uint32_t(std::min<uint64_t>(samples, UINT32_MAX));
  for (uint32_t tile = 0; tile < tiles; ++tile) {
    const uint32_t block = LoadGuestWord(query + kQueryBlocks + tile * 4);
    const uint32_t physical = (block & kGuestMask) + ((((block >> 20) & 0xFFF) + 512) & 0x1000);
    if (!block || uint64_t(physical) + kQueryBlockWords * 4 > kGuestBytes) {
      continue;
    }
    uint32_t* words = reinterpret_cast<uint32_t*>(g_guest + physical);
    const uint32_t passed = tile == 0 ? count : 0;
    std::fill(words + 1, words + kQueryBlockWords, 0u);
    words[kQueryZPass] = passed;
    __atomic_store_n(&words[kQueryTotal], passed, __ATOMIC_RELEASE);
  }
}

void OnMetalSamples(const std::vector<QuerySlot>& slots, const uint64_t* samples) {
  const bool answer = REXCVAR_GET(crackdown_native_only);
  std::lock_guard lock(g_query_mutex);
  for (size_t i = 0; i < slots.size(); ++i) {
    const uint64_t counted = slots[i].area == 1.0f ? samples[i] : uint64_t(std::llround(double(samples[i]) / slots[i].area));
    if (answer) {
      AnswerQuery(slots[i].query, counted);
    }
    const auto entry = g_query_answers.try_emplace(slots[i].id).first;
    entry->second.metal = counted;
    SettleQuery(entry, slots[i].query);
  }
}

// The mean of a histogram with the brightest few left out, which is near enough what the game's exposure works from.
double ExposureOf(const std::array<uint64_t, 256>& histogram) {
  uint64_t skipped = 0, samples = 0, sum = 0;
  for (int value = 255; value >= 0; --value) {
    const uint64_t skip = std::min(histogram[size_t(value)], kExposureSkippedSamples - skipped);
    skipped += skip;
    samples += histogram[size_t(value)] - skip;
    sum += (histogram[size_t(value)] - skip) * uint64_t(value);
  }
  return samples ? double(sum) / double(samples) : 0.0;
}

// Where a pixel of a resolved texture sits, from the start of its memory.
size_t GuestPixelOffset(uint32_t x, uint32_t y, uint32_t pitch, bool tiled) {
  return tiled ? size_t(texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, kPixelBytesLog2)) : (size_t(y) * pitch + x) * 4;
}

// Takes g_readback_mutex. Lays Metal's copy beside what the game read, if both are here.
void CompareReadback(const ResolveKey& key, uint32_t width, uint32_t height) {
  const auto metal = g_metal_pixels.find(key);
  const auto guest = g_guest_pixels.find(key);
  if (metal == g_metal_pixels.end() || guest == g_guest_pixels.end()) {
    return;
  }
  std::array<std::array<uint64_t, 256>, 2> byte_zero{};
  std::array<uint64_t, 4> guest_sum{}, metal_sum{}, difference{}, far{};
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t at = GuestPixelOffset(x, y, guest->second.pitch, guest->second.tiled);
      if (at + 4 > guest->second.bytes.size()) {
        continue;
      }
      const uint8_t* guest_pixel = guest->second.bytes.data() + at;
      const uint8_t* metal_pixel = metal->second.data() + (size_t(y) * width + x) * 4;
      ++byte_zero[0][guest_pixel[0]];
      ++byte_zero[1][metal_pixel[3]];
      for (size_t i = 0; i < 4; ++i) {
        const int apart = std::abs(int(guest_pixel[i]) - int(metal_pixel[3 - i]));
        guest_sum[i] += guest_pixel[i];
        metal_sum[i] += metal_pixel[3 - i];
        difference[i] += uint64_t(apart);
        far[i] += apart > kFarPixel;
      }
    }
  }
  const double pixels = double(width) * height;
  ReadbackTotals& totals = g_readback_totals;
  ++totals.compared;
  for (size_t i = 0; i < 4; ++i) {
    totals.guest_mean[i] += double(guest_sum[i]) / pixels;
    totals.metal_mean[i] += double(metal_sum[i]) / pixels;
    totals.difference[i] += double(difference[i]) / pixels;
    totals.far[i] += double(far[i]) / pixels;
  }
  totals.guest_exposure += ExposureOf(byte_zero[0]);
  totals.metal_exposure += ExposureOf(byte_zero[1]);
  g_guest_pixels.erase(guest);
}

void OnMetalPixels(const std::vector<ReadbackSlot>& slots, const uint8_t* pixels) {
  std::lock_guard lock(g_readback_mutex);
  for (const ReadbackSlot& slot : slots) {
    const uint8_t* start = pixels + slot.offset;
    g_metal_pixels[slot.key].assign(start, start + size_t(slot.width) * slot.height * 4);
    CompareReadback(slot.key, slot.width, slot.height);
    if (g_metal_pixels.size() > kKeptReadbacks) {
      g_metal_pixels.erase(g_metal_pixels.begin());
    }
  }
}

void CopyResolve(const Run& run, MetalDevice& device, int target, int resolve, bool depth) {
  const ResolveCopy& copy = *run.resolve;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, run.dest.data(), sizeof(fetch));
  int stack = 0;
  uint32_t slice = 0;
  if (depth && g_textures->FindStackSlice(run.dest.data(), stack, slice)) {
    device.CopyDepthToStack(target, stack, slice);
  }
  // A tile's resolve names its own texture. The picture of them both keeps it at its real height.
  const bool one_tile = TextureCache::HoldsOneTile(run.dest.data());
  MetalCopy out;
  out.target = target;
  out.depth = depth;
  out.resolve = resolve;
  out.level = copy.level;
  out.left = copy.has_rect ? copy.rect[0] : 0;
  out.top = copy.has_rect ? copy.rect[1] : 0;
  out.right = copy.has_rect ? copy.rect[2] : int32_t(fetch.size_2d.width) + 1;
  out.bottom = copy.has_rect ? copy.rect[3] : int32_t(fetch.size_2d.height) + 1;
  out.dest_x = copy.dest_x;
  out.dest_y = one_tile ? out.top : copy.dest_y;
  device.CopyTarget(out);
  if (one_tile) {
    out.resolve = g_textures->ResolveInto(run.dest.data(), out.top >= kSecondTileTop ? 1 : 0, ScaleOf(target));
    out.dest_y = copy.dest_y;
    if (out.resolve >= 0) {
      device.CopyTarget(out);
    }
  }
  ++g_totals.native_resolves;
}

// A resolve's clear reaches only what its surface holds. The game clears a tile and then copies the next, and they share a
// target here, so the rows the first tile overlaps the second with are left for the second.
void ClearAfterResolve(const Run& run, MetalDevice& device, int target) {
  const ResolveCopy& copy = *run.resolve;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, run.dest.data(), sizeof(fetch));
  const int32_t surface_bottom = int32_t(run.targets.surface_height) == kFirstTileBottom ? kSecondTileTop : int32_t(run.targets.surface_height);
  MetalRectClear clear;
  clear.target = target;
  clear.color = (copy.flags & kResolveClearColor) != 0;
  clear.depth = (copy.flags & kResolveClearDepth) != 0;
  std::copy(copy.clear_color.begin(), copy.clear_color.end(), clear.color_value);
  clear.depth_value = copy.clear_depth;
  clear.stencil_value = copy.clear_stencil & 0xFF;
  if (copy.has_rect) {
    clear.left = copy.rect[0];
    clear.top = copy.rect[1];
    clear.right = copy.rect[2];
    clear.bottom = copy.rect[3] == kFirstTileBottom ? surface_bottom : copy.rect[3];
  } else {
    clear.right = copy.texture ? int32_t(fetch.size_2d.width) + 1 : int32_t(run.targets.width);
    clear.bottom = copy.texture ? std::min(int32_t(fetch.size_2d.height) + 1, surface_bottom) : surface_bottom;
  }
  device.ClearRect(clear);
}

// A target nothing has drawn to this frame holds nothing worth copying, so the texture goes back to what guest memory has.
void ApplyResolve(const Run& run, MetalDevice& device, const std::set<int>& drawn, int& current_target, std::vector<ReadbackSlot>& readbacks) {
  const ResolveCopy& copy = *run.resolve;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, run.dest.data(), sizeof(fetch));
  const uint32_t source = copy.flags & kResolveSourceMask;
  const bool depth = source == kResolveDepth;
  const int tile = HalfSizeTiles(run.targets) ? (copy.has_rect && copy.rect[3] <= kHalfSecondTileHeight ? 1 : 0) : -1;
  const auto found = g_targets.find({run.targets.color, run.targets.depth, run.targets.width, run.targets.height, tile, run.targets.float_color,
                                     run.targets.samples});
  const int target = found == g_targets.end() ? -1 : found->second;
  const int resolve = copy.texture && !g_debug_no_resolves && target >= 0 && drawn.contains(target) && (depth || (source == 0 && run.targets.color))
                          ? g_textures->ResolveInto(run.dest.data(), -1, ScaleOf(target))
                          : -1;
  if (g_dump) {
    *g_dump << fmt::format("R {} flags {:X} rect {} {} {} {} {} to {} {} level {} fetch f{} {}x{} {:05X} swizzle {:03X} type {} dim {} mips {} {} clear {} {} tgt {} {:08X} {:08X} {}\n", g_debug_id, copy.flags, copy.has_rect,
                           copy.rect[0], copy.rect[1], copy.rect[2], copy.rect[3], copy.dest_x, copy.dest_y, copy.level, uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                           uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.base_address), uint32_t(fetch.swizzle), uint32_t(fetch.type), uint32_t(fetch.dimension), uint32_t(fetch.mip_address),
                           uint32_t(fetch.mip_max_level), copy.clear_depth, copy.clear_stencil, target, run.targets.color, run.targets.depth, resolve);
  }
  // A target with nothing drawn on it yet this frame is cleared by its first draw.
  const bool clears = (copy.flags & (kResolveClearColor | kResolveClearDepth)) != 0 && target >= 0 && drawn.contains(target);
  if (resolve >= 0) {
    CopyResolve(run, device, target, resolve, depth);
    if (!depth && uint32_t(fetch.base_address) == g_front_page) {
      g_next_front_target = target;
    }
    bool read = false;
    {
      std::lock_guard lock(g_readback_mutex);
      read = !depth && g_read_textures.contains({uint32_t(fetch.base_address), uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1});
    }
    const size_t offset = read ? device.ReadResolve(resolve) : SIZE_MAX;
    if (offset != SIZE_MAX) {
      readbacks.push_back({{run.resolve_number, uint32_t(fetch.base_address)}, offset, uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1});
    }
  } else if (copy.texture) {
    ++g_totals.missed_resolves;
    g_textures->ForgetResolve(run.dest.data());
  }
  if (clears && !g_debug_no_clears) {
    ClearAfterResolve(run, device, target);
  }
  if (resolve >= 0 || clears) {
    current_target = -1;
  }
}

// With the emulator drawing nothing, its swap still reads the front buffer from guest memory, applies the gamma ramp,
// draws the overlays and presents. So Metal's copy goes there, as the Xenos would have laid it out.
void WriteFrontBuffer(const FetchConstant& front_buffer, MetalDevice& device, FrameAnswers& answers) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, front_buffer.data(), sizeof(fetch));
  const xenos::Endian endian = xenos::Endian(uint32_t(fetch.endianness));
  if (!g_textures->HasResolve(front_buffer.data()) || (endian != xenos::Endian::kNone && endian != xenos::Endian::k8in32)) {
    ++g_totals.front_buffers_missed;
    return;
  }
  const int resolve = g_textures->ResolveInto(front_buffer.data());
  const uint32_t address = uint32_t(fetch.base_address) << 12;
  const uint32_t pitch = uint32_t(fetch.pitch) * 32;
  const uint32_t height = uint32_t(fetch.size_2d.height) + 1;
  const uint32_t bytes = pitch * (fetch.tiled ? rex::align(height, 32u) : height) * 4;
  const size_t at = (address & kGuestMask) + rex::memory::detail::PhysicalHostOffset(address);
  if (resolve < 0 || !pitch || at + bytes > kGuestBytes ||
      !device.WriteResolveToGuest(resolve, at, pitch, fetch.tiled, endian == xenos::Endian::k8in32)) {
    ++g_totals.front_buffers_missed;
    return;
  }
  answers.front_buffer = address;
  answers.front_buffer_bytes = bytes;
}

// The front buffer's own resolve, at whatever size Metal drew it, for the swap to present.
std::array<float, 16> Inverse(const std::array<float, 16>& m) {
  // Cofactors, rows of m as rows.
  std::array<double, 16> inv;
  const auto a = [&](int r, int c) { return double(m[size_t(r * 4 + c)]); };
  inv[0] = a(1,1)*a(2,2)*a(3,3) - a(1,1)*a(2,3)*a(3,2) - a(2,1)*a(1,2)*a(3,3) + a(2,1)*a(1,3)*a(3,2) + a(3,1)*a(1,2)*a(2,3) - a(3,1)*a(1,3)*a(2,2);
  inv[4] = -a(1,0)*a(2,2)*a(3,3) + a(1,0)*a(2,3)*a(3,2) + a(2,0)*a(1,2)*a(3,3) - a(2,0)*a(1,3)*a(3,2) - a(3,0)*a(1,2)*a(2,3) + a(3,0)*a(1,3)*a(2,2);
  inv[8] = a(1,0)*a(2,1)*a(3,3) - a(1,0)*a(2,3)*a(3,1) - a(2,0)*a(1,1)*a(3,3) + a(2,0)*a(1,3)*a(3,1) + a(3,0)*a(1,1)*a(2,3) - a(3,0)*a(1,3)*a(2,1);
  inv[12] = -a(1,0)*a(2,1)*a(3,2) + a(1,0)*a(2,2)*a(3,1) + a(2,0)*a(1,1)*a(3,2) - a(2,0)*a(1,2)*a(3,1) - a(3,0)*a(1,1)*a(2,2) + a(3,0)*a(1,2)*a(2,1);
  inv[1] = -a(0,1)*a(2,2)*a(3,3) + a(0,1)*a(2,3)*a(3,2) + a(2,1)*a(0,2)*a(3,3) - a(2,1)*a(0,3)*a(3,2) - a(3,1)*a(0,2)*a(2,3) + a(3,1)*a(0,3)*a(2,2);
  inv[5] = a(0,0)*a(2,2)*a(3,3) - a(0,0)*a(2,3)*a(3,2) - a(2,0)*a(0,2)*a(3,3) + a(2,0)*a(0,3)*a(3,2) + a(3,0)*a(0,2)*a(2,3) - a(3,0)*a(0,3)*a(2,2);
  inv[9] = -a(0,0)*a(2,1)*a(3,3) + a(0,0)*a(2,3)*a(3,1) + a(2,0)*a(0,1)*a(3,3) - a(2,0)*a(0,3)*a(3,1) - a(3,0)*a(0,1)*a(2,3) + a(3,0)*a(0,3)*a(2,1);
  inv[13] = a(0,0)*a(2,1)*a(3,2) - a(0,0)*a(2,2)*a(3,1) - a(2,0)*a(0,1)*a(3,2) + a(2,0)*a(0,2)*a(3,1) + a(3,0)*a(0,1)*a(2,2) - a(3,0)*a(0,2)*a(2,1);
  inv[2] = a(0,1)*a(1,2)*a(3,3) - a(0,1)*a(1,3)*a(3,2) - a(1,1)*a(0,2)*a(3,3) + a(1,1)*a(0,3)*a(3,2) + a(3,1)*a(0,2)*a(1,3) - a(3,1)*a(0,3)*a(1,2);
  inv[6] = -a(0,0)*a(1,2)*a(3,3) + a(0,0)*a(1,3)*a(3,2) + a(1,0)*a(0,2)*a(3,3) - a(1,0)*a(0,3)*a(3,2) - a(3,0)*a(0,2)*a(1,3) + a(3,0)*a(0,3)*a(1,2);
  inv[10] = a(0,0)*a(1,1)*a(3,3) - a(0,0)*a(1,3)*a(3,1) - a(1,0)*a(0,1)*a(3,3) + a(1,0)*a(0,3)*a(3,1) + a(3,0)*a(0,1)*a(1,3) - a(3,0)*a(0,3)*a(1,1);
  inv[14] = -a(0,0)*a(1,1)*a(3,2) + a(0,0)*a(1,2)*a(3,1) + a(1,0)*a(0,1)*a(3,2) - a(1,0)*a(0,2)*a(3,1) - a(3,0)*a(0,1)*a(1,2) + a(3,0)*a(0,2)*a(1,1);
  inv[3] = -a(0,1)*a(1,2)*a(2,3) + a(0,1)*a(1,3)*a(2,2) + a(1,1)*a(0,2)*a(2,3) - a(1,1)*a(0,3)*a(2,2) - a(2,1)*a(0,2)*a(1,3) + a(2,1)*a(0,3)*a(1,2);
  inv[7] = a(0,0)*a(1,2)*a(2,3) - a(0,0)*a(1,3)*a(2,2) - a(1,0)*a(0,2)*a(2,3) + a(1,0)*a(0,3)*a(2,2) + a(2,0)*a(0,2)*a(1,3) - a(2,0)*a(0,3)*a(1,2);
  inv[11] = -a(0,0)*a(1,1)*a(2,3) + a(0,0)*a(1,3)*a(2,1) + a(1,0)*a(0,1)*a(2,3) - a(1,0)*a(0,3)*a(2,1) - a(2,0)*a(0,1)*a(1,3) + a(2,0)*a(0,3)*a(1,1);
  inv[15] = a(0,0)*a(1,1)*a(2,2) - a(0,0)*a(1,2)*a(2,1) - a(1,0)*a(0,1)*a(2,2) + a(1,0)*a(0,2)*a(2,1) + a(2,0)*a(0,1)*a(1,2) - a(2,0)*a(0,2)*a(1,1);
  const double det = a(0,0) * inv[0] + a(0,1) * inv[4] + a(0,2) * inv[8] + a(0,3) * inv[12];
  std::array<float, 16> out{};
  if (std::abs(det) > 1e-12) {
    for (size_t i = 0; i < 16; ++i) {
      out[i] = float(inv[i] / det);
    }
  }
  return out;
}

// The camera, from the view-projection most of the frame's world draws share, which the game keeps in c0 to c3. Its
// projection is reversed and infinite: row 2 is (0, 0, 0, near).
std::optional<MetalCamera> CameraOf(const std::array<float, 16>& view_projection, float delta_time) {
  const auto length = [&](int row) {
    return std::sqrt(view_projection[size_t(row * 4)] * view_projection[size_t(row * 4)] +
                     view_projection[size_t(row * 4 + 1)] * view_projection[size_t(row * 4 + 1)] +
                     view_projection[size_t(row * 4 + 2)] * view_projection[size_t(row * 4 + 2)]);
  };
  const float x_scale = length(0), y_scale = length(1), near_plane = view_projection[11];
  if (x_scale <= 0.0f || y_scale <= 0.0f || near_plane <= 0.0f || length(2) > 1e-4f) {
    return std::nullopt;
  }
  MetalCamera camera;
  camera.view_projection = view_projection;
  camera.inverse_view_projection = Inverse(view_projection);
  camera.field_of_view = float(2.0 * std::atan(1.0 / y_scale) * 180.0 / 3.14159265358979);
  camera.aspect = y_scale / x_scale;
  camera.near_plane = near_plane;
  camera.far_plane = kFrameGenerationFarPlane;
  camera.delta_time = delta_time;
  camera.previous_view_projection = g_last_view_projection ? *g_last_view_projection : view_projection;
  camera.reset = !g_last_view_projection;
  return camera;
}

void CopyFrontBuffer(const FetchConstant& front_buffer, MetalDevice& device, const std::optional<std::array<float, 16>>& view_projection) {
  const int resolve = g_textures->HasResolve(front_buffer.data()) ? g_textures->ResolveInto(front_buffer.data()) : -1;
  const uint32_t scale = uint32_t(std::clamp(REXCVAR_GET(crackdown_metalfx_upscale), 1, kMaxUpscale));
  if (resolve < 0) {
    ++g_totals.front_buffers_missed;
    return;
  }
  if (g_frame_generation) {
    const Clock::time_point now = Clock::now();
    const float delta_time = g_last_frame_time.time_since_epoch().count()
                                 ? std::clamp(std::chrono::duration<float>(now - g_last_frame_time).count(), 1.0f / 120.0f, 0.1f)
                                 : 1.0f / 30.0f;
    g_last_frame_time = now;
    const std::optional<MetalCamera> camera = view_projection ? CameraOf(*view_projection, delta_time) : std::nullopt;
    if (camera && device.InterpolatePicture(resolve, *camera, scale)) {
      ++g_totals.frames_generated;
    }
    g_last_view_projection = camera ? view_projection : std::nullopt;
  }
  if (!device.CopyPicture(resolve, scale)) {
    ++g_totals.front_buffers_missed;
  }
}

// The view-projection most of a frame's draws to the front buffer's size used, by how many.
using ViewProjections = std::map<std::array<float, 16>, uint32_t>;

std::optional<std::array<float, 16>> BusiestViewProjection(const ViewProjections& counts) {
  const auto busiest = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  if (busiest == counts.end()) {
    return std::nullopt;
  }
  return busiest->first;
}

// Frame generation's motion for things that move on their own. A scene draw is known again next frame by its geometry
// and how many times that geometry has been drawn so far this frame, which keeps a crowd's shared meshes apart as long as
// they're drawn in the same order. One whose vertex constants past the camera's (c0–c8) changed moves on its own.
struct MotionSeen {
  uint64_t constants;
  size_t offset;  // Of its vertex constants in its frame's memory.
};
std::unordered_map<uint64_t, MotionSeen> g_motion_now;
std::unordered_map<uint64_t, MotionSeen> g_motion_before;
std::unordered_map<uint64_t, uint32_t> g_motion_repeats;
constexpr uint32_t kFirstObjectConstantDword = 9 * 4;
constexpr uint32_t kVertexConstantDwords = uint32_t(kPixelConstantsOffset / 4);

uint64_t Mix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
  return hash * 0xBF58476D1CE4E5B9ull;
}

void NoteMotionDraw(MetalDevice& device, const BakedDraw& draw, const MetalDraw& out) {
  uint64_t key = Mix(Mix(Mix(Mix(0, draw.vertex_shader), draw.index_buffer), draw.index_offset), draw.initiator);
  key = Mix(key, g_motion_repeats[key]++);
  std::array<uint64_t, 4> lanes = {1, 2, 3, 4};
  for (uint32_t i = kFirstObjectConstantDword; i + 8 <= kVertexConstantDwords; i += 8) {
    for (uint32_t lane = 0; lane < 4; ++lane) {
      lanes[lane] = Mix(lanes[lane], uint64_t(g_constants[i + lane * 2]) | uint64_t(g_constants[i + lane * 2 + 1]) << 32);
    }
  }
  const uint64_t constants = Mix(Mix(Mix(lanes[0], lanes[1]), lanes[2]), lanes[3]);
  g_motion_now[key] = {constants, out.vertex_constants};
  const auto before = g_motion_before.find(key);
  if (before != g_motion_before.end() && before->second.constants != constants) {
    device.AddMotionDraw(out, before->second.offset);
    ++g_totals.motion_draws;
  }
}

void Encode(const Frame& frame, MetalDevice& device, uint8_t* memory, FrameAnswers& answers) {
  std::vector<QuerySlot>& query_slots = answers.queries;
  if (g_frame_generation) {
    g_motion_before.swap(g_motion_now);
    g_motion_now.clear();
    g_motion_repeats.clear();
  }
  // Frame generation: the camera, and the front buffer's scene as it stands before the HUD, which is the first draw on
  // it after the 3D ones that tests no depth.
  ViewProjections view_projections;
  bool scene_drawn = false, snapshot_taken = false;
  if (frame.front_buffer) {
    xenos::xe_gpu_texture_fetch_t front;
    std::memcpy(&front, frame.front_buffer->data(), sizeof(front));
    g_front_page = uint32_t(front.base_address);
    g_front_width = uint32_t(front.size_2d.width) + 1;
    g_front_height = uint32_t(front.size_2d.height) + 1;
  }
  g_front_target = g_next_front_target;
  g_debug_id = 0;
  g_debug_range_seen = 0;
  g_debug_stopped = false;
  device.CaptureAlpha(g_debug_alpha);
  g_textures->BeginFrame();
  if (std::exchange(g_debug_reload_stale, false)) {
    const size_t reloaded = g_textures->ReloadStale();
    REXLOG_INFO("Native shadow: reload_stale reloaded {} textures whose memory had changed", reloaded);
  }
  if (REXCVAR_GET(crackdown_texture_sweep)) {
    g_textures->Sweep();
  }
  size_t used = 0, indices_used = 0;
  int current_target = -1;
  std::unordered_map<int, uint32_t> color_draws;
  std::set<int> cleared;
  int last_color_target = -1;
  bool opening_pass = false;
  int open_query = -1;
  auto next_query = frame.queries.begin();
  const auto apply_queries = [&](size_t before_run) {
    for (; next_query != frame.queries.end() && next_query->run <= before_run; ++next_query) {
      open_query = -1;
      if (next_query->begin && query_slots.size() < MetalDevice::kMaxQueries) {
        open_query = int(query_slots.size());
        query_slots.push_back({next_query->id, next_query->query});
      } else if (next_query->begin) {
        ++g_totals.queries_no_room;
      }
      if (g_dump) {
        *g_dump << fmt::format("Q {} {:08X} slot {}\n", next_query->begin ? "begin" : "end", next_query->query, open_query);
      }
    }
  };
  for (size_t run_number = 0; run_number < frame.runs.size(); ++run_number) {
    const Run& run = frame.runs[run_number];
    apply_queries(run_number);
    if (run.resolve) {
      if (!g_debug_stopped) {
        ApplyResolve(run, device, cleared, current_target, answers.readbacks);
      }
      continue;
    }
    std::memcpy(g_constants.data(), run.constants.data(), sizeof(run.constants));
    uint32_t run_cleared = run.clear;
    for (const BakedList& list : *run.lists) {
      for (const BakedDraw& draw : list.draws) {
        // Constant writes aren't predicated, so they apply either way.
        ApplyWrites(list, draw);
        if (!DrawPasses(draw, run.pass_select)) {
          ++g_totals.skipped_draws;
          continue;
        }
        ++g_totals.draws;
        opening_pass |= OpeningPassTarget(run.targets);
        ApplyLiterals(draw);
        // Culling both faces leaves nothing to draw, and Metal has no way to say so.
        if ((draw.mode_control & kCullBothFaces) == kCullBothFaces) {
          continue;
        }
        const bool alpha_test = (draw.color_control & kAlphaTestEnable) != 0;
        g_totals.alpha_tested_draws += alpha_test;
        const PixelShader& found_pixel = run.targets.color || alpha_test ? FindPixelShader(device, draw) : g_no_pixel_shader;
        if (g_debug_skip_blended && run.targets.color && IsBlended(draw)) {
          continue;
        }
        if (g_debug_skip_ps.contains(draw.pixel_shader) ||
            (g_skip_water && std::ranges::find(kWaterPixelShaders, draw.pixel_shader) != kWaterPixelShaders.end())) {
          continue;
        }
        if (g_debug_range_ps && draw.pixel_shader == g_debug_range_ps) {
          const uint32_t number = g_debug_range_seen++;
          if ((number >= g_debug_range_first && number <= g_debug_range_last) != g_debug_range_keep) {
            continue;
          }
        }
        g_debug_stopped |= g_debug_stop_ps && draw.pixel_shader == g_debug_stop_ps;
        if (g_debug_stopped) {
          continue;
        }
        // A query's quad only tests depth, so it counts however it's dressed from the draw before.
        const bool counting = open_query >= 0 && WritesNoColor(draw);
        g_quads_counted += counting;
        const PixelShader& pixel = g_debug_ids || counting ? g_no_pixel_shader : found_pixel;
        // A blend over what's on screen is only as good as what it reads, and without these it washes out the frame.
        if (pixel.handle >= 0 && IsBlended(draw)) {
          const uint32_t* blended_fetch = list.fetch_data.data() + size_t(draw.fetch_constants) * 192;
          bool needs_resolve = false;
          for (const PixelSlot& slot : pixel.slots) {
            const uint32_t* slot_fetch = blended_fetch + size_t(slot.slot) * kFetchConstantDwords;
            needs_resolve |= TextureCache::NeedsNativeResolve(slot_fetch) && !g_textures->HasResolve(slot_fetch);
          }
          if (needs_resolve) {
            ++g_totals.needs_resolve_draws;
            continue;
          }
        }
        if (!counting && !WritesDepth(draw) && !WritesStencil(draw) && run.targets.color) {
          if (WritesNoColor(draw)) {
            ++g_totals.no_color_draws;
            if (g_dump) {
              *g_dump << fmt::format("N vs {} ps {:08X} {} prim {} n {} edram {} mode {:X} depth {:08X} st {:06X} back {:06X} mask {:X}\n", draw.vertex_shader_dwords, draw.pixel_shader, draw.pixel_shader_dwords,
                                     draw.initiator & 0x3F, draw.initiator >> 16, draw.edram_mode, draw.mode_control, draw.depth_control, draw.stencil_front, draw.stencil_back, draw.color_mask);
            }
            continue;
          }
          if (IsBlended(draw) && pixel.handle < 0 && !g_debug_ids) {
            ++g_totals.transparent_draws;
            continue;
          }
        }
        const uint32_t primitive = draw.initiator & 0x3F;
        const uint32_t source = (draw.initiator >> 6) & 3;
        const bool automatic = source == kSourceAutoIndex;
        if (primitive != kPrimitiveTriangleList && primitive != kPrimitiveTriangleStrip && primitive != kPrimitiveTriangleFan &&
            primitive != kPrimitiveQuadList) {
          ++g_totals.other_primitive_draws;
          continue;
        }
        const int shader = FindVertexShader(device, draw, run);
        if (shader < 0) {
          ++g_totals.no_shader_draws;
          continue;
        }
        const uint32_t index_count = draw.initiator >> 16;
        const bool wide = (draw.initiator >> 11) & 1;
        g_totals.other_index_order_draws += !automatic && (draw.index_info >> kIndexEndianShift) != (wide ? kIndexEndian32 : kIndexEndian16);
        Indices indices;
        indices.count = index_count;
        indices.wide = wide;
        const Clock::time_point copy_start = Clock::now();
        const bool copied = automatic || primitive == kPrimitiveTriangleFan || primitive == kPrimitiveQuadList
                                ? ExpandIndices(draw, primitive, automatic, memory, indices_used, indices)
                                : CopyIndices(draw, index_count, wide, memory, indices_used, indices.offset);
        g_totals.index_copy_ns += Since(copy_start);
        if (!copied || used + kDrawBytes > kConstantRegionBytes) {
          ++g_totals.no_room;
          continue;
        }
        // With the viewport turned off, vertices come in pixels, whose centres are on whole numbers.
        const bool screen_space = !(draw.viewport_control & kViewportScaleX);
        const MetalViewport viewport =
            screen_space ? MetalViewport{0.0f, 0.0f, float(run.targets.width), float(run.targets.height), 0.0f, 1.0f} : ViewportOf(draw);
        const int tile = TileOf(draw);
        const int target = InsideTarget(viewport, run.targets) ? TargetOf(device, run, tile) : -1;
        if (target < 0) {
          ++g_totals.no_target_draws;
          continue;
        }
        if (g_frame_generation) {
          const bool depth_tested = (draw.depth_control & kDepthEnable) != 0;
          if (depth_tested && run.targets.width == g_front_width && run.targets.height == g_front_height) {
            std::array<float, 16> view_projection;
            std::memcpy(view_projection.data(), g_constants.data(), sizeof(view_projection));
            ++view_projections[view_projection];
          }
          if (target == g_front_target && !snapshot_taken) {
            if (depth_tested) {
              scene_drawn = true;
            } else if (scene_drawn && device.SnapshotScene(target)) {
              snapshot_taken = true;
              current_target = -1;
            }
          }
        }
        const bool first_use = cleared.insert(target).second;
        const bool clear = run_cleared || first_use;
        if (target != current_target || clear) {
          MetalClear pass_clear;
          // The kill camera clears depth alone between two steps on the finished picture.
          pass_clear.color = first_use || (run_cleared & kClearColorTargets);
          pass_clear.depth = first_use || (run_cleared & kClearDepthStencil);
          pass_clear.depth_value = ClearDepthFor(draw);
          device.BeginPass(target, pass_clear);
          current_target = target;
          ++g_totals.passes;
        }
        run_cleared = 0;
        if (run.targets.color) {
          ++color_draws[target];
          last_color_target = target;
        }

        MetalDraw out;
        out.vertex_shader = shader;
        out.pixel_shader = pixel.handle;
        if (g_debug_ids && run.targets.color) {
          out.debug_id = g_debug_id;
        }
        if (g_dump) {
          const uint32_t* f = list.fetch_data.data() + size_t(draw.fetch_constants) * 192;
          std::string slots;
          const auto word = [](uint32_t value) { return std::bit_cast<float>(value); };
          std::string pc;
          for (int i = 0; i < 96; ++i) {
            pc += fmt::format(" {:.3f}", word(g_constants[1024 + i]));
          }
          *g_dump << "PC" << pc << "\n";
          if (g_debug_vertex_constants) {
            std::string vc;
            for (int i = 0; i < 64 * 4; ++i) {
              vc += fmt::format(" {:.4f}", word(g_constants[i]));
            }
            *g_dump << "VC" << vc << "\n";
          }
          for (const PixelSlot& slot : found_pixel.slots) {
            xenos::xe_gpu_texture_fetch_t fetch;
            std::memcpy(&fetch, f + size_t(slot.slot) * 6, sizeof(fetch));
            slots += fmt::format(" {}k{}:f{}:{}x{}:{:05X}:s{}{}{}{}", slot.slot, slot.kind, uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                                 uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.base_address), uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z), uint32_t(fetch.sign_w));
          }
          *g_dump << fmt::format("{} {} vs {} ps {:08X} {} prim {} n {} tgt {} tiles {:X} vp0 {} {} mode {:X} poly {:.6f} {:.6f} depth {:08X} st {:06X} blend {:08X} cc {:08X} mask {:X} bools {:08X} vp {} {} name {} slots{}\n", g_debug_id, run.pass_select == ~0u ? "D" : "B",
                                 draw.vertex_shader_dwords, draw.pixel_shader, draw.pixel_shader_dwords, primitive, index_count, target, uint32_t(draw.mask), viewport.x, viewport.y, draw.mode_control, std::bit_cast<float>(draw.polygon_offset[0]), std::bit_cast<float>(draw.polygon_offset[1]), draw.depth_control, draw.stencil_front, draw.blend_control, draw.color_control,
                                 draw.color_mask, draw.booleans, viewport.width, viewport.height, found_pixel.name, slots);
        }
        ++g_debug_id;
        out.blend_control = draw.blend_control;
        std::memcpy(out.blend_constant, draw.blend_constant, sizeof(out.blend_constant));
        out.color_mask = WritesNoColor(draw) ? 0 : draw.color_mask;
        out.alpha_test = alpha_test;
        out.alpha_to_coverage = (draw.color_control & kAlphaToCoverage) != 0;
        out.colorless = counting;
        out.query = open_query;
        if (open_query >= 0) {
          query_slots[size_t(open_query)].area = ScaleOf(target) * ScaleOf(target);
        }
        if (counting && g_debug_mark_queries && g_debug_marked.contains(query_slots[size_t(open_query)].query)) {
          out.colorless = false;
          out.color_mask = kColorMaskFirstTarget;
          out.debug_id = kQueryMarkId;
        }
        // Tiles overlap by a few rows, and a quad drawn for one tile would blend twice on those.
        if (tile >= 0 && run.targets.height > uint32_t(kSecondTileTop) && !HalfSizeTiles(run.targets)) {
          out.scissor_top = tile ? kSecondTileTop : 0;
          out.scissor_bottom = tile ? UINT32_MAX : kSecondTileTop;
        }
        out.viewport = viewport;
        out.depth_control = g_debug_no_stencil ? draw.depth_control & ~1u : draw.depth_control;
        out.stencil_front = draw.stencil_front;
        out.stencil_back = draw.stencil_back;
        out.mode_control = draw.mode_control;
        // Slopes are per sixteenth of a pixel. Metal scales a constant bias with a float depth's exponent, so the shader
        // adds that.
        float depth_offset = 0.0f;
        if (draw.mode_control & (kPolygonOffsetFront | kPolygonOffsetBack)) {
          const uint32_t side = (draw.mode_control & kPolygonOffsetFront) ? 0 : 2;
          out.depth_bias_slope = std::bit_cast<float>(draw.polygon_offset[side]) / 16.0f;
          depth_offset = std::bit_cast<float>(draw.polygon_offset[side + 1]);
        }
        out.primitive = primitive == kPrimitiveTriangleStrip ? MetalPrimitive::kTriangleStrip : MetalPrimitive::kTriangleList;
        out.vertex_constants = used;
        out.pixel_constants = used + kPixelConstantsOffset;
        std::memcpy(memory + used, g_constants.data(), kConstantBytes);
        out.fetch_constants = used + kConstantBytes;
        const uint32_t* fetch_constants = list.fetch_data.data() + size_t(draw.fetch_constants) * 192;
        std::memcpy(memory + out.fetch_constants, fetch_constants, kFetchBytes);
        out.shared_constants = out.fetch_constants + kFetchBytes;
        uint8_t* shared = memory + out.shared_constants;
        std::memset(shared, 0, kSharedBytes);
        std::memcpy(shared + kBooleansOffset, &draw.booleans, sizeof(draw.booleans));
        std::memcpy(shared + kAlphaThresholdOffset, &draw.alpha_reference, sizeof(draw.alpha_reference));
        const std::array<float, 4> screen =
            screen_space ? std::array<float, 4>{2.0f / viewport.width, -2.0f / viewport.height, 1.0f / viewport.width - 1.0f, 1.0f - 1.0f / viewport.height}
                         : std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f};
        // Pixel centres sit on whole numbers on the Xenos and half a pixel in on Metal, so everything moves over by that.
        // Without it a full-screen pass misses its last row and column, which on the ocean's tiled wave maps is a grid.
        // Screen-space draws have it built into their transform.
        const std::array<float, 2> half_pixel =
            screen_space ? std::array<float, 2>{0.0f, 0.0f} : std::array<float, 2>{1.0f / viewport.width, -1.0f / viewport.height};
        std::memcpy(shared + kHalfPixelOffset, half_pixel.data(), sizeof(float) * 2);
        std::memcpy(shared + kScreenScaleOffset, screen.data(), sizeof(float) * 2);
        std::memcpy(shared + kScreenOffsetOffset, screen.data() + 2, sizeof(float) * 2);
        std::memcpy(shared + kDepthOffsetOffset, &depth_offset, sizeof(depth_offset));
        const float pixel_scale = 1.0f / ScaleOf(target);
        std::memcpy(shared + kPixelScaleOffset, &pixel_scale, sizeof(pixel_scale));
        // Translated shaders read these as count, start and step.
        for (uint32_t i = 0; i < kLoopConstants; ++i) {
          const uint32_t loop = run.loops[i];
          const std::array<int32_t, 4> values = {int32_t(loop & 0xFF), int32_t((loop >> 8) & 0xFF), int32_t(int8_t(loop >> 16)), 0};
          std::memcpy(shared + kLoopTableOffset + i * 16, values.data(), sizeof(values));
        }
        std::array<int, 36> textures;
        uint32_t texture_count = 0;
        if (pixel.handle >= 0) {
          ++g_totals.shaded_draws;
          for (const PixelSlot& slot : pixel.slots) {
            const uint32_t* fetch = fetch_constants + size_t(slot.slot) * kFetchConstantDwords;
            // 3D means a stack of depth maps. The tables start out pointing at the dummies.
            if (slot.slot < textures.size()) {
              const int texture = slot.kind == 1 ? g_textures->FindStack(fetch) : g_textures->FindTexture(fetch, pixel.reads_whole_picture ? -1 : tile);
              textures[texture_count++] = texture;
              const uint32_t entry = uint32_t(texture) | (slot.kind == 0 ? TextureCache::BiasedChannels(fetch) << kBiasShift : 0);
              std::memcpy(shared + kTextureTableOffsets[slot.kind] + slot.slot * 4, &entry, sizeof(entry));
            }
            const int sampler = g_textures->FindSampler(fetch);
            std::memcpy(shared + kSamplerTableOffset + slot.slot * 4, &sampler, sizeof(sampler));
          }
        }
        for (const uint32_t slot : g_vertex_textures[size_t(shader)]) {
          const uint32_t* fetch = fetch_constants + size_t(kFirstVertexFetch + slot) * kFetchConstantDwords;
          const int texture = g_textures->FindTexture(fetch, tile);
          const int sampler = g_textures->FindSampler(fetch);
          textures[texture_count++] = texture;
          std::memcpy(shared + kVertexTextureTableOffset + slot * 4, &texture, sizeof(texture));
          std::memcpy(shared + kVertexSamplerTableOffset + slot * 4, &sampler, sizeof(sampler));
        }
        out.textures = textures.data();
        out.texture_count = texture_count;
        out.indices = indices.offset;
        out.index_count = indices.count;
        out.wide_indices = indices.wide;
        out.base_vertex = int32_t(draw.index_offset);
        if (g_frame_generation && target == g_front_target && !snapshot_taken && (draw.depth_control & kDepthEnable)) {
          NoteMotionDraw(device, draw, out);
        }
        device.Draw(out);
        used += kDrawBytes;
        ++g_totals.recorded_draws;
        g_totals.recorded_indices += index_count;
        if (!g_check_bytes && !automatic) {
          g_check_address = draw.index_buffer & kGuestMask;
          g_check_bytes = uint32_t(std::min<uint64_t>(kCheckBytes, kGuestBytes - g_check_address));
        }
      }
    }
  }
  apply_queries(frame.runs.size());
  NoteOpeningPass(opening_pass);
  answers.front_width = g_front_width;
  answers.front_height = g_front_height;
  if (DrawsAlone() && frame.front_buffer && !g_debug_stopped) {
    if (g_external_present) {
      CopyFrontBuffer(*frame.front_buffer, device, BusiestViewProjection(view_projections));
    } else {
      WriteFrontBuffer(*frame.front_buffer, device, answers);
    }
  }
  const int shown = g_debug_show >= 0 ? g_debug_show : last_color_target;
  if (shown >= 0) {
    device.ShowTarget(shown, g_debug_show < 0);
  }
}

void ReadManifest() {
  std::ifstream in(g_shader_dir / "manifest.txt");
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    std::string key;
    row >> key;
    std::vector<uint32_t>& addresses = g_manifest[key];
    uint32_t address;
    while (row >> address) {
      addresses.push_back(address);
    }
  }
  std::ifstream loose(g_shader_dir / "loose-manifest.txt");
  std::string loose_key, key;
  while (loose >> loose_key >> key) {
    g_loose_manifest[loose_key] = key;
  }
  std::ifstream vertex_textures(g_shader_dir / "vertex-textures.txt");
  while (std::getline(vertex_textures, line)) {
    std::istringstream row(line);
    std::string key;
    row >> key;
    std::vector<uint32_t>& slots = g_vertex_texture_manifest[key];
    uint32_t slot;
    while (row >> slot) {
      slots.push_back(slot);
    }
  }
  std::ifstream screen_shaders(g_shader_dir / "screen-shaders.txt");
  while (screen_shaders >> line) {
    g_screen_shaders.insert(line);
  }
  std::ifstream pixels(g_shader_dir / "pixel-manifest.txt");
  while (std::getline(pixels, line)) {
    std::istringstream row(line);
    std::string key;
    row >> key;
    std::vector<PixelSlot>& slots = g_pixel_manifest[key];
    PixelSlot slot;
    while (row >> slot.slot >> slot.kind) {
      slots.push_back(slot);
    }
  }
  REXLOG_INFO("Native shadow: {} vertex shaders in {}", g_manifest.size(), g_shader_dir.string());
}

uint32_t Percentile(const std::vector<uint32_t>& sorted, double share) {
  return sorted.empty() ? 0 : sorted[std::min(size_t(double(sorted.size()) * share), sorted.size() - 1)];
}

void Report(const MetalDevice& device) {
  std::vector<uint32_t> sorted = g_encode_us;
  std::sort(sorted.begin(), sorted.end());
  uint64_t guest_frames, dropped, capture_ns, direct_draws, ring_desyncs, vertex_code_copied;
  {
    // Each report covers the frames since the last one, so menus and loading don't blur a run.
    std::lock_guard lock(g_mutex);
    guest_frames = std::exchange(g_guest_frames, 0);
    dropped = g_dropped;
    capture_ns = std::exchange(g_capture_ns, 0);
    direct_draws = std::exchange(g_direct_draws, 0);
    ring_desyncs = g_ring_desyncs;
    vertex_code_copied = std::exchange(g_vertex_code_copied, 0);
  }
  const MetalGpuTotals gpu = device.gpu_totals();
  if (!gpu.last_error.empty()) {
    REXLOG_ERROR("Native shadow: GPU error: {}", gpu.last_error);
  }
  const double frames = double(std::max<uint64_t>(g_totals.frames, 1));
  // The device's GPU totals run from the start, so the last window's GPU time comes from the difference. On the same
  // scene, it climbing is the Mac slowing its GPU, which it can do before the thermal state moves.
  static uint64_t last_gpu_ns = 0, last_gpu_frames = 0;
  const uint64_t window_gpu_frames = gpu.frames - last_gpu_frames;
  const double window_gpu_ms = (gpu.gpu_ns - last_gpu_ns) / 1e6 / double(std::max<uint64_t>(window_gpu_frames, 1));
  last_gpu_ns = gpu.gpu_ns;
  last_gpu_frames = gpu.frames;
  const char* thermal = ThermalState();
  // The sweep's totals run from the start too.
  static TextureCache::SweepTotals last_sweep;
  const TextureCache::SweepTotals sweep = g_textures->sweep_totals();
  const uint64_t sweep_repairs = sweep.repairs - last_sweep.repairs;
  const double sweep_ms = (sweep.ns - last_sweep.ns) / 1e6 / frames;
  const double sweep_mb = (sweep.hashed_bytes - last_sweep.hashed_bytes) / 1e6 / frames;
  const uint64_t sweep_checked = sweep.checked - last_sweep.checked;
  last_sweep = sweep;
  if (g_totals.vertex_code_changed || vertex_code_copied) {
    REXLOG_INFO("Native shadow: {} draws' vertex shaders were rewritten before Metal drew them; {} shaders copied at capture",
                g_totals.vertex_code_changed, vertex_code_copied);
  }
  if (sweep_repairs) {
    REXLOG_INFO("Native shadow: texture sweep reloaded {} textures whose memory changed unnoticed, over the last {} frames",
                sweep_repairs, g_totals.frames);
  }
  REXLOG_INFO("Native shadow: GPU {:.1f} ms a frame over the last {} frames, thermal {}, opening pass in {} of {} frames",
              window_gpu_ms, window_gpu_frames, thermal, g_totals.opening_pass_frames, g_totals.frames);
  std::ofstream out(g_path);
  out << fmt::format(R"({{"thermal":"{}","gpu_ms_recent":{:.3f},"opening_pass_frames":{},)", thermal, window_gpu_ms,
                     g_totals.opening_pass_frames);
  out << fmt::format(R"("vertex_code_changed":{},"vertex_code_copied":{},)", g_totals.vertex_code_changed, vertex_code_copied);
  out << fmt::format(R"("texture_sweep":{{"repairs":{},"checked":{},"mb_per_frame":{:.2f},"ms_per_frame":{:.3f},"rounds":{}}},)",
                     sweep_repairs, sweep_checked, sweep_mb, sweep_ms, sweep.rounds);
  out << fmt::format(
      R"("guest_frames":{},"frames":{},"dropped":{},"draws_per_frame":{:.1f},"skipped_draws_per_frame":{:.1f},"no_room":{},)"
      R"("recorded_draws_per_frame":{:.1f},"recorded_indices_per_frame":{:.0f},"no_shader_draws_per_frame":{:.1f},"no_target_draws_per_frame":{:.1f},"other_primitive_draws_per_frame":{:.1f},"other_index_order_draws_per_frame":{:.1f},"transparent_draws_per_frame":{:.1f},"no_color_draws_per_frame":{:.1f},"alpha_tested_draws_per_frame":{:.1f},"shaded_draws_per_frame":{:.1f},"needs_resolve_draws_per_frame":{:.1f},"native_resolves_per_frame":{:.1f},"missed_resolves_per_frame":{:.1f},"pixel_shaders_found":{},"pixel_shaders_missing":{},"textures_loaded":{},"textures_skipped":{},"textures_reloaded":{},"reloaded_mb":{:.1f},"textures_unchanged":{},"direct_draws_per_frame":{:.1f},"ring_desyncs":{},"passes_per_frame":{:.1f},"targets":{},)"
      R"("shaders_found":{},"shaders_missing":{},"shaders_failed":{},"index_copy_ms_per_frame":{:.3f},)"
      R"("capture_ms_per_frame":{:.4f},"wait_ms_per_frame":{:.3f},"encode_ms_p50":{:.3f},"encode_ms_p95":{:.3f},"encode_ms_max":{:.3f},)"
      R"("gpu_frames":{},"gpu_errors":{},"gpu_ms_per_frame":{:.3f},"gpu_ms_worst":{:.3f},)"
      R"("guest_memory_wrapped":{},"guest_checks":{},"guest_matches":{},"front_buffers_missed":{},"frames_generated_per_frame":{:.2f},"motion_draws_per_frame":{:.1f},"picture":[{},{}],)"
      R"("render_scale":{:.2f},"resampled_copies":{},"dropped_copies":{},"dropped_stack_copies":{},"dropped_reads":{},"gpu_queued_ms_per_frame":{:.3f},"generation_ms_per_frame":{:.3f},)",
      guest_frames, g_totals.frames, dropped, g_totals.draws / frames, g_totals.skipped_draws / frames, g_totals.no_room,
      g_totals.recorded_draws / frames, g_totals.recorded_indices / frames, g_totals.no_shader_draws / frames,
      g_totals.no_target_draws / frames, g_totals.other_primitive_draws / frames, g_totals.other_index_order_draws / frames,
      g_totals.transparent_draws / frames, g_totals.no_color_draws / frames, g_totals.alpha_tested_draws / frames,
      g_totals.shaded_draws / frames, g_totals.needs_resolve_draws / frames, g_totals.native_resolves / frames, g_totals.missed_resolves / frames, g_totals.pixel_shaders_found, g_totals.pixel_shaders_missing, g_textures->textures_loaded(),
      g_textures->textures_skipped(), g_textures->textures_reloaded(), g_textures->reloaded_bytes() / 1e6, g_textures->textures_unchanged(), direct_draws / double(std::max<uint64_t>(guest_frames, 1)), ring_desyncs,
      g_totals.passes / frames, g_targets.size(), g_totals.shaders_found, g_totals.shaders_missing, g_totals.shaders_failed,
      g_totals.index_copy_ns / 1e6 / frames,
      capture_ns / 1e6 / double(std::max<uint64_t>(guest_frames, 1)), g_totals.wait_ns / 1e6 / frames,
      Percentile(sorted, 0.5) / 1e3, Percentile(sorted, 0.95) / 1e3, Percentile(sorted, 1.0) / 1e3, gpu.frames, gpu.errors,
      gpu.gpu_ns / 1e6 / double(std::max<uint64_t>(gpu.frames, 1)), gpu.worst_gpu_ns / 1e6,
      device.guest_memory_wrapped() ? "true" : "false", g_totals.guest_checks, g_totals.guest_matches, g_totals.front_buffers_missed,
      g_totals.frames_generated / frames, g_totals.motion_draws / frames, PictureWidth(), PictureHeight(), g_render_scale, gpu.resampled_copies, gpu.dropped_copies, gpu.dropped_stack_copies, gpu.dropped_reads,
      gpu.queued_ns / 1e6 / double(std::max<uint64_t>(gpu.frames, 1)), gpu.generation_ns / 1e6 / double(std::max<uint64_t>(gpu.frames, 1)));
  QueryTotals queries;
  {
    std::lock_guard lock(g_query_mutex);
    queries = std::exchange(g_query_totals, {});
    queries.quads = std::exchange(g_quads_counted, 0);
  }
  std::string disagreements;
  g_debug_marked.clear();
  for (const QueryDisagreement& disagreement : queries.disagreements) {
    g_debug_marked.insert(disagreement.query);
    disagreements += fmt::format("{}[\"{:08X}\",{},{},{},{}]", disagreements.empty() ? "" : ",", disagreement.query, disagreement.least, disagreement.metal, disagreement.emulator, disagreement.times);
  }
  std::string asks;
  for (const auto& [caller, count] : queries.asks) {
    asks += fmt::format(R"({}"{:08X}":{{"waiting":{},"none":{},"some":{},"largest":{}}})", asks.empty() ? "" : ",", caller, count.waiting, count.none, count.some, count.largest);
  }
  std::string locks;
  for (const auto& [key, count] : queries.locks) {
    locks += fmt::format(R"({}["{:08X}","{:05X}",{},{},{},{}])", locks.empty() ? "" : ",", key[0], key[1], key[2], key[3], key[4], count);
  }
  out << fmt::format(R"("locks":[{}],)", locks);
  PacingTotals pacing;
  {
    std::lock_guard lock(g_pacing_mutex);
    pacing = std::exchange(g_pacing_totals, {});
  }
  std::string waits;
  for (const auto& [caller, wait] : pacing.waits) {
    waits += fmt::format(R"({}"{:08X}":{{"count":{},"ms":{:.1f},"worst_ms":{:.1f},"metal_done":{},"in_flight":{},"open_frame":{}}})", waits.empty() ? "" : ",", caller, wait.count,
                         wait.ns / 1e6, wait.worst_ns / 1e6, wait.metal_done, wait.in_flight, wait.open_frame);
  }
  const double paced_frames = double(std::max<uint64_t>(pacing.frames, 1));
  if (pacing.idle_releases_open) {
    REXLOG_INFO("Native shadow: fences let through early {} times in {} frames, {} with draws not yet drawn", pacing.idle_releases,
                pacing.frames, pacing.idle_releases_open);
  }
  out << fmt::format(R"("pacing":{{"frames":{},"idle_releases":{},"idle_releases_open":{},"fences_per_frame":{:.2f},"metal_ms":{:.2f},"worst_metal_ms":{:.2f},"emulator_lead":{:.2f},"held_frames":{},"held_ms":{:.1f},"waits":{{{}}}}},)", pacing.frames,
                     pacing.idle_releases, pacing.idle_releases_open, double(pacing.fences) / paced_frames, pacing.metal_ns / 1e6 / paced_frames, pacing.worst_metal_ns / 1e6, double(pacing.emulator_lead) / paced_frames, pacing.held_frames, pacing.held_ns / 1e6, waits);
  ReadbackTotals readbacks;
  {
    std::lock_guard lock(g_readback_mutex);
    readbacks = std::exchange(g_readback_totals, {});
  }
  const double compared = double(std::max<uint64_t>(readbacks.compared, 1));
  const auto means = [&](const auto& values) {
    std::string joined;
    for (const double value : values) {
      joined += fmt::format("{}{:.3f}", joined.empty() ? "" : ",", value / compared);
    }
    return joined;
  };
  out << fmt::format(R"("readback":{{"compared":{},"missed":{},"written":{},"late":{},"guest_mean":[{}],"metal_mean":[{}],"difference":[{}],"far":[{}],"guest_exposure":{:.3f},"metal_exposure":{:.3f}}},)",
                     readbacks.compared, readbacks.missed, readbacks.written, readbacks.late, means(readbacks.guest_mean), means(readbacks.metal_mean), means(readbacks.difference),
                     means(readbacks.far), readbacks.guest_exposure / compared, readbacks.metal_exposure / compared);
  out << fmt::format(R"("queries":{{"both_none":{},"both_some":{},"metal_only":{},"emulator_only":{},"unanswered":{},"no_room":{},"quads":{},"samples_ratio":{:.3f},"ratios":[{}],"metal_only_sizes":[{}],"asks":{{{}}},"disagreements":[{}]}}}})",
                     queries.both_none, queries.both_some, queries.metal_only, queries.emulator_only, queries.unanswered, g_totals.queries_no_room, queries.quads,
                     double(queries.metal_samples) / double(std::max<uint64_t>(queries.emulator_samples, 1)), fmt::join(queries.ratios, ","), fmt::join(queries.metal_only_sizes, ","), asks, disagreements);
  const Totals kept = g_totals;
  g_totals = {};
  g_totals.shaders_found = kept.shaders_found;
  g_totals.shaders_missing = kept.shaders_missing;
  g_totals.shaders_failed = kept.shaders_failed;
  g_totals.pixel_shaders_found = kept.pixel_shaders_found;
  g_totals.pixel_shaders_missing = kept.pixel_shaders_missing;
  g_encode_us.clear();
}

void RenderThread() {
  pthread_setname_np("native-shadow");
  std::unique_ptr<MetalDevice> device = MetalDevice::Create(g_guest, kGuestBytes, kFrameBytes);
  if (!device) {
    REXLOG_ERROR("Native shadow: no Metal device");
    g_on.store(false, std::memory_order_relaxed);
    return;
  }
  REXLOG_INFO("Native shadow: Metal is up, guest memory wrapped: {}", device->guest_memory_wrapped());
  ReadManifest();
  g_textures = std::make_unique<TextureCache>(*device, g_memory, g_guest, kGuestBytes);
  if (REXCVAR_GET(crackdown_native_shadow_window)) {
    device->ShowWindow();
  }
  if (DrawsAlone()) {
    UseExternalPresent();
    // Shadow mode compares with the emulator, pixel for pixel, so only the Metal renderer draws bigger.
    g_render_scale = float(std::clamp(REXCVAR_GET(crackdown_render_scale), 100, kMaxRenderScale)) / 100.0f;
    g_textures->SetSceneScale(kScaledWidth, g_render_scale);
    if (g_render_scale != 1.0f) {
      REXLOG_INFO("Native only: the scene is drawn at {}x{}", std::lround(kScaledWidth * g_render_scale),
                  std::lround(kScaledHeight * g_render_scale));
    }
    std::lock_guard lock(g_pacing_mutex);
    g_set_fence_done = g_found_fence_done;
  }
  while (true) {
    std::unique_ptr<Frame> frame;
    {
      std::unique_lock lock(g_mutex);
      // The game can wait for the GPU to go idle with no frame to show, on loading. With nothing of Metal's pending,
      // fences up to the newest issued are let through, or that wait would never end.
      while (!g_wake.wait_for(lock, kIdleFenceRelease, [] { return !g_ready.empty(); })) {
        ReleaseIdleFences(!g_current->runs.empty());
      }
      frame = std::move(g_ready.front());
      g_ready.pop_front();
      if (g_gamma_ramp_new) {
        device->SetGammaRamp(g_gamma_ramp.data());
        g_gamma_ramp_new = false;
      }
    }
    ++g_frame_number;
    std::filesystem::path capture;
    if (g_frame_number % kCaptureFrames == 0) {
      capture = g_path;
      capture.replace_extension(".png");
    }
    std::unique_ptr<std::ofstream> dump;
    if (g_frame_number % 30 == 0 && REXCVAR_GET(crackdown_native_shadow_debug)) {
      const std::filesystem::path control = g_path.parent_path() / "metal-debug.txt";
      std::ifstream in(control);
      std::string word;
      bool want_dump = false;
      bool want_report = false;
      bool want_reload = false;
      std::string name = "live";
      g_debug_skip_blended = g_debug_ids = g_debug_no_resolves = g_debug_alpha = g_debug_no_stencil = g_debug_no_clears = g_debug_mark_queries = g_debug_vertex_constants = false;
      g_debug_show = -1;
      g_debug_skip_ps.clear();
      g_debug_stop_ps = 0;
      g_debug_range_ps = 0;
      while (in >> word) {
        g_debug_skip_blended |= word == "skip_blended";
        g_debug_ids |= word == "ids";
        g_debug_no_resolves |= word == "no_resolves";
        g_debug_alpha |= word == "alpha";
        g_debug_no_stencil |= word == "no_stencil";
        g_debug_no_clears |= word == "no_clears";
        g_debug_mark_queries |= word == "mark_queries";
        g_debug_vertex_constants |= word == "vconst";
        want_report |= word == "report";
        want_reload |= word == "reload_stale";
        const bool keep = word.rfind("only_range=", 0) == 0;
        if (keep || word.rfind("skip_range=", 0) == 0) {
          unsigned ps = 0, first = 0, last = 0;
          if (std::sscanf(word.c_str() + 11, "%x:%u-%u", &ps, &first, &last) == 3) {
            g_debug_range_ps = ps;
            g_debug_range_first = first;
            g_debug_range_last = last;
            g_debug_range_keep = keep;
          }
        }
        if (word.rfind("stop_ps=", 0) == 0) {
          g_debug_stop_ps = uint32_t(std::stoul(word.substr(8), nullptr, 16));
        }
        if (word.rfind("show=", 0) == 0) {
          g_debug_show = std::stoi(word.substr(5));
        }
        if (word.rfind("skip_ps=", 0) == 0) {
          g_debug_skip_ps.insert(uint32_t(std::stoul(word.substr(8), nullptr, 16)));
        }
        if (word.rfind("dump=", 0) == 0) {
          name = word.substr(5);
          want_dump = true;
        }
      }
      in.close();
      g_debug_reload_stale |= want_reload;
      if (want_dump || want_report || want_reload) {
        std::error_code ignored;
        std::filesystem::remove(control, ignored);
        std::ofstream(control) << (g_debug_skip_blended ? "skip_blended " : "") << (g_debug_ids ? "ids " : "")
                               << (g_debug_no_resolves ? "no_resolves " : "") << (g_debug_alpha ? "alpha " : "") << (g_debug_no_stencil ? "no_stencil " : "")
                               << (g_debug_mark_queries ? "mark_queries " : "")
                               << (g_debug_show >= 0 ? fmt::format("show={} ", g_debug_show) : "")
                               << (g_debug_stop_ps ? fmt::format("stop_ps={:X} ", g_debug_stop_ps) : "")
                               << (g_debug_range_ps ? fmt::format("{}={:X}:{}-{} ", g_debug_range_keep ? "only_range" : "skip_range", g_debug_range_ps, g_debug_range_first, g_debug_range_last) : "");
        for (const uint32_t skip : g_debug_skip_ps) {
          std::ofstream(control, std::ios::app) << fmt::format("skip_ps={:X} ", skip);
        }
        if (want_dump) {
          capture = g_path.parent_path() / ("live-" + name + ".png");
          dump = std::make_unique<std::ofstream>(g_path.parent_path() / ("live-" + name + ".txt"));
          g_dump = dump.get();
        }
      }
      // The handler only hands the work to the UI thread, which asks for the next frame.
      if (want_report && g_report_handler) {
        g_report_handler();
      }
    }
    g_skip_water = false;
    {
      std::lock_guard lock(g_report_mutex);
      if (g_report_folder.empty() && !g_report_followup.empty()) {
        capture = g_report_followup / "metal-frame-nowater.png";
        g_skip_water = true;
        REXLOG_INFO("Native shadow: report frame {} without the water saved to {}", g_frame_number, g_report_followup.string());
        g_report_followup.clear();
      }
      if (!g_report_folder.empty()) {
        capture = g_report_folder / "metal-frame.png";
        dump = std::make_unique<std::ofstream>(g_report_folder / "draws.txt");
        g_dump = dump.get();
        std::error_code ignored;
        std::filesystem::copy_file(g_path, g_report_folder / "renderer.json", std::filesystem::copy_options::overwrite_existing,
                                   ignored);
        {
          std::ofstream textures(g_report_folder / "textures.txt");
          g_textures->CheckStale(textures);
        }
        REXLOG_INFO("Native shadow: report frame {} saved to {}", g_frame_number, g_report_folder.string());
        g_report_followup = std::move(g_report_folder);
        g_report_folder.clear();
      }
    }
    auto answers = std::make_shared<FrameAnswers>();
    const MetalFrameTiming timing =
        device->RecordFrame([&](uint8_t* memory) { Encode(*frame, *device, memory, *answers); }, capture,
                            [answers, fence = frame->fence, handed_over = frame->handed_over](const MetalFrameResults& results) {
                              if (!results.pictures_only) {
                                OnMetalSamples(answers->queries, results.query_samples);
                                OnMetalPixels(answers->readbacks, results.readback);
                              }
                              if (results.picture_count && g_frame_generation) {
                                PictureSet set;
                                set.pictures = results.pictures;
                                set.count = results.picture_count;
                                set.width = results.picture_width;
                                set.height = results.picture_height;
                                set.guest_width = answers->front_width;
                                set.guest_height = answers->front_height;
                                {
                                  std::lock_guard lock(g_present_mutex);
                                  g_present_next = set;
                                }
                                g_present_wake.notify_one();
                              } else if (results.picture_count) {
                                std::lock_guard lock(g_picture_mutex);
                                g_picture = results.pictures[results.picture_count - 1];
                                g_picture_width = results.picture_width;
                                g_picture_height = results.picture_height;
                              }
                              if (results.pictures_only) {
                                return;
                              }
                              if (answers->front_buffer_bytes) {
                                // The emulator reloads a texture when it's told its memory changed, as a CPU write would.
                                g_memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                                                         answers->front_buffer, answers->front_buffer_bytes, true, false);
                              }
                              OnFrameFinished(fence, handed_over);
                            });
    g_dump = nullptr;
    frame->runs.clear();
    frame->queries.clear();
    frame->front_buffer.reset();
    {
      std::lock_guard lock(g_mutex);
      g_free.push_back(std::move(frame));
    }
    ++g_totals.frames;
    g_totals.wait_ns += timing.wait_ns;
    g_encode_us.push_back(uint32_t(timing.encode_ns / 1000));
    if (g_frame_number % kCheckFrames == 0 && g_check_bytes) {
      ++g_totals.guest_checks;
      g_totals.guest_matches += device->MatchesGuestMemory(g_check_address, g_check_bytes);
      g_check_bytes = 0;
    }
    if (g_frame_number % kReportFrames == 0) {
      Report(*device);
    }
  }
}

}  // namespace

bool Enabled() {
  return g_on.load(std::memory_order_relaxed);
}

void SetReportHandler(std::function<void()> handler) {
  g_report_handler = std::move(handler);
}

bool RequestReport(const std::filesystem::path& folder) {
  if (!Enabled()) {
    return false;
  }
  std::lock_guard lock(g_report_mutex);
  g_report_folder = folder;
  return true;
}

bool DrawsAlone() {
  return REXCVAR_GET(crackdown_native_only);
}

void Install(rex::memory::Memory* memory, const std::filesystem::path& shader_dir, const std::filesystem::path& output_dir) {
  // A build without the shaders, as one made from source without the shader steps, falls back rather than draw nothing.
  std::error_code ec;
  if (REXCVAR_GET(crackdown_native_only) && !std::filesystem::exists(shader_dir / "manifest.txt", ec)) {
    REXLOG_WARN("Native only: no Metal shaders in {}, so the emulator draws", shader_dir.string());
    rex::cvar::SetFlagByName("crackdown_native_only", "false");
  }
  if (REXCVAR_GET(crackdown_native_only)) {
    // The emulator still runs the ring, so fences, waits and swaps carry on. It leaves query answers to Metal (SDK patch
    // 0015).
    for (const auto& [name, value] : {std::pair{"crackdown_native_shadow", "true"},
                                      {"crackdown_native_pacing", "true"}, {"crackdown_native_exposure", "true"},
                                      {"vulkan_skip_guest_drawing", "true"}}) {
      if (!rex::cvar::SetFlagByName(name, value)) {
        REXLOG_ERROR("Native only: couldn't set {}", name);
      }
    }
  }
  if (!REXCVAR_GET(crackdown_native_shadow)) {
    return;
  }
  g_guest = memory->TranslatePhysical<uint8_t*>(0);
  g_memory = memory;
  g_shader_dir = shader_dir;
  g_path = output_dir / fmt::format("native-shadow-{}.json", uint64_t(std::time(nullptr)));
  if (REXCVAR_GET(crackdown_native_shadow_debug)) {
    std::filesystem::remove(output_dir / "metal-debug.txt", ec);
  }
  g_current = std::make_unique<Frame>();
  g_current->runs.reserve(kRunsPerFrame);
  for (size_t i = 0; i < kPooledFrames; ++i) {
    g_free.push_back(std::make_unique<Frame>());
    g_free.back()->runs.reserve(kRunsPerFrame);
  }
  g_on.store(true, std::memory_order_relaxed);
  std::thread(RenderThread).detach();
  REXLOG_INFO("Native shadow: writing {}", g_path.string());
}

void OnGuestFrame() {
  if (!Enabled()) {
    return;
  }
  // With pacing on, the game waits for Metal rather than Metal dropping a frame. It's bounded so a stuck GPU can't hang it.
  if (REXCVAR_GET(crackdown_native_pacing)) {
    const Clock::time_point start = Clock::now();
    std::unique_lock lock(g_pacing_mutex);
    if (g_frames_handed_over - g_frames_finished >= kMaxFramesBehind) {
      g_frame_finished.wait_for(lock, kLongestHold, [] { return g_frames_handed_over - g_frames_finished < kMaxFramesBehind; });
      ++g_pacing_totals.held_frames;
      g_pacing_totals.held_ns += Since(start);
    }
  }
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  const uint32_t fence = device ? LoadGuestWord(device + kDeviceFence) : 0;
  std::lock_guard lock(g_mutex);
  ++g_guest_frames;
  // The emulator does a resolve some time after the game asks for it, so a frame can read the old contents and settle
  // on them. Marking them again a frame later picks up the new ones.
  for (const FetchConstant& fetch : g_resolved_last_frame) {
    TextureCache::MarkResolved(fetch.data());
  }
  g_resolved_last_frame.swap(g_resolved_this_frame);
  g_resolved_this_frame.clear();
  if (g_current->runs.empty() || g_free.empty()) {
    {
      std::lock_guard pacing_lock(g_pacing_mutex);
      NoteFrameFence(fence, true);
    }
    if (g_current->runs.empty()) {
      return;
    }
    ++g_dropped;
    g_current->runs.clear();
    g_current->queries.clear();
    g_current->front_buffer.reset();
    return;
  }
  g_current->fence = fence;
  g_current->handed_over = Clock::now();
  {
    std::lock_guard pacing_lock(g_pacing_mutex);
    ++g_frames_handed_over;
    NoteFrameFence(fence, false);
    g_pacing_totals.fences += fence - g_handed_over_fence.load(std::memory_order_relaxed);
  }
  g_handed_over_fence.store(fence, std::memory_order_relaxed);
  g_ready.push_back(std::move(g_current));
  g_current = std::move(g_free.back());
  g_free.pop_back();
  g_wake.notify_one();
}

void OnClear(uint32_t flags) {
  if (Enabled()) {
    std::lock_guard lock(g_mutex);
    g_clear_pending |= flags;
  }
}

void CountDirectDraw() {
  std::lock_guard lock(g_mutex);
  ++g_direct_draws;
}

void OnResolve(const ResolveCopy& copy, const Targets& targets, const uint8_t* base) {
  if (!Enabled()) {
    return;
  }
  FetchConstant fetch{};
  if (copy.texture) {
    std::memcpy(fetch.data(), base + copy.texture + kTextureFetchConstant + rex::memory::detail::PhysicalHostOffset(copy.texture),
                sizeof(fetch));
    for (uint32_t& dword : fetch) {
      dword = std::byteswap(dword);
    }
    TextureCache::MarkResolved(fetch.data());
  }
  uint64_t resolve_number = 0;
  if (copy.texture) {
    xenos::xe_gpu_texture_fetch_t decoded;
    std::memcpy(&decoded, fetch.data(), sizeof(decoded));
    std::lock_guard lock(g_readback_mutex);
    resolve_number = ++g_resolve_numbers[uint32_t(decoded.base_address)];
  }
  std::lock_guard lock(g_mutex);
  if (copy.texture) {
    g_resolved_this_frame.push_back(fetch);
  }
  Run& run = g_current->runs.emplace_back();
  run.resolve = copy;
  run.resolve_number = resolve_number;
  run.dest = fetch;
  run.targets = targets;
}

void OnTextureLock(uint32_t caller, uint32_t texture, const uint8_t* base) {
  if (!Enabled() || !texture) {
    return;
  }
  FetchConstant fetch{};
  std::memcpy(fetch.data(), base + texture + kTextureFetchConstant + rex::memory::detail::PhysicalHostOffset(texture), sizeof(fetch));
  for (uint32_t& dword : fetch) {
    dword = std::byteswap(dword);
  }
  xenos::xe_gpu_texture_fetch_t decoded;
  std::memcpy(&decoded, fetch.data(), sizeof(decoded));
  const uint32_t page = uint32_t(decoded.base_address), width = uint32_t(decoded.size_2d.width) + 1, height = uint32_t(decoded.size_2d.height) + 1;
  {
    std::lock_guard lock(g_query_mutex);
    ++g_query_totals.locks[{caller, page, uint32_t(decoded.format), width, height}];
  }
  if (xenos::TextureFormat(uint32_t(decoded.format)) != xenos::TextureFormat::k_8_8_8_8) {
    return;
  }
  std::lock_guard lock(g_readback_mutex);
  g_read_textures.insert({page, width, height});
  const auto number = g_resolve_numbers.find(page);
  const bool check = ++g_locks_seen % kReadbackCheckLocks == 0;
  const bool use_metal = REXCVAR_GET(crackdown_native_exposure);
  if (number == g_resolve_numbers.end() || (!check && !use_metal)) {
    return;
  }
  // A tiled surface runs past its last row, up to the next whole tile.
  const uint32_t pitch = uint32_t(decoded.pitch) * 32;
  const size_t bytes = size_t(pitch) * rex::align(height, 32u) * 4;
  const uint32_t address = ((page << 12) & kGuestMask) + rex::memory::detail::PhysicalHostOffset(page << 12);
  if (!pitch || address + bytes > kGuestBytes) {
    return;
  }
  const ResolveKey key = {number->second, page};
  if (check) {
    GuestPixels& pixels = g_guest_pixels[key];
    pixels.bytes.assign(g_guest + address, g_guest + address + bytes);
    pixels.pitch = pitch;
    pixels.tiled = decoded.tiled;
    CompareReadback(key, width, height);
    if (g_guest_pixels.size() > kKeptReadbacks) {
      g_guest_pixels.erase(g_guest_pixels.begin());
      ++g_readback_totals.missed;
    }
  }
  if (use_metal) {
    const auto metal = g_metal_pixels.find(key);
    if (metal == g_metal_pixels.end()) {
      ++g_readback_totals.late;
      return;
    }
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        uint32_t pixel;
        std::memcpy(&pixel, metal->second.data() + (size_t(y) * width + x) * 4, sizeof(pixel));
        pixel = std::byteswap(pixel);
        std::memcpy(g_guest + address + GuestPixelOffset(x, y, pitch, decoded.tiled), &pixel, sizeof(pixel));
      }
    }
    ++g_readback_totals.written;
  }
}

void OnSwap(uint32_t fetch_address, const uint8_t* base) {
  if (!Enabled() || !fetch_address) {
    return;
  }
  FetchConstant fetch;
  std::memcpy(fetch.data(), rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), fetch_address), sizeof(fetch));
  for (uint32_t& dword : fetch) {
    dword = std::byteswap(dword);
  }
  std::lock_guard lock(g_mutex);
  g_current->front_buffer = fetch;
}

void SetDevice(uint32_t device, const uint8_t* base) {
  g_virtual_guest.store(const_cast<uint8_t*>(base), std::memory_order_relaxed);
  g_device.store(device, std::memory_order_relaxed);
}

// A wait between frames, as on loading, with no draws towards the next one. At a present the frame just drawn hasn't been
// handed over yet, and letting its fences through would let the game write over what Metal is about to read.
void OnFenceWaitStarting() {
  if (!Enabled()) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    if (!g_current->runs.empty()) {
      return;
    }
  }
  ReleaseIdleFences(false);
}

void OnFenceWait(uint32_t caller, uint32_t fence, uint64_t waited_ns) {
  if (!Enabled()) {
    return;
  }
  const bool metal_done = int32_t(g_finished_fence.load(std::memory_order_relaxed) - fence) >= 0;
  const bool handed_over = int32_t(g_handed_over_fence.load(std::memory_order_relaxed) - fence) >= 0;
  std::lock_guard lock(g_pacing_mutex);
  FenceWaits& waits = g_pacing_totals.waits[caller];
  ++waits.count;
  waits.ns += waited_ns;
  waits.worst_ns = std::max(waits.worst_ns, waited_ns);
  ++(metal_done ? waits.metal_done : handed_over ? waits.in_flight : waits.open_frame);
}

void OnQuery(uint32_t query, bool begin) {
  if (!Enabled()) {
    return;
  }
  uint64_t id = 0;
  if (begin) {
    std::lock_guard lock(g_query_mutex);
    id = ++g_last_query;
    g_latest_query[query] = id;
  }
  std::lock_guard lock(g_mutex);
  g_current->queries.push_back({g_current->runs.size(), id, query, begin});
}

void OnQueryAsked(uint32_t caller, uint32_t query, bool ready, uint32_t samples) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_query_mutex);
  QueryAsks& asks = g_query_totals.asks[caller];
  ++(!ready ? asks.waiting : samples ? asks.some : asks.none);
  asks.largest = std::max<uint64_t>(asks.largest, ready ? samples : 0);
  if (!ready) {
    return;
  }
  const auto latest = g_latest_query.find(query);
  if (latest == g_latest_query.end()) {
    return;
  }
  const auto entry = g_query_answers.try_emplace(latest->second).first;
  // The game can ask again until it issues the query anew. Each answer counts once.
  if (!entry->second.emulator) {
    entry->second.emulator = samples;
    SettleQuery(entry, query);
  }
}

void SetGammaRamp(const uint32_t* ramp) {
  std::lock_guard lock(g_mutex);
  std::copy_n(ramp, g_gamma_ramp.size(), g_gamma_ramp.begin());
  g_gamma_ramp_new = true;
}

void SetRingDesyncs(uint64_t desyncs) {
  std::lock_guard lock(g_mutex);
  g_ring_desyncs = desyncs;
}

void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* float_constants,
             const uint32_t* loop_constants, const Targets& targets, std::shared_ptr<const std::vector<uint8_t>> vertex_code,
             std::shared_ptr<const VertexCode> list_code) {
  const Clock::time_point start = Clock::now();
  std::lock_guard lock(g_mutex);
  Run& run = g_current->runs.emplace_back();
  run.lists = std::move(lists);
  run.pass_select = pass_select;
  run.clear = g_clear_pending;
  g_clear_pending = 0;
  run.targets = targets;
  run.vertex_code = std::move(vertex_code);
  run.list_code = std::move(list_code);
  // Binding rewrites a vertex shader in place for each vertex format it's used with, and Metal draws a frame later, so a
  // shader read then may have been rewritten for another format: its draws then read their vertices with the wrong
  // stride, as huge flat-coloured wedges. Copy now any the command buffer's own copies don't cover.
  if (!run.vertex_code && run.lists && g_guest) {
    std::shared_ptr<VertexCode> copies;
    for (const BakedList& list : *run.lists) {
      for (const BakedDraw& draw : list.draws) {
        const uint32_t dwords = draw.vertex_shader_dwords;
        const uint32_t address = draw.vertex_shader & kGuestMask;
        if (!dwords || dwords > kMaxShaderDwords || uint64_t(address) + dwords * 4ull > kGuestBytes) {
          continue;
        }
        const uint64_t key = uint64_t(draw.vertex_shader) << 32 | dwords;
        if ((run.list_code && run.list_code->contains(key)) || (copies && copies->contains(key))) {
          continue;
        }
        if (!copies) {
          copies = run.list_code ? std::make_shared<VertexCode>(*run.list_code) : std::make_shared<VertexCode>();
        }
        (*copies)[key].assign(g_guest + address, g_guest + address + size_t(dwords) * 4);
        ++g_vertex_code_copied;
      }
    }
    if (copies) {
      run.list_code = std::move(copies);
    }
  }
  std::memcpy(run.constants.data(), float_constants, sizeof(run.constants));
  std::memcpy(run.loops.data(), loop_constants, sizeof(run.loops));
  g_capture_ns += Since(start);
}

}  // namespace crackdown::native::shadow
