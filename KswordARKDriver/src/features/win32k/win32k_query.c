/*++

Module Name:

    win32k_query.c

Abstract:

    Read-only win32k GUI audit collectors.

Environment:

    Kernel-mode Driver Framework

--*/

#include "win32k_query.h"
#include "win32k_support.h"
#include "win32k_fallback.h"
#include "../../platform/pool_compat.h"
#include "ark/ark_keyboard.h"

#include <ntstrsafe.h>

#define KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY))

#define KSWORD_ARK_WIN32K_WINDOW_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_WINDOW_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_WINDOW_ENTRY))

#define KSWORD_ARK_WIN32K_GUI_THREAD_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_GUI_THREAD_ENTRY))

#define KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY))

#define KSWORD_ARK_WIN32K_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_WIN32K_HOOK_SNAPSHOT_RESPONSE) - sizeof(KSWORD_ARK_WIN32K_HOOK_ENTRY))

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif

#define KSWORD_ARK_WIN32K_BRIDGE_TAG 'B3WK'

NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId( // 通过公开PID查询取得会话访问所需的稳定进程引用。
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

static NTSTATUS
KswordARKWin32kPrepareProfileHeader(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Outptr_ KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE** ResponseOut,
    _Out_ size_t* EntryCapacityOut
    )
/*++

Routine Description:

    Validate and initialize a Win32k profile/status response header.

Arguments:

    OutputBuffer - METHOD_BUFFERED output.
    OutputBufferLength - Writable output bytes.
    ResponseOut - Receives the response pointer.
    EntryCapacityOut - Receives variable session-entry capacity.

Return Value:

    STATUS_SUCCESS or a buffer validation failure.

--*/
{
    if (OutputBuffer == NULL || ResponseOut == NULL || EntryCapacityOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    *ResponseOut = (KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE*)OutputBuffer;
    *EntryCapacityOut =
        (OutputBufferLength - KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY);
    (*ResponseOut)->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    (*ResponseOut)->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    (*ResponseOut)->entrySize = sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY);
    (*ResponseOut)->lastStatus = STATUS_SUCCESS;
    KswordARKWin32kInitializeOffsets(&(*ResponseOut)->fieldOffsets);
    return STATUS_SUCCESS;
}
static ULONG
KswordARKWin32kMapKeyboardStatus(
    _In_ ULONG KeyboardStatus
    )
/*++

Routine Description:

    Map the legacy keyboard hotkey/hook status space to the Win32k audit status space.

Arguments:

    KeyboardStatus - Status returned by the keyboard enumeration backend.

Return Value:

    A KSWORD_ARK_WIN32K_STATUS_* value that preserves the backend failure meaning.

--*/
{
    switch (KeyboardStatus) {
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK:
        return KSWORD_ARK_WIN32K_STATUS_OK;
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL:
        return KSWORD_ARK_WIN32K_STATUS_PARTIAL;
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED:
        return KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED;
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND:
        return KSWORD_ARK_WIN32K_STATUS_WIN32K_NOT_FOUND;
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_PATTERN_NOT_FOUND:
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE:
        return KSWORD_ARK_WIN32K_STATUS_ENUM_FAILED;
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED:
        return KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED;
    case KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED:
        return KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
    default:
        return KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    }
}

static VOID
KswordARKWin32kFillKeyboardBridgeRequest(
    _Out_ KSWORD_ARK_ENUM_KEYBOARD_REQUEST* KeyboardRequest,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _In_ ULONG DefaultFlags
    )
/*++

Routine Description:

    Convert a Win32k audit request into the existing read-only keyboard request shape.

Arguments:

    KeyboardRequest - Receives the keyboard request packet.
    Request - Optional Win32k request from user mode.
    DefaultFlags - Keyboard enumeration defaults for hotkey or hook mode.

Return Value:

    None. Invalid inputs are ignored by leaving a zeroed request.

--*/
{
    if (KeyboardRequest == NULL) {
        return;
    }

    RtlZeroMemory(KeyboardRequest, sizeof(*KeyboardRequest));
    KeyboardRequest->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    KeyboardRequest->flags = DefaultFlags;
    KeyboardRequest->maxEntries = KSWORD_ARK_WIN32K_DEFAULT_MAX_ENTRIES;
    if (Request != NULL) {
        KeyboardRequest->processId = Request->processId;
        KeyboardRequest->flags &= ~KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS; // 两个协议的位值不同，不能直接复制Win32k flags。
        if ((Request->flags & KSWORD_ARK_WIN32K_QUERY_FLAG_INCLUDE_DIAGNOSTICS) != 0UL) { // 显式翻译诊断位。
            KeyboardRequest->flags |= KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS; // 仅请求诊断时让后端投影诊断行。
        }
        if (Request->processId != 0UL) { // 非零PID既选择会话，也必须限制返回行归属。
            KeyboardRequest->flags |= KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS; // 不再将同一会话其它进程混入结果。
        }
        if (Request->maxEntries != 0UL) {
            KeyboardRequest->maxEntries = KswordARKWin32kNormalizeMaxEntries(Request->maxEntries);
        }
    }
}

// 确认后端实际可选择的单个会话；不把无法到达的指定会话伪装为空成功。
static NTSTATUS
KswordARKWin32kResolveHotkeyBridgeSession(
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ ULONG* SessionIdOut
    )
{
    KSWORD_WIN32K_PS_GET_PROCESS_SESSION_ID_FN getSessionId = KswordARKWin32kResolvePsGetProcessSessionId(); // 复用既有公开导出解析入口。
    PEPROCESS process = NULL; // 指定PID的进程引用只在本函数持有。
    ULONG callerSessionId = 0UL; // CURRENT_SESSION_ONLY始终以原调用者会话为准。
    ULONG selectedSessionId = 0UL; // 后端将查询此进程上下文里的热键表。
    ULONG requestedSessionId = Request != NULL ? Request->sessionId : 0UL; // 零值沿用协议的未指定语义。
    BOOLEAN sessionFiltered = requestedSessionId != 0UL; // 会话零仍可通过CURRENT_SESSION_ONLY明确选择。
    NTSTATUS status = STATUS_SUCCESS; // 公开PID查询失败码直接返回调用者。
    *SessionIdOut = 0UL; // 未确认前不发布会话身份。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || getSessionId == NULL) { // 无法安全确认上下文时不继续枚举。
        return getSessionId == NULL ? STATUS_PROCEDURE_NOT_FOUND : STATUS_INVALID_DEVICE_STATE; // 区分能力缺失与调用级别错误。
    }
    callerSessionId = getSessionId(PsGetCurrentProcess()); // IOCTL沿原InCallerContext调用者获取会话。
    selectedSessionId = callerSessionId; // PID未指定时只查询调用者会话。
    if (Request != NULL && Request->processId != 0UL) { // 非零PID必须能取得真实对象。
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &process); // 不依赖裸进程地址。
        if (!NT_SUCCESS(status)) return status; // 指定PID失效不得换成当前会话。
        selectedSessionId = getSessionId(process); // 只在引用存活期间读取目标会话。
        ObDereferenceObject(process); // 释放本函数取得的唯一进程引用。
    }
    if (Request != NULL && (Request->flags & KSWORD_ARK_WIN32K_QUERY_FLAG_CURRENT_SESSION_ONLY) != 0UL) { // 当前会话位必须真实生效。
        if (sessionFiltered && requestedSessionId != callerSessionId) return STATUS_NOT_SUPPORTED; // 冲突的两个会话约束不能同时满足。
        requestedSessionId = callerSessionId; // 显式固定为原调用者会话。
        sessionFiltered = TRUE; // 同时支持调用者属于session0的情况。
    }
    if (sessionFiltered && requestedSessionId != selectedSessionId) return STATUS_NOT_SUPPORTED; // 单会话后端不能默默查询另一个会话。
    *SessionIdOut = selectedSessionId; // 只有实际可查询的会话身份才交给投影层。
    return STATUS_SUCCESS; // PID/TID行筛选在canonical结果投影阶段继续执行。
}

