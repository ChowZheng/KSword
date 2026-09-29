/*++

Module Name:

    clipboard_policy_ioctl.c

Abstract:

    剪贴板策略 IOCTL 的派发层：只做安全策略闸门、写权限校验和转发，
    真正的字段校验、存储与查询逻辑放在 clipboard_policy_runtime.c。

Environment:

    Kernel-mode Driver Framework

--*/

// ark_driver.h 聚合了本文件需要的 WDF/协议/安全策略/日志声明。
#include "ark/ark_driver.h"
// 通用 WDF buffer/access 校验，写类 IOCTL 统一走这里。
#include "../../dispatch/ioctl_validation.h"

// RtlStringCbVPrintfA 与可变参数宏声明。
#include <ntstrsafe.h>
#include <stdarg.h>

static VOID
KswordARKClipboardPolicyIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ _Printf_format_string_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    派发层专用的日志格式化辅助函数，与 runtime.c 里那份分开——
    runtime.c 需要 State 指针，而这里在拿到 state 之前就可能要记日志
    （例如安全策略拒绝时 state 还没被用到）。

--*/
{
    // logMessage：单条日志的格式化缓冲区，容量取仓库统一上限。
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    // arguments：可变参数游标。
    va_list arguments;

    // 展开并格式化，成功才入队，失败大概率是调用方传了非法格式串。
    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (void)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKClipboardPolicyIoctlSetConfigHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_SET_CLIPBOARD_POLICY。下发一整张剪贴板策略表会
    改变后续所有被保护进程的行为，因此和其它"整表替换"类操作一样，
    先过中央安全策略闸门，再校验写权限，最后才转发给运行时层。

Arguments:

    Device - WDF 设备，用于安全策略判定与审计日志。
    Request - 携带定长配置包的当前 IOCTL 请求。
    InputBufferLength - 配置包长度。
    OutputBufferLength - 本 IOCTL 不使用输出缓冲区。
    BytesReturned - 成功时回填实际消费的输入字节数。

Return Value:

    安全策略、访问校验或运行时层给出的 NTSTATUS。

--*/
{
    // status：贯穿函数的状态码。
    NTSTATUS status;
    // 本 IOCTL 不产生输出，显式标注未使用避免告警。
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // 出参先判空并清零。
    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    {
        // safetyContext：本次危险操作的判定上下文，整块清零后只填必填字段。
        KSWORD_ARK_SAFETY_CONTEXT safetyContext;
        RtlZeroMemory(&safetyContext, sizeof(safetyContext));
        // Operation 决定了本次判定套用哪条策略允许位和风险等级。
        safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_CLIPBOARD_SET_POLICY;
        // 剪贴板策略不针对单个目标进程，TargetProcessId 填 0 表示"面向配置本身"。
        safetyContext.TargetProcessId = 0UL;
        // 走 UI 下发路径，标记为已由 UI 确认，避免二次弹窗。
        safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
        status = KswordARKSafetyEvaluate(Device, &safetyContext);
        if (!NT_SUCCESS(status)) {
            // 安全策略拒绝：记录一条 Warn 并直接返回，不再往下走。
            KswordARKClipboardPolicyIoctlLog(Device, "Warn", "Clipboard policy denied by safety policy, status=0x%08X.", (unsigned int)status);
            return status;
        }
    }

    // 写类 IOCTL 统一校验调用方句柄是否具备写权限，防止只读句柄绕过 FILE_WRITE_ACCESS。
    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        KswordARKClipboardPolicyIoctlLog(Device, "Warn", "Clipboard policy denied: write access required, status=0x%08X.", (unsigned int)status);
        return status;
    }

    // 两道闸门都通过，转发给运行时层做真正的字段校验与存储。
    status = KswordARKClipboardPolicyIoctlSetConfig(Request, InputBufferLength, BytesReturned);
    if (!NT_SUCCESS(status)) {
        // 运行时层的失败原因已经在 runtime.c 里记过一次更详细的日志，这里只补一条派发层摘要。
        KswordARKClipboardPolicyIoctlLog(Device, "Warn", "Clipboard policy apply failed, status=0x%08X.", (unsigned int)status);
    }
    return status;
}

NTSTATUS
KswordARKClipboardPolicyIoctlQueryStateHandler(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    处理 IOCTL_KSWORD_ARK_QUERY_CLIPBOARD_POLICY。纯只读查询，
    不改变任何状态，因此不需要过安全策略闸门。

Arguments:

    Device - WDF 设备，仅用于失败时的诊断日志。
    Request - 当前 IOCTL 请求。
    InputBufferLength - 本 IOCTL 不使用输入缓冲区。
    OutputBufferLength - 输出响应包的缓冲区长度。
    BytesReturned - 成功时回填响应包字节数。

Return Value:

    运行时层给出的 NTSTATUS。

--*/
{
    // status：贯穿函数的状态码。
    NTSTATUS status;
    // 本 IOCTL 不消费输入缓冲区，显式标注未使用。
    UNREFERENCED_PARAMETER(InputBufferLength);

    // 出参先判空并清零。
    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    // 只读查询直接转发给运行时层。
    status = KswordARKClipboardPolicyIoctlQueryState(Request, OutputBufferLength, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKClipboardPolicyIoctlLog(Device, "Warn", "Clipboard policy state query failed, status=0x%08X.", (unsigned int)status);
    }
    return status;
}
