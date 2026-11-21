// Which copy of the game to run, on Windows: the one --game names, else the disc image the player chose last time, else what is
// in assets beside the exe, else the player is asked with the system's file dialog. Only a choice that was accepted (it holds the
// release this program was made from) is remembered, so a wrong one is asked about again instead of failing at every start. The
// choice is kept in <data>\disc_image.txt (a line, UTF-8), which is also how the restart for a level finds it. The Mac host
// does the same in sdk/host/main.cpp (PickGame, KeptGameFile).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <fstream>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

namespace fs = std::filesystem;

fs::path KeptFile() { return DataDir() / "disc_image.txt"; }

std::string ToUtf8(const std::wstring& w) {
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    if (!s.empty()) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

// the remembered path, empty when there is none or it is no longer a file or folder
fs::path Kept() {
    std::ifstream f(KeptFile(), std::ios::binary);
    std::string line;
    if (!f || !std::getline(f, line)) return {};
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
    if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // a byte order mark, if an editor added one
    if (line.empty()) return {};
    fs::path path = FromUtf8(line);
    std::error_code ec;
    return fs::exists(path, ec) ? path : fs::path();
}

void Keep(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(KeptFile().parent_path(), ec);
    std::ofstream f(KeptFile(), std::ios::binary | std::ios::trunc);
    f << ToUtf8(path.wstring()) << '\n';
    if (!f) LOGW("disc image: could not write %s", KeptFile().string().c_str());
}

void Forget() {
    std::error_code ec;
    fs::remove(KeptFile(), ec);
}

// the system's file dialog; empty when the player cancels it or it cannot be shown
fs::path PickDisc() {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    fs::path chosen;
    IFileOpenDialog* dialog = nullptr;
    HRESULT made = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(made)) {
        LOGE("disc image: the file dialog could not be made (%08lX)", static_cast<unsigned long>(made));
    } else {
        static const COMDLG_FILTERSPEC filters[] = {{L"Xbox disc image (*.iso)", L"*.iso"}, {L"All files (*.*)", L"*.*"}};
        dialog->SetFileTypes(2, filters);
        dialog->SetFileTypeIndex(1);
        dialog->SetTitle(L"Choose your Buffy the Vampire Slayer disc image");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        HRESULT shown = dialog->Show(nullptr);
        if (SUCCEEDED(shown)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR name = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                    chosen = name;
                    CoTaskMemFree(name);
                }
                item->Release();
            }
        } else if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            LOGE("disc image: the file dialog failed (%08lX)", static_cast<unsigned long>(shown));
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return chosen;
}

}  // namespace

bool ChooseGame(GameFiles& game) {
    // a launcher's or the command line's choice is the choice
    if (!Args().game.empty()) return FindGame(game);
    const bool may_ask = !Args().no_dialogs && !Args().prepare && !Args().relaunched;

    GameSearch search;
    if (fs::path kept = Kept(); !kept.empty()) {
        if (FindGameFrom(kept, game, search)) {
            LOGR("Game: %s, as chosen before", kept.string().c_str());
            return true;
        }
        // not the game any more (moved, replaced, or another release): ask again, and say why in the log
        LOGW("disc image: %s no longer works: %ls", kept.string().c_str(), search.problem.c_str());
        Forget();
    }
    search = {};
    if (FindGameFrom({}, game, search)) return true;
    if (!may_ask) {
        Fail(search.code ? search.code : kExitNoGame, search.problem.empty() ? std::wstring(kGame.missing) : search.problem);
    }

    // ask: the file dialog straight away when there is nothing yet, again after a file that is not the game (with the reason),
    // until the player picks one or cancels
    std::wstring problem = search.problem == kGame.missing ? std::wstring() : search.problem;
    for (;;) {
        if (!problem.empty()) {
            std::wstring text = problem + L"\n\nChoose another disc image (an .iso file)?";
            if (MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_OKCANCEL) != IDOK) return false;
        }
        fs::path picked = PickDisc();
        if (picked.empty()) return false;
        GameSearch result;
        if (FindGameFrom(picked, game, result)) {
            LOGR("Game: %s, chosen now", picked.string().c_str());
            Keep(picked);
            return true;
        }
        LOGW("disc image: %s is not accepted: %ls", picked.string().c_str(), result.problem.c_str());
        problem = result.problem.empty() ? std::wstring(L"That file is not the game.") : result.problem;
    }
}

}  // namespace app
