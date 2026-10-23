#pragma once

#include "../Core/Win32Lean.h"

namespace Ksword::Ui {

// kBaseDpi is the DPI every layout constant in this program is authored at. A
// "22" in a MoveWindow call means 22 DIPs, i.e. 22 physical pixels at 100%.
inline constexpr int kBaseDpi = 96;

// CurrentDpi is the DPI the next layout pass resolves against. It is
// process-wide on purpose: this program has exactly one top-level frame and every
// page is a child of it, so all pages live on the same monitor. A per-window DPI
// would mean threading a context handle through hundreds of layout call sites to
// describe a state that cannot occur.
UINT CurrentDpi();

// SetCurrentDpi publishes a new DPI. Call it from WM_DPICHANGED before relaying
// the frame out, so the layout that follows rescales as a whole.
void SetCurrentDpi(UINT dpi);

// SyncCurrentDpiFromWindow reads the window's monitor DPI and publishes it.
// Output is true when the value changed, i.e. a relayout is required.
bool SyncCurrentDpiFromWindow(HWND hwnd);

// Scale converts a baseline-96 logical value into physical pixels.
//
// While the process is DPI-unaware the OS reports 96 for every window, so Scale
// is the identity and adding it changes nothing on screen. That ordering is
// deliberate: the call sites get converted first, and the manifest that switches
// Per-Monitor V2 on is flipped once coverage is complete. Doing it the other way
// round leaves the app half-scaled during the migration, which looks worse than
// the uniform bitmap stretch it has today.
int Scale(int value);

// ScaleForDpi is the same conversion against an explicit DPI. It exists for the
// handful of places that must compute a rectangle for a monitor other than the
// current one.
int ScaleForDpi(int value, UINT dpi);

} // namespace Ksword::Ui
