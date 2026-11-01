#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

#include <rex/system/xmemory.h>

#include "metal_device.h"

namespace crackdown::native {

// Turns fetch constants into Metal textures, read from guest memory: resolved render targets are there too, because the
// emulator goes on writing them. A texture reloads when it's next used after its memory changes, which the emulator's
// page watch reports, along with MarkResolved for the writes it doesn't see. Only 2D textures load so far, and those it
// can't load read as the dummy texture. A texture the render thread has resolved into itself is read from Metal instead.
class TextureCache {
 public:
  TextureCache(MetalDevice& device, rex::memory::Memory* memory, const uint8_t* guest_memory, size_t guest_bytes);

  // For a texture the game has resolved into, given its fetch constant. Any thread.
  static void MarkResolved(const uint32_t* fetch);

  // Whether a fetch constant reads something guest memory doesn't hold: a depth buffer, or a resolve too large for the
  // emulator to copy back.
  static bool NeedsNativeResolve(const uint32_t* fetch);

  // A texture one tile high holds the last tile resolved into it, and a draw for a tile reads that, so each tile has its
  // own. Everything else draws across both, and reads them joined up at their real height, which tile -1 stands for.
  static bool HoldsOneTile(const uint32_t* fetch);

  // Which channels the fetch constant wants as value * 2 - 1, one bit each after its swizzle.
  static uint32_t BiasedChannels(const uint32_t* fetch);

  // Where to copy a resolve to, given the destination's fetch constant, or -1 if it's not a format that can be copied.
  int ResolveInto(const uint32_t* fetch, int tile = -1);
  void ForgetResolve(const uint32_t* fetch);
  bool HasResolve(const uint32_t* fetch) const;

  // A descriptor for the six dwords of one fetch constant.
  int FindTexture(const uint32_t* fetch, int tile = -1);

  // A stack of depth maps read as a 3D texture. Each is resolved on its own, so a resolve has to ask if it's in one, and
  // the first frame to read a stack finds it empty.
  int FindStack(const uint32_t* fetch);
  bool FindStackSlice(const uint32_t* fetch, int& stack, uint32_t& slice) const;

  int FindSampler(const uint32_t* fetch);

  // Call at the start of each frame. Uploads are limited per frame so a burst of streaming can't stall the render thread.
  void BeginFrame() {
    upload_budget_ = kUploadBytesPerFrame;
    ++frame_;
  }

  uint64_t textures_loaded() const { return textures_loaded_; }
  uint64_t textures_skipped() const { return textures_skipped_; }
  uint64_t textures_reloaded() const { return textures_reloaded_; }
  uint64_t reloaded_bytes() const { return reloaded_bytes_; }
  uint64_t textures_unchanged() const { return textures_unchanged_; }

 private:
  static constexpr size_t kUploadBytesPerFrame = 32 << 20;

  struct Entry {
    int descriptor = 0;
    std::array<uint32_t, 6> fetch{};
    uint64_t version = 0;
    uint64_t content = 0;
    uint64_t checked_frame = 0;
  };

  struct Resolved {
    int resolve = -1;
    bool live = false;
    std::unordered_map<uint32_t, int> views;
  };

  struct Stack {
    int descriptor = 0;
    uint32_t slices = 0;
    uint32_t pages_per_slice = 0;
  };

  int Load(const uint32_t* fetch, int descriptor);

  MetalDevice& device_;
  rex::memory::Memory* memory_;
  const uint8_t* guest_memory_;
  size_t guest_bytes_;
  std::unordered_map<uint64_t, Entry> textures_;
  std::unordered_map<uint32_t, int> samplers_;
  // The game reuses memory for different resolves within a frame, so these go by first page, format and size.
  std::map<std::array<uint32_t, 5>, Resolved> resolved_;
  std::map<uint32_t, Stack> stacks_;  // By first page.
  size_t upload_budget_ = 0;
  uint64_t frame_ = 0;
  uint64_t textures_loaded_ = 0;
  uint64_t textures_skipped_ = 0;
  uint64_t textures_reloaded_ = 0;
  uint64_t reloaded_bytes_ = 0;
  uint64_t textures_unchanged_ = 0;
};

}  // namespace crackdown::native
