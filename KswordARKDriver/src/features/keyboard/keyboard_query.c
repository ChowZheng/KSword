/*++

Module Name:

    keyboard_query.c

Abstract:

    Read-only win32k keyboard hotkey and keyboard hook enumeration helpers.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_keyboard.h"
#include "../kernel/hook_scan_support.h"
#include "../kernel/object_header_fallback.h" // 通过公开 ID 查询取得与候选地址精确匹配的线程引用。
#include "keyboard_internal.h"
#include "../../platform/pool_compat.h" // 声明非分页池分配器，确保状态快照分配使用正确的指针返回类型。

#include <ntstrsafe.h>

#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif

#define KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY))

#define KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE \
    (sizeof(KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE) - sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY))

#define KSWORD_ARK_KEYBOARD_HOTKEY_BUCKETS 0x80UL
#define KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT 512UL
#define KSWORD_ARK_KEYBOARD_PROCESS_WALK_LIMIT 4096UL
#define KSWORD_ARK_KEYBOARD_THREAD_WALK_LIMIT 65536UL

// 当前 win32kfull!NtUserSetWindowsHookEx/IsHotKey 形态下的保守默认偏移。
#define KSWORD_ARK_KEYBOARD_HOTKEY_THREADINFO_OFFSET 0x00UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_WINDOW_OFFSET     0x10UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_FLAGS2_OFFSET     0x22UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_CALLBACK_OFFSET   0x08UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_DESTINATION_OFFSET 0x18UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET 0x38UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_OBJECT_SIZE       0x48UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_NEXT_V26100_OFFSET 0x30UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_MODIFIERS_V26100_OFFSET 0x20UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_VK_V26100_OFFSET  0x24UL
#define KSWORD_ARK_KEYBOARD_HOTKEY_PLACEHOLDER_FLAG  0x0100U

#define KSWORD_ARK_HOOK_THREAD_ARRAY_OFFSET        0x3C0UL
#define KSWORD_ARK_HOOK_DESKTOP_INFO_OFFSET        0x1F8UL
#define KSWORD_ARK_HOOK_DESKTOP_ARRAY_OFFSET       0x28UL
#define KSWORD_ARK_HOOK_NEXT_OFFSET                0x28UL
#define KSWORD_ARK_HOOK_TYPE_OFFSET                0x30UL
#define KSWORD_ARK_HOOK_PROCEDURE_OFFSET           0x38UL
#define KSWORD_ARK_HOOK_FLAGS_OFFSET               0x40UL
#define KSWORD_ARK_HOOK_MODULE_ID_OFFSET           0x44UL
#define KSWORD_ARK_HOOK_TARGET_THREAD_INFO_OFFSET  0x48UL

typedef PVOID(NTAPI* KSWORD_W32_GET_USER_SESSION_STATE_FN)(VOID); // Full热键表使用WIN32K用户会话状态，不使用Base的SiloGlobals间接层。

typedef PEPROCESS(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_FN)(
    _In_opt_ PEPROCESS Process
    );

typedef PETHREAD(NTAPI* KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)(
    _In_ PEPROCESS Process,
    _In_opt_ PETHREAD Thread
    );

typedef PVOID(NTAPI* KSWORD_PS_GET_THREAD_WIN32_THREAD_FN)(
    _In_ PETHREAD Thread
    );

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

NTSYSAPI
NTSTATUS
NTAPI
PsLookupProcessByProcessId(
    _In_ HANDLE ProcessId,
    _Outptr_ PEPROCESS* Process
    );

NTKERNELAPI
VOID
KeStackAttachProcess(
    _Inout_ PVOID Process,
    _Out_ PVOID ApcState
    );

NTKERNELAPI
VOID
KeUnstackDetachProcess(
    _In_ PVOID ApcState
    );

static BOOLEAN
KswordARKKeyboardLooksLikeKernelPointer(
    _In_ ULONG_PTR Value
    )
{
#if defined(_M_AMD64)
    return Value >= 0xFFFF000000000000ULL ? TRUE : FALSE;
#else
    return Value >= 0x80000000UL ? TRUE : FALSE;
#endif
}

ULONG64
KswordARKKeyboardHashHotkeyObject(
    _In_reads_bytes_(ObjectBytes) const UCHAR* Object,
    _In_ SIZE_T ObjectBytes
    )
{
    ULONG64 hashValue = 14695981039346656037ULL;
    SIZE_T byteIndex = 0U;

    if (Object == NULL || ObjectBytes == 0U) {
        return 0ULL;
    }

    for (byteIndex = 0U; byteIndex < ObjectBytes; ++byteIndex) {
        hashValue ^= (ULONG64)Object[byteIndex];
        hashValue *= 1099511628211ULL;
    }

    return hashValue;
}

static BOOLEAN
KswordARKKeyboardReadPointer(
    _In_ ULONG_PTR Address,
    _Out_ ULONG_PTR* ValueOut
    )
{
    PVOID pointerValue = NULL;

    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0U;
    if (Address == 0U) {
        return FALSE;
    }

    if (!KswordARKHookReadMemorySafe((const VOID*)Address, &pointerValue, sizeof(pointerValue))) {
        return FALSE;
    }

    *ValueOut = (ULONG_PTR)pointerValue;
    return TRUE;
}

static BOOLEAN
KswordARKKeyboardReadUlong(
    _In_ ULONG_PTR Address,
    _Out_ ULONG* ValueOut
    )
{
    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0UL;
    if (Address == 0U) {
        return FALSE;
    }
    return KswordARKHookReadMemorySafe((const VOID*)Address, ValueOut, sizeof(*ValueOut));
}

static BOOLEAN
KswordARKKeyboardReadUshort(
    _In_ ULONG_PTR Address,
    _Out_ USHORT* ValueOut
    )
{
    if (ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0U;
    if (Address == 0U) {
        return FALSE;
    }
    return KswordARKHookReadMemorySafe((const VOID*)Address, ValueOut, sizeof(*ValueOut));
}

static BOOLEAN
KswordARKKeyboardFindModuleByName(
    _In_ const KSW_HOOK_SYSTEM_MODULE_INFORMATION* ModuleInfo,
    _In_z_ PCSTR ModuleName,
    _Out_ KSW_HOOK_SYSTEM_MODULE_ENTRY* ModuleEntryOut
    )
{
    ULONG moduleIndex = 0UL;

    if (ModuleInfo == NULL || ModuleName == NULL || ModuleEntryOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(ModuleEntryOut, sizeof(*ModuleEntryOut));

    for (moduleIndex = 0UL; moduleIndex < ModuleInfo->NumberOfModules; ++moduleIndex) {
        const KSW_HOOK_SYSTEM_MODULE_ENTRY* moduleEntry = &ModuleInfo->Modules[moduleIndex];
        const UCHAR* fileName = NULL;
        ULONG fileNameBytes = 0UL;

        KswordARKHookGetModuleFileName(moduleEntry, &fileName, &fileNameBytes);
        if (KswordARKHookBoundedAnsiEqualsInsensitive(fileName, fileNameBytes, ModuleName)) {
            RtlCopyMemory(ModuleEntryOut, moduleEntry, sizeof(*ModuleEntryOut));
            return TRUE;
        }
    }

    return FALSE;
}

static KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN
KswordARKKeyboardResolvePsGetNextProcessThread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcessThread");
    return (KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PS_GET_NEXT_PROCESS_FN
KswordARKKeyboardResolvePsGetNextProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetNextProcess");
    return (KSWORD_PS_GET_NEXT_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_PS_GET_THREAD_WIN32_THREAD_FN
KswordARKKeyboardResolvePsGetThreadWin32Thread(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"PsGetThreadWin32Thread");
    return (KSWORD_PS_GET_THREAD_WIN32_THREAD_FN)MmGetSystemRoutineAddress(&routineName);
}

static KSWORD_W32_GET_USER_SESSION_STATE_FN
KswordARKKeyboardResolveUserSessionState(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kEntry
    )
{
    IMAGE_DOS_HEADER dosHeader; // 节表位置来自实际已加载映像的DOS头。
    IMAGE_NT_HEADERS ntHeaders; // PE节与导出目录用于拒绝转发字符串和非代码地址。
    IMAGE_DATA_DIRECTORY exportDirectory; // 导出目录范围只用于排除forwarder。
    ULONG_PTR routineAddress = 0U; // 仅在范围与可执行节校验后转换为可调用函数。
    ULONG routineRva = 0UL; // 导出相对实际WIN32K映像的偏移。
    ULONG sectionTableRva = 0UL; // 节表首项的有界RVA。
    ULONG sectionIndex = 0UL; // 只遍历PE头明确声明的有限节数。
    if (Win32kEntry == NULL || Win32kEntry->ImageBase == NULL || // 空模块不能解析会话入口。
        !KswordARKHookReadImageBytes(Win32kEntry, 0UL, &dosHeader, sizeof(dosHeader)) || // 所有映像读取沿用安全读取器。
        dosHeader.e_lfanew < 0 || !KswordARKHookReadImageNtHeaders(Win32kEntry, &ntHeaders) || // 拒绝无效PE头。
        ntHeaders.FileHeader.NumberOfSections == 0U || ntHeaders.FileHeader.NumberOfSections > 96U) { // 限制损坏节表的遍历预算。
        return NULL; // 解析失败不得退回语义不同的UserGetSiloGlobals。
    }
    routineAddress = (ULONG_PTR)RtlFindExportedRoutineByName(Win32kEntry->ImageBase, "W32GetUserSessionState"); // 精确选择Full实际导入的导出名称。
    if (routineAddress < (ULONG_PTR)Win32kEntry->ImageBase || // 拒绝空地址及落在模块之前的地址。
        routineAddress - (ULONG_PTR)Win32kEntry->ImageBase >= Win32kEntry->ImageSize) { // 转发到其它映像的地址也不接受。
        return NULL; // 只调用当前已加载WIN32K映像内的代码。
    }
    routineRva = (ULONG)(routineAddress - (ULONG_PTR)Win32kEntry->ImageBase); // 上面的映像范围已保证RVA可由ULONG表示。
    if (KswordARKHookGetDataDirectory(&ntHeaders, IMAGE_DIRECTORY_ENTRY_EXPORT, &exportDirectory) && // 有导出目录时显式拒绝forwarder字符串。
        routineRva >= exportDirectory.VirtualAddress && routineRva - exportDirectory.VirtualAddress < exportDirectory.Size) { // 使用减法避免范围终点回绕。
        return NULL; // 导出目录中的字节不是可调用会话函数。
    }
    if (!KswordARKHookAddRvaOffset((ULONG)dosHeader.e_lfanew, 1UL, // 使用共享有界RVA加法定位节表。
            (ULONG)FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + ntHeaders.FileHeader.SizeOfOptionalHeader, &sectionTableRva)) {
        return NULL; // 损坏头部不能构造节表地址。
    }
    for (sectionIndex = 0UL; sectionIndex < ntHeaders.FileHeader.NumberOfSections; ++sectionIndex) { // 逐节验证导出确实位于代码区。
        IMAGE_SECTION_HEADER section; // 单个节头不增加大型内核栈帧。
        ULONG sectionRva = 0UL; // 当前节头的有界映像偏移。
        ULONG sectionBytes = 0UL; // 已映射节范围取虚拟与文件大小的较大值。
        if (!KswordARKHookAddRvaOffset(sectionTableRva, sectionIndex, (ULONG)sizeof(section), &sectionRva) || // 拒绝节表索引溢出。
            !KswordARKHookReadImageBytes(Win32kEntry, sectionRva, &section, sizeof(section))) { // 节头仍通过共享映像读取器读取。
            return NULL; // 不能确认节属性时拒绝调用。
        }
        sectionBytes = section.Misc.VirtualSize > section.SizeOfRawData ? section.Misc.VirtualSize : section.SizeOfRawData; // 同时覆盖正常虚拟节与尾部对齐。
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0UL && // 仅接受可执行节。
            routineRva >= section.VirtualAddress && routineRva - section.VirtualAddress < sectionBytes) { // 导出必须完整落在该映射范围内。
            return (KSWORD_W32_GET_USER_SESSION_STATE_FN)routineAddress; // 代码与模块范围都确认后才发布入口。
        }
    }
    return NULL; // 未命中代码节时保持不可用。
}

// 保留只读枚举的首个失败原因，输出容量不足时不覆盖更具体的截断状态。
static VOID
KswordARKKeyboardMarkHotkeyPartial(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* Response,
    _In_ NTSTATUS FailureStatus
    )
{
    if (Response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) { // 截断状态已有独立客户端语义。
        Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL; // 读失败或无法确认身份都不是成功空结果。
    }
    if (NT_SUCCESS(Response->lastStatus)) { // 保留最先观察到的实际失败。
        Response->lastStatus = FailureStatus; // 供R3区分缺页、损坏链与身份缺失。
    }
}

static BOOLEAN
KswordARKKeyboardResolveIsHotKeyBody(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kfullEntry,
    _Out_ ULONG_PTR* IsHotKeyAddressOut
    )
{
    ULONG_PTR editionAddress = 0U;
    UCHAR bytes[32] = { 0 };
    ULONG byteIndex = 0UL;

    if (Win32kfullEntry == NULL || IsHotKeyAddressOut == NULL) {
        return FALSE;
    }
    *IsHotKeyAddressOut = 0U;

    editionAddress = (ULONG_PTR)RtlFindExportedRoutineByName(
        Win32kfullEntry->ImageBase,
        "EditionIsHotKey");
    if (editionAddress == 0U ||
        editionAddress < (ULONG_PTR)Win32kfullEntry->ImageBase ||
        editionAddress >= ((ULONG_PTR)Win32kfullEntry->ImageBase + Win32kfullEntry->ImageSize)) {
        return FALSE;
    }

    if (!KswordARKHookReadMemorySafe((const VOID*)editionAddress, bytes, sizeof(bytes))) {
        return FALSE;
    }

    for (byteIndex = 0UL; byteIndex + 5UL <= sizeof(bytes); ++byteIndex) {
        if (bytes[byteIndex] == 0xE8U) {
            LONG relativeOffset = 0;
            ULONG_PTR targetAddress = 0U;

            RtlCopyMemory(&relativeOffset, bytes + byteIndex + 1UL, sizeof(relativeOffset));
            targetAddress = editionAddress + byteIndex + 5UL + (LONG_PTR)relativeOffset;
            if (targetAddress >= (ULONG_PTR)Win32kfullEntry->ImageBase &&
                targetAddress < ((ULONG_PTR)Win32kfullEntry->ImageBase + Win32kfullEntry->ImageSize)) {
                *IsHotKeyAddressOut = targetAddress;
                return TRUE;
            }
        }
    }

    return FALSE;
}

static BOOLEAN
KswordARKKeyboardResolveHotkeyLayout(
    _In_ ULONG_PTR IsHotKeyAddress,
    _Out_ ULONG* TableOffsetOut,
    _Out_ ULONG_PTR* TableBaseOut,
    _Out_ ULONG* NextOffsetOut,
    _Out_ ULONG* ModifiersOffsetOut,
    _Out_ ULONG* VkOffsetOut,
    _Out_ ULONG* IdOffsetOut
    )
{
    UCHAR bytes[192] = { 0 };
    ULONG index = 0UL;
    ULONG tableOffset = 0UL;
    ULONG_PTR tableBase = 0U;
    ULONG nextOffset = 0UL;
    ULONG modifiersOffset = 0UL;
    ULONG vkOffset = 0UL;

    if (IsHotKeyAddress == 0U ||
        TableOffsetOut == NULL ||
        TableBaseOut == NULL ||
        NextOffsetOut == NULL ||
        ModifiersOffsetOut == NULL ||
        VkOffsetOut == NULL ||
        IdOffsetOut == NULL) {
        return FALSE;
    }

    *TableOffsetOut = 0UL;
    *TableBaseOut = 0U;
    *NextOffsetOut = 0UL;
    *ModifiersOffsetOut = 0UL;
    *VkOffsetOut = 0UL;
    *IdOffsetOut = 0UL;

    if (!KswordARKHookReadMemorySafe((const VOID*)IsHotKeyAddress, bytes, sizeof(bytes))) {
        return FALSE;
    }

    for (index = 0UL; index + 8UL <= sizeof(bytes); ++index) {
        if (bytes[index] == 0x4AU &&
            bytes[index + 1UL] == 0x8BU &&
            bytes[index + 2UL] == 0xBCU &&
            bytes[index + 3UL] == 0xC0U) {
            RtlCopyMemory(&tableOffset, bytes + index + 4UL, sizeof(tableOffset));
        }
        if (bytes[index] == 0x48U &&
            bytes[index + 1UL] == 0x8DU &&
            bytes[index + 2UL] == 0x1DU) {
            LONG relativeOffset = 0;

            RtlCopyMemory(&relativeOffset, bytes + index + 3UL, sizeof(relativeOffset));
            tableBase = IsHotKeyAddress + index + 7UL + (LONG_PTR)relativeOffset;
        }
        if (bytes[index] == 0x0FU &&
            bytes[index + 1UL] == 0xB7U &&
            bytes[index + 2UL] == 0x47U) {
            modifiersOffset = (ULONG)bytes[index + 3UL];
        }
        if (bytes[index] == 0x0FU &&
            bytes[index + 1UL] == 0xB7U &&
            bytes[index + 2UL] == 0x43U) {
            modifiersOffset = (ULONG)bytes[index + 3UL];
        }
        if (bytes[index] == 0x39U &&
            bytes[index + 1UL] == 0x77U) {
            vkOffset = (ULONG)bytes[index + 2UL];
        }
        if (bytes[index] == 0x39U &&
            bytes[index + 1UL] == 0x7BU) {
            vkOffset = (ULONG)bytes[index + 2UL];
        }
        if (bytes[index] == 0x48U &&
            bytes[index + 1UL] == 0x8BU &&
            bytes[index + 2UL] == 0x7FU) {
            nextOffset = (ULONG)bytes[index + 3UL];
        }
        if (bytes[index] == 0x48U &&
            bytes[index + 1UL] == 0x8BU &&
            bytes[index + 2UL] == 0x5BU) {
            nextOffset = (ULONG)bytes[index + 3UL];
        }
    }

    if ((tableOffset == 0UL && tableBase == 0U) ||
        nextOffset == 0UL ||
        modifiersOffset == 0UL ||
        vkOffset == 0UL) {
        return FALSE;
    }

    *TableOffsetOut = tableOffset;
    *TableBaseOut = tableBase;
    *NextOffsetOut = nextOffset;
    *ModifiersOffsetOut = modifiersOffset;
    *VkOffsetOut = vkOffset;
    *IdOffsetOut = vkOffset + sizeof(ULONG);
    return TRUE;
}

NTSTATUS
KswordARKKeyboardResolveHotkeyRuntime(
    _Out_ KSW_KEYBOARD_HOTKEY_RUNTIME* RuntimeOut
    )
{
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kbaseEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kEntry; // Full热键表的会话入口来自独立WIN32K模块。
    KSWORD_W32_GET_USER_SESSION_STATE_FN userSessionState = NULL; // 不再使用Base的SiloGlobals层。
    NTSTATUS status = STATUS_SUCCESS;

    if (RuntimeOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(RuntimeOut, sizeof(*RuntimeOut));

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    if (!NT_SUCCESS(status) || moduleInfo == NULL || moduleInfoBytes == 0UL) {
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

    if (!KswordARKKeyboardFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry) ||
        !KswordARKKeyboardFindModuleByName(moduleInfo, "win32kbase.sys", &win32kbaseEntry) ||
        !KswordARKKeyboardFindModuleByName(moduleInfo, "win32k.sys", &win32kEntry)) { // 会话导出模块缺失时保持不可用。
        status = STATUS_NOT_FOUND;
        goto Cleanup;
    }

    RuntimeOut->Win32kfullBase = win32kfullEntry.ImageBase;
    RuntimeOut->Win32kfullSize = win32kfullEntry.ImageSize;
    RuntimeOut->Win32kbaseBase = win32kbaseEntry.ImageBase;
    RuntimeOut->Win32kbaseSize = win32kbaseEntry.ImageSize;
    if (!KswordARKKeyboardResolveIsHotKeyBody(
            &win32kfullEntry,
            &RuntimeOut->IsHotKeyAddress) ||
        !KswordARKKeyboardResolveHotkeyLayout(
            RuntimeOut->IsHotKeyAddress,
            &RuntimeOut->TableOffset,
            &RuntimeOut->TableBase,
            &RuntimeOut->NextOffset,
            &RuntimeOut->ModifiersOffset,
            &RuntimeOut->VkOffset,
            &RuntimeOut->IdOffset)) {
        status = STATUS_NOT_FOUND;
        goto Cleanup;
    }

    userSessionState = KswordARKKeyboardResolveUserSessionState(&win32kEntry); // 与Full实际调用的用户会话状态保持一致。
    if (userSessionState != NULL) { // 仅调用已经过映像代码区校验的入口。
        __try {
            RuntimeOut->SessionGlobals = (ULONG_PTR)userSessionState(); // 取得热键表真正使用的会话基址。
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            RuntimeOut->SessionGlobals = 0U;
            goto Cleanup;
        }
    }

    if (RuntimeOut->TableBase == 0U && RuntimeOut->SessionGlobals == 0U) {
        status = STATUS_INVALID_DEVICE_STATE;
        goto Cleanup;
    }
    status = STATUS_SUCCESS;

Cleanup:
    ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(RuntimeOut, sizeof(*RuntimeOut));
    }
    return status;
}

static ULONG
KswordARKKeyboardSnapshotThreadCidOffset(
    VOID
    )
/*++

Routine Description:

    只保留本次热键查询需要的 ETHREAD.Cid 偏移。完整 DynData 状态放在临时池中，
    避免逐条身份查询反复快照，或在已有枚举栈帧上叠加大型状态结构。

Return Value:

    可用的偏移；快照分配失败或 IRQL 不允许时返回缺失哨兵。

--*/
{
    KSW_DYN_STATE* snapshot = NULL; // 大型状态不放到内核调用栈中。
    ULONG threadCidOffset = MAXULONG; // 默认不支持身份读取，仍允许其余热键字段查询。

    if (KeGetCurrentIrql() > APC_LEVEL) { // DynData push lock 和候选读取不允许更高 IRQL。
        return threadCidOffset; // 无需分配或访问全局状态。
    }
    snapshot = (KSW_DYN_STATE*)KswordARKAllocateNonPagedPool(sizeof(*snapshot), KSW_HOOK_SCAN_TAG); // 每次完整查询只分配一次小型状态快照。
    if (snapshot == NULL) { // 内存不足仅使 PID/TID 变为未确认，不改变热键枚举结果。
        return threadCidOffset; // 没有临时分配需要清理。
    }
    KswordARKDynDataSnapshot(snapshot); // 在既有状态锁保护下复制完整状态。
    threadCidOffset = snapshot->Kernel.EtCid; // 仅把稳定的偏移值传给所有行。
    ExFreePoolWithTag(snapshot, KSW_HOOK_SCAN_TAG); // 在访问任何热键或模块之前释放临时池。
    return threadCidOffset; // 身份读取 helper 将继续检查缺失哨兵和安全引用。
}

