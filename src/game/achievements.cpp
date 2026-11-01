// What earns Buffy's achievements (listed in common/game_info.cpp, kept by the SDK's app/achievements.cpp). The original game
// has none of its own. docs/xdk-4432.md, "Hook points", says which of these have been seen firing in play.
#include <algorithm>
#include <cctype>
#include <string>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kGame = 0x7D0090;       // TGame::gGame
constexpr uint32_t kSlayerVtable = 0x67A0CC;  // ISlayer: IDarkSlayer has the same functions and isn't the player
constexpr uint32_t kHard = 3;              // TGameOptions +0x1C: 1 easy, 2 normal, 3 hard

// lower case: the same map comes as M03_Cemetery_1 and m03_cemetery_1
std::string CurrentMap() {
    const uint32_t game = kernel::R32(kGame);
    if (!game) return {};
    std::string map = kernel::ReadCString(kernel::R32(kernel::R32(game + 0x40) + 0x40), 80);
    std::transform(map.begin(), map.end(), map.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return map;
}

bool IsPlayer(uint32_t character) { return kernel::R32(character) == kSlayerVtable; }

// ---- chapters run between the Library's gang meetings. Dying is counted for good, so a chapter is clean when the count
// hasn't moved since the chapter began. chapter_start@<n> holds the count plus one, 0 being "not seen beginning", so a save
// loaded halfway through a chapter doesn't earn it.
uint64_t Deaths() { return ach::Count("deaths"); }

void BeginChapter(int chapter) {
    const std::string name = "chapter_start@" + std::to_string(chapter);
    const uint64_t now = ach::Count(name.c_str()), want = Deaths() + 1;
    if (want > now) ach::Add(name.c_str(), want - now);
}

void EndChapter(int chapter) {
    const std::string name = "chapter_start@" + std::to_string(chapter);
    if (ach::Count(name.c_str()) == Deaths() + 1) ach::Unlock("chapter_deathless");
    BeginChapter(chapter + 1);
}

// ---- leaving a level. Bosses are by the level: some never die in a way a death hook would see. Only IMapCompleted and
// IMapIndexer leave a level from play: quitting, the menus and a reload after dying don't come through here. IMapCompleted goes
// on along the game's level table. IMapIndexer can go to any numbered level of the mission, backwards too, so it only counts
// when it goes to the level that comes next.
struct LevelEnd {
    const char* map;
    const char* next;   // in the disc's level table
    const char* earns;  // may be null
    int ends_chapter;   // 0: none
};
constexpr LevelEnd kLevelEnds[] = {
    {"m00_spanishmission_3", "m01_highschool_1", "ch_training", 0},
    {"m01_highschool_4", "hub01", "boss_malik", 1},
    {"m02_bronze_4", "m03_cemetery_1", "boss_spike", 0},
    {"m03_cemetery_4", "hub02", "boss_scylla", 2},
    {"m05_sunken_4", "hub03", "boss_matereani", 3},
    {"m06_mansion_2", "hub04", "boss_scylla_snake", 4},
    {"m07_docks_5", "m07_docks_6", "boss_urd", 0},
    {"m07_docks_6", "hub05", nullptr, 5},
    {"m08_highschool_4", "hub06", "boss_malik_tiger", 6},
    {"m09_foundry_5", "hub07", "boss_angel", 7},
    {"m11_dreamrealm_3", "m11_dreamrealm_1", "boss_dark_buffy", 0},
    {"m11_dreamrealm_4", "m12_apse", "boss_dreamers", 0},
    {"m12_apse", "m12_bronze-ending", "boss_master", 8},
    {"g00_buffyhistory", "g00_buffyhistory0", "history", 0},  // the end of it asks for index 0, which goes back to the menu
};

// target: where IMapIndexer is going, null for IMapCompleted
void LeftLevel(const std::string* target) {
    const std::string map = CurrentMap();
    LOGI("achievements: left level %s for %s", map.c_str(), target ? target->c_str() : "the next level");
    for (const LevelEnd& end : kLevelEnds) {
        if (map != end.map || (target && *target != end.next)) continue;
        if (end.earns) ach::Unlock(end.earns);
        if (end.ends_chapter) EndChapter(end.ends_chapter);
    }
}

// as IMapIndexer::DoMapChange works it out: the map's name with its last digits replaced by the index, as wide as they were
std::string IndexerTarget(uint32_t trigger) {
    std::string map = CurrentMap();
    size_t digits = 0;
    while (!map.empty() && std::isdigit(static_cast<unsigned char>(map.back()))) {
        map.pop_back();
        digits++;
    }
    std::string index = std::to_string(int32_t(kernel::R32(trigger + 0x290)));
    if (index.size() < digits) index.insert(0, digits - index.size(), '0');
    return map + index;
}

// ---- secrets. Each level keeps the most ever found there, so reloading can't count one twice.
constexpr uint64_t kAllSecrets = 77;  // the sum of secretcount in the disc's level table

void SecretFound(X86Context& ctx) {
    // ISecretCounter::GetCount(long& found, long& total)
    const uint32_t sp = ctx.esp;
    ctx.esp -= 16;
    const uint32_t found_at = ctx.esp, total_at = ctx.esp + 4;
    kernel::W32(found_at, 0);
    kernel::W32(total_at, 0);
    kernel::CallGuest(ctx, 0x23C640, {found_at, total_at});
    const uint32_t found = kernel::R32(found_at), total = kernel::R32(total_at);
    ctx.esp = sp;

    const std::string name = "secrets@" + CurrentMap();
    const uint64_t best = ach::Count(name.c_str());
    LOGI("achievements: secret, %u of %u here (best %llu)", found, total, (unsigned long long)best);
    ach::Unlock("secret_1");
    if (total >= 3 && found >= total) ach::Unlock("secret_level");
    if (found <= best) return;
    ach::Add(name.c_str(), found - best);
    const uint64_t all = ach::Add("secrets", found - best);
    if (all >= 30) ach::Unlock("secret_30");
    if (all >= kAllSecrets) ach::Unlock("secret_all");
}

}  // namespace

