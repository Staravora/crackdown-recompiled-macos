#include "free_camera.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window_listener.h>

#include "cheat_hooks.h"
#include "cheats.h"
#include "guest_layout.h"

REXCVAR_DEFINE_STRING(crackdown_free_camera_start, "", "Crackdown",
                      "Start with the free camera on, at x,y,z,yaw,pitch");
REXCVAR_DEFINE_INT32(crackdown_free_camera_freeze_after, 0, "Crackdown",
                     "Freeze time this many frames into play, once the screen has faded in");

namespace crackdown::free_camera {
namespace {

using Clock = std::chrono::steady_clock;
using rex::ui::VirtualKey;

// Above ReXApp's input listener, which hands keys to the game. Nothing here consumes them.
constexpr size_t kInputZOrder = 900;

// Basis z is forward, y up and x left. Each is a vector of four floats, and the fourth is left alone.
constexpr uint32_t kBasisX = 272;
constexpr uint32_t kBasisY = 288;
constexpr uint32_t kBasisZ = 304;
constexpr uint32_t kPosition = 320;
constexpr size_t kPoseBytes = 64;

constexpr float kSpeed = 12.0f;
constexpr float kFastSpeed = 60.0f;
constexpr float kLookPerPixel = 0.0025f;
constexpr float kMaxPitch = 1.55f;
constexpr float kMaxStep = 0.1f;
constexpr int kSpots = 9;

struct Vec3 {
  float x, y, z;
};

struct Pose {
  Vec3 position;
  float yaw;
  float pitch;
};

std::atomic<uint32_t> g_keys{0};
std::atomic<bool> g_fast{false};
std::atomic<int> g_look_x{0};
std::atomic<int> g_look_y{0};
std::atomic<int> g_jump_to{0};
std::atomic<int> g_save_to{0};
std::atomic<bool> g_forced{false};

rex::ui::Window* g_window = nullptr;
uint8_t* g_membase = nullptr;
std::filesystem::path g_spots_path;

// The rest is the camera thread's.
std::array<std::optional<Pose>, kSpots> g_spots;
std::optional<Pose> g_start;
std::optional<Pose> g_pose;
std::array<uint8_t, kPoseBytes> g_game_pose{};
uint32_t g_overridden_camera = 0;
Clock::time_point g_last_step{};
std::atomic<bool> g_frozen{false};
int g_play_frames = 0;
uint32_t g_scale_bits = 0;

enum Key : uint32_t {
  kForward = 1 << 0,
  kBack = 1 << 1,
  kLeft = 1 << 2,
  kRight = 1 << 3,
  kDown = 1 << 4,
  kUp = 1 << 5,
};

std::optional<uint32_t> KeyBit(VirtualKey key) {
  switch (key) {
    case VirtualKey::kW:
      return kForward;
    case VirtualKey::kS:
      return kBack;
    case VirtualKey::kA:
      return kLeft;
    case VirtualKey::kD:
      return kRight;
    case VirtualKey::kQ:
      return kDown;
    case VirtualKey::kE:
      return kUp;
    default:
      return std::nullopt;
  }
}

bool Wanted() {
  return g_forced.load(std::memory_order_relaxed) || cheats::Enabled(cheats::Cheat::FreeCamera);
}

class Listener final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (e.target() != g_window) {
      return;
    }
    g_fast.store(e.is_shift_pressed(), std::memory_order_relaxed);
    if (const std::optional<uint32_t> bit = KeyBit(e.virtual_key())) {
      g_keys.fetch_or(*bit, std::memory_order_relaxed);
      return;
    }
    if (!Wanted() || e.prev_state()) {
      return;
    }
    if (e.virtual_key() == VirtualKey::kF) {
      cheats::Cycle(cheats::Cheat::FreezeTime);
      return;
    }
    const int digit = int(e.virtual_key()) - int(VirtualKey::k1);
    if (digit >= 0 && digit < kSpots) {
      (e.is_shift_pressed() ? g_save_to : g_jump_to).store(digit + 1, std::memory_order_relaxed);
    }
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    if (e.target() != g_window) {
      return;
    }
    g_fast.store(e.is_shift_pressed(), std::memory_order_relaxed);
    if (const std::optional<uint32_t> bit = KeyBit(e.virtual_key())) {
      g_keys.fetch_and(~*bit, std::memory_order_relaxed);
    }
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (e.target() == g_window && Wanted()) {
      g_look_x.fetch_add(int(e.dx()), std::memory_order_relaxed);
      g_look_y.fetch_add(int(e.dy()), std::memory_order_relaxed);
    }
  }
};