static VOID
KswordARKKeyboardFillHotkeyThreadIdentity(
    _In_ ULONG_PTR ThreadInfo,
    _In_ ULONG ThreadCidOffset, // 同一热键查询使用一份 DynData.Cid 偏移快照。
    _Out_ ULONG_PTR* ThreadObjectOut,
    _Out_ ULONG* ProcessIdOut,
    _Out_ ULONG* ThreadIdOut
    )
{
    ULONG_PTR threadObject = 0U;
    ULONG_PTR candidateThreadId = 0U; // 安全读取的 ID 仅用于取得公开对象引用。
    NTSTATUS referenceStatus = STATUS_SUCCESS; // 候选引用失败时不调用线程访问器。

    if (ThreadObjectOut != NULL) {
        *ThreadObjectOut = 0U;
    }
    if (ProcessIdOut != NULL) {
        *ProcessIdOut = 0UL;
    }
    if (ThreadIdOut != NULL) {
        *ThreadIdOut = 0UL;
    }

    if (KeGetCurrentIrql() > APC_LEVEL || // 候选快照和公开 PID/TID lookup 仅允许 APC_LEVEL 及以下。
        ThreadInfo == 0U ||
        !KswordARKKeyboardReadPointer(ThreadInfo, &threadObject) ||
        !KswordARKKeyboardLooksLikeKernelPointer(threadObject)) {
        return;
    }

    if (ThreadObjectOut != NULL) {
        *ThreadObjectOut = threadObject;
    }

    if (ThreadCidOffset == 0UL || ThreadCidOffset == MAXULONG || ThreadCidOffset == 0xFFFFUL || // 缺失偏移不能被当成 ETHREAD.Cid 地址。
        PsThreadType == NULL || *PsThreadType == NULL) { // 对象管理器未提供线程类型时保持身份未确认。
        return; // 已保留裸地址诊断，PID/TID 继续为零。
    }
    if (threadObject > MAXULONG_PTR - (2U * sizeof(PVOID)) || // 为 Cid.UniqueProcess/UniqueThread 两个字段完整宽度保留空间。
        (ULONG_PTR)ThreadCidOffset > MAXULONG_PTR - threadObject - (2U * sizeof(PVOID))) { // 防止候选字段地址和读取范围回绕。
        return; // 无法构造安全候选字段地址时不继续 lookup。
    }
    if (!KswordARKKeyboardReadPointer(threadObject + ThreadCidOffset + sizeof(PVOID), &candidateThreadId) || // ETHREAD.Cid.UniqueThread 仍只通过安全读取取得。
        candidateThreadId == 0U) { // 空 ID 无法确认存活线程身份。
        return; // 不以裸 ETHREAD 调用 PsGetThreadId 来弥补读取失败。
    }
    referenceStatus = KswordARKObjectHeaderReferenceObjectSafe( // 先按 ID 引用，再要求地址与类型都精确匹配。
        (PVOID)threadObject, // 该地址在 lookup 完成前仅是诊断候选。
        (HANDLE)candidateThreadId, // ID 回收导致不同地址时公共 helper 会拒绝并释放 lookup 对象。
        *PsThreadType); // 只允许公开线程类型的受保护对象。
    if (!NT_SUCCESS(referenceStatus)) { // 退出、回收、类型不符或 lookup 失败都保留未确认状态。
        return; // 此分支没有取得引用，不释放候选地址。
    }
    if (ProcessIdOut != NULL) { // 公开访问器只作用于本函数已经持有引用的线程。
        *ProcessIdOut = HandleToULong(PsGetThreadProcessId((PETHREAD)threadObject)); // 使用稳定线程对象返回实际所属进程 ID。
    }
    if (ThreadIdOut != NULL) { // 可选输出不影响引用的成对释放。
        *ThreadIdOut = HandleToULong(PsGetThreadId((PETHREAD)threadObject)); // 使用稳定对象返回实际线程 ID。
    }
    ObDereferenceObject((PVOID)threadObject); // 释放公共安全 helper 成功取得的唯一引用。
}

