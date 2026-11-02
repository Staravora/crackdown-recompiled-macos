#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include <imgui.h>

#include "check.h"
#include "keybinds.h"

using namespace crackdown;

namespace {

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(CRACKDOWN_SRC_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

const KeyboardAction* Find(std::string_view key) {
  for (const KeyboardAction& action : kKeyboardActions) {
    if (action.key == key) {
      return &action;
    }
  }
  return nullptr;
}

struct PendingStore : trg::SettingsStore {
  std::map<std::string, std::string, std::less<>> saved;
  std::map<std::string, std::string, std::less<>> pending;

  PendingStore() {
    true_value = "true";
    false_value = "false";
  }

  std::string Get(std::string_view key, std::string_view def = {}) const override {
    const auto it = pending.find(key);
    return it == pending.end() || it->second.empty() ? std::string(def) : it->second;
  }
  void Set(std::string_view key, std::string value) override { pending[std::string(key)] = std::move(value); }
  bool Save() override {
    saved = pending;
    return true;
  }
};

void TestTable() {
  CHECK(kKeyboardActions.size() == 24);
  for (const KeyboardAction& action : kKeyboardActions) {
    CHECK(std::string_view(action.key).starts_with("keybind_"));
    CHECK(action.label[0] != '\0');
    CHECK(action.default_binding[0] != '\0');
    CHECK(KeyboardBindingIsValid(action.default_binding));
  }
  CHECK(!Find("keybind_guide"));
}

void TestMapping() {
  const struct {
    const char* key;
    const char* label;
    const char* default_binding;
  } kExpected[] = {
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
  };
  CHECK(sizeof(kExpected) / sizeof(kExpected[0]) == kKeyboardActions.size());
  for (const auto& expected : kExpected) {
    const KeyboardAction* action = Find(expected.key);
    CHECK(action != nullptr);
    if (action) {
      CHECK(std::string_view(action->label) == expected.label);
      CHECK(std::string_view(action->default_binding) == expected.default_binding);
    }
  }
}

void TestCapture() {
  CHECK(KeyboardBindingFor(ImGuiKey_D, false, false, false) == "D");
  CHECK(KeyboardBindingFor(ImGuiKey_Space, false, false, false) == "Space");
  CHECK(KeyboardBindingFor(ImGuiKey_UpArrow, true, false, false) == "Shift+Up");
  CHECK(KeyboardBindingFor(ImGuiKey_A, false, true, false) == "Ctrl+A");
  CHECK(KeyboardBindingFor(ImGuiKey_S, false, false, true) == "Alt+S");
  CHECK(KeyboardBindingFor(ImGuiKey_F5, true, true, true) == "Shift+Ctrl+Alt+F5");
  CHECK(KeyboardBindingFor(ImGuiKey_Keypad0, false, false, false) == "Numpad0");
  CHECK(KeyboardBindingFor(ImGuiKey_Equal, false, false, false) == "Plus");
  CHECK(KeyboardBindingFor(ImGuiKey_Semicolon, false, false, false) == "Semicolon");

  for (ImGuiKey key : {ImGuiKey_A, ImGuiKey_UpArrow, ImGuiKey_Space, ImGuiKey_F1, ImGuiKey_KeypadAdd}) {
    for (bool shift : {false, true}) {
      for (bool ctrl : {false, true}) {
        for (bool alt : {false, true}) {
          CHECK(KeyboardBindingIsValid(KeyboardBindingFor(key, shift, ctrl, alt)));
        }
      }
    }
  }
}

void TestRejection() {
  CHECK(KeyboardBindingFor(ImGuiKey_LeftShift, true, false, false) == "Shift");
  CHECK(KeyboardBindingFor(ImGuiKey_RightCtrl, false, true, false) == "Control");
  CHECK(KeyboardBindingFor(ImGuiKey_LeftAlt, false, false, true) == "Option");
  CHECK(KeyboardBindingFor(ImGuiKey_LeftSuper, false, false, false) == "Command");
  CHECK(KeyboardBindingIsValid("Shift"));
  CHECK(KeyboardBindingIsValid("Option,Command"));
  for (ImGuiKey key : {ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F7, ImGuiKey_F10, ImGuiKey_F11,
                       ImGuiKey_F12, ImGuiKey_Escape, ImGuiKey_GraveAccent}) {
    CHECK(KeyboardBindingIsReserved(key));
    CHECK(KeyboardBindingFor(key, false, false, false).empty());
    CHECK(KeyboardBindingFor(key, true, false, false).empty());
  }
  for (ImGuiKey key : {ImGuiKey_GamepadFaceDown, ImGuiKey_MouseLeft, ImGuiKey_KeypadDecimal, ImGuiKey_Oem102,
                       ImGuiKey_Menu}) {
    CHECK(KeyboardKeyName(key) == nullptr);
    CHECK(KeyboardBindingFor(key, false, false, false).empty());
  }
  CHECK(KeyboardBindingIsValid("Escape"));
  CHECK(!KeyboardBindingIsValid("F12"));
  CHECK(!KeyboardBindingIsValid("Backtick"));
}

void TestEscapeCancelsCapture() {
  CHECK(KeyboardBindingIsReserved(ImGuiKey_Escape));
  const std::string captured = KeyboardBindingFor(ImGuiKey_Escape, false, false, false);
  CHECK(captured.empty());

  PendingStore store;
  store.Set("keybind_y", "G");
  CHECK(store.Save());
  if (!captured.empty()) {
    store.Set("keybind_y", captured);
  }
  CHECK(SavedKeyboardBinding(store, *Find("keybind_y")) == "G");
}

void TestValidity() {
  for (const char* good : {"Space", "Semicolon,Space", "Shift+Up", "Ctrl+Shift+W", "NumpadPlus", "F1",
                           "Shift+Up,Shift+Down", " Z , Tab ", "Q,I,RMB", "E,O,LMB", "LMB", "Shift", "Escape"}) {
    CHECK(KeyboardBindingIsValid(good));
  }
  for (const char* bad : {"", "NotAKey", "Shift+", "Foo+Up", "Up,", ",Up", "Up,,Down", "Shift+Escape",
                          "Ctrl+F3", "Control+", "Return+", "MouseMove"}) {
    CHECK(!KeyboardBindingIsValid(bad));
  }
}

void TestLabel() {
  CHECK(KeyboardBindingLabel("Semicolon,Space") == "Semicolon or Space");
  CHECK(KeyboardBindingLabel("Shift+Up") == "Shift+Up");
  CHECK(KeyboardBindingLabel("Z,Tab") == "Z or Tab");
#if defined(__APPLE__)
  CHECK(KeyboardBindingLabel("Q,I,RMB") == "Q or I or Right click");
  CHECK(KeyboardBindingLabel("Alt+Up,LMB") == "Option+Up or Click");
#else
  CHECK(KeyboardBindingLabel("Q,I,RMB") == "Q or I or RMB");
#endif
  CHECK(KeyboardBindingLabel("") == "None");
}

void TestSaved() {
  PendingStore store;
  for (const KeyboardAction& action : kKeyboardActions) {
    CHECK(SavedKeyboardBinding(store, action) == action.default_binding);
  }
  store.Set("keybind_a", "Shift+Up");
  CHECK(SavedKeyboardBinding(store, *Find("keybind_a")) == "Shift+Up");
  CHECK(KeyboardBindingLabel(SavedKeyboardBinding(store, *Find("keybind_a"))) == "Shift+Up");
  store.Set("keybind_b", "NotAKey");
  CHECK(SavedKeyboardBinding(store, *Find("keybind_b")) == "F,Backspace");
  store.Set("keybind_start", "F12");
  CHECK(SavedKeyboardBinding(store, *Find("keybind_start")) == "Escape");
  CHECK(KeyboardBindingLabel(SavedKeyboardBinding(store, *Find("keybind_back"))) == "Tab");
  CHECK(SavedKeyboardBinding(store, *Find("keybind_right_trigger")) == "LMB");
}

void TestPendingAndReset() {
  PendingStore store;
  for (const KeyboardAction& action : kKeyboardActions) {
    store.pending[action.key] = action.default_binding;
    store.saved[action.key] = action.default_binding;
  }
  store.saved["crackdown_show_fps"] = "true";
  store.pending["crackdown_show_fps"] = "true";

  store.Set("keybind_a", "G");
  CHECK(SavedKeyboardBinding(store, *Find("keybind_a")) == "G");
  CHECK(store.saved["keybind_a"] == "Space,Return");
  CHECK(store.Save());
  CHECK(store.saved["keybind_a"] == "G");

  ResetKeyboardDefaults(store);
  for (const KeyboardAction& action : kKeyboardActions) {
    CHECK(store.Get(action.key) == action.default_binding);
    CHECK(SavedKeyboardBinding(store, action) == action.default_binding);
  }
  CHECK(store.Get("crackdown_show_fps") == "true");
  CHECK(store.saved["keybind_a"] == "G");
  CHECK(store.Save());
  CHECK(store.saved["keybind_a"] == "Space,Return");
  CHECK(store.Get("keybind_right_trigger") == "LMB");
  CHECK(store.Get("keybind_left_trigger") == "RMB");
}

void TestKeyboardAndMouseReset() {
  PendingStore store;
  store.Set("keybind_a", "G");
  store.Set("mnk_sensitivity", "2.89");
  store.Set("mnk_mode", "false");
  store.Set("crackdown_show_fps", "false");
  store.Set("resolution_scale", "2");

  ResetKeyboardAndMouseDefaults(store);

  for (const KeyboardAction& action : kKeyboardActions) {
    CHECK(store.Get(action.key) == action.default_binding);
  }
  CHECK(store.Get("mnk_sensitivity") == std::to_string(mouse_look::kDefaultSensitivity));
  CHECK(store.Get("mnk_mode") == "false");
  CHECK(store.Get("crackdown_show_fps") == "false");
  CHECK(store.Get("resolution_scale") == "2");
}

void TestSerialization() {
  const auto safe = [](std::string_view value) {
    return value.find('"') == std::string_view::npos && value.find('\\') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos && value.find('\r') == std::string_view::npos;
  };
  for (const KeyboardAction& action : kKeyboardActions) {
    CHECK(safe(action.key));
    CHECK(safe(action.default_binding));
  }
  for (ImGuiKey key : {ImGuiKey_A, ImGuiKey_Apostrophe, ImGuiKey_Backslash, ImGuiKey_UpArrow, ImGuiKey_KeypadAdd}) {
    CHECK(safe(KeyboardBindingFor(key, true, true, true)));
  }
}

void TestSources() {
  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "#include \"keybinds.h\""));
  CHECK(Has(settings, "for (const KeyboardAction& action : kKeyboardActions)"));
  CHECK(Has(settings, "const std::vector<std::string> kGameSettingKeys"));
  CHECK(Has(settings, "rex::cvar::SetFlagByName(key, key == \"mnk_sensitivity\""));

  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "ui.KeyBindRow(action.label, nullptr, shown)"));
  CHECK(Has(launcher, "ResetKeyboardAndMouseDefaults(ui.settings())"));
  CHECK(Has(launcher, "KeyboardBindingFor(*key, io.KeyShift, io.KeyCtrl, io.KeyAlt)"));
  CHECK(Has(launcher, "KeyboardBindingLabel(SavedKeyboardBinding(ui.settings(), action))"));
  CHECK(Has(launcher, "Click a key, then press the new one. Esc cancels."));
  CHECK(Has(launcher, "\"Keyboard and mouse defaults\""));
  CHECK(Has(launcher, "Restores the default keyboard controls and mouse sensitivity."));
  CHECK(Has(launcher, "ui.Toggle(\"Keyboard and mouse\","));
  CHECK(Has(launcher, "Use keyboard and mouse for gameplay controls together with the controller."));
  CHECK(Has(launcher, "\"mnk_mode\", false,"));
  CHECK(Has(launcher, "\"Off\", \"On\""));
  CHECK(Has(launcher, "ui.settings().GetBool(\"mnk_mode\", false)"));
  CHECK(Has(launcher, "ImGui::BeginDisabled()"));

  const std::string app = ReadSource("crackdown_app.h");
  CHECK(Has(app, "SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>())"));
}

