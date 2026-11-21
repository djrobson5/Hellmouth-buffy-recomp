// A shortcut to the game beside the executable ("<title>.lnk": target the executable, working folder its folder, the icon the first start
// made from the player's disc, <data>\Buffy.ico, else the executable's own, and the --data argument if one was given), made or brought up
// to date at each normal start. A .lnk holds absolute paths, so one that was made before the folder was moved says where the game used to
// be: it is written again when what it holds differs from what this start knows, and left alone when it does not.
//
// Windows only, and not checked on a Mac. Best effort: a folder that cannot be written to, or a COM call that fails, is a line in the log
// and nothing else. It writes only that one file, beside the executable: nothing on the Desktop or in the Start menu, and the shortcut is
// never resolved (resolving can search the disk and pop up a dialog).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <filesystem>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

namespace fs = std::filesystem;

// the executable's full path, however long
std::wstring ExePath() {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    return std::wstring(path.data(), n);
}

// a path as one command line argument, as CommandLineToArgvW reads it back: in quotes, with the backslashes before the closing quote doubled
std::wstring QuotedPath(const fs::path& p) {
    std::wstring s = p.wstring();
    size_t trailing = 0;
    while (trailing < s.size() && s[s.size() - 1 - trailing] == L'\\') trailing++;
    return L"\"" + s + std::wstring(trailing, L'\\') + L"\"";
}

// what the shortcut should hold
struct Wanted {
    std::wstring target, arguments, folder, icon;
    int icon_index = 0;
};

bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// whether the file holds what is wanted, read without resolving it
bool Holds(IShellLinkW* link, const Wanted& want) {
    std::vector<wchar_t> target(32768), arguments(32768), folder(32768), icon(32768);
    int icon_index = -1;
    if (FAILED(link->GetPath(target.data(), int(target.size()), nullptr, SLGP_RAWPATH))) return false;
    if (FAILED(link->GetArguments(arguments.data(), int(arguments.size())))) return false;
    if (FAILED(link->GetWorkingDirectory(folder.data(), int(folder.size())))) return false;
    if (FAILED(link->GetIconLocation(icon.data(), int(icon.size()), &icon_index))) return false;
    return Same(target.data(), want.target) && want.arguments == arguments.data() && Same(folder.data(), want.folder) && Same(icon.data(), want.icon) &&
           icon_index == want.icon_index;
}

}  // namespace

void UpdateShortcut() {
    // a restart for a level and a launcher's start are not the player starting the game
    if (Args().relaunched || Args().no_dialogs) return;
    const fs::path exe = ExePath();
    const fs::path file = exe.parent_path() / (std::wstring(kGame.title) + L".lnk");
    Wanted want;
    want.target = exe.wstring();
    want.folder = exe.parent_path().wstring();
    if (!Args().data.empty()) want.arguments = L"--data " + QuotedPath(Args().data);
    std::error_code ec;
    const fs::path icon = DataDir() / L"Buffy.ico";
    if (fs::is_regular_file(icon, ec)) {
        want.icon = icon.wstring();
    } else {
        want.icon = want.target;  // the pictures were not made: the executable's own icon
    }

    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IShellLinkW* link = nullptr;
    IPersistFile* persist = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || !link ||
        FAILED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
        LOGW("shortcut: COM would not make a shortcut object");
    } else {
        const bool had = fs::exists(file, ec);
        if (had && SUCCEEDED(persist->Load(file.c_str(), STGM_READ)) && Holds(link, want)) {
            LOGR("shortcut: %ls is up to date", file.c_str());
        } else {
            link->SetPath(want.target.c_str());
            link->SetArguments(want.arguments.c_str());
            link->SetWorkingDirectory(want.folder.c_str());
            link->SetIconLocation(want.icon.c_str(), want.icon_index);
            link->SetDescription(kGame.title);
            HRESULT saved = persist->Save(file.c_str(), TRUE);
            if (SUCCEEDED(saved)) LOGR("shortcut: %ls %s", file.c_str(), had ? "written again (what it held was not this start's)" : "made");
            else LOGW("shortcut: %ls could not be written (0x%08lX): the folder may be read only; the game starts all the same", file.c_str(), (unsigned long)saved);
        }
    }
    if (persist) persist->Release();
    if (link) link->Release();
    if (SUCCEEDED(init)) CoUninitialize();
}

}  // namespace app
