#pragma once

// ============================================================
// hook/ClipboardGuardHook.h
// 作用：
// 1) 声明剪贴板保护功能里全部 Win32 平导出 hook（8 个）与 OLE 平导出 hook（2 个）；
// 2) 这十个都是普通导出函数的 inline hook，复用 HookEngine 现成的安装机制，
//    HookTargets.cpp 的 g_bindings[] 只需要引用这里的符号，不需要另外的安装/卸载入口；
// 3) 原函数指针、Hook 记录、Hooked 函数体全部集中在本模块，
//    不再散落回已经上万行的 HookTargets.cpp。
// ============================================================

#include "pch.h"
#include "HookEngine.h"

#include <objidl.h>

namespace apimon
{
#include "ApiMonitorClipboardDeclarations.inc"
}
