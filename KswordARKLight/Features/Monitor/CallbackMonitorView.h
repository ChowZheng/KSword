#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Monitor {

HWND CreateCallbackMonitorPage(HWND parent, const RECT& bounds);

} // namespace Ksword::Features::Monitor
