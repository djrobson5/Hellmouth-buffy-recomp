// Extras, the front end's TMenuCredits (menu 0x0B), drawn by the game's own menu code: Arena 1 to 4 are always listed,
// four Hard versions are added, and a Level Select entry (see docs/xdk-4432.md, "Extras"). The new entries are
// TTextButtons built the way the menu file builds them: a record in the game's binary menu format is put in guest memory,
// the menu's stream is pointed at it, and the game's own constructor, PersistRead and AddControl do the rest, so the
// buttons look and navigate like the stock ones.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "game/extras.h"
#include "game/levels.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kGame = 0x7D0090;               // TGame::gGame
constexpr uint32_t kCreditsVtable = 0x68CCC4;      // GUI::TMenuCredits
constexpr uint32_t kCtrlDefs = 0x7E6B90;           // GUI::g_lpCtrlDef: the control classes, by name
constexpr uint32_t kOperatorNew = 0x2BC1A0;        // the game's operator new
constexpr uint32_t kGetMapSequencer = 0x936B0;     // TGO::Interface_GetMapSequencer: the mission list
constexpr uint32_t kGetManager = 0x1EB7E0;         // GUI::GetManager
constexpr uint32_t kGetBuffyManager = 0x217F30;    // GUI::GetBuffyManager
constexpr uint32_t kSessionManager = 0x7F1924;     // TSessionManager::s_pInstance
constexpr uint32_t kFadeMode = 0x7F041C;           // GUI::TBuffyMenu::s_fadeMode
constexpr uint32_t kIdArena1 = 0x101;              // IDC_CREDITS_ARENA_1 .. 4
constexpr uint32_t kIdHard1 = 0x105;               // the new Arena (Hard) buttons, 0x105 .. 0x108
constexpr uint32_t kIdLevelSelect = 0x82;          // the new Level Select button
constexpr uint32_t kRunOptionsCheats = 0x34;       // TScriptRunOptions: the cheats console variable, a bit mask
constexpr uint32_t kCheatHard = 0x08;              // cheat 3: arenas load their "d" versions

// virtual slots (guest vtables, 4 bytes each)
constexpr uint32_t kCtrlPersistRead = 0x0C;
constexpr uint32_t kCtrlGetID = 0x30;
constexpr uint32_t kCtrlSetOffset = 0x50;          // SetOffset(x, y)
constexpr uint32_t kCtrlSetState = 0x60;           // 0 shown, 3 hidden
constexpr uint32_t kCanvasAddControl = 0x64;
constexpr uint32_t kButtonSetText = 0x6C;          // SetText(char*), which copies
constexpr uint32_t kMenuFindControl = 0x84;

uint32_t Virt(X86Context& ctx, uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args) {
    if (!obj) return 0;  // a control the menu file doesn't have
    const uint32_t saved = ctx.ecx;
    ctx.ecx = obj;
    const uint32_t r = kernel::CallGuest(ctx, kernel::R32(kernel::R32(obj) + slot), args);
    ctx.ecx = saved;
    return r;
}

uint32_t GuestNew(X86Context& ctx, uint32_t size) { return kernel::CallGuest(ctx, kOperatorNew, {size}); }

// A string in guest memory for a call that copies it (SetText and the dialog keep their own copies): one buffer, reused, so
// opening a menu again and again doesn't take more memory each time.
uint32_t ScratchString(X86Context& ctx, const std::string& text) {
    static uint32_t buf = 0, cap = 0;
    if (text.size() + 1 > cap) {
        cap = std::max<uint32_t>(256, uint32_t(text.size() + 1));
        buf = GuestNew(ctx, cap);
        if (!buf) cap = 0;
    }
    if (!buf) return 0;
    std::memcpy(kernel::HostPtr(buf), text.c_str(), text.size() + 1);
    return buf;
}

uint8_t R8(uint32_t va) { return *static_cast<uint8_t*>(kernel::HostPtr(va)); }

uint32_t RunOptions() { return kernel::R32(kernel::R32(kGame) + 0x24); }

