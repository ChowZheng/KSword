// Forced into every fixture translation unit so MSVC member access mangling
// matches. This header is never used by the application or driver projects.
#pragma once
#include <QtWidgets>
#include "../../Ksword5.1/Ksword5.1/UI/KvmControl.h"
#define private public
#include "../../Ksword5.1/Ksword5.1/UI/KvmGuestVmPanel.h"
#include "../../Ksword5.1/Ksword5.1/KvmDock/KvmDock.h"
#include "../../Ksword5.1/Ksword5.1/KernelDock/KernelHvmTab.h"
#undef private
