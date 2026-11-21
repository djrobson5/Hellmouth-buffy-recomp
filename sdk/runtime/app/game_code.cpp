// The recompiled game code when it is not in the executable: <exe>.dll beside it, which the executable delay loads, so it
// must be loaded before the first call into it. Without XBE_GAME_CODE_DLL the code is linked in and there is nothing to do.
#include "app/app.h"

#ifdef XBE_GAME_CODE_DLL
#include "core/log.h"
#include "image_symbols.h"

namespace dispatch {
bool Load(const std::filesystem::path& dll);  // cpu/dispatch.cpp
}
#endif

namespace app {

#ifdef XBE_GAME_CODE_DLL
bool PrepareGameCode(const GameFiles&) {
    std::filesystem::path dll = ExeDir() / IMAGE_LIBRARY;
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec))
        Fail(kExitNoGameCode, L"The game's code, " + dll.filename().wstring() + L", is missing from " + dll.parent_path().wstring() + L".");
    if (!dispatch::Load(dll))
        Fail(kExitNoGameCode, dll.filename().wstring() + L" would not load: it may be damaged, or made for another version of the game.");
    LOGR("Game code: %s", dll.string().c_str());
    return true;
}
#else
bool PrepareGameCode(const GameFiles&) { return true; }
#endif

}  // namespace app