// ---- the menu file's record of a TTextButton: its class name, the flag and the long that PersistRead skips, the name, the
// id, the tab order and the number of properties, then each property as its name, a type word and its value. Offset and
// Extent are two longs, Visible a byte, Text and the fonts one long (a string and font resource id), and the two alignments
// a type word and a long of zero.
struct Record {
    std::vector<uint8_t> b;
    void Str(const char* s) { b.insert(b.end(), s, s + std::strlen(s) + 1); }
    void U32(uint32_t v) {
        for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i)));
    }
    void Prop(const char* name, uint32_t type) {
        Str(name);
        U32(type);
    }
};

Record TextButton(const char* name, uint32_t id, uint32_t tab, int x, int y, uint32_t text) {
    Record r;
    r.Str("TTextButton");
    r.b.push_back(0);
    r.U32(0);
    r.Str(name);
    r.U32(id);
    r.U32(tab);
    r.U32(8);
    r.Prop("Offset", 4);
    r.U32(uint32_t(x));
    r.U32(uint32_t(y));
    r.Prop("Extent", 4);
    r.U32(100);
    r.U32(50);
    r.Prop("Visible", 2);
    r.b.push_back(1);
    r.Prop("Text", 3);
    r.U32(text);
    r.Prop("Normal Font", 3);
    r.U32(2);
    r.Prop("Active Font", 3);
    r.U32(1);
    r.Prop("Horizontal Alignment (0L/1C/2R)", 0);
    r.U32(0);
    r.Prop("Vertical Alignment (0T/1C/2B)", 0);
    r.U32(0);
    return r;
}

// the game's class table: a linked list whose nodes hold the class's id (+4), its name (+8), the next node (+0xC) and its
// constructor (+0x14)
uint32_t FindControlClass(const char* name) {
    for (uint32_t def = kernel::R32(kCtrlDefs); def; def = kernel::R32(def + 0xC))
        if (kernel::ReadCString(kernel::R32(def + 8), 64) == name) return def;
    return 0;
}

// Builds one TTextButton in a menu from a record, as TMenu::PersistRead builds each control of the file.
uint32_t AddButton(X86Context& ctx, uint32_t menu, uint32_t stream, const Record& r, const char* text) {
    const uint32_t def = FindControlClass("TTextButton");
    if (!def) {
        LOGE("extras: no TTextButton class");
        return 0;
    }
    const uint32_t buf = GuestNew(ctx, uint32_t(r.b.size() + 64));
    if (!buf) return 0;
    std::memcpy(kernel::HostPtr(buf), r.b.data(), r.b.size());
    kernel::W32(stream + 4, uint32_t(r.b.size()));  // AssignBuffer: size, start, position
    kernel::W32(stream + 8, buf);
    kernel::W32(stream + 0xC, buf);

    const uint32_t sp = ctx.esp;
    ctx.esp -= 16;
    const uint32_t out = ctx.esp;
    kernel::W32(out, 0);
    kernel::CallGuest(ctx, kernel::R32(def + 0x14), {out, menu, kernel::R32(def + 4)});
    const uint32_t ctrl = kernel::R32(out);
    ctx.esp = sp;
    if (!ctrl) return 0;
    Virt(ctx, ctrl, kCtrlPersistRead, {stream});
    Virt(ctx, menu, kCanvasAddControl, {ctrl});
    if (text)
        if (const uint32_t s = ScratchString(ctx, text)) Virt(ctx, ctrl, kButtonSetText, {s});
    return ctrl;
}

