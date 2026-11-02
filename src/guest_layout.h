#pragma once

#include <array>
#include <cstdint>

#include "guest_build.h"

namespace crackdown {

// Where the host code finds the game's globals and object fields in each recompiled guest. The Title Update 6 guest is
// a rebuild, so these differ; code that is shared between the builds reads them through Layout(). Evidence per entry:
// config/tu6/address-map.csv.
struct GuestLayout {
  uint32_t camera_aspect;
  uint32_t time_scale;
  uint32_t timing_object;
  uint32_t limiter_frame_rate;
  uint32_t measured_frame_valid;
  uint32_t measured_frame_ms;
  uint32_t player_controller;
  uint32_t coop_session;
  uint32_t entity_manager;
  uint32_t font_object;
  uint32_t camera_tuning;
  uint32_t presentation_flag;
  uint32_t frontend_singleton;
  std::array<uint32_t, 4> thrown_weapon_vtables;

  uint32_t repeat_present_return;
  uint32_t console_language_return;
  uint32_t null_audio_return;
  // fence, ring space, ring progress, swap throttle, resource
  std::array<uint32_t, 5> gpu_wait_returns;

  uint32_t character_look_target;
  uint32_t character_ragdoll_flags;
  uint8_t character_ragdoll_mask;
  uint32_t character_primary_equipment;
  uint32_t character_secondary_equipment;
  uint32_t character_vehicle;
  uint32_t character_vehicle_flags;
  uint32_t vehicle_state;
  uint32_t controller_desired_velocity;
  std::array<uint32_t, 2> controller_aim;
  uint32_t action_character;
  // The intro sequence state, counted from the address its per-frame callback receives.
  uint32_t sequence_state;

  uint32_t camera_bytes;
  uint32_t camera_velocity;
  uint32_t camera_target;
  uint32_t camera_previous_target;
  uint32_t camera_listener_point;

  uint32_t hit_merge_ticks;
  bool jump_height_cheat;

  // The orb map overlay (src/orb_map.cpp). cPickup's own vtable, and the list-entry descriptor vtable the minimap and
  // pause map markers share with supply points; the supply point table global; and the two float constants the pause
  // map view's world-to-layout conversion reads (an "unset" pan sentinel and the pan blend factor).
  uint32_t orb_pickup_vtable;
  uint32_t orb_list_vtable;
  uint32_t orb_supply_points;
  uint32_t orb_unset_pan;
  uint32_t orb_pan_factor;
};

inline constexpr uint32_t kCharacterPositionOffset = 0x140;

inline constexpr GuestLayout kLayoutBase = {
    .camera_aspect = 0x82BAA498,
    .time_scale = 0x82C63578,
    .timing_object = 0x82DE25C0,
    .limiter_frame_rate = 0x82BAA330,
    .measured_frame_valid = 0x82DE3FB0,
    .measured_frame_ms = 0x82D99128,
    .player_controller = 0x82DE3F3C,
    .coop_session = 0x82DE3EA8,
    .entity_manager = 0x82DE3F2C,
    .font_object = 0x82D94CBC,
    .camera_tuning = 0x82DE258C,
    .presentation_flag = 0x82DE450A,
    .frontend_singleton = 0x835D3C70,
    .thrown_weapon_vtables = {0x820991D8, 0x8209C3D0, 0x8209CA90, 0x8209D0D8},
    .repeat_present_return = 0x826CE148,
    .console_language_return = 0x8232293C,
    .null_audio_return = 0x8225D5F0,
    .gpu_wait_returns = {0x826BB590, 0x826BAD10, 0x826BA6EC, 0x826BD160, 0x826CD9C0},
    .character_look_target = 5040,
    .character_ragdoll_flags = 4220,
    .character_ragdoll_mask = 0xFF,
    .character_primary_equipment = 6304,
    .character_secondary_equipment = 6308,
    .character_vehicle = 5580,
    .character_vehicle_flags = 5457,
    .vehicle_state = 3064,
    .controller_desired_velocity = 5072,
    .controller_aim = {5152, 5104},
    .action_character = 260,
    .sequence_state = 136,
    .camera_bytes = 7152,
    .camera_velocity = 6336,
    .camera_target = 6352,
    .camera_previous_target = 6368,
    .camera_listener_point = 6384,
    .hit_merge_ticks = 10,
    .jump_height_cheat = true,
    .orb_pickup_vtable = 0x820B39C8,
    .orb_list_vtable = 0x820A83C8,
    .orb_supply_points = 0x82DE2678,
    .orb_unset_pan = 0x820722DC,
    .orb_pan_factor = 0x8207047C,
};

inline constexpr GuestLayout kLayoutTitleUpdate6 = {
    .camera_aspect = 0x82C3ACB8,
    .time_scale = 0x82CF6C98,
    .timing_object = 0x82E79A14,
    .limiter_frame_rate = 0x82C3A9A4,
    .measured_frame_valid = 0x82E7B447,
    .measured_frame_ms = 0x82E2C268,
    .player_controller = 0x82E7B3DC,
    .coop_session = 0x82E7B344,
    .entity_manager = 0x82E7B3CC,
    .font_object = 0x82E27B78,
    .camera_tuning = 0x82E799DC,
    .presentation_flag = 0x82E7BA07,
    .frontend_singleton = 0x835FB5A0,
    .thrown_weapon_vtables = {0x820A0530, 0x8209C2B0, 0x8209C988, 0x8209CFE8},
    .repeat_present_return = 0x827574F0,
    .console_language_return = 0x82356964,
    .null_audio_return = 0x82282388,
    .gpu_wait_returns = {0x82744928, 0x827440A8, 0x82743A84, 0x827464F8, 0x82756D68},
    .character_look_target = 3088,
    .character_ragdoll_flags = 2436,
    .character_ragdoll_mask = 0x20,
    .character_primary_equipment = 4304,
    .character_secondary_equipment = 4308,
    .character_vehicle = 3588,
    .character_vehicle_flags = 3473,
    .vehicle_state = 3048,
    .controller_desired_velocity = 5696,
    .controller_aim = {5760, 5728},
    .action_character = 252,
    .sequence_state = 140,
    .camera_bytes = 7184,
    .camera_velocity = 6368,
    .camera_target = 6384,
    .camera_previous_target = 6400,
    .camera_listener_point = 6416,
    .hit_merge_ticks = 5,
    .jump_height_cheat = false,
    .orb_pickup_vtable = 0x820BAD38,
    .orb_list_vtable = 0x820AD9D8,
    .orb_supply_points = 0x82E79AD4,
    .orb_unset_pan = 0x820725DC,
    .orb_pan_factor = 0x82070594,
};

inline constexpr const GuestLayout& LayoutFor(content::GuestBuild build) {
  return build == content::GuestBuild::kTitleUpdate6 ? kLayoutTitleUpdate6 : kLayoutBase;
}

inline const GuestLayout& Layout() {
  return LayoutFor(RunningBuild());
}

}  // namespace crackdown