static VOID
KswordARKKeyboardAppendHotkeyEntry(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ ULONG_PTR SessionGlobals,
    _In_ ULONG BucketIndex,
    _In_ ULONG Depth,
    _In_ ULONG_PTR HotkeyObject,
    _In_ ULONG NextOffset,
    _In_ ULONG ModifiersOffset,
    _In_ ULONG VkOffset,
    _In_ ULONG IdOffset,
    _In_ ULONG ThreadCidOffset, // 每次查询的 ETHREAD.Cid 偏移，不在逐行函数中分配状态。
    _In_ ULONG RequestFlags,
    _In_ ULONG FilterProcessId
    )
{
    KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY tempEntry;
    USHORT modifiers = 0U;
    USHORT flags2 = 0U;
    ULONG_PTR nextHotkey = 0U;
    ULONG_PTR threadInfo = 0U;
    ULONG_PTR threadObject = 0U;
    ULONG_PTR windowObject = 0U;
    UCHAR objectBytes[KSWORD_ARK_KEYBOARD_HOTKEY_OBJECT_SIZE] = { 0 };
    ULONG_PTR callbackAddress = 0U;
    ULONG_PTR destinationHandle = 0U;
    ULONG_PTR childListFlink = 0U;
    ULONG_PTR childListBlink = 0U;

    if (Response == NULL || HotkeyObject == 0U) {
        return;
    }

    RtlZeroMemory(&tempEntry, sizeof(tempEntry));
    tempEntry.source = KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE;
    tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    tempEntry.bucketIndex = BucketIndex;
    tempEntry.depth = Depth;
    tempEntry.hotkeyObject = (ULONG64)HotkeyObject;
    tempEntry.sessionGlobals = (ULONG64)SessionGlobals;

    if (!KswordARKKeyboardReadPointer(HotkeyObject + NextOffset, &nextHotkey)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
    }
    if (!KswordARKKeyboardReadPointer(HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_THREADINFO_OFFSET, &threadInfo)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
    }
    if (!KswordARKKeyboardReadPointer(HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_WINDOW_OFFSET, &windowObject)) { // 窗口字段失败不能伪装为正常的无窗口热键。
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL; // 后续诊断筛选前统一发布到响应状态。
    }
    if (!KswordARKKeyboardReadUshort(HotkeyObject + ModifiersOffset, &modifiers)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
    }
    if (!KswordARKKeyboardReadUshort(HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_FLAGS2_OFFSET, &flags2)) {
        flags2 = 0U;
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED; // 标志读取失败不能伪装为普通零标志项。
    }
    if (!KswordARKKeyboardReadUlong(HotkeyObject + VkOffset, &tempEntry.virtualKey)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
    }
    if (!KswordARKKeyboardReadUlong(HotkeyObject + IdOffset, &tempEntry.hotkeyId)) {
        tempEntry.hotkeyId = 0UL;
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL; // 未读到ID时仍保留真实诊断状态。
    }

    tempEntry.nextHotkeyObject = (ULONG64)nextHotkey;
    tempEntry.threadInfo = (ULONG64)threadInfo;
    tempEntry.windowObject = (ULONG64)windowObject;
    tempEntry.modifiers = (ULONG)modifiers;
    tempEntry.modifierFlags2 = (ULONG)flags2;
    tempEntry.windowHandle = (ULONG64)windowObject;
    tempEntry.entryFlags = KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_MAIN;
    if (NextOffset == KSWORD_ARK_KEYBOARD_HOTKEY_NEXT_V26100_OFFSET &&
        ModifiersOffset == KSWORD_ARK_KEYBOARD_HOTKEY_MODIFIERS_V26100_OFFSET &&
        VkOffset == KSWORD_ARK_KEYBOARD_HOTKEY_VK_V26100_OFFSET &&
        IdOffset == (KSWORD_ARK_KEYBOARD_HOTKEY_VK_V26100_OFFSET + sizeof(ULONG)) &&
        KswordARKHookReadMemorySafe(
            (const VOID*)HotkeyObject,
            objectBytes,
            sizeof(objectBytes))) {
        RtlCopyMemory(
            &callbackAddress,
            objectBytes + KSWORD_ARK_KEYBOARD_HOTKEY_CALLBACK_OFFSET,
            sizeof(callbackAddress));
        RtlCopyMemory(
            &destinationHandle,
            objectBytes + KSWORD_ARK_KEYBOARD_HOTKEY_DESTINATION_OFFSET,
            sizeof(destinationHandle));
        RtlCopyMemory(
            &childListFlink,
            objectBytes + KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET,
            sizeof(childListFlink));
        RtlCopyMemory(
            &childListBlink,
            objectBytes + KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET + sizeof(PVOID),
            sizeof(childListBlink));
        tempEntry.callbackAddress = (ULONG64)callbackAddress;
        tempEntry.destinationHandle = (ULONG64)destinationHandle;
        tempEntry.childListFlink = (ULONG64)childListFlink;
        tempEntry.childListBlink = (ULONG64)childListBlink;
        tempEntry.objectSize = (ULONG)sizeof(objectBytes);
        tempEntry.snapshotHash = KswordARKKeyboardHashHotkeyObject(objectBytes, sizeof(objectBytes));
        if (childListFlink != (HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET) ||
            childListBlink != (HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET)) {
            tempEntry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_HAS_CHILDREN;
        }
        if ((flags2 & KSWORD_ARK_KEYBOARD_HOTKEY_PLACEHOLDER_FLAG) != 0U) {
            tempEntry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_PLACEHOLDER;
        }
        if (callbackAddress != 0U) {
            tempEntry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_CALLBACK;
        }
        if (threadInfo != 0U && callbackAddress == 0U && flags2 == 0U &&
            tempEntry.virtualKey != 0UL && tempEntry.virtualKey <= 0xFFUL &&
            (tempEntry.modifiers & ~0x0000400FUL) == 0UL &&
            childListFlink == (HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET) &&
            childListBlink == (HotkeyObject + KSWORD_ARK_KEYBOARD_HOTKEY_CHILD_LIST_OFFSET) &&
            BucketIndex == (tempEntry.virtualKey & (KSWORD_ARK_KEYBOARD_HOTKEY_BUCKETS - 1UL))) {
            tempEntry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_MUTABLE;
        }
    }
    KswordARKKeyboardFillHotkeyThreadIdentity(
        threadInfo,
        ThreadCidOffset, // 候选线程只通过这份偏移安全读取 ID。
        &threadObject,
        &tempEntry.processId,
        &tempEntry.threadId);
    tempEntry.threadObject = (ULONG64)threadObject;
    if (tempEntry.status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK || // 读取失败先发布到整体响应，不能被后续筛选抹掉。
        (threadInfo != 0U && tempEntry.processId == 0UL)) { // 有安装线程但未确认PID属于身份缺失诊断。
        tempEntry.lastStatus = tempEntry.status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK ? STATUS_PARTIAL_COPY : STATUS_NOT_FOUND; // 区分字段读取与归属查询失败。
        if (tempEntry.status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK) tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL; // 身份未知不得作为正常进程行。
        KswordARKKeyboardMarkHotkeyPartial(Response, tempEntry.lastStatus); // 即使诊断行不展示也保留不完整结果语义。
    }
    if ((RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS) != 0UL &&
        FilterProcessId != 0UL &&
        tempEntry.processId != FilterProcessId) {
        return;
    }
    if ((RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM) == 0UL && threadInfo == 0U) { // 无安装线程的系统项只在显式包含系统时展示。
        return; // 不把系统项混入普通进程列表。
    }
    if ((RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS) == 0UL && // 普通查询不展示读取失败或占位诊断行。
        (tempEntry.status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK || // 失败状态已在整体响应中保留。
            (tempEntry.entryFlags & KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_PLACEHOLDER) != 0UL)) { // 占位项不是独立注册热键。
        return; // 诊断筛选只控制投影，不改变失败回执。
    }

    if (Response->totalCount < MaxEntries) {
        Response->totalCount += 1UL;
    }

    if (Response->returnedCount >= EntryCapacity) {
        Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED;
        return;
    }

    (VOID)RtlStringCchPrintfW(
        tempEntry.detail,
        KSWORD_ARK_KEYBOARD_DETAIL_CHARS,
        L"bucket=%lu depth=%lu",
        BucketIndex,
        Depth);

    RtlCopyMemory(
        &Response->entries[Response->returnedCount],
        &tempEntry,
        sizeof(tempEntry));
    Response->returnedCount += 1UL;
}