void TestKeyboardToggle() {
  const std::string config = check::ReadText(std::string(CRACKDOWN_SRC_DIR) + "/../config/crackdown.toml");
  CHECK(Has(config, "mnk_mouse = true"));
  CHECK(Has(config, "keybind_right_trigger = \"LMB\""));
  CHECK(Has(config, "keybind_left_trigger = \"RMB\""));

  PendingStore store;
  CHECK(store.Get("mnk_mode", "false") == "false");
  store.SetBool("mnk_mode", true);
  CHECK(store.Get("mnk_mode") == "true");
  CHECK(store.Save());
  CHECK(store.saved["mnk_mode"] == "true");

  store.Set("keybind_a", "G");
  store.Set("mnk_mode", "false");
  CHECK(store.Save());
  ResetKeyboardDefaults(store);
  CHECK(store.Get("mnk_mode") == "false");
  CHECK(store.Get("keybind_a") == "Space,Return");

  store.Set("keybind_b", "H");
  store.Set("mnk_mode", "false");
  CHECK(store.Save());
  PendingStore reopened;
  reopened.saved = store.saved;
  reopened.pending = store.saved;
  CHECK(reopened.Get("mnk_mode") == "false");
  CHECK(SavedKeyboardBinding(reopened, *Find("keybind_b")) == "H");
  reopened.Set("mnk_mode", "true");
  CHECK(reopened.Save());
  CHECK(SavedKeyboardBinding(reopened, *Find("keybind_b")) == "H");
}

