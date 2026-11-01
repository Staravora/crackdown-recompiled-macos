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

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "../updater/sha256.h"
#include "metal_device.h"
#include "texture_cache.h"

REXCVAR_DEFINE_BOOL(crackdown_native_shadow, false, "Crackdown",
                    "Draw every frame's baked geometry in Metal, off screen, beside the emulator, and write native-shadow-<time>.json");

REXCVAR_DEFINE_BOOL(crackdown_native_shadow_window, false, "Crackdown", "Show shadow mode's picture in a window of its own");

REXCVAR_DEFINE_BOOL(crackdown_native_shadow_debug, false, "Crackdown",
                    "Poll metal-debug.txt in the logs folder, so a running game can be examined from outside");

namespace crackdown::native::shadow {
namespace {

namespace xenos = rex::graphics::xenos;

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
constexpr size_t kScreenScaleOffset = 280;
constexpr size_t kScreenOffsetOffset = 288;
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

constexpr uint32_t kSourceAutoIndex = 2;
constexpr uint32_t kPrimitiveTriangleList = 4;
constexpr uint32_t kPrimitiveTriangleFan = 5;
constexpr uint32_t kPrimitiveTriangleStrip = 6;
constexpr uint32_t kPrimitiveQuadList = 13;
constexpr uint64_t kHashBasis = 0xCBF29CE484222325ull;
constexpr uint64_t kHashPrime = 0x100000001B3ull;
constexpr uint32_t kDepthEnable = 1 << 1;
constexpr uint32_t kDepthWrite = 1 << 2;
constexpr uint32_t kBlendDestinationShift = 8;
constexpr uint32_t kBlendFactorMask = 0x1F;
constexpr uint32_t kBlendFactorZero = 0;
constexpr uint32_t kBlendFactorOne = 1;
constexpr uint32_t kColorMaskFirstTarget = 0xF;
constexpr uint32_t kAlphaTestEnable = 1 << 3;
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
constexpr uint32_t kFirstTileMask = 0x3;
constexpr uint32_t kSecondTileMask = 0xC;

using FetchConstant = std::array<uint32_t, kFetchConstantDwords>;

// Draws, or a resolve, which fills in only resolve, dest and targets.
struct Run {
  std::optional<ResolveCopy> resolve;
  FetchConstant dest{};
  std::shared_ptr<const std::vector<BakedList>> lists;
  uint32_t pass_select = 0;
  bool clear = false;
  std::array<uint32_t, 4> clear_args{};
  float clear_z = 0.0f;
  Targets targets;
  std::shared_ptr<const std::vector<uint8_t>> vertex_code;
  std::array<uint32_t, kAluDwords> constants{};
  std::array<uint32_t, kLoopConstants> loops{};
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
bool g_clear_pending = false;
std::array<uint32_t, 4> g_clear_args{};
float g_clear_z = 0.0f;

// Render thread only from here down.
uint8_t* g_guest = nullptr;
rex::memory::Memory* g_memory = nullptr;
std::filesystem::path g_path;
std::filesystem::path g_shader_dir;
std::unordered_map<std::string, std::vector<uint32_t>> g_manifest;
// By address and size. -1 means looked at and no use.
std::unordered_map<uint64_t, int> g_vertex_shaders;
struct PixelSlot {
  uint32_t slot;
  uint32_t kind;  // 0 is 2D, 1 is 3D and 2 is cube.
};
struct PixelShader {
  int handle = -1;
  std::vector<PixelSlot> slots;
};
std::unordered_map<std::string, std::vector<PixelSlot>> g_pixel_manifest;
std::unordered_map<uint64_t, PixelShader> g_pixel_shaders;
std::unique_ptr<TextureCache> g_textures;
const PixelShader g_no_pixel_shader;
Totals g_totals;
uint64_t g_frame_number = 0;
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
          set(at + 1, get(at + 1) & 0x80000000);
          set(at + 2, 0);
        }
      }
    }
  }
  const std::string_view bytes(reinterpret_cast<const char*>(words.data()), size_t(dwords) * 4);
  return trg::update::Sha256Hex(bytes).substr(0, 16);
}

