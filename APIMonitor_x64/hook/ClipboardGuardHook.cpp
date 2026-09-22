#include "pch.h"
#include "ClipboardGuardHook.h"
#include "ClipboardGuardCommon.h"
#include "ClipboardGuardVTableHook.h"

namespace apimon
{
    // ---- 原函数指针定义（声明在 ClipboardGuardHook.h，供 g_bindings[] 与本文件共用）----
    OpenClipboardFn g_openClipboardOriginal = nullptr;
    CloseClipboardFn g_closeClipboardOriginal = nullptr;
    GetClipboardDataFn g_getClipboardDataOriginal = nullptr;
    SetClipboardDataFn g_setClipboardDataOriginal = nullptr;
    EmptyClipboardFn g_emptyClipboardOriginal = nullptr;
    EnumClipboardFormatsFn g_enumClipboardFormatsOriginal = nullptr;
    IsClipboardFormatAvailableFn g_isClipboardFormatAvailableOriginal = nullptr;
    GetPriorityClipboardFormatFn g_getPriorityClipboardFormatOriginal = nullptr;
    OleGetClipboardFn g_oleGetClipboardOriginal = nullptr;
    OleSetClipboardFn g_oleSetClipboardOriginal = nullptr;

    // ---- Hook 记录定义 ----
    InlineHookRecord g_openClipboardHook{};
    InlineHookRecord g_closeClipboardHook{};
    InlineHookRecord g_getClipboardDataHook{};
    InlineHookRecord g_setClipboardDataHook{};
    InlineHookRecord g_emptyClipboardHook{};
    InlineHookRecord g_enumClipboardFormatsHook{};
    InlineHookRecord g_isClipboardFormatAvailableHook{};
    InlineHookRecord g_getPriorityClipboardFormatHook{};
    InlineHookRecord g_oleGetClipboardHook{};
    InlineHookRecord g_oleSetClipboardHook{};

