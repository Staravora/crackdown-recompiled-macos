#include "native_probe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "baked_buffer.h"
#include "shadow.h"
#include "texture_probe.h"

REXCVAR_DEFINE_BOOL(crackdown_native_probe, false, "Crackdown",
                    "During a benchmark, decode the baked command buffers live, time it and write native-probe-<label>.json");

namespace crackdown::native::probe {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kDeviceVertexConstants = 1792;
constexpr uint32_t kDevicePixelConstants = 5888;
constexpr uint32_t kDeviceBinMaskLow = 12428;
constexpr uint32_t kDeviceBinMaskHigh = 12432;
constexpr uint32_t kConstantsPerStage = 256;
constexpr uint32_t kDwordsPerConstant = 4;
constexpr uint32_t kCameraConstants = 12;
constexpr uint32_t kCommandBufferFlags = 108;
constexpr uint32_t kCommandBufferSegments = 116;
constexpr uint32_t kCommandBufferNoLists = 0x20;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
constexpr uint32_t kMaxSegments = 4096;
constexpr uint32_t kMaxListsPerSegment = 4096;
constexpr uint32_t kMaxListDwords = 1 << 16;
constexpr uint32_t kSampleDwords = 64;
constexpr uint32_t kAluDwords = 2048;
constexpr size_t kReportRows = 16;

struct Buffer {
  uint32_t flags = 0;
  // Shared with shadow frames still in flight, so a list that's decoded again doesn't pull them out from under the
  // render thread.
  std::shared_ptr<const std::vector<BakedList>> lists = std::make_shared<const std::vector<BakedList>>();
  const uint8_t* sample_host = nullptr;
  uint32_t sample_dwords = 0;
  uint32_t sample = 0;
  uint64_t runs = 0;
  uint32_t changes = 0;
};

struct Totals {
  uint64_t runs = 0;
  uint64_t lists = 0;
  uint64_t draws = 0;
  uint64_t skipped_draws = 0;
  uint64_t redecodes = 0;
  uint64_t unreadable = 0;
  uint64_t constant_dwords = 0;
  uint64_t literal_dwords = 0;
  uint64_t state_changes = 0;
  uint64_t decode_ns = 0;
  uint64_t replay_ns = 0;
  uint64_t camera_ns = 0;
  uint64_t all_constants_ns = 0;
  uint64_t worst_run_ns = 0;
};

std::atomic<bool> g_window{false};
std::mutex g_mutex;
Totals g_totals;
uint64_t g_frames = 0;
BakedLibrary g_library;
std::unordered_map<uint32_t, Buffer> g_buffers;
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> g_select;

std::array<uint32_t, kAluDwords> g_constants{};
std::array<uint32_t, kAluDwords> g_device_constants{};
uint32_t g_pipeline = ~0u;
uint32_t g_textures = ~0u;
uint32_t g_dynamic = ~0u;

uint64_t Since(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

const uint8_t* GuestPointer(const uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, GuestPointer(base, address), sizeof(value));
  return std::byteswap(value);
}

// Nothing here is trusted until its pages are known to be mapped.
bool Readable(uint32_t address, uint32_t bytes) {
  auto* memory = rex::system::kernel_state()->memory();
  auto* heap = address >= 0x10000 ? memory->LookupHeap(address) : nullptr;
  if (!heap || bytes == 0 || uint64_t(address) + bytes > 0x100000000ull) {
    return false;
  }
  const uint32_t page = heap->page_size();
  for (uint64_t at = address & ~uint64_t(page - 1); at < uint64_t(address) + bytes; at += page) {
    uint32_t protect = 0;
    if (!heap->QueryProtect(uint32_t(at), &protect) || !(protect & rex::memory::kMemoryProtectRead)) {
      return false;
    }
  }
  return true;
}

// All of physical memory stays mapped on the host, so a list only has to fit inside it.
const uint8_t* HostOfList(uint32_t list, uint32_t dwords) {
  if (uint64_t(list & kPhysicalMask) + uint64_t(dwords) * 4 > kPhysicalMask + 1ull) {
    return nullptr;
  }
  return rex::system::kernel_state()->memory()->TranslatePhysical<const uint8_t*>(list);
}

uint32_t SampleOf(const Buffer& entry) {
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < entry.sample_dwords * 4; ++i) {
    hash = (hash ^ entry.sample_host[i]) * 16777619u;
  }
  return hash;
}

