// Extras, the front end's TMenuCredits (menu 0x0B), drawn by the game's own menu code: Arena 1 to 4 are always listed, and
// four Hard versions are added (see docs/xdk-4432.md, "Extras"). The new entries are
// TTextButtons built the way the menu file builds them: a record in the game's binary menu format is put in guest memory,
// the menu's stream is pointed at it, and the game's own constructor, PersistRead and AddControl do the rest, so the
// buttons look and navigate like the stock ones.
#include <algorithm>
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
constexpr uint32_t kIdArena1 = 0x101;              // IDC_CREDITS_ARENA_1 .. 4
constexpr uint32_t kIdHard1 = 0x105;               // the new Arena (Hard) buttons, 0x105 .. 0x108
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
// lines, which the headless reference renderer leaves out), so ten need closer spacing than the file's six, whose own
// places run from 119 to 360 with 43 between the arenas. These run from 96 to 384, 32 apart, at the file's x.
struct Slot {
    uint32_t id;
    int x, y;
};
constexpr int kTop = 96, kStep = 32;
constexpr Slot kLayout[] = {
    {0x80, 110, kTop + 0 * kStep},  // Buffy History 101
    {0x81, 111, kTop + 1 * kStep},  // Credits
    {kIdArena1 + 0, 113, kTop + 2 * kStep}, {kIdArena1 + 1, 113, kTop + 3 * kStep},
    {kIdArena1 + 2, 113, kTop + 4 * kStep}, {kIdArena1 + 3, 113, kTop + 5 * kStep},
    {kIdHard1 + 0, 113, kTop + 6 * kStep},  {kIdHard1 + 1, 113, kTop + 7 * kStep},
    {kIdHard1 + 2, 113, kTop + 8 * kStep},  {kIdHard1 + 3, 113, kTop + 9 * kStep},
};


void ExtendMenu(X86Context& ctx, uint32_t menu, uint32_t stream) {
    const uint32_t size = kernel::R32(stream + 4), start = kernel::R32(stream + 8), pos = kernel::R32(stream + 0xC);
    auto find = [&](uint32_t id) { return Virt(ctx, menu, kMenuFindControl, {id}); };

    uint32_t tab = 11;
    for (uint32_t i = 0; i < 4; i++) {
        const std::string name = "IDC_CREDITS_ARENA_" + std::to_string(i + 1) + "_HARD";
        const std::string text = "Arena " + std::to_string(i + 1) + " (Hard)";
        AddButton(ctx, menu, stream, TextButton(name.c_str(), kIdHard1 + i, tab++, 0, 0, 90), text.c_str());
    }
    kernel::W32(stream + 4, size);  // the file's stream as it was
    kernel::W32(stream + 8, start);
    kernel::W32(stream + 0xC, pos);


    for (const Slot& s : kLayout)
        if (const uint32_t c = find(s.id)) Virt(ctx, c, kCtrlSetOffset, {uint32_t(s.x), uint32_t(s.y)});
    // the file hides the arenas until a cheat code unlocks them
    for (uint32_t i = 0; i < 4; i++)
        if (const uint32_t c = find(kIdArena1 + i)) Virt(ctx, c, kCtrlSetState, {0});
    LOGI("extras: the Extras menu has the Hard arenas");
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
X86_EXTERN_FUNC(__imp__sub_001EBE00);
X86_FUNC(sub_001EBE00) {
    const uint32_t id = kernel::R32(ctx.esp + 4);
    LOGI("extras: menu %u (back %u)", id, kernel::R32(ctx.esp + 8) & 0xFF);
    __imp__sub_001EBE00(ctx, base);
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
