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

#define KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT 512UL
#define KSWORD_ARK_KEYBOARD_PROCESS_WALK_LIMIT 4096UL
#define KSWORD_ARK_KEYBOARD_THREAD_WALK_LIMIT 65536UL

// 当前 win32kfull!NtUserSetWindowsHookEx/IsHotKey 形态下的保守默认偏移。
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

typedef PVOID(NTAPI* KSWORD_USER_SESSION_STATE_GETTER_FN)(VOID); // Full热键表使用WIN32K用户会话状态，不使用Base的SiloGlobals间接层。

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

NTKERNELAPI NTSTATUS NTAPI PsAcquireProcessExitSynchronization(_In_ PEPROCESS Process); // 附加前拒绝已开始退出的进程。
NTKERNELAPI VOID NTAPI PsReleaseProcessExitSynchronization(_In_ PEPROCESS Process); // 与成功取得的退出保护严格成对。

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

static KSWORD_USER_SESSION_STATE_GETTER_FN
KswordARKKeyboardResolveUserSessionState(
    _In_ const KSW_HOOK_SYSTEM_MODULE_ENTRY* Win32kEntry,
    _In_z_ PCSTR ExportName // 提供者来自当前Full实际导入词汇，而非OS版本。
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
    routineAddress = (ULONG_PTR)RtlFindExportedRoutineByName(Win32kEntry->ImageBase, ExportName); // W32与SGD均须与Full实际槽绑定。
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
            return (KSWORD_USER_SESSION_STATE_GETTER_FN)routineAddress; // 代码与模块范围都确认后才发布入口。
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

/* 冷规则匹配在原调用上下文完成，再绑定本次实际USER namespace。 */
NTSTATUS
KswordARKKeyboardResolveHotkeyRuntime(_Out_ KSW_KEYBOARD_HOTKEY_RUNTIME* RuntimeOut)
{
    KSW_HOOK_SYSTEM_MODULE_INFORMATION* modules = NULL; ULONG moduleBytes = 0UL; // 当前模块快照的拥有者及完整长度。
    KSW_HOOK_SYSTEM_MODULE_ENTRY full, base, provider; // 提供者按实际导入词汇决定，不按版本选择。
    KSWORD_USER_SESSION_STATE_GETTER_FN getter = NULL; NTSTATUS status; // 真实导出与各阶段失败码分别保留。
    if (!RuntimeOut) return STATUS_INVALID_PARAMETER; // 无有效接收对象时不取得任何模块资源。
    RtlZeroMemory(RuntimeOut, sizeof(*RuntimeOut)); // 失败路径不能向调用者泄漏旧证明。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE; // 规则、池及会话入口要求可等待上下文。
    status = KswordARKHookBuildModuleSnapshot(&modules, &moduleBytes); // 对规则和实际导入绑定使用同一份模块身份。
    if (!NT_SUCCESS(status) || !modules || moduleBytes == 0UL) return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status; // 空快照不等于找到了所需模块。
    // Full和Base缺一不可，不借用同名不相关模块或硬编码地址。
    if (!KswordARKKeyboardFindModuleByName(modules, "win32kfull.sys", &full) ||
        !KswordARKKeyboardFindModuleByName(modules, "win32kbase.sys", &base)) { status = STATUS_NOT_FOUND; goto Cleanup; }
    status = KswordARKKeyboardResolveRuleSchema(&full, &base, modules, RuntimeOut); // 不另建快照，冷态IAT绑定使用同一组实际模块。
    if (!NT_SUCCESS(status)) goto Cleanup; // 没有完整schema时不能继续绑定可调用入口。
    if (RuntimeOut->SessionGetterProvider > 1UL) { status = STATUS_NOT_SUPPORTED; goto Cleanup; } // 未承载的提供者明确不支持。
    // 根据规则证明的实际导入选择提供者模块，不查询系统版本号。
    if (!KswordARKKeyboardFindModuleByName(modules,
            RuntimeOut->SessionGetterProvider == 1UL ? "win32ksgd.sys" : "win32k.sys", &provider)) { status = STATUS_NOT_FOUND; goto Cleanup; }
    getter = KswordARKKeyboardResolveUserSessionState(&provider, // 只解析该模块中对应语义的真实导出。
        RuntimeOut->SessionGetterProvider == 1UL ? "SGDGetUserSessionState" : "W32GetUserSessionState");
    if (!getter) { status = STATUS_PROCEDURE_NOT_FOUND; goto Cleanup; } // 导出缺失不能靠虚构会话指针补偿。
    RuntimeOut->SessionGetterAddress = (ULONG_PTR)getter; // 后续还须与Full的实际IAT值相等。
    status = KswordARKKeyboardBindHotkeyRuntimeContext(RuntimeOut); // 不支持无真实getter的虚构全局namespace。
Cleanup:
    ExFreePoolWithTag(modules, KSW_HOOK_SCAN_TAG); // 无论匹配成功与否都释放唯一模块快照。
    if (!NT_SUCCESS(status)) RtlZeroMemory(RuntimeOut, sizeof(*RuntimeOut)); // 不发布部分能力或上次成功残留。
    return status; // 保留实际匹配、导出或上下文错误。
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

/* 从同一完整节点快照按已证明宽度取值，偏移零合法。 */
static BOOLEAN
KswKeyboardNodeValue(const UCHAR* Bytes, ULONG Size, ULONG Offset, ULONG Width, ULONG64* Value)
{
    *Value = 0ULL; // 宽度小于八字节时保持明确零扩展。
    if (!Width || Width > sizeof(*Value) || Offset > Size || Width > Size - Offset) return FALSE; // 以减法校验完整字段，拒绝回绕或截断。
    RtlCopyMemory(Value, Bytes + Offset, Width); // 只从已取得的同一节点字节投影，不另读现场。
    return TRUE; // 完整字段读取才允许调用者继续组合结果。
}

/* 逐行投影只使用已证明的实际schema，诊断筛选不擦除整体失败状态。 */
static VOID
KswordARKKeyboardAppendHotkeyEntry(
    KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* Response, ULONG EntryCapacity, ULONG MaxEntries,
    const KSW_KEYBOARD_HOTKEY_RUNTIME* Runtime, ULONG BucketIndex, ULONG Depth, ULONG_PTR HotkeyObject,
    ULONG ThreadCidOffset, ULONG RequestFlags, ULONG FilterProcessId)
{
    KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY entry; // 原协议对象保持完整，实际节点大小单独回执。
    UCHAR bytes[256]; // 规则对象最大256字节，绝不按旧版本固定大小读取。
    ULONG64 fields[11] = {0}; // next/owner/window/callback/destination/modifiers/flags/vk/id/child两端。
    ULONG_PTR threadObject = 0U, childHead = 0U; // 身份查询与子链比较都来源于此快照。
    BOOLEAN complete = FALSE; // 不完整或未证明的节点不能获得MUTABLE标记。
    if (!Response || !Runtime || !HotkeyObject) return; // 无有效节点或响应时不制造空身份行。
    RtlZeroMemory(&entry, sizeof(entry)); // 未读取的字段始终为明确零值。
    entry.source = KSWORD_ARK_KEYBOARD_SOURCE_WIN32K_HOTKEY_TABLE; entry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK; // 来源和逐项状态属于原始typed记录。
    entry.bucketIndex = BucketIndex; entry.depth = Depth; entry.hotkeyObject = HotkeyObject; entry.sessionGlobals = Runtime->SessionGlobals; // 保留精确桶、对象和namespace供变更复核。
    entry.sessionId = Runtime->SessionId; // 包括SID0，实际namespace另在sessionGlobals中比较。
    entry.entryFlags = KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_MAIN; // 主节点身份不代表已获得可修改资格。
    // 规则范围、一次完整对象读取和每个角色字段都成立才发布快照。
    complete = Runtime->RuleProven && Runtime->ObjectSize != 0UL && Runtime->ObjectSize <= sizeof(bytes) &&
        HotkeyObject <= MAXULONG_PTR - Runtime->ObjectSize && KswordARKHookReadMemorySafe((const VOID*)HotkeyObject, bytes, Runtime->ObjectSize) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->NextOffset, 8UL, &fields[0]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->OwnerOffset, 8UL, &fields[1]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->WindowOffset, 8UL, &fields[2]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->CallbackOffset, 8UL, &fields[3]) &&
        (Runtime->DestinationWidth == 0UL || KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->DestinationOffset, Runtime->DestinationWidth, &fields[4])) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->ModifiersOffset, Runtime->ModifiersWidth, &fields[5]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->FlagsOffset, Runtime->FlagsWidth, &fields[6]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->VkOffset, Runtime->VkWidth, &fields[7]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->IdOffset, Runtime->IdWidth, &fields[8]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->ChildOffset, 8UL, &fields[9]) &&
        KswKeyboardNodeValue(bytes, Runtime->ObjectSize, Runtime->ChildOffset + 8UL, 8UL, &fields[10]);
    if (complete) { // 未完整取得的对象不会进入任何可修改判断。
        childHead = HotkeyObject + Runtime->ChildOffset; // 空子链应有的两个自指针地址。
        entry.nextHotkeyObject = fields[0]; entry.threadInfo = fields[1]; entry.windowObject = fields[2]; entry.windowHandle = fields[2]; // 保留原始window表示，不擅自转换tagWND为HWND。
        entry.callbackAddress = fields[3]; entry.destinationHandle = fields[4]; entry.modifiers = (ULONG)fields[5]; // 可选字段缺失时此前保持零值。
        entry.modifierFlags2 = (ULONG)fields[6]; entry.virtualKey = (ULONG)fields[7]; entry.hotkeyId = (ULONG)fields[8]; // schema已限制数值字段最多DWORD。
        entry.childListFlink = fields[9]; entry.childListBlink = fields[10]; entry.objectSize = Runtime->ObjectSize; // 客户端快照携带完整链身份与实际长度。
        entry.snapshotHash = KswordARKKeyboardHashHotkeyObject(bytes, Runtime->ObjectSize); // Hash覆盖实际完整对象而非固定前缀。
        if (fields[9] != childHead || fields[10] != childHead) entry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_HAS_CHILDREN; // 任一端非自指都按有子项处理。
        if ((fields[6] & KSWORD_ARK_KEYBOARD_HOTKEY_PLACEHOLDER_FLAG) != 0U) entry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_PLACEHOLDER; // 占位节点只作只读诊断。
        if (fields[3] != 0U) entry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_CALLBACK; // 回调项不进入普通独立热键写路径。
        KswordARKKeyboardFillHotkeyThreadIdentity((ULONG_PTR)fields[1], ThreadCidOffset, &threadObject, &entry.processId, &entry.threadId); // 通过公开对象引用确认PID/TID。
        entry.threadObject = threadObject; // 裸线程地址只与已确认的身份一并保留。
    } else { entry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED; entry.lastStatus = STATUS_PARTIAL_COPY; } // 不完整对象返回读失败，不能伪装普通空行。
    if (entry.status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK || (entry.threadInfo != 0U && entry.processId == 0UL)) { // 归属未确认不能当作正常系统热键。
        if (entry.lastStatus == STATUS_SUCCESS) entry.lastStatus = STATUS_NOT_FOUND; // 保留先前更具体的读失败。
        if (entry.status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK) entry.status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_PARTIAL; // 已有字段可保留，但身份仍为部分结果。
        KswordARKKeyboardMarkHotkeyPartial(Response, entry.lastStatus); // 筛掉诊断行之前保留整体失败状态。
    }
    // 只有完整普通节点、合法字段、空子链和匹配哈希桶同时成立才允许R3发起变更。
    if (complete && entry.status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK && entry.threadInfo && !entry.callbackAddress && !entry.modifierFlags2 &&
        entry.virtualKey != 0UL && entry.virtualKey <= 0xFFUL && (entry.modifiers & ~0x0000400FUL) == 0UL &&
        entry.windowHandle != 1ULL && entry.windowHandle < (ULONG64)MAXULONG_PTR - 16ULL &&
        (Runtime->WindowRepresentation == 0UL || entry.windowHandle == 0ULL ||
            (entry.windowHandle >= (ULONG64)(ULONG_PTR)MmSystemRangeStart && (entry.windowHandle & 7ULL) == 0ULL)) &&
        entry.childListFlink == childHead && entry.childListBlink == childHead &&
        BucketIndex == (entry.virtualKey & (Runtime->BucketCount - 1UL))) entry.entryFlags |= KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_MUTABLE;
    if ((RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_FILTER_PROCESS) && FilterProcessId && entry.processId != FilterProcessId) return; // 按规范PID过滤，不借显示文字推断归属。
    if (!(RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM) && !entry.threadInfo) return; // 无安装线程的系统项遵守显式范围选择。
    // 诊断行可从列表隐藏，但上方已经将失败传播到响应整体状态。
    if (!(RequestFlags & KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_DIAGNOSTICS) &&
        (entry.status != KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK || (entry.entryFlags & KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY_FLAG_PLACEHOLDER))) return;
    if (Response->totalCount < MaxEntries) ++Response->totalCount; // 计数遵守请求预算，避免整数增长无界。
    if (Response->returnedCount >= EntryCapacity) { Response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_BUFFER_TRUNCATED; return; } // 缓冲截断有独立回执，不能冒充完整枚举。
    (VOID)RtlStringCchPrintfW(entry.detail, KSWORD_ARK_KEYBOARD_DETAIL_CHARS, L"bucket=%lu depth=%lu", BucketIndex, Depth); // 固定格式只补充只读诊断。
    RtlCopyMemory(&Response->entries[Response->returnedCount++], &entry, sizeof(entry)); // 按真实entry大小发布一次完整typed记录。
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

