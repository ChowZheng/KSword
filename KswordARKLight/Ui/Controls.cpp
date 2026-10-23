#include "Controls.h"

#include "Dpi.h"
#include "Theme.h"

#include <commctrl.h>

namespace Ksword::Ui {
namespace {
HFONT gSystemUIFont = nullptr;

using SystemParametersInfoForDpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);

// ResolveSystemParametersInfoForDpi looks the API up lazily: it exists from
// Windows 10 1607, and importing it statically would stop the binary from loading
// on older builds.
SystemParametersInfoForDpiFn ResolveSystemParametersInfoForDpi() {
    static SystemParametersInfoForDpiFn cached = []() -> SystemParametersInfoForDpiFn {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (!user32) {
            return nullptr;
        }
        return reinterpret_cast<SystemParametersInfoForDpiFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "SystemParametersInfoForDpi")));
    }();
    return cached;
}

// CreateSystemUIFont queries the non-client message font at the DPI the layout is
// currently authored against. SystemParametersInfoW answers for the *system* DPI,
// which is wrong under Per-Monitor V2 on a differently scaled monitor: the layout
// would rescale while the text would not, and every fixed-height row would clip.
// Output is a newly created HFONT or the DEFAULT_GUI_FONT fallback.
HFONT CreateSystemUIFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    bool queried = false;
    if (const SystemParametersInfoForDpiFn forDpi = ResolveSystemParametersInfoForDpi()) {
        queried = forDpi(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0, CurrentDpi()) != FALSE;
    }
    if (!queried) {
        queried = ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, metrics.cbSize, &metrics, 0) != FALSE;
    }
    if (queried) {
        if (HFONT font = ::CreateFontIndirectW(&metrics.lfMessageFont)) {
            return font;
        }
    }
    return reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}
} // namespace

HFONT SystemUIFont() {
    if (!gSystemUIFont) {
        gSystemUIFont = CreateSystemUIFont();
    }
    return gSystemUIFont;
}

void RefreshSystemUIFont() {
    if (gSystemUIFont && reinterpret_cast<HGDIOBJ>(gSystemUIFont) != ::GetStockObject(DEFAULT_GUI_FONT)) {
        ::DeleteObject(gSystemUIFont);
    }
    gSystemUIFont = nullptr;
}

bool RegisterControlClasses(HINSTANCE instance) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    ::InitCommonControlsEx(&icc);
    (void)instance;
    return true;
}

HWND CreateText(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h) {
    HWND hwnd = ::CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
    }
    return hwnd;
}

HWND CreateButton(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h,
    const ButtonRole role) {
    // Default keeps the native button. The other two roles must control the
    // surface, so they switch the control to owner-draw and remember the role on
    // the window itself; the message handler then just forwards WM_DRAWITEM.
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        (role == ButtonRole::Default ? BS_PUSHBUTTON : (BS_OWNERDRAW | BS_NOTIFY));
    HWND hwnd = ::CreateWindowExW(0, L"BUTTON", text.c_str(), style,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
        if (role != ButtonRole::Default) {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(role));
        }
    }
    return hwnd;
}

void PlaceStatusLabel(HWND label, const int left, const int rowTop, const int width, const int height) {
    if (!label) {
        return;
    }
    const int available = width - left;
    if (available < kMinStatusLabelWidth) {
        // A label narrower than this shows two characters and an ellipsis at
        // best. Hide it: a hidden status is a state the layout can recover from
        // on the next wide resize, a zero-width one is indistinguishable from a
        // broken page.
        ::ShowWindow(label, SW_HIDE);
        return;
    }
    ::ShowWindow(label, SW_SHOW);
    ::MoveWindow(label, left, rowTop + 4, available, height, TRUE);
}

void DrawThemedButton(const DRAWITEMSTRUCT& item) {
    if (!item.hDC) {
        return;
    }
    const auto role = static_cast<ButtonRole>(::GetWindowLongPtrW(item.hwndItem, GWLP_USERDATA));
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;

    COLORREF surface = AppTheme().panelColor;
    COLORREF text = AppTheme().textColor;
    if (role == ButtonRole::Danger) {
        surface = pressed ? AppTheme().dangerPressedColor : AppTheme().dangerColor;
        text = AppTheme().dangerTextColor;
    } else if (role == ButtonRole::Primary) {
        surface = pressed ? AppTheme().accentDarkColor : AppTheme().accentColor;
        text = AppTheme().dangerTextColor;
    } else if (pressed) {
        surface = RGB(228, 232, 238);
    }
    if (disabled) {
        surface = RGB(238, 240, 243);
        text = AppTheme().mutedTextColor;
    }

    RECT rect = item.rcItem;
    HBRUSH fill = ::CreateSolidBrush(surface);
    ::FillRect(item.hDC, &rect, fill);
    ::DeleteObject(fill);
    // The border keeps a white-on-white Default button readable and separates the
    // Danger/Primary block from the panel behind it.
    HBRUSH border = ::CreateSolidBrush(role == ButtonRole::Default ? AppTheme().borderColor : surface);
    ::FrameRect(item.hDC, &rect, border);
    ::DeleteObject(border);

    wchar_t caption[128] = {};
    ::GetWindowTextW(item.hwndItem, caption, 128);
    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC, text);
    HGDIOBJ oldFont = ::SelectObject(item.hDC, SystemUIFont());
    RECT textRect = rect;
    ::InflateRect(&textRect, -4, 0);
    ::DrawTextW(item.hDC, caption, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    ::SelectObject(item.hDC, oldFont);

    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = rect;
        ::InflateRect(&focus, -3, -3);
        ::DrawFocusRect(item.hDC, &focus);
    }
}

void SetWindowFontRecursive(HWND root) {
    if (!root) {
        return;
    }
    ::SendMessageW(root, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
    ::EnumChildWindows(root, [](HWND child, LPARAM) -> BOOL {
        ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(SystemUIFont()), TRUE);
        return TRUE;
    }, 0);
}

void PaintPanel(HDC dc, const RECT& rect) {
    ::FillRect(dc, &rect, AppTheme().panelBrush());
    // The frame pen is cached on the theme: creating and deleting it per panel
    // per frame produced a GDI object churn on every repaint.
    HPEN pen = AppTheme().borderPen();
    HGDIOBJ oldPen = pen ? ::SelectObject(dc, pen) : nullptr;
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    ::SelectObject(dc, oldBrush);
    if (oldPen) {
        ::SelectObject(dc, oldPen);
    }
}

void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format) {
    const int oldMode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = ::SetTextColor(dc, color);
    HFONT selectedFont = font ? font : SystemUIFont();
    HGDIOBJ oldFont = ::SelectObject(dc, reinterpret_cast<HGDIOBJ>(selectedFont));
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_END_ELLIPSIS);
    ::SelectObject(dc, oldFont);
    ::SetTextColor(dc, oldColor);
    ::SetBkMode(dc, oldMode);
}

} // namespace Ksword::Ui