void TestHotkeySeparation() {
  CHECK(Has(ReadSource("frame_counter.cpp"), "RegisterBind(\"bind_crackdown_frame_counter\", \"F2\""));
  CHECK(Has(ReadSource("cheat_menu_key.h"), "VirtualKey::kF10"));
  CHECK(Has(ReadSource("window_host.cpp"), "VirtualKey::kF11"));
  CHECK(Has(ReadSource("crackdown_app.h"), "CreateAchievementsOverlay"));
  for (const char* file : {"frame_counter.cpp", "cheat_menu_key.h", "achievements_overlay.cpp", "crackdown_app.h"}) {
    CHECK(!Has(ReadSource(file), "mnk_mode"));
  }
}

}  // namespace

using rex::ui::VirtualKey;

VirtualKey ParseTestKey(std::string_view name) {
  static const std::map<std::string_view, VirtualKey> kKeys = {
      {"X", VirtualKey::kX},     {"Z", VirtualKey::kZ},   {"P", VirtualKey::kP},
      {"Tab", VirtualKey::kTab}, {"Up", VirtualKey::kUp}, {"Return", VirtualKey::kReturn}};
  const auto found = kKeys.find(name);
  return found == kKeys.end() ? VirtualKey::kNone : found->second;
}

bool ReleasesCapture(VirtualKey key, const std::map<std::string, std::string>& binds, KeyModifiers held = {}) {
  return IsMenuIntentKey(
      key, held, [&](std::string_view name) { return binds.at(std::string(name)); }, ParseTestKey);
}

