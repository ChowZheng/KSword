#pragma once

#include "../../Core/Win32Lean.h"

namespace Ksword::Features::Monitor {

// CreateMonitorFeaturePage creates the ETW and kernel callback monitor workspace.
HWND CreateMonitorFeaturePage(HWND parent, const RECT& bounds);

bool RequestMonitorFeatureProcess(HWND page, DWORD processId);

} // namespace Ksword::Features::Monitor
