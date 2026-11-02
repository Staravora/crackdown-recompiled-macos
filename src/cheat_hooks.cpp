#include "cheat_hooks.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "cheats.h"
#include "guest_hooks.h"
#include "guest_layout.h"
#include "guest_memory.h"

namespace crackdown::cheats {
namespace {

using guest::Load32;
using guest::LoadFloat;
using guest::Plausible;
using guest::Store32;
using guest::StoreFloat;

using GuestFunction = hooks::GuestFunction;

// The local player controller global (Layout().player_controller); its +292 holds the controlled Character plus the
// 32-byte base offset. Layout().coop_session is non-null while a co-op or network game session exists.
constexpr uint32_t kCharacterOffsetInController = 292;
constexpr uint32_t kCharacterBase = 32;

constexpr uint32_t kHealthOffset = 400;
constexpr uint32_t kHealthScaleOffset = 404;
constexpr uint32_t kHealthMultiplierOffset = 408;

constexpr uint32_t kDamageInfoTypeOffset = 128;
constexpr uint32_t kDamageTypeFall = 2;

constexpr uint32_t kWeaponProfileOffset = 152;
constexpr uint32_t kWeaponOwnerOffset = 160;
constexpr uint32_t kWeaponClipOffset = 1312;
constexpr uint32_t kWeaponReserveOffset = 1316;
constexpr uint32_t kProfileMaxClipOffset = 576;
constexpr uint32_t kProfileMaxReserveOffset = 580;
constexpr uint32_t kMaxAmmoSanity = 100000;

constexpr uint8_t kDrivingFlags = 0x18;
constexpr uint32_t kVehicleWreckedState = 4;
constexpr uint32_t kEngineInstanceVehicleOffset = 232;
constexpr uint32_t kEngineInstanceTorqueScaleOffset = 728;

constexpr uint32_t kActionOwnerOffset = 16;
constexpr uint32_t kCharacterMotionFlagsOffset = 2345;
constexpr uint8_t kUsesMotionFlag = 0x40;
constexpr uint32_t kLaunchVelocityOffset = 1876;

std::atomic<bool> g_available{false};

bool SinglePlayer(uint8_t* base) { return Load32(base, Layout().coop_session) == 0; }

bool ActsOn(uint8_t* base, Cheat cheat, uint32_t entity) {
  return Enabled(cheat) && entity && SinglePlayer(base) && entity == LocalPlayerCharacter(base);
}

// The vehicle the local player is driving, or 0.
uint32_t PlayerDrivenVehicle(uint8_t* base) {
  const uint32_t character = LocalPlayerCharacter(base);
  const uint8_t flags = character ? guest::Load8(base, character + Layout().character_vehicle_flags) : 0;
  if (!character || (flags & kDrivingFlags) != kDrivingFlags) {
    return 0;
  }
  const uint32_t vehicle = Load32(base, character + Layout().character_vehicle);
  return Plausible(vehicle) ? vehicle : 0;
}

bool IsThrownWeapon(uint8_t* base, uint32_t weapon) {
  const uint32_t vtable = Load32(base, weapon);
  const auto& thrown = Layout().thrown_weapon_vtables;
  return std::find(thrown.begin(), thrown.end(), vtable) != thrown.end();
}

bool OwnedByPlayer(uint8_t* base, uint32_t weapon, uint32_t character) {
  return Plausible(weapon) && Load32(base, weapon + kWeaponOwnerOffset) == character;
}

void RefillWeapon(uint8_t* base, uint32_t weapon) {
  const uint32_t profile = Load32(base, weapon + kWeaponProfileOffset);
  if (!Plausible(profile)) {
    return;
  }
  const uint32_t max_clip = Load32(base, profile + kProfileMaxClipOffset);
  const uint32_t max_reserve = Load32(base, profile + kProfileMaxReserveOffset);
  if (max_clip && max_clip < kMaxAmmoSanity) {
    Store32(base, weapon + kWeaponClipOffset, max_clip);
  }
  if (max_reserve && max_reserve < kMaxAmmoSanity) {
    Store32(base, weapon + kWeaponReserveOffset, max_reserve);
  }
}

void RefillHeldWeapons(uint8_t* base, uint32_t character) {
  for (const uint32_t slot : {Layout().character_primary_equipment, Layout().character_secondary_equipment}) {
    const uint32_t weapon = Load32(base, character + slot);
    if (OwnedByPlayer(base, weapon, character) && !IsThrownWeapon(base, weapon)) {
      RefillWeapon(base, weapon);
    }
  }
}

void RefillVehicleHealth(uint8_t* base) {
  const uint32_t vehicle = PlayerDrivenVehicle(base);
  if (!vehicle || Load32(base, vehicle + Layout().vehicle_state) == kVehicleWreckedState) {
    return;
  }
  const float full = LoadFloat(base, vehicle + kHealthScaleOffset) * LoadFloat(base, vehicle + kHealthMultiplierOffset);
  if (full > 0.0f) {
    StoreFloat(base, vehicle + kHealthOffset, full);
  }
}

// Vehicle damage entry: r3 = vehicle, r4 = damage info. The player's own vehicle takes no damage.
void GuardVehicle(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (Enabled(Cheat::InfiniteVehicleHealth) && SinglePlayer(base) && ctx.r3.u32 == PlayerDrivenVehicle(base)) {
    ctx.f1.f64 = 0.0;
    return;
  }
  original(ctx, base);
}

// Engine torque: r5 = the Havok vehicle instance, whose scale multiplies torque and resistance for that call only.
void BoostEngine(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t instance = ctx.r5.u32;
  uint32_t scale_address = 0;
  float scale = 0.0f;
  if (Enabled(Cheat::VehicleSpeed) && Plausible(instance) && SinglePlayer(base)) {
    const uint32_t subobject = Load32(base, instance + kEngineInstanceVehicleOffset);
    if (subobject && subobject - kCharacterBase == PlayerDrivenVehicle(base)) {
      scale_address = instance + kEngineInstanceTorqueScaleOffset;
      scale = LoadFloat(base, scale_address);
      StoreFloat(base, scale_address, scale * float(Factor(Cheat::VehicleSpeed)));
    }
  }
  original(ctx, base);
  if (scale_address) {
    StoreFloat(base, scale_address, scale);
  }
}

// The player's stick-to-world velocity (x at +0, z at +8): r3 = the player controller.
void ScaleWalkVelocity(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t player = ctx.r3.u32;
  original(ctx, base);
  if (!Enabled(Cheat::MovementSpeed) || !Plausible(player) || player != Load32(base, Layout().player_controller) ||
      !SinglePlayer(base)) {
    return;
  }
  const float factor = float(Factor(Cheat::MovementSpeed));
  const uint32_t velocity = player + Layout().controller_desired_velocity;
  StoreFloat(base, velocity, LoadFloat(base, velocity) * factor);
  StoreFloat(base, velocity + 8, LoadFloat(base, velocity + 8) * factor);
}

// The ballistic launch solver: r3 = the action. The launch speed is scaled by the square root of the multiplier so
// the apex height scales by about the multiplier.
void ScaleJumpLaunch(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t action = ctx.r3.u32;
  const uint32_t owner = Load32(base, action + kActionOwnerOffset);
  const uint32_t character = Plausible(owner) ? Load32(base, owner + Layout().action_character) : 0;
  original(ctx, base);
  if (!Layout().jump_height_cheat || !Enabled(Cheat::JumpHeight) || !Plausible(character) ||
      character != LocalPlayerCharacter(base) || !SinglePlayer(base) ||
      !(guest::Load8(base, character + kCharacterMotionFlagsOffset) & kUsesMotionFlag)) {
    return;
  }
  const float launch = LoadFloat(base, character + kLaunchVelocityOffset);
  const float scaled = launch * std::sqrt(float(Factor(Cheat::JumpHeight)));
  StoreFloat(base, character + kLaunchVelocityOffset, scaled);
}

void RefillHealth(uint8_t* base, uint32_t character) {
  const float current = LoadFloat(base, character + kHealthOffset);
  const float full = LoadFloat(base, character + kHealthScaleOffset) * LoadFloat(base, character + kHealthMultiplierOffset);
  if (current > 0.0f && full > current) {
    StoreFloat(base, character + kHealthOffset, full);
  }
}

// The weapon's clip is put back after a call that spent ammo, so the shot goes out and the count stays.
template <typename Owned>
void KeepClip(PPCContext& ctx, uint8_t* base, Cheat cheat, GuestFunction original, Owned owned) {
  const uint32_t weapon = ctx.r3.u32;
  const uint32_t character = LocalPlayerCharacter(base);
  uint32_t clip_before = 0;
  bool keep = false;
  if (Enabled(cheat) && Plausible(weapon) && SinglePlayer(base) && character) {
    keep = owned(base, weapon, character);
    clip_before = keep ? Load32(base, weapon + kWeaponClipOffset) : 0;
  }
  original(ctx, base);
  if (!keep) {
    return;
  }
  const uint32_t clip_after = Load32(base, weapon + kWeaponClipOffset);
  if (clip_after < clip_before) {
    Store32(base, weapon + kWeaponClipOffset, clip_before);
  }
}

}  // namespace

bool Available() { return g_available.load(std::memory_order_relaxed); }

uint32_t LocalPlayerCharacter(uint8_t* base) {
  const uint32_t controller = Load32(base, Layout().player_controller);
  if (!Plausible(controller)) {
    return 0;
  }
  const uint32_t subobject = Load32(base, controller + kCharacterOffsetInController);
  return Plausible(subobject) ? subobject - kCharacterBase : 0;
}

void RunGameTick(uint8_t* base) {
  const uint32_t character = LocalPlayerCharacter(base);
  const bool available = character && SinglePlayer(base);
  g_available.store(available, std::memory_order_relaxed);
  for (const CheatInfo& info : kCheats) {
    if (!Enabled(info.cheat) || !TakeRefillRequest(info.cheat)) {
      continue;
    }
    if (!available) {
      RequestRefill(info.cheat);
      continue;
    }
    switch (info.cheat) {
      case Cheat::InfiniteHealth:
        RefillHealth(base, character);
        break;
      case Cheat::InfiniteVehicleHealth:
        RefillVehicleHealth(base);
        break;
      case Cheat::InfiniteAmmo:
        RefillHeldWeapons(base, character);
        break;
      default:
        break;
    }
  }
}

}  // namespace crackdown::cheats

