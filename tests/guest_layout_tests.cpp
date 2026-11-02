// The guest addresses of the two builds stay apart: shared host code reads them through the layout, bindings and
// midasm hooks name each build's own functions, and config/tu6/address-map.csv, the developer record of how every
// Title Update 6 value was found, agrees with all of them.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cheats.h"
#include "check.h"
#include "guest_layout.h"

namespace fs = std::filesystem;
using crackdown::GuestLayout;
using crackdown::kLayoutBase;
using crackdown::kLayoutTitleUpdate6;

namespace {

fs::path Root() {
  return fs::path(CRACKDOWN_SRC_DIR) / "..";
}

std::string Read(const fs::path& relative) {
  return check::ReadText(Root() / relative);
}

struct Row {
  std::string kind, name, tu0, tu6, tu0_word, tu6_word, tu0_size, tu6_size, evidence;
};

std::vector<Row> LoadMap() {
  std::vector<Row> rows;
  std::istringstream in(Read("config/tu6/address-map.csv"));
  std::string line;
  std::getline(in, line);
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    std::vector<std::string> fields;
    size_t from = 0;
    for (int i = 0; i < 8; ++i) {
      const size_t comma = line.find(',', from);
      fields.push_back(line.substr(from, comma - from));
      from = comma + 1;
    }
    fields.push_back(line.substr(from));
    rows.push_back({fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7], fields[8]});
  }
  return rows;
}

uint32_t Hex(const std::string& text) {
  return uint32_t(std::stoul(text, nullptr, 16));
}

std::string Upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return text;
}

using Accessor = std::function<uint32_t(const GuestLayout&)>;

// Every layout field the record carries, by its record name.
std::map<std::string, Accessor> Accessors() {
  std::map<std::string, Accessor> a;
  a["camera_aspect"] = [](const GuestLayout& l) { return l.camera_aspect; };
  a["time_scale"] = [](const GuestLayout& l) { return l.time_scale; };
  a["timing_object"] = [](const GuestLayout& l) { return l.timing_object; };
  a["limiter_frame_rate"] = [](const GuestLayout& l) { return l.limiter_frame_rate; };
  a["measured_frame_valid"] = [](const GuestLayout& l) { return l.measured_frame_valid; };
  a["measured_frame_ms"] = [](const GuestLayout& l) { return l.measured_frame_ms; };
  a["player_controller"] = [](const GuestLayout& l) { return l.player_controller; };
  a["coop_session"] = [](const GuestLayout& l) { return l.coop_session; };
  a["entity_manager"] = [](const GuestLayout& l) { return l.entity_manager; };
  a["font_object"] = [](const GuestLayout& l) { return l.font_object; };
  a["camera_tuning"] = [](const GuestLayout& l) { return l.camera_tuning; };
  a["presentation_flag"] = [](const GuestLayout& l) { return l.presentation_flag; };
  a["frontend_singleton"] = [](const GuestLayout& l) { return l.frontend_singleton; };
  for (size_t i = 0; i < 4; ++i) {
    a["thrown_weapon_vtable_" + std::to_string(i)] = [i](const GuestLayout& l) { return l.thrown_weapon_vtables[i]; };
  }
  a["repeat_present_return"] = [](const GuestLayout& l) { return l.repeat_present_return; };
  a["console_language_return"] = [](const GuestLayout& l) { return l.console_language_return; };
  a["null_audio_return"] = [](const GuestLayout& l) { return l.null_audio_return; };
  const char* waits[] = {"fence", "ring_space", "ring_progress", "swap_throttle", "resource"};
  for (size_t i = 0; i < 5; ++i) {
    a[std::string("gpu_wait_") + waits[i] + "_return"] = [i](const GuestLayout& l) { return l.gpu_wait_returns[i]; };
  }
  a["character_look_target"] = [](const GuestLayout& l) { return l.character_look_target; };
  a["character_ragdoll_flags"] = [](const GuestLayout& l) { return l.character_ragdoll_flags; };
  a["character_ragdoll_mask"] = [](const GuestLayout& l) { return uint32_t(l.character_ragdoll_mask); };
  a["character_primary_equipment"] = [](const GuestLayout& l) { return l.character_primary_equipment; };
  a["character_secondary_equipment"] = [](const GuestLayout& l) { return l.character_secondary_equipment; };
  a["character_vehicle"] = [](const GuestLayout& l) { return l.character_vehicle; };
  a["character_vehicle_flags"] = [](const GuestLayout& l) { return l.character_vehicle_flags; };
  a["vehicle_state"] = [](const GuestLayout& l) { return l.vehicle_state; };
  a["controller_desired_velocity"] = [](const GuestLayout& l) { return l.controller_desired_velocity; };
  a["controller_aim_0"] = [](const GuestLayout& l) { return l.controller_aim[0]; };
  a["controller_aim_1"] = [](const GuestLayout& l) { return l.controller_aim[1]; };
  a["action_character"] = [](const GuestLayout& l) { return l.action_character; };
  a["sequence_state"] = [](const GuestLayout& l) { return l.sequence_state; };
  a["camera_bytes"] = [](const GuestLayout& l) { return l.camera_bytes; };
  a["camera_velocity"] = [](const GuestLayout& l) { return l.camera_velocity; };
  a["camera_target"] = [](const GuestLayout& l) { return l.camera_target; };
  a["camera_previous_target"] = [](const GuestLayout& l) { return l.camera_previous_target; };
  a["camera_listener_point"] = [](const GuestLayout& l) { return l.camera_listener_point; };
  a["hit_merge_ticks"] = [](const GuestLayout& l) { return l.hit_merge_ticks; };
  a["jump_height_cheat"] = [](const GuestLayout& l) { return uint32_t(l.jump_height_cheat); };
  a["orb_pickup_vtable"] = [](const GuestLayout& l) { return l.orb_pickup_vtable; };
  a["orb_list_vtable"] = [](const GuestLayout& l) { return l.orb_list_vtable; };
  a["orb_supply_points"] = [](const GuestLayout& l) { return l.orb_supply_points; };
  a["orb_unset_pan"] = [](const GuestLayout& l) { return l.orb_unset_pan; };
  a["orb_pan_factor"] = [](const GuestLayout& l) { return l.orb_pan_factor; };
  return a;
}

