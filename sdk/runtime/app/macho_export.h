#pragma once
// Reads a string that a Mach-O dylib exports as data, from the file alone: nothing is loaded or run, so it is safe on a dylib that
// may be stale, damaged or built for another program (a dlopen would bind its names to the running program's and fail, or run
// nothing of it but still map it), and it works on any machine. The macOS counterpart of app/pe_export.h: the game's code dylib
// exports X86GameCodeStamp, which says what it was built for (cpu/dispatch.cpp, app/game_build.cpp).
// Only the thin 64 bit little endian files that lld writes for arm64 (and x86-64) are read: the symbol table (LC_SYMTAB) gives the
// symbol's address and the segments (LC_SEGMENT_64) turn that into a place in the file.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace macho {

namespace detail {

inline uint32_t U32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
inline uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32; }

struct Segment {
    uint64_t vmaddr, filesize, fileoff;
};

}  // namespace detail

// The text of the symbol `name` (a data symbol: an array of chars, as `extern "C" const char X86GameCodeStamp[]`, which Mach-O names
// with a leading underscore). False with why (a sentence that goes after "the linked file") when the file is not a dylib that has it.
inline bool ReadExportedText(const std::filesystem::path& path, const char* name, std::string& out, std::string& why) {
    using namespace detail;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        why = "cannot be opened";
        return false;
    }
    auto read = [&](uint64_t at, size_t len) {
        std::vector<uint8_t> b(len);
        f.clear();
        f.seekg(std::streamoff(at));
        f.read(reinterpret_cast<char*>(b.data()), std::streamsize(len));
        b.resize(size_t(f.gcount() > 0 ? f.gcount() : 0));
        return b;
    };
    std::vector<uint8_t> head = read(0, 32);
    if (head.size() < 32 || U32(head.data()) != 0xFEEDFACFu) {  // MH_MAGIC_64
        why = "is not a 64 bit Mach-O file";
        return false;
    }
    const uint32_t ncmds = U32(&head[16]), sizeofcmds = U32(&head[20]);
    if (sizeofcmds > (16u << 20)) {
        why = "has an impossible header";
        return false;
    }
    std::vector<uint8_t> cmds = read(32, sizeofcmds);
    if (cmds.size() != sizeofcmds) {
        why = "is cut short";
        return false;
    }
    std::vector<Segment> segments;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    size_t at = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (at + 8 > cmds.size()) break;
        const uint32_t cmd = U32(&cmds[at]), size = U32(&cmds[at + 4]);
        if (size < 8 || at + size > cmds.size()) break;
        if (cmd == 0x19 && size >= 72)  // LC_SEGMENT_64: name[16], vmaddr, vmsize, fileoff, filesize
            segments.push_back({U64(&cmds[at + 24]), U64(&cmds[at + 48]), U64(&cmds[at + 40])});
        else if (cmd == 0x2 && size >= 24) {  // LC_SYMTAB: symoff, nsyms, stroff, strsize
            symoff = U32(&cmds[at + 8]);
            nsyms = U32(&cmds[at + 12]);
            stroff = U32(&cmds[at + 16]);
            strsize = U32(&cmds[at + 20]);
        }
        at += size;
    }
    if (!nsyms || !strsize) {
        why = "has no symbol table";
        return false;
    }
    if (strsize > (256u << 20) || nsyms > (64u << 20)) {
        why = "has an impossible symbol table";
        return false;
    }
    std::vector<uint8_t> strings = read(stroff, strsize), symbols = read(symoff, size_t(nsyms) * 16);
    if (strings.size() != strsize || symbols.size() != size_t(nsyms) * 16) {
        why = "has a symbol table that is cut short";
        return false;
    }
    const std::string want = std::string("_") + name;
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t* sym = &symbols[size_t(i) * 16];  // n_strx, n_type, n_sect, n_desc, n_value
        const uint32_t strx = U32(sym);
        if ((sym[4] & 0x0E) != 0x0E || strx >= strsize) continue;  // N_SECT: defined in a section
        const char* sname = reinterpret_cast<const char*>(&strings[strx]);
        size_t n = 0;
        while (n < strsize - strx && sname[n]) n++;
        if (n != want.size() || std::memcmp(sname, want.data(), n) != 0) continue;
        const uint64_t value = U64(sym + 8);
        for (const Segment& s : segments) {
            if (value < s.vmaddr || value >= s.vmaddr + s.filesize) continue;
            std::vector<uint8_t> text = read(s.fileoff + (value - s.vmaddr), 512);
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
    why = std::string("does not export ") + name;
    return false;
}

}  // namespace macho