// Which of the fetched x, y, z, w, 0 or 1 each lane of input should hold, so that a translated shader, which swizzles
// input the way the shipped ucode did, ends up with what the live ucode's swizzle asks for. Binding rewrites that to suit
// the format, such as swapping red and blue. A lane nothing reads stays as fetched.
uint32_t SelectFor(uint32_t shipped, uint32_t live) {
  std::array<uint32_t, 4> select = {0, 1, 2, 3};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t from = (shipped >> (i * 3)) & 7, to = (live >> (i * 3)) & 7;
    if (from < 4 && (to < 4 || to == 4 || to == 5)) {
      select[from] = to;
    }
  }
  return select[0] | select[1] << 3 | select[2] << 6 | select[3] << 9;
}

// What pullElement needs from each fetch, read from the live ucode. A mini fetch shares the full fetch's slot and stride.
// The manifest holds each fetch's address and shipped swizzle.
std::vector<MetalPullElement> PullElements(const uint8_t* code, uint32_t dwords, const std::vector<uint32_t>& manifest) {
  std::vector<MetalPullElement> elements;
  uint32_t slot = 0, stride = 0;
  for (size_t entry = 0; entry + 1 < manifest.size(); entry += 2) {
    const uint32_t address = manifest[entry], shipped = manifest[entry + 1];
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
      reads |= (((d1 & 0xFFF) >> (i * 3)) & 7) <= 5;
    }
    const uint32_t flags = ((d1 >> 12) & 7) | (uint32_t(reads) << 3);
    const int32_t exponent = (int32_t(d1 << 2) >> 26);
    const int32_t offset = int32_t(d2 << 1) >> 9;
    elements.push_back({slot | (stride << 8) | (((d1 >> 16) & 0x3F) << 16) | (flags << 24), offset,
                        float(std::ldexp(1.0, exponent)), SelectFor(shipped, d1 & 0xFFF)});
  }
  return elements;
}

