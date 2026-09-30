#pragma once

#include "../../Core/Win32Lean.h"

#include <string>

namespace Ksword::Features::Driver {

// System(PID 4) thread evidence and the same guarded driver-thread actions as
// the main application's DriverDock system-thread page.
HWND CreateDriverThreadView(HWND parent, const RECT& bounds);
std::wstring ExportDriverThreadViewTsv(HWND page);

} // namespace Ksword::Features::Driver
