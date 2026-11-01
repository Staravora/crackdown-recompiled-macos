#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <rex/system/xmemory.h>

#include "metal_device.h"

namespace crackdown::native {

// Turns fetch constants into Metal textures, read from guest memory: resolved render targets are there too, because the
// emulator goes on writing them. A texture reloads when it's next used after its memory changes, which the emulator's
// page watch reports, along with MarkResolved for the writes it doesn't see. Only 2D textures load so far, and those it
// can't load read as the dummy texture.
class TextureCache {
 public:
  TextureCache(MetalDevice& device, rex::memory::Memory* memory, const uint8_t* guest_memory, size_t guest_bytes);

  // For a texture the game has resolved into, given its fetch constant. Any thread.
  static void MarkResolved(const uint32_t* fetch);

  // A descriptor for the six dwords of one fetch constant.
  int FindTexture(const uint32_t* fetch);
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

  int Load(const uint32_t* fetch, int descriptor);

  MetalDevice& device_;
  rex::memory::Memory* memory_;
  const uint8_t* guest_memory_;
  size_t guest_bytes_;
  std::unordered_map<uint64_t, Entry> textures_;
  std::unordered_map<uint32_t, int> samplers_;
  size_t upload_budget_ = 0;
  uint64_t frame_ = 0;
  uint64_t textures_loaded_ = 0;
  uint64_t textures_skipped_ = 0;
  uint64_t textures_reloaded_ = 0;
  uint64_t reloaded_bytes_ = 0;
  uint64_t textures_unchanged_ = 0;
};

}  // namespace crackdown::native
