#include "texture_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(crackdown_texture_probe, false, "Crackdown",
                    "Track when textures change against a slow hash of their memory, and write texture-probe-<time>.json");

namespace crackdown::native::textures {
namespace {

namespace xenos = rex::graphics::xenos;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kDeviceFetchConstants = 1024;
constexpr uint32_t kFetchConstantBytes = 24;
constexpr uint32_t kFetchConstantCount = 32;
constexpr uint32_t kTextureFetchConstant = 28;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
constexpr uint32_t kPhysical4kView = 0xE0000000;
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPages = 1 << 17;
constexpr uint32_t kMaxFlushBytes = 64 << 20;
constexpr uint64_t kHashBytesPerFrame = 4 << 20;
constexpr uint32_t kReportFrames = 900;
constexpr uint32_t kRecentFrames = 30;
constexpr uint32_t kSamples = 8;
constexpr uint32_t kSampleBytes = 64;
constexpr size_t kReportRows = 24;

struct Range {
  uint32_t start = 0;
  uint32_t bytes = 0;
};

struct Texture {
  Range base;
  Range mips;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t tiled = 0;
  uint64_t hash = 0;
  bool hashed = false;
  bool resolved = false;
  bool locked = false;
  bool sampled = false;
  bool sample_changed = false;
  uint64_t sample = 0;
  uint32_t checked_frame = 0;
  uint32_t bound_frame = 0;
  uint32_t misses = 0;
  uint32_t changes = 0;
};

struct Totals {
  uint64_t flushes = 0;
  uint64_t flush_bytes = 0;
  uint64_t flush_ns = 0;
  uint64_t locks = 0;
  uint64_t resolves = 0;
  uint64_t bound_textures = 0;
  uint64_t bound_bytes = 0;
  uint64_t bind_ns = 0;
  uint64_t new_textures = 0;
  uint64_t new_bytes = 0;
  uint64_t hashed_bytes = 0;
  uint64_t hash_ns = 0;
  uint64_t checks = 0;
  uint64_t changed_flushed = 0;
  uint64_t changed_locked = 0;
  uint64_t changed_resolved = 0;
  uint64_t changed_missed = 0;
  uint64_t changed_sample_only = 0;
  uint64_t sample_caught = 0;
  uint64_t sample_missed = 0;
  uint64_t sample_ns = 0;
  uint64_t sampled_textures = 0;
  uint64_t changed_bytes = 0;
  uint64_t missed_bytes = 0;
  uint64_t false_alarms = 0;
  uint64_t false_alarm_bytes = 0;
  uint64_t worst_new_bytes_in_frame = 0;
};

std::atomic<bool> g_on{false};
std::mutex g_mutex;
uint8_t* g_membase = nullptr;
std::filesystem::path g_path;
uint32_t g_frame = 1;
Totals g_totals;
std::map<uint64_t, Texture> g_textures;
std::vector<uint64_t> g_order;
size_t g_cursor = 0;
std::vector<uint32_t> g_flushed(kPages, 0);
std::vector<uint32_t> g_resolved(kPages, 0);
std::vector<uint32_t> g_set_frame;
std::vector<uint64_t> g_bound_now;
std::array<std::array<uint8_t, kFetchConstantBytes>, kFetchConstantCount> g_bound{};
uint64_t g_frame_new_bytes = 0;
std::vector<std::string> g_timeline;
Totals g_last_row;

uint64_t Since(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

uint32_t Physical(uint32_t address) {
  return (address & kPhysicalMask) + (address >= kPhysical4kView ? 0x1000 : 0);
}

struct BlockInfo {
  uint32_t texels;
  uint32_t bytes;
};

BlockInfo BlockOf(uint32_t format) {
  switch (xenos::TextureFormat(format)) {
    case xenos::TextureFormat::k_8:
      return {1, 1};
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_4_4_4_4:
      return {1, 2};
    case xenos::TextureFormat::k_DXT1:
      return {4, 8};
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
      return {4, 16};
    default:
      return {1, 4};
  }
}

xenos::xe_gpu_texture_fetch_t Decode(const uint32_t* dwords) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, dwords, sizeof(fetch));
  return fetch;
}

// An estimate that errs large: mips are taken as a third of the base, and the layout's own padding is left out.
void Measure(const xenos::xe_gpu_texture_fetch_t& fetch, Texture& texture) {
  const BlockInfo block = BlockOf(uint32_t(fetch.format));
  const bool is_3d = fetch.dimension == xenos::DataDimension::k3D;
  const bool is_cube = fetch.dimension == xenos::DataDimension::kCube;
  const uint32_t width = (is_3d ? uint32_t(fetch.size_3d.width) : uint32_t(fetch.size_2d.width)) + 1;
  const uint32_t height = (is_3d ? uint32_t(fetch.size_3d.height) : uint32_t(fetch.size_2d.height)) + 1;
  const uint32_t depth = is_3d ? uint32_t(fetch.size_3d.depth) + 1 : (is_cube ? 6 : 1);
  const uint32_t pitch = std::max(uint32_t(fetch.pitch) << 5, width);
  uint32_t row_blocks = (pitch + block.texels - 1) / block.texels;
  uint32_t rows = (height + block.texels - 1) / block.texels;
  if (fetch.tiled) {
    row_blocks = (row_blocks + 31) & ~31u;
    rows = (rows + 31) & ~31u;
  }
  const uint64_t bytes = uint64_t(row_blocks) * rows * block.bytes * depth;
  texture.format = uint32_t(fetch.format);
  texture.width = width;
  texture.height = height;
  texture.tiled = uint32_t(fetch.tiled);
  texture.base = {Physical(uint32_t(fetch.base_address) << 12), uint32_t(std::min<uint64_t>(bytes, 256 << 20))};
  texture.mips = {};
  if (fetch.mip_address) {
    texture.mips = {Physical(uint32_t(fetch.mip_address) << 12), std::max<uint32_t>(texture.base.bytes / 3, 4096)};
  }
}

const uint8_t* HostOf(const Range& range) {
  if (!range.bytes || uint64_t(range.start) + range.bytes > kPhysicalMask + 1ull) {
    return nullptr;
  }
  return rex::system::kernel_state()->memory()->TranslatePhysical<const uint8_t*>(range.start);
}

uint64_t HashRange(uint64_t hash, const Range& range) {
  const uint8_t* host = HostOf(range);
  if (!host) {
    return hash;
  }
  uint64_t word;
  size_t at = 0;
  for (; at + 8 <= range.bytes; at += 8) {
    std::memcpy(&word, host + at, 8);
    hash = (std::rotl(hash, 23) ^ word) * 0x9E3779B97F4A7C15ull;
  }
  return hash;
}

bool MarkedSince(const std::vector<uint32_t>& pages, const Range& range, uint32_t since) {
  if (!range.bytes) {
    return false;
  }
  const uint32_t first = range.start >> kPageShift;
  const uint32_t last = std::min((uint64_t(range.start) + range.bytes - 1) >> kPageShift, uint64_t(kPages - 1));
  for (uint32_t page = first; page <= last; ++page) {
    if (pages[page] >= since) {
      return true;
    }
  }
  return false;
}

void Mark(std::vector<uint32_t>& pages, const Range& range) {
  if (!range.bytes) {
    return;
  }
  const uint32_t first = range.start >> kPageShift;
  const uint32_t last = std::min((uint64_t(range.start) + range.bytes - 1) >> kPageShift, uint64_t(kPages - 1));
  for (uint32_t page = first; page <= last; ++page) {
    pages[page] = g_frame;
  }
}

void Bind(const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (uint32_t(fetch.type) != uint32_t(xenos::FetchConstantType::kTexture) || !fetch.base_address) {
    return;
  }
  // The same memory can be viewed in several shapes, so each view is its own entry.
  Texture shape;
  Measure(fetch, shape);
  const uint64_t key = uint64_t(shape.base.start) << 32 |
                       (shape.format << 26 ^ shape.width << 13 ^ shape.height ^ shape.tiled << 25);
  auto [it, added] = g_textures.try_emplace(key, shape);
  Texture& texture = it->second;
  if (added) {
    g_order.push_back(key);
    ++g_totals.new_textures;
    const uint64_t bytes = uint64_t(texture.base.bytes) + texture.mips.bytes;
    g_totals.new_bytes += bytes;
    g_frame_new_bytes += bytes;
  }
  if (texture.bound_frame != g_frame) {
    texture.bound_frame = g_frame;
    g_bound_now.push_back(key);
    ++g_totals.bound_textures;
    g_totals.bound_bytes += uint64_t(texture.base.bytes) + texture.mips.bytes;
  }
}

// The cheap check a renderer could afford: a few spots across each texture bound this frame.
uint64_t SampleOf(const Range& range) {
  const uint8_t* host = HostOf(range);
  if (!host || range.bytes < kSampleBytes) {
    return 0;
  }
  uint64_t hash = 0x9E3779B97F4A7C15ull, word;
  const uint32_t step = (range.bytes - kSampleBytes) / (kSamples - 1);
  for (uint32_t i = 0; i < kSamples; ++i) {
    for (uint32_t at = 0; at < kSampleBytes; at += 8) {
      std::memcpy(&word, host + i * step + at, 8);
      hash = (std::rotl(hash, 23) ^ word) * 0x9E3779B97F4A7C15ull;
    }
  }
  return hash;
}

void SampleBound() {
  const Clock::time_point start = Clock::now();
  for (const uint64_t key : g_bound_now) {
    Texture& texture = g_textures[key];
    const uint64_t sample = SampleOf(texture.base) ^ std::rotl(SampleOf(texture.mips), 1);
    texture.sample_changed |= texture.sampled && sample != texture.sample;
    texture.sample = sample;
    texture.sampled = true;
  }
  g_totals.sampled_textures += g_bound_now.size();
  g_bound_now.clear();
  g_totals.sample_ns += Since(start);
}

// The truth: a change here happened whether or not anything announced it.
void CheckTextures() {
  const Clock::time_point start = Clock::now();
  uint64_t budget = kHashBytesPerFrame;
  size_t visited = 0;
  while (budget && visited < g_order.size()) {
    if (g_cursor >= g_order.size()) {
      g_cursor = 0;
    }
    Texture& texture = g_textures[g_order[g_cursor++]];
    ++visited;
    // Only what's in use matters. A change while unbound shows at the first check after the next bind.
    if (texture.bound_frame + kRecentFrames < g_frame) {
      continue;
    }
    const uint64_t bytes = uint64_t(texture.base.bytes) + texture.mips.bytes;
    const uint64_t hash = HashRange(HashRange(0x9E3779B97F4A7C15ull, texture.base), texture.mips);
    g_totals.hashed_bytes += bytes;
    budget -= std::min(budget, bytes);
    const bool flushed = MarkedSince(g_flushed, texture.base, texture.checked_frame) ||
                         MarkedSince(g_flushed, texture.mips, texture.checked_frame);
    const bool resolved = MarkedSince(g_resolved, texture.base, texture.checked_frame) ||
                          MarkedSince(g_resolved, texture.mips, texture.checked_frame);
    texture.resolved |= texture.hashed && resolved;
    if (texture.hashed) {
      ++g_totals.checks;
      if (hash != texture.hash) {
        ++texture.changes;
        g_totals.changed_bytes += bytes;
        ++(texture.sample_changed ? g_totals.sample_caught : g_totals.sample_missed);
        if (flushed) {
          ++g_totals.changed_flushed;
        } else if (texture.locked) {
          ++g_totals.changed_locked;
        } else if (resolved) {
          ++g_totals.changed_resolved;
        } else if (texture.sample_changed) {
          ++g_totals.changed_sample_only;
        } else {
          ++g_totals.changed_missed;
          g_totals.missed_bytes += bytes;
          ++texture.misses;
        }
      } else if (flushed) {
        ++g_totals.false_alarms;
        g_totals.false_alarm_bytes += bytes;
      }
    }
    texture.hash = hash;
    texture.hashed = true;
    texture.locked = false;
    texture.sample_changed = false;
    texture.checked_frame = g_frame + 1;
  }
  g_totals.hash_ns += Since(start);
}

void Report() {
  const double frames = double(g_frame);
  uint64_t all_bytes = 0, resolved_count = 0;
  uint64_t missed_tiled = 0, missed_linear = 0, linear_bound_bytes = 0, linear_bound = 0;
  std::vector<const Texture*> missed;
  for (const auto& [base, texture] : g_textures) {
    all_bytes += uint64_t(texture.base.bytes) + texture.mips.bytes;
    resolved_count += texture.resolved;
    if (texture.misses) {
      missed.push_back(&texture);
      (texture.tiled ? missed_tiled : missed_linear) += texture.misses;
    }
    if (!texture.tiled && !texture.resolved && texture.bound_frame + kRecentFrames >= g_frame) {
      ++linear_bound;
      linear_bound_bytes += uint64_t(texture.base.bytes) + texture.mips.bytes;
    }
  }
  // Tiled ones first: linear planes are the movie player's, and they'd fill the list.
  std::sort(missed.begin(), missed.end(), [](const Texture* a, const Texture* b) {
    return a->tiled != b->tiled ? a->tiled > b->tiled : a->misses > b->misses;
  });
  std::string rows;
  for (size_t i = 0; i < std::min(missed.size(), kReportRows); ++i) {
    const Texture& t = *missed[i];
    rows += fmt::format(R"({}{{"base":"0x{:X}","format":{},"width":{},"height":{},"tiled":{},"bytes":{},"misses":{},"changes":{}}})",
                        i ? "," : "", t.base.start, t.format, t.width, t.height, t.tiled, t.base.bytes, t.misses, t.changes);
  }
  const Totals& d = g_last_row;
  g_timeline.push_back(fmt::format(
      R"({{"frame":{},"textures":{},"new_mb":{:.2f},"changed_flushed":{},"changed_missed":{},"flushes":{},"flush_mb":{:.2f}}})", g_frame,
      g_textures.size(), (g_totals.new_bytes - d.new_bytes) / 1048576.0, g_totals.changed_flushed - d.changed_flushed,
      g_totals.changed_missed - d.changed_missed, g_totals.flushes - d.flushes,
      (g_totals.flush_bytes - d.flush_bytes) / 1048576.0));
  g_last_row = g_totals;
  std::string timeline;
  for (size_t i = 0; i < g_timeline.size(); ++i) {
    timeline += (i ? "," : "") + g_timeline[i];
  }

  std::ofstream out(g_path);
  out << fmt::format(
      R"({{"frames":{},"textures":{},"resolved_textures":{},"all_mb":{:.1f},"new_mb":{:.1f},"worst_new_mb_in_frame":{:.2f},)"
      R"("bound_textures_per_frame":{:.1f},"bound_mb_per_frame":{:.1f},"bind_ms_per_frame":{:.4f},)"
      R"("flushes_per_frame":{:.1f},"flush_kb_per_frame":{:.1f},"flush_ms_per_frame":{:.4f},"locks":{},"resolves":{},)"
      R"("checks":{},"changed_flushed":{},"changed_locked":{},"changed_resolved":{},"changed_missed":{},)"
      R"("missed_tiled":{},"missed_linear":{},"linear_recent":{},"linear_recent_mb":{:.1f},"changed_mb":{:.1f},"missed_mb":{:.1f},"false_alarms":{},"false_alarm_mb":{:.1f},)"
      R"("changed_sample_only":{},"sample_caught":{},"sample_missed":{},"sample_ms_per_frame":{:.4f},)"
      R"("hash_gb_per_s":{:.2f},"hashed_gb":{:.2f},"missed":[{}],"timeline":[{}]}})",
      g_frame, g_textures.size(), resolved_count, all_bytes / 1048576.0, g_totals.new_bytes / 1048576.0,
      g_totals.worst_new_bytes_in_frame / 1048576.0, g_totals.bound_textures / frames,
      g_totals.bound_bytes / frames / 1048576.0, g_totals.bind_ns / 1e6 / frames, g_totals.flushes / frames,
      g_totals.flush_bytes / frames / 1024.0, g_totals.flush_ns / 1e6 / frames, g_totals.locks, g_totals.resolves, g_totals.checks,
      g_totals.changed_flushed, g_totals.changed_locked, g_totals.changed_resolved, g_totals.changed_missed,
      missed_tiled, missed_linear, linear_bound, linear_bound_bytes / 1048576.0, g_totals.changed_bytes / 1048576.0, g_totals.missed_bytes / 1048576.0, g_totals.false_alarms,
      g_totals.false_alarm_bytes / 1048576.0, g_totals.changed_sample_only, g_totals.sample_caught, g_totals.sample_missed,
      g_totals.sample_ns / 1e6 / frames, g_totals.hash_ns ? double(g_totals.hashed_bytes) / g_totals.hash_ns : 0.0,
      g_totals.hashed_bytes / 1e9, rows, timeline);
}

}  // namespace

