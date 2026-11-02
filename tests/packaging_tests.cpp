// What the two recompiled guests mean for the sources and the packages, checked from the files themselves: one host
// executable ships, no game, update or DLC data does, and each guest is built into that executable under its own symbols.

#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>

#include "check.h"

namespace fs = std::filesystem;

namespace {

fs::path Root() {
  return fs::path(CRACKDOWN_SRC_DIR) / "..";
}

std::string Read(const char* relative) {
  return check::ReadText(Root() / relative);
}

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// The lines between `begin` and `end`, trimmed.
std::set<std::string> Block(const std::string& text, const std::string& begin, const std::string& end) {
  std::set<std::string> lines;
  const size_t from = text.find(begin);
  if (from == std::string::npos) {
    return lines;
  }
  const size_t to = text.find(end, from + begin.size());
  std::istringstream in(text.substr(from + begin.size(), to == std::string::npos ? std::string::npos : to - from - begin.size()));
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    line.erase(0, line.find_first_not_of(' '));
    if (!line.empty()) {
      lines.insert(line);
    }
  }
  return lines;
}

void TestAppImageCarriesOneExecutable() {
  std::printf("the AppImage holds one executable and no Xbox 360 data\n");
  const std::string script = Read("tools/package-linux.sh");
  const std::set<std::string> expected = Block(script, "EXPECTED=\"", "\"\nACTUAL=");
  CHECK(expected.count("usr/bin/crackdown"));
  size_t executables = 0;
  for (const std::string& entry : expected) {
    CHECK(entry.find("crackdown_tu6") == std::string::npos);
    const std::string leaf = entry.rfind("usr/bin/", 0) == 0 ? entry.substr(8) : std::string();
    executables += !leaf.empty() && leaf.find('/') == std::string::npos && leaf.find('.') == std::string::npos;
  }
  CHECK(executables == 1);
  CHECK(expected.count("usr/bin/librexruntime.so"));
  CHECK(expected.count("usr/bin/librexgpu-xenos.so"));
  for (const std::string& entry : expected) {
    const std::string lower = [&] {
      std::string out = entry;
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      return out;
    }();
    CHECK(!Has(lower, "default.xex"));
    CHECK(!Has(lower, ".xexp"));
    CHECK(!Has(lower, "gamestrings"));
    CHECK(!Has(lower, "title_update"));
    CHECK(!Has(lower, "00000002"));
    CHECK(!Has(lower, ".header"));
    CHECK(!Has(lower, "4d5307dc"));
  }
  CHECK(Has(script, "for f in crackdown librexruntime.so librexgpu-xenos.so crackdown.toml"));
  CHECK(Has(script, "strip --strip-unneeded \"$APPDIR/usr/bin/crackdown\"\n"));
  CHECK(Has(script, "check-guest-code.sh\" \"$ROOT\" tu6"));
  CHECK(Has(script, "readelf -d \"$APPDIR/usr/bin/crackdown\" | grep -q 'R.*PATH"));
  CHECK(Has(script, "for f in usr/bin/crackdown usr/bin/librexruntime.so"));
  // Nothing but the retired name is left of the second executable; the scanner refuses it.
  CHECK(Has(script, "crackdown_tu6 | crackdown_tu6.exe | save-versions | .switch-* |"));
  CHECK(!Has(script, "$BIN/crackdown_tu6"));
  CHECK(!Has(script, "usr/bin/crackdown_tu6\n"));
}

void TestAppImageScannerStillRefusesGameData() {
  std::printf("the AppImage scanner refuses the files the update and DLC installers write\n");
  const std::string script = Read("tools/package-linux.sh");
  for (const char* name : {"default.xex", "default.xexp", "gamestrings_dlc.str", "title_update", "dlc-install-temp", "user_data",
                           "4d5307dc"}) {
    CHECK(Has(script, std::string(name) + " |"));
  }
  for (const char* type : {"*.xex", "*.xexp", "*.str", "*.header", "*.iso", "*.dff", "*.xwb", "*.xzp"}) {
    CHECK(Has(script, std::string(type) + " |"));
  }
  CHECK(Has(script, "LIVE | PIRS | \"CON \" | XEX2"));
}