// Where each entry sits: one column, in the order Up and Down walk it. The menu draws every entry (the unfocused ones as
// lines, which the headless reference renderer leaves out), so eleven need closer spacing than the file's six, whose own
// places run from 119 to 360 with 43 between the arenas. These run from 96 to 416, 32 apart, at the file's x.
struct Slot {
    uint32_t id;
    int x, y;
};
constexpr int kTop = 96, kStep = 32;
constexpr Slot kLayout[] = {
    {0x80, 110, kTop + 0 * kStep},  // Buffy History 101
    {0x81, 111, kTop + 1 * kStep},  // Credits
    {kIdLevelSelect, 113, kTop + 2 * kStep},
    {kIdArena1 + 0, 113, kTop + 3 * kStep}, {kIdArena1 + 1, 113, kTop + 4 * kStep},
    {kIdArena1 + 2, 113, kTop + 5 * kStep}, {kIdArena1 + 3, 113, kTop + 6 * kStep},
    {kIdHard1 + 0, 113, kTop + 7 * kStep},  {kIdHard1 + 1, 113, kTop + 8 * kStep},
    {kIdHard1 + 2, 113, kTop + 9 * kStep},  {kIdHard1 + 3, 113, kTop + 10 * kStep},
};

// the menu's controls are a linked list (the list at +0x2C, its first node at +0x30; a node is the control, the next
// node and the previous one), and Up and Down walk it
void MoveAfter(uint32_t menu, uint32_t ctrl, uint32_t after) {
    std::vector<uint32_t> nodes;
    for (uint32_t n = kernel::R32(menu + 0x30); n; n = kernel::R32(n + 4)) nodes.push_back(n);
    std::vector<uint32_t> ctrls;
    for (uint32_t n : nodes) ctrls.push_back(kernel::R32(n));
    auto from = std::find(ctrls.begin(), ctrls.end(), ctrl);
    auto to = std::find(ctrls.begin(), ctrls.end(), after);
    if (from == ctrls.end() || to == ctrls.end()) return;
    ctrls.erase(from);
    to = std::find(ctrls.begin(), ctrls.end(), after);
    ctrls.insert(to + 1, ctrl);
    for (size_t i = 0; i < nodes.size(); i++) kernel::W32(nodes[i], ctrls[i]);
}

void ExtendMenu(X86Context& ctx, uint32_t menu, uint32_t stream) {
    const uint32_t size = kernel::R32(stream + 4), start = kernel::R32(stream + 8), pos = kernel::R32(stream + 0xC);
    auto find = [&](uint32_t id) { return Virt(ctx, menu, kMenuFindControl, {id}); };

    uint32_t tab = 11;
    const uint32_t level =
        AddButton(ctx, menu, stream, TextButton("IDC_CREDITS_LEVEL_SELECT", kIdLevelSelect, tab++, 0, 0, 90), "Level Select");
    for (uint32_t i = 0; i < 4; i++) {
        const std::string name = "IDC_CREDITS_ARENA_" + std::to_string(i + 1) + "_HARD";
        const std::string text = "Arena " + std::to_string(i + 1) + " (Hard)";
        AddButton(ctx, menu, stream, TextButton(name.c_str(), kIdHard1 + i, tab++, 0, 0, 90), text.c_str());
    }
    kernel::W32(stream + 4, size);  // the file's stream as it was
    kernel::W32(stream + 8, start);
    kernel::W32(stream + 0xC, pos);

    // Level Select comes before the arenas in the order Up and Down walk
    const uint32_t credits = find(0x81);
    if (level && credits) MoveAfter(menu, level, credits);

    for (const Slot& s : kLayout)
        if (const uint32_t c = find(s.id)) Virt(ctx, c, kCtrlSetOffset, {uint32_t(s.x), uint32_t(s.y)});
    // the file hides the arenas until a cheat code unlocks them
    for (uint32_t i = 0; i < 4; i++)
        if (const uint32_t c = find(kIdArena1 + i)) Virt(ctx, c, kCtrlSetState, {0});
    LOGI("extras: the Extras menu has Level Select and the Hard arenas");
}

// ---- the levels
// The mission list as TMapSequencer holds it: the count at +4, the array at +8, and 0x2C bytes to an entry: the map's name
// (a TString, whose first field is the text) at +0, the level's name for the menus at +0x10, the special flag at +0x28.
std::vector<levels::Entry> Table(X86Context& ctx) {
    std::vector<levels::Entry> table;
    const uint32_t seq = kernel::CallGuest(ctx, kGetMapSequencer, {});
    if (!seq) return table;
    const uint32_t count = kernel::R32(seq + 4), array = kernel::R32(seq + 8);
    for (uint32_t i = 0; i < count && array; i++) {
        const uint32_t e = array + i * 0x2C;
        table.push_back({kernel::ReadCString(kernel::R32(e), 64), R8(e + 0x28) != 0});
    }
    return table;
}