std::set<std::string> BindingAddresses(const std::string& text) {
  std::set<std::string> found;
  const std::regex binding(R"(CRACKDOWN_(?:BIND|BIND_REPLACEMENT|GUEST_CALL)\(([0-9A-F]{8}))");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), binding); it != std::sregex_iterator(); ++it) {
    found.insert((*it)[1].str());
  }
  return found;
}

std::map<std::string, std::string> MidasmHooks(const std::string& overrides) {
  std::map<std::string, std::string> hooks;
  const std::regex hook(R"re(address = 0x([0-9A-F]{8})\r?\nname = "(\w+)")re");
  for (auto it = std::sregex_iterator(overrides.begin(), overrides.end(), hook); it != std::sregex_iterator(); ++it) {
    hooks[(*it)[1].str()] = (*it)[2].str();
  }
  return hooks;
}

void TestRecordMatchesTheLayout() {
  std::printf("the address record equals both layout tables\n");
  const std::map<std::string, Accessor> accessors = Accessors();
  std::set<std::string> covered;
  for (const Row& row : LoadMap()) {
    if (row.kind != "data" && row.kind != "return" && row.kind != "layout") {
      continue;
    }
    const auto it = accessors.find(row.name);
    CHECK(it != accessors.end());
    if (it == accessors.end()) {
      continue;
    }
    covered.insert(row.name);
    const bool hex = row.kind != "layout";
    CHECK(it->second(kLayoutBase) == (hex ? Hex(row.tu0) : uint32_t(std::stoul(row.tu0))));
    CHECK(it->second(kLayoutTitleUpdate6) == (hex ? Hex(row.tu6) : uint32_t(std::stoul(row.tu6))));
  }
  for (const auto& [name, accessor] : accessors) {
    CHECK(covered.count(name) == 1);
  }
}

void TestBindingsMatchTheRecord() {
  std::printf("each build's bindings are exactly the record's functions of that build\n");
  std::set<std::string> base, tu6;
  for (const Row& row : LoadMap()) {
    if (row.kind != "fn" && row.kind != "call" && row.kind != "base") {
      continue;
    }
    CHECK(!row.tu0.empty());
    // A base row is a hook that only the base build needs, such as the D3D census, so it has no Title Update 6 target.
    CHECK(row.kind != "base" || row.tu6.empty());
    base.insert(Upper(row.tu0));
    if (!row.tu6.empty()) {
      tu6.insert(Upper(row.tu6));
    }
  }
  CHECK(BindingAddresses(Read("src/guest_bindings_base.cpp")) == base);
  CHECK(BindingAddresses(Read("src/guest_bindings_tu6.cpp")) == tu6);
}

void TestMidasmHooksMatchTheRecord() {
  std::printf("each build's midasm hooks are exactly the record's sites of that build\n");
  std::map<std::string, std::string> base, tu6;
  for (const Row& row : LoadMap()) {
    if (row.kind != "site") {
      continue;
    }
    if (!row.tu0.empty()) {
      base[Upper(row.tu0)] = row.name;
    }
    CHECK(!row.tu6.empty());
    tu6[Upper(row.tu6)] = row.name;
  }
  CHECK(MidasmHooks(Read("config/overrides.toml")) == base);
  CHECK(MidasmHooks(Read("config/tu6/overrides.toml")) == tu6);
}

void TestNoAddressIsSharedBetweenBuilds() {
  std::printf("no guest address of one build is used as an address of the other\n");
  std::set<std::string> base, tu6;
  for (const Row& row : LoadMap()) {
    if (row.kind == "layout") {
      continue;
    }
    if (!row.tu0.empty()) {
      base.insert(Upper(row.tu0));
    }
    if (!row.tu6.empty()) {
      tu6.insert(Upper(row.tu6));
    }
  }
  CHECK(!base.empty() && !tu6.empty());
  for (const std::string& address : tu6) {
    CHECK(base.count(address) == 0);
  }
  for (const std::string& address : base) {
    CHECK(address.size() == 8);
  }
  for (const std::string& address : tu6) {
    const uint32_t value = Hex(address);
    CHECK(value >= 0x82000000 && value < 0x84000000);
  }
}