// ISlayer::Event_KilledEnemy(ICharacter*): only kills credited to Buffy
X86_EXTERN_FUNC(__imp__sub_0020E1F0);
X86_FUNC(sub_0020E1F0) {
    if (IsPlayer(ctx.ecx)) {
        const uint64_t kills = ach::Add("kills", 1);
        ach::Unlock("kills_1");
        if (kills >= 100) ach::Unlock("kills_100");
        if (kills >= 250) ach::Unlock("kills_250");
        if (kills >= 500) ach::Unlock("kills_500");
    }
    __imp__sub_0020E1F0(ctx, base);
}

// ICharacterBtvS::DieAsPlayer
X86_EXTERN_FUNC(__imp__sub_00206010);
X86_FUNC(sub_00206010) {
    if (IsPlayer(ctx.ecx)) {
        ach::Add("deaths", 1);
        ach::Unlock("die_once");
    }
    __imp__sub_00206010(ctx, base);
}

// IMapCompleted::DoMapChange
X86_EXTERN_FUNC(__imp__sub_000EEB50);
X86_FUNC(sub_000EEB50) {
    LeftLevel(nullptr);
    __imp__sub_000EEB50(ctx, base);
}

// IMapIndexer::DoMapChange
X86_EXTERN_FUNC(__imp__sub_000EECD0);
X86_FUNC(sub_000EECD0) {
    const std::string target = IndexerTarget(ctx.ecx);
    LeftLevel(&target);
    __imp__sub_000EECD0(ctx, base);
}

// GUI::TManager::LaunchMap(char*): the menus starting a level. A new game begins the first chapter here.
X86_EXTERN_FUNC(__imp__sub_001EC330);
X86_FUNC(sub_001EC330) {
    const std::string map = kernel::ReadCString(kernel::R32(ctx.esp + 4), 80);
    LOGI("achievements: launching %s", map.c_str());
    if (map.size() >= 20 && (map[0] == 'M' || map[0] == 'm') && map.substr(1, 2) == "00" && map.back() == '1') BeginChapter(1);
    __imp__sub_001EC330(ctx, base);
}

// TGBuffyInterface::ActivateEndCredits: the end of the game. Credits from the Extras menu don't come here.
X86_EXTERN_FUNC(__imp__sub_0023EF50);
X86_FUNC(sub_0023EF50) {
    ach::Unlock("game_finished");
    const uint32_t game = kernel::R32(kGame);
    if (game && kernel::R32(kernel::R32(game + 0x20) + 0x1C) == kHard) ach::Unlock("game_hard");
    __imp__sub_0023EF50(ctx, base);
}

// ISecretCounter::Trigger: it adds one unless this activator has already been counted
X86_EXTERN_FUNC(__imp__sub_0023C7A0);
X86_FUNC(sub_0023C7A0) {
    const uint32_t counter = ctx.ecx, before = kernel::R32(counter + 0x274);
    __imp__sub_0023C7A0(ctx, base);
    if (kernel::R32(counter + 0x274) != before) SecretFound(ctx);
}
