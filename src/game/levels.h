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

// The levels Level Select lists, as indexes into the table, in the table's order: the story's levels (not the specials) that
// have been reached. The first level is always there.
inline std::vector<size_t> Reachable(const std::vector<Entry>& table, const std::function<bool(const std::string&)>& reached) {
    std::vector<size_t> list;
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i].special) continue;
        if (list.empty() || reached(Lower(table[i].map))) list.push_back(i);
    }
    return list;
}

// Chapters run between the Library's gang meetings (the HUBnn entries). A hub opens the chapter after it, so chapter 1 is
// everything before HUB01, chapter 2 is HUB01 up to HUB02, and so on.
inline bool IsHub(const Entry& e) { return Lower(e.map).rfind("hub", 0) == 0; }

inline int Chapter(const std::vector<Entry>& table, size_t index) {
    int chapter = 1;
    for (size_t i = 0; i <= index && i < table.size(); i++)
        if (!table[i].special && IsHub(table[i])) chapter++;
    return chapter;
}

// The table entry a chapter is entered through: the hub before it, or for chapter 1 the story's second level (the first
// one's record is there from the start, so it can't show the session was played from the beginning). table.size() when
// there's no such entry, which no session has a record of, so a check on it fails.
inline size_t ChapterGate(const std::vector<Entry>& table, int chapter) {
    if (chapter <= 1) return table.size() > 1 ? 1 : table.size();
    int hubs = 0;
    for (size_t i = 0; i < table.size(); i++)
        if (!table[i].special && IsHub(table[i]) && ++hubs == chapter - 1) return i;
    return table.size();
}

// The number of chapters: one more than the hubs
inline int Chapters(const std::vector<Entry>& table) {
    int hubs = 0;
    for (const Entry& e : table)
        if (!e.special && IsHub(e)) hubs++;
    return hubs + 1;
}

}  // namespace levels