static BOOLEAN
KswordARKKeyboardHookAlreadySeen(
    _In_ const KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG_PTR HookObject
    )
{
    ULONG index = 0UL;

    if (Response == NULL || HookObject == 0U) {
        return FALSE;
    }

    for (index = 0UL; index < Response->returnedCount; ++index) {
        if ((ULONG_PTR)Response->entries[index].hookObject == HookObject) {
            return TRUE;
        }
    }

    return FALSE;
}

static VOID
KswordARKKeyboardAppendHookEntry(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ ULONG Source,
    _In_ ULONG HookScope,
    _In_ ULONG_PTR ChainHead,
    _In_ ULONG_PTR HookObject,
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId
    )
{
    KSWORD_ARK_KEYBOARD_HOOK_ENTRY tempEntry;
    ULONG hookType = 0UL;
    ULONG hookFlags = 0UL;
    ULONG moduleId = 0UL;
    ULONG_PTR nextHook = 0U;
    ULONG_PTR procedureOffset = 0U;
    ULONG_PTR targetThreadInfo = 0U;

    if (Response == NULL || HookObject == 0U) {
        return;
    }

    if (KswordARKKeyboardHookAlreadySeen(Response, HookObject)) {
        return;
    }

    RtlZeroMemory(&tempEntry, sizeof(tempEntry));
    tempEntry.source = Source;
    tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    tempEntry.hookScope = HookScope;
    tempEntry.processId = ProcessId;
    tempEntry.threadId = ThreadId;
    tempEntry.hookObject = (ULONG64)HookObject;
    tempEntry.chainHead = (ULONG64)ChainHead;

    if (!KswordARKKeyboardReadUlong(HookObject + KSWORD_ARK_HOOK_TYPE_OFFSET, &hookType)) {
        tempEntry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
    }
    if (hookType != KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD &&
        hookType != KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD_LL) {
        return;
    }

    if (Response->totalCount < MaxEntries) {
        Response->totalCount += 1UL;
    }

    if (Response->returnedCount >= EntryCapacity) {
        Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED;
        return;
    }

    (VOID)KswordARKKeyboardReadPointer(HookObject + KSWORD_ARK_HOOK_NEXT_OFFSET, &nextHook);
    (VOID)KswordARKKeyboardReadUlong(HookObject + KSWORD_ARK_HOOK_FLAGS_OFFSET, &hookFlags);
    (VOID)KswordARKKeyboardReadUlong(HookObject + KSWORD_ARK_HOOK_MODULE_ID_OFFSET, &moduleId);
    (VOID)KswordARKKeyboardReadPointer(HookObject + KSWORD_ARK_HOOK_PROCEDURE_OFFSET, &procedureOffset);
    (VOID)KswordARKKeyboardReadPointer(HookObject + KSWORD_ARK_HOOK_TARGET_THREAD_INFO_OFFSET, &targetThreadInfo);

    tempEntry.hookType = hookType;
    tempEntry.flags = hookFlags;
    tempEntry.moduleId = moduleId;
    tempEntry.nextHookObject = (ULONG64)nextHook;
    tempEntry.procedureOffset = (ULONG64)procedureOffset;
    tempEntry.targetThreadInfo = (ULONG64)targetThreadInfo;
    (VOID)RtlStringCchPrintfW(
        tempEntry.detail,
        KSWORD_ARK_KEYBOARD_DETAIL_CHARS,
        L"scope=%lu chain=0x%p",
        HookScope,
        (PVOID)ChainHead);

    RtlCopyMemory(
        &Response->entries[Response->returnedCount],
        &tempEntry,
        sizeof(tempEntry));
    Response->returnedCount += 1UL;
}