void TestAppRunKeepsOneEntryPoint() {
  std::printf("AppRun starts the one executable, which selects its guest itself\n");
  const std::string apprun = Read("packaging/linux/AppRun");
  CHECK(Has(apprun, "exec \"$HERE/usr/bin/crackdown\" \"$@\""));
  CHECK(!Has(apprun, "crackdown_tu6"));
  CHECK(!Has(apprun, "mkdir"));
  CHECK(!Has(apprun, "user_data"));
}

void TestWindowsPackageCarriesOneExecutable() {
  std::printf("the Windows package holds crackdown.exe, refuses crackdown_tu6.exe, and refuses update and DLC data\n");
  const std::string script = Read("tools/package-release.ps1");
  CHECK(Has(script, "$RequiredRootFiles = @('crackdown.exe', 'trg-updater.exe'"));
  CHECK(Has(script, "$ForbiddenNames = @('crackdown_tu6.exe', 'save-versions',"));
  CHECK(Has(script, "'.switch-*')"));
  CHECK(Has(Read("tools/test-release-check.ps1"), "'save snapshots'"));
  CHECK(Has(script, "foreach ($leaf in @('crackdown.exe', 'trg-updater.exe', 'rexruntime.dll'"));
  CHECK(!Has(script, "Copy-Item (Join-Path $ReleaseBuild 'crackdown_tu6.exe')"));
  CHECK(Has(Read("tools/test-release-check.ps1"), "'second executable' = 'crackdown_tu6.exe'"));
  CHECK(Has(Read("tools/build.ps1"), "foreach ($name in @('crackdown.exe', 'crackdown.toml')"));
  for (const char* name : {"'Default.xexp'", "'GameStrings_DLC.str'", "'title_update'", "'dlc-install-temp'", "'default.xex'"}) {
    CHECK(Has(script, name));
  }
  for (const char* type : {"'*.xexp'", "'*.str'", "'*.header'", "'*.xex'"}) {
    CHECK(Has(script, type));
  }
}

void TestBothGuestsLinkIntoOneExecutable() {
  std::printf("crackdown links both guests, each with its own bindings, and no second executable exists\n");
  const std::string cmake = Read("cmake/crackdown_guests.cmake");
  CHECK(Has(cmake, "crackdown_add_guest(base guest_base_"));
  CHECK(Has(cmake, "crackdown_add_guest(tu6 guest_tu6_"));
  CHECK(Has(cmake, "src/guest_bindings_base.cpp"));
  CHECK(Has(cmake, "src/guest_bindings_tu6.cpp"));
  CHECK(Has(cmake, "generated/tu6"));
  CHECK(Has(cmake, "target_link_libraries(crackdown PRIVATE ${CRACKDOWN_GUEST_TARGETS})"));
  CHECK(Has(cmake, "CRACKDOWN_TU6_GUEST_LINKED=1"));
  CHECK(!Has(cmake, "add_executable"));
  CHECK(!Has(cmake, "allow-multiple-definition"));
  CHECK(!fs::exists(Root() / "cmake/crackdown_tu6.cmake"));
  CHECK(!fs::exists(Root() / "src/tu6_unmapped.cpp"));
  const std::string main = Read("CMakeLists.txt");
  CHECK(Has(main, "include(cmake/crackdown_guests.cmake)"));
  CHECK(!Has(main, "src/guest_bindings_base.cpp"));
  CHECK(!Has(main, "src/guest_bindings_tu6.cpp"));
  CHECK(!Has(main, "crackdown_tu6"));
  CHECK(!Has(main, "add_executable(crackdown_tu6"));
  CHECK(Has(Read("cmake/rexglue_pregenerated.cmake"), "macro(rexglue_setup_host_target"));

  // Host code replaces guest functions only through the bindings, whose files each build compiles one of.
  for (const auto& entry : fs::directory_iterator(Root() / "src")) {
    const std::string name = entry.path().filename().string();
    if (entry.path().extension() != ".cpp" || name.rfind("guest_binding", 0) == 0) {
      continue;
    }
    const std::string text = check::ReadText(entry.path());
    if (Has(text, "REX_HOOK_RAW(sub_")) {
      std::printf("  %s replaces a guest function outside the bindings\n", name.c_str());
    }
    CHECK(!Has(text, "REX_HOOK_RAW(sub_"));
  }
  CHECK(Has(Read("config/overrides.toml"), "[[midasm_hook]]"));
  CHECK(Has(Read("config/tu6/overrides.toml"), "[[midasm_hook]]"));
}

