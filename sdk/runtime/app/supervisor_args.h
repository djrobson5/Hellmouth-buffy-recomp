#pragma once
// Whether a start is a normal play start that RecompLauncher made, and so is run under a supervisor (supervisor.cpp). Only the
// command line is read here, as main.cpp's Args() reads it, and no Windows header is needed: tools/sim_first_start.sh runs
// tools/supervisor_args_check.cpp over it on a Mac.
#include <string>
#include <vector>

namespace app {

// the command line split as CommandLineToArgvW does: the program name by the simple rule (a quote switches, no escapes), the rest by the
// usual one (2n backslashes and a quote are n backslashes and a switch, 2n+1 are n backslashes and a quote)
inline std::vector<std::wstring> SplitCommandLine(const std::wstring& line) {
    std::vector<std::wstring> out;
    size_t i = 0;
    const size_t n = line.size();
    auto blank = [&](size_t k) { return line[k] == L' ' || line[k] == L'\t'; };
    while (i < n && blank(i)) i++;
    if (i < n) {  // argv[0]
        std::wstring a;
        bool quoted = false;
        for (; i < n && (quoted || !blank(i)); i++) {
            if (line[i] == L'"') quoted = !quoted;
            else a += line[i];
        }
        out.push_back(a);
    }
    for (;;) {
        while (i < n && blank(i)) i++;
        if (i >= n) break;
        std::wstring a;
        bool quoted = false;
        while (i < n && (quoted || !blank(i))) {
            size_t slashes = 0;
            while (i < n && line[i] == L'\\') slashes++, i++;
            if (i < n && line[i] == L'"') {
                a.append(slashes / 2, L'\\');
                if (slashes % 2) a += L'"';
                else quoted = !quoted;
                i++;
            } else {
                a.append(slashes, L'\\');
                if (i < n && (quoted || !blank(i))) a += line[i++];
            }
        }
        out.push_back(a);
    }
    return out;
}

// True for a normal play start under RecompLauncher: RECOMPLAUNCHER=1 (by_launcher), no marker of ours yet (already_supervised: this
// is the game's own process, started by a supervisor), and none of the one-shot modes: --prepare, --relaunched (a restart for a
// level), --version and --describe-settings. The options are found as main.cpp's Args() finds them: "--name=value" counts as
// "--name", and the word after an option that takes a value is that value, not an option.
inline bool ShouldSupervise(const std::wstring& command_line, bool by_launcher, bool already_supervised) {
    if (!by_launcher || already_supervised) return false;
    static const wchar_t* const kTakesValue[] = {L"--game",   L"--config",         L"--textures", L"--log",     L"--saves",
                                                  L"--data", L"--shader-cache", L"--screenshots", L"--instance"};
    static const wchar_t* const kOneShot[] = {L"--prepare", L"--relaunched", L"--version", L"--describe-settings"};
    const std::vector<std::wstring> argv = SplitCommandLine(command_line);
    for (size_t i = 1; i < argv.size(); i++) {
        std::wstring a = argv[i];
        bool has_value = false;
        if (a.rfind(L"--", 0) == 0)
            if (size_t eq = a.find(L'='); eq != std::wstring::npos) {
                a.resize(eq);
                has_value = true;
            }
        for (const wchar_t* name : kOneShot)
            if (a == name) return false;
        for (const wchar_t* name : kTakesValue)
            if (a == name) {
                if (!has_value) i++;
                break;
            }
    }
    return true;
}

}  // namespace app
