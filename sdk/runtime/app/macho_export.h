#pragma once
// Reads symbols of a Mach-O file from the file alone: nothing is loaded or run, so it is safe on a dylib that may be stale, damaged or
// built for another program (a dlopen would bind its names to the running program's and fail, or run nothing of it but still map it),
// and it works on any machine. The macOS counterpart of app/pe_export.h: the game's code dylib exports X86GameCodeStamp, which says what
// it was built for (cpu/dispatch.cpp, app/game_build.cpp); the host's own file is read for the names it defines (dispatch::Load).
// Only thin 64 bit little endian arm64 files are read, as lld writes them: the symbol table (LC_SYMTAB) gives a symbol's address and
// the segments (LC_SEGMENT_64) turn that into a place in the file. Every size and offset is checked against the file's own size.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace macho {

constexpr uint32_t kTypeExecute = 2, kTypeDylib = 6;

namespace detail {

inline uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
inline uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32; }

struct Segment {
    uint64_t vmaddr, filesize, fileoff;
};

// the file's segments, symbol table and string table, checked
struct Image {
    std::ifstream f;
    uint64_t size = 0;
    std::vector<Segment> segments;
    std::vector<uint8_t> symbols, strings;
    uint32_t nsyms = 0;

    std::vector<uint8_t> Read(uint64_t at, uint64_t len) {
        std::vector<uint8_t> b;
        if (at > size || len > size - at) return b;
        b.resize(size_t(len));
        f.clear();
        f.seekg(std::streamoff(at));
        f.read(reinterpret_cast<char*>(b.data()), std::streamsize(len));
        b.resize(size_t(f.gcount() > 0 ? f.gcount() : 0));
        return b;
    }

    // false with why (a sentence that goes after "the linked file") when the file is not an arm64 Mach-O of `filetype` with a symbol table
    bool Open(const std::filesystem::path& path, uint32_t filetype, std::string& why) {
        f.open(path, std::ios::binary | std::ios::ate);
        if (!f) {
            why = "cannot be opened";
            return false;
        }
        size = uint64_t(f.tellg());
        std::vector<uint8_t> head = Read(0, 32);
        if (head.size() < 32 || U32(head.data()) != 0xFEEDFACFu) {  // MH_MAGIC_64
            why = "is not a 64 bit Mach-O file";
            return false;
        }
        if (U32(&head[4]) != 0x0100000Cu) {  // CPU_TYPE_ARM64
            why = "is not for arm64";
            return false;
        }
        if (U32(&head[12]) != filetype) {
            why = filetype == kTypeDylib ? "is not a dynamic library" : "is not an executable";
            return false;
        }
        const uint32_t ncmds = U32(&head[16]), sizeofcmds = U32(&head[20]);
        if (sizeofcmds > (16u << 20) || sizeofcmds > size - 32) {
            why = "has an impossible header";
            return false;
        }
        std::vector<uint8_t> cmds = Read(32, sizeofcmds);
        if (cmds.size() != sizeofcmds) {
            why = "is cut short";
            return false;
        }
        uint32_t symoff = 0, stroff = 0, strsize = 0;
        size_t at = 0;
        for (uint32_t i = 0; i < ncmds; i++) {
            if (at + 8 > cmds.size()) break;
            const uint32_t cmd = U32(&cmds[at]), csize = U32(&cmds[at + 4]);
            if (csize < 8 || csize > cmds.size() - at) break;
            if (cmd == 0x19 && csize >= 72) {  // LC_SEGMENT_64: name[16], vmaddr, vmsize, fileoff, filesize
                Segment s{U64(&cmds[at + 24]), U64(&cmds[at + 48]), U64(&cmds[at + 40])};
                // a segment that wraps around the address space, or lies outside the file, places nothing
                if (s.vmaddr + s.filesize >= s.vmaddr && s.fileoff <= size && s.filesize <= size - s.fileoff) segments.push_back(s);
            } else if (cmd == 0x2 && csize >= 24) {  // LC_SYMTAB: symoff, nsyms, stroff, strsize
                symoff = U32(&cmds[at + 8]);
                nsyms = U32(&cmds[at + 12]);
                stroff = U32(&cmds[at + 16]);
                strsize = U32(&cmds[at + 20]);
            }
            at += csize;
        }
        if (!nsyms || !strsize) {
            why = "has no symbol table";
            return false;
        }
        // the tables must fit in the file before anything is allocated for them
        if (uint64_t(nsyms) * 16 > size || strsize > size || symoff > size - uint64_t(nsyms) * 16 || stroff > size - strsize) {
            why = "has a symbol table that does not fit in the file";
            return false;
        }
        strings = Read(stroff, strsize);
        symbols = Read(symoff, uint64_t(nsyms) * 16);
        if (strings.size() != strsize || symbols.size() != uint64_t(nsyms) * 16) {
            why = "has a symbol table that is cut short";
            return false;
        }
        return true;
    }