std::string ReachedKey(const std::string& lower_map) { return "reached@" + lower_map; }

void NoteReached(const std::string& lower_map) {
    const std::string key = ReachedKey(lower_map);
    if (!ach::Count(key.c_str())) ach::Add(key.c_str(), 1);
}

// The levels the player's saves have reached, from the saves themselves: every record of a session file marked visited. Done
// each time Level Select opens, so a save copied in later counts too.
void SeedFromSaves() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path udata = app::SavesDir() / "E" / "UDATA" / "45410012";
    if (!fs::is_directory(udata, ec)) return;
    size_t files = 0, found = 0;
    for (fs::recursive_directory_iterator it(udata, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ses") continue;
        std::ifstream f(it->path(), std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        files++;
        for (const std::string& map : levels::VisitedMaps(data.data(), data.size())) {
            if (!ach::Count(ReachedKey(map).c_str())) found++;
            NoteReached(map);
        }
    }
    LOGI("extras: %zu saved games read, %zu levels newly reached", files, found);
}

// once, the first time a level begins with this code: the saves made before it, whose levels nothing else recorded
void EnsureSeeded() {
    if (ach::Count("reached_seeded")) return;
    SeedFromSaves();
    ach::Add("reached_seeded", 1);
}

// ---- Level Select
// Extras opens the front end's Load Game menu (TMenuLoadSession, id 5) in a mode that skips its list of saved games and goes
// straight to its list of checkpoints, filled with the levels reached, earliest first. A pick goes on through the game's own
// New Game flow, the Start Game confirmation (menu 12) and then the difficulty (menu 13), and LaunchMap swaps the new game's
// first level for the one picked. The mode is on from Extras opening menu 5 until any change to a menu other than 5, 12 and 13.
constexpr uint32_t kMenuLoadSession = 5, kMenuStartGame = 12, kMenuDifficulty = 13;
constexpr uint32_t kTitleControl = 0x2C, kHeadingControl = 0x2E, kCaptionControl = 0xFC;  // "Load Game", "Checkpoint", "Select Checkpoint"
constexpr uint32_t kCheckpointList = 0x46, kSessionList = 0x44, kSessionBar = 0x45, kCheckpointBar = 0x47;
constexpr uint32_t kSessionLabel = 0xFB, kDeleteIcons[2] = {0xF1, 0xF2};
constexpr uint32_t kImageLoadGame = 54, kImageExtras = 53;  // the pictures of the two titles, as the menu file names them
constexpr uint32_t kCommandA = 0x27, kCommandB = 0x29, kCommandX = 0x28, kCommandLeft = 0x23, kCommandRight = 0x24;

// virtual slots
constexpr uint32_t kManagerSetMenu = 0x38, kManagerGetResource = 0x4C;
constexpr uint32_t kMenuGotoControl = 0x70;
constexpr uint32_t kListGetCurSel = 0x88, kListAddString = 0x8C, kListReset = 0x98;
constexpr uint32_t kTextGetText = 0x70;
constexpr uint32_t kDialogShow = 0x2D0;

struct {
    bool armed = false;               // Extras has asked for menu 5
    bool active = false;              // menu 5 and what follows it are Level Select's
    std::vector<size_t> list;         // the table's indexes, in the order the list shows them
    uint32_t array = 0;               // the table's array in guest memory, 0x2C bytes to an entry
    std::string pending;              // the map picked, as the table spells it
    bool title_swapped = false;       // the menu's title and captions are ours
    std::string stock_heading, stock_caption;
} g_ls;

uint32_t This(X86Context& ctx, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    const uint32_t saved = ctx.ecx;
    ctx.ecx = self;
    const uint32_t r = kernel::CallGuest(ctx, fn, args);
    ctx.ecx = saved;
    return r;
}

uint32_t Manager(X86Context& ctx) { return kernel::CallGuest(ctx, kGetManager, {}); }

void SetMenu(X86Context& ctx, uint32_t id, bool back) { Virt(ctx, Manager(ctx), kManagerSetMenu, {id, back ? 1u : 0u}); }

// TSound::Play on one of the Buffy manager's sounds: the game's own click (0x198), refusal (0x2B8) and so on
void PlaySound(X86Context& ctx, uint32_t which) {
    const uint32_t buffy = kernel::CallGuest(ctx, kGetBuffyManager, {});
    This(ctx, 0xB65A0, buffy + which, {1, 1, 0x42C80000, 0x3F800000, 0});
}

// A dialog the menu shows over itself, closed by A (the menu's flag at +0x4B, which its OnCommand checks first)
void ShowDialog(X86Context& ctx, uint32_t menu, const std::string& text) {
    const uint32_t s = ScratchString(ctx, text);
    if (!s) return;
    const uint32_t buffy = kernel::CallGuest(ctx, kGetBuffyManager, {});
    *static_cast<uint8_t*>(kernel::HostPtr(menu + 0x4B)) = 1;
    PlaySound(ctx, 0x2B8);
    Virt(ctx, kernel::R32(buffy + 0x70), kDialogShow, {s, 0, 0, 1, 1, 0});
}

// The checks the main menu's Start Game button makes before it goes to the confirmation (TMenuMain::OnMessage): the space
// on the disk is always 50,000 blocks or more here, so only the two about how many saved games there are matter. The game's
// own words come back, or "" when a new game can start.
std::string NewGameBlocked(X86Context& ctx) {
    const uint32_t sm = kernel::R32(kSessionManager);
    if (!(This(ctx, 0x237AA0, sm, {}) & 0xFF)) return kernel::ReadCString(0x68D778, 200);  // TSessionManager::QueryForAvailableSession
    const uint32_t sp = ctx.esp;
    ctx.esp -= 64;
    const uint32_t array = ctx.esp;
    std::memset(kernel::HostPtr(array), 0, 32);
    This(ctx, 0x238660, sm, {array, 0});  // TSessionManager::QuerySessions
    const uint32_t count = kernel::R32(array + 4);
    This(ctx, 0x64D90, array, {});        // the array's destructor
    ctx.esp = sp;
    if (count < 10) return {};
    char text[256];
    std::snprintf(text, sizeof text, "%s", kernel::ReadCString(0x68D6E8, 200).c_str());
    const std::string format = text;
    std::snprintf(text, sizeof text, format.c_str(), 10);  // "... only allows %i saved games ..."
    return text;
}

void SetImage(X86Context& ctx, uint32_t control, uint32_t image) {
    const uint32_t resource = Virt(ctx, kernel::R32(0x7E6BD0), kManagerGetResource, {1, image});  // GUI::g_lpManager
    if (control && resource) kernel::W32(control + 0x28, resource);
}

void SetText(X86Context& ctx, uint32_t control, const std::string& text) {
    if (const uint32_t s = ScratchString(ctx, text)) Virt(ctx, control, kButtonSetText, {s});
}

// After TMenuLoadSession::OnLoad has listed the saved games: in Level Select, the checkpoint list as the stock menu shows it
// once a game is chosen (the replay of TMenuLoadSession::OnCommand's switch at 0x221744), with the reached levels in it.
void ShowLevelSelect(X86Context& ctx, uint32_t menu) {
    auto find = [&](uint32_t id) { return Virt(ctx, menu, kMenuFindControl, {id}); };
    const uint32_t title = find(kTitleControl), heading = find(kHeadingControl), caption = find(kCaptionControl);
    if (!g_ls.title_swapped && g_ls.active) {
        g_ls.stock_heading = kernel::ReadCString(Virt(ctx, heading, kTextGetText, {}), 80);
        g_ls.stock_caption = kernel::ReadCString(Virt(ctx, caption, kTextGetText, {}), 80);
    }
    if (!g_ls.active) {
        if (g_ls.title_swapped) {
            SetImage(ctx, title, kImageLoadGame);
            SetText(ctx, heading, g_ls.stock_heading);
            SetText(ctx, caption, g_ls.stock_caption);
            // Level Select hid the list of saved games' scroll bar, which the stock OnLoad doesn't show again (the list itself
            // comes back as it takes the focus)
            Virt(ctx, find(kSessionBar), kCtrlSetState, {0});
            g_ls.title_swapped = false;
        }
        return;
    }
    SetImage(ctx, title, kImageExtras);
    SetText(ctx, heading, "Level");
    SetText(ctx, caption, "Select Level");
    g_ls.title_swapped = true;

    const std::vector<levels::Entry> table = Table(ctx);
    const uint32_t seq = kernel::CallGuest(ctx, kGetMapSequencer, {});
    g_ls.array = seq ? kernel::R32(seq + 8) : 0;
    if (!g_ls.array || table.empty()) {
        LOGW("extras: Level Select found no mission list");
        g_ls.list.clear();
        return;
    }
    SeedFromSaves();
    g_ls.list = levels::Reachable(table, [](const std::string& map) { return ach::Count(ReachedKey(map).c_str()) > 0; });
    const uint32_t list = find(kCheckpointList);
    Virt(ctx, list, kListReset, {});
    for (size_t index : g_ls.list) Virt(ctx, list, kListAddString, {kernel::R32(g_ls.array + uint32_t(index) * 0x2C + 0x10)});
    kernel::W32(menu + 0x58, 0xFFFFFFFF);  // the thumbnail follows the selection from scratch

    Virt(ctx, find(kSessionLabel), kCtrlSetState, {3});
    Virt(ctx, caption, kCtrlSetState, {0});
    Virt(ctx, list, kCtrlSetState, {0});
    Virt(ctx, find(kCheckpointBar), kCtrlSetState, {0});
    Virt(ctx, menu, kMenuGotoControl, {kCheckpointList});
    Virt(ctx, find(kSessionList), kCtrlSetState, {3});
    Virt(ctx, find(kSessionBar), kCtrlSetState, {3});
    for (uint32_t id : kDeleteIcons) Virt(ctx, find(id), kCtrlSetState, {3});
    LOGI("extras: Level Select lists %zu of %zu levels", g_ls.list.size(), table.size());
}

// TMenuLoadSession::OnCommand in Level Select: A picks the level, B goes back to Extras, and the delete command, which would
// delete saved games, does nothing. True when the command is dealt with here.
bool LevelSelectCommand(X86Context& ctx, uint32_t menu, uint32_t command) {
    // the stock menu is busy: fading, launching, deleting or showing a dialog, which A closes. It deals with those itself.
    if (kernel::R32(kFadeMode) || R8(menu + 0x4E) || R8(menu + 0x4C) || R8(menu + 0x4B)) return false;
    switch (command) {
    case kCommandA: {
        const int index = int(Virt(ctx, kernel::R32(menu + 0x70), kListGetCurSel, {}));
        if (index < 0 || size_t(index) >= g_ls.list.size()) return true;
        This(ctx, 0x218C20, menu, {command});  // TBuffyMenu::OnBuffyCommand
        g_ls.pending = kernel::ReadCString(kernel::R32(g_ls.array + uint32_t(g_ls.list[size_t(index)]) * 0x2C), 80);
        const std::string blocked = NewGameBlocked(ctx);
        if (!blocked.empty()) {
            LOGI("extras: Level Select can't start a game: %s", blocked.c_str());
            g_ls.pending.clear();
            ShowDialog(ctx, menu, blocked);
            return true;
        }
        LOGI("extras: Level Select picked %s", g_ls.pending.c_str());
        This(ctx, 0x218C60, menu, {command});  // TBuffyMenu::OnCommandSounds
        SetMenu(ctx, kMenuStartGame, true);
        return true;
    }
    case kCommandB:
        This(ctx, 0x218C20, menu, {command});
        if (const uint32_t back = kernel::R32(menu + 0x40)) {
            PlaySound(ctx, 0x198);
            SetMenu(ctx, back, false);
        }
        return true;
    case kCommandX:
    case kCommandLeft:
    case kCommandRight:
        return true;
    default:
        return false;
    }
}

void OpenLevelSelect(X86Context& ctx) {
    g_ls.armed = true;
    LOGI("extras: Level Select opens");
    SetMenu(ctx, kMenuLoadSession, true);
}

}  // namespace

