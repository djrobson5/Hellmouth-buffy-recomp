#pragma once
#include "cpu/x86_context.h"

// A mid-level rewind inside the process (a spike; off unless HELLMOUTH_REWIND_TEST is set). Snapshots of the whole guest are
// taken, and put back, at the top of TGameManagerImplementation::Frame as ExecuteGame's loop calls it.
namespace snapback {
bool Enabled();
// at that place, on the main thread; depth is the native stack in use there (the same from every call of this)
void Frame(X86Context& ctx, uint64_t depth);
}  // namespace snapback
