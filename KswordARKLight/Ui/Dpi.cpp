#include "Dpi.h"

namespace Ksword::Ui {
namespace {

UINT gCurrentDpi = kBaseDpi;

using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

// ResolveGetDpiForWindow looks the API up lazily. It exists from Windows 10 1607;
// a static import would make the binary refuse to load on anything older, and
// this project still starts on build 10240.
GetDpiForWindowFn ResolveGetDpiForWindow() {
    static GetDpiForWindowFn cached = []() -> GetDpiForWindowFn {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (!user32) {
            return nullptr;
        }
        return reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "GetDpiForWindow")));
    }();
    return cached;
}

UINT DpiOfWindow(HWND hwnd) {
    if (hwnd) {
        if (const GetDpiForWindowFn getDpiForWindow = ResolveGetDpiForWindow()) {
            if (const UINT dpi = getDpiForWindow(hwnd)) {
                return dpi;
            }
        }
    }
    // No window, or an OS older than the API: the screen DC still answers.
    HDC dc = ::GetDC(nullptr);
    const int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSY) : kBaseDpi;
    if (dc) {
        ::ReleaseDC(nullptr, dc);
    }
    return dpi > 0 ? static_cast<UINT>(dpi) : static_cast<UINT>(kBaseDpi);
}

} // namespace

UINT CurrentDpi() {
    return gCurrentDpi;
}

void SetCurrentDpi(const UINT dpi) {
    if (dpi != 0) {
        gCurrentDpi = dpi;
    }
}

bool SyncCurrentDpiFromWindow(HWND hwnd) {
    const UINT dpi = DpiOfWindow(hwnd);
    if (dpi == gCurrentDpi) {
        return false;
    }
    gCurrentDpi = dpi;
    return true;
}

int ScaleForDpi(const int value, const UINT dpi) {
    if (dpi == 0 || dpi == static_cast<UINT>(kBaseDpi)) {
        return value;
    }
    return ::MulDiv(value, static_cast<int>(dpi), kBaseDpi);
}

int Scale(const int value) {
    return ScaleForDpi(value, gCurrentDpi);
}

} // namespace Ksword::Ui