// TMenu::PersistRead(TReadStream*): a menu's controls from the menu file. The Extras menu is extended once it has read its own.
X86_EXTERN_FUNC(__imp__sub_001ECBC0);
X86_FUNC(sub_001ECBC0) {
    const uint32_t menu = ctx.ecx, stream = kernel::R32(ctx.esp + 4);
    __imp__sub_001ECBC0(ctx, base);
    if (kernel::R32(menu) == kCreditsVtable) ExtendMenu(ctx, menu, stream);
}

// TMenuCredits::OnMessage(TControl*, long message, long, long): message 5 is a button chosen
X86_EXTERN_FUNC(__imp__sub_0021D890);
X86_FUNC(sub_0021D890) {
    const uint32_t menu = ctx.ecx, ctrl = kernel::R32(ctx.esp + 4), message = kernel::R32(ctx.esp + 8);
    if (message == 5 && R8(menu + 0x48)) {
        const uint32_t id = Virt(ctx, ctrl, kCtrlGetID, {});
        if (id >= kIdHard1 && id < kIdHard1 + 4) {
            // the stock arena button does the work with cheat 3's "d" added to the name, and the cheat goes again once it has
            // queued the level, so it isn't carried into the levels after
            const uint32_t stock = Virt(ctx, menu, kMenuFindControl, {id - kIdHard1 + kIdArena1});
            const uint32_t cheats = RunOptions() + kRunOptionsCheats, before = kernel::R32(cheats);
            kernel::W32(cheats, before | kCheatHard);
            kernel::W32(ctx.esp + 4, stock);
            LOGI("extras: Arena %u (Hard)", id - kIdHard1 + 1);
            __imp__sub_0021D890(ctx, base);
            kernel::W32(cheats, (kernel::R32(cheats) & ~kCheatHard) | (before & kCheatHard));
            return;
        }
        if (id == kIdLevelSelect) {
            OpenLevelSelect(ctx);
            ctx.esp += 4 + 16;  // the return address and the four arguments this stands in for
            return;
        }
    }
    __imp__sub_0021D890(ctx, base);
}

