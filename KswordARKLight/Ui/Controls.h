#pragma once

#include "../Core/Win32Lean.h"

#include <string>

namespace Ksword::Ui {

// RegisterControlClasses registers all custom lightweight controls. Input is the
// module instance; processing registers idempotent WNDCLASS records; output is
// true when all required classes are available.
bool RegisterControlClasses(HINSTANCE instance);

// SystemUIFont returns the current Windows UI message font. There is no input;
// processing queries NONCLIENTMETRICS and creates a matching HFONT; output is a
// process-owned font handle that callers must not delete.
HFONT SystemUIFont();

// RefreshSystemUIFont drops the cached Windows UI message font. There is no
// input; processing recreates it on next use; no value is returned.
void RefreshSystemUIFont();

// CreateText creates a child STATIC using the Windows UI message font. Inputs
// are parent, id, text and geometry; processing creates the HWND; output is HWND.
HWND CreateText(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h);

// CreateButton creates a compact child push button using the Windows UI message
// font. Inputs are parent, id, text and geometry; output is the HWND.
HWND CreateButton(HWND parent, int id, const std::wstring& text, int x, int y, int w, int h);

// SetWindowFontRecursive applies the Windows UI message font to a window
// subtree. Input is a root HWND; processing sends WM_SETFONT; no return.
void SetWindowFontRecursive(HWND root);

// PaintPanel fills a themed panel rectangle. Inputs are paint DC and geometry;
// processing draws background and border; no value is returned.
void PaintPanel(HDC dc, const RECT& rect);

// PaintLabelBackground themes a STATIC label that sits on a custom-painted
// surface. Read-only multi-line EDIT controls report through
// WM_CTLCOLORSTATIC as well, and forcing a transparent background on them
// smears the text as soon as the user scrolls, so this helper accepts a plain
// STATIC only. Inputs are the WM_CTLCOLORSTATIC DC, the child HWND, the
// background brush and the label color; output is the brush to return from the
// message handler, or 0 when the caller must fall through to DefWindowProc.
inline LRESULT PaintLabelBackground(HDC dc, HWND child, HBRUSH background, COLORREF textColor) {
    wchar_t className[16] = {};
    if (!dc || !child || ::GetClassNameW(child, className, 16) == 0) {
        return 0;
    }
    if (::lstrcmpiW(className, L"Static") != 0) {
        return 0;
    }
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, textColor);
    return reinterpret_cast<LRESULT>(background);
}

// DrawTextLine draws one clipped text line. Inputs are a DC, text, rect, color,
// font and alignment flags; processing selects the font temporarily; no return.
void DrawTextLine(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font, UINT format);

} // namespace Ksword::Ui
