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
    // ---- 原函数指针类型 ----
    using OpenClipboardFn = BOOL(WINAPI*)(HWND);
    using CloseClipboardFn = BOOL(WINAPI*)();
    using GetClipboardDataFn = HANDLE(WINAPI*)(UINT);
    using SetClipboardDataFn = HANDLE(WINAPI*)(UINT, HANDLE);
    using EmptyClipboardFn = BOOL(WINAPI*)();
    using EnumClipboardFormatsFn = UINT(WINAPI*)(UINT);
    using IsClipboardFormatAvailableFn = BOOL(WINAPI*)(UINT);
    using GetPriorityClipboardFormatFn = int(WINAPI*)(UINT*, int);
    using OleGetClipboardFn = HRESULT(WINAPI*)(IDataObject**);
    using OleSetClipboardFn = HRESULT(WINAPI*)(IDataObject*);

    // ---- 原函数指针：InstallInlineHook 安装成功后由 HookEngine 回填 ----
    extern OpenClipboardFn g_openClipboardOriginal;
    extern CloseClipboardFn g_closeClipboardOriginal;
    extern GetClipboardDataFn g_getClipboardDataOriginal;
    extern SetClipboardDataFn g_setClipboardDataOriginal;
    extern EmptyClipboardFn g_emptyClipboardOriginal;
    extern EnumClipboardFormatsFn g_enumClipboardFormatsOriginal;
    extern IsClipboardFormatAvailableFn g_isClipboardFormatAvailableOriginal;
    extern GetPriorityClipboardFormatFn g_getPriorityClipboardFormatOriginal;
    extern OleGetClipboardFn g_oleGetClipboardOriginal;
    extern OleSetClipboardFn g_oleSetClipboardOriginal;

    // ---- Hook 记录：g_bindings[] 需要取这些的地址传给 InstallInlineHook ----
    extern InlineHookRecord g_openClipboardHook;
    extern InlineHookRecord g_closeClipboardHook;
    extern InlineHookRecord g_getClipboardDataHook;
    extern InlineHookRecord g_setClipboardDataHook;
    extern InlineHookRecord g_emptyClipboardHook;
    extern InlineHookRecord g_enumClipboardFormatsHook;
    extern InlineHookRecord g_isClipboardFormatAvailableHook;
    extern InlineHookRecord g_getPriorityClipboardFormatHook;
    extern InlineHookRecord g_oleGetClipboardHook;
    extern InlineHookRecord g_oleSetClipboardHook;

    // ---- Hooked 函数体：供 HookTargets.cpp 的 g_bindings[] 引用 ----
    BOOL WINAPI HookedOpenClipboard(HWND ownerWindow);
    BOOL WINAPI HookedCloseClipboard();
    HANDLE WINAPI HookedGetClipboardData(UINT formatValue);
    HANDLE WINAPI HookedSetClipboardData(UINT formatValue, HANDLE dataHandle);
    BOOL WINAPI HookedEmptyClipboard();
    UINT WINAPI HookedEnumClipboardFormats(UINT formatValue);
    BOOL WINAPI HookedIsClipboardFormatAvailable(UINT formatValue);
    int WINAPI HookedGetPriorityClipboardFormat(UINT* formatPriorityList, int formatCount);
    HRESULT WINAPI HookedOleGetClipboard(IDataObject** dataObjectOut);
    HRESULT WINAPI HookedOleSetClipboard(IDataObject* dataObject);
}
