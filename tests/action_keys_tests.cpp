#include "action_keys.h"

#include <cmath>

#include "check.h"

using namespace crackdown::action_keys;

namespace {

void TestPickUpHoldsLongEnough() {
  PickUpHold hold;
  CHECK(!hold.Update(false, 0));
  CHECK(hold.Update(true, 100));
  CHECK(hold.Update(false, 120));
  CHECK(hold.Update(false, 100 + kPickUpHoldMs - 1));
  CHECK(!hold.Update(false, 100 + kPickUpHoldMs));
}

void TestPickUpKeepsGoingWhileHeld() {
  PickUpHold hold;
  CHECK(hold.Update(true, 0));
  CHECK(hold.Update(true, kPickUpHoldMs * 3));
  CHECK(!hold.Update(false, kPickUpHoldMs * 3));
}

void TestDrivingPullsTriggersAndLeavesTheStick() {
  Pad pad;
  pad.left_y = kStickFull;
  ApplyDriving(pad, true, false);
  CHECK(pad.right_trigger == 255);
  CHECK(pad.left_trigger == 0);
  CHECK(pad.left_y == kStickFull);
  ApplyDriving(pad, false, true);
  CHECK(pad.left_trigger == 255);
}

void TestFlickPushesAndEasesBack() {
  LockAim aim;
  double x = 0, y = 0;
  CHECK(!aim.Update(0, 0, 0, x, y));
  CHECK(aim.Update(0, -kCountsToFullStick, 16, x, y));
  CHECK(y > 0.9);  // Mouse up is stick up.
  CHECK(x == 0.0);
  CHECK(aim.Update(0, 0, 16 + int64_t(kAimHalfLifeMs), x, y));
  CHECK(y < 0.6);
  CHECK(!aim.Update(0, 0, 16 + int64_t(kAimHalfLifeMs) * 5, x, y));
  CHECK(x == 0.0 && y == 0.0);
}

void TestStickStaysInsideItsCircle() {
  LockAim aim;
  double x = 0, y = 0;
  CHECK(aim.Update(kCountsToFullStick * 5, kCountsToFullStick * 5, 0, x, y));
  CHECK(std::hypot(x, y) <= 1.0001);
  CHECK(x > 0.0 && y < 0.0);
}

void TestSmallMovesStayCentred() {
  LockAim aim;
  double x = 0, y = 0;
  CHECK(!aim.Update(5, 5, 0, x, y));
}

void TestBinds() {
  Pad pad;
  CHECK(ApplyBind(pad, "keybind_left_trigger"));
  CHECK(pad.left_trigger == 255);
  CHECK(ApplyBind(pad, "keybind_b"));
  CHECK(pad.buttons == kButtonB);
  CHECK(ApplyBind(pad, "keybind_lstick_down"));
  CHECK(pad.left_y == -kStickFull);
  CHECK(!ApplyBind(pad, "keybind_guide"));
}

}  // namespace

int main() {
  TestPickUpHoldsLongEnough();
  TestPickUpKeepsGoingWhileHeld();
  TestDrivingPullsTriggersAndLeavesTheStick();
  TestFlickPushesAndEasesBack();
  TestStickStaysInsideItsCircle();
  TestSmallMovesStayCentred();
  TestBinds();
  return check::Finish("action_keys_tests");
}
