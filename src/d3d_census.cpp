#include "d3d_census.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "guest_hooks.h"
#include "native/native_probe.h"
#include "native/shadow.h"
#include "native/texture_probe.h"

REXCVAR_DEFINE_BOOL(crackdown_d3d_census, false, "Crackdown",
                    "During a benchmark, count the game's Direct3D calls and write census-<label>.json");

namespace crackdown::census {

namespace xenos = rex::graphics::xenos;
using Clock = std::chrono::steady_clock;

// Fields of the guest D3D device and of its resources.
constexpr uint32_t kDeviceFetchConstants = 1024;
constexpr uint32_t kFetchConstantBytes = 24;
constexpr uint32_t kFetchConstantCount = 32;
constexpr uint32_t kDeviceRenderTargets = 12160;
constexpr uint32_t kDeviceBinMaskLow = 12428;
constexpr uint32_t kDeviceBinMaskHigh = 12432;
constexpr uint32_t kDeviceTileCount = 12468;
constexpr uint32_t kDeviceTileRects = 12472;
constexpr uint32_t kTextureFetchConstant = 28;
constexpr uint32_t kCommandBufferFlags = 108;
constexpr uint32_t kCommandBufferSegments = 116;
constexpr uint32_t kCommandBufferNoLists = 0x20;

constexpr uint32_t kMaxTiles = 16;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
constexpr uint32_t kPhysical4kView = 0xE0000000;
constexpr uint32_t kMaxSegments = 4096;
constexpr uint32_t kMaxListsPerSegment = 4096;
constexpr uint32_t kSampleDwords = 64;
constexpr uint64_t kDumpDwordsPerList = 1 << 16;
constexpr uint64_t kDumpBudgetDwords = 1 << 24;
constexpr uint32_t kIssueBegin = 2;
constexpr size_t kReportRows = 40;

enum DrawKind { kRingVertices, kRingIndices, kVertices, kIndices, kDrawKinds };
constexpr std::array<const char*, kDrawKinds> kDrawNames = {"ring_vertices", "ring_indexed", "vertices", "indexed"};

struct FrameRow {
  std::array<uint32_t, kDrawKinds> draws{};
  uint32_t tiled_draws = 0;
  uint32_t tile_blocks = 0;
  uint32_t tiles = 0;
  uint32_t clears = 0;
  uint32_t resolves = 0;
  uint32_t texture_binds = 0;
  uint32_t queries = 0;
  uint32_t locks = 0;
  uint32_t command_buffers = 0;
  uint32_t flushes = 0;
  uint32_t flush_kb = 0;
  uint64_t draw_ns = 0;
  uint64_t resolve_ns = 0;
};

struct TextureShape {
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t tiled;
  uint32_t dimension;
  auto operator<=>(const TextureShape&) const = default;
};

struct Texture {
  TextureShape shape{};
  uint64_t binds = 0;
};

struct ResolveKey {
  uint32_t flags;
  bool from_end_tiling;
  std::array<int32_t, 4> source;
  TextureShape dest;
  auto operator<=>(const ResolveKey&) const = default;
};

struct ResolveStats {
  uint64_t count = 0;
  std::set<uint32_t> bases;
};

std::atomic<bool> g_window{false};
std::mutex g_mutex;

std::vector<FrameRow> g_frames;
FrameRow g_row;
std::array<uint64_t, 32> g_primitives{};
std::array<uint64_t, kDrawKinds> g_elements{};
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_predication;
std::map<std::string, uint64_t> g_tile_sets;
std::map<std::tuple<uint32_t, uint32_t>, uint64_t> g_command_buffers;

// A command buffer's lists, noted the first time it runs. Their packets are only copied out when the window ends and
// are parsed offline: lists can chain into the ring, and following them here froze the game.
struct BufferContents {
  uint32_t flags = 0;
  uint32_t segments = 0;
  uint32_t dwords = 0;
  uint32_t unreadable = 0;
  const uint8_t* sample_host = nullptr;
  uint32_t sample_dwords = 0;
  uint32_t sample = 0;
  uint32_t sample_changes = 0;
  uint64_t runs = 0;
  std::vector<std::pair<uint32_t, uint32_t>> lists;
};
std::map<uint32_t, BufferContents> g_buffer_contents;
std::map<ResolveKey, ResolveStats> g_resolves;
std::map<uint64_t, uint64_t> g_threads;
bool g_tiling = false;

std::array<std::array<uint8_t, kFetchConstantBytes>, kFetchConstantCount> g_bound{};
std::map<uint32_t, Texture> g_textures;
std::set<uint32_t> g_used_bases;
std::set<uint32_t> g_locked_bases;

// Kept from process start: a texture created at boot must not count as streamed in the window.
std::set<uint32_t> g_created_bases;

thread_local bool t_in_end_tiling = false;

// The 4 KB page view sits a page further on in host memory, so a guest pointer can't simply be added to the base.
const uint8_t* GuestPointer(const uint8_t* base, uint32_t address) {
  return base + address + rex::memory::detail::PhysicalHostOffset(address);
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, GuestPointer(base, address), sizeof(value));
  return std::byteswap(value);
}