static VOID
KswordARKWin32kCopyKeyboardDetail(
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars,
    _In_reads_(SourceChars) const WCHAR* Source,
    _In_ ULONG SourceChars,
    _In_z_ PCWSTR Prefix
    )
/*++

Routine Description:

    Build a bounded detail string for a Win32k row converted from keyboard audit data.

Arguments:

    Destination - Win32k protocol detail field.
    DestinationChars - Destination WCHAR capacity.
    Source - Keyboard backend detail field.
    SourceChars - Source WCHAR capacity.
    Prefix - Short conversion prefix.

Return Value:

    None. The destination is always terminated when capacity is nonzero.

--*/
{
    WCHAR sourceBuffer[KSWORD_ARK_KEYBOARD_DETAIL_CHARS];
    ULONG sourceIndex = 0UL;

    if (Destination == NULL || DestinationChars == 0UL) {
        return;
    }

    Destination[0] = L'\0';
    RtlZeroMemory(sourceBuffer, sizeof(sourceBuffer));
    if (Source != NULL && SourceChars != 0UL) {
        for (sourceIndex = 0UL;
            sourceIndex + 1UL < RTL_NUMBER_OF(sourceBuffer) &&
            sourceIndex < SourceChars &&
            Source[sourceIndex] != L'\0';
            ++sourceIndex) {
            sourceBuffer[sourceIndex] = Source[sourceIndex];
        }
    }

    (VOID)RtlStringCchPrintfW(
        Destination,
        DestinationChars,
        L"%ws%ws",
        (Prefix != NULL) ? Prefix : L"",
        sourceBuffer);
    Destination[DestinationChars - 1UL] = L'\0';
}

