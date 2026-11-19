#pragma once
#include <cstdint>

#include "cpu/x86_context.h"

// Mid-level checkpoints (rewind.cpp): rewind points are snapshots of the whole guest, kept in memory, which the death screen's
// Continue goes back to in place of reloading the level.
namespace snapback {
// [gameplay] mid_level_checkpoints in the settings (on by default), read at start, so a level takes it from the file as it begins
extern bool g_checkpoints;
// GameInfo::init, after the settings are read and before the game starts
void Init();
// whether anything here is on (the checkpoints, or a test trigger): when not, the hooks go straight to the game
bool Enabled();
// moves on at each restore: a host side cache of guest memory (a pointer kept from before) is stale when it differs
uint32_t Epoch();
// TGameManagerImplementation::Frame, from ExecuteGame's loop, on the main thread. depth is the native stack in use there (the
// same from every call of this). True when it did something that took the frame (a snapshot, a restore).
bool Frame(X86Context& ctx, uint64_t depth);
// TGameManagerImplementation::MapSet has run: a level has begun
void LevelStarted(X86Context& ctx);
// ICharacterBtvS::DieAsPlayer: the player died (for the time since the last rewind)
void PlayerDied();
// TGO::PlayerDeathReload(arg): true when the call is taken over (the next Frame goes back to a rewind point) and the caller returns
bool Death(X86Context& ctx, uint32_t arg);
// kernel::Relaunch is about to start the next instance (the level reloads, or the game goes to its menu): the points are dropped
void Relaunching();
}  // namespace snapback
