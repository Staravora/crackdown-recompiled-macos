#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
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

  // Where to copy a resolve to, given the destination's fetch constant, or -1 if it's not a format that can be copied. A new
  // one is drawn at scale, the scale of the target that first copies into it.
  int ResolveInto(const uint32_t* fetch, int tile = -1, float scale = 1.0f);
  void ForgetResolve(const uint32_t* fetch);
  bool HasResolve(const uint32_t* fetch) const;

  // A descriptor for the six dwords of one fetch constant.
  int FindTexture(const uint32_t* fetch, int tile = -1);

  // A stack of depth maps read as a 3D texture. Each is resolved on its own, so a resolve has to ask if it's in one, and
  // the first frame to read a stack finds it empty. One as wide as the scene is filled from the scene's depth, so it's drawn
  // at SetSceneScale's scale.
  int FindStack(const uint32_t* fetch);
  void SetSceneScale(uint32_t scene_width, float scale) {
    scene_width_ = scene_width;
    scene_scale_ = scale;
  }
  bool FindStackSlice(const uint32_t* fetch, int& stack, uint32_t& slice) const;

  int FindSampler(const uint32_t* fetch);

  // Call at the start of each frame. Uploads are limited per frame so a burst of streaming can't stall the render thread.
  void BeginFrame();

  uint64_t textures_loaded() const { return textures_loaded_; }
  uint64_t textures_skipped() const { return textures_skipped_; }
  uint64_t textures_reloaded() const { return textures_reloaded_; }
  uint64_t reloaded_bytes() const { return reloaded_bytes_; }
  uint64_t textures_unchanged() const { return textures_unchanged_; }
  // For a problem report: every loaded texture whose guest memory no longer holds what was uploaded. "missed" means no
  // page change was reported, so it will never reload; "pending" means one was, and the reload hasn't happened yet.
  void CheckStale(std::ostream& out) const;
  // Reloads every loaded texture whose guest memory no longer holds what was uploaded, whether or not a change was
  // reported, ignoring the upload budget. For testing whether stale textures are what a glitch is. Returns how many.
  size_t ReloadStale();

  // Checks the next share of the textures drawn lately against guest memory, and reloads any whose memory changed
  // without the page watch reporting it. Some writes into guest memory get past the watch (a district's textures streamed
  // in on load or teleport), and without this Metal draws those textures from old data, black or in flat colours, until
  // something else bumps their pages. Call once a frame, after BeginFrame. Covers them all in about a second.
  void Sweep();
  struct SweepTotals {
    uint64_t repairs = 0;        // Textures reloaded because their memory changed unreported.
    uint64_t repaired_bytes = 0;
    uint64_t checked = 0;        // Textures compared with memory.
    uint64_t hashed_bytes = 0;
    uint64_t ns = 0;             // Time spent comparing and reloading.
    uint64_t rounds = 0;         // Times it has been through them all.
  };
  const SweepTotals& sweep_totals() const { return sweep_; }

 private:
  static constexpr size_t kUploadBytesPerFrame = 32 << 20;
  static constexpr size_t kSweepBytesPerFrame = 8 << 20;
  static constexpr uint64_t kSweepFrames = 30;         // How long a round takes, at most, when it's under the byte limit.
  static constexpr uint64_t kSweepRecentFrames = 60;   // Only textures drawn within this many frames are checked.

  uint32_t scene_width_ = 0;
  float scene_scale_ = 1.0f;

  // A guest page of a texture's data, as of its last load.
  struct PageCheck {
    uint32_t version = 0;
    uint64_t content = 0;
  };

  struct Entry {
    int descriptor = 0;
    std::array<uint32_t, 6> fetch{};
    uint64_t version = 0;
    // By page, in order through both ranges. A write beside a texture bumps a page it shares, so only the pages that moved
    // are hashed again, not the whole texture.
    std::vector<PageCheck> pages;
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
  int anisotropic_override_ = -1;  // The launcher's texture filtering: -1 leaves it to the game, else an AnisoFilter.
  uint64_t textures_loaded_ = 0;
  uint64_t textures_skipped_ = 0;
  uint64_t textures_reloaded_ = 0;
  uint64_t reloaded_bytes_ = 0;
  uint64_t textures_unchanged_ = 0;
  std::vector<uint64_t> sweep_keys_;
  size_t sweep_at_ = 0;
  SweepTotals sweep_;
};

}  // namespace crackdown::native
