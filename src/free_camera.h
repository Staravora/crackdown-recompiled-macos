#pragma once

#include <cstdint>
#include <filesystem>

#include <rex/ui/window.h>

namespace crackdown::free_camera {

// A test tool: flies the game's own camera, so the emulator and the native renderer both follow it. The game keeps
// updating its camera underneath, and the override goes on after that update and comes off before the next, so the
// game's camera logic never sees it. Turned on from the cheat menu.
//
// The world's detail follows the player, so the player's position goes along too. They stay where they are on the map
// but drop under the map, and stay at that height once the camera is turned off.
//
// WASD moves, Q and E go down and up, the mouse looks and Shift goes faster. 1-9 jump to a saved spot, Shift+1-9 save
// one to camera-spots.txt in the user data folder, and F freezes time.
void Install(rex::ui::Window* window, uint8_t* membase, const std::filesystem::path& user_data_root);

// What freezing time multiplies game time by. Not 0, because the camera divides by its time step.
inline constexpr float kFrozenTimeScale = 0.001f;

// Whether the player's input should stay away from the game.
bool Active();

// The title's time scale slows animation and the camera, but at a measured frame rate the simulation step ignores it,
// so its hook has to scale that too.
bool TimeFrozen();

void BeforeCameraUpdate(uint8_t* base, uint32_t camera);
void AfterCameraUpdate(uint8_t* base, uint32_t camera);
void OnGuestFrame();

}  // namespace crackdown::free_camera