// Copies the words out and decodes the copy. Pointers inside a list are never followed: doing that from a game hook is
// what froze the game the first time.
void ReadLists(Buffer& entry, std::vector<BakedList>& decoded, uint32_t buffer, const uint8_t* base) {
  entry.sample_host = nullptr;
  if (!Readable(buffer, kCommandBufferSegments + 4)) {
    ++g_totals.unreadable;
    return;
  }
  entry.flags = Load32(base, buffer + kCommandBufferFlags);
  uint32_t segment = (entry.flags & kCommandBufferNoLists) ? 0 : Load32(base, buffer + kCommandBufferSegments);
  std::vector<uint32_t> words;
  for (uint32_t i = 0; segment && i < kMaxSegments; ++i) {
    if (!Readable(segment, 8)) {
      ++g_totals.unreadable;
      return;
    }
    const uint32_t lists = Load32(base, segment + 4);
    if (lists > kMaxListsPerSegment || !Readable(segment, (lists + 1) * 8)) {
      ++g_totals.unreadable;
      return;
    }
    for (uint32_t list = 0; list < lists; ++list) {
      const uint32_t entry_address = segment + (list + 1) * 8;
      const uint32_t address = Load32(base, entry_address);
      const uint32_t dwords = std::min(Load32(base, entry_address + 4) & 0xFFFFF, kMaxListDwords);
      const uint8_t* host = HostOfList(address, dwords);
      if (!host) {
        ++g_totals.unreadable;
        continue;
      }
      words.resize(dwords);
      std::memcpy(words.data(), host, size_t(dwords) * 4);
      for (uint32_t& word : words) {
        word = std::byteswap(word);
      }
      decoded.push_back(g_library.Decode(words.data(), words.size()));
      ++g_totals.lists;
      if (!entry.sample_host) {
        entry.sample_host = host;
        entry.sample_dwords = std::min(dwords, kSampleDwords);
      }
    }
    segment = Load32(base, segment);
  }
  if (entry.sample_host) {
    entry.sample = SampleOf(entry);
  }
}

void DecodeBuffer(Buffer& entry, uint32_t buffer, const uint8_t* base) {
  auto decoded = std::make_shared<std::vector<BakedList>>();
  ReadLists(entry, *decoded, buffer, base);
  entry.lists = std::move(decoded);
}

// What the native layer would do with a list at run time, short of Metal.
void Replay(const Buffer& entry, uint32_t pass_select) {
  for (const BakedList& list : *entry.lists) {
    for (const BakedDraw& draw : list.draws) {
      // Constant writes aren't predicated, so they apply either way.
      for (uint32_t i = 0; i < draw.constant_writes; ++i) {
        const BakedConstantWrite& write = list.constant_writes[draw.first_constant_write + i];
        if (write.first_dword + uint32_t(write.dwords) <= kAluDwords) {
          std::memcpy(g_constants.data() + write.first_dword, list.constant_data.data() + write.data,
                      write.dwords * sizeof(uint32_t));
          g_totals.constant_dwords += write.dwords;
        }
      }
      if (!DrawPasses(draw, pass_select)) {
        ++g_totals.skipped_draws;
        continue;
      }
      ++g_totals.draws;
      if (const uint8_t* literals = draw.literal_dwords ? HostOfList(draw.literals, draw.literal_dwords) : nullptr) {
        const uint32_t first = uint32_t(draw.literal_register) * kDwordsPerConstant;
        if (first + draw.literal_dwords <= kAluDwords) {
          std::memcpy(g_constants.data() + first, literals, size_t(draw.literal_dwords) * 4);
          for (uint32_t i = 0; i < draw.literal_dwords; ++i) {
            g_constants[first + i] = std::byteswap(g_constants[first + i]);
          }
          g_totals.literal_dwords += draw.literal_dwords;
        }
      }
      g_totals.state_changes += draw.pipeline != g_pipeline;
      g_totals.state_changes += draw.textures != g_textures;
      g_totals.state_changes += draw.dynamic != g_dynamic;
      g_pipeline = draw.pipeline;
      g_textures = draw.textures;
      g_dynamic = draw.dynamic;
    }
  }
}

void CopyDeviceConstants(uint32_t device, const uint8_t* base, uint32_t offset, uint32_t count, uint32_t first) {
  const uint32_t dwords = count * kDwordsPerConstant;
  std::memcpy(g_device_constants.data() + first, GuestPointer(base, device + offset), size_t(dwords) * 4);
  for (uint32_t i = 0; i < dwords; ++i) {
    g_device_constants[first + i] = std::byteswap(g_device_constants[first + i]);
  }
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(crackdown_native_probe);
}

void Begin() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_window.store(true, std::memory_order_relaxed);
}

void OnGuestFrame() {
  if (g_window.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_mutex);
    ++g_frames;
  }
}

