// The first start's own build code (sdk/runtime/app/game_build.cpp: the steps, the object reuse, the progress, the stamp check)
// run on this Mac over a folder laid out as the release zip is, with this Mac's programs standing in for the Windows ones under
// the names the zip gives them (compiler/clang.exe is this Mac's clang, and so on). Only starting programs differs from the
// Windows executable: a POSIX runner here, CreateProcess there. It reads the disc image through kernel::Disc, as the executable does.
// tools/sim_first_start.sh builds it and drives it.
//
//   first_start_check --app <folder with compiler/> --data <data folder> --game <disc image or folder with Default.xbe>
//       [--slots N] [--stop-after N] [--abi <token>] [--pictures N] [--check] [--print-stamp] [--sources <folder>]
//   --check        only say whether the DLL is current (exit 0 current, 1 not)
//   --stop-after   cancel after N files are compiled (a closed window)
//   --abi          pretend the executable has another ABI token (an update)
//   --pictures     pretend the executable's recipe for the achievement pictures and the icon is version N (default: the real one, 1);
//                  0 makes none. A start whose DLL is current makes them when they are not made, as the executable does.
//   --print-stamp  print the stamp the executable expects, as built from the headers here, and exit
//   --sources      compile and link the sources in <folder> (tools/regen.sh's generated/game) and nothing else, with no disc: the build
//                  of the game's code for a macOS host (sdk/host/build.sh --game-code-dylib, which compiles this with its own
//                  game_code_flags.h, that of a Mach-O dylib). --game is not needed.
// Prints what --prepare prints ("progress <0 to 1> <text>" lines) on stdout and "error: <step>: <reason>" on stderr.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>

#include "app/game_build.h"
#include "app/game_code_recipe.h"
#include "app/posix_runner.h"
#include "kernel/disc.h"
#include "xbe/image_hash.h"