static VOID
KswordARKKeyboardWalkHookChain(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ ULONG Source,
    _In_ ULONG HookScope,
    _In_ ULONG_PTR ChainHead,
    _In_ ULONG ProcessId,
    _In_ ULONG ThreadId
    )
{
    ULONG_PTR hookObject = 0U;
    ULONG depth = 0UL;

    if (Response == NULL || ChainHead == 0U) {
        return;
    }

    if (!KswordARKKeyboardReadPointer(ChainHead, &hookObject)) {
        return;
    }

    while (hookObject != 0U && depth < KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) {
        ULONG_PTR nextHook = 0U;

        KswordARKKeyboardAppendHookEntry(
            Response,
            EntryCapacity,
            MaxEntries,
            Source,
            HookScope,
            ChainHead,
            hookObject,
            ProcessId,
            ThreadId);

        if (!KswordARKKeyboardReadPointer(hookObject + KSWORD_ARK_HOOK_NEXT_OFFSET, &nextHook)) {
            break;
        }
        if (nextHook == hookObject) {
            break;
        }
        hookObject = nextHook;
        ++depth;
    }
}

static VOID
KswordARKKeyboardEnumerateHookChainsForThread(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ PETHREAD ThreadObject,
    _In_ ULONG ProcessId,
    _In_ KSWORD_PS_GET_THREAD_WIN32_THREAD_FN PsGetThreadWin32Thread,
    _In_ ULONG Flags
    )
{
    static const ULONG hookTypes[] = {
        KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD,
        KSWORD_ARK_KEYBOARD_HOOK_TYPE_KEYBOARD_LL
    };
    PVOID threadInfoPointer = NULL;
    ULONG_PTR threadInfo = 0U;
    ULONG threadId = 0UL;
    ULONG index = 0UL;

    if (Response == NULL || ThreadObject == NULL || PsGetThreadWin32Thread == NULL) {
        return;
    }

    __try {
        threadInfoPointer = PsGetThreadWin32Thread(ThreadObject);
        threadId = HandleToULong(PsGetThreadId(ThreadObject));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        threadInfoPointer = NULL;
    }

    threadInfo = (ULONG_PTR)threadInfoPointer;
    if (threadInfo == 0U) {
        return;
    }

    for (index = 0UL; index < RTL_NUMBER_OF(hookTypes); ++index) {
        ULONG hookType = hookTypes[index];
        ULONG_PTR chainHead = 0U;

        if ((Flags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS) != 0UL) {
            chainHead = threadInfo +
                KSWORD_ARK_HOOK_THREAD_ARRAY_OFFSET +
                ((ULONG_PTR)(hookType + 1UL) * sizeof(PVOID));
            KswordARKKeyboardWalkHookChain(
                Response,
                EntryCapacity,
                MaxEntries,
                KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_THREAD_HOOK_CHAIN,
                KSWORD_ARK_KEYBOARD_HOOK_SCOPE_THREAD,
                chainHead,
                ProcessId,
                threadId);
        }

        if ((Flags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS) != 0UL) {
            ULONG_PTR desktopInfo = 0U;

            if (KswordARKKeyboardReadPointer(threadInfo + KSWORD_ARK_HOOK_DESKTOP_INFO_OFFSET, &desktopInfo) &&
                desktopInfo != 0U) {
                chainHead = desktopInfo +
                    KSWORD_ARK_HOOK_DESKTOP_ARRAY_OFFSET +
                    ((ULONG_PTR)(hookType + 1UL) * sizeof(PVOID));
                KswordARKKeyboardWalkHookChain(
                    Response,
                    EntryCapacity,
                    MaxEntries,
                    KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_GLOBAL_HOOK_CHAIN,
                    KSWORD_ARK_KEYBOARD_HOOK_SCOPE_GLOBAL,
                    chainHead,
                    ProcessId,
                    threadId);
            }
        }
    }
}

