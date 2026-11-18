// The game's level table and the levels a player has reached: logic only, with no game memory in it, so it can be tested on
// its own. The callers read the table from the game (TMapSequencer) and the saves from disk.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace levels {

inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// One entry of the disc's mission list (gameinfo\missionlist.txt), as TMapSequencer holds it: the map's name, and whether
// it's one of the specials (the arenas and Buffy History 101) rather than a level of the story.
struct Entry {
    std::string map;
    bool special = false;
};

// A session file (.ses, 131,748 bytes) is a 0x24 byte header (version 0x00010013, 64 slots, the time played, the difficulty
// and a signature), 64 records of 0x3A bytes, and 64 blobs of 0x7D0. A record is a visited byte, a variant byte and the map's
// name, 0 ended in 56 bytes. The game keeps the same header and records in memory as its TSession.
constexpr uint32_t kSesVersion = 0x00010013;
constexpr size_t kSesHeader = 0x24, kSesRecord = 0x3A, kSesSlots = 64, kSesName = kSesRecord - 2;

// the maps (lower case) of the records marked visited, in the file's order; none when the data isn't a session
inline std::vector<std::string> VisitedMaps(const uint8_t* data, size_t size) {
    std::vector<std::string> maps;
    uint32_t version = 0, slots = 0;
    if (size < kSesHeader + kSesSlots * kSesRecord) return maps;
    std::memcpy(&version, data, 4);
    std::memcpy(&slots, data + 4, 4);
    if (version != kSesVersion || slots != kSesSlots) return maps;
    for (size_t i = 0; i < kSesSlots; i++) {
        const uint8_t* rec = data + kSesHeader + i * kSesRecord;
        if (!rec[0]) continue;
        const char* name = reinterpret_cast<const char*>(rec + 2);
        maps.push_back(Lower(std::string(name, strnlen(name, kSesName))));
    }
    return maps;
}

}  // namespace levels