NTSTATUS
KswordARKWin32kQueryProfileStatus(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Query loaded win32k module state and currently observable GUI sessions.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional filter and budget packet.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS on a completed diagnostic response.

--*/
{
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kbaseEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    BOOLEAN win32kLoaded = FALSE;
    BOOLEAN win32kbaseLoaded = FALSE;
    BOOLEAN win32kfullLoaded = FALSE;
    ULONG64 runtimeCapabilityMask = 0ULL;
    KSWORD_ARK_WIN32K_PROFILE_STATUS_RESPONSE* response = NULL;
    size_t entryCapacity = 0U;
    NTSTATUS status = STATUS_SUCCESS;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    status = KswordARKWin32kPrepareProfileHeader(
        OutputBuffer,
        OutputBufferLength,
        &response,
        &entryCapacity);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (NT_SUCCESS(status)) {
        win32kLoaded = KswordARKWin32kFindModuleByName(moduleInfo, "win32k.sys", &win32kEntry);
        win32kbaseLoaded = KswordARKWin32kFindModuleByName(moduleInfo, "win32kbase.sys", &win32kbaseEntry);
        win32kfullLoaded = KswordARKWin32kFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry);
    }

    KswordARKWin32kFillModuleState(
        &response->win32k,
        L"win32k.sys",
        win32kLoaded,
        win32kLoaded ? &win32kEntry : NULL);
    KswordARKWin32kFillModuleState(
        &response->win32kbase,
        L"win32kbase.sys",
        win32kbaseLoaded,
        win32kbaseLoaded ? &win32kbaseEntry : NULL);
    KswordARKWin32kFillModuleState(
        &response->win32kfull,
        L"win32kfull.sys",
        win32kfullLoaded,
        win32kfullLoaded ? &win32kfullEntry : NULL);

    response->capabilityMask = KswordARKWin32kModuleCapabilityMask(
        win32kLoaded,
        win32kbaseLoaded,
        win32kfullLoaded,
        win32kbaseLoaded ? &win32kbaseEntry : NULL,
        &response->missingCapabilityMask,
        &response->userGetSiloGlobals);
    runtimeCapabilityMask = KswordARKWin32kFallbackProbeCapabilityMask();
    response->capabilityMask |= runtimeCapabilityMask;

    if (!win32kbaseLoaded || !win32kfullLoaded) {
        response->status = KSWORD_ARK_WIN32K_STATUS_WIN32K_NOT_FOUND;
    }
    else if (runtimeCapabilityMask != 0ULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
    }
    else {
        response->status = KSWORD_ARK_WIN32K_STATUS_PROFILE_MISSING;
    }

    KswordARKWin32kCollectSessionSummary(response, entryCapacity, Request);

    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }

    *BytesWrittenOut =
        KSWORD_ARK_WIN32K_PROFILE_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_SESSION_ENTRY));
    UNREFERENCED_PARAMETER(moduleInfoBytes);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKWin32kQueryWindowSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Query windows through the validated runtime-signature fallback when no
    private-symbol profile is available.

