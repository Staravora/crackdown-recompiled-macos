#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace rex::ui {
class Window;
}

namespace crackdown::action_keys {

// A pad read as the guest gets it, with host byte order.
struct Pad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t left_x = 0;
  int16_t left_y = 0;
  int16_t right_x = 0;
  int16_t right_y = 0;
};

inline constexpr uint16_t kButtonB = 0x2000;
inline constexpr int16_t kStickFull = 32767;

// What the game wants before a press counts as a hold. Longer doesn't hurt, shorter is a kick.
inline constexpr int64_t kPickUpHoldMs = 650;

// What a key does that a pad press can't: the pad's B kicks on a tap and picks up on a hold, so one press of this key
// stands for the hold. Keeping the key down keeps B down.
class PickUpHold {
 public:
  bool Update(bool key_down, int64_t now_ms) {
    if (key_down && !was_down_) {
      release_at_ms_ = now_ms + kPickUpHoldMs;
    }
    was_down_ = key_down;
    return key_down || now_ms < release_at_ms_;
  }

 private:
  bool was_down_ = false;
  int64_t release_at_ms_ = 0;
};

// In a vehicle the triggers are gas and brake, so the keys that walk pull those as well. The stick's up and down do
// nothing there, and leaving them alone keeps W and S working in the pause menu.
inline void ApplyDriving(Pad& pad, bool gas, bool brake) {
  if (gas) {
    pad.right_trigger = 255;
  }
  if (brake) {
    pad.left_trigger = 255;
  }
}

inline constexpr uint8_t kLockOnTrigger = 64;
inline constexpr double kCountsToFullStick = 150.0;  // A short flick on a mouse or a trackpad.
inline constexpr double kAimHalfLifeMs = 220.0;
inline constexpr double kAimDeadzone = 0.15;

// While locked on the camera is the game's, and the right stick picks the part of the target. The mouse has no centre to
// come back to, so it pushes a stick of its own that eases back when the hand stops.
class LockAim {
 public:
  void Reset() {
    x_ = y_ = 0.0;
    last_ms_ = -1;
  }

  // dx and dy are mouse counts since the last call. Writes the stick, up positive, and returns whether it's off centre.
  bool Update(double dx, double dy, int64_t now_ms, double& stick_x, double& stick_y) {
    if (last_ms_ >= 0) {
      const double decay = std::pow(0.5, double(now_ms - last_ms_) / kAimHalfLifeMs);
      x_ *= decay;
      y_ *= decay;
    }
    last_ms_ = now_ms;
    x_ += dx / kCountsToFullStick;
    y_ -= dy / kCountsToFullStick;
    const double length = std::hypot(x_, y_);
    if (length > 1.0) {
      x_ /= length;
      y_ /= length;
    }
    if (length < kAimDeadzone) {
      stick_x = stick_y = 0.0;
      return false;
    }
    stick_x = x_;
    stick_y = y_;
    return true;
  }

 private:
  double x_ = 0.0;
  double y_ = 0.0;
  int64_t last_ms_ = -1;
};

// The same pad control a keybind_* setting drives, for keys the driver can't read itself: a modifier on its own never
// fires there, since holding one stops any key without it from matching.
inline bool ApplyBind(Pad& pad, std::string_view bind) {
  struct Button {
    std::string_view bind;
    uint16_t bit;
  };
  static constexpr std::array<Button, 14> kButtons = {{{"keybind_dpad_up", 0x0001},
                                                       {"keybind_dpad_down", 0x0002},
                                                       {"keybind_dpad_left", 0x0004},
                                                       {"keybind_dpad_right", 0x0008},
                                                       {"keybind_start", 0x0010},
                                                       {"keybind_back", 0x0020},
                                                       {"keybind_lstick_press", 0x0040},
                                                       {"keybind_rstick_press", 0x0080},
                                                       {"keybind_left_shoulder", 0x0100},
                                                       {"keybind_right_shoulder", 0x0200},
                                                       {"keybind_a", 0x1000},
                                                       {"keybind_b", kButtonB},
                                                       {"keybind_x", 0x4000},
                                                       {"keybind_y", 0x8000}}};
  for (const Button& button : kButtons) {
    if (button.bind == bind) {
      pad.buttons |= button.bit;
      return true;
    }
  }
  if (bind == "keybind_left_trigger") {
    pad.left_trigger = 255;
  } else if (bind == "keybind_right_trigger") {
    pad.right_trigger = 255;
  } else if (bind == "keybind_lstick_up") {
    pad.left_y = kStickFull;
  } else if (bind == "keybind_lstick_down") {
    pad.left_y = -kStickFull;
  } else if (bind == "keybind_lstick_left") {
    pad.left_x = -kStickFull;
  } else if (bind == "keybind_lstick_right") {
    pad.left_x = kStickFull;
  } else if (bind == "keybind_rstick_up") {
    pad.right_y = kStickFull;
  } else if (bind == "keybind_rstick_down") {
    pad.right_y = -kStickFull;
  } else if (bind == "keybind_rstick_left") {
    pad.right_x = -kStickFull;
  } else if (bind == "keybind_rstick_right") {
    pad.right_x = kStickFull;
  } else {
    return false;
  }
  return true;
}

// Rewrites guest pad reads while the keyboard is in use, and listens for the keys that need it.
void Install(rex::ui::Window* window);
void Filter(uint8_t* base, uint32_t state, bool driving);

}  // namespace crackdown::action_keys
