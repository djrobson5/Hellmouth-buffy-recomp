#pragma once
// Building the game's code on the player's PC (XBE_GAME_CODE_DLL), from their own disc: the steps, their order, what is reused
// and how far along it is. No Windows, window or runtime calls in here, so the same code can be run on another machine over
// the same folders (tools/first_start_check.cpp does, on a Mac, with that machine's compiler); starting programs is the
// caller's (Runner), and so are the window and the questions (app/game_code.cpp).
//
// What it runs, with <compiler> the folder of the tools beside the executable and <work> the work folder:
//   buffy_config.exe   final.map + Default.xbe -> game.toml                           (the config the player's disc gives)
//   xbe_recomp.exe     game.toml + Default.xbe -> C++ sources, exports.def, imports.def
//   buffy_config.exe   --pictures: two of the disc's files -> the achievement pictures and the icon, in <data> (not part of the DLL:
//                      made on their own marker, so that a start with a current DLL still makes them; see MakePictures)
//   clang.exe          each source -> .obj, the biggest first, as many at once as the PC has room for
//   lld-link.exe       the objects + <compiler>\<library>.lib (the executable's import library) -> <library>
// then the DLL's stamp is read and must be the one the executable expects, and the DLL is moved into place.
// On macOS (Recipe::target MachO) the tools have no .exe and the link is a dylib's (app/macho_export.h reads the stamp):
//   clang              each source -> .o (arm64)
//   ld64.lld           the objects + alias.rsp (WriteAliasResponse) -> <library>.dylib, dynamic_lookup binding the names the executable
//                      has (the runtime, its overrides) when the dylib is loaded; no executable import library to link against
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gamebuild {

namespace fs = std::filesystem;
using Str = fs::path::string_type;  // wide on Windows, UTF-8 elsewhere: what a program is started with

// how programs are started and waited for: a Windows one in app/game_code.cpp, a POSIX one in tools/first_start_check.cpp
class Runner {
public:
    virtual ~Runner() = default;
    // starts a program (its full path, and its arguments without the program) in a folder. Its output and errors go to the
    // build log. Returns an id above 0, or 0 and why it could not be started.
    virtual int Start(const fs::path& program, const std::vector<Str>& args, const fs::path& cwd, std::string& why) = 0;
    // waits up to ms milliseconds for any started program to end: its id and exit code (a Windows one's, or 128 + the signal on
    // another system), id 0 when none did
    struct Ended {
        int id = 0;
        uint32_t code = 0;
    };
    virtual Ended Wait(int ms) = 0;
    virtual void KillAll() = 0;                      // ends every program still running
    virtual void Note(const std::string& line) = 0;  // a line of UTF-8 in the build log
};

// where everything is
struct Layout {
    fs::path app;   // the folder of the executable: <app>\compiler holds the tools
    fs::path data;  // the data folder
    fs::path logs;  // the log folder: <data>\logs, or where --log puts it
    fs::path Compiler() const { return app / "compiler"; }
    fs::path Build() const { return data / "build"; }                  // the finished DLL
    fs::path Work() const { return data / "cache" / "game-code"; }     // sources and objects: about 300 MB, kept for a quicker rebuild
    fs::path Log() const { return logs / "compile.log"; }
    fs::path PicturesScratch() const { return data / "cache" / "pictures"; }  // the two disc files while the pictures are made
    fs::path PicturesMarker() const { return data / "pictures.stamp"; }       // written last: the pictures are made, by this recipe
    fs::path Tool(const char* name) const { return Compiler() / (std::string(name) + ".exe"); }
};

// what the game's code is built into: a PE DLL (Windows; lld-link) or a Mach-O dylib (macOS; ld64.lld)
enum class Target { Pe, MachO };

// what the executable knows about its game code
struct Recipe {
    Target target = Target::Pe;
    std::string library;        // the DLL's name, Buffy.dll (Buffy.dylib for MachO)
    std::string config_library; // what game.toml's `library` says, when that is not `library` (Buffy.dll, which the recompiler's exports.def and the dylib share)
    std::string stamp;          // what the DLL's X86GameCodeStamp must say
    std::string image_stamp;    // the part of it that comes from the image ("image=<hash> symbols=<hash>"), which the sources must say too
    std::string compile_flags;  // for clang, space separated (game_code_flags.h)
    std::string link_flags;     // for lld-link (ld64.lld, without the install name, response file and output)
    size_t slots = 1;           // compiles at once
    int pictures = 0;           // the version of the recipe for the pictures and the icon (game_code_recipe.h); 0 when the game has none
};

enum class Stage { Reading, Config, Translating, Pictures, Planning, Compiling, Linking, Checking, Done };

// how far along the build is, written by the build and read by whatever shows it; changed (if set) is called after each change,
// from the build's thread
struct Progress {
    std::atomic<Stage> stage{Stage::Reading};
    std::atomic<uint64_t> bytes_done{0}, bytes_total{0};  // of the sources being compiled
    std::atomic<uint32_t> files_done{0}, files_total{0};
    std::atomic<uint32_t> first_wave{1};                   // files compiled before the time left means anything
    std::atomic<int64_t> compile_start_ms{0};              // steady clock
    std::atomic<bool> cancel{false};
    std::function<void()> changed;
    // 0 to 1. The stages that do not report in between count for what they take: the compiles take nearly all the time, so
    // they count by the bytes of source done, as the big files take longer
    double Fraction() const;
    std::string Text() const;  // "Compiling the game's code, file 12 of 129"
};

// what the player's disc gives, as the first start reads it
struct Inputs {
    std::vector<uint8_t> xbe;  // Default.xbe
    std::vector<uint8_t> map;  // final.map
};
using ReadInputs = std::function<bool(Inputs& inputs, std::string& why)>;

// and what the pictures are made from (interfc/levelThumbnails.mtx and media/launch_buffy.xpr), read apart from the rest because a disc
// that lacks them still builds the game: the pictures are made when the files are there, and are never what stops a start
struct PictureInputs {
    std::vector<uint8_t> thumbnails;  // interfc/levelThumbnails.mtx
    std::vector<uint8_t> launch;      // media/launch_buffy.xpr
};
using ReadPictures = std::function<bool(PictureInputs& inputs, std::string& why)>;

// where it went wrong, for the player and for the log
struct Failure {
    std::string step;    // "Compiling x86_recomp.17.cpp"
    std::string reason;  // one line: what happened
    std::string hint;    // what to try, when something is known; may be several sentences
    std::string Line() const { return step + ": " + reason; }
};

enum class Outcome { Built, Failed, Cancelled };

// What stops a build before it starts, for the player to fix; empty when nothing does: tools missing, no writing, no room.
// `translate` false: the sources are given (BuildFromSources), so the tools that make them are not needed.
std::string Blocker(const Layout& layout, const Recipe& recipe, bool translate = true);

// The achievement pictures and the icon (<data>\achievements\<id>.png, <data>\Buffy.png and Buffy.ico) are made from the disc by
// `buffy_config --pictures`. They have a marker of their own, <data>\pictures.stamp, which holds the recipe's version and is written when
// all of it is there: a start whose DLL is current but whose pictures are not made (or were made by an older recipe) makes them, in the
// build (after the translation) and on their own (EnsurePictures). A picture that cannot be made is never a failure: a line in the
// log says why (and the next start tries again).
bool PicturesCurrent(const Layout& layout, const Recipe& recipe);
// makes them now, whatever the marker says; true when they were made. `read` is called for the disc's two files.
bool MakePictures(const Layout& layout, const Recipe& recipe, const ReadPictures& read, Runner& run);
// makes them when the marker says they are not there
inline bool EnsurePictures(const Layout& layout, const Recipe& recipe, const ReadPictures& read, Runner& run) {
    return PicturesCurrent(layout, recipe) || MakePictures(layout, recipe, read, run);
}

// Runs the build. Objects whose source, flags and compiler are what they were are reused, so a closed window or a failure
// loses only the files in progress (the sources are made again each time and compared by content: the recompiler writes every
// file). Writes <Build>\<library> and <Build>\<library>.stamp when it succeeds, and a line of each step and command to the log, with the
// time each phase took ("time: translate 5300 ms": disc read, config, translate, pictures, plan, compile (all the files, with how many were
// compiled), link, check and install) and "time: total" at the end, whatever the outcome. The pictures are made after the translation if
// they are not there (`pictures` may be empty: none are made).
Outcome Build(const Layout& layout, const Recipe& recipe, const ReadInputs& read, const ReadPictures& pictures, Runner& run, Progress& progress,
              Failure& failure);

// The compile, link and install steps alone, over the sources of a recompile that is already made (the folder that tools/regen.sh
// fills: x86_func_mapping.cpp, x86_recomp.<n>.cpp, x86_recomp_shared.h, image_symbols.h, exports.def). Nothing is read from a disc and
// nothing is written to `sources`: the objects, the response file and the log go to <Work>, as the steps of Build do. For development
// (sdk/host/build.sh --game-code-dylib), where the game's code is built without the first start's translation.
Outcome BuildFromSources(const Layout& layout, const Recipe& recipe, const fs::path& sources, Runner& run, Progress& progress, Failure& failure);

// the DLL at <Build>\<library> against the stamp it must say
enum class DllState { Missing, Current, Stale, Unreadable };
DllState CheckDll(const fs::path& dll, const std::string& stamp, std::string* found = nullptr, Target target = Target::Pe);

// macOS: the response file for the dylib's link. In a Mach-O dylib a weak definition always beats the executable's override, so the
// generated code defines no weak name (X86_WEAK_FUNC in cpu/x86_ops.h expands to nothing there) and the link gives each recompiled
// function its name as an alias of its body instead: "-alias ___imp__sub_X _sub_X", for every `__imp__sub_X` that exports.def lists
// except the names the executable overrides (one name a line in `overrides`, as sdk/host/list_overrides.sh writes it from the built
// executable). A function that is overridden keeps its name undefined in the dylib, which binds to the executable when it is loaded.
// (44 thousand lines: on the command line they would pass the argument limit.)
struct AliasStats {
    size_t listed = 0;      // recompiled functions in exports.def
    size_t overridden = 0;  // of those, the ones the executable overrides
    size_t aliased = 0;     // lines written
    size_t unused = 0;      // overrides that exports.def does not list (names this image lacks)
};
bool WriteAliasResponse(const fs::path& exports_def, const fs::path& overrides, const fs::path& out, AliasStats& stats, std::string& why);

// the last bytes of a text file, from a whole line: where a tool's error is
std::string LogTail(const fs::path& log, size_t bytes);

// how a tool's exit code reads: the Windows ones a missing or blocked program gives (0xC0000135 a DLL it needs is not there,
// 0xC0000142 it could not start, 0xC000007B a 32 bit program, 0xC0000409 a crash), as text, "exit code 1" for another
std::string ExitText(uint32_t code);

}  // namespace gamebuild
