/*++

Module Name:

    process_resolver.c

Abstract:

    This file resolves optional kernel process routines dynamically.

Environment:

    Kernel-mode Driver Framework

--*/

#include <ntifs.h>
#include <ntimage.h>
#include "process_resolver.h"
#include "process_accessor_decode.h"
#include "token_layout_resolver.h"
#include "runtime_signature_scan.h"

#define KSW_RUNTIME_PROCESS_SCAN_BYTES 0x1000U
#define KSW_RUNTIME_THREAD_SCAN_BYTES 0x1000U
#define KSW_RUNTIME_THREAD_LIST_BUDGET 0x1000U

typedef ULONG_PTR(NTAPI* KSWORD_OBJECT_ACCESSOR_FN)(
    _In_ PVOID Object
    );

typedef BOOLEAN(NTAPI* KSWORD_PROCESS_BOOLEAN_ACCESSOR_FN)(
    _In_ PEPROCESS Process
    );

typedef ULONG_PTR(NTAPI* KSWORD_CURRENT_THREAD_ACCESSOR_FN)(
    VOID
    );

typedef UCHAR(NTAPI* KSWORD_PROCESS_PROTECTION_ACCESSOR_FN)(
    _In_ PEPROCESS Process
    );

typedef UCHAR(NTAPI* KSWORD_PROCESS_SIGNATURE_ACCESSOR_FN)(
    _In_ PEPROCESS Process,
    _Out_opt_ PUCHAR SectionSignatureLevel
    );

// 用途：按原始导出入口确定所属映像，入口跳转只能在该映像的代码区段内跟随。
NTSYSAPI PVOID NTAPI RtlPcToFileHeader(
    _In_ PVOID PcValue,
    _Outptr_ PVOID* BaseOfImage);

static BOOLEAN
KswordARKDriverReadPointerGuarded(
    _In_ const VOID* Address,
    _Out_ ULONG_PTR* ValueOut
    );

static VOID
KswordARKDriverInitializeRuntimeDynDataOffsets(
    _Out_ PKSWORD_RUNTIME_DYNDATA_OFFSETS Offsets
    )
{
    LONG* field = NULL;
    SIZE_T index = 0U;

    if (Offsets == NULL) {
        return;
    }
    field = (LONG*)Offsets;
    for (index = 0U; index < sizeof(*Offsets) / sizeof(*field); ++index) {
        field[index] = -1;
    }
}

// 用途：安全解析导出所属映像及可执行区段，供访问器入口和直接跳转目标验证。
static BOOLEAN
KswordARKDriverAccessorImageView(
    _In_ PVOID Routine, // 已通过导出表确认的访问器入口。
    _Out_ KSW_RUNTIME_IMAGE_VIEW* ViewOut) // 接收安全解析后的 PE 映像与区段边界。
{
    PVOID imageBase = NULL; // 原始导出入口所属的加载映像基址。
    IMAGE_DOS_HEADER dosHeader; // 安全读取的 DOS 头本地副本。
    IMAGE_NT_HEADERS64 ntHeaders; // 安全读取的 x64 NT 头本地副本。
    ULONG_PTR ntAddress = 0U; // 经溢出检查后计算的 NT 头地址。

    // 先验证入口所属 PE，再做解码或跟随跳转；头地址与头内容都不能直接解引用。
    if (Routine == NULL || ViewOut == NULL ||
        RtlPcToFileHeader(Routine, &imageBase) == NULL || imageBase == NULL ||
        !KswordARKRuntimeReadMemory(imageBase, &dosHeader, sizeof(dosHeader)) ||
        dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0 ||
        dosHeader.e_lfanew > 0x00100000L ||
        (ULONG_PTR)imageBase > MAXULONG_PTR - (ULONG)dosHeader.e_lfanew) {
        return FALSE; // 无法证明头地址和映像身份时保持字段不可用。
    }
    ntAddress = (ULONG_PTR)imageBase + (ULONG)dosHeader.e_lfanew; // 在已检查的范围内定位 NT 头。
    if (!KswordARKRuntimeReadMemory(
            (const VOID*)ntAddress, &ntHeaders, sizeof(ntHeaders)) ||
        ntHeaders.Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        ntHeaders.OptionalHeader.SizeOfImage < 0x1000UL ||
        !KswordARKRuntimeInitializeImageView(
            imageBase, ntHeaders.OptionalHeader.SizeOfImage, ViewOut)) {
        return FALSE; // 签名、体系结构或区段解析不一致时拒绝解码。
    }
    return KswordARKRuntimeAddressIsExecutable(ViewOut, (ULONG_PTR)Routine, 1U); // 入口本身也必须位于代码区段。
}

// 用途：为独立解码器提供同一映像代码区段中的完整单字节安全读取。
static int
KswordARKDriverAccessorReadCode(
    void* Context, // 原始导出映像的已验证区段视图。
    KSW_ACCESSOR_ADDRESS Address, // 本次代码字节或跳转目标的读取地址。
    KSW_ACCESSOR_BYTE* ByteOut) // 完整复制成功时接收一个字节。
{
    const KSW_RUNTIME_IMAGE_VIEW* view = (const KSW_RUNTIME_IMAGE_VIEW*)Context; // 安全读取共用的映像边界。

    // 逐字节检查代码区段；任何入口跳转目标都不能跨出原始 PE。
    if (view == NULL || ByteOut == NULL ||
        !KswordARKRuntimeAddressIsExecutable(view, (ULONG_PTR)Address, 1U)) {
        return 0; // 缺少上下文或地址越界时不给解码器任何候选字节。
    }
    return KswordARKRuntimeReadMemory((const VOID*)Address, ByteOut, 1U) ? 1 : 0; // MmCopyMemory 完整读取才算成功。
}

