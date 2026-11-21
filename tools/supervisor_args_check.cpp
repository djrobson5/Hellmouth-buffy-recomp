// The decision of the supervisor (sdk/runtime/app/supervisor_args.h, which has no Windows code) run on this Mac over command lines as
// RecompLauncher and a player would give them: which are run under a supervisor, and which are the game itself. The launcher's own
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
    std::printf("%zu cases\n", cases.size() + splits.size());
    return wrong ? 1 : 0;
}
