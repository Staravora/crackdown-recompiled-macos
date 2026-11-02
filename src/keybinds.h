#pragma once

#include <array>
#include <string>
#include <string_view>

#include <imgui.h>
#include <rex/ui/virtual_key.h>

#include "trg/settings.h"

#include "mouse_look.h"

namespace crackdown {

// One keyboard control: the ReXGlue keybind_* setting, its launcher label and its Crackdown default.
struct KeyboardAction {
  const char* key;
  const char* label;
  const char* default_binding;
};

// keybind_guide is omitted: it is empty by default and the pad ignores it.
inline constexpr std::array<KeyboardAction, 24> kKeyboardActions = {{
    {"keybind_lstick_up", "Move forward", "W"},
    {"keybind_lstick_down", "Move back", "S"},
    {"keybind_lstick_left", "Move left", "A"},
    {"keybind_lstick_right", "Move right", "D"},
    {"keybind_a", "Jump / Accept", "Space,Return"},
    {"keybind_b", "Kick (hold: pick up) / Back", "F,Backspace"},
    {"keybind_x", "Reload (hold: equip)", "R"},
    {"keybind_y", "Enter / Exit / Use", "E"},
    {"keybind_right_trigger", "Fire", "LMB"},
    {"keybind_left_trigger", "Lock on", "RMB"},
    {"keybind_right_shoulder", "Throw grenade", "Q"},
    {"keybind_left_shoulder", "Left bumper", "V"},
    {"keybind_lstick_press", "Crouch", "C"},
    {"keybind_rstick_press", "Right stick press", "B"},
    {"keybind_rstick_up", "Camera up", "Up"},
    {"keybind_rstick_down", "Camera down", "Down"},
    {"keybind_rstick_left", "Camera left", "Left"},
    {"keybind_rstick_right", "Camera right", "Right"},
    {"keybind_dpad_up", "D-pad up", "1,Shift+Up"},
    {"keybind_dpad_down", "D-pad down", "2,Shift+Down"},
    {"keybind_dpad_left", "D-pad left", "3,Shift+Left"},
    {"keybind_dpad_right", "D-pad right", "4,Shift+Right"},
    {"keybind_back", "Map", "Tab"},
    {"keybind_start", "Pause", "Escape"},
}};

// Keys that do more than one pad press. See action_keys.h.
inline constexpr std::array<KeyboardAction, 3> kCrackdownActions = {{
    {"crackdown_key_pickup", "Pick up / Drop (one press)", "G"},
    {"crackdown_key_gas", "Gas, while driving", "W"},
    {"crackdown_key_brake", "Brake / Reverse, while driving", "S"},
}};

namespace keybind_detail {

inline std::string_view Trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') {
    text.remove_prefix(1);
  }
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace keybind_detail

// The ReXGlue name for an ImGui key, or nullptr when it cannot be a gameplay binding.
inline const char* KeyboardKeyName(ImGuiKey key) {
  switch (key) {
    case ImGuiKey_Tab:
      return "Tab";
    case ImGuiKey_LeftArrow:
      return "Left";
    case ImGuiKey_RightArrow:
      return "Right";
    case ImGuiKey_UpArrow:
      return "Up";
    case ImGuiKey_DownArrow:
      return "Down";
    case ImGuiKey_PageUp:
      return "PageUp";
    case ImGuiKey_PageDown:
      return "PageDown";
    case ImGuiKey_Home:
      return "Home";
    case ImGuiKey_End:
      return "End";
    case ImGuiKey_Insert:
      return "Insert";
    case ImGuiKey_Delete:
      return "Delete";
    case ImGuiKey_Backspace:
      return "Backspace";
    case ImGuiKey_Space:
      return "Space";
    case ImGuiKey_Enter:
      return "Return";
    case ImGuiKey_Escape:
      return "Escape";
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
      return "Control";
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
      return "Shift";
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
      return "Option";
    case ImGuiKey_LeftSuper:
    case ImGuiKey_RightSuper:
      return "Command";
    case ImGuiKey_Apostrophe:
      return "Quote";
    case ImGuiKey_Comma:
      return "Comma";
    case ImGuiKey_Minus:
      return "Minus";
    case ImGuiKey_Period:
      return "Period";
    case ImGuiKey_Slash:
      return "Slash";
    case ImGuiKey_Semicolon:
      return "Semicolon";
    case ImGuiKey_Equal:
      return "Plus";
    case ImGuiKey_LeftBracket:
      return "LBracket";
    case ImGuiKey_Backslash:
      return "Backslash";
    case ImGuiKey_RightBracket:
      return "RBracket";
    case ImGuiKey_GraveAccent:
      return "Backtick";
    case ImGuiKey_CapsLock:
      return "CapsLock";
    case ImGuiKey_ScrollLock:
      return "ScrollLock";
    case ImGuiKey_NumLock:
      return "NumLock";
    case ImGuiKey_PrintScreen:
      return "PrintScreen";
    case ImGuiKey_Pause:
      return "Pause";
    case ImGuiKey_KeypadDivide:
      return "NumpadSlash";
    case ImGuiKey_KeypadMultiply:
      return "NumpadStar";
    case ImGuiKey_KeypadSubtract:
      return "NumpadMinus";
    case ImGuiKey_KeypadAdd:
      return "NumpadPlus";
    case ImGuiKey_KeypadEnter:
      return "NumpadEnter";
    default:
      break;
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    static constexpr const char* kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    return kDigits[key - ImGuiKey_0];
  }
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    static constexpr const char* kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                               "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    return kLetters[key - ImGuiKey_A];
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    static constexpr const char* kFunction[] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",
                                                "F9",  "F10", "F11", "F12", "F13", "F14", "F15", "F16",
                                                "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
    return kFunction[key - ImGuiKey_F1];
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    static constexpr const char* kNumpad[] = {"Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
                                              "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9"};
    return kNumpad[key - ImGuiKey_Keypad0];
  }
  return nullptr;
}