static BOOLEAN
KswordARKDriverReadAccessorValue(
    _In_ PVOID Object,
    _In_ const KSWORD_ACCESSOR_DISPLACEMENT* Displacement,
    _Out_ ULONG_PTR* ValueOut
    )
{
    if (Object == NULL || Displacement == NULL || ValueOut == NULL ||
        Displacement->Offset <= 0) {
        return FALSE;
    }
    *ValueOut = 0U;

    // 位移来自对访问器代码的反汇编解码，猜错时会落到对象之外，同样要安全读。
    {
        const UCHAR* address = (const UCHAR*)Object + Displacement->Offset;
        ULONG64 raw = 0ULL;
        SIZE_T width = 0U;

        switch (Displacement->LoadKind) {
        case KswordAccessorAddress:
            *ValueOut = (ULONG_PTR)address;
            return TRUE;
        case KswordAccessorLoadPointer:
            width = sizeof(ULONG_PTR);
            break;
        case KswordAccessorLoadUlong:
            width = sizeof(ULONG);
            break;
        case KswordAccessorLoadUshort:
            width = sizeof(USHORT);
            break;
        case KswordAccessorLoadUchar:
            width = sizeof(UCHAR);
            break;
        default:
            return FALSE;
        }
        if (!KswordARKRuntimeReadMemory(address, &raw, width)) {
            return FALSE;
        }
        *ValueOut = (ULONG_PTR)raw;
    }
    return TRUE;
}

