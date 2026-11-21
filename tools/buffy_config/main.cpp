// Writes config/game.toml from the game's linker map, as tools/make_config.py does, without Python, so that the player's PC can make
// it from the files on their own disc (the first start of the Windows build runs it before the recompiler). It is a port of
// make_config.py and buffymap.py and must write the same bytes: tools/check_config_tool.sh compares the two on the local assets.
//
//   buffy_config --map final.map --xbe Default.xbe [--names sdk_names.txt] [--out game.toml]
//   buffy_config --scan-sdk <sdk folder> --out sdk_names.txt
//
// The first writes the recompiler's input: what xbe_symbols does for a title with signatures, with real names. Only an exact API
// name or an entry of kAliases binds. A loose match could replace an internal layer the library still needs
// (CDirectSoundVoice::SetPitch is not IDirectSoundBuffer_SetPitch), so a name the map lacks stays 0: the host's override then has
// nothing to catch, and the code that needed it shows up in the coverage report (tools/coverage.py). The evidence for the values
// is in docs/xdk-4432.md.
//
// sdk_names.txt is every guest name the runtime can bind: make_config.py found them in the sources of sdk/runtime when it ran, which
// the player's PC has not got, so --scan-sdk finds them at build time and the file goes beside this program.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void Die(const std::string& message) {
    std::fprintf(stderr, "buffy_config: %s\n", message.c_str());
    std::exit(1);
}

std::string Hex8(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

// --- the game's linker map (buffymap.py) ---

// The map's Rva+Base column is a PE link address and does not match the XBE, so an address is the XBE section's VA plus the map's
// section offset. The map's section numbers line up with the XBE's sections only through this table.
struct SectionInfo {
    const char* name;
    uint32_t va;
};
const std::map<int, SectionInfo> kSections = {
    {0x1, {".text", 0x00011000}}, {0x2, {"D3D", 0x005913A0}},    {0x3, {"D3DX", 0x005A29C0}},     {0x4, {"XGRPH", 0x005A4BC0}},
    {0x5, {"DSOUND", 0x005A6BE0}}, {0x6, {"WMADECXM", 0x005C38C0}}, {0x7, {"WMADEC", 0x005C4100}},  {0x8, {"WMVDEC", 0x005DCE80}},
    {0x9, {"XPP", 0x0060F740}},   {0xA, {".rdata", 0x00616D80}}, {0xB, {".data", 0x00750EA0}},     {0xC, {"DOLBY", 0x009163C0}},
};
const char* const kCodeSections[] = {".text", "D3D", "D3DX", "XGRPH", "DSOUND", "WMADECXM", "WMADEC", "WMVDEC", "XPP"};

struct Symbol {
    std::string name;
    uint32_t va;
    int section;
    bool is_func;
    std::string lib;  // "" for the game's own objects
};

struct Segment {
    int section;
    uint32_t offset, length;
    std::string name;
};

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }
bool IsHexLower(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool AllHexLower(const char* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!IsHexLower(p[i])) return false;
    return true;
}
uint32_t ParseHex(const char* p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) v = v * 16 + uint32_t(p[i] <= '9' ? p[i] - '0' : p[i] - 'a' + 10);
    return v;
}

struct Map {
    std::vector<Segment> segments;
    std::vector<Symbol> symbols;
    std::unordered_map<std::string, size_t> by_name;  // the first symbol of each name

    // ` 0001:00000000 00576e21H .text                   CODE`
    static bool ParseSegment(const std::string& l, Segment& out) {
        if (l.size() < 28 || l[0] != ' ' || l[5] != ':' || l[14] != ' ' || l[23] != 'H' || l[24] != ' ') return false;
        if (!AllHexLower(&l[1], 4) || !AllHexLower(&l[6], 8) || !AllHexLower(&l[15], 8)) return false;
        size_t p = 25, q = p;
        while (q < l.size() && !IsSpace(l[q])) q++;
        if (q == p || q == l.size()) return false;
        size_t c = q;
        while (c < l.size() && IsSpace(l[c])) c++;
        size_t e = l.size();
        while (e > c && IsSpace(l[e - 1])) e--;
        std::string cls = l.substr(c, e - c);
        if (cls != "CODE" && cls != "DATA") return false;
        out = {int(ParseHex(&l[1], 4)), ParseHex(&l[6], 8), ParseHex(&l[15], 8), l.substr(p, q - p)};
        return true;
    }