void OnRunCommandBuffer(uint32_t device, uint32_t buffer, uint32_t predication, const uint8_t* base) {
  const bool timed = g_window.load(std::memory_order_relaxed) || textures::Enabled();
  if (!timed && !shadow::Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  const Clock::time_point run_start = Clock::now();

  auto [it, added] = g_buffers.try_emplace(buffer);
  Buffer& entry = it->second;
  if (added) {
    DecodeBuffer(entry, buffer, base);
    g_totals.decode_ns += Since(run_start);
  } else if (entry.sample_host) {
    if (SampleOf(entry) != entry.sample) {
      const Clock::time_point again = Clock::now();
      ++entry.changes;
      ++g_totals.redecodes;
      DecodeBuffer(entry, buffer, base);
      g_totals.decode_ns += Since(again);
    }
  }

  if (shadow::Enabled()) {
    CopyDeviceConstants(device, base, kDeviceVertexConstants, kCameraConstants, 0);
    shadow::Capture(entry.lists, predication, g_device_constants.data());
  }
  if (!timed) {
    return;
  }

  const Clock::time_point replay_start = Clock::now();
  // The game passes this straight into SET_BIN_SELECT_HI.
  Replay(entry, predication);
  g_totals.replay_ns += Since(replay_start);
  if (textures::Enabled()) {
    for (const BakedList& list : *entry.lists) {
      for (const BakedDraw& draw : list.draws) {
        if (DrawPasses(draw, predication)) {
          textures::OnFetchSet(draw.textures, g_library.texture_set(draw.textures));
        }
      }
    }
  }

  const uint64_t run_ns = Since(run_start);
  g_totals.worst_run_ns = std::max(g_totals.worst_run_ns, run_ns);

  // Two ways the native layer could pick up what lists don't carry: only the block no list writes, or everything.
  const Clock::time_point camera_start = Clock::now();
  CopyDeviceConstants(device, base, kDeviceVertexConstants, kCameraConstants, 0);
  g_totals.camera_ns += Since(camera_start);

  const Clock::time_point all_start = Clock::now();
  CopyDeviceConstants(device, base, kDeviceVertexConstants, kConstantsPerStage, 0);
  CopyDeviceConstants(device, base, kDevicePixelConstants, kConstantsPerStage, kConstantsPerStage * kDwordsPerConstant);
  g_totals.all_constants_ns += Since(all_start);

  ++g_select[{entry.flags, predication, Load32(base, device + kDeviceBinMaskLow), Load32(base, device + kDeviceBinMaskHigh)}];
  ++entry.runs;
  ++g_totals.runs;
}

void Write(const std::filesystem::path& path, const std::string& label) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_window.store(false, std::memory_order_relaxed);

  const double frames = double(std::max<uint64_t>(g_frames, 1));
  const double draws = double(std::max<uint64_t>(g_totals.draws, 1));
  uint32_t changing = 0;
  for (const auto& [address, entry] : g_buffers) {
    changing += entry.changes != 0;
  }
  std::vector<std::pair<uint64_t, std::string>> rows;
  for (const auto& [key, count] : g_select) {
    const auto& [flags, predication, mask_low, mask_high] = key;
    rows.push_back({count, fmt::format(R"({{"flags":"0x{:X}","predication":"0x{:X}","mask_low":"0x{:X}","mask_high":"0x{:X}","runs":{}}})",
                                       flags, predication, mask_low, mask_high, count)});
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string select;
  for (size_t i = 0; i < std::min(rows.size(), kReportRows); ++i) {
    select += (i ? "," : "") + rows[i].second;
  }

  std::ofstream out(path);
  out << fmt::format(
      R"({{"label":"{}","frames":{},"buffers":{},"lists":{},"changing_buffers":{},"redecodes":{},"unreadable":{},)"
      R"("runs_per_frame":{:.1f},"draws_per_frame":{:.1f},"skipped_draws_per_frame":{:.1f},)"
      R"("packets":{},"unknown_packets":{},"truncated_packets":{},"pipelines":{},"texture_sets":{},"dynamic_states":{},)"
      R"("constant_dwords_per_draw":{:.1f},"literal_dwords_per_draw":{:.1f},"state_changes_per_draw":{:.2f},)"
      R"("decode_ms_total":{:.3f},"replay_ms_per_frame":{:.4f},"replay_us_per_draw":{:.4f},"worst_run_us":{:.1f},)"
      R"("camera_constants_ms_per_frame":{:.4f},"all_constants_ms_per_frame":{:.4f},"select":[{}]}})",
      label, g_frames, g_buffers.size(), g_totals.lists, changing, g_totals.redecodes, g_totals.unreadable,
      g_totals.runs / frames, g_totals.draws / frames, g_totals.skipped_draws / frames,
      g_library.stats().packets, g_library.stats().unknown_packets, g_library.stats().truncated_packets,
      g_library.pipeline_count(), g_library.texture_set_count(), g_library.dynamic_state_count(),
      g_totals.constant_dwords / draws, g_totals.literal_dwords / draws, g_totals.state_changes / draws,
      g_totals.decode_ns / 1e6, g_totals.replay_ns / 1e6 / frames, g_totals.replay_ns / 1e3 / draws,
      g_totals.worst_run_ns / 1e3, g_totals.camera_ns / 1e6 / frames, g_totals.all_constants_ns / 1e6 / frames, select);
  REXLOG_INFO("Native probe: {} buffers, {} unknown packets", g_buffers.size(), g_library.stats().unknown_packets);
}

}  // namespace crackdown::native::probe