static LONG
KswordARKDriverResolveValidatedAccessorOffset(
    _In_ PCWSTR RoutineName, // 用于取得公开导出地址的例程名称。
    _In_ PVOID ValidationObject // 具有可信引用或当前上下文保证的现场对象。
    )
{
    UNICODE_STRING routineName; // 提交给系统导出解析器的例程名称。
    KSWORD_OBJECT_ACCESSOR_FN accessor = NULL; // 已确认导出身份的现场访问器。
    KSW_RUNTIME_IMAGE_VIEW imageView; // 将解码与入口跳转约束在原始导出的 PE 映像内。
    KSWORD_ACCESSOR_DISPLACEMENT displacement; // 严格指令解码确认的读取操作及字段偏移。
    ULONG_PTR accessorValue = 0U; // 已确认导出在同一个现场对象上返回的语义依据。
    ULONG_PTR fieldValue = 0U; // 候选字段经过完整安全读取后的本地值。

    if (RoutineName == NULL || ValidationObject == NULL) {
        return -1;
    }
    RtlInitUnicodeString(&routineName, RoutineName); // 构造不会改写调用者字符串的导出名称视图。
    accessor = (KSWORD_OBJECT_ACCESSOR_FN)MmGetSystemRoutineAddress(&routineName); // 从系统导出表取得可信入口。
    if (accessor == NULL ||
        !KswordARKDriverAccessorImageView((PVOID)accessor, &imageView) ||
        !KswordARKResolveAccessorDisplacement(
            KswordARKDriverAccessorReadCode,
            &imageView,
            (KSW_ACCESSOR_ADDRESS)accessor,
            &displacement) ||
        !KswordARKDriverReadAccessorValue(
            ValidationObject, &displacement, &fieldValue)) {
        return -1;
    }

    // 已确认身份的导出仍作为现场语义依据；仅机器码形态匹配不能发布偏移。
    __try {
        accessorValue = accessor(ValidationObject); // 仅调用已确认身份的公开访问器，不调用候选代码地址。
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (fieldValue != accessorValue) {
        return -1;
    }
    return displacement.Offset; // 指令形态和现场语义一致后才发布偏移。
}

static LONG
KswordARKDriverResolveCurrentThreadStackOffset(
    _In_z_ PCWSTR RoutineName,
    _In_ PETHREAD CurrentThread
    )
/*++

Routine Description:

    Decode a no-argument current-thread stack accessor.  Accepted x64 code is
    deliberately narrow: load KTHREAD from gs:[188h], load one pointer field
    using disp8/disp32, then return.  The candidate must equal the live export
    result for the same current thread before it is published.

Return Value:

    Validated KTHREAD field offset, or -1 for any missing/ambiguous shape.

--*/
{
    static const UCHAR currentThreadLoad[] = {
        0x65U, 0x48U, 0x8BU, 0x04U, 0x25U,
        0x88U, 0x01U, 0x00U, 0x00U
    };
    UNICODE_STRING routineName;
    KSWORD_CURRENT_THREAD_ACCESSOR_FN accessor = NULL;
    UCHAR code[32];
    LONG offset = -1;
    SIZE_T instructionEnd = 0U;
    ULONG_PTR accessorValue = 0U;
    ULONG_PTR fieldValue = 0U;

    if (RoutineName == NULL || CurrentThread == NULL) {
        return -1;
    }
    RtlInitUnicodeString(&routineName, RoutineName);
    accessor = (KSWORD_CURRENT_THREAD_ACCESSOR_FN)
        MmGetSystemRoutineAddress(&routineName);
    if (accessor == NULL) {
        return -1;
    }
    RtlZeroMemory(code, sizeof(code));
    if (!KswordARKRuntimeReadMemory((const VOID*)accessor, code, sizeof(code))) { // 导出代码先安全复制，失败不执行候选解码。
        return -1; // 代码不可完整读取时该偏移不可用。
    }
    __try {
        accessorValue = accessor();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (RtlCompareMemory(
            code,
            currentThreadLoad,
            sizeof(currentThreadLoad)) != sizeof(currentThreadLoad)) {
        return -1;
    }

    if (code[9] == 0x48U && code[10] == 0x8BU && code[11] == 0x40U) {
        offset = (LONG)code[12];
        instructionEnd = 13U;
    }
    else if (code[9] == 0x48U && code[10] == 0x8BU &&
        code[11] == 0x80U) {
        RtlCopyMemory(&offset, &code[12], sizeof(offset));
        instructionEnd = 16U;
    }
    if (offset <= 0 || offset > (LONG)KSW_RUNTIME_THREAD_SCAN_BYTES ||
        code[instructionEnd] != 0xC3U ||
        accessorValue < (ULONG_PTR)MmSystemRangeStart) {
        return -1;
    }
    if (!KswordARKDriverReadPointerGuarded(
            (const UCHAR*)CurrentThread + offset,
            &fieldValue) ||
        fieldValue != accessorValue) {
        return -1;
    }
    return offset;
}

static LONG
KswordARKDriverResolveActiveProcessLinksOffset(
    _In_ PEPROCESS Process,
    _In_ LONG UniqueProcessIdOffset
    )
{
    LONG activeProcessLinksOffset = -1;
    PLIST_ENTRY link = NULL;

    if (Process == NULL || UniqueProcessIdOffset <= 0 ||
        UniqueProcessIdOffset > (LONG)(0x00003FFFU - sizeof(HANDLE))) {
        return -1;
    }
    activeProcessLinksOffset =
        UniqueProcessIdOffset + (LONG)sizeof(HANDLE);
    link = (PLIST_ENTRY)((PUCHAR)Process + activeProcessLinksOffset);

    /*
     * 互反性校验要二次解引用 Flink/Blink，而这两个值来自候选偏移处的内存，
     * 在偏移猜错时就是任意值。逐跳安全读，别在异常边界里直接跟指针走。
     */
    {
        LIST_ENTRY head;
        LIST_ENTRY forward;
        LIST_ENTRY backward;

        if (!KswordARKRuntimeReadMemory(link, &head, sizeof(head)) ||
            head.Flink == NULL || head.Blink == NULL ||
            !KswordARKRuntimeReadMemory(head.Flink, &forward, sizeof(forward)) ||
            !KswordARKRuntimeReadMemory(head.Blink, &backward, sizeof(backward)) ||
            forward.Blink != link ||
            backward.Flink != link) {
            return -1;
        }
    }
    return activeProcessLinksOffset;
}

LONG
KswordARKDriverResolveProcessFlagsOffset(
    _In_ PEPROCESS Process
    )
/*++

Routine Description:

    Recover EPROCESS.Flags from the exported PsGetProcessExitProcessCalled
    bit accessor.  The resolver accepts only the compact x64 sequence that
    loads one ULONG from RCX, shifts the result, and masks it to one bit.  The
    decoded bit is then compared with the live accessor result before the
    offset is published.

Arguments:

    Process - Live process object used for semantic validation.

Return Value:

    A validated EPROCESS.Flags offset, or -1 when the accessor shape or live
    value is ambiguous.

--*/
{
    UNICODE_STRING routineName;
    KSWORD_PROCESS_BOOLEAN_ACCESSOR_FN accessor = NULL;
    UCHAR code[32] = { 0 };
    ULONG index = 0UL;
    LONG resolvedOffset = -1;
    ULONG resolvedBit = MAXULONG;
    ULONG matchCount = 0UL;
    BOOLEAN accessorValue = FALSE;

    if (Process == NULL) {
        return -1;
    }

    RtlInitUnicodeString(&routineName, L"PsGetProcessExitProcessCalled");
    accessor = (KSWORD_PROCESS_BOOLEAN_ACCESSOR_FN)MmGetSystemRoutineAddress(&routineName);
    if (accessor == NULL) {
        return -1;
    }

    if (!KswordARKRuntimeReadMemory((const VOID*)accessor, code, sizeof(code))) { // 访问器机器码快照不能依赖异常保护直接复制。
        return -1; // 未完整读取时不尝试匹配标志字段。
    }
    __try {
        accessorValue = accessor(Process) ? TRUE : FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    for (index = 0UL; index + 11UL <= RTL_NUMBER_OF(code); ++index) {
        LONG candidateOffset = -1;
        ULONG shift = MAXULONG;
        ULONG suffix = index + 6UL;

        /* mov r32, dword ptr [rcx+disp32] */
        if (code[index] != 0x8BU || (code[index + 1UL] & 0xC7U) != 0x81U) {
            continue;
        }
        RtlCopyMemory(&candidateOffset, code + index + 2UL, sizeof(candidateOffset));
        if (candidateOffset <= 0 || candidateOffset > 0x00003FFF) {
            continue;
        }

        /* shr eax, imm8; and al/eax, 1 */
        if (code[suffix] == 0xC1U && code[suffix + 1UL] == 0xE8U) {
            shift = code[suffix + 2UL];
            suffix += 3UL;
        }
        if (shift >= 32UL) {
            continue;
        }
        if (!((code[suffix] == 0x24U && code[suffix + 1UL] == 0x01U) ||
              (code[suffix] == 0x83U && code[suffix + 1UL] == 0xE0U &&
               code[suffix + 2UL] == 0x01U))) {
            continue;
        }

        resolvedOffset = candidateOffset;
        resolvedBit = shift;
        matchCount += 1UL;
    }

    if (matchCount != 1UL || resolvedOffset <= 0 || resolvedBit >= 32UL) {
        return -1;
    }

    {
        ULONG flags = 0UL; // 候选 Flags 偏移对应的本地安全读取副本。
        // 候选偏移可能落到对象外；SEH 不能兜住未映射内核地址，必须完整安全复制。
        if (!KswordARKRuntimeReadMemory(
                (const UCHAR*)Process + resolvedOffset, &flags, sizeof(flags))) {
            return -1; // 无法完整读取时不发布 Flags 偏移。
        }
        if ((((flags >> resolvedBit) & 1UL) != 0UL) != accessorValue) {
            return -1;
        }
    }

    return resolvedOffset;
}

static BOOLEAN
KswordARKDriverReadPointerGuarded(
    _In_ const VOID* Address,
    _Out_ ULONG_PTR* ValueOut
    )
/*++

Routine Description:

    Read one kernel pointer-sized value through an exception boundary.

Arguments:

    Address - Candidate readable kernel address.
    ValueOut - Receives the copied scalar value.

Return Value:

    TRUE when the read completed; otherwise FALSE.

--*/
{
    if (Address == NULL || ValueOut == NULL) {
        return FALSE;
    }
    *ValueOut = 0U;

    // 同 ReadListEntryGuarded：候选地址不可信，异常边界挡不住未映射内核地址。
    return KswordARKRuntimeReadMemory(Address, ValueOut, sizeof(*ValueOut));
}

static BOOLEAN
KswordARKDriverReadListEntryGuarded(
    _In_ const LIST_ENTRY* Address,
    _Out_ LIST_ENTRY* EntryOut
    )
/*++

Routine Description:

    Copy one candidate LIST_ENTRY without trusting the candidate mapping.

Arguments:

    Address - Candidate list-entry address.
    EntryOut - Receives a stable scalar snapshot of both links.

Return Value:

    TRUE when both links were copied; otherwise FALSE.

--*/
{
    if (Address == NULL || EntryOut == NULL) {
        return FALSE;
    }
    RtlZeroMemory(EntryOut, sizeof(*EntryOut));

    /*
     * 候选地址来自上一跳链表里读出的任意指针，完全可能没有映射。内核态触碰
     * 未映射地址产生的是 bugcheck 0x50 而不是可捕获异常，__try/__except 拦不住
     * ——这条路径曾在 DriverEntry 的偏移探测阶段直接蓝屏。必须走 MmCopyMemory。
     */
    return KswordARKRuntimeReadMemory(Address, EntryOut, sizeof(*EntryOut));
}

static BOOLEAN
KswordARKDriverAddressInsideObjectWindow(
    _In_ ULONG_PTR Address,
    _In_ ULONG_PTR ObjectBase,
    _In_ SIZE_T ObjectWindow
    )
/*++

Routine Description:

    Test whether an address lies inside one bounded live-object inspection window.

Arguments:

    Address - Address being classified.
    ObjectBase - Start of the live kernel object.
    ObjectWindow - Maximum number of bytes inspected from the object.

Return Value:

    TRUE when Address is within the non-wrapping half-open window.

--*/
{
    if (ObjectBase == 0U || ObjectWindow == 0U ||
        ObjectBase > MAXULONG_PTR - ObjectWindow) {
        return FALSE;
    }
    return (Address >= ObjectBase && Address < ObjectBase + ObjectWindow) ? TRUE : FALSE;
}

static BOOLEAN
KswordARKDriverValidateThreadListCandidate(
    _In_ PEPROCESS Process,
    _In_ PETHREAD CurrentThread,
    _In_ LONG KthreadProcessOffset,
    _In_ ULONG ThreadListEntryOffset,
    _Out_ ULONG* ProcessThreadListHeadOffsetOut
    )
/*++

Routine Description:

    Validate one ETHREAD list-entry candidate by walking only reciprocal links
    until the list reaches a head embedded in the owning EPROCESS. Every thread
    node must expose the already validated KTHREAD.Process pointer at the same
    offset. This turns the offset search into a structural identity check rather
    than a naked pointer-pattern match.

Arguments:

    Process - Owning live process object.
    CurrentThread - Referenced live thread known to belong to Process.
    KthreadProcessOffset - Accessor-derived KTHREAD.Process offset.
    ThreadListEntryOffset - Candidate ETHREAD.ThreadListEntry offset.
    ProcessThreadListHeadOffsetOut - Receives the matching EPROCESS head offset.

Return Value:

    TRUE when a complete bounded path reaches a reciprocal EPROCESS list head.

--*/
{
    ULONG_PTR processBase = (ULONG_PTR)Process;
    ULONG_PTR currentLinkAddress = (ULONG_PTR)CurrentThread + ThreadListEntryOffset;
    LIST_ENTRY currentEntry;
    ULONG step = 0UL;

    if (Process == NULL || CurrentThread == NULL ||
        KthreadProcessOffset < 0 || ProcessThreadListHeadOffsetOut == NULL) {
        return FALSE;
    }
    *ProcessThreadListHeadOffsetOut = 0UL;
    if (!KswordARKDriverReadListEntryGuarded((const LIST_ENTRY*)currentLinkAddress, &currentEntry) ||
        currentEntry.Flink == NULL || currentEntry.Blink == NULL) {
        return FALSE;
    }

    for (step = 0UL; step < KSW_RUNTIME_THREAD_LIST_BUDGET; ++step) {
        ULONG_PTR nextAddress = (ULONG_PTR)currentEntry.Flink;
        LIST_ENTRY nextEntry;

        if (!KswordARKDriverReadListEntryGuarded((const LIST_ENTRY*)nextAddress, &nextEntry) ||
            (ULONG_PTR)nextEntry.Blink != currentLinkAddress) {
            return FALSE;
        }

        if (KswordARKDriverAddressInsideObjectWindow(
                nextAddress,
                processBase,
                KSW_RUNTIME_PROCESS_SCAN_BYTES)) {
            ULONG_PTR firstThreadLinkAddress = (ULONG_PTR)nextEntry.Flink;
            LIST_ENTRY firstThreadEntry;
            ULONG_PTR firstThreadProcess = 0U;

            if (nextAddress < processBase ||
                nextAddress - processBase > MAXULONG ||
                firstThreadLinkAddress <= ThreadListEntryOffset ||
                !KswordARKDriverReadListEntryGuarded(
                    (const LIST_ENTRY*)firstThreadLinkAddress,
                    &firstThreadEntry) ||
                (ULONG_PTR)firstThreadEntry.Blink != nextAddress ||
                !KswordARKDriverReadPointerGuarded(
                    (const UCHAR*)(firstThreadLinkAddress - ThreadListEntryOffset) + KthreadProcessOffset,
                    &firstThreadProcess) ||
                firstThreadProcess != processBase) {
                return FALSE;
            }

            *ProcessThreadListHeadOffsetOut = (ULONG)(nextAddress - processBase);
            return TRUE;
        }

        if (nextAddress <= ThreadListEntryOffset) {
            return FALSE;
        }
        {
            ULONG_PTR nextThreadAddress = nextAddress - ThreadListEntryOffset;
            ULONG_PTR nextThreadProcess = 0U;

            if (!KswordARKDriverReadPointerGuarded(
                    (const UCHAR*)nextThreadAddress + KthreadProcessOffset,
                    &nextThreadProcess) ||
                nextThreadProcess != processBase) {
                return FALSE;
            }
        }

        currentLinkAddress = nextAddress;
        currentEntry = nextEntry;
    }
    return FALSE;
}

static VOID
KswordARKDriverResolveThreadListOffsets(
    _In_opt_ PEPROCESS Process,
    _In_ PETHREAD CurrentThread,
    _In_ LONG KthreadProcessOffset,
    _Out_ LONG* ProcessThreadListHeadOffsetOut,
    _Out_ LONG* ThreadListEntryOffsetOut
    )
/*++

Routine Description:

    Find a unique reciprocal process/thread list pair in two live objects.

Arguments:

    Process - Current process object.
    CurrentThread - Current thread object.
    KthreadProcessOffset - Previously validated KTHREAD.Process offset.
    ProcessThreadListHeadOffsetOut - Receives EPROCESS.ThreadListHead.
    ThreadListEntryOffsetOut - Receives ETHREAD.ThreadListEntry.

Return Value:

    None. Both outputs remain unavailable unless exactly one pair validates.

--*/
{
    ULONG candidateOffset = 0UL;
    LONG foundProcessOffset = -1;
    LONG foundThreadOffset = -1;

    if (ProcessThreadListHeadOffsetOut == NULL || ThreadListEntryOffsetOut == NULL) {
        return;
    }
    *ProcessThreadListHeadOffsetOut = -1;
    *ThreadListEntryOffsetOut = -1;
    if (Process == NULL || CurrentThread == NULL || KthreadProcessOffset < 0) {
        return;
    }

    for (candidateOffset = 0UL;
         candidateOffset + sizeof(LIST_ENTRY) <= KSW_RUNTIME_THREAD_SCAN_BYTES;
         candidateOffset += (ULONG)sizeof(PVOID)) {
        ULONG processHeadOffset = 0UL;

        if (!KswordARKDriverValidateThreadListCandidate(
                Process,
                CurrentThread,
                KthreadProcessOffset,
                candidateOffset,
                &processHeadOffset)) {
            continue;
        }
        if (foundThreadOffset >= 0) {
            return;
        }
        foundProcessOffset = (LONG)processHeadOffset;
        foundThreadOffset = (LONG)candidateOffset;
    }

    *ProcessThreadListHeadOffsetOut = foundProcessOffset;
    *ThreadListEntryOffsetOut = foundThreadOffset;
}

VOID
KswordARKDriverResolveReadOnlyDynDataOffsets(
    _Out_ PKSWORD_RUNTIME_DYNDATA_OFFSETS Offsets
    )
{
    PEPROCESS process = PsGetCurrentProcess();
    PETHREAD thread = PsGetCurrentThread();
    PACCESS_TOKEN token = NULL;
    LONG threadProcessIdOffset = -1;
    LONG threadIdOffset = -1;

    if (Offsets == NULL) {
        return;
    }
    KswordARKDriverInitializeRuntimeDynDataOffsets(Offsets);
    if (process != NULL) {
        Offsets->EpUniqueProcessId =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessId",
                process);
        Offsets->EpActiveProcessLinks =
            KswordARKDriverResolveActiveProcessLinksOffset(
                process,
                Offsets->EpUniqueProcessId);
        Offsets->EpImageFileName =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessImageFileName",
                process);
        Offsets->EpCreateTime =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessCreateTimeQuadPart",
                process);
        Offsets->EpExitStatus =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessExitStatus",
                process);
        Offsets->EpPeb =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessPeb",
                process);
        Offsets->EpWin32Process =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessWin32Process",
                process);
        Offsets->EpWow64Process =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessWow64Process",
                process);
        Offsets->EpInheritedFromUniqueProcessId =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessInheritedFromUniqueProcessId",
                process);
        Offsets->EpSectionBaseAddress =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessSectionBaseAddress",
                process);
        Offsets->EpJob =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessJob",
                process);
        Offsets->EpDebugPort =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessDebugPort",
                process);
        Offsets->EpPriorityClass =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessPriorityClass",
                process);
        Offsets->EpActiveThreads =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessActiveThreadCount",
                process);
        Offsets->EpWin32WindowStation =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessWin32WindowStation",
                process);
        Offsets->EpSecurityPort =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetProcessSecurityPort",
                process);

        token = PsReferencePrimaryToken(process);
        if (token != NULL) {
            Offsets->EpToken = KswordARKDriverResolveProcessTokenOffset(process, token);
            KswordARKDriverResolveTokenLayoutOffsets(
                token,
                &Offsets->TokUserAndGroupCount,
                &Offsets->TokUserAndGroups,
                &Offsets->TokIntegrityLevelIndex,
                &Offsets->TokMandatoryPolicy);
            PsDereferencePrimaryToken(token);
            token = NULL;
        }
        Offsets->EpFlags = KswordARKDriverResolveProcessFlagsOffset(process);
    }
    if (thread != NULL) {
        threadProcessIdOffset =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetThreadProcessId",
                thread);
        threadIdOffset =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetThreadId",
                thread);
        if (threadProcessIdOffset > 0 &&
            threadIdOffset ==
                threadProcessIdOffset + (LONG)sizeof(HANDLE)) {
            Offsets->EtCid = threadProcessIdOffset;
        }
        Offsets->KtProcess =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetThreadProcess",
                thread);
        Offsets->KtInitialStack =
            KswordARKDriverResolveCurrentThreadStackOffset(
                L"IoGetInitialStack",
                thread);
        Offsets->KtStackLimit =
            KswordARKDriverResolveCurrentThreadStackOffset(
                L"PsGetCurrentThreadStackLimit",
                thread);
        Offsets->KtStackBase =
            KswordARKDriverResolveCurrentThreadStackOffset(
                L"PsGetCurrentThreadStackBase",
                thread);
        Offsets->EtStartAddress =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetThreadStartAddress",
                thread);
        Offsets->EtWin32StartAddress =
            KswordARKDriverResolveValidatedAccessorOffset(
                L"PsGetThreadWin32StartAddress",
                thread);
        KswordARKDriverResolveThreadListOffsets(
            process,
            thread,
            Offsets->KtProcess,
            &Offsets->EpThreadListHead,
            &Offsets->EtThreadListEntry);
    }
}