// TGO::Map_QueueChange(const char*, long): the game starting any level or going back to the menu
X86_EXTERN_FUNC(__imp__sub_00095360);
X86_FUNC(sub_00095360) {
    LOGI("extras: Map_QueueChange(%s) cheats %#x", kernel::ReadCString(kernel::R32(ctx.esp + 4), 80).c_str(),
         kernel::R32(RunOptions() + kRunOptionsCheats));
    __imp__sub_00095360(ctx, base);
}

// GUI::TManager::SetMenu(id, bool back): the front end changing menus. TBuffyManager::SetMenu fades and ends up here.
// Level Select is on from Extras opening menu 5, and off at any change to a menu other than 5, 12 and 13.
X86_EXTERN_FUNC(__imp__sub_001EBE00);
X86_FUNC(sub_001EBE00) {
    const uint32_t id = kernel::R32(ctx.esp + 4);
    LOGI("extras: menu %u (back %u)", id, kernel::R32(ctx.esp + 8) & 0xFF);
    if (id == kMenuLoadSession) {
        g_ls.active = g_ls.armed || g_ls.active;
        g_ls.armed = false;
    } else if (id != kMenuStartGame && id != kMenuDifficulty) {
        if (g_ls.active) LOGI("extras: Level Select ends");
        g_ls.armed = g_ls.active = false;
        g_ls.pending.clear();
    }
    __imp__sub_001EBE00(ctx, base);
}

