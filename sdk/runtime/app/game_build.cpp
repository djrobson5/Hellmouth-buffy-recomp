#include "app/game_build.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <thread>

#include "app/pe_export.h"
#include "kernel/crypto.h"

namespace gamebuild {

namespace {

constexpr uint32_t kDllNotFound = 0xC0000135, kInitFailed = 0xC0000142, kBadImage = 0xC000007B, kAccessDenied = 0xC0000022;

using Clock = std::chrono::steady_clock;

int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

// the program names, flags and file names handed to a program are ASCII; paths go through native()
Str A(const std::string& ascii) {
    Str s;
    s.reserve(ascii.size());
    for (char c : ascii) s.push_back(static_cast<Str::value_type>(static_cast<unsigned char>(c)));
    return s;
}

std::string U8(const fs::path& path) {
    auto s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::vector<std::string> Split(const std::string& flags) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < flags.size()) {
        size_t end = flags.find(' ', at);
        if (end == std::string::npos) end = flags.size();
        if (end > at) out.push_back(flags.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

std::string Sha1Hex(const uint8_t digest[20]) {
    std::string hex;
    for (int i = 0; i < 20; i++) {
        hex += "0123456789abcdef"[digest[i] >> 4];
        hex += "0123456789abcdef"[digest[i] & 15];
    }
    return hex;
}

// a hash of a text and then a file's bytes, to tell whether an object is what that source and those flags make
bool KeyOf(const std::string& salt, const fs::path& source, std::string& key) {
    std::ifstream f(source, std::ios::binary);
    if (!f) return false;
    alignas(8) uint8_t ctx[128];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, reinterpret_cast<const uint8_t*>(salt.data()), salt.size());
    std::vector<uint8_t> chunk(1 << 20);
    for (;;) {
        f.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(chunk.size()));
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        crypto::ShaUpdate(ctx, chunk.data(), size_t(n));
    }
    uint8_t digest[20];
    crypto::ShaFinal(ctx, digest);
    key = Sha1Hex(digest);
    return !f.bad();
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), std::streamsize(size));
    f.flush();
    return bool(f);
}

std::string ReadText(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

uint64_t SizeOf(const fs::path& p) {
    std::error_code ec;
    uint64_t n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

// the number after `key =` on a line of a TOML file that starts with it; false when there is none
bool ConfigNumber(const std::string& text, const char* key, uint32_t& value) {
    size_t at = 0;
    const size_t n = std::strlen(key);
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, n, key) == 0) {
            size_t eq = text.find('=', at + n);
            if (eq != std::string::npos && eq < end && text.find_first_not_of(" \t", at + n) == eq) {
                value = uint32_t(std::strtoul(text.c_str() + eq + 1, nullptr, 10));
                return true;
            }
        }
        at = end + 1;
    }
    return false;
}

// the string a `library = "..."` line holds
std::string ConfigLibrary(const std::string& text) {
    size_t at = text.find("\nlibrary");
    if (at == std::string::npos) return {};
    size_t q1 = text.find('"', at), nl = text.find('\n', at + 1);
    if (q1 == std::string::npos || q1 > nl) return {};
    size_t q2 = text.find('"', q1 + 1);
    return q2 == std::string::npos ? std::string() : text.substr(q1 + 1, q2 - q1 - 1);
}

// `#define IMAGE_CODE_STAMP "..."` in the recompiler's image_symbols.h
std::string ImageStampOf(const std::string& text) {
    size_t at = text.find("#define IMAGE_CODE_STAMP \"");
    if (at == std::string::npos) return {};
    at = text.find('"', at);
    size_t end = text.find('"', at + 1);
    return end == std::string::npos ? std::string() : text.substr(at + 1, end - at - 1);
}