/* 冷匹配在原上下文，目标附加只用于实际namespace绑定与有界数据快照。 */
NTSTATUS
KswordARKDriverEnumerateKeyboardHotkeys(PVOID OutputBuffer, size_t OutputBufferLength,
    const KSWORD_ARK_ENUM_KEYBOARD_REQUEST* Request, size_t* BytesWrittenOut)
{
    KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE* response; // 响应头始终与真实错误状态一起返回。
    KSW_KEYBOARD_HOTKEY_RUNTIME runtime; // 只含值与当前请求会话，不包含大型Evidence。
    PEPROCESS process = NULL; DECLSPEC_ALIGN(16) UCHAR attachState[128]; // 指定PID的引用和附加成对清理。
    BOOLEAN attached = FALSE, processLocked = FALSE; ULONG flags = Request ? Request->flags : KSWORD_ARK_KEYBOARD_ENUM_FLAG_INCLUDE_SYSTEM; // 附加与退出保护分别成对清理，范围保留请求语义。
    ULONG maximum = Request && Request->maxEntries ? Request->maxEntries : 1024UL; // 零预算使用原默认上限。
    ULONG capacity, bucket, cidOffset; ULONG_PTR table; NTSTATUS status; // 输出容量、桶游标、身份偏移与真实错误独立保存。
    if (!OutputBuffer || !BytesWrittenOut) return STATUS_INVALID_PARAMETER; // 不向无效输出写入响应头。
    *BytesWrittenOut = 0U; // 响应头尚不能完整承载时不声称返回数据。
    if (OutputBufferLength < KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE) return STATUS_BUFFER_TOO_SMALL; // 首先验证固定头的完整空间。
    RtlZeroMemory(OutputBuffer, OutputBufferLength); RtlZeroMemory(&runtime, sizeof(runtime)); // 避免未用字段泄漏旧缓冲或schema。
    response = (KSWORD_ARK_ENUM_KEYBOARD_HOTKEYS_RESPONSE*)OutputBuffer; // 后续所有错误都通过同一固定响应头返回。
    response->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION; response->entrySize = sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY); // 客户端按协议和实际行步长解析。
    response->flags = flags; response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN; // 只有整次枚举未出错时才转为OK。
    capacity = (ULONG)((OutputBufferLength - KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE) / sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY)); // 仅计算可完整写入的行数。
    if (capacity > maximum) capacity = maximum; // 用户预算与输出容量取较小值。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE; response->lastStatus = STATUS_INVALID_DEVICE_STATE; goto Cleanup; } // 非可等待上下文不执行规则和附加。
    if (Request && Request->processId) { // 指定PID必须真正取得目标引用，不能退回调用者会话。
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &process); // 引用防止对象在重型匹配期间释放。
        if (!NT_SUCCESS(status)) { response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED; response->lastStatus = status; process = NULL; goto Cleanup; } // 保留原始lookup错误并避免释放不存在的引用。
        // 先持有目标引用；重型规则证明完成前不附加地址空间。
    }
    status = KswordARKKeyboardResolveHotkeyRuntime(&runtime); // 不再使用192字节窗口内的最后一次opcode命中。
    if (!NT_SUCCESS(status)) { // 匹配失败不再用硬编码布局继续扫描。
        response->lastStatus = status; // 将具体资源、模块或上下文错误传回R3。
        // 不支持、模块缺失、会话缺失和读取失败各有独立业务状态。
        response->status = status == STATUS_NOT_SUPPORTED || status == STATUS_REVISION_MISMATCH ? KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNSUPPORTED :
            (status == STATUS_NOT_FOUND ? KSWORD_ARK_KEYBOARD_ENUM_STATUS_WIN32K_NOT_FOUND :
                (status == STATUS_INVALID_DEVICE_STATE || status == STATUS_PROCEDURE_NOT_FOUND ?
                    KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE : KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED));
        goto Cleanup; // 失败路径仍返回已构造的响应头并释放目标引用。
    }
    cidOffset = KswordARKKeyboardSnapshotThreadCidOffset(); // 偏移快照与池分配也在附加之前完成。
    if (process) { // 全局查询保留调用者上下文，指定进程才建立附加。
        status = PsAcquireProcessExitSynchronization(process); // 防止即将退出的进程地址空间被附加。
        if (!NT_SUCCESS(status)) { response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED; response->lastStatus = status; goto Cleanup; } // 未取得保护时不能执行对应释放。
        processLocked = TRUE; // 防止长证明期间已经结束的进程留下可附加的旧对象引用。
        if (PsGetProcessExitStatus(process) != STATUS_PENDING) { // 已结束对象即使仍被引用也不能作为现场查询上下文。
            response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_READ_FAILED; response->lastStatus = STATUS_PROCESS_IS_TERMINATING; goto Cleanup; // 明确区分退出与正常空结果。
        }
        RtlZeroMemory(attachState, sizeof(attachState)); KeStackAttachProcess((PVOID)process, attachState); attached = TRUE; // 附加状态只由这一层创建与清理。
        status = KswordARKKeyboardBindHotkeyRuntimeContext(&runtime); // 只重新取得已选择进程的SID和namespace。
        if (NT_SUCCESS(status)) status = KswordARKKeyboardValidateRuleRuntime(&runtime); // 附加期间只核对warm指纹，不重跑CFG。
        if (!NT_SUCCESS(status)) { response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_SESSION_UNAVAILABLE; response->lastStatus = status; goto Cleanup; } // 无法绑定目标namespace就不扫描其它会话。
    }
    response->win32kBase = (ULONG64)(ULONG_PTR)runtime.Win32kfullBase; response->sessionGlobals = runtime.SessionGlobals; // 回执映像和实际namespace便于客户端保持同一身份。
    response->tableOffset = runtime.TableOffset; response->hotkeyNextOffset = runtime.NextOffset; // 表与后继字段均由本次通用规则取得。
    response->hotkeyModifiersOffset = runtime.ModifiersOffset; response->hotkeyVkOffset = runtime.VkOffset; response->hotkeyIdOffset = runtime.IdOffset; // 保留协议已有的只读布局诊断。
    table = runtime.TableBase ? runtime.TableBase : runtime.UserSessionState + runtime.TableOffset; // 相对表必须使用实际会话getter的基址。
    for (bucket = 0UL; bucket < runtime.BucketCount; ++bucket) { // 桶数来自实际哈希掩码与循环交叉证明。
        ULONG_PTR object = 0U; ULONG depth = 0UL; // 每个桶都有独立链遍历预算。
        if (!KswordARKKeyboardReadPointer(table + (ULONG_PTR)bucket * sizeof(PVOID), &object)) { KswordARKKeyboardMarkHotkeyPartial(response, STATUS_PARTIAL_COPY); continue; } // 单桶读取失败保留其它行并报告整体部分结果。
        while (object && depth < KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) { // 无界或损坏链不能无限占用查询线程。
            ULONG_PTR next = 0U; // 后继必须每次重新完整读取。
            KswordARKKeyboardAppendHotkeyEntry(response, capacity, maximum, &runtime, bucket, depth, object, cidOffset, flags, Request ? Request->processId : 0UL); // 投影与筛选共用同一个实际schema。
            if (object > MAXULONG_PTR - runtime.NextOffset || !KswordARKKeyboardReadPointer(object + runtime.NextOffset, &next)) { KswordARKKeyboardMarkHotkeyPartial(response, STATUS_PARTIAL_COPY); break; } // 防回绕并保留读失败状态。
            if (next == object) { KswordARKKeyboardMarkHotkeyPartial(response, STATUS_DATA_ERROR); break; } // 自环不是合法链尾。
            object = next; ++depth; // 成功推进一个完整节点后才消耗下一层预算。
        }
        if (object && depth >= KSWORD_ARK_KEYBOARD_CHAIN_WALK_LIMIT) KswordARKKeyboardMarkHotkeyPartial(response, STATUS_BUFFER_OVERFLOW); // 多节点环或过长链均明确报告截断。
    }
    if (response->status == KSWORD_ARK_KEYBOARD_ENUM_STATUS_UNKNOWN) response->status = KSWORD_ARK_KEYBOARD_ENUM_STATUS_OK; // 不覆盖任何已观察失败或容量截断。
Cleanup:
    if (attached) KeUnstackDetachProcess(attachState); // 先离开目标地址空间再释放进程保护。
    if (processLocked) PsReleaseProcessExitSynchronization(process); // 只释放实际成功取得的退出同步。
    if (process) ObDereferenceObject(process); // 每个lookup引用只释放一次。
    *BytesWrittenOut = KSWORD_ARK_KEYBOARD_HOTKEY_RESPONSE_HEADER_SIZE + (size_t)response->returnedCount * sizeof(KSWORD_ARK_KEYBOARD_HOTKEY_ENTRY); // 响应长度仅包含已完整写入的行。
    return STATUS_SUCCESS; // 运输成功与响应中的业务枚举状态分开。
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
