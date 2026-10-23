#pragma once

#include "../Core/Win32Lean.h"

#include <string>

namespace Ksword::Ui {

// CreateLoadingOverlay creates a hidden child overlay. Call SetLoadingOverlay
// when a page starts or finishes background work, and keep it positioned over
// the page's result area from the parent WM_SIZE handler.
HWND CreateLoadingOverlay(HWND parent, int id, const RECT& bounds);

// SetLoadingOverlay toggles the overlay and updates its message. Showing the
// overlay starts a lightweight spinner timer; hiding it stops the timer.
//
// Pass spinner=false for states that are not pending work, such as an empty
// result. A rotating indicator over "no rows" tells the user to wait for
// something that is never coming, which is the opposite of an honest empty state.
void SetLoadingOverlay(HWND overlay, bool visible, const std::wstring& message = L"", bool spinner = true);

} // namespace Ksword::Ui
