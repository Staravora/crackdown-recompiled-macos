#include "action_keys.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "cheat_hooks.h"
#include "keybinds.h"
#include "mouse_look.h"

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DEFINE_STRING(crackdown_key_pickup, "E", "Input/Keybinds/Crackdown", "Pick up or drop with one press");
REXCVAR_DEFINE_STRING(crackdown_key_gas, "W", "Input/Keybinds/Crackdown", "Gas, while driving");
REXCVAR_DEFINE_STRING(crackdown_key_brake, "S", "Input/Keybinds/Crackdown", "Brake and reverse, while driving");
REXCVAR_DEFINE_BOOL(crackdown_trackpad_lock_on, false, "Input", "Hold Shift to lock on");

namespace crackdown::action_keys {

namespace {

using rex::ui::VirtualKey;

constexpr uint32_t kGamepadOffset = 4;

struct ModifierBind {
  VirtualKey key;
  const char* action;
};

constexpr size_t kKeys = 256;

std::array<std::atomic<bool>, kKeys> g_down{};
std::vector<ModifierBind> g_modifier_binds;
std::vector<VirtualKey> g_pickup_keys, g_gas_keys, g_brake_keys;
std::mutex g_pickup_mutex;
PickUpHold g_pickup;
LockAim g_lock_aim;
rex::ui::Window* g_window = nullptr;

bool Held(const std::vector<VirtualKey>& keys) {
  for (const VirtualKey key : keys) {
    if (g_down[static_cast<uint16_t>(key)].load(std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void ForEachToken(std::string_view binding, auto&& visit) {
  while (!binding.empty()) {
    const size_t comma = binding.find(',');
    const std::string_view token = keybind_detail::Trim(binding.substr(0, comma));
    binding = comma == std::string_view::npos ? std::string_view() : binding.substr(comma + 1);
    if (!token.empty()) {
      visit(token);
    }
  }
}

std::vector<VirtualKey> ModifierKeys(std::string_view name) {
  if (name == "Shift") return {VirtualKey::kShift};
  if (name == "Control") return {VirtualKey::kControl};
  if (name == "Option") return {VirtualKey::kMenu};
  if (name == "Command") return {VirtualKey::kLWin, VirtualKey::kRWin};
  return {};
}

std::vector<VirtualKey> KeysOf(const std::string& binding) {
  std::vector<VirtualKey> keys;
  ForEachToken(binding, [&](std::string_view token) {
    const std::vector<VirtualKey> modifiers = ModifierKeys(token);
    keys.insert(keys.end(), modifiers.begin(), modifiers.end());
    if (const VirtualKey key = rex::ui::ParseVirtualKey(token); key != VirtualKey::kNone && modifiers.empty()) {
      keys.push_back(key);
    }
  });
  return keys;
}

void Configure() {
  g_modifier_binds.clear();
  for (const KeyboardAction& action : kKeyboardActions) {
    ForEachToken(rex::cvar::GetFlagByName(action.key), [&](std::string_view token) {
      for (const VirtualKey key : ModifierKeys(token)) {
        g_modifier_binds.push_back({key, action.key});
      }
    });
  }
  if (REXCVAR_GET(crackdown_trackpad_lock_on)) {
    g_modifier_binds.push_back({VirtualKey::kShift, "keybind_left_trigger"});
  }
  g_pickup_keys = KeysOf(REXCVAR_GET(crackdown_key_pickup));
  g_gas_keys = KeysOf(REXCVAR_GET(crackdown_key_gas));
  g_brake_keys = KeysOf(REXCVAR_GET(crackdown_key_brake));
  REXLOG_INFO("Action keys: {} modifier binds, trackpad lock-on {}", g_modifier_binds.size(),
              REXCVAR_GET(crackdown_trackpad_lock_on));
}

bool BoundAsModifier(VirtualKey key) {
  for (const ModifierBind& bind : g_modifier_binds) {
    if (bind.key == key) {
      return true;
    }
  }
  return false;
}

// Above the keyboard driver, whose rule that a key's modifiers must match exactly would silence every other key while a
// modifier that's bound on its own is held. So the driver never hears about those.
class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  static constexpr size_t kZOrder = 900;

  void OnKeyDown(rex::ui::KeyEvent& e) override { Note(e, true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { Note(e, false); }

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    for (std::atomic<bool>& down : g_down) {
      down.store(false, std::memory_order_relaxed);
    }
  }

 private:
  static void Note(rex::ui::KeyEvent& e, bool down) {
    const uint16_t key = static_cast<uint16_t>(e.virtual_key());
    if (e.target() != g_window || key >= kKeys) {
      return;
    }
    g_down[key].store(down, std::memory_order_relaxed);
    if (BoundAsModifier(e.virtual_key())) {
      e.set_handled(true);
    }
  }
};

Listener g_listener;

uint16_t Load16(const uint8_t* at) {
  uint16_t value;
  std::memcpy(&value, at, sizeof(value));
  return std::byteswap(value);
}

void Store16(uint8_t* at, uint16_t value) {
  value = std::byteswap(value);
  std::memcpy(at, &value, sizeof(value));
}

}  // namespace

void Install(rex::ui::Window* window) {
  g_window = window;
  Configure();
  window->AddInputListener(&g_listener, Listener::kZOrder);
  window->AddListener(&g_listener);
}

void Filter(uint8_t* base, uint32_t state, bool driving) {
  if (!REXCVAR_GET(mnk_mode)) {
    return;
  }
  uint8_t* gamepad = base + state + kGamepadOffset;
  Pad pad;
  pad.buttons = Load16(gamepad);
  pad.left_trigger = gamepad[2];
  pad.right_trigger = gamepad[3];
  pad.left_x = int16_t(Load16(gamepad + 4));
  pad.left_y = int16_t(Load16(gamepad + 6));
  pad.right_x = int16_t(Load16(gamepad + 8));
  pad.right_y = int16_t(Load16(gamepad + 10));
  for (const ModifierBind& bind : g_modifier_binds) {
    if (g_down[static_cast<uint16_t>(bind.key)].load(std::memory_order_relaxed)) {
      ApplyBind(pad, bind.action);
    }
  }
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  {
    std::lock_guard lock(g_pickup_mutex);
    if (g_pickup.Update(Held(g_pickup_keys), now_ms)) {
      pad.buttons |= kButtonB;
    }
  }
  if (driving) {
    ApplyDriving(pad, Held(g_gas_keys), Held(g_brake_keys));
  }
  // In a vehicle the same trigger is the brake, and the mouse should carry on turning the camera.
  const bool locked_on = !driving && pad.left_trigger >= kLockOnTrigger;
  mouse_look::SetLockedOn(locked_on);
  if (locked_on) {
    const mouse_look::Delta delta = mouse_look::TakeLockedDelta();
    double stick_x, stick_y;
    if (g_lock_aim.Update(delta.x, delta.y, now_ms, stick_x, stick_y)) {
      pad.right_x = int16_t(stick_x * kStickFull);
      pad.right_y = int16_t(stick_y * kStickFull);
    }
  } else {
    g_lock_aim.Reset();
  }
  Store16(gamepad, pad.buttons);
  gamepad[2] = pad.left_trigger;
  gamepad[3] = pad.right_trigger;
  Store16(gamepad + 4, uint16_t(pad.left_x));
  Store16(gamepad + 6, uint16_t(pad.left_y));
  Store16(gamepad + 8, uint16_t(pad.right_x));
  Store16(gamepad + 10, uint16_t(pad.right_y));
}

}  // namespace crackdown::action_keys