Listener g_listener;

float LoadFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, base + address, sizeof(bits));
  return std::bit_cast<float>(std::byteswap(bits));
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  const uint32_t bits = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(base + address, &bits, sizeof(bits));
}

void StoreVec(uint8_t* base, uint32_t address, const Vec3& value) {
  StoreFloat(base, address, value.x);
  StoreFloat(base, address + 4, value.y);
  StoreFloat(base, address + 8, value.z);
}

Vec3 Forward(const Pose& pose) {
  const float flat = std::cos(pose.pitch);
  return {std::sin(pose.yaw) * flat, std::sin(pose.pitch), std::cos(pose.yaw) * flat};
}

Vec3 Left(const Pose& pose) {
  return {std::cos(pose.yaw), 0.0f, -std::sin(pose.yaw)};
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Pose PoseFromGame(const uint8_t* base, uint32_t camera) {
  const Vec3 forward = {LoadFloat(base, camera + kBasisZ), LoadFloat(base, camera + kBasisZ + 4),
                        LoadFloat(base, camera + kBasisZ + 8)};
  return {{LoadFloat(base, camera + kPosition), LoadFloat(base, camera + kPosition + 4),
           LoadFloat(base, camera + kPosition + 8)},
          std::atan2(forward.x, forward.z),
          std::asin(std::clamp(forward.y, -1.0f, 1.0f))};
}

void Move(Pose& pose) {
  const Clock::time_point now = Clock::now();
  const float step = g_last_step == Clock::time_point{}
                         ? 0.0f
                         : std::min(std::chrono::duration<float>(now - g_last_step).count(), kMaxStep);
  g_last_step = now;

  pose.yaw -= float(g_look_x.exchange(0, std::memory_order_relaxed)) * kLookPerPixel;
  pose.pitch = std::clamp(pose.pitch - float(g_look_y.exchange(0, std::memory_order_relaxed)) * kLookPerPixel,
                          -kMaxPitch, kMaxPitch);

  const uint32_t keys = g_keys.load(std::memory_order_relaxed);
  const float along = float((keys & kForward) != 0) - float((keys & kBack) != 0);
  const float across = float((keys & kLeft) != 0) - float((keys & kRight) != 0);
  const float lift = float((keys & kUp) != 0) - float((keys & kDown) != 0);
  const float distance = step * (g_fast.load(std::memory_order_relaxed) ? kFastSpeed : kSpeed);
  const Vec3 forward = Forward(pose), left = Left(pose);
  pose.position.x += (forward.x * along + left.x * across) * distance;
  pose.position.y += (forward.y * along + lift) * distance;
  pose.position.z += (forward.z * along + left.z * across) * distance;
}

void SaveSpots() {
  std::ofstream out(g_spots_path);
  for (int i = 0; i < kSpots; ++i) {
    if (const std::optional<Pose>& spot = g_spots[size_t(i)]) {
      out << i + 1 << ' ' << spot->position.x << ' ' << spot->position.y << ' ' << spot->position.z << ' ' << spot->yaw
          << ' ' << spot->pitch << '\n';
    }
  }
}

void LoadSpots() {
  std::ifstream in(g_spots_path);
  int number;
  Pose pose;
  while (in >> number >> pose.position.x >> pose.position.y >> pose.position.z >> pose.yaw >> pose.pitch) {
    if (number >= 1 && number <= kSpots) {
      g_spots[size_t(number - 1)] = pose;
    }
  }
}

void HandleSpots(Pose& pose) {
  if (const int save = g_save_to.exchange(0, std::memory_order_relaxed)) {
    g_spots[size_t(save - 1)] = pose;
    SaveSpots();
    REXLOG_INFO("Free camera: saved spot {}: {},{},{},{},{}", save, pose.position.x, pose.position.y, pose.position.z,
                pose.yaw, pose.pitch);
  }
  if (const int jump = g_jump_to.exchange(0, std::memory_order_relaxed)) {
    if (const std::optional<Pose>& spot = g_spots[size_t(jump - 1)]) {
      pose = *spot;
    }
  }
}

std::optional<Pose> ParsePose(const std::string& text) {
  Pose pose;
  if (std::sscanf(text.c_str(), "%f,%f,%f,%f,%f", &pose.position.x, &pose.position.y, &pose.position.z, &pose.yaw,
                  &pose.pitch) != 5) {
    return std::nullopt;
  }
  return pose;
}

// The game picks what to load and how much detail to draw from where the player stands, not where the camera is. Its
// physics puts the real position back every update, so this is written every update, and the player keeps their place
// on the map. Their height doesn't survive it: they drop under the map.
void CarryPlayer(uint8_t* base) {
  if (const uint32_t character = cheats::LocalPlayerCharacter(base)) {
    StoreVec(base, character + kCharacterPositionOffset, g_pose->position);
  }
}

}  // namespace