int FindVertexShader(MetalDevice& device, const BakedDraw& draw, const Run& run) {
  // By content where the code came with the draw: a copy in the ring has a new address each time it's made.
  uint64_t key = uint64_t(draw.vertex_shader) << 32 | draw.vertex_shader_dwords;
  if (run.vertex_code) {
    key = kHashBasis;
    for (const uint8_t byte : *run.vertex_code) {
      key = (key ^ byte) * kHashPrime;
    }
  }
  const auto found = g_vertex_shaders.find(key);
  if (found != g_vertex_shaders.end()) {
    return found->second;
  }
  int handle = -1;
  const uint32_t address = draw.vertex_shader & kGuestMask;
  if (draw.vertex_shader_dwords && draw.vertex_shader_dwords <= kMaxShaderDwords &&
      uint64_t(address) + draw.vertex_shader_dwords * 4ull <= kGuestBytes) {
    const uint8_t* code = run.vertex_code ? run.vertex_code->data() : g_guest + address;
    const std::string masked = MaskedKey(code, draw.vertex_shader_dwords);
    const auto manifest = g_manifest.find(masked);
    if (manifest == g_manifest.end()) {
      ++g_totals.shaders_missing;
      REXLOG_INFO("Native shadow: no vertex shader {} ({} dwords at {:08X})", masked, draw.vertex_shader_dwords, address);
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
    } else {
      std::string error;
      shader.handle = device.AddPixelShader(g_shader_dir / ("ps_" + name + ".metallib"), error);
      shader.slots = manifest->second;
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
  auto operator<=>(const TargetKey&) const = default;
};
std::map<TargetKey, int> g_targets;

MetalViewport ViewportOf(const BakedDraw& draw) {
  const std::array<float, 6> v = std::bit_cast<std::array<float, 6>>(draw.viewport);
  const float width = std::abs(v[0]) * 2.0f, height = std::abs(v[2]) * 2.0f;
  // Depth comes out as offset + scale * z, which sky objects use to keep to the far end of the range.
  return {v[1] - width / 2.0f, v[3] - height / 2.0f, width, height, v[5], v[5] + v[4]};
}

int TargetOf(MetalDevice& device, const Run& run) {
  const Targets& targets = run.targets;
  const TargetKey key = {targets.color, targets.depth, targets.width, targets.height};
  if (!key.width || !key.height || key.width > kMaxTargetSize || key.height > kMaxTargetSize) {
    return -1;
  }
  const auto found = g_targets.find(key);
  if (found != g_targets.end()) {
    return found->second;
  }
  const int target = device.AddTarget(key.width, key.height, key.color != 0);
  REXLOG_INFO("Native shadow: target {}x{}, colour {:08X}, depth {:08X}", key.width, key.height, key.color, key.depth);
  g_targets[key] = target;
  return target;
}

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

bool IsBlended(const BakedDraw& draw) {
  const uint32_t source_factor = draw.blend_control & kBlendFactorMask;
  const uint32_t destination_factor = (draw.blend_control >> kBlendDestinationShift) & kBlendFactorMask;
  return source_factor != kBlendFactorOne || destination_factor != kBlendFactorZero;
}

bool WritesNoColor(const BakedDraw& draw) {
  return (draw.color_mask & kColorMaskFirstTarget) == 0;
}

// A depth test that passes on greater-or-equal means reversed depth, which clears to 0.
double ClearDepthFor(const BakedDraw& draw) {
  return ((draw.depth_control >> 4) & 7) >= 4 ? 0.0 : 1.0;
}

// Live switches for examining a running game, set from metal-debug.txt: skip_blended, ids (each draw flat, in a colour
// that gives its number), no_resolves, show=<target>, skip_ps=<address>, stop_ps=<address>, which ends the frame at that
// shader's first draw, alpha, which puts alpha in a capture's colours, and dump=<name>, which writes live-<name>.png and
// .txt beside it.
bool g_debug_skip_blended = false;
bool g_debug_ids = false;
bool g_debug_no_resolves = false;
int g_debug_show = -1;
std::set<uint32_t> g_debug_skip_ps;
uint32_t g_debug_stop_ps = 0;
bool g_debug_stopped = false;
bool g_debug_alpha = false;
bool g_debug_no_stencil = false;
std::ofstream* g_dump = nullptr;
int g_debug_id = 0;

// A target nothing has drawn to this frame holds nothing worth copying, so the texture goes back to what guest memory has.
// A clear waits for the next pass, because the game clears one tile and then copies the next, and here they share a target.
void ApplyResolve(const Run& run, MetalDevice& device, const std::set<int>& drawn, std::map<int, MetalClear>& clears,
                  int& current_target) {
  const ResolveCopy& copy = *run.resolve;
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, run.dest.data(), sizeof(fetch));
  const uint32_t source = copy.flags & kResolveSourceMask;
  const bool depth = source == kResolveDepth;
  const auto found = g_targets.find({run.targets.color, run.targets.depth, run.targets.width, run.targets.height});
  const int target = found == g_targets.end() ? -1 : found->second;
  const int resolve = copy.texture && !g_debug_no_resolves && target >= 0 && drawn.contains(target) && (depth || (source == 0 && run.targets.color))
                          ? g_textures->ResolveInto(run.dest.data())
                          : -1;
  if (g_dump) {
    *g_dump << fmt::format("R {} flags {:X} rect {} {} {} {} {} to {} {} level {} fetch f{} {}x{} {:05X} swizzle {:03X} type {} dim {} mips {} {} clear {} {} tgt {} {:08X} {:08X} {}\n", g_debug_id, copy.flags, copy.has_rect,
                           copy.rect[0], copy.rect[1], copy.rect[2], copy.rect[3], copy.dest_x, copy.dest_y, copy.level, uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                           uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.base_address), uint32_t(fetch.swizzle), uint32_t(fetch.type), uint32_t(fetch.dimension), uint32_t(fetch.mip_address),
                           uint32_t(fetch.mip_max_level), copy.clear_depth, copy.clear_stencil, target, run.targets.color, run.targets.depth, resolve);
  }
  if (target >= 0 && (copy.flags & kResolveClearColor)) {
    MetalClear& clear = clears[target];
    clear.color = true;
    std::copy(copy.clear_color.begin(), copy.clear_color.end(), clear.color_value);
  }
  if (target >= 0 && (copy.flags & kResolveClearDepth)) {
    MetalClear& clear = clears[target];
    clear.depth = true;
    clear.depth_value = copy.clear_depth;
  }
  if (!copy.texture) {
    return;
  }
  if (resolve < 0) {
    ++g_totals.missed_resolves;
    g_textures->ForgetResolve(run.dest.data());
    return;
  }
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
    out.resolve = g_textures->ResolveInto(run.dest.data(), out.top >= kSecondTileTop ? 1 : 0);
    out.dest_y = copy.dest_y;
    if (out.resolve >= 0) {
      device.CopyTarget(out);
    }
  }
  current_target = -1;
  ++g_totals.native_resolves;
}

