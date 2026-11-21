#pragma once
// Loading the recompiled game code when it is a DLL beside the executable (XBE_GAME_CODE_DLL): cpu/dispatch.cpp, used by
// app/game_code.cpp
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace dispatch {
enum class LoadStatus { Ok, WrongExeName, NotLoaded, NoTables, Stale };
struct LoadResult {
    LoadStatus status;
    unsigned long error;     // the Windows error, for NotLoaded
    const char* dll_stamp;   // what the DLL says it was built for ("" if it says nothing), for Stale
    std::string detail = {}; // macOS: what the loader said (dlerror), or the symbol the dylib lacks (NoTables); it names the missing symbol of a Stale one
};
// exe_name: what this executable must be called (Buffy.exe), as the DLL's imports name it. stamp: what the DLL's
// X86GameCodeStamp must say (the executable's ABI and the game image: app/game_code.cpp makes it, tools/recomp writes the DLL's);
// a DLL that says otherwise, or nothing, is Stale. So is one Windows will not bind (ERROR_PROC_NOT_FOUND): it wants something
// this executable does not export, as a DLL made for another version of it would.
LoadResult Load(const std::filesystem::path& dll, const std::wstring& exe_name, const char* stamp);
}  // namespace dispatch

#if defined(XBE_GAME_CODE_DLL) && defined(__APPLE__)
namespace dispatch {
// macOS: the dylib at an absolute path, bound to this executable (dlopen RTLD_NOW|RTLD_LOCAL: a name it needs that the executable does
// not export fails here, saying which, and is Stale: the dylib was made for another version of the executable), its tables and
// stamp looked up by name, and the slots of the executable's X86_EXTERN_FUNC calls filled. `stamp` is what X86GameCodeStamp must say.
LoadResult Load(const std::filesystem::path& dylib, const char* stamp);
// what was loaded, for --check-game-code
struct Tables {
    size_t functions = 0, continuations = 0;  // entries of X86FuncMappings and X86Continuations
    size_t externs = 0, externs_filled = 0;   // the executable's X86_EXTERN_FUNC slots, and those Load found in the dylib
    uint32_t code_base = 0, code_end = 0, entry = 0;
};
Tables Describe();
}  // namespace dispatch
#endif