int32_t LoadS32(const uint8_t* base, uint32_t address) {
  return std::bit_cast<int32_t>(Load32(base, address));
}

xenos::xe_gpu_texture_fetch_t DecodeFetch(const uint8_t* constant) {
  std::array<uint32_t, 6> dwords;
  std::memcpy(dwords.data(), constant, sizeof(dwords));
  for (uint32_t& dword : dwords) {
    dword = std::byteswap(dword);
  }
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, dwords.data(), sizeof(fetch));
  return fetch;
}

// Resources hold a virtual address and the device a physical one. The 4 KB page view is offset by a page.
uint32_t PhysicalOf(uint32_t address) {
  return (address & kPhysicalMask) + (address >= kPhysical4kView ? 0x1000 : 0);
}

uint32_t BaseOf(const xenos::xe_gpu_texture_fetch_t& fetch) {
  return PhysicalOf(uint32_t(fetch.base_address) << 12);
}

TextureShape ShapeOf(const xenos::xe_gpu_texture_fetch_t& fetch) {
  return {uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1,
          uint32_t(fetch.tiled), uint32_t(fetch.dimension)};
}

bool IsTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  return uint32_t(fetch.type) == uint32_t(xenos::FetchConstantType::kTexture);
}

// Only the slots that changed since the last draw are decoded.
void SampleBoundTextures(uint32_t device, const uint8_t* base) {
  for (uint32_t i = 0; i < kFetchConstantCount; ++i) {
    const uint8_t* constant = GuestPointer(base, device + kDeviceFetchConstants + i * kFetchConstantBytes);
    if (std::memcmp(g_bound[i].data(), constant, kFetchConstantBytes) == 0) {
      continue;
    }
    std::memcpy(g_bound[i].data(), constant, kFetchConstantBytes);
    const xenos::xe_gpu_texture_fetch_t fetch = DecodeFetch(constant);
    if (!IsTexture(fetch)) {
      continue;
    }
    const uint32_t texture_base = BaseOf(fetch);
    Texture& texture = g_textures[texture_base];
    texture.shape = ShapeOf(fetch);
    ++texture.binds;
    ++g_row.texture_binds;
    g_used_bases.insert(texture_base);
  }
}

// The census must never take the game down, so nothing it follows is trusted until the pages are known to be mapped.
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

BufferContents& ContentsOf(uint32_t buffer, const uint8_t* base) {
  auto [it, added] = g_buffer_contents.try_emplace(buffer);
  BufferContents& contents = it->second;
  if (added && Readable(buffer, kCommandBufferSegments + 4)) {
    contents.flags = Load32(base, buffer + kCommandBufferFlags);
    uint32_t segment = (contents.flags & kCommandBufferNoLists) ? 0 : Load32(base, buffer + kCommandBufferSegments);
    for (uint32_t i = 0; segment && i < kMaxSegments; ++i) {
      if (!Readable(segment, 8)) {
        ++contents.unreadable;
        break;
      }
      const uint32_t lists = Load32(base, segment + 4);
      if (lists > kMaxListsPerSegment || !Readable(segment, (lists + 1) * 8)) {
        ++contents.unreadable;
        break;
      }
      ++contents.segments;
      for (uint32_t list = 0; list < lists; ++list) {
        const uint32_t entry = segment + (list + 1) * 8;
        const uint32_t dwords = Load32(base, entry + 4) & 0xFFFFF;
        contents.lists.push_back({Load32(base, entry), dwords});
        contents.dwords += dwords;
      }
      segment = Load32(base, segment);
    }
    if (!contents.lists.empty()) {
      const auto [list, dwords] = contents.lists.front();
      contents.sample_dwords = std::min(dwords, kSampleDwords);
      contents.sample_host = HostOfList(list, contents.sample_dwords);
    }
  }
  return contents;
}