// The character's damage entry. Skipping it is what the game's own god mode does.
void crackdown::hooks::DamageEntry(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  using namespace crackdown::cheats;
  const uint32_t character = ctx.r3.u32;
  if (ActsOn(base, Cheat::InfiniteHealth, character)) {
    return;
  }
  if (ActsOn(base, Cheat::NoFallDamage, character) &&
      Load32(base, ctx.r4.u32 + kDamageInfoTypeOffset) == kDamageTypeFall) {
    return;
  }
  original(ctx, base);
}

// The two weapon fire paths that spend the clip.
void crackdown::hooks::FireWeaponTracked(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  crackdown::cheats::KeepClip(ctx, base, crackdown::cheats::Cheat::InfiniteAmmo, original,
                              crackdown::cheats::OwnedByPlayer);
}

bool crackdown::cheats::PlayerIsDriving(uint8_t* base) { return crackdown::cheats::PlayerDrivenVehicle(base) != 0; }

void crackdown::hooks::VehicleDamage(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  crackdown::cheats::GuardVehicle(ctx, base, original);
}

void crackdown::hooks::EngineTorque(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  crackdown::cheats::BoostEngine(ctx, base, original);
}

void crackdown::hooks::WalkVelocity(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  crackdown::cheats::ScaleWalkVelocity(ctx, base, original);
}

void crackdown::hooks::JumpLaunch(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  crackdown::cheats::ScaleJumpLaunch(ctx, base, original);
}
