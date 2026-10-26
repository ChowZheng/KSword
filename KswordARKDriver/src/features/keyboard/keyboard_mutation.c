/*++

Module Name:

    keyboard_mutation.c

Abstract:

    Rule-proven, snapshot-guarded editing and deletion of ordinary win32k
    RegisterHotKey entries.

Environment:

    Kernel-mode Driver Framework, PASSIVE_LEVEL, original caller context.

--*/

#include "ark/ark_driver.h"
#include "keyboard_internal.h"

#include "../../dispatch/ioctl_validation.h"
#include "../kernel/hook_scan_support.h"

#include <ntimage.h>

#define KSW_HOTKEY_CHAIN_LIMIT 512UL
#define KSW_HOTKEY_ALLOWED_MODIFIERS 0x0000400FUL

// 本机 EnterCrit 实际消费 ECX/EDX；NtUserUnregisterHotKey 在调用前将两参置零。
typedef PVOID(NTAPI* KSW_ENTER_CRIT_FN)(_In_ ULONG Flags, _In_ ULONG Reserved);
typedef VOID(NTAPI* KSW_LEAVE_CRIT_FN)(VOID);
typedef PVOID(NTAPI* KSW_SESSION_STATE_FN)(VOID); // SG模式仅调用已核对Full IAT的真实getter。
typedef PVOID(NTAPI* KSW_VALIDATE_HWND_FN)(_In_ PVOID WindowHandle);
typedef PVOID(NTAPI* KSW_PS_GET_THREAD_WIN32_THREAD_FN)(_In_ PETHREAD Thread);
typedef BOOLEAN(NTAPI* KSW_REMOVE_HOTKEY_SLOT_FN)(_Inout_ PVOID* HotkeySlot); // 一参生命周期helper，不调用跨版本变参wrapper。

typedef struct _KSW_HOTKEY_VIEW
{
    UCHAR Bytes[256]; // 实际hash与读取长度由通用规则ObjectSize决定。
    ULONG_PTR OwnerThreadInfo; // 安装线程指针，必须与请求原快照精确一致。
    ULONG_PTR CallbackAddress; // 普通项写路径仅允许零回调。
    ULONG_PTR WindowHandle; // 规则指定的原始windowValue；可能是HWND，也可能是tagWND*。
    ULONG_PTR DestinationHandle; // schema可选目标字段，缺失时保持明确零值。
    ULONG Modifiers; // 宽度按当前schema读取后零扩展。
    ULONG Flags; // 协议可承载的内部标志最大为DWORD。
    ULONG VirtualKey; // 当前字段宽度读取后的零扩展虚拟键。
    ULONG HotkeyId; // 与owner和window共同构成筛选身份。
    ULONG_PTR NextHotkey; // 当前哈希链后继，换桶时也须回读核对。
    ULONG_PTR ChildFlink; // 子项链前向链接，必须指向本对象的链头。
    ULONG_PTR ChildBlink; // 子项链反向链接，同样必须自指。
    ULONG64 SnapshotHash; // 完整实际对象字节的哈希，未确认副作用时不能沿用旧值。
} KSW_HOTKEY_VIEW;

typedef struct _KSW_HOTKEY_RUNTIME
{
    KSW_KEYBOARD_HOTKEY_RUNTIME Layout; // 当前代码证明及请求会话绑定的值副本。
    KSW_ENTER_CRIT_FN EnterCrit; // 按已证明两参ABI进入USER临界区。
    KSW_LEAVE_CRIT_FN LeaveCrit; // 与成功进入操作严格成对退出。
    KSW_VALIDATE_HWND_FN ValidateHwnd; // 真实Base导出的窗口身份验证器。
    KSW_REMOVE_HOTKEY_SLOT_FN RemoveSlot; // 仅接受经当前链遍历找到的节点槽地址。
    KSW_PS_GET_THREAD_WIN32_THREAD_FN PsGetThreadWin32Thread; // 公开内核访问器用于确认调用线程USER上下文。
    ULONG TimeDateStamp; // 回执元数据，不参与版本授权。
    ULONG ImageSize; // 当前Full实际映像范围。
    ULONG PdbAge; // 兼容回执字段，规则实现不依赖PDB名单。
} KSW_HOTKEY_RUNTIME;

NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_z_ PCSTR RoutineName
    );

/* 当前映像只通过通用规则与代码指纹取得能力，PE身份仅用于缓存和诊断。 */
static NTSTATUS
KswHotkeyResolveRuntime(_Out_ KSW_HOTKEY_RUNTIME* RuntimeOut)
{
    UNICODE_STRING name; // 当前线程身份使用公开内核访问器。
    NTSTATUS status; // 保留规则匹配及实际上下文失败，不降级为无结果。
    if (!RuntimeOut) return STATUS_INVALID_PARAMETER; // 空输出不执行解析或导出调用。
    RtlZeroMemory(RuntimeOut, sizeof(*RuntimeOut)); // 任何失败都不能残留上次可调用入口。
    status = KswordARKKeyboardResolveHotkeyRuntime(&RuntimeOut->Layout); // 共用枚举侧同一生产规则及真实IAT绑定。
    if (!NT_SUCCESS(status)) return status; // 没有完整证明就不构造函数指针。
    if (!RuntimeOut->Layout.RuleProven || !RuntimeOut->Layout.ObjectSize || RuntimeOut->Layout.ObjectSize > 256UL) return STATUS_NOT_SUPPORTED; // 本地固定快照能完整容纳当前schema才允许变更。
    RuntimeOut->EnterCrit = (KSW_ENTER_CRIT_FN)((UCHAR*)RuntimeOut->Layout.Win32kbaseBase + RuntimeOut->Layout.EnterCritRva); // 入口RVA已经与Base真实导出交叉绑定。
    RuntimeOut->LeaveCrit = (KSW_LEAVE_CRIT_FN)((UCHAR*)RuntimeOut->Layout.Win32kbaseBase + RuntimeOut->Layout.LeaveCritRva); // 退出入口必须来自同一被验证Base映像。
    RuntimeOut->ValidateHwnd = (KSW_VALIDATE_HWND_FN)((UCHAR*)RuntimeOut->Layout.Win32kbaseBase + RuntimeOut->Layout.ValidateHwndRva); // 窗口验证不能使用用户提供地址。
    RuntimeOut->RemoveSlot = (KSW_REMOVE_HOTKEY_SLOT_FN)((UCHAR*)RuntimeOut->Layout.Win32kfullBase + RuntimeOut->Layout.RemoveRva); // 只绑定独立证明的一参helper，不能调用扫描器。
    RtlInitUnicodeString(&name, L"PsGetThreadWin32Thread"); // 当前线程身份不借用私有结构硬编码偏移。
    RuntimeOut->PsGetThreadWin32Thread = (KSW_PS_GET_THREAD_WIN32_THREAD_FN)MmGetSystemRoutineAddress(&name); // 使用实际内核公开导出。
    RuntimeOut->TimeDateStamp = RuntimeOut->Layout.Win32kfullTimeDateStamp; // 非授权条件，仅回执当前映像元数据。
    RuntimeOut->ImageSize = RuntimeOut->Layout.Win32kfullSize; RuntimeOut->PdbAge = 0UL; // 不再按RSDS目录授权。
    return RuntimeOut->PsGetThreadWin32Thread ? STATUS_SUCCESS : STATUS_PROCEDURE_NOT_FOUND; // 缺少调用线程身份能力时明确拒绝。
}