// Whether the buffer is rewritten between runs: a recorded buffer changes, a baked one should not.
void SampleContents(BufferContents& contents) {
  if (!contents.sample_host) {
    return;
  }
  uint32_t hash = 2166136261u;
  for (uint32_t i = 0; i < contents.sample_dwords * 4; ++i) {
    hash = (hash ^ contents.sample_host[i]) * 16777619u;
  }
  contents.sample_changes += contents.runs && hash != contents.sample;
  contents.sample = hash;
}

// One record per list: buffer, flags, list address, runs, total dwords, copied dwords, then the copied dwords as the guest
// stored them. At the end of the window, so the copy can't disturb what was measured.
void DumpLists(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary);
  uint64_t budget = kDumpBudgetDwords;
  for (const auto& [buffer, contents] : g_buffer_contents) {
    for (const auto& [list, dwords] : contents.lists) {
      const uint32_t copied = uint32_t(std::min<uint64_t>({dwords, kDumpDwordsPerList, budget}));
      const uint8_t* host = copied ? HostOfList(list, copied) : nullptr;
      const std::array<uint32_t, 6> record = {buffer, contents.flags, list, uint32_t(contents.runs), dwords, host ? copied : 0};
      out.write(reinterpret_cast<const char*>(record.data()), sizeof(record));
      if (host) {
        out.write(reinterpret_cast<const char*>(host), copied * 4);
        budget -= copied;
      }
    }
  }
}

void CountDraw(DrawKind kind, uint32_t elements, PPCContext& ctx, uint8_t* base) {
  const uint32_t device = ctx.r3.u32;
  std::lock_guard lock(g_mutex);
  ++g_row.draws[kind];
  g_elements[kind] += elements;
  g_primitives[ctx.r4.u32 & 31] += 1;
  g_row.tiled_draws += g_tiling;
  if (g_tiling) {
    ++g_predication[{Load32(base, device + kDeviceBinMaskLow), Load32(base, device + kDeviceBinMaskHigh)}];
  }
  ++g_threads[std::hash<std::thread::id>{}(std::this_thread::get_id())];
  SampleBoundTextures(device, base);
}

void RunTimed(PPCContext& ctx, uint8_t* base, hooks::GuestFunction original, uint64_t FrameRow::*total) {
  const auto start = Clock::now();
  original(ctx, base);
  const uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  std::lock_guard lock(g_mutex);
  g_row.*total += ns;
}

bool Counting() {
  return g_window.load(std::memory_order_relaxed);
}

uint32_t Median(std::vector<uint32_t> values) {
  if (values.empty()) {
    return 0;
  }
  std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
  return values[values.size() / 2];
}

uint32_t TotalDraws(const FrameRow& row) {
  return row.draws[kRingVertices] + row.draws[kRingIndices] + row.draws[kVertices] + row.draws[kIndices];
}

template <typename Field>
std::string MedianMax(Field field) {
  std::vector<uint32_t> values;
  for (const FrameRow& row : g_frames) {
    values.push_back(field(row));
  }
  const uint32_t max = values.empty() ? 0 : *std::max_element(values.begin(), values.end());
  return fmt::format(R"({{"median":{},"max":{}}})", Median(values), max);
}

std::string ShapeJson(const TextureShape& shape) {
  return fmt::format(R"("format":{},"width":{},"height":{},"tiled":{},"dimension":{})", shape.format, shape.width,
                     shape.height, shape.tiled, shape.dimension);
}