// Only the layout, the bindings and the hook configs may spell a guest address. The base addresses are checked in
// every other source, the Title Update 6 ones in all of them but their own files.
void TestSharedSourcesNameNoGuestAddress() {
  std::printf("shared host sources carry no guest address of either build\n");
  std::vector<std::string> base, tu6;
  for (const Row& row : LoadMap()) {
    if (row.kind == "layout") {
      continue;
    }
    if (!row.tu0.empty()) {
      base.push_back(Upper(row.tu0));
    }
    if (!row.tu6.empty()) {
      tu6.push_back(Upper(row.tu6));
    }
  }
  const std::set<std::string> allowed = {"guest_layout.h", "guest_bindings_base.cpp", "guest_bindings_tu6.cpp"};
  size_t scanned = 0;
  for (const auto& entry : fs::recursive_directory_iterator(Root() / "src")) {
    const std::string ext = entry.path().extension().string();
    if ((ext != ".cpp" && ext != ".h") || allowed.count(entry.path().filename().string())) {
      continue;
    }
    const std::string text = Upper(check::ReadText(entry.path()));
    ++scanned;
    for (const std::string& address : base) {
      CHECK(text.find(address) == std::string::npos);
    }
    for (const std::string& address : tu6) {
      CHECK(text.find(address) == std::string::npos);
    }
  }
  CHECK(scanned > 50);
}

void TestEveryCriticalTitleUpdate6TargetIsMapped() {
  std::printf("the critical hooks of the base build all have a Title Update 6 target\n");
  const std::set<std::string> unmapped = {"jump launch"};
  size_t functions = 0;
  for (const Row& row : LoadMap()) {
    if (row.kind != "fn") {
      continue;
    }
    ++functions;
    if (unmapped.count(row.name)) {
      CHECK(row.tu6.empty());
      continue;
    }
    CHECK(!row.tu6.empty());
    CHECK(Hex(row.tu6) != 0);
    CHECK(Hex(row.tu6) % 4 == 0);
    const std::string::size_type ratio = row.evidence.find("ratio ");
    CHECK(ratio != std::string::npos);
    CHECK(std::stoul(row.tu6_size) >= 8);
  }
  CHECK(functions >= 45);
}

void TestTheLayoutsDifferWhereTheGuestsDo() {
  std::printf("the layouts share nothing that moved\n");
  const std::map<std::string, Accessor> accessors = Accessors();
  for (const Row& row : LoadMap()) {
    if (row.kind != "data" && row.kind != "return") {
      continue;
    }
    const Accessor& get = accessors.at(row.name);
    CHECK(get(kLayoutBase) != get(kLayoutTitleUpdate6));
  }
  CHECK(&crackdown::LayoutFor(crackdown::content::GuestBuild::kBase) == &kLayoutBase);
  CHECK(&crackdown::LayoutFor(crackdown::content::GuestBuild::kTitleUpdate6) == &kLayoutTitleUpdate6);
  CHECK(kLayoutBase.hit_merge_ticks == 10 && kLayoutTitleUpdate6.hit_merge_ticks == 5);
  CHECK(kLayoutBase.character_ragdoll_mask == 0xFF && kLayoutTitleUpdate6.character_ragdoll_mask == 0x20);
}

void TestJumpHeightIsIsolatedToTheBuildThatHasIt() {
  std::printf("only the jump height cheat is off in Title Update 6\n");
  using crackdown::cheats::Cheat;
  CHECK(kLayoutBase.jump_height_cheat && !kLayoutTitleUpdate6.jump_height_cheat);
  CHECK(!check::ReadText(Root() / "src/guest_bindings_tu6.cpp").empty());
  CHECK(Read("src/guest_bindings_tu6.cpp").find("JumpLaunch") == std::string::npos);
  CHECK(Read("src/guest_bindings_base.cpp").find("JumpLaunch") != std::string::npos);
  for (const Cheat cheat : {Cheat::InfiniteHealth, Cheat::MovementSpeed, Cheat::NoFallDamage, Cheat::InfiniteAmmo,
                            Cheat::InfiniteVehicleHealth, Cheat::VehicleSpeed}) {
    CHECK(crackdown::cheats::Supported(cheat));
  }
  CHECK(crackdown::cheats::Supported(Cheat::JumpHeight) == crackdown::Layout().jump_height_cheat);
}

}  // namespace

int main() {
  TestRecordMatchesTheLayout();
  TestBindingsMatchTheRecord();
  TestMidasmHooksMatchTheRecord();
  TestNoAddressIsSharedBetweenBuilds();
  TestSharedSourcesNameNoGuestAddress();
  TestEveryCriticalTitleUpdate6TargetIsMapped();
  TestTheLayoutsDifferWhereTheGuestsDo();
  TestJumpHeightIsIsolatedToTheBuildThatHasIt();
  return check::Finish("guest_layout_tests");
}
