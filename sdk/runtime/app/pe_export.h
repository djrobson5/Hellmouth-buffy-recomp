#pragma once
// Reads a string that a PE file (a DLL) exports as data, from the file alone: nothing is mapped or run, so it is safe on a DLL
// that may be stale, damaged or locked, and it works on any machine (the Mac check of the first start uses it on the Windows DLL
// it builds). The game's code DLL exports X86GameCodeStamp, which says what it was built for (cpu/dispatch.cpp, game_code.cpp).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pe {

namespace detail {
struct Section {
    uint32_t virtual_address, virtual_size, raw_size, raw_offset;
};

inline uint16_t U16(const std::vector<uint8_t>& b, size_t at) {
    return at + 2 <= b.size() ? uint16_t(b[at] | b[at + 1] << 8) : 0;
}
inline uint32_t U32(const std::vector<uint8_t>& b, size_t at) {
    return at + 4 <= b.size() ? uint32_t(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24) : 0;
}

class File {
public:
    explicit File(const std::filesystem::path& path) : f_(path, std::ios::binary) {}
    explicit operator bool() const { return bool(f_); }
    // len bytes at a position, fewer at the end of the file
    std::vector<uint8_t> Read(uint64_t at, size_t len) {
        std::vector<uint8_t> b(len);
        f_.clear();
        f_.seekg(std::streamoff(at));
        f_.read(reinterpret_cast<char*>(b.data()), std::streamsize(len));
        b.resize(size_t(std::max<std::streamsize>(f_.gcount(), 0)));
        return b;
    }
    // a NUL ended string at a position, empty when there is none within max
    std::string CString(uint64_t at, size_t max) {
        std::vector<uint8_t> b = Read(at, max);
        auto end = std::find(b.begin(), b.end(), 0);
        return end == b.end() ? std::string() : std::string(b.begin(), end);
    }

private:
    std::ifstream f_;
};
}  // namespace detail

// The text of the export `name` (a data export: an array of chars, as `extern "C" const char X86GameCodeStamp[]`). False with why
// when the file is not a PE file for a 64 bit program, has no such export, or the text is not there.
inline bool ReadExportedText(const std::filesystem::path& file, const char* name, std::string& text, std::string& why) {
    using namespace detail;
    File f(file);
    if (!f) {
        why = "cannot be opened";
        return false;
    }
    std::vector<uint8_t> head = f.Read(0, 0x1000);
    if (head.size() < 0x40 || head[0] != 'M' || head[1] != 'Z') {
        why = "is not a program file (no MZ header)";
        return false;
    }
    const uint32_t pe = U32(head, 0x3C);
    if (pe + 24 > head.size() || std::memcmp(head.data() + pe, "PE\0\0", 4) != 0) {
        why = "is not a program file (no PE header)";
        return false;
    }
    const uint16_t sections = U16(head, pe + 6), optional_size = U16(head, pe + 20);
    const size_t optional = pe + 24;
    if (U16(head, optional) != 0x20B) {  // PE32+
        why = "is not for a 64 bit program";
        return false;
    }
    const uint32_t export_rva = U32(head, optional + 112), export_size = U32(head, optional + 116);
    if (!export_rva || !export_size) {
        why = "exports nothing";
        return false;
    }
    std::vector<uint8_t> table = f.Read(optional + optional_size, size_t(sections) * 40);
    std::vector<Section> map;
    for (size_t i = 0; i + 40 <= table.size(); i += 40)
        map.push_back({U32(table, i + 12), U32(table, i + 8), U32(table, i + 16), U32(table, i + 20)});
    auto offset_of = [&](uint32_t rva, uint64_t& at) {
        for (const Section& s : map)
            if (rva >= s.virtual_address && rva < s.virtual_address + std::max(s.virtual_size, s.raw_size)) {
                at = uint64_t(s.raw_offset) + (rva - s.virtual_address);
                return true;
            }
        return false;
    };
    uint64_t dir_at = 0;
    if (!offset_of(export_rva, dir_at)) {
        why = "has an export table outside its sections";
        return false;
    }
    std::vector<uint8_t> dir = f.Read(dir_at, 40);
    const uint32_t functions = U32(dir, 20), names = U32(dir, 24);
    uint64_t functions_at = 0, names_at = 0, ordinals_at = 0;
    if (dir.size() < 40 || !offset_of(U32(dir, 28), functions_at) || !offset_of(U32(dir, 32), names_at) ||
        !offset_of(U32(dir, 36), ordinals_at) || names == 0 || names > 1u << 20) {
        why = "has a damaged export table";
        return false;
    }
    const std::vector<uint8_t> name_rvas = f.Read(names_at, size_t(names) * 4);
    if (name_rvas.size() < size_t(names) * 4) {
        why = "has a damaged export table";
        return false;
    }
    auto name_at = [&](uint32_t i, std::string& out) {
        uint64_t at = 0;
        if (!offset_of(U32(name_rvas, size_t(i) * 4), at)) return false;
        out = f.CString(at, 512);
        return true;
    };
    // the names are sorted, so a search by halves finds it; a file whose names are not sorted is searched through
    int64_t found = -1;
    int64_t lo = 0, hi = int64_t(names) - 1;
    while (lo <= hi) {
        int64_t mid = (lo + hi) / 2;
        std::string n;
        if (!name_at(uint32_t(mid), n)) break;
        int c = std::strcmp(n.c_str(), name);
        if (c == 0) {
            found = mid;
            break;
        }
        (c < 0 ? lo : hi) = c < 0 ? mid + 1 : mid - 1;
    }
    for (uint32_t i = 0; found < 0 && i < names; i++) {
        std::string n;
        if (name_at(i, n) && n == name) found = i;
    }
    if (found < 0) {
        why = std::string("has no ") + name;
        return false;
    }
    std::vector<uint8_t> ordinal = f.Read(ordinals_at + uint64_t(found) * 2, 2);
    const uint16_t index = U16(ordinal, 0);
    if (ordinal.size() < 2 || index >= functions) {
        why = "has a damaged export table";
        return false;
    }
    std::vector<uint8_t> rva = f.Read(functions_at + uint64_t(index) * 4, 4);
    uint64_t text_at = 0;
    // a forwarded export points back into the export table: not text
    if (rva.size() < 4 || U32(rva, 0) == 0 || (U32(rva, 0) >= export_rva && U32(rva, 0) < export_rva + export_size) ||
        !offset_of(U32(rva, 0), text_at)) {
        why = std::string(name) + " is not text";
        return false;
    }
    text = f.CString(text_at, 1024);
    if (text.empty()) {
        why = std::string(name) + " is empty";
        return false;
    }
    return true;
}

}  // namespace pe
