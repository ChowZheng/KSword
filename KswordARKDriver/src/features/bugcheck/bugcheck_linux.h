#pragma once

#include "bugcheck_layout.h"

// Linux 风格只读取已有固定快照，并把完整诊断文本编码为屏幕中央二维码。
NTSTATUS
KswordARKBugcheckLinuxDraw(
    _In_ const KSWORD_ARK_BUGCHECK_LAYOUT_CANVAS* Canvas,
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics,
    _In_ ULONG CallbackMask,
    _In_ ULONG ModuleCount
    );
