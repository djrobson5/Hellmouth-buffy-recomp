// What the Extras menu additions (extras.cpp) share with the rest of the game's side of the port.
#pragma once
#include <cstdint>
#include <string>

struct X86Context;

namespace extras {

// TGameManagerImplementation::MapSet has run: a level has begun. It's recorded as reached.
void LevelStarted(X86Context& ctx);



// The game's current map, lower case ("m03_cemetery_1"), or "" outside a level
std::string CurrentMap();

}  // namespace extras