template <typename Row>
std::vector<Row> Busiest(std::vector<Row> rows) {
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });
  rows.resize(std::min(rows.size(), kReportRows));
  return rows;
}

std::string Join(const std::vector<std::string>& parts) {
  return fmt::format("[{}]", fmt::join(parts, ","));
}

std::string ReportTextures() {
  struct Row {
    TextureShape shape;
    const char* origin;
    uint64_t count;
    uint64_t binds;
  };
  std::map<std::pair<TextureShape, const char*>, Row> grouped;
  size_t streamed = 0;
  std::set<uint32_t> resolved;
  for (const auto& [key, stats] : g_resolves) {
    resolved.insert(stats.bases.begin(), stats.bases.end());
  }
  for (const auto& [texture_base, texture] : g_textures) {
    const char* origin = "streamed";
    if (resolved.contains(texture_base)) {
      origin = "resolved";
    } else if (g_created_bases.contains(texture_base)) {
      origin = "created";
    } else {
      ++streamed;
    }
    Row& row = grouped.try_emplace({texture.shape, origin}, Row{texture.shape, origin, 0, 0}).first->second;
    ++row.count;
    row.binds += texture.binds;
  }
  std::vector<Row> rows;
  for (const auto& [key, row] : grouped) {
    rows.push_back(row);
  }
  std::vector<std::string> parts;
  for (const Row& row : Busiest(rows)) {
    parts.push_back(fmt::format(R"({{{},"origin":"{}","textures":{},"binds":{}}})", ShapeJson(row.shape), row.origin,
                                row.count, row.binds));
  }
  return fmt::format(R"({{"distinct":{},"streamed":{},"shapes":{}}})", g_textures.size(), streamed, Join(parts));
}

std::string ReportResolves() {
  struct Row {
    const ResolveKey* key;
    const ResolveStats* stats;
    uint64_t count;
  };
  std::vector<Row> rows;
  for (const auto& [key, stats] : g_resolves) {
    rows.push_back({&key, &stats, stats.count});
  }
  std::vector<std::string> parts;
  for (const Row& row : Busiest(rows)) {
    size_t gpu_read = 0;
    size_t locked = 0;
    for (uint32_t texture_base : row.stats->bases) {
      gpu_read += g_used_bases.contains(texture_base);
      locked += g_locked_bases.contains(texture_base);
    }
    const auto& source = row.key->source;
    parts.push_back(fmt::format(
        R"({{"count":{},"flags":"0x{:X}","from_end_tiling":{},"source_rect":[{},{},{},{}],"dest":{{{}}},)"
        R"("dest_textures":{},"dest_bound_by_gpu":{},"dest_locked_by_cpu":{}}})",
        row.count, row.key->flags, row.key->from_end_tiling, source[0], source[1], source[2], source[3],
        ShapeJson(row.key->dest), row.stats->bases.size(), gpu_read, locked));
  }
  return Join(parts);
}

template <typename Map, typename Format>
std::string ReportMap(const Map& map, Format format) {
  struct Row {
    std::string text;
    uint64_t count;
  };
  std::vector<Row> rows;
  for (const auto& [key, count] : map) {
    rows.push_back({format(key, count), count});
  }
  std::vector<std::string> parts;
  for (const Row& row : Busiest(rows)) {
    parts.push_back(row.text);
  }
  return Join(parts);
}