namespace {

namespace fs = std::filesystem;

// what FindGame does with the disc: the xbe must be this program's release; the disc then gives final.map
bool ReadDisc(const fs::path& game, gamebuild::Inputs& in, std::string& why) {
    std::error_code ec;
    std::unique_ptr<kernel::Disc> disc;
    if (fs::is_directory(game, ec)) {
        auto read = [&](const char* name, std::vector<uint8_t>& out) {
            std::ifstream f(game / name, std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(f), {});
            return bool(f);
        };
        if (!read(IMAGE_XBE, in.xbe) || !read("final.map", in.map)) {
            why = "the folder has no " IMAGE_XBE " and final.map";
            return false;
        }
    } else {
        disc = std::make_unique<kernel::Disc>();
        if (!disc->Open(game, why)) return false;
        if (!disc->Load(IMAGE_XBE, in.xbe)) {
            why = "the disc has no " IMAGE_XBE;
            return false;
        }
        if (!disc->Load("final.map", in.map)) {
            why = "the disc has no final.map";
            return false;
        }
    }
    if (xbe::SectionsHash(in.xbe) != IMAGE_SECTIONS_HASH) {
        why = "that is not the release this program was made for";
        return false;
    }
    return true;
}

// the two disc files the pictures are made from (what app/game_code.cpp's ReadPictureFiles does): a disc, or a folder of extracted files
bool ReadPictureFiles(const fs::path& game, gamebuild::PictureInputs& in, std::string& why) {
    std::error_code ec;
    std::unique_ptr<kernel::Disc> disc;
    if (!fs::is_directory(game, ec)) {
        disc = std::make_unique<kernel::Disc>();
        if (!disc->Open(game, why)) return false;
    }
    struct File {
        const char* disc_path;
        const char* folder_path;
        std::vector<uint8_t>& into;
    };
    for (const File& file : {File{"interfc\\levelThumbnails.mtx", "interfc/levelThumbnails.mtx", in.thumbnails},
                             File{"media\\launch_buffy.xpr", "media/launch_buffy.xpr", in.launch}}) {
        if (disc) {
            if (!disc->Load(file.disc_path, file.into)) {
                why = std::string("the disc image has no ") + file.disc_path;
                return false;
            }
            continue;
        }
        std::ifstream f(game / file.folder_path, std::ios::binary);
        if (!f) {
            why = game.string() + " has no " + file.folder_path;
            return false;
        }
        file.into.assign(std::istreambuf_iterator<char>(f), {});
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path app, data, game, sources;
    size_t slots = 4;
    long stop_after = -1;
    bool check_only = false, print_stamp = false;
    std::string abi;
    int pictures = -1;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--app") app = next();
        else if (a == "--data") data = next();
        else if (a == "--game") game = next();
        else if (a == "--sources") sources = next();
        else if (a == "--slots") slots = size_t(std::atol(next().c_str()));
        else if (a == "--stop-after") stop_after = std::atol(next().c_str());
        else if (a == "--abi") abi = next();
        else if (a == "--pictures") pictures = std::atoi(next().c_str());
        else if (a == "--check") check_only = true;
        else if (a == "--print-stamp") print_stamp = true;
    }
    if (print_stamp) {
        std::printf("%s\n", gamecode::Stamp().c_str());
        return 0;
    }
    if (app.empty() || data.empty() || (game.empty() && sources.empty())) {
        std::fprintf(stderr, "usage: first_start_check --app <folder> --data <folder> --game <disc image> [--slots N] [--stop-after N] [--abi token] [--check] [--sources folder]\n");
        return 2;
    }
    gamebuild::Layout layout{fs::absolute(app), fs::absolute(data), fs::absolute(data) / "logs"};
    gamebuild::Recipe recipe = gamecode::Recipe(slots);
    if (pictures >= 0) recipe.pictures = pictures;
    if (!sources.empty()) recipe.pictures = 0;  // no disc: no pictures
    gamebuild::ReadPictures read_pictures = [&](gamebuild::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); };
    if (!abi.empty()) {  // an update that changed the ABI: the stamp and the compile's token say so
        auto swap = [&](std::string& s) {
            size_t at = s.find(gamecode::AbiToken());
            while (at != std::string::npos) {
                s.replace(at, std::strlen(gamecode::AbiToken()), abi);
                at = s.find(gamecode::AbiToken(), at + abi.size());
            }
        };
        swap(recipe.stamp);
        swap(recipe.compile_flags);
    }
    fs::path dll = layout.Build() / recipe.library;
    std::string found;
    gamebuild::DllState state = gamebuild::CheckDll(dll, recipe.stamp, &found, recipe.target);
    const char* names[] = {"missing", "current", "stale", "unreadable"};
    std::printf("dll: %s (%s)\n", names[int(state)], found.c_str());
    if (state == gamebuild::DllState::Current) {
        if (!check_only) {  // as the executable does: the DLL being current says nothing of the pictures
            if (gamebuild::PicturesCurrent(layout, recipe)) {
                std::printf("pictures: current\n");
            } else {
                gamebuild::PosixRunner runner(layout.Log(), true);
                std::printf(gamebuild::MakePictures(layout, recipe, read_pictures, runner) ? "pictures: made\n" : "pictures: not made\n");
            }
        }
        std::printf("progress 1.000 The game's code is up to date\n");
        return 0;
    }
    if (check_only) return 1;
    if (std::string blocker = gamebuild::Blocker(layout, recipe, sources.empty()); !blocker.empty()) {
        std::fprintf(stderr, "error: %s\n", blocker.c_str());
        return 9;
    }
    gamebuild::PosixRunner runner(layout.Log());
    gamebuild::Progress progress;
    std::string last_text;
    int last_percent = -1;
    progress.changed = [&] {
        int percent = int(progress.Fraction() * 100.0);
        std::string text = progress.Text();
        if (percent != last_percent || text != last_text) {
            std::printf("progress %.3f %s\n", progress.Fraction(), text.c_str());
            std::fflush(stdout);
            last_percent = percent;
            last_text = text;
        }
        if (stop_after >= 0 && progress.stage == gamebuild::Stage::Compiling && long(progress.files_done.load()) >= stop_after)
            progress.cancel = true;
    };
    gamebuild::Failure failure;
    gamebuild::Outcome outcome =
        sources.empty()
            ? gamebuild::Build(
                  layout, recipe, [&](gamebuild::Inputs& in, std::string& why) { return ReadDisc(game, in, why); }, read_pictures, runner, progress, failure)
            : gamebuild::BuildFromSources(layout, recipe, fs::absolute(sources), runner, progress, failure);
    if (outcome == gamebuild::Outcome::Built) {
        std::printf("progress 1.000 Done\n");
        return 0;
    }
    if (outcome == gamebuild::Outcome::Cancelled) {
        std::fprintf(stderr, "cancelled after %u files\n", progress.files_done.load());
        return 3;
    }
    std::fprintf(stderr, "error: %s\n", failure.Line().c_str());
    if (!failure.hint.empty()) std::fprintf(stderr, "hint: %s\n", failure.hint.c_str());
    std::string tail = gamebuild::LogTail(layout.Log(), 600);
    std::fprintf(stderr, "log tail:\n%s\n", tail.c_str());
    return 9;
}
