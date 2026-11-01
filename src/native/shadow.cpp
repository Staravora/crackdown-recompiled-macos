#include "shadow.h"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../updater/sha256.h"
#include "metal_device.h"

REXCVAR_DEFINE_BOOL(crackdown_native_shadow, false, "Crackdown",
                    "Draw every frame's baked geometry in Metal, off screen, beside the emulator, and write native-shadow-<time>.json");

REXCVAR_DEFINE_BOOL(crackdown_native_shadow_window, false, "Crackdown", "Show shadow mode's picture in a window of its own");

namespace crackdown::native::shadow {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kGuestBytes = size_t(1) << 29;
constexpr uint32_t kGuestMask = uint32_t(kGuestBytes - 1);
constexpr uint32_t kAluDwords = 2048;
constexpr uint32_t kVertexConstantDwords = 1024;
constexpr size_t kVertexConstantBytes = kVertexConstantDwords * 4;
constexpr size_t kFetchBytes = 192 * 4;
constexpr size_t kSharedBytes = 512;
constexpr size_t kBooleansOffset = 256;
constexpr size_t kDrawBytes = kVertexConstantBytes + kFetchBytes + kSharedBytes;
constexpr size_t kConstantRegionBytes = 32 << 20;
constexpr size_t kIndexRegionBytes = 32 << 20;
constexpr size_t kFrameBytes = kConstantRegionBytes + kIndexRegionBytes;
constexpr size_t kPooledFrames = 3;
constexpr size_t kRunsPerFrame = 2048;
constexpr uint32_t kReportFrames = 900;
constexpr uint32_t kCaptureFrames = 300;
constexpr uint32_t kCheckFrames = 300;
constexpr uint32_t kCheckBytes = 4096;
constexpr uint32_t kMaxShaderDwords = 1 << 14;

constexpr uint32_t kSourceAutoIndex = 2;
constexpr uint32_t kPrimitiveTriangleList = 4;
constexpr uint32_t kPrimitiveTriangleStrip = 6;

struct Run {
  std::shared_ptr<const std::vector<BakedList>> lists;
  uint32_t pass_select = 0;
  uint32_t segment = 0;
  std::array<uint32_t, kVertexConstantDwords> constants{};
};

struct Frame {
  std::vector<Run> runs;
};

struct Totals {
  uint64_t frames = 0;
  uint64_t draws = 0;
  uint64_t skipped_draws = 0;
  uint64_t no_room = 0;
  uint64_t recorded_draws = 0;
  uint64_t recorded_indices = 0;
  uint64_t no_shader_draws = 0;
  uint64_t other_primitive_draws = 0;
  uint64_t index_copy_ns = 0;
  uint64_t shaders_found = 0;
  uint64_t shaders_missing = 0;
  uint64_t shaders_failed = 0;
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
uint32_t g_segment = 0;

// Render thread only from here down.
uint8_t* g_guest = nullptr;
std::filesystem::path g_path;
std::filesystem::path g_shader_dir;
std::unordered_map<std::string, std::vector<uint32_t>> g_manifest;
// By address and size. -1 means looked at and no use.
std::unordered_map<uint64_t, int> g_vertex_shaders;
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

uint32_t Word(const uint8_t* code, uint32_t dword) {
  uint32_t value;
  std::memcpy(&value, code + size_t(dword) * 4, sizeof(value));
  return std::byteswap(value);
}

// Blanks what binding a shader to a vertex declaration patches, so a live shader hashes like its shipped form. Matches
// masked_code in tools/shader-trial/pull_vertices.py.
std::string MaskedKey(const uint8_t* code, uint32_t dwords) {
  std::vector<uint32_t> words(dwords);
  std::memcpy(words.data(), code, size_t(dwords) * 4);
  const auto get = [&](uint32_t i) { return std::byteswap(words[i]); };
  const auto set = [&](uint32_t i, uint32_t value) { words[i] = std::byteswap(value); };
  uint32_t first_instruction = dwords / 3;
  for (uint32_t pair = 0; pair < first_instruction && pair * 3 + 2 < dwords; ++pair) {
    const uint32_t low = get(pair * 3), middle = get(pair * 3 + 1), high = get(pair * 3 + 2);
    const std::array<std::array<uint32_t, 2>, 2> flow = {{{low, middle & 0xFFFF}, {(middle >> 16) | (high << 16), high >> 16}}};
    for (const auto& [flow_low, flow_high] : flow) {
      const uint32_t opcode = (flow_high >> 12) & 0xF;
      const bool is_exec = (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
      if (!is_exec) {
        continue;
      }
      const uint32_t address = flow_low & 0xFFF, count = (flow_low >> 12) & 7, sequence = (flow_low >> 16) & 0xFFF;
      first_instruction = std::min(first_instruction, address);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = (address + i) * 3;
        if (((sequence >> (i * 2)) & 1) && at + 2 < dwords && (get(at) & 0x1F) == 0) {
          set(at, get(at) & 0xC00FFFFF);
          set(at + 1, get(at + 1) & 0x80000FFF);
          set(at + 2, 0);
        }
      }
    }
  }
  const std::string_view bytes(reinterpret_cast<const char*>(words.data()), size_t(dwords) * 4);
  return trg::update::Sha256Hex(bytes).substr(0, 16);
}

// What pullElement needs from each fetch, read from the live ucode. A mini fetch shares the full fetch's slot and stride.
std::vector<MetalPullElement> PullElements(const uint8_t* code, uint32_t dwords, const std::vector<uint32_t>& addresses) {
  std::vector<MetalPullElement> elements;
  uint32_t slot = 0, stride = 0;
  for (const uint32_t address : addresses) {
    if ((address + 1) * 3 > dwords) {
      return {};
    }
    const uint32_t d0 = Word(code, address * 3), d1 = Word(code, address * 3 + 1), d2 = Word(code, address * 3 + 2);
    if (!((d1 >> 30) & 1)) {
      slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3);
      stride = d2 & 0xFF;
    }
    bool reads = false;
    for (uint32_t i = 0; i < 4; ++i) {
      reads |= (((d1 & 0xFFF) >> (i * 3)) & 7) <= 3;
    }
    const uint32_t flags = ((d1 >> 12) & 7) | (uint32_t(reads) << 3);
    const int32_t exponent = (int32_t(d1 << 2) >> 26);
    const int32_t offset = int32_t(d2 << 1) >> 9;
    elements.push_back({slot | (stride << 8) | (((d1 >> 16) & 0x3F) << 16) | (flags << 24), offset,
                        float(std::ldexp(1.0, exponent))});
  }
  return elements;
}

int FindVertexShader(MetalDevice& device, const BakedDraw& draw) {
  const uint64_t key = uint64_t(draw.vertex_shader) << 32 | draw.vertex_shader_dwords;
  const auto found = g_vertex_shaders.find(key);
  if (found != g_vertex_shaders.end()) {
    return found->second;
  }
  int handle = -1;
  const uint32_t address = draw.vertex_shader & kGuestMask;
  if (draw.vertex_shader_dwords && draw.vertex_shader_dwords <= kMaxShaderDwords &&
      uint64_t(address) + draw.vertex_shader_dwords * 4ull <= kGuestBytes) {
    const uint8_t* code = g_guest + address;
    const std::string masked = MaskedKey(code, draw.vertex_shader_dwords);
    const auto manifest = g_manifest.find(masked);
    if (manifest == g_manifest.end()) {
      ++g_totals.shaders_missing;
      REXLOG_INFO("Native shadow: no vertex shader {} ({} dwords at {:08X})", masked, draw.vertex_shader_dwords, address);
    } else {
      const std::vector<MetalPullElement> elements = PullElements(code, draw.vertex_shader_dwords, manifest->second);
      std::string error;
      handle = device.AddVertexShader(g_shader_dir / ("vs_" + masked + ".metallib"), elements.data(), elements.size(), error);
      if (handle < 0) {
        ++g_totals.shaders_failed;
        REXLOG_ERROR("Native shadow: vertex shader {}: {}", masked, error);
      } else {
        ++g_totals.shaders_found;
      }
    }
  }
  g_vertex_shaders[key] = handle;
  return handle;
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
  // All this game's index buffers are big-endian. Other orders are counted in the report if that ever changes.
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

// The main scene is the stretch between resolves with the most draws, and it's the only one drawn: shadow maps and
// the like go to targets this doesn't have yet.
uint32_t MainSegment(const Frame& frame) {
  std::unordered_map<uint32_t, uint32_t> draws;
  uint32_t best = 0, best_draws = 0;
  for (const Run& run : frame.runs) {
    uint32_t& count = draws[run.segment];
    for (const BakedList& list : *run.lists) {
      count += uint32_t(list.draws.size());
    }
    if (count > best_draws) {
      best_draws = count;
      best = run.segment;
    }
  }
  return best;
}

void Encode(const Frame& frame, MetalDevice& device, uint8_t* memory) {
  const uint32_t main_segment = MainSegment(frame);
  size_t used = 0, indices_used = 0;
  for (const Run& run : frame.runs) {
    std::memcpy(g_constants.data(), run.constants.data(), sizeof(run.constants));
    const bool drawn = run.segment == main_segment;
    for (const BakedList& list : *run.lists) {
      for (const BakedDraw& draw : list.draws) {
        // Constant writes aren't predicated, so they apply either way.
        ApplyWrites(list, draw);
        if (!DrawPasses(draw, run.pass_select)) {
          ++g_totals.skipped_draws;
          continue;
        }
        ++g_totals.draws;
        if (!drawn) {
          continue;
        }
        ApplyLiterals(draw);
        const uint32_t primitive = draw.initiator & 0x3F;
        const uint32_t source = (draw.initiator >> 6) & 3;
        if ((primitive != kPrimitiveTriangleList && primitive != kPrimitiveTriangleStrip) || source == kSourceAutoIndex) {
          ++g_totals.other_primitive_draws;
          continue;
        }
        const int shader = FindVertexShader(device, draw);
        if (shader < 0) {
          ++g_totals.no_shader_draws;
          continue;
        }
        const uint32_t index_count = draw.initiator >> 16;
        const bool wide = (draw.initiator >> 11) & 1;
        size_t indices = 0;
        const Clock::time_point copy_start = Clock::now();
        const bool copied = CopyIndices(draw, index_count, wide, memory, indices_used, indices);
        g_totals.index_copy_ns += Since(copy_start);
        if (!copied || used + kDrawBytes > kConstantRegionBytes) {
          ++g_totals.no_room;
          continue;
        }
        MetalDraw out;
        out.vertex_shader = shader;
        out.depth_control = draw.depth_control;
        out.primitive = primitive == kPrimitiveTriangleStrip ? MetalPrimitive::kTriangleStrip : MetalPrimitive::kTriangleList;
        out.vertex_constants = used;
        std::memcpy(memory + used, g_constants.data(), kVertexConstantBytes);
        out.fetch_constants = used + kVertexConstantBytes;
        std::memcpy(memory + out.fetch_constants, list.fetch_data.data() + size_t(draw.fetch_constants) * 192, kFetchBytes);
        out.shared_constants = out.fetch_constants + kFetchBytes;
        std::memset(memory + out.shared_constants, 0, kSharedBytes);
        std::memcpy(memory + out.shared_constants + kBooleansOffset, &draw.booleans, sizeof(draw.booleans));
        out.indices = indices;
        out.index_count = index_count;
        out.wide_indices = wide;
        out.base_vertex = int32_t(draw.index_offset);
        device.Draw(out);
        used += kDrawBytes;
        ++g_totals.recorded_draws;
        g_totals.recorded_indices += index_count;
        if (!g_check_bytes) {
          g_check_address = draw.index_buffer & kGuestMask;
          g_check_bytes = uint32_t(std::min<uint64_t>(kCheckBytes, kGuestBytes - g_check_address));
        }
      }
    }
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
  REXLOG_INFO("Native shadow: {} vertex shaders in {}", g_manifest.size(), g_shader_dir.string());
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
      R"({{"guest_frames":{},"frames":{},"dropped":{},"draws_per_frame":{:.1f},"skipped_draws_per_frame":{:.1f},"no_room":{},)"
      R"("recorded_draws_per_frame":{:.1f},"recorded_indices_per_frame":{:.0f},"no_shader_draws_per_frame":{:.1f},"other_primitive_draws_per_frame":{:.1f},)"
      R"("shaders_found":{},"shaders_missing":{},"shaders_failed":{},"index_copy_ms_per_frame":{:.3f},)"
      R"("capture_ms_per_frame":{:.4f},"wait_ms_per_frame":{:.3f},"encode_ms_p50":{:.3f},"encode_ms_p95":{:.3f},"encode_ms_max":{:.3f},)"
      R"("gpu_frames":{},"gpu_errors":{},"gpu_ms_per_frame":{:.3f},"gpu_ms_worst":{:.3f},)"
      R"("guest_memory_wrapped":{},"guest_checks":{},"guest_matches":{}}})",
      guest_frames, g_totals.frames, dropped, g_totals.draws / frames, g_totals.skipped_draws / frames, g_totals.no_room,
      g_totals.recorded_draws / frames, g_totals.recorded_indices / frames, g_totals.no_shader_draws / frames,
      g_totals.other_primitive_draws / frames, g_totals.shaders_found, g_totals.shaders_missing, g_totals.shaders_failed,
      g_totals.index_copy_ns / 1e6 / frames,
      capture_ns / 1e6 / double(std::max<uint64_t>(guest_frames, 1)), g_totals.wait_ns / 1e6 / frames,
      Percentile(sorted, 0.5) / 1e3, Percentile(sorted, 0.95) / 1e3, Percentile(sorted, 1.0) / 1e3, gpu.frames, gpu.errors,
      gpu.gpu_ns / 1e6 / double(std::max<uint64_t>(gpu.frames, 1)), gpu.worst_gpu_ns / 1e6,
      device.guest_memory_wrapped() ? "true" : "false", g_totals.guest_checks, g_totals.guest_matches);
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
  if (REXCVAR_GET(crackdown_native_shadow_window)) {
    device->ShowWindow();
  }
  while (true) {
    std::unique_ptr<Frame> frame;
    {
      std::unique_lock lock(g_mutex);
      g_wake.wait(lock, [] { return !g_ready.empty(); });
      frame = std::move(g_ready.front());
      g_ready.pop_front();
    }
    std::filesystem::path capture;
    if ((g_totals.frames + 1) % kCaptureFrames == 0) {
      capture = g_path;
      capture.replace_extension(".png");
    }
    const MetalFrameTiming timing =
        device->RecordFrame([&](uint8_t* memory) { Encode(*frame, *device, memory); }, capture);
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

void Install(rex::memory::Memory* memory, const std::filesystem::path& shader_dir, const std::filesystem::path& output_dir) {
  if (!REXCVAR_GET(crackdown_native_shadow)) {
    return;
  }
  g_guest = memory->TranslatePhysical<uint8_t*>(0);
  g_shader_dir = shader_dir;
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
  g_segment = 0;
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

void OnResolve() {
  if (Enabled()) {
    std::lock_guard lock(g_mutex);
    ++g_segment;
  }
}

void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* vertex_constants) {
  const Clock::time_point start = Clock::now();
  std::lock_guard lock(g_mutex);
  Run& run = g_current->runs.emplace_back();
  run.lists = std::move(lists);
  run.pass_select = pass_select;
  run.segment = g_segment;
  std::memcpy(run.constants.data(), vertex_constants, sizeof(run.constants));
  g_capture_ns += Since(start);
}

}  // namespace crackdown::native::shadow
