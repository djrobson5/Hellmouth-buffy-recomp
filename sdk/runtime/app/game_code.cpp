// The recompiled game code when it is not in the executable: <exe>.dll beside it, which the executable delay loads, so it
// must be loaded before the first call into it. Without XBE_GAME_CODE_DLL the code is linked in and there is nothing to do.
#include "app/app.h"

#ifdef XBE_GAME_CODE_DLL
#include <cstring>

#include "core/log.h"
#include "cpu/dispatch.h"
#include "game_code_flags.h"
#include "image_symbols.h"
#endif

namespace app {

#ifdef XBE_GAME_CODE_DLL
namespace {
#define XBE_STAMP_STR2(x) #x
#define XBE_STAMP_STR(x) XBE_STAMP_STR2(x)
// What a DLL for this executable and this game says in its X86GameCodeStamp, which tools/recomp writes into its x86_func_mapping.cpp:
// the ABI token (game_code_flags.h, from CMake: hashes of the headers the code is compiled against, the compile and link flags, the
// recompiler, and the sources that define the functions this executable replaces) and the image (image_symbols.h: the sections hash
// and a hash of the names and addresses). The DLL gets the ABI token from -DXBE_GAME_CODE_ABI in its compile.
const char kStamp[] = "xbe-game-code abi=" XBE_STAMP_STR(XBE_GAME_CODE_ABI) " " IMAGE_CODE_STAMP;
}  // namespace

bool PrepareGameCode(const GameFiles&) {
    std::filesystem::path dll = ExeDir() / IMAGE_LIBRARY;
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec))
        Fail(kExitNoGameCode, L"The game's code, " + dll.filename().wstring() + L", is missing from " + dll.parent_path().wstring() + L".");
    // the executable's name for this image (the DLL's imports name it)
    std::wstring exe_name = std::filesystem::path(IMAGE_LIBRARY).stem().wstring() + L".exe";
    for (auto& [xbe, exe] : kGame.images)
        if (_stricmp(xbe, IMAGE_XBE) == 0) exe_name = std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    dispatch::LoadResult r = dispatch::Load(dll, exe_name, kStamp);
    switch (r.status) {
        case dispatch::LoadStatus::Ok:
            break;
        case dispatch::LoadStatus::WrongExeName:
            Fail(kExitNoGameCode, L"This program must be named " + exe_name + L", because the game's code, " + dll.filename().wstring() +
                                      L", is built to work with a program of that name. Rename it back.");
        case dispatch::LoadStatus::NotLoaded:
            Fail(kExitNoGameCode, dll.filename().wstring() + L" would not load (Windows error " + std::to_wstring(r.error) +
                                      L"): it may be damaged, or made for another version of the game.");
        case dispatch::LoadStatus::Stale:
            LOGE("Game code: %s says \"%s\"; this executable needs \"%s\"", dll.string().c_str(), r.dll_stamp, kStamp);
            Fail(kExitStaleGameCode, dll.filename().wstring() + L" was built for another version of this program or of the game, and has to be built again.");
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
