// The recompiled game code when it is not in the executable: <exe>.dll beside it, which the executable delay loads, so it
// must be loaded before the first call into it. Without XBE_GAME_CODE_DLL the code is linked in and there is nothing to do.
#include "app/app.h"

#ifdef XBE_GAME_CODE_DLL
#include <cstring>

#include "core/log.h"
#include "cpu/dispatch.h"
#include "image_symbols.h"
#endif

namespace app {

#ifdef XBE_GAME_CODE_DLL
bool PrepareGameCode(const GameFiles&) {
    std::filesystem::path dll = ExeDir() / IMAGE_LIBRARY;
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec))
        Fail(kExitNoGameCode, L"The game's code, " + dll.filename().wstring() + L", is missing from " + dll.parent_path().wstring() + L".");
    // the executable's name for this image (the DLL's imports name it)
    std::wstring exe_name = std::filesystem::path(IMAGE_LIBRARY).stem().wstring() + L".exe";
    for (auto& [xbe, exe] : kGame.images)
        if (_stricmp(xbe, IMAGE_XBE) == 0) exe_name = std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    dispatch::LoadResult r = dispatch::Load(dll, exe_name);
    switch (r.status) {
        case dispatch::LoadStatus::Ok:
            break;
        case dispatch::LoadStatus::WrongExeName:
            Fail(kExitNoGameCode, L"This program must be named " + exe_name + L", because the game's code, " + dll.filename().wstring() +
                                      L", is built to work with a program of that name. Rename it back.");
        case dispatch::LoadStatus::NotLoaded:
            Fail(kExitNoGameCode, dll.filename().wstring() + L" would not load (Windows error " + std::to_wstring(r.error) +
                                      L"): it may be damaged, or made for another version of the game.");
        case dispatch::LoadStatus::NoTables:
            Fail(kExitNoGameCode, dll.filename().wstring() + L" is not the game's code: it lacks the tables that describe it.");
    }
    LOGR("Game code: %s", dll.string().c_str());
    return true;
}
#else
bool PrepareGameCode(const GameFiles&) { return true; }
#endif

}  // namespace app