static VOID
KswordARKKeyboardEnumerateHookChainsForProcess(
    _Inout_ KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* Response,
    _In_ ULONG EntryCapacity,
    _In_ ULONG MaxEntries,
    _In_ PEPROCESS ProcessObject,
    _In_ KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN PsGetNextProcessThread,
    _In_ KSWORD_PS_GET_THREAD_WIN32_THREAD_FN PsGetThreadWin32Thread,
    _In_ ULONG Flags
    )
{
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    BOOLEAN attached = FALSE;
    PETHREAD threadCursor = NULL;
    ULONG processId = 0UL;
    ULONG threadWalkCount = 0UL;

    if (Response == NULL ||
        ProcessObject == NULL ||
        PsGetNextProcessThread == NULL ||
        PsGetThreadWin32Thread == NULL) {
        return;
    }

    processId = HandleToULong(PsGetProcessId(ProcessObject));
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        RtlZeroMemory(attachState, sizeof(attachState));
        KeStackAttachProcess((PVOID)ProcessObject, attachState);
        attached = TRUE;
    }

    threadCursor = PsGetNextProcessThread(ProcessObject, NULL);
    while (threadCursor != NULL &&
        threadWalkCount < KSWORD_ARK_KEYBOARD_THREAD_WALK_LIMIT &&
        Response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) {
        PETHREAD nextThread = PsGetNextProcessThread(ProcessObject, threadCursor);

        KswordARKKeyboardEnumerateHookChainsForThread(
            Response,
            EntryCapacity,
            MaxEntries,
            threadCursor,
            processId,
            PsGetThreadWin32Thread,
            Flags);

        ObDereferenceObject(threadCursor);
        threadCursor = nextThread;
        threadWalkCount += 1UL;
    }

    if (threadCursor != NULL) {
        ObDereferenceObject(threadCursor);
        threadCursor = NULL;
        if (Response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) {
            Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
            Response->lastStatus = STATUS_BUFFER_OVERFLOW;
        }
    }

    if (attached) {
        KeUnstackDetachProcess(attachState);
    }
}