std::string ReportBufferContents() {
  struct Row {
    uint32_t buffer;
    const BufferContents* contents;
    uint64_t count;
  };
  std::vector<Row> rows;
  std::map<uint32_t, uint64_t> runs_by_flags;
  uint64_t runs = 0;
  uint64_t runs_with_lists = 0;
  uint64_t changing = 0;
  uint64_t unreadable = 0;
  for (const auto& [buffer, contents] : g_buffer_contents) {
    runs += contents.runs;
    runs_with_lists += contents.lists.empty() ? 0 : contents.runs;
    changing += contents.sample_changes != 0;
    unreadable += contents.unreadable;
    runs_by_flags[contents.flags] += contents.runs;
    rows.push_back({buffer, &contents, contents.runs});
  }
  std::vector<std::string> parts;
  for (const Row& row : Busiest(rows)) {
    parts.push_back(fmt::format(
        R"({{"buffer":"0x{:X}","flags":"0x{:X}","runs":{},"segments":{},"lists":{},"dwords":{},"changes":{}}})",
        row.buffer, row.contents->flags, row.count, row.contents->segments, row.contents->lists.size(),
        row.contents->dwords, row.contents->sample_changes));
  }
  return fmt::format(
      R"({{"distinct":{},"runs":{},"runs_with_lists":{},"buffers_changing":{},"unreadable":{},"flags":{},"busiest":{}}})",
      g_buffer_contents.size(), runs, runs_with_lists, changing, unreadable,
      ReportMap(runs_by_flags,
                [](uint32_t flags, uint64_t count) { return fmt::format(R"({{"flags":"0x{:X}","runs":{}}})", flags, count); }),
      Join(parts));
}

bool Enabled() {
  return REXCVAR_GET(crackdown_d3d_census);
}

void Write(const std::filesystem::path& path, const std::string& label) {
  if (!Enabled()) {
    return;
  }
  g_window.store(false, std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);

  std::string out = fmt::format(R"({{"label":"{}","frames":{},"created_textures":{},)", label, g_frames.size(),
                                g_created_bases.size());
  out += R"("per_frame":{)";
  out += fmt::format(R"("draws":{},)", MedianMax(TotalDraws));
  for (size_t kind = 0; kind < kDrawKinds; ++kind) {
    out += fmt::format(R"("{}":{},)", kDrawNames[kind], MedianMax([kind](const FrameRow& r) { return r.draws[kind]; }));
  }
  out += fmt::format(R"("tiled_draws":{},)", MedianMax([](const FrameRow& r) { return r.tiled_draws; }));
  out += fmt::format(R"("tile_blocks":{},)", MedianMax([](const FrameRow& r) { return r.tile_blocks; }));
  out += fmt::format(R"("tiles":{},)", MedianMax([](const FrameRow& r) { return r.tiles; }));
  out += fmt::format(R"("clears":{},)", MedianMax([](const FrameRow& r) { return r.clears; }));
  out += fmt::format(R"("resolves":{},)", MedianMax([](const FrameRow& r) { return r.resolves; }));
  out += fmt::format(R"("texture_binds":{},)", MedianMax([](const FrameRow& r) { return r.texture_binds; }));
  out += fmt::format(R"("queries":{},)", MedianMax([](const FrameRow& r) { return r.queries; }));
  out += fmt::format(R"("locks":{},)", MedianMax([](const FrameRow& r) { return r.locks; }));
  out += fmt::format(R"("command_buffers":{},)", MedianMax([](const FrameRow& r) { return r.command_buffers; }));
  out += fmt::format(R"("flushes":{},)", MedianMax([](const FrameRow& r) { return r.flushes; }));
  out += fmt::format(R"("flush_kb":{},)", MedianMax([](const FrameRow& r) { return r.flush_kb; }));
  out += fmt::format(R"("draw_call_us":{},)",
                     MedianMax([](const FrameRow& r) { return uint32_t(r.draw_ns / 1000); }));
  out += fmt::format(R"("resolve_call_us":{}}},)",
                     MedianMax([](const FrameRow& r) { return uint32_t(r.resolve_ns / 1000); }));

  std::vector<std::string> primitives;
  for (size_t i = 0; i < g_primitives.size(); ++i) {
    if (g_primitives[i]) {
      primitives.push_back(fmt::format(R"({{"type":{},"draws":{}}})", i, g_primitives[i]));
    }
  }
  out += fmt::format(R"("primitive_types":{},)", Join(primitives));
  out += fmt::format(R"("elements":[{}],)", fmt::join(g_elements, ","));
  std::vector<uint64_t> threads;
  for (const auto& [thread, draws] : g_threads) {
    threads.push_back(draws);
  }
  out += fmt::format(R"("draws_by_thread":[{}],)", fmt::join(threads, ","));
  out += fmt::format(R"("tile_sets":{},)", ReportMap(g_tile_sets, [](const std::string& key, uint64_t count) {
                       return fmt::format(R"({{"rects":"{}","count":{}}})", key, count);
                     }));
  out += fmt::format(R"("predication":{},)", ReportMap(g_predication, [](const auto& key, uint64_t count) {
                       return fmt::format(R"({{"mask_low":"0x{:X}","mask_high":"0x{:X}","draws":{}}})", key.first, key.second,
                                          count);
                     }));
  out += fmt::format(R"("command_buffers":{},)", ReportMap(g_command_buffers, [](const auto& key, uint64_t count) {
                       return fmt::format(R"({{"buffer":"0x{:X}","predication":"0x{:X}","runs":{}}})", std::get<0>(key),
                                          std::get<1>(key), count);
                     }));
  out += fmt::format(R"("command_buffer_contents":{},)", ReportBufferContents());
  out += fmt::format(R"("textures":{},)", ReportTextures());
  out += fmt::format(R"("resolves":{},)", ReportResolves());

  std::vector<std::string> rows;
  for (const FrameRow& row : g_frames) {
    rows.push_back(fmt::format("[{},{},{},{},{},{},{}]", TotalDraws(row), row.tiled_draws, row.tiles, row.resolves,
                               row.texture_binds, row.queries, row.draw_ns / 1000));
  }
  out += fmt::format(R"("frame_columns":["draws","tiled_draws","tiles","resolves","binds","queries","draw_us"],)");
  out += fmt::format(R"("frame_rows":{}}})", Join(rows));

  std::ofstream(path) << out << "\n";
  auto lists_path = path;
  DumpLists(lists_path.replace_extension(".lists"));
  REXLOG_INFO("D3D census written to {}", path.string());
}