    // ` 0001:00000000       _name       00411000 f   lib:obj`: the Rva+Base column, then f (a function) and i (an import) when present
    static bool ParseSymbol(const std::string& l, Symbol& out) {
        if (l.size() < 15 || l[0] != ' ' || l[5] != ':' || !AllHexLower(&l[1], 4) || !AllHexLower(&l[6], 8) || !IsSpace(l[14]))
            return false;
        std::vector<std::string> t;
        for (size_t i = 14; i < l.size();) {
            while (i < l.size() && IsSpace(l[i])) i++;
            size_t j = i;
            while (j < l.size() && !IsSpace(l[j])) j++;
            if (j > i) t.push_back(l.substr(i, j - i));
            i = j;
        }
        if (t.size() < 3 || t[1].size() != 8 || !AllHexLower(t[1].data(), 8)) return false;
        size_t k = 2, n = t.size();
        bool f = false;
        if (t[k] == "f" && n - k >= 2) f = true, k++;
        if (t[k] == "i" && n - k >= 2) k++;
        if (n - k != 1) return false;
        int sec = int(ParseHex(&l[1], 4));
        auto info = kSections.find(sec);
        if (info == kSections.end()) return false;
        size_t colon = t[k].rfind(':');
        out = {t[0], info->second.va + ParseHex(&l[6], 8), sec, f, colon == std::string::npos ? "" : t[k].substr(0, colon)};
        return true;
    }

    explicit Map(const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) Die("cannot open " + path.string());
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            Segment seg;
            if (ParseSegment(line, seg)) {
                segments.push_back(std::move(seg));
                continue;
            }
            Symbol s;
            if (ParseSymbol(line, s)) symbols.push_back(std::move(s));
        }
        for (size_t i = 0; i < symbols.size(); i++) by_name.emplace(symbols[i].name, i);  // first one wins
    }

    const Symbol* Find(const std::string& name) const {
        auto it = by_name.find(name);
        return it == by_name.end() ? nullptr : &symbols[it->second];
    }

    // [lo, hi) of a section's segment, the first of that name
    bool SegRange(int section, const std::string& name, uint32_t& lo, uint32_t& hi) const {
        for (const Segment& s : segments)
            if (s.section == section && s.name == name) {
                lo = kSections.at(section).va + s.offset;
                hi = lo + s.length;
                return true;
            }
        return false;
    }
};

// --- what the runtime can bind (make_config.py's sdk_names) ---

bool IsWord(char c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// every `key` (ending in its open parenthesis) at a word boundary, followed by an identifier, and for close_paren by `)`
// right after it: the regexes \bKEY\(\s*(\w+) and \bKEY\((\w+)\)
void FindCalls(const std::string& text, const char* key, bool skip_space, bool close_paren, std::set<std::string>& names) {
    const size_t n = std::strlen(key);
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + n)) {
        if (at > 0 && IsWord(text[at - 1])) continue;
        size_t i = at + n;
        if (skip_space)
            while (i < text.size() && (IsSpace(text[i]) || text[i] == '\n' || text[i] == '\r')) i++;
        size_t j = i;
        while (j < text.size() && IsWord(text[j])) j++;
        if (j == i) continue;
        if (close_paren && (j >= text.size() || text[j] != ')')) continue;
        names.insert(text.substr(i, j - i));
    }
}

