// Forced into every fixture translation unit so MSVC member access mangling
// matches. This header is never used by the application or driver projects.
#pragma once
#include <QtWidgets>
#include "../../Ksword5.1/Ksword5.1/UI/HvmControl.h"
#define private public
#include "../../Ksword5.1/Ksword5.1/UI/HvmGuestVmPanel.h"
#include "../../Ksword5.1/Ksword5.1/HvmDock/HvmDock.h"
#include "../../Ksword5.1/Ksword5.1/KernelDock/KernelHvmTab.h"
#undef private
