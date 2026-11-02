#include "action_keys.h"

#include <cmath>

#include "check.h"

using namespace crackdown::action_keys;

namespace {

void TestPressWithNothingInHandPicksUp() {
  CarryKey key;
  CHECK(key.Update(true, false, 100).button_b);
  CHECK(key.Update(false, false, 120).button_b);
  CHECK(key.Update(false, false, 100 + kPickUpHoldMs - 1).button_b);
  CHECK(!key.Update(false, false, 100 + kPickUpHoldMs).button_b);
}

void TestPickingUpDoesNotThrowWhenTheKeyLifts() {
  CarryKey key;
  key.Update(true, false, 0);
  key.Update(true, true, 500);
  const CarryPress lifted = key.Update(false, true, 900);
  CHECK(!lifted.right_trigger);
}

void TestTapWhileCarryingThrows() {
  CarryKey key;
  CHECK(!key.Update(true, true, 0).button_b);
  const CarryPress lifted = key.Update(false, true, 100);
  CHECK(lifted.right_trigger);
  CHECK(!lifted.button_b);
  CHECK(!key.Update(false, true, 100 + kThrowPressMs).right_trigger);
}

void TestHoldWhileCarryingDrops() {
  CarryKey key;
  key.Update(true, true, 0);
  CHECK(!key.Update(true, true, kDropAfterMs - 1).button_b);
  CHECK(key.Update(true, true, kDropAfterMs).button_b);
  CHECK(!key.Update(true, true, kDropAfterMs + kDropPressMs).button_b);
  CHECK(!key.Update(false, false, kDropAfterMs + kDropPressMs + 50).right_trigger);
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
  TestPressWithNothingInHandPicksUp();
  TestPickingUpDoesNotThrowWhenTheKeyLifts();
  TestTapWhileCarryingThrows();
  TestHoldWhileCarryingDrops();
  TestDrivingPullsTriggersAndLeavesTheStick();
  TestFlickPushesAndEasesBack();
  TestStickStaysInsideItsCircle();
  TestSmallMovesStayCentred();
  TestBinds();
  return check::Finish("action_keys_tests");
}
