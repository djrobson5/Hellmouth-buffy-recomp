#pragma once
// Loading the recompiled game code when it is a DLL beside the executable (XBE_GAME_CODE_DLL): cpu/dispatch.cpp, used by
// app/game_code.cpp
#include <filesystem>
#include <string>

namespace dispatch {
enum class LoadStatus { Ok, WrongExeName, NotLoaded, NoTables };
struct LoadResult {
    LoadStatus status;
    unsigned long error;  // the Windows error, for NotLoaded
};
// exe_name: what this executable must be called (Buffy.exe), as the DLL's imports name it
LoadResult Load(const std::filesystem::path& dll, const std::wstring& exe_name);
}  // namespace dispatch