// what to try after a tool failed: antivirus and a long path are the two causes the first starts of other releases met
std::string HintFor(const Layout& layout, bool did_not_start, uint32_t code, bool vanished) {
    std::string hint;
    if (did_not_start || vanished || code == kInitFailed || code == kAccessDenied || code == 0xC0000005)
        hint = "Antivirus software may have blocked or removed a file of the game. Allow the game's folder in it (or extract the "
               "zip again) and start the game again: what is done so far is kept. ";
    else if (code == kDllNotFound || code == kBadImage)
        hint = "A file beside the compiler programs is missing or damaged. Extract the whole zip again, then start the game. ";
    if (layout.Work().native().size() > 110)
        hint += "The game's folder has a long path. Move the game to a short one, such as C:\\Hellmouth, and start it again. ";
    if (!hint.empty() && hint.back() == ' ') hint.pop_back();
    return hint;
}

std::string Ms(int64_t ms) { return std::to_string(ms) + " ms"; }

// a command as the log shows it
std::string Command(const fs::path& program, const std::vector<Str>& args) {
    std::string line = U8(program);
    for (const Str& a : args) {
        fs::path p(a);
        line += ' ' + U8(p);
    }
    return line;
}

enum class Ran { Ok, NotStarted, Failed, Cancelled };

// one program, waited for: the line "run: <command>", then "exit: <what> after N ms". `code` is its exit code; `why` says what stopped one
// that did not start. Cancelled when `cancel` was set (everything running is ended).
Ran RunProgram(Runner& run, const fs::path& program, const std::vector<Str>& args, const fs::path& cwd, const std::atomic<bool>& cancel, uint32_t& code,
               std::string& why) {
    run.Note("run: " + Command(program, args));
    int64_t began = NowMs();
    int id = run.Start(program, args, cwd, why);
    if (!id) return Ran::NotStarted;
    for (;;) {
        Runner::Ended ended = run.Wait(250);
        if (cancel) {
            run.KillAll();
            return Ran::Cancelled;
        }
        if (!ended.id) continue;
        if (ended.id != id) continue;  // not ours: nothing else runs now
        run.Note("exit: " + ExitText(ended.code) + " after " + std::to_string(NowMs() - began) + " ms");
        code = ended.code;
        return ended.code == 0 ? Ran::Ok : Ran::Failed;
    }
}

// the pictures, made: the two disc files into the scratch folder, `buffy_config --pictures` over them into <data>, and the marker last. Never a
// failure of the build: what went wrong is a line in the log. False also when `cancel` ended it.
bool MakePicturesImpl(const Layout& layout, const Recipe& recipe, const ReadPictures& read, Runner& run, const std::atomic<bool>& cancel) {
    if (recipe.pictures <= 0) return true;
    const fs::path scratch = layout.PicturesScratch();
    std::error_code ec;
    auto finish = [&](bool made, const std::string& note) {
        fs::remove_all(scratch, ec);
        run.Note("pictures: " + note);
        return made;
    };
    run.Note("pictures: making the achievement pictures and the icon in " + U8(layout.data) + " (recipe " + std::to_string(recipe.pictures) + ")");
    // what is there is not whole until this finishes, so the marker goes first; one that cannot be removed would go on saying "made" over files being replaced
    fs::remove(layout.PicturesMarker(), ec);
    if (ec && Exists(layout.PicturesMarker())) return finish(false, "not made: cannot remove the marker " + U8(layout.PicturesMarker()) + ": " + ec.message());
    ec.clear();
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    if (ec) return finish(false, "not made: cannot make " + U8(scratch) + ": " + ec.message());
    PictureInputs in;
    std::string why;
    if (!read || !read(in, why)) return finish(false, "not made: " + (why.empty() ? std::string("the disc's files could not be read") : why));
    if (!WriteFile(scratch / "levelThumbnails.mtx", in.thumbnails.data(), in.thumbnails.size()) ||
        !WriteFile(scratch / "launch_buffy.xpr", in.launch.data(), in.launch.size()))
        return finish(false, "not made: cannot write into " + U8(scratch));
    fs::create_directories(layout.data, ec);
    uint32_t code = 0;
    switch (RunProgram(run, layout.Tool("buffy_config"), {A("--pictures"), A("levelThumbnails.mtx"), A("launch_buffy.xpr"), layout.data.native()},
                       scratch, cancel, code, why)) {
        case Ran::Ok:
            break;
        case Ran::Cancelled:
            return finish(false, "stopped");
        case Ran::NotStarted:
            return finish(false, "not made: buffy_config.exe could not be started (" + why + ")");
        case Ran::Failed:
            return finish(false, "not made: buffy_config.exe failed (" + ExitText(code) + ")");
    }
    for (const char* name : {"Buffy.ico", "Buffy.png"})
        if (!Exists(layout.data / name)) return finish(false, std::string("not made: buffy_config.exe wrote no ") + name);
    const std::string marker = "pictures " + std::to_string(recipe.pictures) + "\n";
    if (!WriteFile(layout.PicturesMarker(), marker.data(), marker.size()))
        return finish(false, "not made: cannot write " + U8(layout.PicturesMarker()));
    return finish(true, "made");
}