NTSTATUS
KswordARKDriverEnumerateKeyboardHotkeys(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kEntry; // 热键会话状态从WIN32K模块解析。
    KSWORD_W32_GET_USER_SESSION_STATE_FN userSessionState = NULL; // 与Full热键代码使用相同的状态层。
    ULONG requestFlags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM;
    ULONG maxEntries = 1024UL;
    ULONG entryCapacity = 0UL;
    ULONG_PTR isHotKeyAddress = 0U;
    ULONG_PTR sessionGlobals = 0U;
    ULONG_PTR tableBase = 0U;
    ULONG tableOffset = 0UL;
    ULONG nextOffset = 0UL;
    ULONG modifiersOffset = 0UL;
    ULONG vkOffset = 0UL;
    ULONG idOffset = 0UL;
    ULONG threadCidOffset = MAXULONG; // 缺失时保持 PID/TID 为零并保留其他热键诊断。
    PEPROCESS attachProcess = NULL;
    DECLSPEC_ALIGN(16) UCHAR attachState[128];
    BOOLEAN attached = FALSE;
    BOOLEAN referencedProcess = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bucketIndex = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Request != NULL) {
        requestFlags = Request->flags;
        if (Request->maxEntries != 0UL) {
            maxEntries = Request->maxEntries;
        }
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY);
    response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
    response->flags = requestFlags;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { // 会话导出和进程附加必须在可等待上下文执行。
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE; // 不允许跳过附加后查询其它会话。
        response->lastStatus = STATUS_INVALID_DEVICE_STATE; // 返回真实的上下文错误。
        goto Cleanup; // 尚未取得引用时走统一安全清理。
    }
    entryCapacity = (ULONG)((OutputBufferLength - KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY));
    if (entryCapacity > maxEntries) {
        entryCapacity = maxEntries;
    }

    threadCidOffset = KswordARKKeyboardSnapshotThreadCidOffset(); // 只在枚举开始时获取一次偏移，且不增加大型栈帧。
    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (!NT_SUCCESS(status) || moduleInfo == NULL) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND;
        *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE;
        return STATUS_SUCCESS;
    }

    if (!KswordARKKeyboardFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry) ||
        !KswordARKKeyboardFindModuleByName(moduleInfo, "win32k.sys", &win32kEntry)) { // 缺少正确状态模块时不替换为Base导出。
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND;
        response->lastStatus = STATUS_NOT_FOUND; // 模块快照成功不等于所需模块存在。
        goto Cleanup;
    }

    response->win32kBase = (ULONG64)(ULONG_PTR)win32kfullEntry.ImageBase;
    userSessionState = KswordARKKeyboardResolveUserSessionState(&win32kEntry); // 只采用实际Full导入的用户会话入口。
    if (!KswordARKKeyboardResolveIsHotKeyBody(&win32kfullEntry, &isHotKeyAddress) ||
        !KswordARKKeyboardResolveHotkeyLayout(
            isHotKeyAddress,
            &tableOffset,
            &tableBase,
            &nextOffset,
            &modifiersOffset,
            &vkOffset,
            &idOffset)) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PATTERN_NOT_FOUND;
        response->lastStatus = STATUS_NOT_FOUND;
        goto Cleanup;
    }

    if (tableBase == 0U && userSessionState == NULL) { // 没有直接表地址时必须能取得正确会话基址。
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE;
        response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        goto Cleanup;
    }

    response->tableOffset = tableOffset;
    response->hotkeyNextOffset = nextOffset;
    response->hotkeyModifiersOffset = modifiersOffset;
    response->hotkeyVkOffset = vkOffset;
    response->hotkeyIdOffset = idOffset;

    if (Request != NULL && Request->processId != 0UL) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &attachProcess);
        response->lastStatus = status;
        if (!NT_SUCCESS(status)) {
            attachProcess = NULL;
            response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED; // 指定PID失效时不能退回当前调用者会话。
            goto Cleanup; // 保留PsLookupProcessByProcessId的真实失败码。
        }
        else {
            referencedProcess = TRUE;
        }
    }

    if (attachProcess != NULL) { // 上方已经验证PASSIVE_LEVEL，指定PID必须实际完成附加。
        RtlZeroMemory(attachState, sizeof(attachState));
        KeStackAttachProcess((PVOID)attachProcess, attachState);
        attached = TRUE;
    }

    if (userSessionState != NULL) { // 使用与Full表访问相同层级的基址。
        __try {
            sessionGlobals = (ULONG_PTR)userSessionState(); // 当前已处于请求选择的进程上下文。
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            sessionGlobals = 0U;
            response->lastStatus = GetExceptionCode();
            response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE; // 入口执行失败时不能以直接表地址掩盖上下文错误。
            goto Cleanup; // 保留真实异常状态并成对解除附加。
        }
    }

    if (tableBase == 0U && sessionGlobals == 0U) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE;
        if (NT_SUCCESS(response->lastStatus)) response->lastStatus = STATUS_INVALID_DEVICE_STATE; // 空会话状态不是正常空表。
        goto Cleanup;
    }
    response->sessionGlobals = (ULONG64)sessionGlobals;

    for (bucketIndex = 0UL; bucketIndex < KSWORD_ARK_KEYBOARD_HOTKEY_BUCKETS; ++bucketIndex) {
        ULONG_PTR listHeadAddress = ((tableBase != 0U) ? tableBase : (sessionGlobals + tableOffset)) +
            ((ULONG_PTR)bucketIndex * sizeof(PVOID));
        ULONG_PTR hotkeyObject = 0U;
        ULONG depth = 0UL;

        if (!KswordARKKeyboardReadPointer(listHeadAddress, &hotkeyObject)) {
            KswordARKKeyboardMarkHotkeyPartial(response, STATUS_PARTIAL_COPY); // 桶头读取失败不得变成成功零行。
            continue;
        }

        while (hotkeyObject != 0U && depth < KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) {
            ULONG_PTR nextHotkey = 0U;

            KswordARKKeyboardAppendHotkeyEntry(
                response,
                entryCapacity,
                maxEntries,
                sessionGlobals,
                bucketIndex,
                depth,
                hotkeyObject,
                nextOffset,
                modifiersOffset,
                vkOffset,
                idOffset,
                threadCidOffset, // 每个热键使用同一查询快照的 Cid 偏移。
                requestFlags,
                (Request != NULL) ? Request->processId : 0UL);

            if (!KswordARKKeyboardReadPointer(hotkeyObject + nextOffset, &nextHotkey)) {
                KswordARKKeyboardMarkHotkeyPartial(response, STATUS_PARTIAL_COPY); // 链断读保留已取得行并标记不完整。
                break;
            }
            if (nextHotkey == hotkeyObject) {
                KswordARKKeyboardMarkHotkeyPartial(response, STATUS_DATA_ERROR); // 自环是损坏或竞争快照，不是链尾。
                break;
            }
            hotkeyObject = nextHotkey;
            ++depth;
        }
        if (hotkeyObject != 0U && depth >= KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) { // 多节点环及超长链都受既有预算限制。
            KswordARKKeyboardMarkHotkeyPartial(response, STATUS_BUFFER_OVERFLOW); // 达到预算明确报告部分结果。
        }
    }

    if (response->status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN) { // 不覆盖读取失败、身份缺失或截断状态。
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    }