--*/
{
    return KswordARKWin32kFallbackQueryWindowSnapshot(
        OutputBuffer,
        OutputBufferLength,
        Request,
        BytesWrittenOut);
}

NTSTATUS
KswordARKWin32kQueryGuiThreadSnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Enumerate public GUI-thread identities and enrich tagQ fields only after
    the NtUserGetGUIThreadInfo-derived layout passes live window validation.

--*/
{
    return KswordARKWin32kFallbackQueryGuiThreadSnapshot(
        OutputBuffer,
        OutputBufferLength,
        Request,
        BytesWrittenOut);
}
NTSTATUS
KswordARKWin32kQueryHotkeySnapshot(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_WIN32K_QUERY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    Return a PDB-gated hotkey snapshot header that coexists with keyboard IOCTLs.

Arguments:

    OutputBuffer - METHOD_BUFFERED output packet.
    OutputBufferLength - Writable output size.
    Request - Optional request; currently used only for flags.
    BytesWrittenOut - Receives bytes written.

Return Value:

    STATUS_SUCCESS with PROFILE_MISSING until hotkey PDB fields are supplied.

--*/
{
    KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE* response = NULL;
    KSWORD_ARK_ENUM_KEYBOARD_REQUEST keyboardRequest;
    KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* keyboardResponse = NULL;
    ULONG entryCapacity = 0UL;
    ULONG copyCount = 0UL;
    ULONG index = 0UL;
    size_t keyboardBufferLength = 0U;
    size_t keyboardBytesWritten = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG selectedSessionId = 0UL; // canonical热键表所属的实际进程会话。

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (OutputBufferLength < KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_WIN32K_HOTKEY_SNAPSHOT_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_WIN32K_PROTOCOL_VERSION;
    response->status = KSWORD_ARK_WIN32K_STATUS_UNKNOWN;
    response->entrySize = sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY);
    response->flags = (Request != NULL) ? Request->flags : 0UL;
    response->lastStatus = STATUS_SUCCESS;
    response->capabilityMask = KSWORD_ARK_WIN32K_CAP_HOTKEY_PROFILE;
    KswordARKWin32kInitializeOffsets(&response->fieldOffsets);
    status = KswordARKWin32kResolveHotkeyBridgeSession(Request, &selectedSessionId); // 会话选择失败必须在读取其它会话前返回。
    if (!NT_SUCCESS(status)) { // 不支持的会话约束与PID查找失败分别保留真实状态。
        response->status = status == STATUS_NOT_SUPPORTED || status == STATUS_PROCEDURE_NOT_FOUND ? KSWORD_ARK_WIN32K_STATUS_UNSUPPORTED : KSWORD_ARK_WIN32K_STATUS_ENUM_FAILED; // 不返回假成功空表。
        response->lastStatus = status; // 供R3展示可恢复的会话或进程错误。
        *BytesWrittenOut = KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE; // 失败响应只包含规范头。
        return STATUS_SUCCESS; // 业务错误沿原协议状态返回。
    }

    entryCapacity = (ULONG)((OutputBufferLength - KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE) /
        sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY));
    KswordARKWin32kFillKeyboardBridgeRequest(
        &keyboardRequest,
        Request,
        KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM |
        KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS);

    keyboardBufferLength = sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE) +
        ((size_t)keyboardRequest.maxEntries * sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY));
    keyboardResponse = (KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE*)KswordARKAllocateNonPagedPool(
        keyboardBufferLength,
        KSWORD_ARK_WIN32K_BRIDGE_TAG);
    if (keyboardResponse == NULL) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        response->lastStatus = STATUS_INSUFFICIENT_RESOURCES;
        *BytesWrittenOut = KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    status = KswordARKDriverEnumerateKeyboardHotkeys(
        keyboardResponse,
        keyboardBufferLength,
        &keyboardRequest,
        &keyboardBytesWritten);
    response->lastStatus = status;
    if (!NT_SUCCESS(status) ||
        keyboardBytesWritten < (sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY))) {
        response->status = KSWORD_ARK_WIN32K_STATUS_READ_FAILED;
        if (NT_SUCCESS(status)) {
            response->lastStatus = STATUS_BUFFER_TOO_SMALL;
        }
        ExFreePoolWithTag(keyboardResponse, KSWORD_ARK_WIN32K_BRIDGE_TAG);
        *BytesWrittenOut = KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    response->totalCount = Request != NULL && Request->threadId != 0UL ? 0UL : keyboardResponse->totalCount; // TID过滤后的数量必须来自实际匹配行。
    response->returnedCount = 0UL;
    response->status = KswordARKWin32kMapKeyboardStatus(keyboardResponse->status);
    response->lastStatus = keyboardResponse->lastStatus;
    if (Request != NULL && Request->processId == 0UL && Request->sessionId == 0UL && // 无明确会话选择的宽查询只能得到当前会话子集。
        (Request->flags & KSWORD_ARK_WIN32K_QUERY_FLAG_CURRENT_SESSION_ONLY) == 0UL && // 后端没有跨会话遍历能力。
        response->status == KSWORD_ARK_WIN32K_STATUS_OK) { // 保留已有更具体的后端失败或截断状态。
        response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL; // 返回有用子集并明确能力边界。
        response->lastStatus = STATUS_NOT_SUPPORTED; // 不声称完成所有会话查询。
    }
    response->fieldOffsets.hotkeyNext = keyboardResponse->hotkeyNextOffset;
    response->fieldOffsets.hotkeyModifiers = keyboardResponse->hotkeyModifiersOffset;
    response->fieldOffsets.hotkeyVirtualKey = keyboardResponse->hotkeyVkOffset;
    response->fieldOffsets.hotkeyId = keyboardResponse->hotkeyIdOffset;

    copyCount = keyboardResponse->returnedCount;
    for (index = 0UL; index < copyCount; ++index) {
        const KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY* sourceEntry = &keyboardResponse->entries[index];
        KSWORD_ARK_WIN32K_HOTKEY_ENTRY* targetEntry = NULL; // 先筛选canonical行，再分配输出槽位。
        if (sourceEntry->sessionId != selectedSessionId) { // PID在预检与实际查询之间退出/复用时，不给另一会话贴旧标签。
            if (response->status == KSWORD_ARK_WIN32K_STATUS_OK) response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL;
            if (NT_SUCCESS(response->lastStatus)) response->lastStatus = STATUS_REVISION_MISMATCH;
            continue;
        }
        if (Request != NULL && // PID与TID筛选都要求明确归属，未知身份不能穿透。
            ((Request->processId != 0UL && sourceEntry->processId == 0UL) || // 后端未知PID仍防御性拒绝。
                (Request->threadId != 0UL && sourceEntry->threadId == 0UL))) { // 未知TID不属于任何已确认线程筛选结果。
            if (response->status == KSWORD_ARK_WIN32K_STATUS_OK) response->status = KSWORD_ARK_WIN32K_STATUS_PARTIAL; // 归属无法确认时保留不完整状态。
            if (NT_SUCCESS(response->lastStatus)) response->lastStatus = STATUS_NOT_FOUND; // 不能把身份缺失藏成正常零行。
            continue; // 诊断状态保留在响应头，未知归属行不展示为匹配项。
        }
        if (Request != NULL && // 普通不匹配行只影响投影，不改变查询成功语义。
            ((Request->processId != 0UL && sourceEntry->processId != Request->processId) || // 显式PID筛选。
                (Request->threadId != 0UL && sourceEntry->threadId != Request->threadId))) { // 显式TID筛选。
            continue; // 同会话其它线程不得混入结果。
        }
        if (Request != NULL && Request->threadId != 0UL) response->totalCount += 1UL; // 统计所有已读取的匹配线程项。
        if (response->returnedCount >= entryCapacity) { // 容量检查放在过滤后，避免提前丢弃后续匹配行。
            response->status = KSWORD_ARK_WIN32K_STATUS_BUFFER_TRUNCATED; // 结果确实超出输出空间时明确截断。
            response->missingCapabilityMask |= KSWORD_ARK_WIN32K_CAP_HOTKEY_PROFILE; // 沿用原截断能力回执。
            if (NT_SUCCESS(response->lastStatus)) response->lastStatus = STATUS_BUFFER_OVERFLOW; // 保留首个失败原因。
            continue; // 继续计数，不写越界槽位。
        }
        targetEntry = &response->entries[response->returnedCount]; // 输出按筛选后的连续槽位写入。

        RtlZeroMemory(targetEntry, sizeof(*targetEntry));
        targetEntry->source = sourceEntry->source;
        targetEntry->status = KswordARKWin32kMapKeyboardStatus(sourceEntry->status);
        targetEntry->flags = sourceEntry->flags;
        targetEntry->processId = sourceEntry->processId;
        targetEntry->threadId = sourceEntry->threadId;
        targetEntry->sessionId = sourceEntry->sessionId; // v4使用实际查询持有进程的SID，不依赖较早的预检快照。
        targetEntry->modifiers = sourceEntry->modifiers;
        targetEntry->virtualKey = sourceEntry->virtualKey;
        targetEntry->hotkeyId = sourceEntry->hotkeyId;
        targetEntry->depth = sourceEntry->depth;
        targetEntry->lastStatus = sourceEntry->lastStatus;
        targetEntry->hotkeyObject = sourceEntry->hotkeyObject;
        targetEntry->nextHotkeyObject = sourceEntry->nextHotkeyObject;
        targetEntry->hwnd = sourceEntry->windowObject;
        targetEntry->tagWnd = sourceEntry->windowObject;
        targetEntry->threadInfo = sourceEntry->threadInfo;
        KswordARKWin32kCopyKeyboardDetail(
            targetEntry->detail,
            KSWORD_ARK_WIN32K_DETAIL_CHARS,
            sourceEntry->detail,
            KSWORD_ARK_KEYBOARD_DETAIL_CHARS,
            L"keyboard bridge: ");
        response->returnedCount += 1UL; // 只有成功投影的匹配行才增加返回数量。
    }

    ExFreePoolWithTag(keyboardResponse, KSWORD_ARK_WIN32K_BRIDGE_TAG);
    *BytesWrittenOut = KSWORD_ARK_WIN32K_HOTKEY_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_WIN32K_HOTKEY_ENTRY));
    return STATUS_SUCCESS;
}