void Encode(const Frame& frame, MetalDevice& device, uint8_t* memory) {
  g_debug_id = 0;
  g_debug_stopped = false;
  device.CaptureAlpha(g_debug_alpha);
  g_textures->BeginFrame();
  size_t used = 0, indices_used = 0;
  int current_target = -1;
  std::unordered_map<int, uint32_t> color_draws;
  std::set<int> cleared;
  std::map<int, MetalClear> resolve_clears;
  int last_color_target = -1;
  for (const Run& run : frame.runs) {
    if (run.resolve) {
      if (!g_debug_stopped) {
        ApplyResolve(run, device, cleared, resolve_clears, current_target);
      }
      continue;
    }
    std::memcpy(g_constants.data(), run.constants.data(), sizeof(run.constants));
    bool run_cleared = run.clear;
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
        // Culling both faces leaves nothing to draw, and Metal has no way to say so.
        if ((draw.mode_control & kCullBothFaces) == kCullBothFaces) {
          continue;
        }
        const bool alpha_test = (draw.color_control & kAlphaTestEnable) != 0;
        g_totals.alpha_tested_draws += alpha_test;
        const PixelShader& found_pixel = run.targets.color ? FindPixelShader(device, draw) : g_no_pixel_shader;
        if (g_debug_skip_blended && run.targets.color && IsBlended(draw)) {
          continue;
        }
        if (g_debug_skip_ps.contains(draw.pixel_shader)) {
          continue;
        }
        g_debug_stopped |= g_debug_stop_ps && draw.pixel_shader == g_debug_stop_ps;
        if (g_debug_stopped) {
          continue;
        }
        const PixelShader& pixel = g_debug_ids ? g_no_pixel_shader : found_pixel;
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
        if (!WritesDepth(draw) && run.targets.color) {
          if (WritesNoColor(draw)) {
            ++g_totals.no_color_draws;
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
          if (g_dump) {
            const uint32_t* f = list.fetch_data.data() + size_t(draw.fetch_constants) * 192;
            std::string slots;
            for (const PixelSlot& slot : found_pixel.slots) {
              xenos::xe_gpu_texture_fetch_t fetch;
              std::memcpy(&fetch, f + size_t(slot.slot) * 6, sizeof(fetch));
              slots += fmt::format(" {}k{}:f{}:{}x{}:{:05X}", slot.slot, slot.kind, uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                                   uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.base_address));
            }
            const MetalViewport v = ViewportOf(draw);
            *g_dump << fmt::format("S {} {} prim {} src {} n {} vs {:08X} {} ps {:08X} {} ps_handle {} col {:08X} dep {:08X} {}x{} depth {:08X} blend {:08X} mask {:X} vp {} {} {} {} slots{}\n", g_debug_id,
                                   run.pass_select == ~0u ? "D" : "B", primitive, source, draw.initiator >> 16, draw.vertex_shader, draw.vertex_shader_dwords, draw.pixel_shader, draw.pixel_shader_dwords,
                                   pixel.handle, run.targets.color, run.targets.depth, run.targets.width, run.targets.height, draw.depth_control, draw.blend_control, draw.color_mask, v.x, v.y, v.width, v.height, slots);
          }
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
        const int target = InsideTarget(viewport, run.targets) ? TargetOf(device, run) : -1;
        if (target < 0) {
          ++g_totals.no_target_draws;
          continue;
        }
        const bool first_use = cleared.insert(target).second;
        const bool clear = run_cleared || first_use;
        const auto resolve_clear = resolve_clears.find(target);
        if (target != current_target || clear || resolve_clear != resolve_clears.end()) {
          MetalClear pass_clear;
          if (clear) {
            pass_clear.color = pass_clear.depth = true;
            pass_clear.depth_value = ClearDepthFor(draw);
          } else if (resolve_clear != resolve_clears.end()) {
            pass_clear = resolve_clear->second;
          }
          if (resolve_clear != resolve_clears.end()) {
            resolve_clears.erase(resolve_clear);
          }
          device.BeginPass(target, pass_clear);
          current_target = target;
          ++g_totals.passes;
        }
        run_cleared = false;
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
          for (int i = 0; i < 12; ++i) {
            pc += fmt::format(" {:.3f}", word(g_constants[1024 + i]));
          }
          *g_dump << "PC" << pc << "\n";
          if (draw.pixel_shader_dwords >= 300 && draw.pixel_shader_dwords <= 330 && index_count == 4) {
            std::string more;
            for (int i = 0; i < 256; ++i) {
              const float* v = reinterpret_cast<const float*>(&g_constants[1024 + i * 4]);
              if (v[0] || v[1] || v[2] || v[3]) {
                more += fmt::format(" c{}=({:.4f} {:.4f} {:.4f} {:.4f})", i, v[0], v[1], v[2], v[3]);
              }
            }
            *g_dump << "PX" << more << "\n";
          }
          if (run_cleared || clear) {
            *g_dump << fmt::format("C {:X} {:X} {:08X} {:X} z {} tgt {} {:08X} {:08X}\n", run.clear_args[0], run.clear_args[1], run.clear_args[2], run.clear_args[3], run.clear_z, target, run.targets.color, run.targets.depth);
          }
          for (const PixelSlot& slot : found_pixel.slots) {
            xenos::xe_gpu_texture_fetch_t fetch;
            std::memcpy(&fetch, f + size_t(slot.slot) * 6, sizeof(fetch));
            slots += fmt::format(" {}k{}:f{}:{}x{}:{:05X}", slot.slot, slot.kind, uint32_t(fetch.format), uint32_t(fetch.size_2d.width) + 1,
                                 uint32_t(fetch.size_2d.height) + 1, uint32_t(fetch.base_address));
          }
          *g_dump << fmt::format("{} {} vs {} ps {:08X} {} prim {} n {} tgt {} mode {:X} poly {:.6f} {:.6f} depth {:08X} st {:06X} blend {:08X} cc {:08X} mask {:X} bools {:08X} vp {} {} slots{}\n", g_debug_id, run.pass_select == ~0u ? "D" : "B",
                                 draw.vertex_shader_dwords, draw.pixel_shader, draw.pixel_shader_dwords, primitive, index_count, target, draw.mode_control, std::bit_cast<float>(draw.polygon_offset[0]), std::bit_cast<float>(draw.polygon_offset[1]), draw.depth_control, draw.stencil_front, draw.blend_control, draw.color_control,
                                 draw.color_mask, draw.booleans, viewport.width, viewport.height, slots);
        }
        if (g_dump && index_count <= 4 && automatic) {
          const uint32_t* f = list.fetch_data.data() + size_t(draw.fetch_constants) * 192;
          const uint32_t base = (f[190] & ~3u) & kGuestMask;
          std::string vertices;
          for (uint32_t i = 0; i < 28 && base + (i + 1) * 4 <= kGuestBytes; ++i) {
            vertices += fmt::format(" {:.4f}", std::bit_cast<float>(Word(g_guest + base, i)));
          }
          *g_dump << fmt::format("V {:08X} {:08X} offset {} mask {:X} vte {:X}:{}\n", f[190], f[191], draw.index_offset, draw.mask, draw.viewport_control, vertices);
        }
        if (g_dump && primitive == kPrimitiveQuadList && !automatic) {
          std::string first;
          for (uint32_t i = 0; i < 24; ++i) {
            first += fmt::format(" {}", GuestIndex(g_guest + (draw.index_buffer & kGuestMask), wide, i));
          }
          const uint32_t* f = list.fetch_data.data() + size_t(draw.fetch_constants) * 192;
          std::string vertices;
          for (uint32_t i = 0; i < 12; ++i) {
            vertices += fmt::format(" {:08X}", Word(g_guest + (f[190] & ~3u & kGuestMask), i));
          }
          std::string c;
          for (uint32_t i = 16; i < 64; ++i) {
            c += fmt::format(" {:.3f}", std::bit_cast<float>(g_constants[i]));
          }
          c += fmt::format(" lits {}", draw.literal_count);
          for (uint32_t i = 0; i < draw.literal_count; ++i) {
            c += fmt::format(" [{:08X} {} {}]", draw.literals[i].address, draw.literals[i].first_dword, draw.literals[i].dwords);
          }
          *g_dump << fmt::format("I {:08X} {:08X} wide {} offset {} fetch {:08X} {:08X}:{} |{} |{}\n", draw.index_buffer, draw.index_info, wide, draw.index_offset, f[190], f[191], first, vertices, c);
        }
        ++g_debug_id;
        out.blend_control = draw.blend_control;
        out.color_mask = draw.color_mask;
        out.alpha_test = alpha_test;
        out.viewport = viewport;
        out.depth_control = g_debug_no_stencil ? draw.depth_control & ~1u : draw.depth_control;
        out.stencil_front = draw.stencil_front;
        out.stencil_back = draw.stencil_back;
        out.mode_control = draw.mode_control;
        // Offsets are in units of the 24-bit depth buffer's last place, and slopes are per sixteenth of a pixel.
        if (draw.mode_control & (kPolygonOffsetFront | kPolygonOffsetBack)) {
          const uint32_t side = (draw.mode_control & kPolygonOffsetFront) ? 0 : 2;
          out.depth_bias_slope = std::bit_cast<float>(draw.polygon_offset[side]) / 16.0f;
          out.depth_bias = std::bit_cast<float>(draw.polygon_offset[side + 1]) * float(1 << 24);
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
        std::memcpy(shared + kScreenScaleOffset, screen.data(), sizeof(float) * 2);
        std::memcpy(shared + kScreenOffsetOffset, screen.data() + 2, sizeof(float) * 2);
        // Translated shaders read these as count, start and step.
        for (uint32_t i = 0; i < kLoopConstants; ++i) {
          const uint32_t loop = run.loops[i];
          const std::array<int32_t, 4> values = {int32_t(loop & 0xFF), int32_t((loop >> 8) & 0xFF), int32_t(int8_t(loop >> 16)), 0};
          std::memcpy(shared + kLoopTableOffset + i * 16, values.data(), sizeof(values));
        }
        std::array<int, 32> textures;
        uint32_t texture_count = 0;
        if (pixel.handle >= 0) {
          ++g_totals.shaded_draws;
          for (const PixelSlot& slot : pixel.slots) {
            const uint32_t* fetch = fetch_constants + size_t(slot.slot) * kFetchConstantDwords;
            // Only 2D and stacks load so far, and the tables start out pointing at the dummies.
            if (slot.kind <= 1 && slot.slot < textures.size()) {
              const uint32_t tiles = uint32_t(draw.mask);
              const int tile = tiles == kFirstTileMask ? 0 : tiles == kSecondTileMask ? 1 : -1;
              const int texture = slot.kind == 0 ? g_textures->FindTexture(fetch, tile) : g_textures->FindStack(fetch);
              textures[texture_count++] = texture;
              std::memcpy(shared + kTextureTableOffsets[slot.kind] + slot.slot * 4, &texture, sizeof(texture));
            }
            const int sampler = g_textures->FindSampler(fetch);
            std::memcpy(shared + kSamplerTableOffset + slot.slot * 4, &sampler, sizeof(sampler));
          }
        }
        out.textures = textures.data();
        out.texture_count = texture_count;
        out.indices = indices.offset;
        out.index_count = indices.count;
        out.wide_indices = indices.wide;
        out.base_vertex = int32_t(draw.index_offset);
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
  const int shown = g_debug_show >= 0 ? g_debug_show : last_color_target;
  if (shown >= 0) {
    device.ShowTarget(shown);
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
  uint64_t guest_frames, dropped, capture_ns, direct_draws, ring_desyncs;
  {
    // Each report covers the frames since the last one, so menus and loading don't blur a run.
    std::lock_guard lock(g_mutex);
    guest_frames = std::exchange(g_guest_frames, 0);
    dropped = g_dropped;
    capture_ns = std::exchange(g_capture_ns, 0);
    direct_draws = std::exchange(g_direct_draws, 0);
    ring_desyncs = g_ring_desyncs;
  }
  const MetalGpuTotals gpu = device.gpu_totals();
  if (!gpu.last_error.empty()) {
    REXLOG_ERROR("Native shadow: GPU error: {}", gpu.last_error);
  }
  const double frames = double(std::max<uint64_t>(g_totals.frames, 1));
  std::ofstream out(g_path);
  out << fmt::format(
      R"({{"guest_frames":{},"frames":{},"dropped":{},"draws_per_frame":{:.1f},"skipped_draws_per_frame":{:.1f},"no_room":{},)"
      R"("recorded_draws_per_frame":{:.1f},"recorded_indices_per_frame":{:.0f},"no_shader_draws_per_frame":{:.1f},"no_target_draws_per_frame":{:.1f},"other_primitive_draws_per_frame":{:.1f},"other_index_order_draws_per_frame":{:.1f},"transparent_draws_per_frame":{:.1f},"no_color_draws_per_frame":{:.1f},"alpha_tested_draws_per_frame":{:.1f},"shaded_draws_per_frame":{:.1f},"needs_resolve_draws_per_frame":{:.1f},"native_resolves_per_frame":{:.1f},"missed_resolves_per_frame":{:.1f},"pixel_shaders_found":{},"pixel_shaders_missing":{},"textures_loaded":{},"textures_skipped":{},"textures_reloaded":{},"reloaded_mb":{:.1f},"textures_unchanged":{},"direct_draws_per_frame":{:.1f},"ring_desyncs":{},"passes_per_frame":{:.1f},"targets":{},)"
      R"("shaders_found":{},"shaders_missing":{},"shaders_failed":{},"index_copy_ms_per_frame":{:.3f},)"
      R"("capture_ms_per_frame":{:.4f},"wait_ms_per_frame":{:.3f},"encode_ms_p50":{:.3f},"encode_ms_p95":{:.3f},"encode_ms_max":{:.3f},)"
      R"("gpu_frames":{},"gpu_errors":{},"gpu_ms_per_frame":{:.3f},"gpu_ms_worst":{:.3f},)"
      R"("guest_memory_wrapped":{},"guest_checks":{},"guest_matches":{}}})",
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
      device.guest_memory_wrapped() ? "true" : "false", g_totals.guest_checks, g_totals.guest_matches);
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
  while (true) {
    std::unique_ptr<Frame> frame;
    {
      std::unique_lock lock(g_mutex);
      g_wake.wait(lock, [] { return !g_ready.empty(); });
      frame = std::move(g_ready.front());
      g_ready.pop_front();
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
      std::string name = "live";
      g_debug_skip_blended = g_debug_ids = g_debug_no_resolves = g_debug_alpha = g_debug_no_stencil = false;
      g_debug_show = -1;
      g_debug_skip_ps.clear();
      g_debug_stop_ps = 0;
      while (in >> word) {
        g_debug_skip_blended |= word == "skip_blended";
        g_debug_ids |= word == "ids";
        g_debug_no_resolves |= word == "no_resolves";
        g_debug_alpha |= word == "alpha";
        g_debug_no_stencil |= word == "no_stencil";
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
      if (want_dump) {
        std::error_code ignored;
        std::filesystem::remove(control, ignored);
        std::ofstream(control) << (g_debug_skip_blended ? "skip_blended " : "") << (g_debug_ids ? "ids " : "")
                               << (g_debug_no_resolves ? "no_resolves " : "") << (g_debug_alpha ? "alpha " : "") << (g_debug_no_stencil ? "no_stencil " : "")
                               << (g_debug_show >= 0 ? fmt::format("show={} ", g_debug_show) : "")
                               << (g_debug_stop_ps ? fmt::format("stop_ps={:X} ", g_debug_stop_ps) : "");
        for (const uint32_t skip : g_debug_skip_ps) {
          std::ofstream(control, std::ios::app) << fmt::format("skip_ps={:X} ", skip);
        }
        capture = g_path.parent_path() / ("live-" + name + ".png");
        dump = std::make_unique<std::ofstream>(g_path.parent_path() / ("live-" + name + ".txt"));
        g_dump = dump.get();
      }
    }
    const MetalFrameTiming timing =
        device->RecordFrame([&](uint8_t* memory) { Encode(*frame, *device, memory); }, capture);
    g_dump = nullptr;
    frame->runs.clear();
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

void Install(rex::memory::Memory* memory, const std::filesystem::path& shader_dir, const std::filesystem::path& output_dir) {
  if (!REXCVAR_GET(crackdown_native_shadow)) {
    return;
  }
  g_guest = memory->TranslatePhysical<uint8_t*>(0);
  g_memory = memory;
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
  // The emulator does a resolve some time after the game asks for it, so a frame can read the old contents and settle
  // on them. Marking them again a frame later picks up the new ones.
  for (const FetchConstant& fetch : g_resolved_last_frame) {
    TextureCache::MarkResolved(fetch.data());
  }
  g_resolved_last_frame.swap(g_resolved_this_frame);
  g_resolved_this_frame.clear();
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

void OnClear(uint32_t flags, uint32_t rect, uint32_t color, uint32_t stencil, float depth) {
  if (Enabled()) {
    std::lock_guard lock(g_mutex);
    g_clear_pending = true;
    g_clear_args = {flags, rect, color, stencil};
    g_clear_z = depth;
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
  std::lock_guard lock(g_mutex);
  if (copy.texture) {
    g_resolved_this_frame.push_back(fetch);
  }
  Run& run = g_current->runs.emplace_back();
  run.resolve = copy;
  run.dest = fetch;
  run.targets = targets;
}

void SetRingDesyncs(uint64_t desyncs) {
  std::lock_guard lock(g_mutex);
  g_ring_desyncs = desyncs;
}

void Capture(std::shared_ptr<const std::vector<BakedList>> lists, uint32_t pass_select, const uint32_t* float_constants,
             const uint32_t* loop_constants, const Targets& targets, std::shared_ptr<const std::vector<uint8_t>> vertex_code) {
  const Clock::time_point start = Clock::now();
  std::lock_guard lock(g_mutex);
  Run& run = g_current->runs.emplace_back();
  run.lists = std::move(lists);
  run.pass_select = pass_select;
  run.clear = g_clear_pending;
  run.clear_args = g_clear_args;
  run.clear_z = g_clear_z;
  g_clear_pending = false;
  run.targets = targets;
  run.vertex_code = std::move(vertex_code);
  std::memcpy(run.constants.data(), float_constants, sizeof(run.constants));
  std::memcpy(run.loops.data(), loop_constants, sizeof(run.loops));
  g_capture_ns += Since(start);
}

}  // namespace crackdown::native::shadow