/* 对象模式仅做有界头部候选读回，再由系统句柄验证器确认同一个对象。 */
static PVOID KswHotkeyValidateWindowObject(const KSW_HOTKEY_RUNTIME* Runtime, ULONG_PTR RawWindow)
{
    ULONG_PTR candidates[8]; ULONG i, j; // 最多64字节，不依赖某个版本的HEAD字段偏移。
    if (!RawWindow) return NULL; // 零值线程热键由调用者单独允许。
    if (Runtime->Layout.WindowRepresentation == 0UL) return Runtime->ValidateHwnd((PVOID)RawWindow); // HWND模式直接按真实句柄调用系统验证器。
    // tagWND模式仅允许完整、对齐且可安全读取的内核对象头，不猜固定HEAD偏移。
    if (Runtime->Layout.WindowRepresentation != 1UL || RawWindow < (ULONG_PTR)MmSystemRangeStart ||
        (RawWindow & 7U) != 0U || RawWindow > MAXULONG_PTR - sizeof(candidates) ||
        !KswordARKHookReadMemorySafe((const VOID*)RawWindow, candidates, sizeof(candidates))) return NULL;
    for (i = 0UL; i < RTL_NUMBER_OF(candidates); ++i) { // 所有候选仅来自固定64字节对象头。
        if (candidates[i] == 0U || candidates[i] == 1U || candidates[i] >= MAXULONG_PTR - 16U) continue; // 不把特殊sentinel当普通HWND。
        for (j = 0UL; j < i; ++j) if (candidates[j] == candidates[i]) break; // 重复数值不应重复调用系统窗口验证器。
        if (j != i) continue; // 同一候选只调用一次系统验证器。
        if (Runtime->ValidateHwnd((PVOID)candidates[i]) == (PVOID)RawWindow) return (PVOID)RawWindow; // 只有精确往返到原对象才接受。
    }
    return NULL; // 未有精确对象往返的候选就不允许写入。
}

/* 每个字段都单独校验范围，避免损坏schema形成越界节点访问。 */
static BOOLEAN KswHotkeyReadField(const UCHAR* Bytes, ULONG Size, ULONG Offset, ULONG Width, VOID* Output, ULONG OutputSize)
{
    if (!Bytes || !Output || !Width || Width > OutputSize || Offset > Size || Width > Size - Offset) return FALSE; // 防止偏移回绕、截断及超出schema对象范围。
    RtlZeroMemory(Output, OutputSize); RtlCopyMemory(Output, Bytes + Offset, Width); // 按原宽度完整复制并对剩余高位零扩展。
    return TRUE; // 只有完整字段才进入快照身份比较。
}
static BOOLEAN
KswHotkeyReadView(_In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout,
    _In_ ULONG_PTR HotkeyObject, _Out_ KSW_HOTKEY_VIEW* ViewOut)
{
    // 仅对规则已证明且完整可读的普通内核对象建立快照。
    if (!Layout || !ViewOut || HotkeyObject < (ULONG_PTR)MmSystemRangeStart || (HotkeyObject & 7U) != 0U ||
        !Layout->RuleProven || Layout->ObjectSize == 0UL ||
        Layout->ObjectSize > sizeof(ViewOut->Bytes) || HotkeyObject > MAXULONG_PTR - Layout->ObjectSize) return FALSE;
    RtlZeroMemory(ViewOut, sizeof(*ViewOut)); // 可选缺失字段不得从上次快照继承。
    if (!KswordARKHookReadMemorySafe((const VOID*)HotkeyObject, ViewOut->Bytes, Layout->ObjectSize)) return FALSE; // 一次完整对象读取后再按schema投影字段。
    // 每个角色按其实际宽度取值，零偏移合法，缺失Destination不会误读Owner。
    if (!KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->OwnerOffset, 8UL, &ViewOut->OwnerThreadInfo, sizeof(ViewOut->OwnerThreadInfo)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->CallbackOffset, 8UL, &ViewOut->CallbackAddress, sizeof(ViewOut->CallbackAddress)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->WindowOffset, 8UL, &ViewOut->WindowHandle, sizeof(ViewOut->WindowHandle)) ||
        (Layout->DestinationWidth != 0UL && !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->DestinationOffset, Layout->DestinationWidth, &ViewOut->DestinationHandle, sizeof(ViewOut->DestinationHandle))) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->ModifiersOffset, Layout->ModifiersWidth, &ViewOut->Modifiers, sizeof(ViewOut->Modifiers)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->FlagsOffset, Layout->FlagsWidth, &ViewOut->Flags, sizeof(ViewOut->Flags)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->VkOffset, Layout->VkWidth, &ViewOut->VirtualKey, sizeof(ViewOut->VirtualKey)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->IdOffset, Layout->IdWidth, &ViewOut->HotkeyId, sizeof(ViewOut->HotkeyId)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->NextOffset, 8UL, &ViewOut->NextHotkey, sizeof(ViewOut->NextHotkey)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->ChildOffset, 8UL, &ViewOut->ChildFlink, sizeof(ViewOut->ChildFlink)) ||
        !KswHotkeyReadField(ViewOut->Bytes, Layout->ObjectSize, Layout->ChildOffset + 8UL, 8UL, &ViewOut->ChildBlink, sizeof(ViewOut->ChildBlink))) return FALSE;
    ViewOut->SnapshotHash = KswordARKKeyboardHashHotkeyObject(ViewOut->Bytes, Layout->ObjectSize); // 完整实际对象长度属于快照身份。
    return TRUE; // 只有完整字段集合与实际对象hash均已取得才返回成功。
}