Cleanup:
    if (attached) {
        KeUnstackDetachProcess(attachState);
    }
    if (referencedProcess && attachProcess != NULL) {
        ObDereferenceObject(attachProcess);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }

    *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnumerateKeyboardHooks(
    _Out_writes_bytes_to_(OutputBufferLength, *BytesWrittenOut) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_opt_ const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
{
    KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE* response = NULL;
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* moduleInfo = NULL;
    ULONG moduleInfoBytes = 0UL;
    KSW_HOOK_SYSTEM_MODULE_ENTRY win32kfullEntry;
    KSWORD_PS_GET_NEXT_PROCESS_FN psGetNextProcess = NULL;
    KSWORD_PS_GET_NEXT_PROCESS_THREAD_FN psGetNextProcessThread = NULL;
    KSWORD_PS_GET_THREAD_WIN32_THREAD_FN psGetThreadWin32Thread = NULL;
    ULONG requestFlags = KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_THREAD_HOOKS |
        KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_GLOBAL_HOOKS;
    ULONG maxEntries = 1024UL;
    ULONG entryCapacity = 0UL;
    PEPROCESS processObject = NULL;
    PEPROCESS processCursor = NULL;
    BOOLEAN referencedProcess = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG processWalkCount = 0UL;

    if (OutputBuffer == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Request != NULL) {
        requestFlags = Request->flags;
        if (Request->maxEntries != 0UL) {
            maxEntries = Request->maxEntries;
        }
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    response = (KSWORD_ARK_ENUM_KEYBOARD_HOOKS_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    response->entrySize = sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY);
    response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN;
    response->flags = requestFlags;
    response->threadHookArrayOffset = KSWORD_ARK_HOOK_THREAD_ARRAY_OFFSET;
    response->desktopInfoOffset = KSWORD_ARK_HOOK_DESKTOP_INFO_OFFSET;
    response->desktopHookArrayOffset = KSWORD_ARK_HOOK_DESKTOP_ARRAY_OFFSET;
    response->hookNextOffset = KSWORD_ARK_HOOK_NEXT_OFFSET;
    response->hookTypeOffset = KSWORD_ARK_HOOK_TYPE_OFFSET;
    response->hookProcedureOffset = KSWORD_ARK_HOOK_PROCEDURE_OFFSET;
    response->hookFlagsOffset = KSWORD_ARK_HOOK_FLAGS_OFFSET;
    response->hookModuleIdOffset = KSWORD_ARK_HOOK_MODULE_ID_OFFSET;
    response->hookTargetThreadInfoOffset = KSWORD_ARK_HOOK_TARGET_THREAD_INFO_OFFSET;
    entryCapacity = (ULONG)((OutputBufferLength - KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY));
    if (entryCapacity > maxEntries) {
        entryCapacity = maxEntries;
    }

    psGetNextProcess = KswordARKKeyboardResolvePsGetNextProcess();
    psGetNextProcessThread = KswordARKKeyboardResolvePsGetNextProcessThread();
    psGetThreadWin32Thread = KswordARKKeyboardResolvePsGetThreadWin32Thread();
    if (psGetNextProcessThread == NULL || psGetThreadWin32Thread == NULL) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED;
        response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
        goto Cleanup;
    }

    status = KswordARKHookBuildModuleSnapshot(&moduleInfo, &moduleInfoBytes);
    response->lastStatus = status;
    if (NT_SUCCESS(status) && moduleInfo != NULL &&
        KswordARKKeyboardFindModuleByName(moduleInfo, "win32kfull.sys", &win32kfullEntry)) {
        response->win32kBase = (ULONG64)(ULONG_PTR)win32kfullEntry.ImageBase;
    }

    if (Request != NULL && Request->processId != 0UL) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &processObject);
        response->lastStatus = status;
        if (!NT_SUCCESS(status)) {
            response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED;
            goto Cleanup;
        }
        referencedProcess = TRUE;

        KswordARKKeyboardEnumerateHookChainsForProcess(
            response,
            entryCapacity,
            maxEntries,
            processObject,
            psGetNextProcessThread,
            psGetThreadWin32Thread,
            requestFlags);
    }
    else {
        if (psGetNextProcess == NULL) {
            response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED;
            response->lastStatus = STATUS_PROCEDURE_NOT_FOUND;
            goto Cleanup;
        }

        processCursor = psGetNextProcess(NULL);
        while (processCursor != NULL &&
            processWalkCount < KSWORD_ARK_KEYBOARD_PROCESS_WALK_LIMIT &&
            response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) {
            PEPROCESS nextProcess = psGetNextProcess(processCursor);

            KswordARKKeyboardEnumerateHookChainsForProcess(
                response,
                entryCapacity,
                maxEntries,
                processCursor,
                psGetNextProcessThread,
                psGetThreadWin32Thread,
                requestFlags);

            ObDereferenceObject(processCursor);
            processCursor = nextProcess;
            processWalkCount += 1UL;
        }

        if (processCursor != NULL) {
            ObDereferenceObject(processCursor);
            processCursor = NULL;
            if (response->status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED) {
                response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL;
                response->lastStatus = STATUS_BUFFER_OVERFLOW;
            }
        }
    }

    if (response->status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN) {
        response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK;
    }

Cleanup:
    if (referencedProcess && processObject != NULL) {
        ObDereferenceObject(processObject);
    }
    if (moduleInfo != NULL) {
        ExFreePoolWithTag(moduleInfo, KSW_HOOK_SCAN_TAG);
    }

    *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOOK_RESPONSE_HEADER_SIZE +
        ((size_t)response->returnedCount * sizeof(KSWORD_ARK_KEYBOARD_HOOK_ENTRY));
    return STATUS_SUCCESS;
}
