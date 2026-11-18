// What the Extras menu additions (extras.cpp) share with the rest of the game's side of the port.
#pragma once
#include <cstdint>
#include <string>

struct X86Context;

namespace extras {

// TGameManagerImplementation::MapSet has run: a level has begun. It's recorded as reached.
void LevelStarted(X86Context& ctx);

// GUI::TManager::LaunchMap(name): the menus start a level. Level Select swaps the first level's name for the one picked; the
// name to launch is returned, in guest memory.
uint32_t LaunchMap(X86Context& ctx, uint32_t name);


// The game's current map, lower case ("m03_cemetery_1"), or "" outside a level
std::string CurrentMap();

}  // namespace extras
