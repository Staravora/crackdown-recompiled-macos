// After a ragdoll the character's head look-at target can become NaN and stay NaN; the head controller and the audio
// listener, built from the same head data, then carry it into the character state. These hooks keep finite values
// flowing; with finite data they change nothing.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "free_camera.h"
#include "guest_hooks.h"
#include "guest_layout.h"
#include "guest_memory.h"
#include "nan_guard_rules.h"

namespace {

using crackdown::guest::LoadFloat;
using crackdown::guest::StoreFloat;

constexpr int64_t kLogGapMs = 2000;

uint8_t* GuestBase() {
  return rex::system::kernel_state()->memory()->virtual_membase();
}

// The title leaves the w lane as garbage, even a NaN bit pattern on the character position; only x, y and z count.
bool XyzFinite(uint8_t* base, uint32_t address) {
  float xyz[3];
  for (uint32_t i = 0; i < 3; ++i) {
    xyz[i] = LoadFloat(base, address + 4 * i);
  }
  return crackdown::nan_guard::XyzFinite(xyz);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct RepairLog {
  const char* what;
  std::atomic<int64_t> last_ms{0};
  std::atomic<uint32_t> pending{0};

  void Note() {
    const uint32_t count = pending.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t now = NowMs();
    int64_t last = last_ms.load(std::memory_order_relaxed);
    if (now - last >= kLogGapMs && last_ms.compare_exchange_strong(last, now)) {
      pending.store(0, std::memory_order_relaxed);
      REXLOG_WARN("[NAN_GUARD] {} was not finite ({} repair(s) since the last line)", what, count);
    }
  }
};

RepairLog g_listener_log{"The audio listener"};
RepairLog g_target_log{"The character's look-at target"};
RepairLog g_aim_log{"The player's aim point"};
RepairLog g_camera_log{"The camera"};

// Only the audio thread builds the listener.
float g_listener_matrix[4][3];
float g_listener_vector[3];
bool g_listener_valid = false;

// The game thread updates the character.
float g_last_target[3];
bool g_target_valid = false;

void GuardLookTarget(uint32_t character) {
  uint8_t* base = GuestBase();
  const uint32_t target = character + crackdown::Layout().character_look_target;
  if (XyzFinite(base, target)) {
    for (uint32_t i = 0; i < 3; ++i) g_last_target[i] = LoadFloat(base, target + 4 * i);
    g_target_valid = true;
    return;
  }
  g_target_log.Note();
  for (uint32_t i = 0; i < 3; ++i) {
    const float fallback = LoadFloat(base, character + crackdown::kCharacterPositionOffset + 4 * i);
    StoreFloat(base, target + 4 * i, g_target_valid ? g_last_target[i] : (std::isfinite(fallback) ? fallback : 0.0f));
  }
}

}  // namespace

// Codegen hook before the listener is submitted: r3 is the listener's 4x4 matrix (row 3 the position, from the
// character's head data), r4 its velocity vector.
void crackdown_audio_listener_guard(PPCRegister& matrix, PPCRegister& vector) {
  uint8_t* base = GuestBase();
  bool finite = XyzFinite(base, vector.u32);
  for (uint32_t row = 0; row < 4; ++row) {
    finite = finite && XyzFinite(base, matrix.u32 + 16 * row);
  }
  if (finite) {
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t i = 0; i < 3; ++i) g_listener_matrix[row][i] = LoadFloat(base, matrix.u32 + 16 * row + 4 * i);
    }
    for (uint32_t i = 0; i < 3; ++i) g_listener_vector[i] = LoadFloat(base, vector.u32 + 4 * i);
    g_listener_valid = true;
    return;
  }
  g_listener_log.Note();
  for (uint32_t row = 0; row < 4; ++row) {
    for (uint32_t i = 0; i < 3; ++i) {
      const float identity = row == i ? 1.0f : 0.0f;
      StoreFloat(base, matrix.u32 + 16 * row + 4 * i, g_listener_valid ? g_listener_matrix[row][i] : identity);
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    StoreFloat(base, vector.u32 + 4 * i, g_listener_valid ? g_listener_vector[i] : 0.0f);
  }
}

