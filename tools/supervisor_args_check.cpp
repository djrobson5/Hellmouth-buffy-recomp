// The decision of the supervisor and of the Mac's keeper (sdk/runtime/app/supervisor_args.h, which has no Windows code) run on this Mac over
// command lines and argvs as RecompLauncher and a player would give them: which are run under a supervisor, and which are the game itself. The launcher's own
// start, and its setup, are the real ones (tools/recomplauncher.json.in). tools/sim_first_start.sh builds and runs it.
//
//   supervisor_args_check            prints each case that is wrong and "<n> cases"; exits 1 if any is wrong
#include <cstdio>
#include <string>
#include <vector>

#include "app/supervisor_args.h"

namespace {

struct Case {
    const wchar_t* line;
    bool launcher;    // RECOMPLAUNCHER=1
    bool marker;      // the supervisor's marker is in the environment
    bool supervised;  // expected
    const char* why;
};

}  // namespace

int main() {
    const std::vector<Case> cases = {
        {LR"("C:\Games\Buffy\Buffy.exe" --game=D:\Buffy.iso --data C:\Data --no-dialogs)", true, false, true, "RecompLauncher's start"},
        {LR"(C:\Games\Buffy\Buffy.exe --game=D:\Buffy.iso --data C:\Data --no-dialogs)", true, false, true, "the same, the program name not quoted"},
        {LR"("C:\Games\Buffy\Buffy.exe")", true, false, true, "no arguments"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game "D:\My Games\Buffy.iso" --data "C:\Data Dir\" --no-dialogs)", true, false, true, "quoted paths, a backslash before the closing quote"},
        {LR"("C:\Games\Buffy\Buffy.exe" --instance 2 --data C:\Data)", true, false, true, "a second copy"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game=D:\Buffy.iso --no-dialogs)", false, false, false, "not started by RecompLauncher"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game=D:\Buffy.iso --no-dialogs)", true, true, false, "the game process of a supervisor"},
        {LR"("C:\Games\Buffy\Buffy.exe" --prepare --game=D:\Buffy.iso --data C:\Data)", true, false, false, "--prepare"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game=D:\Buffy.iso --data C:\Data --prepare --clean-work)", true, false, false, "--prepare after the other options"},
        {LR"("C:\Games\Buffy\Buffy.exe" --prepare=1)", true, false, false, "--prepare=value, which main.cpp reads as --prepare"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game=D:\Buffy.iso --relaunched)", true, false, false, "a restart for a level"},
        {LR"("C:\Games\Buffy\Buffy.exe" --version)", true, false, false, "--version"},
        {LR"("C:\Games\Buffy\Buffy.exe" --describe-settings)", true, false, false, "--describe-settings"},
        {LR"("C:\Games\Buffy\Buffy.exe" --data --prepare)", true, false, true, "--prepare as the value of --data is a folder name"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game "D:\x --prepare\Buffy.iso")", true, false, true, "--prepare inside a quoted path"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game=D:\prepare\--prepare.iso --data C:\Data)", true, false, true, "--prepare only as part of a path"},
        {LR"("C:\Games\--prepare\Buffy.exe" --game=D:\Buffy.iso)", true, false, true, "--prepare in the program's own path"},
        {LR"("C:\Games\Buffy\Buffy.exe" --prepared)", true, false, true, "another word that starts the same"},
        {LR"("C:\Games\Buffy\Buffy.exe"	--version)", true, false, false, "a tab between the arguments"},
        {LR"(  "C:\Games\Buffy\Buffy.exe"   --game=x   --relaunched  )", true, false, false, "spaces around the arguments"},
        {LR"("C:\Games\Buffy\Buffy.exe" --game=a\"b --relaunched)", true, false, false, "an escaped quote does not hide what follows"},
        {L"", true, false, true, "no command line at all"},
    };
    int wrong = 0;
    for (const Case& c : cases) {
        const bool got = app::ShouldSupervise(c.line, c.launcher, c.marker);
        if (got != c.supervised) {
            std::printf("WRONG: %s: supervised %d, expected %d\n", c.why, got, c.supervised);
            wrong++;
        }
    }
    // the splitting itself, on the cases CommandLineToArgvW documents
    struct Split {
        const wchar_t* line;
        std::vector<std::wstring> argv;
    };
    const std::vector<Split> splits = {
        {LR"("a b c" d e)", {L"a b c", L"d", L"e"}},
        {LR"(a "b\"c" d)", {L"a", L"b\"c", L"d"}},
        {LR"(a "b\\"c d" e)", {L"a", L"b\\c", L"d e"}},
        {LR"(a b\\\"c d)", {L"a", L"b\\\"c", L"d"}},
        {LR"(a b\\c d)", {L"a", L"b\\\\c", L"d"}},
        {LR"("C:\Dir\" x y)", {L"C:\\Dir\\", L"x", L"y"}},
        {LR"(a "" b)", {L"a", L"", L"b"}},
    };
    for (const Split& s : splits)
        if (app::SplitCommandLine(s.line) != s.argv) {
            std::printf("WRONG: splitting [%ls]\n", s.line);
            wrong++;
        }
    // a Mac's argv: already split, UTF-8, the same decision (the keeper of kernel/keeper.h; the Mac-only one-shots are not Windows's)
    struct Argv {
        std::vector<const char*> argv;
        bool launcher, marker, supervised;
        const char* why;
    };
    const char* const app = "/Applications/Hellmouth.app/Contents/MacOS/Buffy";
    const std::vector<Argv> macs = {
        {{app, "--game", "/Users/x/Buffy.iso", "--data", "/Users/x/Data", "--no-dialogs"}, true, false, true, "mac: RecompLauncher's start"},
        {{app}, true, false, true, "mac: no arguments"},
        {{app, "--game=/Users/x/My Games/Buffy.iso"}, true, false, true, "mac: a space in a path is one argument"},
        {{app, "-psn_0_12345"}, true, false, true, "mac: the process serial number an app gets from Finder is no option"},
        {{app, "/Users/x/Buffy.iso"}, true, false, true, "mac: a game without an option"},
        {{app, "--game", "/x.iso"}, false, false, false, "mac: not started by RecompLauncher"},
        {{app, "--game", "/x.iso"}, true, true, false, "mac: the game process of a keeper"},
        {{app, "--prepare", "--game", "/x.iso"}, true, false, false, "mac: --prepare"},
        {{app, "--game=/x.iso", "--prepare"}, true, false, false, "mac: --prepare last"},
        {{app, "--relaunched"}, true, false, false, "mac: --relaunched"},
        {{app, "--version"}, true, false, false, "mac: --version"},
        {{app, "--describe-settings"}, true, false, false, "mac: --describe-settings"},
        {{app, "--check-game-code"}, true, false, false, "mac: --check-game-code"},
        {{app, "--print-paths", "--data", "/d"}, true, false, false, "mac: --print-paths"},
        {{app, "--title-id"}, true, false, false, "mac: --title-id"},
        {{app, "--data", "--prepare"}, true, false, true, "mac: --prepare as the value of --data is a folder name"},
        {{app, "--game", "/x --prepare/y.iso"}, true, false, true, "mac: --prepare inside a path"},
        {{app, "--game", "--version"}, true, false, true, "mac: --version as the value of --game"},
        {{app, "--game-code", "/d/Buffy.dylib"}, true, false, true, "mac: --game-code with its value is a play start"},
        {{app, "--instance", "2", "--data", "/d"}, true, false, true, "mac: a second copy"},
        {{app, "--prepared"}, true, false, true, "mac: another word that starts the same"},
    };
    for (const Argv& c : macs) {
        const bool got = app::ShouldSuperviseArgv(app::WidenArgv(int(c.argv.size()), const_cast<char* const*>(c.argv.data())), c.launcher, c.marker);
        if (got != c.supervised) {
            std::printf("WRONG: %s: supervised %d, expected %d\n", c.why, got, c.supervised);
            wrong++;
        }
    }
    std::printf("%zu cases\n", cases.size() + splits.size() + macs.size());
    return wrong ? 1 : 0;
}