// Keys the port or SDK already handles while playing, plus Escape and F12.
inline bool KeyboardBindingIsReserved(ImGuiKey key) {
  switch (key) {
    case ImGuiKey_Escape:
    case ImGuiKey_F2:
    case ImGuiKey_F3:
    case ImGuiKey_F4:
    case ImGuiKey_F7:
    case ImGuiKey_F10:
    case ImGuiKey_F11:
    case ImGuiKey_F12:
    case ImGuiKey_GraveAccent:
      return true;
    default:
      return false;
  }
}

// A modifier held on its own. The driver can't read these, so action_keys.cpp does.
inline bool KeyboardKeyIsModifier(std::string_view name) {
  return name == "Shift" || name == "Control" || name == "Option" || name == "Command";
}

// The binding to store for a captured key, e.g. "Space" or "Shift+Up".
inline std::string KeyboardBindingFor(ImGuiKey key, bool shift, bool ctrl, bool alt) {
  const char* name = KeyboardKeyName(key);
  if (!name || KeyboardBindingIsReserved(key)) {
    return {};
  }
  if (KeyboardKeyIsModifier(name)) {
    return name;
  }
  std::string binding;
  if (shift) {
    binding += "Shift+";
  }
  if (ctrl) {
    binding += "Ctrl+";
  }
  if (alt) {
    binding += "Alt+";
  }
  binding += name;
  return binding;
}

namespace keybind_detail {

// A key as the player's own keyboard calls it.
inline std::string KeyWord(std::string_view part) {
#if defined(__APPLE__)
  std::string shown;
  for (size_t plus = part.find('+'); plus != std::string_view::npos && plus != 0; plus = part.find('+')) {
    const std::string_view head = part.substr(0, plus);
    shown += head == "Alt" ? "Option" : head == "Ctrl" ? "Control" : std::string(head);
    shown += '+';
    part.remove_prefix(plus + 1);
  }
  if (part == "LMB") {
    return shown + "Click";
  }
  if (part == "RMB") {
    return shown + "Right click";
  }
  if (part == "Backspace") {
    return shown + "Delete";
  }
  return shown + std::string(part);
#else
  return std::string(part);
#endif
}

}  // namespace keybind_detail

// "Semicolon,Space" -> "Semicolon or Space".
inline std::string KeyboardBindingLabel(std::string_view binding) {
  if (binding.empty()) {
    return "None";
  }
  std::string label;
  std::size_t start = 0;
  for (;;) {
    const std::string_view rest = binding.substr(start);
    const std::size_t comma = rest.find(',');
    const std::string_view part = keybind_detail::Trim(rest.substr(0, comma));
    if (!label.empty()) {
      label += " or ";
    }
    label += keybind_detail::KeyWord(part);
    if (comma == std::string_view::npos) {
      break;
    }
    start += comma + 1;
  }
  return label;
}