// A step that runs programs; the build's state is all in here so that the steps read in order
class Builder {
public:
    Builder(const Layout& layout, const Recipe& recipe, Runner& run, Progress& progress, Failure& failure)
        : layout_(layout), recipe_(recipe), run_(run), progress_(progress), failure_(failure), work_(layout.Work()) {}

    Outcome Run(const ReadInputs& read, const ReadPictures& pictures);

private:
    Outcome RunSteps(const ReadInputs& read, const ReadPictures& pictures);
    // a step, and a line in the log for the time it took ("time: translate 5300 ms"); the detail follows the time
    template <class Step>
    bool Timed(const char* phase, Step&& step, const std::function<std::string()>& detail = {}) {
        const int64_t began = NowMs();
        const bool ok = step();
        run_.Note(std::string("time: ") + phase + " " + Ms(NowMs() - began) + (detail ? detail() : std::string()));
        return ok;
    }

    void SetStage(gamebuild::Stage stage) {
        progress_.stage = stage;
        Changed();
    }
    void Changed() {
        if (progress_.changed) progress_.changed();
    }
    bool Fail(std::string step, std::string reason, std::string hint = {}) {
        failure_ = {std::move(step), std::move(reason), std::move(hint)};
        run_.Note("FAILED: " + failure_.Line());
        if (!failure_.hint.empty()) run_.Note("  " + failure_.hint);
        return false;
    }

    // one program, waited for: its exit code; false (with the failure set) when it could not be started or ended badly
    bool RunTool(const std::string& step, const char* tool, const std::vector<Str>& args, const fs::path& cwd);
    bool Prepare(const ReadInputs& read);
    bool Config();
    bool Translate();
    bool Pictures(const ReadPictures& read);
    bool Plan();
    void RemoveTemps();
    bool Compile();
    bool Link();
    bool Install();

    struct Job {
        std::string name;  // x86_recomp.12
        fs::path source, object, key_file;
        std::string key;
        uint64_t bytes = 0;
    };

    const Layout& layout_;
    const Recipe& recipe_;
    Runner& run_;
    Progress& progress_;
    Failure& failure_;
    fs::path work_;
    uint32_t partitions_ = 0;
    std::vector<Job> jobs_;  // the files to compile
    std::vector<std::string> all_;  // every source's name, in the order they are linked
    std::string salt_;       // what an object's key holds besides its source
};

bool Builder::RunTool(const std::string& step, const char* tool, const std::vector<Str>& args, const fs::path& cwd) {
    std::string why;
    uint32_t code = 0;
    switch (RunProgram(run_, layout_.Tool(tool), args, cwd, progress_.cancel, code, why)) {
        case Ran::Ok:
            return true;
        case Ran::Cancelled:
            return false;
        case Ran::NotStarted:
            return Fail(step, std::string(tool) + ".exe could not be started (" + why + ")", HintFor(layout_, true, 0, false));
        case Ran::Failed:
            break;
    }
    return Fail(step, std::string(tool) + ".exe failed (" + ExitText(code) + ")", HintFor(layout_, false, code, false));
}