bool Enabled() {
  return g_on.load(std::memory_order_relaxed);
}

void Install(uint8_t* membase, const std::filesystem::path& output_dir) {
  if (!REXCVAR_GET(crackdown_texture_probe)) {
    return;
  }
  g_membase = membase;
  g_path = output_dir / fmt::format("texture-probe-{}.json", uint64_t(std::time(nullptr)));
  g_on.store(true, std::memory_order_relaxed);
  REXLOG_INFO("Texture probe: writing {}", g_path.string());
}

void OnGuestFrame() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  SampleBound();
  CheckTextures();
  g_totals.worst_new_bytes_in_frame = std::max(g_totals.worst_new_bytes_in_frame, g_frame_new_bytes);
  g_frame_new_bytes = 0;
  ++g_frame;
  if (g_frame % kReportFrames == 0) {
    Report();
  }
}

void OnDraw(uint32_t device) {
  if (!Enabled()) {
    return;
  }
  const uint8_t* constants = g_membase + device + kDeviceFetchConstants + rex::memory::detail::PhysicalHostOffset(device);
  std::lock_guard lock(g_mutex);
  const Clock::time_point start = Clock::now();
  for (uint32_t i = 0; i < kFetchConstantCount; ++i) {
    const uint8_t* constant = constants + i * kFetchConstantBytes;
    if (std::memcmp(g_bound[i].data(), constant, kFetchConstantBytes) != 0) {
      std::memcpy(g_bound[i].data(), constant, kFetchConstantBytes);
      std::array<uint32_t, 6> dwords;
      std::memcpy(dwords.data(), constant, sizeof(dwords));
      for (uint32_t& dword : dwords) {
        dword = std::byteswap(dword);
      }
      Bind(Decode(dwords.data()));
    }
  }
  g_totals.bind_ns += Since(start);
}