// GUI::TMenuLoadSession::OnLoad
X86_EXTERN_FUNC(__imp__sub_00220F10);
X86_FUNC(sub_00220F10) {
    const uint32_t menu = ctx.ecx;
    __imp__sub_00220F10(ctx, base);
    ShowLevelSelect(ctx, menu);
}

// GUI::TMenuLoadSession::OnCommand(command)
X86_EXTERN_FUNC(__imp__sub_00221380);
X86_FUNC(sub_00221380) {
    const uint32_t menu = ctx.ecx, command = kernel::R32(ctx.esp + 4);
    if (g_ls.active && R8(menu + 0x48) && LevelSelectCommand(ctx, menu, command)) {
        ctx.esp += 8;  // the return address and the command
        return;
    }
    __imp__sub_00221380(ctx, base);
}

// ---- the interface the rest of the port uses (extras.h)
std::string extras::CurrentMap() {
    const uint32_t game = kernel::R32(kGame);
    if (!game) return {};
    return levels::Lower(kernel::ReadCString(kernel::R32(kernel::R32(game + 0x40) + 0x40), 80));
}

void extras::LevelStarted(X86Context& ctx) {
    const std::string map = CurrentMap();
    if (map.empty()) return;
    EnsureSeeded();
    const uint32_t ro = RunOptions();
    bool story = false;
    for (const levels::Entry& e : Table(ctx))
        if (!e.special && levels::Lower(e.map) == map) story = true;
    if (story) NoteReached(map);
    LOGI("extras: level %s begins (%s), cheats %#x", map.c_str(), story ? "reached" : "not a story level",
         ro ? kernel::R32(ro + kRunOptionsCheats) : 0);
}