void Begin() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_frames.clear();
  g_row = {};
  g_window.store(true, std::memory_order_relaxed);
}

void OnGuestFrame() {
  if (!Counting()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_frames.push_back(g_row);
  g_row = {};
}

}  // namespace crackdown::census

namespace crackdown::hooks {

void CensusBeginVertices(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnDraw(ctx.r3.u32);
  if (!census::Counting()) {
    return original(ctx, base);
  }
  census::CountDraw(census::kRingVertices, ctx.r5.u32, ctx, base);
  census::RunTimed(ctx, base, original, &census::FrameRow::draw_ns);
}

void CensusBeginIndexedVertices(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnDraw(ctx.r3.u32);
  if (!census::Counting()) {
    return original(ctx, base);
  }
  census::CountDraw(census::kRingIndices, ctx.r7.u32, ctx, base);
  census::RunTimed(ctx, base, original, &census::FrameRow::draw_ns);
}

void CensusDrawVertices(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnDraw(ctx.r3.u32);
  const uint32_t device = ctx.r3.u32;
  if (!census::Counting()) {
    original(ctx, base);
    native::probe::AfterDraw(device, base);
    return;
  }
  census::CountDraw(census::kVertices, ctx.r6.u32, ctx, base);
  census::RunTimed(ctx, base, original, &census::FrameRow::draw_ns);
  native::probe::AfterDraw(device, base);
}

void CensusDrawIndexed(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnDraw(ctx.r3.u32);
  const uint32_t device = ctx.r3.u32;
  if (!census::Counting()) {
    original(ctx, base);
    native::probe::AfterDraw(device, base);
    return;
  }
  census::CountDraw(census::kIndices, ctx.r7.u32, ctx, base);
  census::RunTimed(ctx, base, original, &census::FrameRow::draw_ns);
  native::probe::AfterDraw(device, base);
}

void RingSegmentEnd(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::probe::OnRingSegmentEnding(ctx.r3.u32, base);
  original(ctx, base);
}

void CensusClear(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::shadow::OnClear();
  if (census::Counting()) {
    std::lock_guard lock(census::g_mutex);
    ++census::g_row.clears;
  }
  original(ctx, base);
}

void CensusResolve(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnResolve(ctx.r6.u32);
  if (!census::Counting()) {
    return original(ctx, base);
  }
  {
    const uint32_t rect = ctx.r5.u32;
    const uint32_t dest = ctx.r6.u32;
    census::ResolveKey key{ctx.r4.u32, census::t_in_end_tiling, {-1, -1, -1, -1}, {}};
    if (rect) {
      for (uint32_t i = 0; i < 4; ++i) {
        key.source[i] = census::LoadS32(base, rect + i * 4);
      }
    }
    uint32_t dest_base = 0;
    if (dest) {
      const auto fetch = census::DecodeFetch(census::GuestPointer(base, dest + census::kTextureFetchConstant));
      key.dest = census::ShapeOf(fetch);
      dest_base = census::BaseOf(fetch);
    }
    std::lock_guard lock(census::g_mutex);
    ++census::g_row.resolves;
    census::ResolveStats& stats = census::g_resolves[key];
    ++stats.count;
    if (dest_base) {
      stats.bases.insert(dest_base);
    }
  }
  census::RunTimed(ctx, base, original, &census::FrameRow::resolve_ns);
}

void CensusBeginTiling(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t device = ctx.r3.u32;
  original(ctx, base);
  if (!census::Counting()) {
    return;
  }
  const uint32_t tiles = std::min(census::Load32(base, device + census::kDeviceTileCount), census::kMaxTiles);
  std::string rects;
  for (uint32_t i = 0; i < tiles; ++i) {
    const uint32_t rect = device + census::kDeviceTileRects + i * 16;
    rects += fmt::format("{}{},{},{},{}", i ? " " : "", census::LoadS32(base, rect), census::LoadS32(base, rect + 4),
                         census::LoadS32(base, rect + 8), census::LoadS32(base, rect + 12));
  }
  std::lock_guard lock(census::g_mutex);
  census::g_tiling = true;
  ++census::g_row.tile_blocks;
  census::g_row.tiles += tiles;
  ++census::g_tile_sets[rects];
}

void CensusEndTiling(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  census::t_in_end_tiling = true;
  original(ctx, base);
  census::t_in_end_tiling = false;
  std::lock_guard lock(census::g_mutex);
  census::g_tiling = false;
}

void CensusRunCommandBuffer(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::probe::OnRunCommandBuffer(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, base);
  if (census::Counting()) {
    std::lock_guard lock(census::g_mutex);
    ++census::g_row.command_buffers;
    ++census::g_command_buffers[{ctx.r4.u32, ctx.r5.u32}];
    census::BufferContents& contents = census::ContentsOf(ctx.r4.u32, base);
    census::SampleContents(contents);
    ++contents.runs;
  }
  original(ctx, base);
}

void CensusQueryIssue(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (census::Counting() && (ctx.r4.u32 & census::kIssueBegin)) {
    std::lock_guard lock(census::g_mutex);
    ++census::g_row.queries;
  }
  original(ctx, base);
}

void CensusTextureLock(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnLock(ctx.r3.u32);
  if (census::Counting()) {
    const auto fetch = census::DecodeFetch(census::GuestPointer(base, ctx.r3.u32 + census::kTextureFetchConstant));
    std::lock_guard lock(census::g_mutex);
    ++census::g_row.locks;
    census::g_locked_bases.insert(census::BaseOf(fetch));
  }
  original(ctx, base);
}

void CensusCreateTexture(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
  if (!census::Enabled() || !ctx.r3.u32) {
    return;
  }
  const auto fetch = census::DecodeFetch(census::GuestPointer(base, ctx.r3.u32 + census::kTextureFetchConstant));
  std::lock_guard lock(census::g_mutex);
  census::g_created_bases.insert(census::BaseOf(fetch));
}

void CensusCacheFlush(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  native::textures::OnCacheFlush(ctx.r3.u32, ctx.r4.u32);
  if (census::Counting()) {
    std::lock_guard lock(census::g_mutex);
    ++census::g_row.flushes;
    census::g_row.flush_kb += (ctx.r4.u32 > ctx.r3.u32 ? ctx.r4.u32 - ctx.r3.u32 : 0) / 1024;
  }
  original(ctx, base);
}

}  // namespace crackdown::hooks
