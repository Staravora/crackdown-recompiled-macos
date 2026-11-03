#include <string>

#include "check.h"
#include "frame_drop_watch.h"

using namespace crackdown;

namespace {

// Feeds n seconds at fps; true if any of them asked for a report.
bool Feed(FrameDropWatch& watch, int n, double fps) {
  bool fired = false;
  std::string reason;
  for (int i = 0; i < n; ++i) {
    fired |= watch.Sample(fps, reason);
  }
  return fired;
}

void TestSteadyThenDrop() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 5, 30.0));
  std::string reason;
  CHECK(watch.Sample(16.8, reason));
  CHECK(reason == "the frame rate fell from a steady 30 to 16.8 FPS");
}

void TestNeedsFiveSteadySeconds() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 4, 30.0));
  CHECK(!Feed(watch, 1, 16.0));
}

void TestUnsteadyDoesNotCount() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 4, 30.0));
  CHECK(!Feed(watch, 1, 24.0));  // One slow second in the window.
  CHECK(!Feed(watch, 1, 16.0));
}

void TestSlowAllAlongDoesNotCount() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 60, 18.0));
}

void TestCooldownAndLimit() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 5, 30.0));
  CHECK(Feed(watch, 1, 16.0));
  // Back to steady soon after: too soon for another.
  CHECK(!Feed(watch, 10, 30.0));
  CHECK(!Feed(watch, 1, 16.0));
  // After the cooldown it can fire again, up to the limit.
  CHECK(!Feed(watch, 120, 30.0));
  CHECK(Feed(watch, 1, 16.0));
  CHECK(!Feed(watch, 130, 30.0));
  CHECK(Feed(watch, 1, 16.0));
  CHECK(!Feed(watch, 130, 30.0));
  CHECK(!Feed(watch, 1, 16.0));
}

}  // namespace

int main() {
  TestSteadyThenDrop();
  TestNeedsFiveSteadySeconds();
  TestUnsteadyDoesNotCount();
  TestSlowAllAlongDoesNotCount();
  TestCooldownAndLimit();
  return check::Finish("frame_drop_watch_tests");
}
