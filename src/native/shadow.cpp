#include "shadow.h"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "metal_device.h"

REXCVAR_DEFINE_BOOL(crackdown_native_shadow, false, "Crackdown",
                    "Record every frame's baked draws in Metal, off screen, beside the emulator, and write native-shadow-<time>.json");

namespace crackdown::native::shadow {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kGuestBytes = size_t(1) << 29;
constexpr uint32_t kGuestMask = uint32_t(kGuestBytes - 1);
constexpr uint32_t kAluDwords = 2048;
constexpr uint32_t kCameraDwords = 48;
constexpr uint32_t kPixelConstantsDword = 1024;
constexpr size_t kVertexConstantBytes = 4096;
constexpr size_t kPixelConstantBytes = 1024;
constexpr size_t kDrawConstantBytes = kVertexConstantBytes + kPixelConstantBytes;
constexpr size_t kConstantBytes = 20 << 20;
constexpr size_t kPooledFrames = 3;
constexpr size_t kRunsPerFrame = 2048;
constexpr uint32_t kReportFrames = 900;
constexpr uint32_t kCheckFrames = 300;
constexpr uint32_t kCheckBytes = 4096;

struct Run {
  std::shared_ptr<const std::vector<BakedList>> lists;
  uint32_t pass_select = 0;
  std::array<uint32_t, kCameraDwords> camera{};
};

struct Frame {
  std::vector<Run> runs;
};

struct Totals {
  uint64_t frames = 0;
  uint64_t draws = 0;
  uint64_t skipped_draws = 0;
  uint64_t no_constant_room = 0;
  uint64_t wait_ns = 0;
  uint64_t guest_checks = 0;
  uint64_t guest_matches = 0;
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

// Render thread only from here down.
uint8_t* g_guest = nullptr;
std::filesystem::path g_path;
Totals g_totals;
std::vector<uint32_t> g_encode_us;
std::array<uint32_t, kAluDwords> g_constants{};
uint32_t g_check_address = 0;
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
  const uint32_t first = uint32_t(draw.literal_register) * 4;
  const uint32_t address = draw.literals & kGuestMask;
  if (!draw.literal_dwords || first + draw.literal_dwords > kAluDwords ||
      uint64_t(address) + draw.literal_dwords * 4ull > kGuestBytes) {
    return;
  }
  std::memcpy(g_constants.data() + first, g_guest + address, size_t(draw.literal_dwords) * 4);
  for (uint32_t i = 0; i < draw.literal_dwords; ++i) {
    g_constants[first + i] = std::byteswap(g_constants[first + i]);
  }
}

void Encode(const Frame& frame, MetalDevice& device, uint8_t* constants) {
  size_t used = 0;
  for (const Run& run : frame.runs) {
    std::memcpy(g_constants.data(), run.camera.data(), sizeof(run.camera));
    for (const BakedList& list : *run.lists) {
      for (const BakedDraw& draw : list.draws) {
        // Constant writes aren't predicated, so they apply either way.
        ApplyWrites(list, draw);
        if (!DrawPasses(draw, run.pass_select)) {
          ++g_totals.skipped_draws;
          continue;
        }
        ++g_totals.draws;
        ApplyLiterals(draw);
        if (used + kDrawConstantBytes > kConstantBytes) {
          ++g_totals.no_constant_room;
          continue;
        }
        std::memcpy(constants + used, g_constants.data(), kVertexConstantBytes);
        std::memcpy(constants + used + kVertexConstantBytes, g_constants.data() + kPixelConstantsDword, kPixelConstantBytes);
        device.Draw(draw.pipeline % MetalDevice::kPipelines, used, used + kVertexConstantBytes);
        used += kDrawConstantBytes;
        if (!g_check_bytes && draw.index_buffer) {
          g_check_address = draw.index_buffer & kGuestMask;
          g_check_bytes = uint32_t(std::min<uint64_t>(kCheckBytes, kGuestBytes - g_check_address));
        }
      }
    }
  }
}

uint32_t Percentile(const std::vector<uint32_t>& sorted, double share) {
  return sorted.empty() ? 0 : sorted[std::min(size_t(double(sorted.size()) * share), sorted.size() - 1)];
}

void Report(const MetalDevice& device) {
  std::vector<uint32_t> sorted = g_encode_us;
  std::sort(sorted.begin(), sorted.end());
  uint64_t guest_frames, dropped, capture_ns;
  {
    std::lock_guard lock(g_mutex);
    guest_frames = g_guest_frames;
    dropped = g_dropped;
    capture_ns = g_capture_ns;
  }
  const MetalGpuTotals gpu = device.gpu_totals();
  const double frames = double(std::max<uint64_t>(g_totals.frames, 1));
  std::ofstream out(g_path);
  out << fmt::format(
      R"({{"guest_frames":{},"frames":{},"dropped":{},"draws_per_frame":{:.1f},"skipped_draws_per_frame":{:.1f},"no_constant_room":{},)"
      R"("capture_ms_per_frame":{:.4f},"wait_ms_per_frame":{:.3f},"encode_ms_p50":{:.3f},"encode_ms_p95":{:.3f},"encode_ms_max":{:.3f},)"
      R"("gpu_frames":{},"gpu_errors":{},"gpu_ms_per_frame":{:.3f},"gpu_ms_worst":{:.3f},)"
      R"("guest_memory_wrapped":{},"guest_checks":{},"guest_matches":{}}})",
      guest_frames, g_totals.frames, dropped, g_totals.draws / frames, g_totals.skipped_draws / frames, g_totals.no_constant_room,
      capture_ns / 1e6 / double(std::max<uint64_t>(guest_frames, 1)), g_totals.wait_ns / 1e6 / frames,
      Percentile(sorted, 0.5) / 1e3, Percentile(sorted, 0.95) / 1e3, Percentile(sorted, 1.0) / 1e3, gpu.frames, gpu.errors,
      gpu.gpu_ns / 1e6 / double(std::max<uint64_t>(gpu.frames, 1)), gpu.worst_gpu_ns / 1e6,
      device.guest_memory_wrapped() ? "true" : "false", g_totals.guest_checks, g_totals.guest_matches);
}

void RenderThread() {
  pthread_setname_np("native-shadow");
  std::unique_ptr<MetalDevice> device = MetalDevice::Create(g_guest, kGuestBytes, kConstantBytes);
  if (!device) {
    REXLOG_ERROR("Native shadow: no Metal device");
    g_on.store(false, std::memory_order_relaxed);
    return;
  }
  REXLOG_INFO("Native shadow: Metal is up, guest memory wrapped: {}", device->guest_memory_wrapped());
  while (true) {
    std::unique_ptr<Frame> frame;
    {
      std::unique_lock lock(g_mutex);
      g_wake.wait(lock, [] { return !g_ready.empty(); });
      frame = std::move(g_ready.front());
      g_ready.pop_front();
    }
    const MetalFrameTiming timing =
        device->RecordFrame([&](uint8_t* constants) { Encode(*frame, *device, constants); });
    frame->runs.clear();
    {
      std::lock_guard lock(g_mutex);
      g_free.push_back(std::move(frame));
    }
    ++g_totals.frames;
    g_totals.wait_ns += timing.wait_ns;
    g_encode_us.push_back(uint32_t(timing.encode_ns / 1000));
    if (g_totals.frames % kCheckFrames == 0 && g_check_bytes) {
      ++g_totals.guest_checks;
      g_totals.guest_matches += device->MatchesGuestMemory(g_check_address, g_check_bytes);
      g_check_bytes = 0;
    }
    if (g_totals.frames % kReportFrames == 0) {
      Report(*device);
    }
  }
}

}  // namespace

bool Enabled() {
  return g_on.load(std::memory_order_relaxed);
}

void Install(rex::memory::Memory* memory, const std::filesystem::path& output_dir) {
  if (!REXCVAR_GET(crackdown_native_shadow)) {
    return;
  }
  g_guest = memory->TranslatePhysical<uint8_t*>(0);
  g_path = output_dir / fmt::format("native-shadow-{}.json", uint64_t(std::time(nullptr)));
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
  std::lock_guard lock(g_mutex);
  ++g_guest_frames;
  if (g_current->runs.empty()) {
    return;
  }
  if (g_free.empty()) {
    ++g_dropped;
    g_current->runs.clear();
    return;
  }
  g_ready.push_back(std::move(g_current));
  g_current = std::move(g_free.back());
  g_free.pop_back();
  g_wake.notify_one();
}

void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* camera) {
  const Clock::time_point start = Clock::now();
  std::lock_guard lock(g_mutex);
  Run& run = g_current->runs.emplace_back();
  run.lists = std::move(lists);
  run.pass_select = pass_select;
  std::memcpy(run.camera.data(), camera, sizeof(run.camera));
  g_capture_ns += Since(start);
}

}  // namespace crackdown::native::shadow
