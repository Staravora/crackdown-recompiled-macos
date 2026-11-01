#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "metal_device.h"

namespace crackdown::native {

// Turns fetch constants into Metal textures, read from guest memory: resolved render targets are there too, because the
// emulator goes on writing them. Only 2D textures load so far, and a texture doesn't update once it has loaded. Those it
// can't load read as the dummy texture.
class TextureCache {
 public:
  TextureCache(MetalDevice& device, const uint8_t* guest_memory, size_t guest_bytes);

  // A descriptor for the six dwords of one fetch constant.
  int FindTexture(const uint32_t* fetch);
  int FindSampler(const uint32_t* fetch);

  // Call at the start of each frame. Uploads are limited per frame so a burst of streaming can't stall the render thread.
  void BeginFrame() { upload_budget_ = kUploadBytesPerFrame; }

  uint64_t textures_loaded() const { return textures_loaded_; }
  uint64_t textures_skipped() const { return textures_skipped_; }

 private:
  static constexpr size_t kUploadBytesPerFrame = 32 << 20;

  int Load(const uint32_t* fetch);

  MetalDevice& device_;
  const uint8_t* guest_memory_;
  size_t guest_bytes_;
  std::unordered_map<uint64_t, int> textures_;
  std::unordered_map<uint32_t, int> samplers_;
  size_t upload_budget_ = 0;
  uint64_t textures_loaded_ = 0;
  uint64_t textures_skipped_ = 0;
};

}  // namespace crackdown::native