    // HookedOpenClipboard 作用：
    // - 处理：OpenClipboard 只是"声明接下来要用剪贴板"，不搬运任何数据，
    //   按方案不在这一层拦截（拦这里会连正常的复制/粘贴 UI 反馈都破坏掉），
    //   只记录事件，始终放行。
    BOOL WINAPI HookedOpenClipboard(HWND ownerWindow)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_openClipboardOriginal(ownerWindow);
        }
        const BOOL resultValue = g_openClipboardOriginal(ownerWindow);
        ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"OpenClipboard", 0, nullptr, ClipboardPolicyAction::Allow);
        return resultValue;
    }

    // HookedCloseClipboard 作用：释放剪贴板锁，同样不搬运数据，且拦截它会让
    // 调用方永久持有剪贴板、破坏其它程序的剪贴板访问，因此始终放行。
    BOOL WINAPI HookedCloseClipboard()
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_closeClipboardOriginal();
        }
        const BOOL resultValue = g_closeClipboardOriginal();
        ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"CloseClipboard", 0, nullptr, ClipboardPolicyAction::Allow);
        return resultValue;
    }

    // HookedGetClipboardData 作用：
    // - 处理：真正的"读"发生点。命中 BLOCK 策略时完全不调用原函数，直接返回
    //   NULL 并置 ERROR_ACCESS_DENIED，调用方看到的和真的没有该格式数据一样；
    //   放行时才把返回句柄交给 ReportClipboardEvent 尝试取 size/hash。
    HANDLE WINAPI HookedGetClipboardData(const UINT formatValue)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_getClipboardDataOriginal(formatValue);
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Read);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Read, L"User32", L"GetClipboardData", formatValue, nullptr, actionValue);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }

        const HANDLE resultHandle = g_getClipboardDataOriginal(formatValue);
        const DWORD lastError = ::GetLastError();
        ReportClipboardEvent(ClipboardOperationKind::Read, L"User32", L"GetClipboardData", formatValue, resultHandle, actionValue);
        ::SetLastError(lastError);
        return resultHandle;
    }

    // HookedSetClipboardData 作用：真正的"写"发生点之一。命中 BLOCK 时不调用
    // 原函数，返回 NULL（与 SetClipboardData 失败时的公开语义一致）。
    HANDLE WINAPI HookedSetClipboardData(const UINT formatValue, const HANDLE dataHandle)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_setClipboardDataOriginal(formatValue, dataHandle);
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Write);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Write, L"User32", L"SetClipboardData", formatValue, nullptr, actionValue);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }

        const HANDLE resultHandle = g_setClipboardDataOriginal(formatValue, dataHandle);
        const DWORD lastError = ::GetLastError();
        ReportClipboardEvent(ClipboardOperationKind::Write, L"User32", L"SetClipboardData", formatValue, nullptr, actionValue);
        ::SetLastError(lastError);
        return resultHandle;
    }

    // HookedEmptyClipboard 作用：
    // - 处理：清空剪贴板本身就是一种"写"（否则"拦截写"只挡住了 SetClipboardData，
    //   EmptyClipboard 仍能把剪贴板清空，留下一个奇怪的半生效状态），
    //   因此同样受 clipboardWriteAction 管控。
    BOOL WINAPI HookedEmptyClipboard()
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_emptyClipboardOriginal();
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Write);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Write, L"User32", L"EmptyClipboard", 0, nullptr, actionValue);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }

        const BOOL resultValue = g_emptyClipboardOriginal();
        ReportClipboardEvent(ClipboardOperationKind::Write, L"User32", L"EmptyClipboard", 0, nullptr, actionValue);
        return resultValue;
    }

    // HookedEnumClipboardFormats 作用：枚举当前剪贴板已提供的格式列表，
    // 受 clipboardEnumAction 管控；命中 BLOCK 时返回 0（等价于"没有更多格式"）。
    UINT WINAPI HookedEnumClipboardFormats(const UINT formatValue)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_enumClipboardFormatsOriginal(formatValue);
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Enum);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"EnumClipboardFormats", formatValue, nullptr, actionValue);
            ::SetLastError(ERROR_ACCESS_DENIED);
            return 0;
        }

        const UINT resultValue = g_enumClipboardFormatsOriginal(formatValue);
        ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"EnumClipboardFormats", formatValue, nullptr, actionValue);
        return resultValue;
    }

    // HookedIsClipboardFormatAvailable 作用：查询某格式是否存在，
    // 命中 BLOCK 时直接回答 FALSE（"这个格式不存在"），最贴近真实拦截语义。
    BOOL WINAPI HookedIsClipboardFormatAvailable(const UINT formatValue)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_isClipboardFormatAvailableOriginal(formatValue);
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Enum);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"IsClipboardFormatAvailable", formatValue, nullptr, actionValue);
            return FALSE;
        }

        const BOOL resultValue = g_isClipboardFormatAvailableOriginal(formatValue);
        ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"IsClipboardFormatAvailable", formatValue, nullptr, actionValue);
        return resultValue;
    }

    // HookedGetPriorityClipboardFormat 作用：按优先级列表选一个当前可用格式，
    // 命中 BLOCK 时返回 0（公开语义："列表里一个都不可用"）。
    int WINAPI HookedGetPriorityClipboardFormat(UINT* const formatPriorityList, const int formatCount)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_getPriorityClipboardFormatOriginal(formatPriorityList, formatCount);
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Enum);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"GetPriorityClipboardFormat", 0, nullptr, actionValue);
            return 0;
        }

        const int resultValue = g_getPriorityClipboardFormatOriginal(formatPriorityList, formatCount);
        ReportClipboardEvent(ClipboardOperationKind::Enum, L"User32", L"GetPriorityClipboardFormat", 0, nullptr, actionValue);
        return resultValue;
    }

    // HookedOleGetClipboard 作用：
    // - 处理：和 OpenClipboard 同理，本身不搬运数据，只是拿到一个可以查询/读取
    //   剪贴板内容的 IDataObject 接口，因此不在这里拦截、只记录；
    //   真正的读发生在返回对象的 GetData/QueryGetData/EnumFormatEtc 调用上，
    //   所以这里成功后要给返回对象打虚表补丁。
    HRESULT WINAPI HookedOleGetClipboard(IDataObject** const dataObjectOut)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_oleGetClipboardOriginal(dataObjectOut);
        }

        const HRESULT resultValue = g_oleGetClipboardOriginal(dataObjectOut);
        if (SUCCEEDED(resultValue) && dataObjectOut != nullptr && *dataObjectOut != nullptr)
        {
            InstallClipboardDataObjectVTableHooksIfNeeded(*dataObjectOut);
        }
        ReportClipboardEvent(ClipboardOperationKind::Enum, L"Ole32", L"OleGetClipboard", 0, nullptr, ClipboardPolicyAction::Allow);
        return resultValue;
    }

    // HookedOleSetClipboard 作用：
    // - 处理：调用方把自己的 IDataObject 注册成新的剪贴板内容，这一步本身就是
    //   "写"的完成点（后续读者的 GetData 会回调到调用方对象，不是这里的原函数），
    //   受 clipboardWriteAction 管控，命中 BLOCK 时不调用原函数、直接报访问拒绝。
    HRESULT WINAPI HookedOleSetClipboard(IDataObject* const dataObject)
    {
        if (IsInlineHookInternalBypassActive())
        {
            return g_oleSetClipboardOriginal(dataObject);
        }

        const ClipboardPolicyAction actionValue = ResolveClipboardAction(ClipboardOperationKind::Write);
        if (actionValue == ClipboardPolicyAction::Block)
        {
            ReportClipboardEvent(ClipboardOperationKind::Write, L"Ole32", L"OleSetClipboard", 0, nullptr, actionValue);
            return E_ACCESSDENIED;
        }

        const HRESULT resultValue = g_oleSetClipboardOriginal(dataObject);
        ReportClipboardEvent(ClipboardOperationKind::Write, L"Ole32", L"OleSetClipboard", 0, nullptr, actionValue);
        return resultValue;
    }
}
