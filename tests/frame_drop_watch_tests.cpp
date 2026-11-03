#include <initializer_list>
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

// Feeds the seconds in order; the reason if any asked for a report, else empty.
std::string Run(FrameDropWatch& watch, std::initializer_list<double> seconds) {
  std::string reason, fired;
  for (const double fps : seconds) {
    if (watch.Sample(fps, reason)) {
      fired = reason;
    }
  }
  return fired;
}

// The glitch coming on, session 711 from 13:50:42: a steady stretch, a three-second slide, then under 20 and staying
// down.
void TestGlitchOnset() {
  FrameDropWatch watch;
  CHECK(Run(watch, {26.3, 29.9, 27.7, 29.4, 29.5, 28.1, 29.0, 29.5, 26.7, 25.0, 21.0, 16.8, 22.2}).empty());
  CHECK(Run(watch, {22.8}) == "the frame rate fell from a steady 28 to 16.8 FPS and stayed under 25");
}

// A slide longer than the ramp allows isn't the glitch's sudden fall.
void TestSlowSlideDoesNotCount() {
  FrameDropWatch watch;
  CHECK(Run(watch, {30, 30, 30, 30, 30, 26, 25, 24, 23, 22, 21, 19, 18, 18, 18}).empty());
}

// Opening the map (15:00:38) and the pause menu (15:12:04) in session 714, each after a steady stretch: they dip as low
// as the glitch but are back within two seconds.
void TestMenusDoNotCount() {
  FrameDropWatch map;
  CHECK(!Feed(map, 8, 30.0));
  CHECK(Run(map, {19.7, 24.6, 27.9, 28.9}).empty());
  FrameDropWatch pause;
  CHECK(!Feed(pause, 8, 30.0));
  CHECK(Run(pause, {17.7, 20.9, 30.8, 30.0}).empty());
  // The same lead-in with a lasting drop does fire, so it's the recovery that keeps the menus out.
  FrameDropWatch lasting;
  CHECK(!Feed(lasting, 8, 30.0));
  CHECK(!Run(lasting, {17.7, 20.9, 22.0}).empty());
}

void TestNeedsFiveSteadySeconds() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 7, 30.0));
  CHECK(!Feed(watch, 3, 16.0));
}

void TestUnsteadyDoesNotCount() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 2, 30.0));
  CHECK(!Feed(watch, 1, 24.0));  // One slow second in the steady stretch.
  CHECK(!Feed(watch, 5, 30.0));
  CHECK(!Feed(watch, 3, 16.0));
}

void TestSlowAllAlongDoesNotCount() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 60, 18.0));
}

void TestCooldownAndLimit() {
  FrameDropWatch watch;
  CHECK(!Feed(watch, 8, 30.0));
  CHECK(Feed(watch, 3, 16.0));
  // Back to steady soon after: too soon for another.
  CHECK(!Feed(watch, 10, 30.0));
  CHECK(!Feed(watch, 3, 16.0));
  // After the cooldown it can fire again, up to the limit.
  CHECK(!Feed(watch, 120, 30.0));
  CHECK(Feed(watch, 3, 16.0));
  CHECK(!Feed(watch, 130, 30.0));
  CHECK(Feed(watch, 3, 16.0));
  CHECK(!Feed(watch, 130, 30.0));
  CHECK(!Feed(watch, 3, 16.0));
}

}  // namespace

int main() {
  TestGlitchOnset();
  TestSlowSlideDoesNotCount();
  TestMenusDoNotCount();
  TestNeedsFiveSteadySeconds();
  TestUnsteadyDoesNotCount();
  TestSlowAllAlongDoesNotCount();
  TestCooldownAndLimit();
  return check::Finish("frame_drop_watch_tests");
}
