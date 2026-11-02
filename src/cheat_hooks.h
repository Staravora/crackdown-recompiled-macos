#pragma once

#include <cstdint>

namespace crackdown::cheats {

bool PlayerIsDriving(uint8_t* base);

// Once per frame on the game thread: applies the refills that turning a cheat on asks for.
void RunGameTick(uint8_t* base);

// Whether the last frame had a single-player session with a controlled player, so the cheats can act.
bool Available();

// The Character the local player controls, or 0.
uint32_t LocalPlayerCharacter(uint8_t* base);

}  // namespace crackdown::cheats
