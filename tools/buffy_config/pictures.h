#pragma once
// The achievement pictures and the game's icon, made from two files of the player's own disc (pictures.cpp).
#include <filesystem>
#include <string>

namespace pictures {

// Reads interfc/levelThumbnails.mtx and media/launch_buffy.xpr (the files, already taken from the disc) and writes, in `out`:
//   achievements/<id>.png   square crops of the level thumbnails and the launch screen, the pictures tools/make_achievement_icons.py makes
//   Buffy.png               the icon, 256 x 256
//   Buffy.ico               the same at 256 (PNG), 48, 32 and 16 (DIB)
// Returns false, and says why in `why`, when a file is not what it should be or cannot be written.
bool Make(const std::filesystem::path& thumbnails, const std::filesystem::path& launch, const std::filesystem::path& out, std::string& why);

}  // namespace pictures
