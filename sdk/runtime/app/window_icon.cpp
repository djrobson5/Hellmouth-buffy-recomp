// The game window's icon, from the one the first start made from the player's disc: <data>\Buffy.ico (buffy_config --pictures, see
// game_build.h). Windows only, and not checked on a Mac: a window without it keeps the executable's own plain icon.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>

#include "app/app.h"
#include "core/log.h"

namespace app {

void SetWindowIcons(void* window) {
    HWND hwnd = static_cast<HWND>(window);
    const std::filesystem::path file = DataDir() / L"Buffy.ico";
    std::error_code ec;
    if (!hwnd || !std::filesystem::is_regular_file(file, ec)) return;
    const UINT dpi = GetDpiForWindow(hwnd);
    // the sizes the taskbar and the title bar want at this DPI; the file holds 256, 48, 32 and 16, and Windows picks the nearest
    const struct {
        WPARAM which;
        int metric_x, metric_y;
    } kinds[] = {{ICON_BIG, SM_CXICON, SM_CYICON}, {ICON_SMALL, SM_CXSMICON, SM_CYSMICON}};
    for (const auto& kind : kinds) {
        HANDLE icon = LoadImageW(nullptr, file.c_str(), IMAGE_ICON, GetSystemMetricsForDpi(kind.metric_x, dpi), GetSystemMetricsForDpi(kind.metric_y, dpi),
                                 LR_LOADFROMFILE);
        if (!icon) {
            LOGW("window icon: %ls would not load (Windows error %lu)", file.c_str(), GetLastError());
            return;
        }
        SendMessageW(hwnd, WM_SETICON, kind.which, reinterpret_cast<LPARAM>(icon));  // kept for the life of the process
    }
}

}  // namespace app
