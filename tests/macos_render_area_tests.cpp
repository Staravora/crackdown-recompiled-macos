#include <random>
#include <rex/graphics/vulkan/render_area.h>
#include "check.h"

using rex::graphics::vulkan::RenderAreaBounds;

bool Equal(VkRect2D a, VkRect2D b) {
  return a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
         a.extent.width == b.extent.width && a.extent.height == b.extent.height;
}
bool Contains(VkRect2D r, int x, int y) {
  return x >= r.offset.x && y >= r.offset.y &&
         int64_t(x) < int64_t(r.offset.x) + r.extent.width &&
         int64_t(y) < int64_t(r.offset.y) + r.extent.height;
}

int main() {
  const VkRect2D original{{0, 0}, {1280, 4096}};
  RenderAreaBounds b;
  b.Begin(original, true);
  b.Draw();
  CHECK(Equal(b.End(), original));  // Unknown dynamic state must not trim.
  VkRect2D small{{20, 30}, {100, 80}};
  b.SetScissor(0, 1, &small);
  b.Begin(original, true);
  b.Draw();
  VkRect2D larger{{0, 0}, {1280, 720}};
  b.SetScissor(0, 1, &larger);
  b.Draw();
  CHECK(Equal(b.End(), larger));  // A later draw expands the same pass.
  b.Begin(original, true);
  b.Draw();
  CHECK(Equal(b.End(), larger));  // Scissor persists across passes.
  b.Begin(original, false);
  b.Draw();
  CHECK(Equal(b.End(), original));  // Attachment clears/resolves need full area.
  VkRect2D multiple[]{small, larger};
  b.SetScissor(0, 2, multiple);
  b.Begin(original, true);
  b.Draw();
  CHECK(Equal(b.End(), original));

  // Compare against discrete pixel coverage rather than another bounds formula.
  std::mt19937 random(0x360);
  std::uniform_int_distribution<int> offset(0, 63), extent(0, 40);
  for (int trial = 0; trial < 500; ++trial) {
    RenderAreaBounds tracker;
    VkRect2D area{{8, 9}, {32, 30}};
    tracker.Begin(area, true);
    bool covered[64][64]{};
    for (int draw = 0; draw < 12; ++draw) {
      VkRect2D scissor{{offset(random), offset(random)},
                       {uint32_t(extent(random)), uint32_t(extent(random))}};
      tracker.SetScissor(0, 1, &scissor);
      tracker.Draw();
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
          covered[y][x] |= Contains(area, x, y) && Contains(scissor, x, y);
    }
    auto trimmed = tracker.End();
    CHECK(trimmed.extent.width > 0 && trimmed.extent.height > 0);
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) {
        CHECK(!covered[y][x] || Contains(trimmed, x, y));
        CHECK(!Contains(trimmed, x, y) || Contains(area, x, y));
      }
  }
  return check::Finish("conservative Vulkan render areas");
}