bool Builder::Prepare(const ReadInputs& read) {
    SetStage(Stage::Reading);
    std::error_code ec;
    fs::create_directories(work_, ec);
    fs::create_directories(layout_.Build(), ec);
    if (ec) return Fail("Preparing the folders", "cannot make " + U8(work_) + ": " + ec.message());
    Inputs inputs;
    std::string why;
    if (!read(inputs, why)) return Fail("Reading the game from your disc", why);
    if (!WriteFile(work_ / "Default.xbe", inputs.xbe.data(), inputs.xbe.size()) ||
        !WriteFile(work_ / "final.map", inputs.map.data(), inputs.map.size()))
        return Fail("Writing the game's files", "cannot write into " + U8(work_), HintFor(layout_, true, 0, false));
    run_.Note("disc: Default.xbe " + std::to_string(inputs.xbe.size()) + " bytes, final.map " + std::to_string(inputs.map.size()) + " bytes");
    return true;
}

bool Builder::Config() {
    SetStage(Stage::Config);
    std::error_code ec;
    fs::remove(work_ / "game.toml", ec);
    if (!RunTool("Making the game's configuration", "buffy_config",
                 {A("--map"), A("final.map"), A("--xbe"), A("Default.xbe"), A("--out"), A("game.toml")}, work_))
        return false;
    std::string toml = ReadText(work_ / "game.toml");
    if (toml.empty()) return Fail("Making the game's configuration", "buffy_config.exe wrote no game.toml", HintFor(layout_, false, 0, true));
    if (!ConfigNumber(toml, "partitions", partitions_) || partitions_ == 0 || partitions_ > 4096)
        return Fail("Making the game's configuration", "game.toml has no partition count");
    if (ConfigLibrary("\n" + toml) != recipe_.library)
        return Fail("Making the game's configuration", "game.toml names the DLL \"" + ConfigLibrary("\n" + toml) + "\", not " + recipe_.library);
    return true;
}