void OnFetchSet(uint32_t set, const uint32_t* fetch_dwords) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (set >= g_set_frame.size()) {
    g_set_frame.resize(set + 1, 0);
  }
  if (g_set_frame[set] == g_frame) {
    return;
  }
  g_set_frame[set] = g_frame;
  const Clock::time_point start = Clock::now();
  for (uint32_t i = 0; i < kFetchConstantCount; ++i) {
    Bind(Decode(fetch_dwords + i * 6));
  }
  g_totals.bind_ns += Since(start);
}

void OnCacheFlush(uint32_t start_address, uint32_t end_address) {
  if (!Enabled() || end_address <= start_address || end_address - start_address > kMaxFlushBytes) {
    return;
  }
  std::lock_guard lock(g_mutex);
  const Clock::time_point start = Clock::now();
  const uint32_t physical = Physical(start_address);
  const uint32_t first = physical >> kPageShift;
  const uint32_t last = std::min((physical + (end_address - start_address) - 1) >> kPageShift, kPages - 1);
  for (uint32_t page = first; page <= last; ++page) {
    g_flushed[page] = g_frame;
  }
  ++g_totals.flushes;
  g_totals.flush_bytes += end_address - start_address;
  g_totals.flush_ns += Since(start);
}

void OnLock(uint32_t texture) {
  if (!Enabled() || !texture) {
    return;
  }
  const uint8_t* constant = g_membase + texture + kTextureFetchConstant + rex::memory::detail::PhysicalHostOffset(texture);
  std::array<uint32_t, 6> dwords;
  std::memcpy(dwords.data(), constant, sizeof(dwords));
  for (uint32_t& dword : dwords) {
    dword = std::byteswap(dword);
  }
  std::lock_guard lock(g_mutex);
  ++g_totals.locks;
  const uint64_t first = uint64_t(Physical(uint32_t(Decode(dwords.data()).base_address) << 12)) << 32;
  for (auto it = g_textures.lower_bound(first); it != g_textures.end() && (it->first >> 32) == (first >> 32); ++it) {
    it->second.locked = true;
  }
}

void OnResolve(uint32_t texture) {
  if (!Enabled() || !texture) {
    return;
  }
  const uint8_t* constant = g_membase + texture + kTextureFetchConstant + rex::memory::detail::PhysicalHostOffset(texture);
  std::array<uint32_t, 6> dwords;
  std::memcpy(dwords.data(), constant, sizeof(dwords));
  for (uint32_t& dword : dwords) {
    dword = std::byteswap(dword);
  }
  const xenos::xe_gpu_texture_fetch_t fetch = Decode(dwords.data());
  Texture written;
  Measure(fetch, written);
  std::lock_guard lock(g_mutex);
  ++g_totals.resolves;
  // By page, because other textures can share this memory.
  Mark(g_resolved, written.base);
  Mark(g_resolved, written.mips);
}

}  // namespace crackdown::native::textures
