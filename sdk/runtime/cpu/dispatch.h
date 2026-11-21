#pragma once
// Loading the recompiled game code when it is a DLL beside the executable (XBE_GAME_CODE_DLL): cpu/dispatch.cpp, used by
// app/game_code.cpp
#include <filesystem>
#include <string>

namespace dispatch {
enum class LoadStatus { Ok, WrongExeName, NotLoaded, NoTables, Stale };
struct LoadResult {
    LoadStatus status;
    unsigned long error;     // the Windows error, for NotLoaded
    const char* dll_stamp;   // what the DLL says it was built for ("" if it says nothing), for Stale
};
// exe_name: what this executable must be called (Buffy.exe), as the DLL's imports name it. stamp: what the DLL's
// X86GameCodeStamp must say (the executable's ABI and the game image: app/game_code.cpp makes it, tools/recomp writes the DLL's);
// a DLL that says otherwise, or nothing, is Stale. So is one Windows will not bind (ERROR_PROC_NOT_FOUND): it wants something
// this executable does not export, as a DLL made for another version of it would.
LoadResult Load(const std::filesystem::path& dll, const std::wstring& exe_name, const char* stamp);
}  // namespace dispatch