bool Builder::Translate() {
    SetStage(Stage::Translating);
    // the sources are made again, whole: none of an earlier run's is left to be taken for this one's
    std::error_code ec;
    std::vector<fs::path> old;
    for (fs::directory_iterator it(work_, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = U8(it->path().filename());
        if (name.rfind("x86_recomp.", 0) == 0 && it->path().extension() == ".cpp") old.push_back(it->path());
    }
    for (const fs::path& f : old) fs::remove(f, ec);
    for (const char* f : {"x86_func_mapping.cpp", "x86_recomp_shared.h", "image_symbols.h", "exports.def", "imports.def"}) fs::remove(work_ / f, ec);
    if (!RunTool("Translating the game's code", "xbe_recomp",
                 {A("game.toml"), A("Default.xbe"), work_.native(), A("--xdk"), (layout_.Compiler() / "xdk").native()}, work_))
        return false;
    all_ = {"x86_func_mapping"};
    for (uint32_t i = 0; i < partitions_; i++) all_.push_back("x86_recomp." + std::to_string(i));
    for (const std::string& name : all_)
        if (!Exists(work_ / (name + ".cpp")))
            return Fail("Translating the game's code", "xbe_recomp.exe did not write " + name + ".cpp", HintFor(layout_, false, 0, true));
    for (const char* f : {"x86_recomp_shared.h", "image_symbols.h", "exports.def", "imports.def"})
        if (!Exists(work_ / f)) return Fail("Translating the game's code", std::string("xbe_recomp.exe did not write ") + f, HintFor(layout_, false, 0, true));
    // the image the disc gave must be the one this executable was made for, or the DLL would be refused after all that compiling
    std::string image = ImageStampOf(ReadText(work_ / "image_symbols.h"));
    if (image != recipe_.image_stamp) {
        run_.Note("image: the sources say \"" + image + "\", this program expects \"" + recipe_.image_stamp + "\"");
        return Fail("Translating the game's code", "the game on the disc is not the release this program was made for",
                    "Use the US release's disc image. If it is, this program and the compiler files may not belong together: extract the whole zip "
                    "again, and if it still fails keep logs\\compile.log (it says what the sources claimed and what was expected).");
    }
    return true;
}

// the achievement pictures and the icon, when they are not made yet: never a failure, only a cancel stops the build here
bool Builder::Pictures(const ReadPictures& read) {
    SetStage(Stage::Pictures);
    if (!read) {
        run_.Note("pictures: none to make");
        return true;
    }
    if (PicturesCurrent(layout_, recipe_)) {
        run_.Note("pictures: already made");
        return true;
    }
    MakePicturesImpl(layout_, recipe_, read, run_, progress_.cancel);
    return !progress_.cancel;
}

// what a compile that was ended left: its own output beside the object's name, and the compiler's scratch file for it
void Builder::RemoveTemps() {
    std::error_code ec;
    std::vector<fs::path> temps;
    for (fs::directory_iterator it(work_, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".tmp") temps.push_back(it->path());
    for (const fs::path& t : temps) fs::remove(t, ec);
}

bool Builder::Plan() {
    SetStage(Stage::Planning);
    RemoveTemps();
    // an object is reused when its source, the flags it was compiled with (the ABI token is one of them) and the compiler are
    // what they were
    salt_ = recipe_.compile_flags + "\nclang " + std::to_string(SizeOf(layout_.Tool("clang"))) + "\n";
    jobs_.clear();
    progress_.bytes_total = 0;
    uint64_t reused = 0;
    for (const std::string& name : all_) {
        Job job;
        job.name = name;
        job.source = work_ / (name + ".cpp");
        job.object = work_ / (name + ".obj");
        job.key_file = work_ / (name + ".obj.key");
        job.bytes = SizeOf(job.source);
        if (!KeyOf(salt_, job.source, job.key)) return Fail("Checking the sources", "cannot read " + name + ".cpp");
        if (progress_.cancel) return false;
        if (Exists(job.object) && SizeOf(job.object) > 0 && ReadText(job.key_file) == job.key + "\n") {
            reused++;
            continue;
        }
        jobs_.push_back(std::move(job));
    }
    // the biggest first, so that the last files to finish are small ones
    std::sort(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) { return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name; });
    for (const Job& j : jobs_) progress_.bytes_total += j.bytes;
    progress_.files_total = uint32_t(jobs_.size());
    progress_.files_done = 0;
    progress_.bytes_done = 0;
    run_.Note("compile: " + std::to_string(jobs_.size()) + " files to compile, " + std::to_string(reused) + " reused from an earlier start");
    return true;
}

bool Builder::Compile() {
    SetStage(Stage::Compiling);
    const size_t slots = std::max<size_t>(recipe_.slots, 1);
    progress_.first_wave = uint32_t(std::min<size_t>(slots, jobs_.size()));
    progress_.compile_start_ms = NowMs();
    run_.Note("compile: " + std::to_string(slots) + " at a time");
    std::vector<std::string> flags = Split(recipe_.compile_flags);
    std::vector<Str> base;
    for (const std::string& f : flags) base.push_back(A(f));
    base.push_back(A("-isystem"));
    base.push_back((layout_.Compiler() / "include" / "freestanding").native());
    base.push_back(A("-I") + (layout_.Compiler() / "include").native());
    base.push_back(A("-c"));

    std::map<int, const Job*> running;
    size_t next = 0;
    auto stop = [&]() {
        run_.KillAll();
        RemoveTemps();
    };
    while (next < jobs_.size() || !running.empty()) {
        while (running.size() < slots && next < jobs_.size()) {
            const Job& job = jobs_[next++];
            std::vector<Str> args = base;
            args.push_back(A(job.name + ".cpp"));
            args.push_back(A("-o"));
            args.push_back(A(job.name + ".obj.tmp"));
            std::string why;
            run_.Note("run: clang " + job.name + ".cpp");
            int id = run_.Start(layout_.Tool("clang"), args, work_, why);
            if (!id) {
                stop();
                return Fail("Compiling " + job.name + ".cpp", "clang.exe could not be started (" + why + ")", HintFor(layout_, true, 0, false));
            }
            running[id] = &job;
        }
        Runner::Ended ended = run_.Wait(250);
        if (progress_.cancel) {
            stop();
            return false;
        }
        if (!ended.id) continue;
        auto it = running.find(ended.id);
        if (it == running.end()) continue;
        const Job& job = *it->second;
        running.erase(it);
        if (ended.code != 0) {
            run_.Note("clang " + job.name + ".cpp: " + ExitText(ended.code));
            stop();
            return Fail("Compiling " + job.name + ".cpp", "clang.exe failed (" + ExitText(ended.code) + ")", HintFor(layout_, false, ended.code, false));
        }
        // an object is written beside its name and moved there once whole: a closed window loses only files in progress. A
        // scanner may hold the new file for a moment, so a move that fails is tried a few times.
        fs::path temp = fs::path(job.object).concat(".tmp");
        if (!Exists(temp) || SizeOf(temp) == 0) {
            stop();
            return Fail("Compiling " + job.name + ".cpp", "clang.exe ended well but its output is gone", HintFor(layout_, false, 0, true));
        }
        std::error_code ec;
        for (int attempt = 0; attempt < 20; attempt++) {
            fs::rename(temp, job.object, ec);
            if (!ec) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (ec) {
            stop();
            return Fail("Saving " + job.name + ".obj", ec.message(), HintFor(layout_, true, 0, false));
        }
        std::string key = job.key + "\n";
        if (!WriteFile(job.key_file, key.data(), key.size())) {
            stop();
            return Fail("Saving " + job.name + ".obj.key", "cannot write into " + U8(work_));
        }
        progress_.bytes_done += job.bytes;
        progress_.files_done++;
        Changed();
    }
    return true;
}

bool Builder::Link() {
    SetStage(Stage::Linking);
    fs::path staging = layout_.Build() / "new";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return Fail("Linking " + recipe_.library, "cannot make " + U8(staging) + ": " + ec.message());
    std::vector<Str> args;
    for (const std::string& f : Split(recipe_.link_flags)) args.push_back(A(f));
    args.push_back(A("/def:exports.def"));
    args.push_back(A("/implib:code.lib"));
    args.push_back(A("/out:") + (staging / recipe_.library).native());
    std::string stem = fs::path(recipe_.library).stem().string();
    args.push_back((layout_.Compiler() / (stem + ".lib")).native());
    for (const std::string& name : all_) args.push_back(A(name + ".obj"));
    return RunTool("Linking " + recipe_.library, "lld-link", args, work_);
}

bool Builder::Install() {
    SetStage(Stage::Checking);
    fs::path staged = layout_.Build() / "new" / recipe_.library;
    std::string found, why;
    if (!pe::ReadExportedText(staged, "X86GameCodeStamp", found, why))
        return Fail("Checking " + recipe_.library, "the linked file " + why, HintFor(layout_, false, 0, true));
    if (found != recipe_.stamp) {
        run_.Note("stamp: built \"" + found + "\", expected \"" + recipe_.stamp + "\"");
        return Fail("Checking " + recipe_.library, "the new file is not for this program (its stamp differs)",
                    "The compiler files and this program may not belong together: extract the whole zip again.");
    }
    fs::path final_dll = layout_.Build() / recipe_.library;
    std::error_code ec;
    for (int attempt = 0; attempt < 20; attempt++) {
        fs::rename(staged, final_dll, ec);
        if (!ec) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (ec)
        return Fail("Saving " + recipe_.library, ec.message(),
                    "If the game is running, close it and start it again. Otherwise antivirus software may be holding the file. " +
                        HintFor(layout_, false, 0, false));
    fs::remove_all(layout_.Build() / "new", ec);
    WriteFile(layout_.Build() / (recipe_.library + ".stamp"), (found + "\n").data(), found.size() + 1);
    run_.Note("built: " + U8(final_dll) + " (" + std::to_string(SizeOf(final_dll)) + " bytes), stamp \"" + found + "\"");
    return true;
}

Outcome Builder::Run(const ReadInputs& read, const ReadPictures& pictures) {
    const int64_t began = NowMs();
    Outcome outcome = RunSteps(read, pictures);
    run_.Note("time: total " + Ms(NowMs() - began));  // whatever the outcome: a failed or stopped build says how far it got
    return outcome;
}

Outcome Builder::RunSteps(const ReadInputs& read, const ReadPictures& pictures) {
    auto done = [&](bool ok) {
        if (ok) return Outcome::Built;
        return progress_.cancel ? Outcome::Cancelled : Outcome::Failed;
    };
    if (!Timed("disc read", [&] { return Prepare(read); })) return done(false);
    if (!Timed("config", [&] { return Config(); })) return done(false);
    if (!Timed("translate", [&] { return Translate(); })) return done(false);
    if (!Timed("pictures", [&] { return Pictures(pictures); })) return done(false);
    if (!Timed("plan", [&] { return Plan(); })) return done(false);
    // all the files, with how many were compiled (finished: a build that failed or was stopped says how far it got; the rest were kept from an
    // earlier start) and at how many at once
    const bool compiled = Timed("compile", [&] { return Compile(); }, [&] {
        return " (" + std::to_string(progress_.files_done.load()) + " files compiled, " + std::to_string(std::max<size_t>(recipe_.slots, 1)) + " at a time)";
    });
    if (!compiled) return done(false);
    if (!Timed("link", [&] { return Link(); })) return done(false);
    if (!Timed("check and install", [&] { return Install(); })) return done(false);
    SetStage(Stage::Done);
    return Outcome::Built;
}

}  // namespace

Outcome Build(const Layout& layout, const Recipe& recipe, const ReadInputs& read, const ReadPictures& pictures, Runner& run, Progress& progress,
              Failure& failure) {
    Builder builder(layout, recipe, run, progress, failure);
    return builder.Run(read, pictures);
}

bool PicturesCurrent(const Layout& layout, const Recipe& recipe) {
    if (recipe.pictures <= 0) return true;  // none to make
    std::error_code ec;
    return ReadText(layout.PicturesMarker()) == "pictures " + std::to_string(recipe.pictures) + "\n" && Exists(layout.data / "Buffy.ico") &&
           Exists(layout.data / "Buffy.png") && fs::is_directory(layout.data / "achievements", ec);
}

bool MakePictures(const Layout& layout, const Recipe& recipe, const ReadPictures& read, Runner& run) {
    const std::atomic<bool> never{false};
    const int64_t began = NowMs();
    const bool made = MakePicturesImpl(layout, recipe, read, run, never);
    run.Note("time: pictures " + Ms(NowMs() - began));  // in a build the phase is timed there
    return made;
}

std::string Blocker(const Layout& layout, const Recipe& recipe) {
    const fs::path c = layout.Compiler();
    std::vector<fs::path> needed = {layout.Tool("clang"), layout.Tool("lld-link"), layout.Tool("xbe_recomp"), layout.Tool("buffy_config"),
                                    c / "sdk_names.txt", c / (fs::path(recipe.library).stem().string() + ".lib"),
                                    c / "include" / "cpu" / "x86_ops.h", c / "include" / "cpu" / "x86_context.h",
                                    c / "include" / "freestanding" / "cstdint", c / "xdk" / "4432" / "layout.toml"};
    for (const fs::path& f : needed)
        if (!Exists(f))
            return "The compiler folder is missing " + U8(f.filename()) + " (" + U8(c) + "). Extract the whole zip again, keeping its folders together, then start the game.";
    std::error_code ec;
    fs::create_directories(layout.Work(), ec);
    fs::create_directories(layout.Build(), ec);
    fs::create_directories(layout.Log().parent_path(), ec);
    // can the folders be written? a file is made and removed
    for (const fs::path& dir : {layout.Work(), layout.Build()}) {
        fs::path probe = dir / "write-test.tmp";
        if (!WriteFile(probe, "x", 1))
            return "The folder " + U8(dir) + " cannot be written to. Move the game to a folder you can write to, such as Documents, or choose one with --data.";
        fs::remove(probe, ec);
    }
    fs::space_info space = fs::space(layout.Work(), ec);
    if (!ec && space.available < (1ull << 30))
        return "Preparing the game needs 1 GB of free space on the drive of " + U8(layout.Work()) + ".";
    return {};
}

DllState CheckDll(const fs::path& dll, const std::string& stamp, std::string* found) {
    if (!Exists(dll)) return DllState::Missing;
    std::string text, why;
    if (!pe::ReadExportedText(dll, "X86GameCodeStamp", text, why)) {
        if (found) *found = why;
        return DllState::Unreadable;
    }
    if (found) *found = text;
    return text == stamp ? DllState::Current : DllState::Stale;
}

std::string LogTail(const fs::path& log, size_t bytes) {
    std::ifstream f(log, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(bytes) ? size - std::streamoff(bytes) : 0;
    std::string tail(size_t(size - start), '\0');
    f.seekg(start);
    f.read(tail.data(), std::streamsize(tail.size()));
    tail.resize(size_t(f.gcount()));
    if (start) tail.erase(0, tail.find('\n') == std::string::npos ? 0 : tail.find('\n') + 1);
    return tail;
}

std::string ExitText(uint32_t code) {
    switch (code) {
        case kDllNotFound: return "exit code " + Hex(code) + ": a DLL it needs was not found";
        case kInitFailed: return "exit code " + Hex(code) + ": it could not start";
        case kBadImage: return "exit code " + Hex(code) + ": not a program for this PC";
        case kAccessDenied: return "exit code " + Hex(code) + ": access denied";
        case 0xC0000005: return "exit code " + Hex(code) + ": it crashed";
        case 0xC0000409: return "exit code " + Hex(code) + ": it crashed";
        default: break;
    }
    if (code >= 0x80000000u) return "exit code " + Hex(code);
    return "exit code " + std::to_string(code);
}

double Progress::Fraction() const {
    switch (stage.load()) {
        case Stage::Reading: return 0.0;
        case Stage::Config: return 0.01;
        case Stage::Translating: return 0.03;
        case Stage::Pictures: return 0.07;
        case Stage::Planning: return 0.09;
        case Stage::Compiling: {
            uint64_t total = bytes_total.load();
            double f = total ? double(bytes_done.load()) / double(total) : 1.0;
            return 0.10 + 0.87 * std::min(1.0, f);
        }
        case Stage::Linking: return 0.97;
        case Stage::Checking: return 0.99;
        default: return 1.0;
    }
}

std::string Progress::Text() const {
    switch (stage.load()) {
        case Stage::Reading: return "Reading the game from your disc";
        case Stage::Config: return "Setting up";
        case Stage::Translating: return "Translating the game's code";
        case Stage::Pictures: return "Making the achievement pictures and the icon";
        case Stage::Planning: return "Checking what is already built";
        case Stage::Compiling:
            return "Compiling the game's code, file " + std::to_string(std::min(files_done.load() + 1, std::max(files_total.load(), 1u))) +
                   " of " + std::to_string(files_total.load());
        case Stage::Linking: return "Linking the game's code";
        case Stage::Checking: return "Checking the game's code";
        default: return "Done";
    }
}

}  // namespace gamebuild