void TestPersistentContentIsOutsideTheProgramFolder() {
  std::printf("installed content and the update are only addressed through the user data folder\n");
  for (const char* source : {"src/dlc.cpp", "src/title_update.cpp", "src/stfs.cpp", "src/content_state.cpp"}) {
    const std::string text = Read(source);
    CHECK(!Has(text, "GetExecutableFolder"));
    CHECK(!Has(text, "PortFolder"));
    CHECK(!Has(text, "APPIMAGE"));
    CHECK(!Has(text, "getenv"));
  }
  const std::string app = Read("src/crackdown_app.h");
  CHECK(Has(app, "paths.update_data_root = crackdown::TitleUpdateFolder(paths.user_data_root);"));
  CHECK(!Has(app, "game_data_root = crackdown::TitleUpdateFolder"));
}

void TestRelaunchStillStartsTheAppImage() {
  std::printf("a relaunch from an AppImage still starts the AppImage file\n");
  const std::string relaunch = Read("src/relaunch_command.h");
  CHECK(Has(relaunch, "appimage && *appimage ? std::string(appimage) : std::string(self_executable)"));
  const std::string platform = Read("src/platform.cpp");
  CHECK(Has(platform, "MakeRelaunchCommand(std::move(args), std::getenv(\"APPIMAGE\"), self, extra)"));
  // macOS has no environ in a dynamic library, so the environment comes from _NSGetEnviron there.
  CHECK(Has(platform, "posix_spawn(&pid, command->executable.c_str(), nullptr, nullptr, argv.data(), environment)"));
}

void TestGuestSwitchRelaunchesTheSameProgram() {
  std::printf("a guest switch after installing or removing the update relaunches this same program\n");
  const std::string platform = Read("src/platform.cpp");
  CHECK(Has(platform, "GetModuleFileNameW(nullptr, exe, MAX_PATH)"));
  CHECK(Has(platform, "CreateProcessW(exe, command_line.data()"));
  CHECK(!Has(platform, "crackdown_tu6"));
  CHECK(!Has(platform, "SwitchGuestBuild") && !Has(platform, "GuestExecutable"));
  const std::string launcher = Read("src/launcher.cpp");
  CHECK(Has(launcher, "bool BuildChanged() const { return content::SelectGuestBuild(content_->State()) != RunningBuild(); }"));
  CHECK(Has(launcher, "(cb_.window_changed && cb_.window_changed()) || BuildChanged()"));
  CHECK(Has(launcher, "if (cb_.restart && cb_.restart())"));
  CHECK(Has(Read("src/crackdown_app.h"), "crackdown::RelaunchSelf(L\"--crackdown_skip_launcher=true\")"));
  CHECK(!Has(Read("src/crackdown_app.h"), "crackdown_tu6"));
}

void TestGuestNamesKeepTheirCase() {
  std::printf("the guest file names are spelled as the game and the update spell them\n");
  const std::string catalog = Read("src/content_catalog.h");
  CHECK(Has(catalog, "kBaseXexName[] = \"default.xex\""));
  CHECK(Has(catalog, "kUpdateXexpName[] = \"Default.xexp\""));
  CHECK(Has(catalog, "kUpdateStringsName[] = \"GameStrings_DLC.str\""));
  CHECK(Has(Read("src/crackdown_app.h"), "update:\\\\Default.xexp"));
  CHECK(Has(Read("tools/dump_image/main.cpp"), "update:\\\\Default.xexp"));
  CHECK(Has(Read("tools/codegen-inputs.ps1"), "out\\tu6\\game\\Default.xexp"));
  CHECK(Has(Read("tools/codegen-inputs.ps1"), "out\\tu6\\game\\default.xex"));
}

}  // namespace

int main() {
  TestAppImageCarriesOneExecutable();
  TestAppImageScannerStillRefusesGameData();
  TestAppRunKeepsOneEntryPoint();
  TestWindowsPackageCarriesOneExecutable();
  TestBothGuestsLinkIntoOneExecutable();
  TestPersistentContentIsOutsideTheProgramFolder();
  TestRelaunchStillStartsTheAppImage();
  TestGuestSwitchRelaunchesTheSameProgram();
  TestGuestNamesKeepTheirCase();
  return check::Finish("packaging_tests");
}