// Resolve PsSuspendProcess first; this export is available on more systems.
KSWORD_PS_SUSPEND_PROCESS_FN
KswordARKDriverResolvePsSuspendProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsSuspendProcess");
    return (KSWORD_PS_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// Fallback resolver for Zw/Nt suspend APIs that use process handle input.
KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN
KswordARKDriverResolveZwOrNtSuspendProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwSuspendProcess");
    {
        KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN routineAddress =
            (KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
        if (routineAddress != NULL) {
            return routineAddress;
        }
    }

    RtlInitUnicodeString(&routineName, L"NtSuspendProcess");
    return (KSWORD_ZW_OR_NT_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// Resolve PsResumeProcess first, mirroring the suspend path exactly.
KSWORD_PS_RESUME_PROCESS_FN
KswordARKDriverResolvePsResumeProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsResumeProcess");
    return (KSWORD_PS_RESUME_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

// Fallback resolver for Zw/Nt resume APIs that use process handle input.
KSWORD_ZW_OR_NT_RESUME_PROCESS_FN
KswordARKDriverResolveZwOrNtResumeProcess(
    VOID
    )
{
    UNICODE_STRING routineName;

    RtlInitUnicodeString(&routineName, L"ZwResumeProcess");
    {
        KSWORD_ZW_OR_NT_RESUME_PROCESS_FN routineAddress =
            (KSWORD_ZW_OR_NT_RESUME_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
        if (routineAddress != NULL) {
            return routineAddress;
        }
    }

    RtlInitUnicodeString(&routineName, L"NtResumeProcess");
    return (KSWORD_ZW_OR_NT_RESUME_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_PS_IS_PROTECTED_PROCESS_FN
KswordARKDriverResolvePsIsProtectedProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsIsProtectedProcess");
    return (KSWORD_PS_IS_PROTECTED_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}

KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN
KswordARKDriverResolvePsIsProtectedProcessLight(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"PsIsProtectedProcessLight");
    return (KSWORD_PS_IS_PROTECTED_PROCESS_LIGHT_FN)MmGetSystemRoutineAddress(&routineName);
}

static LONG
KswordARKDriverDecodeReturnedUcharOffset(
    _In_reads_bytes_(ByteCount) const UCHAR* Bytes,
    _In_ SIZE_T ByteCount
    )
{
    LONG candidate = -1;
    SIZE_T index = 0U;

    if (Bytes == NULL) {
        return -1;
    }
    for (index = 0U; index + 7U <= ByteCount; ++index) {
        LONG offset = -1;
        SIZE_T instructionBytes = 0U;

        // mov al, byte ptr [rcx+disp32]
        if (Bytes[index] == 0x8AU && Bytes[index + 1U] == 0x81U) {
            RtlCopyMemory(&offset, Bytes + index + 2U, sizeof(offset));
            instructionBytes = 6U;
        }
        // movzx eax, byte ptr [rcx+disp32]
        else if (index + 8U <= ByteCount &&
            Bytes[index] == 0x0FU && Bytes[index + 1U] == 0xB6U &&
            Bytes[index + 2U] == 0x81U) {
            RtlCopyMemory(&offset, Bytes + index + 3U, sizeof(offset));
            instructionBytes = 7U;
        }
        else {
            continue;
        }
        if (offset <= 0 || offset > 0x0FFF ||
            index + instructionBytes >= ByteCount ||
            Bytes[index + instructionBytes] != 0xC3U) {
            continue;
        }
        if (candidate >= 0 && candidate != offset) {
            return -1;
        }
        candidate = offset;
    }
    return candidate;
}

static BOOLEAN
KswordARKDriverResolveProcessSignatureOffsets(
    _Out_ LONG* SignatureLevelOffsetOut,
    _Out_ LONG* SectionSignatureLevelOffsetOut
    )
{
    UNICODE_STRING routineName;
    KSWORD_PROCESS_SIGNATURE_ACCESSOR_FN accessor = NULL;
    PEPROCESS process = PsGetCurrentProcess();
    UCHAR code[32];
    UCHAR signatureValue = 0U;
    UCHAR sectionValue = 0U;
    UCHAR signatureField = 0U;
    UCHAR sectionField = 0U;
    LONG signatureOffset = -1;
    LONG sectionOffset = -1;
    SIZE_T index = 0U;

    if (SignatureLevelOffsetOut == NULL ||
        SectionSignatureLevelOffsetOut == NULL || process == NULL) {
        return FALSE;
    }
    *SignatureLevelOffsetOut = -1;
    *SectionSignatureLevelOffsetOut = -1;
    RtlInitUnicodeString(&routineName, L"PsGetProcessSignatureLevel");
    accessor = (KSWORD_PROCESS_SIGNATURE_ACCESSOR_FN)
        MmGetSystemRoutineAddress(&routineName);
    if (accessor == NULL) {
        return FALSE;
    }
    RtlZeroMemory(code, sizeof(code));
    if (!KswordARKRuntimeReadMemory((const VOID*)accessor, code, sizeof(code))) { // 安全建立签名访问器的完整本地机器码快照。
        return FALSE; // 未完整读取时输出继续为 unavailable。
    }
    __try {
        signatureValue = accessor(process, &sectionValue);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    for (index = 0U; index + 8U <= sizeof(code); ++index) {
        LONG offset = -1;

        if (code[index] != 0x8AU || code[index + 1U] != 0x81U) {
            continue;
        }
        RtlCopyMemory(&offset, code + index + 2U, sizeof(offset));
        if (offset <= 0 || offset > 0x0FFF) {
            continue;
        }
        // The optional section-signing output is copied through RDX.
        if (code[index + 6U] == 0x88U && code[index + 7U] == 0x02U) {
            if (sectionOffset >= 0 && sectionOffset != offset) {
                return FALSE;
            }
            sectionOffset = offset;
        }
        // The signature level itself is returned in AL.
        if (code[index + 6U] == 0xC3U) {
            if (signatureOffset >= 0 && signatureOffset != offset) {
                return FALSE;
            }
            signatureOffset = offset;
        }
    }
    if (signatureOffset <= 0 || sectionOffset <= 0 ||
        signatureOffset == sectionOffset) {
        return FALSE;
    }
    if (!KswordARKRuntimeReadMemory((const UCHAR*)process + signatureOffset,
            &signatureField, sizeof(signatureField)) || // 猜测偏移只读取完整一字节；未映射页不会直接解引用。
        !KswordARKRuntimeReadMemory((const UCHAR*)process + sectionOffset,
            &sectionField, sizeof(sectionField))) { // 第二字段单独安全读取，不依赖第一字段所在页。
        return FALSE; // 任一候选不可读时均不得发布签名偏移。
    }
    if (signatureField != signatureValue || sectionField != sectionValue) {
        return FALSE;
    }
    *SignatureLevelOffsetOut = signatureOffset;
    *SectionSignatureLevelOffsetOut = sectionOffset;
    return TRUE;
}

LONG
KswordARKDriverResolveProcessProtectionOffset(
    VOID
    )
{
    UNICODE_STRING routineName;
    KSWORD_PROCESS_PROTECTION_ACCESSOR_FN accessor = NULL;
    PEPROCESS process = PsGetCurrentProcess();
    UCHAR code[32];
    UCHAR accessorValue = 0U;
    UCHAR fieldValue = 0U;
    LONG offset = -1;

    if (process == NULL) {
        return -1;
    }
    RtlInitUnicodeString(&routineName, L"PsGetProcessProtection");
    accessor = (KSWORD_PROCESS_PROTECTION_ACCESSOR_FN)
        MmGetSystemRoutineAddress(&routineName);
    if (accessor == NULL) {
        return -1;
    }
    RtlZeroMemory(code, sizeof(code));
    if (!KswordARKRuntimeReadMemory((const VOID*)accessor, code, sizeof(code))) { // 先安全建立保护级别访问器代码快照。
        return -1; // 机器码不可读时不解析偏移。
    }
    __try {
        accessorValue = accessor(process);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    offset = KswordARKDriverDecodeReturnedUcharOffset(code, sizeof(code));
    if (offset <= 0) {
        return -1;
    }
    if (!KswordARKRuntimeReadMemory((const UCHAR*)process + offset,
            &fieldValue, sizeof(fieldValue))) { // 解码候选地址仍可能不可映射，统一完整安全读取。
        return -1; // 读取失败保持保护字段偏移不可用。
    }
    return fieldValue == accessorValue ? offset : -1;
}

LONG
KswordARKDriverResolveProcessSignatureLevelOffset(
    VOID
    )
{
    LONG signatureOffset = -1;
    LONG sectionOffset = -1;
    LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();

    if (protectionOffset <= 0 ||
        !KswordARKDriverResolveProcessSignatureOffsets(
            &signatureOffset,
            &sectionOffset) ||
        sectionOffset + (LONG)sizeof(UCHAR) != protectionOffset ||
        signatureOffset + (LONG)sizeof(UCHAR) != sectionOffset) {
        return -1;
    }
    return signatureOffset;
}

LONG
KswordARKDriverResolveProcessSectionSignatureLevelOffset(
    VOID
    )
{
    LONG signatureOffset = -1;
    LONG sectionOffset = -1;
    LONG protectionOffset = KswordARKDriverResolveProcessProtectionOffset();

    if (protectionOffset <= 0 ||
        !KswordARKDriverResolveProcessSignatureOffsets(
            &signatureOffset,
            &sectionOffset) ||
        sectionOffset + (LONG)sizeof(UCHAR) != protectionOffset ||
        signatureOffset + (LONG)sizeof(UCHAR) != sectionOffset) {
        return -1;
    }
    return sectionOffset;
}

//
// ============================================================================
// EPROCESS.SectionObject / EPROCESS.ObjectTable 的偏移来源边界
// ----------------------------------------------------------------------------
// 这两个偏移原本只有 System Informer 偏移表一个来源，而该表按 ntoskrnl 的
// TimeDateStamp + SizeOfImage 精确匹配，新内核往往不在表内，于是进程列表的
// 「HandleTable」「SectionObject」两列可能为 Unavailable。ObjectTable 保留
// 有界安全读取的运行时来源；SectionObject 没有独立引用锚点时只用精确 profile。
//
// ObjectTable 从导出访问器解出锚点后逐字段安全读取并验证多进程一致性，
// 不把任何猜测的 SectionObject 指针交给内核对象 API。
// ============================================================================
//

// KswordARKDriverDecodeSingleFieldAccessorOffset 作用：
// - 输入：一个形如 `mov rax, [rcx+disp32]; ret` 的导出访问器名；
// - 处理：取其前 8 字节并校验 48 8B 81 <disp32> C3 编码；
// - 返回：disp32；编码不符或超出合理范围时返回 -1。
static LONG
KswordARKDriverDecodeSingleFieldAccessorOffset(
    _In_z_ const WCHAR* RoutineName
    )
{
    UNICODE_STRING routineName;
    const UCHAR* routineAddress = NULL;
    UCHAR code[8];
    LONG offset = -1;

    RtlInitUnicodeString(&routineName, (PWSTR)RoutineName);
    routineAddress = (const UCHAR*)MmGetSystemRoutineAddress(&routineName);
    if (routineAddress == NULL) {
        return -1;
    }

    RtlZeroMemory(code, sizeof(code));
    if (!KswordARKRuntimeReadMemory(routineAddress, code, sizeof(code))) { // 导出访问器代码通过统一安全读取器建立快照。
        return -1; // 代码不完整时拒绝给 ObjectTable 搜索提供锚点。
    }

    // 48 8B 81 <disp32> C3 = mov rax, [rcx+disp32] / ret
    if (code[0] != 0x48U || code[1] != 0x8BU || code[2] != 0x81U || code[7] != 0xC3U) {
        return -1;
    }
    RtlCopyMemory(&offset, code + 3, sizeof(offset));
    if (offset <= 0 || offset > 0x0FFF) {
        return -1;
    }
    return offset;
}

// KswordARKDriverIsProbableKernelPointer 作用：
// - 输入：候选内核指针与需要试读的字节数；
// - 处理：只接受 8 字节对齐的规范内核地址，并要求首尾字节当前均已驻留；
// - 返回：只通过数值/驻留初筛时为 TRUE，不能证明对象类型或生命周期。
// 说明：实际候选读取仍须使用 KswordARKRuntimeReadMemory，不能据此直接读对象头。
static BOOLEAN
KswordARKDriverIsProbableKernelPointer(
    _In_opt_ const VOID* Pointer,
    _In_ ULONG RequiredBytes
    )
{
    const ULONG_PTR value = (ULONG_PTR)Pointer;

    if (Pointer == NULL || (value & 0x7ULL) != 0ULL) {
        return FALSE;
    }
    if (value < 0xFFFF800000000000ULL) {
        return FALSE;
    }
    if (!MmIsAddressValid((PVOID)Pointer)) {
        return FALSE;
    }
    if (!MmIsAddressValid((PVOID)(value + RequiredBytes - 1ULL))) {
        return FALSE;
    }
    return TRUE;
}

LONG
KswordARKDriverResolveProcessSectionObjectOffset(
    VOID
    )
/*++

Routine Description:

    SectionObject 的运行期猜测保持不可用。访问器指令扫描不能证明读出的
    候选是一个存活 Section 对象，且没有独立公开 ID 查询可获得精确引用。
    ObGetObjectType 会直接访问对象头，MmIsAddressValid 和 SEH 不能为猜测
    地址建立对象生命周期，因此只能消费匹配 profile 提供的字段偏移。

Return Value:

    返回 -1，调用方保留已有精确 profile 偏移；缺失时继续报告不可用。

--*/
{
    return -1; // 没有可独立引用并验证的 Section 锚点时，拒绝对猜测对象调用内核对象 API。
}

LONG
KswordARKDriverResolveProcessObjectTableOffset(
    VOID
    )
/*++

Routine Description:

    解析 EPROCESS.ObjectTable 偏移。

    没有任何导出例程直接返回该字段，因此改为「已知锚点之间的受限搜索 +
    跨进程一致性校验」：

    - 锚点取 PsGetProcessId 解出的 UniqueProcessId 偏移，向后覆盖 0x600 字节；
      不假设 ObjectTable 与 Win32Process/SectionObject 等字段的先后顺序，
      因为不同版本的 EPROCESS 会重排字段。
    - 判据：该槽位的指针指向的 _HANDLE_TABLE 内含一个等于本进程 PID 的 ULONG。
      要求同一个槽位偏移与同一个表内偏移在多个采样进程上同时成立，
      并且整个窗口内解唯一；出现第二个自洽解即判为不可信并放弃。

    两个边界都由运行时解码得到，不写死任何版本相关常量。

Return Value:

    成功返回偏移；窗口内无解或解不唯一时返回 -1。

--*/
{
    // 采样进程越多误判概率越低；解析只在初始化时跑一次，取够用即可。
    enum {
        kSampleProcessCapacity = 6,
        kHandleTableProbeBytes = 0x60,
        // 以 UniqueProcessId 为锚点向后覆盖 EPROCESS 主体；窗口内解唯一才采信，
        // 因此宁可放宽范围，也不去假设某个版本的字段先后顺序。
        kObjectTableSearchWindowBytes = 0x600
    };

    PEPROCESS sampleProcesses[kSampleProcessCapacity];
    ULONG samplePids[kSampleProcessCapacity];
    ULONG sampleCount = 0UL;
    ULONG scanPid = 0UL;
    LONG lowerBound = KswordARKDriverDecodeSingleFieldAccessorOffset(L"PsGetProcessId");
    LONG upperBound = 0;
    LONG candidateOffset = -1;
    LONG resolvedOffset = -1;
    ULONG sampleIndex = 0UL;

    if (lowerBound <= 0) {
        return -1;
    }
    lowerBound += (LONG)sizeof(VOID*);
    upperBound = lowerBound + (LONG)kObjectTableSearchWindowBytes;

    RtlZeroMemory(sampleProcesses, sizeof(sampleProcesses));
    RtlZeroMemory(samplePids, sizeof(samplePids));

    // 采样：逐 PID 查找，取到足够数量即停。PID 按 4 的粒度分配。
    for (scanPid = 4UL;
         scanPid <= 0x4000UL && sampleCount < (ULONG)kSampleProcessCapacity;
         scanPid += 4UL) {
        PEPROCESS candidateProcess = NULL;

        if (!NT_SUCCESS(PsLookupProcessByProcessId(ULongToHandle(scanPid), &candidateProcess))) {
            continue;
        }
        if (PsGetProcessExitStatus(candidateProcess) != STATUS_PENDING) {
            // 已退出进程的 ObjectTable 已被置空，无法用于判据。
            ObDereferenceObject(candidateProcess);
            continue;
        }
        sampleProcesses[sampleCount] = candidateProcess;
        samplePids[sampleCount] = scanPid;
        ++sampleCount;
    }

    // 少于两个样本时跨进程一致性校验失去意义，宁可放弃。
    if (sampleCount < 2UL) {
        goto Cleanup;
    }

    for (candidateOffset = lowerBound;
         candidateOffset + (LONG)sizeof(VOID*) <= upperBound;
         candidateOffset += (LONG)sizeof(VOID*)) {
        LONG agreedTableOffset = -1;
        BOOLEAN candidateOk = TRUE;

        for (sampleIndex = 0UL; sampleIndex < sampleCount && candidateOk; ++sampleIndex) {
            const UCHAR* handleTable = NULL;
            LONG tableOffset = 0;
            BOOLEAN matched = FALSE;

            // ObjectTable 偏移还未确认；只能安全复制候选指针，不能直接解引用。
            if (!KswordARKRuntimeReadMemory(
                    (const UCHAR*)sampleProcesses[sampleIndex] + candidateOffset,
                    &handleTable, sizeof(handleTable))) {
                candidateOk = FALSE;
                break;
            }
            if (!KswordARKDriverIsProbableKernelPointer(handleTable, (ULONG)kHandleTableProbeBytes)) {
                candidateOk = FALSE;
                break;
            }

            // _HANDLE_TABLE 头部含 UniqueProcessId；不写死它的位置，
            // 而是要求同一个表内偏移在所有采样进程上都命中各自的 PID。
            for (tableOffset = 0;
                 tableOffset + (LONG)sizeof(ULONG) <= (LONG)kHandleTableProbeBytes;
                 tableOffset += (LONG)sizeof(ULONG)) {
                ULONG tableValue = 0UL;

                if (agreedTableOffset >= 0 && tableOffset != agreedTableOffset) {
                    continue;
                }
                // 表地址和表内偏移都来自候选，逐项完整安全读后再与公开 PID 比对。
                if (!KswordARKRuntimeReadMemory(
                        handleTable + tableOffset, &tableValue, sizeof(tableValue))) {
                    continue;
                }
                if (tableValue != samplePids[sampleIndex]) {
                    continue;
                }
                agreedTableOffset = tableOffset;
                matched = TRUE;
                break;
            }
            if (!matched) {
                candidateOk = FALSE;
            }
        }

        if (!candidateOk) {
            continue;
        }
        if (resolvedOffset >= 0) {
            // 窗口内出现第二个自洽解，说明判据不足以区分，放弃而不是赌一个。
            resolvedOffset = -1;
            goto Cleanup;
        }
        resolvedOffset = candidateOffset;
    }

Cleanup:
    for (sampleIndex = 0UL; sampleIndex < sampleCount; ++sampleIndex) {
        if (sampleProcesses[sampleIndex] != NULL) {
            ObDereferenceObject(sampleProcesses[sampleIndex]);
        }
    }
    return resolvedOffset;
}

// Resolve ZwSetInformationProcess dynamically for broad WDK compatibility.
KSWORD_ZW_SET_INFORMATION_PROCESS_FN
KswordARKDriverResolveZwSetInformationProcess(
    VOID
    )
{
    UNICODE_STRING routineName;
    RtlInitUnicodeString(&routineName, L"ZwSetInformationProcess");
    return (KSWORD_ZW_SET_INFORMATION_PROCESS_FN)MmGetSystemRoutineAddress(&routineName);
}