// The new game begins at the first level of the mission list. In Level Select the name goes in, in memory the game's own
// operator new gave: its callers keep their strings, and the game frees none of this.
uint32_t extras::LaunchMap(X86Context& ctx, uint32_t name) {
    if (!g_ls.active || g_ls.pending.empty()) return name;
    const std::vector<levels::Entry> table = Table(ctx);
    const std::string launching = kernel::ReadCString(name, 80);
    if (table.empty() || levels::Lower(table[0].map) != levels::Lower(launching)) {
        LOGW("extras: Level Select picked %s, but the menu is launching %s", g_ls.pending.c_str(), launching.c_str());
        return name;
    }
    const uint32_t s = GuestNew(ctx, uint32_t(g_ls.pending.size() + 1));
    if (!s) return name;
    std::memcpy(kernel::HostPtr(s), g_ls.pending.c_str(), g_ls.pending.size() + 1);
    LOGI("extras: the new game starts at %s instead of %s", g_ls.pending.c_str(), launching.c_str());
    g_ls.pending.clear();
    g_ls.active = false;
    return s;
}

// The records of the session in memory (TSessionManager +0x0C, the header and records of a .ses file) say which levels it has
// reached.
bool extras::SessionPlayedChapter(X86Context& ctx, int chapter) {
    const uint32_t sm = kernel::R32(kSessionManager);
    const std::vector<levels::Entry> table = Table(ctx);
    if (!sm || table.empty()) return false;
    const std::vector<std::string> reached =
        levels::VisitedMaps(static_cast<const uint8_t*>(kernel::HostPtr(sm + 0x0C)), levels::kSesHeader + levels::kSesSlots * levels::kSesRecord);
    const size_t gate = levels::ChapterGate(table, chapter);
    const bool played = gate < table.size() && std::find(reached.begin(), reached.end(), levels::Lower(table[gate].map)) != reached.end();
    LOGI("extras: the session %s chapter %d from its start (its records: %zu, through %s)", played ? "played" : "didn't play", chapter,
         reached.size(), gate < table.size() ? table[gate].map.c_str() : "?");
    return played;
}

bool extras::SessionPlayedFromStart(X86Context& ctx) {
    const std::vector<levels::Entry> table = Table(ctx);
    if (table.empty()) return false;
    for (int chapter = 1; chapter <= levels::Chapters(table); chapter++)
        if (!SessionPlayedChapter(ctx, chapter)) return false;
    return true;
}