// Capture follows the Start and Back bindings, not fixed physical keys.
void TestMenuIntentFollowsBindings() {
  const std::map<std::string, std::string> defaults = {{"keybind_start", Find("keybind_start")->default_binding},
                                                       {"keybind_back", Find("keybind_back")->default_binding}};
  for (const VirtualKey key : {VirtualKey::kEscape, VirtualKey::kTab}) {
    CHECK(ReleasesCapture(key, defaults));
  }
  CHECK(!ReleasesCapture(VirtualKey::kP, defaults));
  CHECK(!ReleasesCapture(VirtualKey::kReturn, defaults));
  CHECK(!ReleasesCapture(VirtualKey::kTab, defaults, {.shift = true}));

  const std::map<std::string, std::string> remapped = {{"keybind_start", "P"}, {"keybind_back", "Shift+Up"}};
  CHECK(!ReleasesCapture(VirtualKey::kX, remapped));
  CHECK(!ReleasesCapture(VirtualKey::kZ, remapped));
  CHECK(ReleasesCapture(VirtualKey::kP, remapped));
  CHECK(ReleasesCapture(VirtualKey::kUp, remapped, {.shift = true}));
  CHECK(!ReleasesCapture(VirtualKey::kUp, remapped));
  CHECK(ReleasesCapture(VirtualKey::kEscape, remapped));

  const std::map<std::string, std::string> unbound = {{"keybind_start", ""}, {"keybind_back", ""}};
  CHECK(ReleasesCapture(VirtualKey::kEscape, unbound));
  CHECK(!ReleasesCapture(VirtualKey::kX, unbound));

  const std::string host = ReadSource("window_host.cpp");
  CHECK(Has(host, "crackdown::IsMenuIntentKey("));
  CHECK(!Has(host, "case rex::ui::VirtualKey::kX:"));
}

void TestLauncherCtrlSSave() {
  std::printf("Ctrl+S saves through the launcher Save path, also with text input focused\n");
  const std::string patch = ReadSource("../patches/trg-launcher/0004-ctrl-s-save-in-text-input.patch");
  CHECK(Has(patch, "+  if (capturing_key() || ImGui::IsPopupOpen(\"\", ImGuiPopupFlags_AnyPopupId)) return;"));
  CHECK(Has(patch, "+  const bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);"));
  CHECK(Has(patch, "+  if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {"));
  CHECK(Has(patch, "+    Save();"));
  CHECK(Has(patch, "+  if (io.WantTextInput) return;"));
  CHECK(Has(patch, "-  } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {"));
  CHECK(!Has(patch, "+  if (io.KeyCtrl"));
  CHECK(!Has(patch, "DEBUG_SAULO"));
  CHECK(!Has(patch, "status_"));
  const std::string cmake = ReadSource("../CMakeLists.txt");
  CHECK(Has(cmake, "does not handle Ctrl+S before the text-input guard"));
  const std::string sdk = ReadSource("../third_party/trg-launcher/src/launcher.cpp");
  CHECK(Has(sdk, "SetStatus(ok ? \"Settings saved.\""));
  CHECK(Has(sdk, "if (ImGui::Button(\"Save\", ImVec2(bw, bh))) Save();"));
  CHECK(Has(sdk, "ImGui::IsKeyPressed(ImGuiKey_Enter, false)"));
}

int main() {
  TestMenuIntentFollowsBindings();
  TestTable();
  TestMapping();
  TestCapture();
  TestRejection();
  TestEscapeCancelsCapture();
  TestValidity();
  TestLabel();
  TestSaved();
  TestPendingAndReset();
  TestKeyboardAndMouseReset();
  TestSerialization();
  TestSources();
  TestKeyboardToggle();
  TestHotkeySeparation();
  TestLauncherCtrlSSave();
  return check::Finish("keyboard_tests");
}