void Install(rex::ui::Window* window, uint8_t* membase, const std::filesystem::path& user_data_root) {
  g_window = window;
  g_membase = membase;
  g_spots_path = user_data_root / "camera-spots.txt";
  LoadSpots();
  g_start = ParsePose(REXCVAR_GET(crackdown_free_camera_start));
  g_forced.store(g_start.has_value(), std::memory_order_relaxed);
  window->AddInputListener(&g_listener, kInputZOrder);
}

bool Active() {
  // Outside play this would hold back the menus, including the benchmark's way through them.
  return Wanted() && cheats::Available();
}

bool TimeFrozen() {
  return g_frozen.load(std::memory_order_relaxed);
}

void BeforeCameraUpdate(uint8_t* base, uint32_t camera) {
  if (camera == g_overridden_camera) {
    std::memcpy(base + camera + kBasisX, g_game_pose.data(), kPoseBytes);
    g_overridden_camera = 0;
  }
}

void AfterCameraUpdate(uint8_t* base, uint32_t camera) {
  if (!Active()) {
    g_pose.reset();
    return;
  }
  // The game keeps four cameras, and only the live one has been anywhere.
  if (LoadFloat(base, camera + kPosition) == 0.0f && LoadFloat(base, camera + kPosition + 8) == 0.0f) {
    return;
  }
  if (!g_pose) {
    g_pose = g_start ? *g_start : PoseFromGame(base, camera);
    g_last_step = {};
    REXLOG_INFO("Free camera: on at {},{},{},{},{}", g_pose->position.x, g_pose->position.y, g_pose->position.z,
                g_pose->yaw, g_pose->pitch);
  }
  HandleSpots(*g_pose);
  Move(*g_pose);

  std::memcpy(g_game_pose.data(), base + camera + kBasisX, kPoseBytes);
  g_overridden_camera = camera;
  const Vec3 forward = Forward(*g_pose), left = Left(*g_pose);
  StoreVec(base, camera + kBasisX, left);
  StoreVec(base, camera + kBasisY, Cross(forward, left));
  StoreVec(base, camera + kBasisZ, forward);
  StoreVec(base, camera + kPosition, g_pose->position);
  CarryPlayer(base);
}

void OnGuestFrame() {
  if (!g_membase) {
    return;
  }
  const uint32_t address = Layout().time_scale;
  const bool playing = cheats::Available();
  g_play_frames = playing ? g_play_frames + 1 : 0;
  const int after = REXCVAR_GET(crackdown_free_camera_freeze_after);
  const bool frozen = cheats::Enabled(cheats::Cheat::FreezeTime) || (after > 0 && g_play_frames >= after);
  if (frozen && playing) {
    if (!TimeFrozen()) {
      std::memcpy(&g_scale_bits, g_membase + address, sizeof(g_scale_bits));
      g_frozen.store(true, std::memory_order_relaxed);
      REXLOG_INFO("Free camera: time frozen");
    }
    StoreFloat(g_membase, address, kFrozenTimeScale);
  } else if (TimeFrozen()) {
    std::memcpy(g_membase + address, &g_scale_bits, sizeof(g_scale_bits));
    g_frozen.store(false, std::memory_order_relaxed);
  }
}

}  // namespace crackdown::free_camera