// Codegen hooks before the two reads of the character's look-at target, in the head controller and in the look matrix
// update. r31 is the character. A NaN target becomes the last finite target (no head turn).
void crackdown_look_target_guard(PPCRegister& character) {
  GuardLookTarget(character.u32);
}

// The player controller's aim update (r3 = controller) writes two aim points, the first copied to the character's
// look-at target. A target whose body went NaN hands it a NaN point every update; the previous point is kept.
void crackdown::hooks::AimUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const std::array<uint32_t, 2>& kAimOffsets = crackdown::Layout().controller_aim;
  constexpr uint32_t kCharacterOffsetInController = 292;
  constexpr uint32_t kCharacterBase = 32;
  const uint32_t controller = ctx.r3.u32;
  uint8_t saved[2][16];
  for (int i = 0; i < 2; ++i) {
    std::memcpy(saved[i], rex::memory::GuestPtr<const uint8_t*>(base, controller + kAimOffsets[i]), 16);
  }
  original(ctx, base);
  for (int i = 0; i < 2; ++i) {
    const uint32_t field = controller + kAimOffsets[i];
    if (XyzFinite(base, field)) {
      continue;
    }
    g_aim_log.Note();
    uint8_t* out = rex::memory::GuestPtr<uint8_t*>(base, field);
    bool saved_finite = true;
    for (int lane = 0; lane < 3; ++lane) {
      uint32_t raw = 0;
      std::memcpy(&raw, saved[i] + 4 * lane, 4);
      saved_finite = saved_finite && std::isfinite(std::bit_cast<float>(std::byteswap(raw)));
    }
    if (saved_finite) {
      std::memcpy(out, saved[i], 12);
      continue;
    }
    const uint32_t subobject = crackdown::guest::Load32(base, controller + kCharacterOffsetInController);
    const uint32_t character = crackdown::guest::Plausible(subobject) ? subobject - kCharacterBase : 0;
    for (uint32_t lane = 0; lane < 3; ++lane) {
      const float position =
          character ? LoadFloat(base, character + crackdown::kCharacterPositionOffset + 4 * lane) : 0.0f;
      StoreFloat(base, field + 4 * lane, std::isfinite(position) ? position : 0.0f);
    }
  }
}

// The camera's per-frame update (r3 = camera). Its target-derived points feed the audio listener and the aim ray; a
// non-finite camera state is put back to the state before the update.
void crackdown::hooks::CameraUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t camera = ctx.r3.u32;
  const bool plausible = crackdown::guest::Plausible(camera);
  thread_local std::array<uint8_t, crackdown::nan_guard::kMaxCameraBytes> saved;
  const size_t bytes = crackdown::Layout().camera_bytes;
  if (plausible) {
    crackdown::free_camera::BeforeCameraUpdate(base, camera);
    std::memcpy(saved.data(), rex::memory::GuestPtr<const uint8_t*>(base, camera), bytes);
  }
  original(ctx, base);
  if (!plausible) {
    return;
  }
  std::string broken;
  for (const crackdown::nan_guard::CameraField& field : crackdown::nan_guard::CameraFields(crackdown::Layout())) {
    if (XyzFinite(base, camera + field.offset)) {
      continue;
    }
    broken += broken.empty() ? field.name : std::string(", ") + field.name;
  }
  if (!broken.empty()) {
    g_camera_log.Note();
    static std::atomic_flag logged_fields;
    if (!logged_fields.test_and_set()) {
      REXLOG_WARN("[NAN_GUARD] camera fields not finite after its update: {}", broken);
    }
    std::memcpy(rex::memory::GuestPtr<uint8_t*>(base, camera), saved.data(), bytes);
  }
  crackdown::free_camera::AfterCameraUpdate(base, camera);
}
