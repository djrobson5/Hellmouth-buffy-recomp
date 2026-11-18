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

// Whether the session being played reached a chapter from its start. A session's records show the levels it has reached, and
// the one for a level is written when the level before it ends, so a session made by Level Select at a later level lacks the
// records for the chapters before it. The first chapter is entered through the story's second level, the others through the
// hub (the Library's gang meeting) that opens them.
bool SessionPlayedChapter(X86Context& ctx, int chapter);

// Whether the session has entered every chapter. One record isn't enough: every session also has the first level's record, so
// Load Game can replay the first level of a Level Select game and write the second level's.
bool SessionPlayedFromStart(X86Context& ctx);

// The game's current map, lower case ("m03_cemetery_1"), or "" outside a level
std::string CurrentMap();

}  // namespace extras
