/*++

Module Name:

    handle_ioctl.c

Abstract:

    IOCTL handlers for KswordARK handle-table inspection operations.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "../../dispatch/ioctl_validation.h"

#include <ntstrsafe.h>
#include <stdarg.h>

// ntddk-based feature headers omit this ntifs export; mirror its WDK declaration.
NTKERNELAPI ULONG IoGetRequestorProcessId(_In_ PIRP Irp);

#define KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE) - sizeof(KSWORD_ARK_HANDLE_ENTRY))

/* Mutation handler: preserve buffered input, check device access and explicit confirmation. */
NTSTATUS
KswordARKHandleIoctlCloseHandle(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
{
    KSWORD_ARK_CLOSE_HANDLE_REQUEST copy; /* Private input survives METHOD_BUFFERED output writes. */
    KSWORD_ARK_CLOSE_HANDLE_RESPONSE* response = NULL; /* Fixed result includes recovery status. */
    KSWORD_ARK_SAFETY_CONTEXT safety = { 0 }; /* Temporary suspension uses the existing suspend policy. */
    PVOID input = NULL; /* WDF-owned request buffer. */
    PVOID output = NULL; /* WDF-owned response buffer. */
    size_t inputBytes = 0U; /* Actual WDF input capacity. */
    size_t outputBytes = 0U; /* Actual WDF output capacity. */
    NTSTATUS status = STATUS_SUCCESS; /* Validation failures complete through the dispatcher. */

    UNREFERENCED_PARAMETER(InputBufferLength); /* Required-size helpers validate actual buffers. */
    UNREFERENCED_PARAMETER(OutputBufferLength); /* Output retrieval validates actual response capacity. */
    *BytesReturned = 0U; /* Do not advertise response bytes before output retrieval. */
    status = KswordARKValidateDeviceIoControlWriteAccess(Request); /* Require a writable device handle. */
    if (!NT_SUCCESS(status)) { /* Reject read-only clients before parsing mutation input. */
        return status; /* Dispatcher completes the denied request. */
    }
    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(copy), &input, &inputBytes); /* Require full identity. */
    if (!NT_SUCCESS(status)) { /* Never interpret a partial packet. */
        return status; /* Preserve the transport validation error. */
    }
    RtlCopyMemory(&copy, input, sizeof(copy)); /* Snapshot before touching the shared output allocation. */
    if (copy.size != sizeof(copy) || copy.version != KSWORD_ARK_CLOSE_HANDLE_VERSION ||
        copy.flags != KSWORD_ARK_CLOSE_HANDLE_FLAG_UI_CONFIRMED ||
        copy.expectedCreateTime100ns == 0ULL || copy.expectedObjectAddress == 0ULL ||
        copy.handleValue == 0ULL || copy.handleValue >= 0x80000000ULL) { /* Reject missing identity and kernel/pseudo handles. */
        return STATUS_INVALID_PARAMETER; /* No backend side effects for malformed input. */
    }
    status = KswordARKValidateUserPid(copy.processId); /* Keep Idle/System out of this mutation path. */
    if (!NT_SUCCESS(status)) { /* Invalid and protected PIDs cannot own an eligible user handle. */
        return status; /* Leave the process unchanged. */
    }
    if (copy.processId == IoGetRequestorProcessId(WdfRequestWdmGetIrp(Request)) ||
        ULongToHandle(copy.processId) == PsGetCurrentProcessId()) { /* Never suspend the requester or executing process. */
        return STATUS_ACCESS_DENIED; /* Prevent self-suspension and closing the active driver channel. */
    }
    safety.Operation = KSWORD_ARK_SAFETY_OPERATION_PROCESS_SUSPEND; /* Close temporarily suspends the owner. */
    safety.TargetProcessId = copy.processId; /* Central policy evaluates the selected owner. */
    safety.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED; /* Packet carried exact confirmation flags. */
    status = KswordARKSafetyEvaluate(Device, &safety); /* Preserve established critical-process policy. */
    if (!NT_SUCCESS(status)) { /* A policy-denied target must not reach the backend. */
        return status; /* Preserve the denial for the client. */
    }
    status = KswordARKRetrieveRequiredOutputBuffer(Request, sizeof(*response), &output, &outputBytes); /* Require the recovery receipt. */
    if (!NT_SUCCESS(status)) { /* Never mutate without room to return the actual outcome. */
        return status; /* No close has occurred yet. */
    }
    response = (KSWORD_ARK_CLOSE_HANDLE_RESPONSE*)output; /* Output allocation is validated. */
    RtlZeroMemory(response, sizeof(*response)); /* Do not expose stale buffered input. */
    response->size = sizeof(*response); /* Advertise the exact fixed-layout response. */
    response->version = KSWORD_ARK_CLOSE_HANDLE_VERSION; /* Independent mutation protocol version. */
    response->closeStatus = KswordARKDriverCloseHandle(&copy, &response->resumeStatus); /* Backend owns cleanup. */
    *BytesReturned = sizeof(*response); /* Both semantic statuses are valid even on close failure. */
    return STATUS_SUCCESS; /* Semantic outcome is returned in the fixed receipt. */
}

static VOID
KswordARKHandleIoctlLog(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR FormatText,
    ...
    )