/* 数值写入只接受schema已证明的宽度，不能静默截断新值或回滚值。 */
static BOOLEAN KswHotkeyWriteValue(ULONG_PTR Address, ULONG Width, ULONG Value)
{
    if (Width == 1UL && Value <= MAXUCHAR) { *(volatile UCHAR*)Address = (UCHAR)Value; return TRUE; } // BYTE仅允许无损表示的新值或回滚值。
    if (Width == 2UL && Value <= MAXUSHORT) { *(volatile USHORT*)Address = (USHORT)Value; return TRUE; } // WORD不静默丢弃高位。
    if (Width == 4UL) { *(volatile ULONG*)Address = Value; return TRUE; } // DWORD覆盖协议承载的全部数值位。
    return FALSE; // 未证明宽度或超范围数值不产生写入。
}
static BOOLEAN
KswHotkeyViewMatchesRequest(
    _In_ const KSW_HOTKEY_VIEW* View,
    _In_ const KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_REQUEST* Request
    )
{
    return View != NULL && Request != NULL &&
        View->OwnerThreadInfo == (ULONG_PTR)Request->expectedThreadInfo &&
        View->WindowHandle == (ULONG_PTR)Request->expectedWindowHandle &&
        View->DestinationHandle == (ULONG_PTR)Request->expectedDestinationHandle &&
        View->CallbackAddress == (ULONG_PTR)Request->expectedCallbackAddress &&
        View->NextHotkey == (ULONG_PTR)Request->expectedNextHotkeyObject &&
        View->ChildFlink == (ULONG_PTR)Request->expectedChildListFlink &&
        View->ChildBlink == (ULONG_PTR)Request->expectedChildListBlink &&
        View->Modifiers == Request->expectedModifiers &&
        View->Flags == Request->expectedModifierFlags2 &&
        View->VirtualKey == Request->expectedVirtualKey &&
        View->HotkeyId == Request->expectedHotkeyId &&
        View->SnapshotHash == Request->expectedSnapshotHash;
}

static BOOLEAN
KswHotkeyViewIsOrdinaryStandalone(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout, // 同一个已验证schema贯穿所有读取、写入和回滚。
    _In_ ULONG_PTR HotkeyObject,
    _In_ const KSW_HOTKEY_VIEW* View
    )
{
    const ULONG_PTR childHead = HotkeyObject + Layout->ChildOffset;

    return View != NULL && View->OwnerThreadInfo != 0U &&
        View->CallbackAddress == 0U && View->Flags == 0U &&
        View->VirtualKey != 0UL && View->VirtualKey <= 0xFFUL &&
        (View->Modifiers & ~KSW_HOTKEY_ALLOWED_MODIFIERS) == 0UL &&
        View->WindowHandle != 1U && View->WindowHandle < MAXULONG_PTR - 16U &&
        (Layout->WindowRepresentation == 0UL || View->WindowHandle == 0U ||
            (View->WindowHandle >= (ULONG_PTR)MmSystemRangeStart && (View->WindowHandle & 7U) == 0U)) &&
        View->ChildFlink == childHead && View->ChildBlink == childHead;
}

static NTSTATUS
KswHotkeyFindObjectLink(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout, // 同一个已验证schema贯穿所有读取、写入和回滚。
    _In_ ULONG_PTR TableAddress,
    _In_ ULONG BucketIndex,
    _In_ ULONG_PTR HotkeyObject,
    _Out_ ULONG_PTR* LinkAddressOut,
    _Out_ ULONG_PTR* NextHotkeyOut
    )
{
    ULONG_PTR linkAddress = TableAddress + ((ULONG_PTR)BucketIndex * sizeof(PVOID));
    ULONG_PTR currentObject = 0U;
    ULONG depth = 0UL;

    if (LinkAddressOut == NULL || NextHotkeyOut == NULL || BucketIndex >= Layout->BucketCount) {
        return STATUS_INVALID_PARAMETER;
    }
    *LinkAddressOut = 0U;
    *NextHotkeyOut = 0U;
    while (depth < KSW_HOTKEY_CHAIN_LIMIT) {
        if (!KswordARKHookReadMemorySafe((const VOID*)linkAddress, &currentObject, sizeof(currentObject))) {
            return STATUS_PARTIAL_COPY;
        }
        if (currentObject == 0U) {
            return STATUS_NOT_FOUND;
        }
        if (currentObject == HotkeyObject) {
            KSW_HOTKEY_VIEW view;

            if (!KswHotkeyReadView(Layout, currentObject, &view)) {
                return STATUS_PARTIAL_COPY;
            }
            *LinkAddressOut = linkAddress;
            *NextHotkeyOut = view.NextHotkey;
            return STATUS_SUCCESS;
        }
        if (currentObject < (ULONG_PTR)MmSystemRangeStart || (currentObject & 7U) != 0U ||
            currentObject > MAXULONG_PTR - Layout->NextOffset) return STATUS_DATA_ERROR; // 损坏链不能通过地址回绕穿过目标校验。
        linkAddress = currentObject + Layout->NextOffset;
        ++depth;
    }
    return STATUS_DATA_ERROR;
}

static NTSTATUS
KswHotkeyFindConflict(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout, // 同一个已验证schema贯穿所有读取、写入和回滚。
    _In_ ULONG_PTR TableAddress,
    _In_ ULONG_PTR TargetObject,
    _In_ ULONG Modifiers,
    _In_ ULONG VirtualKey
    )
{
    ULONG bucketIndex = 0UL;

    for (bucketIndex = 0UL; bucketIndex < Layout->BucketCount; ++bucketIndex) {
        ULONG_PTR currentObject = 0U;
        ULONG depth = 0UL;
        const ULONG_PTR headAddress = TableAddress + ((ULONG_PTR)bucketIndex * sizeof(PVOID));

        if (!KswordARKHookReadMemorySafe((const VOID*)headAddress, &currentObject, sizeof(currentObject))) {
            return STATUS_PARTIAL_COPY;
        }
        while (currentObject != 0U && depth < KSW_HOTKEY_CHAIN_LIMIT) {
            KSW_HOTKEY_VIEW view;

            if (!KswHotkeyReadView(Layout, currentObject, &view)) {
                return STATUS_PARTIAL_COPY;
            }
            if (currentObject != TargetObject &&
                view.Modifiers == Modifiers && view.VirtualKey == VirtualKey) {
                return STATUS_OBJECT_NAME_COLLISION;
            }
            if (view.NextHotkey == currentObject) {
                return STATUS_DATA_ERROR;
            }
            currentObject = view.NextHotkey;
            ++depth;
        }
        if (currentObject != 0U) {
            return STATUS_DATA_ERROR;
        }
    }
    return STATUS_SUCCESS;
}

