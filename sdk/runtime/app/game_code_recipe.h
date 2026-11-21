#pragma once
// What this executable knows about its game code, as app/game_build.h takes it: the flags CMake wrote for the compile and link
// (game_code_flags.h, one place for them) and the image the recompiler wrote (image_symbols.h), made into the stamp a DLL for
// this executable and this game must say. Included by app/game_code.cpp, and by tools/first_start_check.cpp for the Mac check.
#include <string>

#include "app/game_build.h"
#include "game_code_flags.h"
#include "image_symbols.h"

#define XBE_RECIPE_STR2(x) #x
#define XBE_RECIPE_STR(x) XBE_RECIPE_STR2(x)

namespace gamecode {

// the ABI token the DLL is compiled with (abi<32 hex>)
inline const char* AbiToken() { return XBE_RECIPE_STR(XBE_GAME_CODE_ABI); }

// "xbe-game-code abi=<token> image=<sections hash> symbols=<hash>": what tools/recomp writes into the DLL's x86_func_mapping.cpp as
// X86GameCodeStamp (the token from the compile's -DXBE_GAME_CODE_ABI; the rest from the image) and what dispatch::Load compares
// (one string in the executable's bytes, which tools/check_game_code_dll.sh and tools/sim_first_start.sh read)
inline constexpr char kStampText[] = "xbe-game-code abi=" XBE_RECIPE_STR(XBE_GAME_CODE_ABI) " " IMAGE_CODE_STAMP;
inline std::string Stamp() { return kStampText; }

// the flags are ASCII, written as wide strings by CMake
inline std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; *text; text++) out.push_back(char(*text));
    return out;
}

inline gamebuild::Recipe Recipe(size_t slots) {
    gamebuild::Recipe r;
    r.library = IMAGE_LIBRARY;
    r.stamp = Stamp();
    r.image_stamp = IMAGE_CODE_STAMP;
    r.compile_flags = Narrow(XBE_GAME_CODE_COMPILE_FLAGS);
    r.link_flags = Narrow(XBE_GAME_CODE_LINK_FLAGS);
    r.slots = slots;
    return r;
}

}  // namespace gamecode