/*++

Routine Description:

    Format one handle IOCTL diagnostic line. 中文说明：日志失败不影响用户请求，
    这里只保留足够上下文用于排查 capability 和缓冲区问题。

Arguments:

    Device - WDF device that owns the log channel.
    LevelText - Log level string.
    FormatText - printf-style ANSI template.
    ... - Template arguments.

Return Value:

    None.

--*/
{
    CHAR logMessage[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    va_list arguments;

    va_start(arguments, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logMessage, sizeof(logMessage), FormatText, arguments))) {
        (VOID)KswordARKDriverEnqueueLogFrame(Device, LevelText, logMessage);
    }
    va_end(arguments);
}

NTSTATUS
KswordARKHandleIoctlEnumProcessHandles(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_ENUM_PROCESS_HANDLES. 中文说明：请求包必须提供 PID，
    输出包可截断，R3 依据 totalCount/returnedCount 判断是否需要更大缓冲。

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length.
    OutputBufferLength - Caller output length.
    BytesReturned - Receives response bytes written by the feature.

Return Value:

    NTSTATUS from validation or handle-table enumeration.

--*/
{
    KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST* enumRequest = NULL;
    KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST requestSnapshot = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_ENUM_PROCESS_HANDLES_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 enum-handle ioctl: input buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    /* Preserve METHOD_BUFFERED input before the backend clears the shared response buffer. */
    RtlCopyMemory(
        &requestSnapshot,
        inputBuffer,
        sizeof(requestSnapshot));
    enumRequest = &requestSnapshot;
    if (enumRequest->processId == 0UL) {
        KswordARKHandleIoctlLog(Device, "Warn", "R0 enum-handle ioctl: missing pid.");
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE,
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 enum-handle ioctl: output buffer invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverEnumerateProcessHandles(outputBuffer, actualOutputLength, enumRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        if (!KSWORD_ARK_ENUM_HANDLE_STATUS_IS_EXPECTED_CHURN(enumRequest->flags, status)) {
            KswordARKHandleIoctlLog(
                Device,
                "Error",
                "R0 enum-handle failed: pid=%lu, status=0x%08X, outBytes=%Iu.",
                (unsigned long)enumRequest->processId,
                (unsigned int)status,
                *BytesReturned);
        }
        return status;
    }

    if ((enumRequest->flags & KSWORD_ARK_ENUM_HANDLE_FLAG_QUIET_LOG) == 0UL &&
        *BytesReturned >= KSWORD_ARK_HANDLE_ENUM_RESPONSE_HEADER_SIZE) {
        KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE* responseHeader = (KSWORD_ARK_ENUM_PROCESS_HANDLES_RESPONSE*)outputBuffer;
        KswordARKHandleIoctlLog(
            Device,
            "Info",
            "R0 enum-handle success: pid=%lu, total=%lu, returned=%lu, status=%lu, outBytes=%Iu.",
            (unsigned long)responseHeader->processId,
            (unsigned long)responseHeader->totalCount,
            (unsigned long)responseHeader->returnedCount,
            (unsigned long)responseHeader->overallStatus,
            *BytesReturned);
    }

    return status;
}

NTSTATUS
KswordARKHandleIoctlQueryHandleObject(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
/*++

Routine Description:

    Handle IOCTL_KSWORD_ARK_QUERY_HANDLE_OBJECT. 中文说明：该路径只接收 PID 和
    handle value，不接收 object address，避免把展示地址变成操作凭据。

Arguments:

    Device - WDF device used for logging.
    Request - Current IOCTL request.
    InputBufferLength - Caller input length.
    OutputBufferLength - Caller output length.
    BytesReturned - Receives fixed response length.

Return Value:

    NTSTATUS from validation or object query helper.

--*/
{
    KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST* queryRequest = NULL;
    KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST requestSnapshot = { 0 };
    PVOID inputBuffer = NULL;
    PVOID outputBuffer = NULL;
    size_t actualInputLength = 0;
    size_t actualOutputLength = 0;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0;

    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_REQUEST),
        &inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 query-handle-object ioctl: input invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    /* Preserve METHOD_BUFFERED input before the backend clears the shared response buffer. */
    RtlCopyMemory(
        &requestSnapshot,
        inputBuffer,
        sizeof(requestSnapshot));
    queryRequest = &requestSnapshot;
    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE),
        &outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 query-handle-object ioctl: output invalid, status=0x%08X.", (unsigned int)status);
        return status;
    }

    status = KswordARKDriverQueryHandleObject(outputBuffer, actualOutputLength, queryRequest, BytesReturned);
    if (!NT_SUCCESS(status)) {
        KswordARKHandleIoctlLog(Device, "Error", "R0 query-handle-object failed: pid=%lu, handle=0x%I64X, status=0x%08X.", (unsigned long)queryRequest->processId, queryRequest->handleValue, (unsigned int)status);
        return status;
    }

    if ((queryRequest->flags & KSWORD_ARK_QUERY_OBJECT_FLAG_QUIET_LOG) == 0UL &&
        *BytesReturned >= sizeof(KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE)) {
        KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE* response = (KSWORD_ARK_QUERY_HANDLE_OBJECT_RESPONSE*)outputBuffer;
        KswordARKHandleIoctlLog(
            Device,
            "Info",
            "R0 query-handle-object success: pid=%lu, handle=0x%I64X, queryStatus=%lu, proxyStatus=%lu.",
            (unsigned long)response->processId,
            response->handleValue,
            (unsigned long)response->queryStatus,
            (unsigned long)response->proxyStatus);
    }

    return STATUS_SUCCESS;
}