/* 编辑失败后完整遍历实际桶链，只有唯一归属或完整缺失才能用于回执。 */
static NTSTATUS KswHotkeyFindObjectBucket(const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout,
    ULONG_PTR TableAddress, ULONG_PTR HotkeyObject, ULONG* BucketIndexOut)
{
    ULONG bucketIndex, matchCount = 0UL; // 不根据预期新旧桶推断实际链归属。
    if (BucketIndexOut == NULL) return STATUS_INVALID_PARAMETER;
    *BucketIndexOut = MAXULONG;
    for (bucketIndex = 0UL; bucketIndex < Layout->BucketCount; ++bucketIndex) {
        ULONG_PTR currentObject = 0U; ULONG depth = 0UL;
        if (!KswordARKHookReadMemorySafe((const VOID*)(TableAddress + (ULONG_PTR)bucketIndex * sizeof(PVOID)),
                &currentObject, sizeof(currentObject))) return STATUS_PARTIAL_COPY; // 任一桶头读不全就不发布当前快照。
        while (currentObject != 0U && depth < KSW_HOTKEY_CHAIN_LIMIT) {
            ULONG_PTR nextObject = 0U;
            if (currentObject < (ULONG_PTR)MmSystemRangeStart || (currentObject & 7U) != 0U ||
                currentObject > MAXULONG_PTR - Layout->NextOffset - sizeof(nextObject)) return STATUS_DATA_ERROR;
            if (!KswordARKHookReadMemorySafe((const VOID*)(currentObject + Layout->NextOffset),
                    &nextObject, sizeof(nextObject))) return STATUS_PARTIAL_COPY; // 包含目标之后的链尾也必须可读。
            if (currentObject == HotkeyObject) {
                ++matchCount; *BucketIndexOut = bucketIndex;
                if (matchCount != 1UL) return STATUS_DATA_ERROR; // 重复挂链不能伪装成一个已确认的桶。
            }
            if (nextObject == currentObject) return STATUS_DATA_ERROR;
            currentObject = nextObject; ++depth;
        }
        if (currentObject != 0U) return STATUS_DATA_ERROR; // 环或超深链保持未确认状态。
    }
    return matchCount == 1UL ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static BOOLEAN
KswHotkeyOtherBytesUnchanged(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout, // 同一个已验证schema贯穿所有读取、写入和回滚。
    _In_ const KSW_HOTKEY_VIEW* Before,
    _In_ const KSW_HOTKEY_VIEW* After
    )
{
    ULONG byteIndex = 0UL;

    if (Before == NULL || After == NULL) {
        return FALSE;
    }
    for (byteIndex = 0UL; byteIndex < Layout->ObjectSize; ++byteIndex) {
        const BOOLEAN mutableByte =
            (byteIndex >= Layout->ModifiersOffset && byteIndex < Layout->ModifiersOffset + Layout->ModifiersWidth) ||
            (byteIndex >= Layout->VkOffset && byteIndex < Layout->VkOffset + Layout->VkWidth) ||
            (byteIndex >= Layout->NextOffset && byteIndex < Layout->NextOffset + sizeof(PVOID));

        if (!mutableByte && Before->Bytes[byteIndex] != After->Bytes[byteIndex]) {
            return FALSE;
        }
    }
    return TRUE;
}

static NTSTATUS
KswHotkeyWriteEdit(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout, // 同一个已验证schema贯穿所有读取、写入和回滚。
    _In_ ULONG_PTR TableAddress,
    _In_ ULONG_PTR OldLinkAddress,
    _In_ ULONG OldBucketIndex,
    _In_ ULONG NewBucketIndex,
    _In_ ULONG_PTR HotkeyObject,
    _In_ const KSW_HOTKEY_VIEW* Before,
    _In_ ULONG NewModifiers,
    _In_ ULONG NewVirtualKey,
    _Out_ KSW_HOTKEY_VIEW* AfterOut,
    _Out_ BOOLEAN* RolledBackOut
    )
{
    ULONG_PTR newHeadAddress = 0U;
    ULONG_PTR newHeadObject = 0U;
    ULONG_PTR verifyPointer = 0U;
    BOOLEAN rebucket = FALSE;
    BOOLEAN writeStarted = FALSE;
    BOOLEAN validPostState = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    if (Before == NULL || AfterOut == NULL || RolledBackOut == NULL ||
        OldBucketIndex >= Layout->BucketCount || NewBucketIndex >= Layout->BucketCount) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(AfterOut, sizeof(*AfterOut));
    *RolledBackOut = FALSE;
    rebucket = OldBucketIndex != NewBucketIndex;
    newHeadAddress = TableAddress + ((ULONG_PTR)NewBucketIndex * sizeof(PVOID));
    if (rebucket && !KswordARKHookReadMemorySafe(
            (const VOID*)newHeadAddress,
            &newHeadObject,
            sizeof(newHeadObject))) {
        return STATUS_PARTIAL_COPY;
    }

    __try {
        writeStarted = TRUE;
        if (rebucket) {
            *(volatile ULONG_PTR*)OldLinkAddress = Before->NextHotkey;
            *(volatile ULONG_PTR*)(HotkeyObject + Layout->NextOffset) = newHeadObject;
            *(volatile ULONG_PTR*)newHeadAddress = HotkeyObject;
        }
        if (!KswHotkeyWriteValue(HotkeyObject + Layout->ModifiersOffset, Layout->ModifiersWidth, NewModifiers)) { status = STATUS_INVALID_PARAMETER; __leave; } // 拒绝超出实际字段宽度的新值。
        if (!KswHotkeyWriteValue(HotkeyObject + Layout->VkOffset, Layout->VkWidth, NewVirtualKey)) { status = STATUS_INVALID_PARAMETER; __leave; } // 虚拟键也按派生宽度写入。
        KeMemoryBarrier();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    if (NT_SUCCESS(status) && KswHotkeyReadView(Layout, HotkeyObject, AfterOut)) {
        validPostState = AfterOut->Modifiers == NewModifiers &&
            AfterOut->VirtualKey == NewVirtualKey &&
            AfterOut->NextHotkey == (rebucket ? newHeadObject : Before->NextHotkey) &&
            KswHotkeyOtherBytesUnchanged(Layout, Before, AfterOut);
        if (validPostState && KswordARKHookReadMemorySafe(
                (const VOID*)(rebucket ? newHeadAddress : OldLinkAddress),
                &verifyPointer,
                sizeof(verifyPointer))) {
            validPostState = verifyPointer == HotkeyObject;
        }
        else {
            validPostState = FALSE;
        }
        if (validPostState && rebucket && KswordARKHookReadMemorySafe(
                (const VOID*)OldLinkAddress,
                &verifyPointer,
                sizeof(verifyPointer))) {
            validPostState = verifyPointer == Before->NextHotkey;
        }
        else if (validPostState && rebucket) {
            validPostState = FALSE;
        }
    }
    if (validPostState) {
        return STATUS_SUCCESS;
    }
    if (NT_SUCCESS(status)) {
        status = STATUS_DATA_ERROR;
    }

    if (writeStarted) {
        KSW_HOTKEY_VIEW rollbackView;
        BOOLEAN rollbackValid = FALSE;

        RtlZeroMemory(&rollbackView, sizeof(rollbackView));
        __try {
            (VOID)KswHotkeyWriteValue(HotkeyObject + Layout->ModifiersOffset, Layout->ModifiersWidth, Before->Modifiers); // 快照值按原字段宽度恢复。
            (VOID)KswHotkeyWriteValue(HotkeyObject + Layout->VkOffset, Layout->VkWidth, Before->VirtualKey); // 回滚后仍须完整hash复核。
            if (rebucket) {
                *(volatile ULONG_PTR*)newHeadAddress = newHeadObject;
                *(volatile ULONG_PTR*)(HotkeyObject + Layout->NextOffset) = Before->NextHotkey;
                *(volatile ULONG_PTR*)OldLinkAddress = HotkeyObject;
            }
            KeMemoryBarrier();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            rollbackValid = FALSE;
        }
        if (KswHotkeyReadView(Layout, HotkeyObject, &rollbackView) &&
            rollbackView.SnapshotHash == Before->SnapshotHash &&
            KswordARKHookReadMemorySafe(
                (const VOID*)OldLinkAddress,
                &verifyPointer,
                sizeof(verifyPointer)) &&
            verifyPointer == HotkeyObject) {
            rollbackValid = TRUE;
            if (rebucket && (!KswordARKHookReadMemorySafe(
                    (const VOID*)newHeadAddress,
                    &verifyPointer,
                    sizeof(verifyPointer)) || verifyPointer != newHeadObject)) {
                rollbackValid = FALSE;
            }
        }
        *RolledBackOut = rollbackValid;
    }
    return status;
}

static VOID
KswHotkeyInitializeResponse(
    _Out_ KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_RESPONSE* Response,
    _In_opt_ const KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_REQUEST* Request
    )
{
    RtlZeroMemory(Response, sizeof(*Response));
    Response->size = sizeof(*Response);
    Response->version = KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION;
    Response->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_INVALID_REQUEST;
    if (Request != NULL) {
        Response->operation = Request->operation;
        Response->previousBucketIndex = Request->expectedBucketIndex;
        Response->currentBucketIndex = Request->expectedBucketIndex;
        Response->previousModifiers = Request->expectedModifiers;
        Response->currentModifiers = Request->expectedModifiers;
        Response->previousVirtualKey = Request->expectedVirtualKey;
        Response->currentVirtualKey = Request->expectedVirtualKey;
        Response->hotkeyObject = Request->hotkeyObject;
        Response->sessionGlobals = Request->sessionGlobals;
        Response->previousSnapshotHash = Request->expectedSnapshotHash;
        Response->currentSnapshotHash = Request->expectedSnapshotHash;
    }
}

static BOOLEAN
KswHotkeyRequestShapeValid(
    _In_ const KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_REQUEST* Request
    )
{
    if (Request == NULL || Request->size != sizeof(*Request) ||
        Request->version != KSWORD_ARK_KEYBOARD_PROTOCOL_VERSION ||
        (Request->flags & ~KSWORD_ARK_KEYBOARD_MUTATION_FLAG_UI_CONFIRMED) != 0UL ||
        (Request->flags & KSWORD_ARK_KEYBOARD_MUTATION_FLAG_UI_CONFIRMED) == 0UL ||
        Request->confirmationToken != KSWORD_ARK_KEYBOARD_MUTATION_CONFIRMATION_TOKEN ||
        Request->hotkeyObject == 0ULL || Request->sessionGlobals == 0ULL ||
        Request->expectedThreadInfo == 0ULL || Request->expectedSnapshotHash == 0ULL ||
        Request->expectedBucketIndex >= 256UL ||
        Request->expectedModifiers > 0xFFFFUL || Request->expectedModifierFlags2 > 0xFFFFUL ||
        Request->expectedVirtualKey == 0UL || Request->expectedVirtualKey > 0xFFUL) {
        return FALSE;
    }
    if (Request->operation == KSWORD_ARK_KEYBOARD_MUTATION_OPERATION_EDIT) {
        return Request->newModifiers <= 0xFFFFUL &&
            (Request->newModifiers & ~KSW_HOTKEY_ALLOWED_MODIFIERS) == 0UL &&
            Request->newVirtualKey != 0UL && Request->newVirtualKey <= 0xFFUL;
    }
    if (Request->operation == KSWORD_ARK_KEYBOARD_MUTATION_OPERATION_DELETE) {
        return Request->newModifiers == 0UL && Request->newVirtualKey == 0UL;
    }
    return FALSE;
}

static NTSTATUS
KswHotkeyCountOwnerWindowIdMatches(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Layout, // 同一个已验证schema贯穿所有读取、写入和回滚。
    _In_ ULONG_PTR TableAddress,
    _In_ const KSW_HOTKEY_VIEW* TargetView,
    _Out_ ULONG* MatchCountOut
    )
{
    ULONG bucketIndex = 0UL;
    ULONG matchCount = 0UL;

    if (TargetView == NULL || MatchCountOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *MatchCountOut = 0UL;
    for (bucketIndex = 0UL; bucketIndex < Layout->BucketCount; ++bucketIndex) {
        ULONG_PTR currentObject = 0U;
        ULONG depth = 0UL;
        const ULONG_PTR headAddress = TableAddress + ((ULONG_PTR)bucketIndex * sizeof(PVOID));

        if (!KswordARKHookReadMemorySafe((const VOID*)headAddress, &currentObject, sizeof(currentObject))) {
            return STATUS_PARTIAL_COPY;
        }
        while (currentObject != 0U && depth < KSW_HOTKEY_CHAIN_LIMIT) {
            KSW_HOTKEY_VIEW view;

            if (!KswHotkeyReadView(Layout, currentObject, &view)) {
                return STATUS_PARTIAL_COPY;
            }
            if (view.OwnerThreadInfo == TargetView->OwnerThreadInfo &&
                view.WindowHandle == TargetView->WindowHandle &&
                view.HotkeyId == TargetView->HotkeyId) {
                ++matchCount;
                if (matchCount > 1UL) {
                    *MatchCountOut = matchCount;
                    return STATUS_SUCCESS;
                }
            }
            if (view.NextHotkey == currentObject) {
                return STATUS_DATA_ERROR;
            }
            currentObject = view.NextHotkey;
            ++depth;
        }
        if (currentObject != 0U) {
            return STATUS_DATA_ERROR;
        }
    }
    *MatchCountOut = matchCount;
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKKeyboardIoctlMutateHotkey(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesReturned
    )
{
    KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_REQUEST* inputBuffer = NULL;
    KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_RESPONSE* outputBuffer = NULL;
    KSWORD_ARK_MUTATE_KEYBOARD_HOTKEY_REQUEST requestSnapshot;
    KSWORD_ARK_SAFETY_CONTEXT safetyContext;
    KSW_HOTKEY_RUNTIME runtime;
    KSW_HOTKEY_VIEW beforeView = { 0 }; // SEH异常路径也只能观察完整零初始化对象。
    KSW_HOTKEY_VIEW afterView = { 0 }; // 删除读回不依赖编辑路径中的后置清零。
    PVOID windowObject = NULL;
    PVOID callerThreadInfo = NULL;
    ULONG_PTR tableAddress = 0U;
    ULONG currentSessionId = MAXULONG; // USER临界区内再次确认SID，与namespace分别验证。
    ULONG_PTR validatedWindowHandle = 0U; // 窗口对象必须仍对应同一HWND快照。
    ULONG_PTR oldLinkAddress = 0U;
    ULONG_PTR oldNextObject = 0U;
    ULONG matchCount = 0UL;
    ULONG newBucketIndex = 0UL;
    size_t actualInputLength = 0U;
    size_t actualOutputLength = 0U;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN critEntered = FALSE;
    BOOLEAN operationChanged = FALSE;
    BOOLEAN deletionAttempted = FALSE; // 布尔失败或异常不能被解释成没有副作用。
    BOOLEAN editAttempted = FALSE; // 编辑开始后只有真实读回或已验证回滚才能恢复当前hash。
    BOOLEAN snapshotEstablished = FALSE; // 只有完整快照及ordinary门禁通过后才允许异常后比较。
    BOOLEAN rolledBack = FALSE;
    static const WCHAR targetText[] = L"win32k RegisterHotKey table";

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesReturned = 0U;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = KswordARKValidateDeviceIoControlWriteAccess(Request);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = KswordARKRetrieveRequiredInputBuffer(
        Request,
        sizeof(requestSnapshot),
        (PVOID*)&inputBuffer,
        &actualInputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    RtlCopyMemory(&requestSnapshot, inputBuffer, sizeof(requestSnapshot));
    status = KswordARKRetrieveRequiredOutputBuffer(
        Request,
        sizeof(*outputBuffer),
        (PVOID*)&outputBuffer,
        &actualOutputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    KswHotkeyInitializeResponse(outputBuffer, &requestSnapshot);
    *BytesReturned = sizeof(*outputBuffer);
    if (!KswHotkeyRequestShapeValid(&requestSnapshot)) {
        outputBuffer->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&safetyContext, sizeof(safetyContext));
    safetyContext.Operation = KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
    safetyContext.ContextFlags = KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED;
    safetyContext.TargetText = targetText;
    safetyContext.TargetTextChars = (USHORT)(RTL_NUMBER_OF(targetText) - 1U);
    status = KswordARKSafetyEvaluate(Device, &safetyContext);
    if (!NT_SUCCESS(status)) {
        outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_SAFETY_DENIED;
        outputBuffer->lastStatus = status;
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&runtime, sizeof(runtime));
    status = KswHotkeyResolveRuntime(&runtime);
    if (!NT_SUCCESS(status)) {
        outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_UNSUPPORTED_BUILD;
        outputBuffer->lastStatus = status;
        return STATUS_SUCCESS;
    }
    if (requestSnapshot.expectedBucketIndex >= runtime.Layout.BucketCount ||
        requestSnapshot.expectedBucketIndex != (requestSnapshot.expectedVirtualKey & (runtime.Layout.BucketCount - 1UL)) ||
        (requestSnapshot.operation == KSWORD_ARK_KEYBOARD_MUTATION_OPERATION_EDIT &&
            ((runtime.Layout.ModifiersWidth == 1UL && requestSnapshot.newModifiers > MAXUCHAR) ||
                (runtime.Layout.ModifiersWidth == 2UL && requestSnapshot.newModifiers > MAXUSHORT) ||
                (runtime.Layout.VkWidth == 1UL && requestSnapshot.newVirtualKey > MAXUCHAR)))) {
        outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_INVALID_REQUEST;
        outputBuffer->lastStatus = STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS; // 初始包仅做256桶上限检查，此处必须服从真实schema。
    }
    outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_IDENTITY_VALIDATED;
    outputBuffer->imageTimeDateStamp = runtime.TimeDateStamp;
    outputBuffer->imageSize = runtime.ImageSize;
    outputBuffer->pdbAge = runtime.PdbAge;

    callerThreadInfo = runtime.PsGetThreadWin32Thread(PsGetCurrentThread());
    if (callerThreadInfo == NULL || runtime.Layout.SessionGlobals == 0U ||
        runtime.Layout.SessionId != requestSnapshot.expectedSessionId ||
        runtime.Layout.SessionGlobals != (ULONG_PTR)requestSnapshot.sessionGlobals) {
        outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_CALLER_CONTEXT_REQUIRED;
        outputBuffer->lastStatus = STATUS_INVALID_DEVICE_STATE;
        return STATUS_SUCCESS;
    }
    outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CALLER_VALIDATED;
    tableAddress = runtime.Layout.TableBase ? runtime.Layout.TableBase : runtime.Layout.UserSessionState + runtime.Layout.TableOffset; // 已验证相同会话基址的表。

    __try {
        status = KswordARKKeyboardValidateRuleRuntime(&runtime.Layout); // 调用私有入口前也复核其代码与当前缓存代次。
        if (!NT_SUCCESS(status)) { outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT; __leave; }
        // 按经反汇编核对的 USER 入口传参，不让未定义寄存器值进入临界区实现。
        (VOID)runtime.EnterCrit(0UL, 0UL);
        critEntered = TRUE;
        status = KswordARKKeyboardValidateRuleRuntime(&runtime.Layout); // 临界区内只核对指纹，不重建证明图。
        if (!NT_SUCCESS(status)) { outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT; __leave; }
        status = KswordARKKeyboardGetSessionId(&currentSessionId); // SID不与namespace指针混合。
        if (!NT_SUCCESS(status) || currentSessionId != runtime.Layout.SessionId || currentSessionId != requestSnapshot.expectedSessionId ||
            runtime.Layout.SessionGlobals != (ULONG_PTR)requestSnapshot.sessionGlobals) {
            status = STATUS_REVISION_MISMATCH;
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT;
            __leave;
        }
        if (runtime.Layout.UserSessionState != 0U &&
            ((KSW_SESSION_STATE_FN)runtime.Layout.SessionGetterAddress)() != (PVOID)requestSnapshot.sessionGlobals) {
            status = STATUS_REVISION_MISMATCH; outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT; __leave; // SG基址不能在进入临界区时漂移。
        }
        status = KswHotkeyFindObjectLink(&runtime.Layout,
            tableAddress,
            requestSnapshot.expectedBucketIndex,
            (ULONG_PTR)requestSnapshot.hotkeyObject,
            &oldLinkAddress,
            &oldNextObject);
        if (!NT_SUCCESS(status) || !KswHotkeyReadView(&runtime.Layout,
                (ULONG_PTR)requestSnapshot.hotkeyObject,
                &beforeView) ||
            oldNextObject != beforeView.NextHotkey ||
            !KswHotkeyViewMatchesRequest(&beforeView, &requestSnapshot)) {
            if (NT_SUCCESS(status)) {
                status = STATUS_REVISION_MISMATCH;
            }
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT;
            __leave;
        }
        if (!KswHotkeyViewIsOrdinaryStandalone(&runtime.Layout,
                (ULONG_PTR)requestSnapshot.hotkeyObject,
                &beforeView)) {
            status = STATUS_NOT_SUPPORTED;
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_UNSAFE_TARGET;
            __leave;
        }
        snapshotEstablished = TRUE; // 此后才可能进入系统清除helper或编辑副作用。
        outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_SNAPSHOT_VALIDATED;
        outputBuffer->previousSnapshotHash = beforeView.SnapshotHash;
        outputBuffer->currentSnapshotHash = beforeView.SnapshotHash;

        if (beforeView.WindowHandle != 0U) {
            windowObject = KswHotkeyValidateWindowObject(&runtime, beforeView.WindowHandle); // 不将tagWND指针直接传作HWND。
            if (windowObject == NULL) {
                status = STATUS_INVALID_HANDLE;
                outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT;
                __leave;
            }
            if (runtime.Layout.WindowRepresentation == 0UL && ((ULONG_PTR)windowObject > MAXULONG_PTR - runtime.Layout.WindowObjectHandleOffset - sizeof(validatedWindowHandle) ||
                !KswordARKHookReadMemorySafe((const UCHAR*)windowObject + runtime.Layout.WindowObjectHandleOffset,
                    &validatedWindowHandle, sizeof(validatedWindowHandle)) || validatedWindowHandle != beforeView.WindowHandle)) {
                status = STATUS_INVALID_HANDLE; outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_STALE_SNAPSHOT; __leave; // 规则证明的tagWND字段必须与原HWND一致。
            }
        }

        status = KswHotkeyCountOwnerWindowIdMatches(&runtime.Layout, tableAddress, &beforeView, &matchCount); // 删除和编辑都必须先完整确认全桶唯一性。
        if (!NT_SUCCESS(status) || matchCount != 1UL) {
            if (NT_SUCCESS(status)) {
                status = STATUS_OBJECT_NAME_COLLISION; // 重复节点或同归属对象不能被重挂成自环，也不能只删除其中一处。
            }
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_UNSAFE_TARGET;
            __leave; // 任一桶链读不全同样拒绝进入两条副作用路径。
        }

        if (requestSnapshot.operation == KSWORD_ARK_KEYBOARD_MUTATION_OPERATION_DELETE) {
            BOOLEAN removed = FALSE;
            NTSTATUS verifyStatus = STATUS_SUCCESS;

            deletionAttempted = TRUE; outputBuffer->currentSnapshotHash = 0ULL; // 调用开始后直到完整读回都属于未确认状态。
            removed = runtime.RemoveSlot((PVOID*)oldLinkAddress); // 槽位由当前表中精确节点定位，不接受用户提供的任意槽。
            verifyStatus = KswHotkeyFindObjectLink(&runtime.Layout,
                tableAddress, requestSnapshot.expectedBucketIndex, (ULONG_PTR)requestSnapshot.hotkeyObject,
                &oldLinkAddress, &oldNextObject);
            if (verifyStatus == STATUS_NOT_FOUND) {
                operationChanged = TRUE; // 无论AL回执如何，实际消失都是已观察到的变化。
                outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED;
                outputBuffer->currentModifiers = 0UL; outputBuffer->currentVirtualKey = 0UL; outputBuffer->currentSnapshotHash = 0ULL;
                *(volatile ULONG_PTR*)runtime.Layout.CacheAddress = 0U; // 不能留下指向已释放节点的派生缓存。
                KeMemoryBarrier();
                status = removed ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
                outputBuffer->status = removed ? KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OK : KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OPERATION_FAILED;
                __leave; // AL0且对象消失时明确失败且带CHANGED，不宣称无变化。
            }
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OPERATION_FAILED;
            outputBuffer->currentSnapshotHash = 0ULL; // 无完整读回时当前状态为未确认。
            status = NT_SUCCESS(verifyStatus) ? STATUS_UNSUCCESSFUL : verifyStatus;
            if (NT_SUCCESS(verifyStatus) && KswHotkeyReadView(&runtime.Layout, (ULONG_PTR)requestSnapshot.hotkeyObject, &afterView)) {
                outputBuffer->currentModifiers = afterView.Modifiers; outputBuffer->currentVirtualKey = afterView.VirtualKey;
                outputBuffer->currentSnapshotHash = afterView.SnapshotHash;
                if (afterView.SnapshotHash != beforeView.SnapshotHash) {
                    operationChanged = TRUE; outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED;
                }
            }
            __leave; // 不根据失败的布尔回执推断操作没有副作用。
        }

        newBucketIndex = requestSnapshot.newVirtualKey & (runtime.Layout.BucketCount - 1UL);
        status = KswHotkeyFindConflict(&runtime.Layout,
            tableAddress,
            (ULONG_PTR)requestSnapshot.hotkeyObject,
            requestSnapshot.newModifiers,
            requestSnapshot.newVirtualKey);
        if (!NT_SUCCESS(status)) {
            outputBuffer->status = status == STATUS_OBJECT_NAME_COLLISION ?
                KSWORD_ARK_KEYBOARD_MUTATION_STATUS_CONFLICT :
                KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OPERATION_FAILED;
            __leave;
        }
        if (beforeView.Modifiers == requestSnapshot.newModifiers &&
            beforeView.VirtualKey == requestSnapshot.newVirtualKey) {
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OK;
            outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_OTHER_BYTES_SAME;
            status = STATUS_SUCCESS;
            __leave;
        }

        RtlZeroMemory(&afterView, sizeof(afterView));
        editAttempted = TRUE; outputBuffer->currentSnapshotHash = 0ULL; // 写入前先标记当前状态未确认，异常也不能沿用旧hash。
        status = KswHotkeyWriteEdit(&runtime.Layout,
            tableAddress,
            oldLinkAddress,
            requestSnapshot.expectedBucketIndex,
            newBucketIndex,
            (ULONG_PTR)requestSnapshot.hotkeyObject,
            &beforeView,
            requestSnapshot.newModifiers,
            requestSnapshot.newVirtualKey,
            &afterView,
            &rolledBack);
        if (!NT_SUCCESS(status)) {
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OPERATION_FAILED;
            if (rolledBack) {
                outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_ROLLED_BACK;
                outputBuffer->currentSnapshotHash = beforeView.SnapshotHash; // helper已经完整验证原对象与原挂链恢复。
            }
            __leave;
        }
        *(volatile ULONG_PTR*)(runtime.Layout.CacheAddress) = 0U; // 同一规则证明的缓存位。
        KeMemoryBarrier();
        outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OK;
        outputBuffer->responseFlags |=
            KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED |
            KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_OTHER_BYTES_SAME;
        if (newBucketIndex != requestSnapshot.expectedBucketIndex) {
            outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_REBUCKETED;
        }
        outputBuffer->currentBucketIndex = newBucketIndex;
        outputBuffer->currentModifiers = afterView.Modifiers;
        outputBuffer->currentVirtualKey = afterView.VirtualKey;
        outputBuffer->currentSnapshotHash = afterView.SnapshotHash;
        operationChanged = TRUE;
        status = STATUS_SUCCESS;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OPERATION_FAILED;
    }

    if (critEntered && snapshotEstablished && deletionAttempted && !operationChanged && outputBuffer->currentSnapshotHash == 0ULL) {
        __try {
            NTSTATUS verifyStatus = KswHotkeyFindObjectLink(&runtime.Layout, tableAddress,
                requestSnapshot.expectedBucketIndex, (ULONG_PTR)requestSnapshot.hotkeyObject, &oldLinkAddress, &oldNextObject);
            if (verifyStatus == STATUS_NOT_FOUND) {
                operationChanged = TRUE; outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED;
                outputBuffer->currentModifiers = 0UL; outputBuffer->currentVirtualKey = 0UL;
                *(volatile ULONG_PTR*)runtime.Layout.CacheAddress = 0U; KeMemoryBarrier(); // 异常后实际已消失也必须清理派生缓存。
            } else if (NT_SUCCESS(verifyStatus) && KswHotkeyReadView(&runtime.Layout, (ULONG_PTR)requestSnapshot.hotkeyObject, &afterView)) {
                outputBuffer->currentSnapshotHash = afterView.SnapshotHash;
                outputBuffer->currentModifiers = afterView.Modifiers; outputBuffer->currentVirtualKey = afterView.VirtualKey;
                if (afterView.SnapshotHash != beforeView.SnapshotHash) { operationChanged = TRUE; outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED; }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { outputBuffer->currentSnapshotHash = 0ULL; status = GetExceptionCode(); }
    }
    if (critEntered && snapshotEstablished && editAttempted && !NT_SUCCESS(status) && !rolledBack) {
        __try {
            ULONG actualBucketIndex = MAXULONG; // 必须由所有真实桶链确认，不能只检查预期目标桶。
            NTSTATUS verifyStatus = KswHotkeyFindObjectBucket(&runtime.Layout, tableAddress,
                (ULONG_PTR)requestSnapshot.hotkeyObject, &actualBucketIndex);
            outputBuffer->currentSnapshotHash = 0ULL; // 任一链或对象读不全都保持未确认。
            if (verifyStatus == STATUS_NOT_FOUND) {
                operationChanged = TRUE; outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED;
                outputBuffer->currentModifiers = 0UL; outputBuffer->currentVirtualKey = 0UL; // 完整表中已经没有原节点。
            } else if (NT_SUCCESS(verifyStatus) && KswHotkeyReadView(&runtime.Layout,
                    (ULONG_PTR)requestSnapshot.hotkeyObject, &afterView)) {
                outputBuffer->currentSnapshotHash = afterView.SnapshotHash;
                outputBuffer->currentBucketIndex = actualBucketIndex;
                outputBuffer->currentModifiers = afterView.Modifiers; outputBuffer->currentVirtualKey = afterView.VirtualKey;
                if (afterView.SnapshotHash != beforeView.SnapshotHash || actualBucketIndex != requestSnapshot.expectedBucketIndex) {
                    operationChanged = TRUE; outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_CHANGED; // 字节变化或实际移动均为已观察变化。
                    if (actualBucketIndex != requestSnapshot.expectedBucketIndex) outputBuffer->responseFlags |= KSWORD_ARK_KEYBOARD_MUTATION_RESPONSE_REBUCKETED;
                }
            }
            if (operationChanged) { *(volatile ULONG_PTR*)runtime.Layout.CacheAddress = 0U; KeMemoryBarrier(); } // 不保留指向旧键值或旧桶的派生缓存。
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { outputBuffer->currentSnapshotHash = 0ULL; status = GetExceptionCode(); }
    }
    if (critEntered) {
        __try {
            runtime.LeaveCrit();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            outputBuffer->status = KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OPERATION_FAILED;
        }
    }
    if (!operationChanged && outputBuffer->status == KSWORD_ARK_KEYBOARD_MUTATION_STATUS_OK) {
        outputBuffer->currentBucketIndex = requestSnapshot.expectedBucketIndex;
    }
    outputBuffer->lastStatus = status;
    return STATUS_SUCCESS;
}