bool ReadText(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

std::set<std::string> ScanSdk(const fs::path& sdk) {
    std::set<std::string> names;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(sdk / "runtime", ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') {  // glob's ** and * skip hidden names and what is inside hidden folders
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        std::string ext = it->path().extension().string();
        if ((ext == ".cpp" || ext == ".h") && it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::string d3d8;
    for (const fs::path& path : files) {
        std::string text;
        if (!ReadText(path, text)) continue;
        for (const char* key : {"HLE_STDCALL(", "HLE_FASTCALL(", "HLE_CDECL("}) FindCalls(text, key, true, false, names);
        FindCalls(text, "GUEST(", false, true, names);
        FindCalls(text, "SYM(", false, true, names);
        FindCalls(text, "CRT_MATH(", false, false, names);
        if (path == sdk / "runtime" / "xdk" / "d3d8.cpp") d3d8 = std::move(text);
    }
    // each known XDK's signature table: the names under [symbols]
    std::vector<fs::path> tables;
    for (fs::directory_iterator it(sdk / "xdk", ec), end; !ec && it != end; it.increment(ec)) {
        fs::path t = it->path() / "signatures.toml";
        if (!it->path().filename().string().empty() && it->path().filename().string()[0] != '.' && fs::is_regular_file(t, ec)) tables.push_back(t);
    }
    for (const fs::path& t : tables) {
        std::string text;
        if (!ReadText(t, text)) continue;
        size_t at = 0;
        for (; (at = text.find("[symbols]\n", at)) != std::string::npos; at++)
            if (at == 0 || text[at - 1] == '\n') break;
        if (at == std::string::npos) Die(t.string() + ": no [symbols] table");
        for (size_t p = at + 10; p < text.size();) {  // line starts, up to the next table
            if (text[p] == '[') break;
            size_t e = p;
            while (e < text.size() && IsWord(text[e])) e++;
            size_t q = e;
            while (q < text.size() && IsSpace(text[q])) q++;
            if (e > p && q < text.size() && text[q] == '=') names.insert(text.substr(p, e - p));
            size_t nl = text.find('\n', p);
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
    }
    // D3DDevice_SetRenderState_<name>, written wherever d3d8.cpp mentions one
    std::set<std::string> states;
    {
        const std::string key = "D3DDevice_SetRenderState_";
        for (size_t at = d3d8.find(key); at != std::string::npos; at = d3d8.find(key, at + key.size())) {
            if (at > 0 && IsWord(d3d8[at - 1])) continue;
            size_t i = at + key.size(), j = i;
            while (j < d3d8.size() && IsWord(d3d8[j])) j++;
            if (j > i) states.insert(d3d8.substr(i, j - i));
        }
    }
    for (const std::string& s : states) names.insert("D3DDevice_SetRenderState_" + s);
    names.erase("name");
    return names;
}


// --- the config (make_config.py) ---

// SDK name -> decorated name in this map where 4432 calls it something else; ?Name@Class@@ matches up to its signature
const std::map<std::string, std::string> kAliases = {
    {"CDevice_KickOff", "?KickOff@CDevice@D3D@@"},
    {"D3DDevice_MakeSpace", "?MakeSpace@D3D@@"},
    {"D3D_SetFence", "?SetFence@D3D@@"},
    {"D3D_BlockOnTime", "?BlockOnTime@D3D@@"},
    {"D3D_BlockOnResource", "?BlockOnResource@D3D@@"},
    {"D3D_MakeRequestedSpace", "?MakeRequestedSpace@D3D@@"},
    {"D3D_KickOffAndWaitForIdle", "?KickOffAndWaitForIdle@D3D@@"},
    {"CDirectSoundStream_AddRef", "?AddRef@CDirectSoundStream@DirectSound@@UAG"},
    {"CDirectSoundStream_Release", "?Release@CDirectSoundStream@DirectSound@@UAG"},
    {"CDirectSoundStream_GetInfo", "?GetInfo@CDirectSoundStream@DirectSound@@"},
    {"CDirectSoundStream_GetStatus", "?GetStatus@CDirectSoundStream@DirectSound@@"},
    {"CDirectSoundStream_Process", "?Process@CDirectSoundStream@DirectSound@@"},
    {"CDirectSoundStream_Discontinuity", "?Discontinuity@CDirectSoundStream@DirectSound@@"},
    {"CDirectSoundStream_Flush", "?Flush@CDirectSoundStream@DirectSound@@"},
    {"CDirectSoundStream_Pause", "?Pause@CDirectSoundStream@DirectSound@@"},
    {"CDirectSoundStream_SetVolume", "?SetVolume@CDirectSoundStream@DirectSound@@"},
};
const std::map<std::string, std::string> kDataAliases = {
    {"D3D_Device", "?g_Device@D3D@@"},
    {"D3D_pDevice", "?g_pDevice@D3D@@"},
    {"D3D_DirtyFlags", "_D3D__DirtyFlags"},
    {"D3D_RenderState", "_D3D__RenderState"},
    {"D3D_DeferredTextureState", "_D3D__TextureState"},
    {"D3D_IndexData", "_D3D__IndexData"},
    {"D3D_Streams", "?g_Stream@D3D@@"},
    {"D3D_FvfShader", "?g_FvfVertexShader@D3D@@"},
    {"DSound_StreamVtable", "??_7CDirectSoundStream@DirectSound@@6BIDirectSoundStream@@@"},
    {"DSound_StreamVtable2", "??_7CDirectSoundStream@DirectSound@@6BCDirectSoundVoice@1@@"},
    {"XPP_GamepadType", "_XDEVICE_TYPE_GAMEPAD_TABLE"},
    {"XPP_MemoryUnitType", "_XDEVICE_TYPE_MEMORY_UNIT_TABLE"},
    {"D3D_SimpleRenderStateEncode", "_D3DSIMPLERENDERSTATEENCODE"},
};
// The CRT's math shares one body between its stack entry (_acos) and its register entry (__CIacos), and the host's crt_* handlers
// replace that body: x in st0, or for pow x and y above the return address and y in st0. The map doesn't name the bodies, so each
// is read from the register entry's call: SDK name -> (register entry, offset of the call).
const std::map<std::string, std::pair<std::string, uint32_t>> kCrtBodies = {
    {"crt_acos", {"__CIacos", 0x0B}},
    {"crt_asin", {"__CIasin", 0x0B}},
    {"crt_sqrt", {"__CIsqrt", 0x0B}},
    {"crt_pow", {"__CIpow_default", 0x10}},
};
// proven by hand from the disassembly, not named in the map: see docs/xdk-4432.md
const std::map<std::string, uint32_t> kManualData = {
    {"D3D_DefaultVertexShader", 0x0059FD78},  // D3D::CDevice::Init 0x598968 stores g_FvfVertexShader in the device
    {"D3D_DeviceVertexShaderField", 0x380},   // at this offset, CDevice_VertexShader
};
// the sections' read-only and writable parts: linked into the same XBE section as the code, and never code
const std::pair<int, const char*> kDataSegments[] = {
    {2, "D3D_RD"},     {2, "D3D_URW"},    {3, "D3DX_RD"},     {4, "XGRPH_RD"},   {5, "DSOUND_RD"},   {5, "DSOUND_RW"},  {5, "DSOUND_URW"},
    {6, "WMADECXMO_RD"}, {7, "WMADEC_RD"}, {7, "WMADEC_RW"},  {8, "WMVDECRD"},   {8, "WMVDECRW"},    {9, ".XPP$Data"},  {9, ".XPPRDATA"},
};

bool IsCodeSection(int section) {
    const char* n = kSections.at(section).name;
    for (const char* c : kCodeSections)
        if (std::strcmp(n, c) == 0) return true;
    return false;
}

// The one symbol with this decorated name, or with this name up to its signature
const Symbol& Decorated(const Map& m, const std::string& name) {
    if (const Symbol* s = m.Find(name)) return *s;
    // by address, as make_config.py's dict does: a later symbol at the same address replaces the earlier one in place
    std::vector<uint32_t> order;
    std::unordered_map<uint32_t, const Symbol*> found;
    for (const Symbol& s : m.symbols)
        if (s.name.compare(0, name.size(), name) == 0) {
            if (!found.count(s.va)) order.push_back(s.va);
            found[s.va] = &s;
        }
    if (found.size() != 1) Die(name + ": " + std::to_string(found.size()) + " matches in the map");
    return *found[order[0]];
}

// The address a CRT register entry calls: the host's crt_* handlers replace this body, not the named entry
Symbol CrtBody(const Map& m, const std::vector<uint8_t>& xbe, const std::string& name) {
    const auto& [entry, call] = kCrtBodies.at(name);
    const Symbol* e = m.Find(entry);
    if (!e) Die(name + ": " + entry + " is not in the map");
    uint32_t site = e->va + call;
    uint32_t offset = 0x1000 + site - kSections.at(1).va;
    if (size_t(offset) + 5 > xbe.size() || xbe[offset] != 0xE8) Die(name + ": no call at " + Hex8(site));
    int32_t rel;
    std::memcpy(&rel, &xbe[offset + 1], 4);
    return {name, uint32_t(site + 5 + rel), 1, true, "LIBCMT"};
}

// The symbol for an SDK name: its alias, else the exact undecorated API name in a library
const Symbol* Lookup(const Map& m, const std::string& name) {
    auto alias = kAliases.find(name);
    if (alias != kAliases.end()) return &Decorated(m, alias->second);
    if (name.compare(0, 4, "crt_") == 0) return nullptr;  // a stack entry takes its arguments elsewhere: it must never bind to a crt_* handler
    for (const std::string& form : {name, "_" + name})
        if (const Symbol* s = m.Find(form); s && !s->lib.empty()) return s;
    for (const Symbol& s : m.symbols) {
        // [_@]<name>@\d+
        if (s.lib.empty() || s.name.size() < name.size() + 3) continue;
        if (s.name[0] != '_' && s.name[0] != '@') continue;
        if (s.name.compare(1, name.size(), name) != 0 || s.name[1 + name.size()] != '@') continue;
        bool digits = s.name.size() > name.size() + 2;
        for (size_t i = name.size() + 2; i < s.name.size(); i++) digits &= s.name[i] >= '0' && s.name[i] <= '9';
        if (digits) return &s;
    }
    return nullptr;
}

int MakeConfig(const fs::path& map_path, const fs::path& xbe_path, const fs::path& names_path, const fs::path& out_path) {
    Map m(map_path);
    std::vector<uint8_t> xbe;
    {
        std::ifstream f(xbe_path, std::ios::binary);
        if (!f) Die("cannot open " + xbe_path.string());
        xbe.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::set<std::string> all_names;
    {
        std::ifstream f(names_path);
        if (!f) Die("cannot open " + names_path.string() + " (written by buffy_config --scan-sdk)");
        for (std::string l; std::getline(f, l);) {
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            if (!l.empty()) all_names.insert(l);
        }
    }

    std::map<std::string, Symbol> symbols;
    std::vector<std::string> absent;
    for (const std::string& name : all_names) {
        if (kDataAliases.count(name) || kManualData.count(name)) continue;
        Symbol crt;
        const Symbol* s = nullptr;
        if (kCrtBodies.count(name)) {
            crt = CrtBody(m, xbe, name);
            s = &crt;
        } else {
            s = Lookup(m, name);
        }
        if (s) symbols[name] = *s;
        else absent.push_back(name);
    }

    std::map<std::string, uint32_t> data;
    for (const auto& [name, decorated] : kDataAliases) data[name] = Decorated(m, decorated).va;
    for (const auto& [name, va] : kManualData) data[name] = va;

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const auto& [sec, seg] : kDataSegments) {
        uint32_t lo, hi;
        if (!m.SegRange(sec, seg, lo, hi)) Die(std::string("segment ") + seg + " is not in the map");
        if (hi > lo) ranges.emplace_back(lo, hi);
    }
    std::sort(ranges.begin(), ranges.end());
    for (const Symbol& s : m.symbols)
        if (s.is_func)
            for (auto& [lo, hi] : ranges)
                if (lo <= s.va && s.va < hi) Die("function " + s.name + " at " + Hex8(s.va) + " is inside a data range");

    std::set<uint32_t> bound;
    for (auto& [name, s] : symbols) bound.insert(s.va);
    std::set<uint32_t> funcs;
    for (const Symbol& s : m.symbols)
        if (s.is_func && IsCodeSection(s.section) && !bound.count(s.va)) funcs.insert(s.va);
    const Symbol* setjmp = m.Find("__setjmp3");
    if (!setjmp) Die("__setjmp3 is not in the map");

    std::string w;
    w += "# Recompiler input for Default.xbe, written by tools/make_config.py from final.map\n\n[xbe]\n";
    w += "path = \"assets/Default.xbe\"\ntitle = 0x45410012\nentry = 0x00579EA2\nxdk = 4432\n";
    w += "out = \"generated/game\"\nlibrary = \"Buffy.dll\"  # the game code DLL beside Buffy.exe, which the executable delay loads\n";
    w += "partitions = 128\n";
    w += "setjmp = [0x" + Hex8(setjmp->va) + "]\n";
    w += "code_sections = [";
    for (size_t i = 0; i < std::size(kCodeSections); i++) w += (i ? ", \"" : "\"") + std::string(kCodeSections[i]) + "\"";
    w += "]\ndata_ranges = [\n";
    for (auto& [lo, hi] : ranges) w += "    [0x" + Hex8(lo) + ", 0x" + Hex8(hi) + "],\n";
    w += "]\n\n# every function the map names\n[functions]\n";
    for (uint32_t va : funcs) w += "0x" + Hex8(va) + " = \"\"\n";
    w += "\n# functions the host replaces or calls by name, 0 where the image has none\n[symbols]\n";
    for (auto& [name, s] : symbols) w += name + " = 0x" + Hex8(s.va) + "\n";
    for (const std::string& name : absent) w += name + " = 0\n";
    w += "\n[data]\n";
    for (auto& [name, va] : data) w += name + " = 0x" + Hex8(va) + "\n";

    {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) Die("cannot write " + out_path.string());
        f << w;
        if (!f) Die("cannot write " + out_path.string());
    }
    std::printf("%zu functions, %zu symbols bound, %zu absent, %zu data, %zu data ranges\n", funcs.size(), symbols.size(), absent.size(),
                data.size(), ranges.size());
    return 0;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: buffy_config --map final.map --xbe Default.xbe [--names sdk_names.txt] [--out game.toml]\n"
                 "       buffy_config --scan-sdk <sdk folder> --out sdk_names.txt\n"
                 "  --names defaults to sdk_names.txt beside this program, --out to config/game.toml (or sdk_names.txt)\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path map = "assets/final.map", xbe = "assets/Default.xbe", names, out, scan;
    bool have_out = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&]() -> fs::path { return i + 1 < argc ? fs::path(argv[++i]) : (Usage(), std::exit(2), fs::path()); };
        if (a == "--map") map = value();
        else if (a == "--xbe") xbe = value();
        else if (a == "--names") names = value();
        else if (a == "--out") out = value(), have_out = true;
        else if (a == "--scan-sdk") scan = value();
        else return Usage();
    }
    if (!scan.empty()) {
        std::set<std::string> found = ScanSdk(scan);
        if (found.empty()) Die(scan.string() + ": no names found; is that the SDK folder?");
        std::ofstream f(have_out ? out : fs::path("sdk_names.txt"), std::ios::binary);
        if (!f) Die("cannot write the names file");
        for (const std::string& n : found) f << n << '\n';
        std::printf("%zu names\n", found.size());
        return 0;
    }
    if (names.empty()) names = fs::path(argv[0]).parent_path() / "sdk_names.txt";
    return MakeConfig(map, xbe, names, have_out ? out : fs::path("config/game.toml"));
}