namespace keybind_detail {

inline bool NameIsBindable(std::string_view name) {
  if (name == "LMB" || name == "RMB" || name == "MMB") {
    return true;
  }
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
    const ImGuiKey key = static_cast<ImGuiKey>(k);
    const char* known = KeyboardKeyName(key);
    if (known && name == known) {
      return !KeyboardBindingIsReserved(key);
    }
  }
  return false;
}

inline bool BindingPartIsValid(std::string_view part) {
  // Escape can't be captured, since it cancels capture, but on its own it's the default for Pause.
  if (part == "Escape") {
    return true;
  }
  for (;;) {
    const std::size_t plus = part.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    const std::string_view head = part.substr(0, plus);
    if (head != "Shift" && head != "Ctrl" && head != "Control" && head != "Alt") {
      break;
    }
    part.remove_prefix(plus + 1);
  }
  return NameIsBindable(part);
}

}  // namespace keybind_detail

inline bool KeyboardBindingIsValid(std::string_view binding) {
  bool any = false;
  std::size_t start = 0;
  for (;;) {
    const std::string_view rest = binding.substr(start);
    const std::size_t comma = rest.find(',');
    const std::string_view part = keybind_detail::Trim(rest.substr(0, comma));
    if (part.empty() || !keybind_detail::BindingPartIsValid(part)) {
      return false;
    }
    any = true;
    if (comma == std::string_view::npos) {
      break;
    }
    start += comma + 1;
  }
  return any;
}

inline std::string SavedKeyboardBinding(const trg::SettingsStore& settings, const KeyboardAction& action) {
  const std::string saved = settings.Get(action.key);
  return KeyboardBindingIsValid(saved) ? saved : std::string(action.default_binding);
}

inline void ResetKeyboardDefaults(trg::SettingsStore& settings) {
  for (const KeyboardAction& action : kKeyboardActions) {
    settings.Set(action.key, action.default_binding);
  }
  for (const KeyboardAction& action : kCrackdownActions) {
    settings.Set(action.key, action.default_binding);
  }
}

inline void ResetKeyboardAndMouseDefaults(trg::SettingsStore& settings) {
  ResetKeyboardDefaults(settings);
  settings.Set("mnk_sensitivity", std::to_string(mouse_look::kDefaultSensitivity));
}

struct KeyModifiers {
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
};

// The MnK driver's rule: a token fires only when its modifier prefixes equal the held modifiers exactly.
template <typename Parse>
bool BindingHasKey(std::string_view binding, rex::ui::VirtualKey key, KeyModifiers held, Parse parse) {
  while (!binding.empty()) {
    const size_t comma = binding.find(',');
    std::string_view token = keybind_detail::Trim(binding.substr(0, comma));
    binding = comma == std::string_view::npos ? std::string_view() : binding.substr(comma + 1);
    KeyModifiers wanted;
    for (size_t plus = token.find('+'); plus != std::string_view::npos && plus != 0; plus = token.find('+')) {
      const std::string_view head = token.substr(0, plus);
      if (head == "Shift") {
        wanted.shift = true;
      } else if (head == "Ctrl" || head == "Control") {
        wanted.ctrl = true;
      } else if (head == "Alt") {
        wanted.alt = true;
      } else {
        break;
      }
      token.remove_prefix(plus + 1);
    }
    if (wanted.shift == held.shift && wanted.ctrl == held.ctrl && wanted.alt == held.alt && !token.empty() &&
        parse(token) == key) {
      return true;
    }
  }
  return false;
}

// Escape, or a key bound to the pad's Start or Back: the player is heading for a menu, not playing.
template <typename Value, typename Parse>
bool IsMenuIntentKey(rex::ui::VirtualKey key, KeyModifiers held, Value binding_of, Parse parse) {
  return key == rex::ui::VirtualKey::kEscape || BindingHasKey(binding_of("keybind_start"), key, held, parse) ||
         BindingHasKey(binding_of("keybind_back"), key, held, parse);
}

}  // namespace crackdown