    // the defined external symbols (N_EXT in a section; no debugging entries), in file order: the name without its underscore, and the address
    template <class Fn>
    void EachDefined(Fn&& fn) const {
        for (uint32_t i = 0; i < nsyms; i++) {
            const uint8_t* sym = &symbols[size_t(i) * 16];  // n_strx, n_type, n_sect, n_desc, n_value
            const uint32_t strx = U32(sym);
            if ((sym[4] & 0xE0) || (sym[4] & 0x0E) != 0x0E || !(sym[4] & 0x01) || strx >= strings.size()) continue;  // not N_STAB, N_SECT, N_EXT
            const char* name = reinterpret_cast<const char*>(&strings[strx]);
            size_t n = 0;
            while (n < strings.size() - strx && name[n]) n++;
            if (n == strings.size() - strx || n < 2 || name[0] != '_') continue;  // unterminated, or no C name
            fn(std::string(name + 1, n - 1), U64(sym + 8));
        }
    }
};

}  // namespace detail

// The text of the symbol `name` in a dylib (a data symbol: an array of chars, as `extern "C" const char X86GameCodeStamp[]`, which Mach-O
// names with a leading underscore). False with why (a sentence that goes after "the linked file") when the file is not a dylib that has it.
inline bool ReadExportedText(const std::filesystem::path& path, const char* name, std::string& out, std::string& why) {
    detail::Image image;
    if (!image.Open(path, kTypeDylib, why)) return false;
    bool found = false;
    uint64_t value = 0;
    image.EachDefined([&](const std::string& n, uint64_t v) {
        if (!found && n == name) {
            found = true;
            value = v;
        }
    });
    if (!found) {
        why = std::string("does not export ") + name;
        return false;
    }
    for (const detail::Segment& s : image.segments) {
        if (value < s.vmaddr || value - s.vmaddr >= s.filesize) continue;
        std::vector<uint8_t> text = image.Read(s.fileoff + (value - s.vmaddr), std::min<uint64_t>(512, s.filesize - (value - s.vmaddr)));
        size_t len = 0;
        while (len < text.size() && text[len]) len++;
        if (len == text.size()) {
            why = std::string("has an unterminated ") + name;
            return false;
        }
        out.assign(reinterpret_cast<const char*>(text.data()), len);
        return true;
    }
    why = std::string("has ") + name + " outside its file";
    return false;
}

// The names of the functions an executable defines and exports whose name starts with `prefix` (no underscore: "sub_"), for a program to
// know what it overrides without asking its loader. False with why when the file is not an arm64 executable with a symbol table.
inline bool DefinedNames(const std::filesystem::path& path, const char* prefix, std::vector<std::string>& out, std::string& why) {
    detail::Image image;
    if (!image.Open(path, kTypeExecute, why)) return false;
    const size_t n = std::strlen(prefix);
    image.EachDefined([&](const std::string& name, uint64_t) {
        if (name.compare(0, n, prefix) == 0) out.push_back(name);
    });
    return true;
}

}  // namespace macho
