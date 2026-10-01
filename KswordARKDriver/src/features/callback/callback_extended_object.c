/*++

Module Name:

    callback_extended_object.c

Abstract:

    枚举命名 CallbackObject 注册项与 Image Verification 回调对象。

Environment:

    Kernel-mode Driver Framework

--*/

#include "callback_extended_internal.h"
#include "callback_extended_kernel.h"

#define KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_BYTES (16UL * 1024UL)
#define KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_LIMIT 4096UL
#define KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION_LIMIT 512UL
#define KSWORD_ARK_CALLBACK_OBJECT_CODE_SCAN_BYTES 0x80UL
#define KSWORD_ARK_CALLBACK_OBJECT_TAG 'oCbK'
#define KSWORD_ARK_CALLBACK_OBJECT_SIGNATURE 0x6C6C6143UL

#ifndef DIRECTORY_QUERY
#define DIRECTORY_QUERY 0x0001
#endif

#ifndef STATUS_NO_MORE_ENTRIES
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#endif

typedef struct _KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_INFORMATION
{
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_INFORMATION;

typedef struct _KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION
{
    LIST_ENTRY Link;
    PVOID CallbackObject;
    PVOID CallbackFunction;
    PVOID CallbackContext;
} KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION;

typedef struct _KSWORD_ARK_CALLBACK_OBJECT_SNAPSHOT
{
    ULONG TraversalIndex;
    ULONG64 RegistrationAddress;
    ULONG64 CallbackFunction;
    ULONG64 CallbackContext;
    ULONG64 Next; // 保存链路用于第二遍一致性验证。
    ULONG64 Previous; // 保存 reciprocal backward link。
} KSWORD_ARK_CALLBACK_OBJECT_SNAPSHOT;

NTSYSAPI
NTSTATUS
NTAPI
ZwOpenDirectoryObject(
    _Out_ PHANDLE DirectoryHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ POBJECT_ATTRIBUTES ObjectAttributes
    );

NTSYSAPI
NTSTATUS
NTAPI
ZwQueryDirectoryObject(
    _In_ HANDLE DirectoryHandle,
    _Out_writes_bytes_opt_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN ReturnSingleEntry,
    _In_ BOOLEAN RestartScan,
    _Inout_ PULONG Context,
    _Out_opt_ PULONG ReturnLength
    );

static NTSTATUS
KswordArkCallbackExtendedOpenCallbackDirectory(
    _Out_ HANDLE* DirectoryHandleOut
    )
/*++

Routine Description:

    打开 \Callback 对象目录。

Arguments:

    DirectoryHandleOut - 输出内核句柄。

Return Value:

    返回 ZwOpenDirectoryObject 的 NTSTATUS。

--*/
{
    UNICODE_STRING directoryName;
    OBJECT_ATTRIBUTES objectAttributes;

    if (DirectoryHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *DirectoryHandleOut = NULL;
    RtlInitUnicodeString(&directoryName, L"\\Callback");
    InitializeObjectAttributes(
        &objectAttributes,
        &directoryName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);
    return ZwOpenDirectoryObject(
        DirectoryHandleOut,
        DIRECTORY_QUERY,
        &objectAttributes);
}

static BOOLEAN
KswordArkCallbackExtendedBuildCallbackObjectName(
    _In_ PCUNICODE_STRING LeafName,
    _Out_writes_(DestinationChars) PWCHAR Destination,
    _In_ ULONG DestinationChars
    )
/*++

Routine Description:

    拼接 \Callback\叶名称。

Arguments:

    LeafName - 对象目录返回的叶名称。
    Destination - 输出完整 NT 对象路径。
    DestinationChars - 输出缓冲字符数。

Return Value:

    成功返回 TRUE；参数或字符串越界返回 FALSE。

--*/
{
    NTSTATUS status = STATUS_SUCCESS;

    if (LeafName == NULL ||
        LeafName->Buffer == NULL ||
        LeafName->Length == 0U ||
        Destination == NULL ||
        DestinationChars == 0UL) {
        return FALSE;
    }

    Destination[0] = L'\0';
    status = RtlStringCchCopyW(
        Destination,
        DestinationChars,
        L"\\Callback\\");
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    status = RtlStringCchCatNW(
        Destination,
        DestinationChars,
        LeafName->Buffer,
        LeafName->Length / sizeof(WCHAR));
    return NT_SUCCESS(status);
}

static ULONG
KswordArkCallbackExtendedEnumerateCallbackObject(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache,
    _In_ ULONG64 CallbackObjectAddress,
    _In_ ULONG CallbackClass,
    _In_ ULONG Source,
    _In_ ULONG RegistrationType,
    _In_z_ PCWSTR ObjectName
    )
/*++

Routine Description:

    遍历一个 CallbackObject 内部注册链。中文说明：当前结构由 ExRegisterCallback
    公开入口的对象布局恢复；持有对象自旋锁复制标量快照，避免并发注销释放节点，
    并在完成安全读取后完成模块判断、字符串格式化与响应构建。

Arguments:

    Builder - 响应构建器。
    ModuleCache - 模块缓存。
    CallbackObjectAddress - CallbackObject 对象体地址。
    CallbackClass - 通用或 Image Verification 类别。
    Source - 枚举来源。
    RegistrationType - 注册子类型。
    ObjectName - 展示名称。

Return Value:

    返回成功验证的注册节点数量。

--*/
{
    ULONG index = 0UL;
    ULONG addedCount = 0UL;
    ULONG snapshotCount = 0UL;
    ULONG snapshotIndex = 0UL;
    ULONG objectSignature = 0UL;
    ULONG64 listHeadAddress = 0ULL;
    ULONG64 currentAddress = 0ULL;
    LIST_ENTRY listHead;
    ULONG64 previousAddress = 0ULL; // PASSIVE_LEVEL 遍历检查反向链路。
    KSWORD_ARK_CALLBACK_OBJECT_SNAPSHOT* snapshots = NULL;

    // 验证调用方提供的响应构建器、模块缓存、对象地址与展示名称。
    if (Builder == NULL ||
        ModuleCache == NULL ||
        CallbackObjectAddress == 0ULL ||
        ObjectName == NULL) {
        KswordArkCallbackRecordRemoveQueryFailure(Builder, CallbackClass, STATUS_DATA_ERROR); // 目标读取失败不能证明已消失。
        return 0UL;
    }
    // 验证对象签名，避免对并非 CallbackObject 的地址获取私有锁。
    if (!KswordArkCallbackEnumReadMemory(
            (const VOID*)(ULONG_PTR)CallbackObjectAddress,
            &objectSignature,
            sizeof(objectSignature)) ||
        objectSignature != KSWORD_ARK_CALLBACK_OBJECT_SIGNATURE) {
        KswordArkCallbackRecordRemoveQueryFailure(Builder, CallbackClass, STATUS_DATA_ERROR); // 目标读取失败不能证明已消失。
        return 0UL;
    }

    // 计算注册链表头地址；x64 CALLBACK_OBJECT 的锁位于 +0x08，链表位于 +0x10。
    listHeadAddress = CallbackObjectAddress + (2ULL * sizeof(ULONG_PTR));
    // 在进入自旋读取前分配非分页快照，快照读取阶段不进行任何内存分配或字符串处理。
    snapshots = (KSWORD_ARK_CALLBACK_OBJECT_SNAPSHOT*)KswordArkAllocateNonPaged(
        sizeof(*snapshots) * KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION_LIMIT,
        KSWORD_ARK_CALLBACK_OBJECT_TAG);
    // 内存不足时不尝试复制节点，保留查询失败状态。
    if (snapshots == NULL) {
        KswordArkCallbackRecordRemoveQueryFailure(Builder, CallbackClass, STATUS_DATA_ERROR); // 目标读取失败不能证明已消失。
        return 0UL;
    }
    // 清零快照数组，确保任何提前结束路径都不会暴露未初始化字段。
    RtlZeroMemory(
        snapshots,
        sizeof(*snapshots) * KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION_LIMIT);

    // 保持 PASSIVE_LEVEL；统一安全读取器拒绝 DISPATCH_LEVEL，不能在私有自旋锁内读取。
    // 读取首次链表头，完成节点快照后再次验证链头与链路。
    RtlZeroMemory(&listHead, sizeof(listHead));
    if (!KswordArkCallbackExtendedReadListEntry(listHeadAddress, &listHead)) {
        // 快照来自本函数的非分页分配，失败路径必须对称释放。
        ExFreePoolWithTag(snapshots, KSWORD_ARK_CALLBACK_OBJECT_TAG);
        KswordArkCallbackRecordRemoveQueryFailure(Builder, CallbackClass, STATUS_DATA_ERROR); // 目标读取失败不能证明已消失。
        return 0UL;
    }

    // 从安全读取时的首节点开始遍历，最多复制固定数量的标量快照。
    currentAddress = (ULONG64)(ULONG_PTR)listHead.Flink;
    previousAddress = listHeadAddress; // 第一个节点的 Blink 必须回指链头。
    while (currentAddress != 0ULL &&
        currentAddress != listHeadAddress &&
        index < KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION_LIMIT) {
        KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION registration;
        ULONG64 nextAddress = 0ULL;

        // 每个节点读取前清零本地结构，避免异常读取留下栈残值。
        RtlZeroMemory(&registration, sizeof(registration));
        // 在对象安全读取时复制注册节点，随后再次读取验证是否变化。
        if (!KswordArkCallbackEnumReadMemory(
                (const VOID*)(ULONG_PTR)currentAddress,
                &registration,
                sizeof(registration)) || (ULONG64)(ULONG_PTR)registration.Link.Blink != previousAddress) { // 前后链路必须一致。
            KswordArkCallbackRecordRemoveQueryFailure(Builder, CallbackClass, STATUS_DATA_ERROR); // 目标读取失败不能证明已消失。
            break;
        }
        // 保存下一节点地址，后续只从已读取副本中使用。
        nextAddress = (ULONG64)(ULONG_PTR)registration.Link.Flink;

        // 仅接受属于当前对象且具有非空回调函数的注册节点。
        if ((ULONG64)(ULONG_PTR)registration.CallbackObject == CallbackObjectAddress &&
            registration.CallbackFunction != NULL) {
            // 快照只保存快照读取后构建响应所需的标量，不保留可失效的节点指针。
            snapshots[snapshotCount].TraversalIndex = index;
            snapshots[snapshotCount].RegistrationAddress = currentAddress;
            snapshots[snapshotCount].CallbackFunction =
                (ULONG64)(ULONG_PTR)registration.CallbackFunction;
            snapshots[snapshotCount].CallbackContext =
                (ULONG64)(ULONG_PTR)registration.CallbackContext;
            snapshots[snapshotCount].Next = nextAddress; // 二次读取需确认链路不变。
            snapshots[snapshotCount].Previous = previousAddress; // 保存原始反向链路。
            // 记录已填充快照数量，容量与遍历上限完全一致。
            ++snapshotCount;
        }

        // 自环表示链表损坏；不能将其当作正常结束。
        if (nextAddress == currentAddress) {
            KswordArkCallbackRecordRemoveQueryFailure(Builder, CallbackClass, STATUS_DATA_ERROR); // 目标读取失败不能证明已消失。
            break;
        }
        // 推进到下一节点并记录原始遍历序号。
        previousAddress = currentAddress; currentAddress = nextAddress; // 双向前进。
        ++index;
    }

    { // 在同一 PASSIVE_LEVEL 安全读取路径中二次确认容器和每个节点。
        LIST_ENTRY verifiedHead; // 独立读取链头，不能拿旧值作为新证据。
        BOOLEAN stable = currentAddress == listHeadAddress && previousAddress == (ULONG64)(ULONG_PTR)listHead.Blink &&
            KswordArkCallbackExtendedReadListEntry(listHeadAddress, &verifiedHead) &&
            verifiedHead.Flink == listHead.Flink && verifiedHead.Blink == listHead.Blink; // 空链同样需要完整确认。
        for (snapshotIndex = 0UL; stable && snapshotIndex < snapshotCount; ++snapshotIndex) { // 有界第二遍节点验证。
            KSWORD_ARK_CALLBACK_OBJECT_REGISTRATION verified; // 当前前缀副本。
            const KSWORD_ARK_CALLBACK_OBJECT_SNAPSHOT* saved = &snapshots[snapshotIndex]; // 受控第一次快照。
            stable = KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)saved->RegistrationAddress, &verified, sizeof(verified)) &&
                (ULONG64)(ULONG_PTR)verified.CallbackObject == CallbackObjectAddress &&
                (ULONG64)(ULONG_PTR)verified.CallbackFunction == saved->CallbackFunction &&
                (ULONG64)(ULONG_PTR)verified.CallbackContext == saved->CallbackContext &&
                (ULONG64)(ULONG_PTR)verified.Link.Flink == saved->Next &&
                (ULONG64)(ULONG_PTR)verified.Link.Blink == saved->Previous; // 任何变化只返回查询失败。
        } // 结束一致性检查。
        if (!stable) { // 不发布可能已经失效的注销句柄。
            snapshotCount = 0UL; // 清空部分结果。
            KswordArkCallbackExtendedAddRow(Builder, ModuleCache, CallbackClass, Source,
                KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED, STATUS_RETRY, RegistrationType,
                0UL, 0UL, 0ULL, CallbackObjectAddress, listHeadAddress, 0UL, ObjectName,
                L"CallbackObject 链在两次安全读取间发生变化或未能完整读取，请刷新后重试。"); // 提供具体失败原因。
        } // 结束变化诊断。
    } // 结束两次快照确认。

    // 快照读取后逐条验证模块归属并构建响应，避免在 DISPATCH_LEVEL 执行复杂逻辑。
    for (snapshotIndex = 0UL; snapshotIndex < snapshotCount; ++snapshotIndex) {
        WCHAR nameText[KSWORD_ARK_CALLBACK_ENUM_NAME_CHARS];
        WCHAR detailText[KSWORD_ARK_CALLBACK_ENUM_DETAIL_CHARS];
        const KSWORD_ARK_CALLBACK_OBJECT_SNAPSHOT* snapshot = &snapshots[snapshotIndex];

        // 非模块地址不进入结果集，但不会影响其余稳定快照。
        if (!KswordArkCallbackEnumIsKernelModuleAddress(
                ModuleCache,
                snapshot->CallbackFunction)) {
            continue;
        }

        // 预清零展示缓冲，保证格式化失败时仍保持确定内容。
        RtlZeroMemory(nameText, sizeof(nameText));
        RtlZeroMemory(detailText, sizeof(detailText));
        // 使用快照读取阶段记录的遍历序号维持现有名称语义。
        (VOID)RtlStringCbPrintfW(
            nameText,
            sizeof(nameText),
            L"%ws[%lu]",
            ObjectName,
            (unsigned long)snapshot->TraversalIndex);
        // 详情只引用标量地址，不会解引用已经注销的注册节点。
        (VOID)RtlStringCbPrintfW(
            detailText,
            sizeof(detailText),
            L"CallbackObject 注册项；Object=0x%p，Registration=0x%p，Function=0x%p，Context=0x%p。",
            (PVOID)(ULONG_PTR)CallbackObjectAddress,
            (PVOID)(ULONG_PTR)snapshot->RegistrationAddress,
            (PVOID)(ULONG_PTR)snapshot->CallbackFunction,
            (PVOID)(ULONG_PTR)snapshot->CallbackContext);
        // 将已验证快照写入统一回调枚举响应。
        KswordArkCallbackExtendedAddRow(
            Builder,
            ModuleCache,
            CallbackClass,
            Source,
            KSWORD_ARK_CALLBACK_ENUM_STATUS_OK,
            STATUS_SUCCESS,
            RegistrationType,
            0UL,
            0UL,
            snapshot->CallbackFunction,
            snapshot->CallbackContext,
            snapshot->RegistrationAddress,
            0UL,
            nameText,
            detailText);
        // 统计成功加入结果集的注册项数量。
        ++addedCount;
    }

    // 释放本函数分配的非分页快照数组。
    ExFreePoolWithTag(snapshots, KSWORD_ARK_CALLBACK_OBJECT_TAG);
    return addedCount;
}

static VOID
KswordArkCallbackExtendedAddNamedCallbackObjects(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache
    )
/*++

Routine Description:

    枚举 \Callback 目录中的命名 CallbackObject 及其注册函数。

Arguments:

    Builder - 响应构建器。
    ModuleCache - 模块缓存。

Return Value:

    无返回值。

--*/
{
    HANDLE directoryHandle = NULL;
    KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_INFORMATION* entry = NULL;
    ULONG queryContext = 0UL;
    ULONG returnLength = 0UL;
    ULONG scannedEntries = 0UL;
    BOOLEAN restartScan = TRUE;
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING callbackTypeName;

    status = KswordArkCallbackExtendedOpenCallbackDirectory(&directoryHandle);
    if (!NT_SUCCESS(status)) {
        KswordArkCallbackExtendedAddRow(
            Builder,
            ModuleCache,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_GENERIC_KERNEL,
            KSWORD_ARK_CALLBACK_ENUM_SOURCE_OBJECT_DIRECTORY,
            KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED,
            status,
            KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_GENERIC_CALLBACK_OBJECT,
            0UL,
            0UL,
            0ULL,
            0ULL,
            0ULL,
            0UL,
            L"\\Callback directory",
            L"打开命名 CallbackObject 目录失败。");
        return;
    }

    entry = (KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_INFORMATION*)KswordArkAllocateNonPaged(
        KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_BYTES,
        KSWORD_ARK_CALLBACK_OBJECT_TAG);
    if (entry == NULL) {
        ZwClose(directoryHandle);
        return;
    }

    RtlInitUnicodeString(&callbackTypeName, L"Callback");
    while (scannedEntries < KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_LIMIT) {
        WCHAR objectName[KSWORD_ARK_CALLBACK_ENUM_NAME_CHARS];
        UNICODE_STRING fullObjectName;
        OBJECT_ATTRIBUTES callbackObjectAttributes;
        PCALLBACK_OBJECT callbackObject = NULL;

        RtlZeroMemory(entry, KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_BYTES);
        status = ZwQueryDirectoryObject(
            directoryHandle,
            entry,
            KSWORD_ARK_CALLBACK_OBJECT_DIRECTORY_BYTES,
            TRUE,
            restartScan,
            &queryContext,
            &returnLength);
        restartScan = FALSE;
        if (status == STATUS_NO_MORE_ENTRIES) {
            break;
        }
        if (!NT_SUCCESS(status)) {
            break;
        }

        ++scannedEntries;
        if (!RtlEqualUnicodeString(
                &entry->TypeName,
                &callbackTypeName,
                TRUE)) {
            continue;
        }

        RtlZeroMemory(objectName, sizeof(objectName));
        if (!KswordArkCallbackExtendedBuildCallbackObjectName(
                &entry->Name,
                objectName,
                RTL_NUMBER_OF(objectName))) {
            continue;
        }

        RtlInitUnicodeString(&fullObjectName, objectName);
        // 使用公开 CallbackObject DDI 打开现有对象；该入口会在内核内部传入真实对象类型。
        InitializeObjectAttributes(
            &callbackObjectAttributes,
            &fullObjectName,
            OBJ_CASE_INSENSITIVE,
            NULL,
            NULL);
        // Create=FALSE 保证枚举只读；AllowMultipleCallbacks 在仅打开现有对象时被系统忽略。
        status = ExCreateCallback(
            &callbackObject,
            &callbackObjectAttributes,
            FALSE,
            FALSE);
        if (!NT_SUCCESS(status) || callbackObject == NULL) {
            continue;
        }

        (VOID)KswordArkCallbackExtendedEnumerateCallbackObject(
            Builder,
            ModuleCache,
            (ULONG64)(ULONG_PTR)callbackObject,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_GENERIC_KERNEL,
            KSWORD_ARK_CALLBACK_ENUM_SOURCE_OBJECT_DIRECTORY,
            KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_GENERIC_CALLBACK_OBJECT,
            objectName);
        ObDereferenceObject(callbackObject);
    }

    ExFreePoolWithTag(entry, KSWORD_ARK_CALLBACK_OBJECT_TAG);
    ZwClose(directoryHandle);
}

static ULONG
KswordArkCallbackExtendedLocateImageCallbackGlobals(
    _Out_writes_(GlobalCapacity) ULONG64* GlobalAddresses,
    _In_ ULONG GlobalCapacity
    )
/*++

Routine Description:

    从 SeRegisterImageVerificationCallback 中提取两个 CallbackObject 全局指针。

Arguments:

    GlobalAddresses - 输出全局指针变量地址。
    GlobalCapacity - 输出槽容量。

Return Value:

    返回去重后的全局地址数量。

--*/
{
    UCHAR codeBytes[KSWORD_ARK_CALLBACK_OBJECT_CODE_SCAN_BYTES];
    ULONG offset = 0UL;
    ULONG foundCount = 0UL;
    ULONG64 routineAddress = (ULONG64)(ULONG_PTR)
        KswordArkCallbackExtendedGetSystemRoutine(L"SeRegisterImageVerificationCallback");

    if (GlobalAddresses == NULL || GlobalCapacity == 0UL || routineAddress == 0ULL) {
        return 0UL;
    }

    RtlZeroMemory(GlobalAddresses, sizeof(ULONG64) * GlobalCapacity);
    RtlZeroMemory(codeBytes, sizeof(codeBytes));
    if (!KswordArkCallbackEnumReadMemory(
            (const VOID*)(ULONG_PTR)routineAddress,
            codeBytes,
            sizeof(codeBytes))) {
        return 0UL;
    }

    for (offset = 0UL;
        offset + 7UL <= sizeof(codeBytes) && foundCount < GlobalCapacity;
        ++offset) {
        ULONG64 globalAddress = 0ULL;
        ULONG existingIndex = 0UL;
        BOOLEAN duplicate = FALSE;

        if (codeBytes[offset] != 0x48U ||
            codeBytes[offset + 1UL] != 0x8BU ||
            codeBytes[offset + 2UL] != 0x0DU) {
            continue;
        }
        if (!KswordArkCallbackExtendedResolveRipRelative(
                routineAddress + offset,
                3UL,
                7UL,
                &globalAddress)) {
            continue;
        }

        for (existingIndex = 0UL; existingIndex < foundCount; ++existingIndex) {
            if (GlobalAddresses[existingIndex] == globalAddress) {
                duplicate = TRUE;
                break;
            }
        }
        if (!duplicate) {
            GlobalAddresses[foundCount] = globalAddress;
            ++foundCount;
        }
    }

    return foundCount;
}

static VOID
KswordArkCallbackExtendedAddImageVerificationCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder,
    _Inout_ KSWORD_ARK_CALLBACK_MODULE_CACHE* ModuleCache
    )
/*++

Routine Description:

    枚举 Informational 与 Block 两个 Image Verification CallbackObject。

Arguments:

    Builder - 响应构建器。
    ModuleCache - 模块缓存。

Return Value:

    无返回值。

--*/
{
    ULONG index = 0UL;
    ULONG globalCount = 0UL;
    ULONG64 globalAddresses[2] = { 0ULL, 0ULL };

    globalCount = KswordArkCallbackExtendedLocateImageCallbackGlobals(
        globalAddresses,
        RTL_NUMBER_OF(globalAddresses));
    if (globalCount == 0UL) {
        KswordArkCallbackExtendedAddRow(
            Builder,
            ModuleCache,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE_VERIFICATION,
            KSWORD_ARK_CALLBACK_ENUM_SOURCE_CALLBACK_OBJECT,
            KSWORD_ARK_CALLBACK_ENUM_STATUS_QUERY_FAILED,
            STATUS_NOT_FOUND,
            KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_UNKNOWN,
            0UL,
            0UL,
            0ULL,
            0ULL,
            0ULL,
            0UL,
            L"SeImageVerification callback objects",
            L"未能从 SeRegisterImageVerificationCallback 公开入口恢复 CallbackObject 全局指针。");
        return;
    }

    for (index = 0UL; index < globalCount; ++index) {
        ULONG64 callbackObject = 0ULL;
        const ULONG registrationType = (index == 0UL)
            ? KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_IMAGE_VERIFY_INFORMATIONAL
            : KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_IMAGE_VERIFY_BLOCK;
        PCWSTR objectName = (index == 0UL)
            ? L"SeImageVerification/Informational"
            : L"SeImageVerification/Block";

        if (!KswordArkCallbackExtendedReadPointer(
                globalAddresses[index],
                &callbackObject) ||
            callbackObject == 0ULL) {
            continue;
        }

        (VOID)KswordArkCallbackExtendedEnumerateCallbackObject(
            Builder,
            ModuleCache,
            callbackObject,
            KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE_VERIFICATION,
            KSWORD_ARK_CALLBACK_ENUM_SOURCE_CALLBACK_OBJECT,
            registrationType,
            objectName);
    }
}

VOID
KswordArkCallbackExtendedAddObjectCallbacks(
    _Inout_ KSWORD_ARK_CALLBACK_ENUM_BUILDER* Builder
    )
/*++

Routine Description:

    聚合命名 CallbackObject 与 Image Verification 回调枚举。

Arguments:

    Builder - 响应构建器。

Return Value:

    无返回值。

--*/
{
    KSWORD_ARK_CALLBACK_MODULE_CACHE moduleCache;

    if (Builder == NULL) {
        return;
    }

    KswordArkCallbackEnumInitModuleCache(&moduleCache);
    const NTSTATUS moduleStatus = KswordArkCallbackEnumEnsureModuleCache(&moduleCache); // 保存真实查询状态。
    if (!NT_SUCCESS(moduleStatus)) {
        KswordArkCallbackRecordRemoveQueryFailure(Builder, KSWORD_ARK_CALLBACK_ENUM_CLASS_GENERIC_KERNEL, moduleStatus); // 相关模块查询失败不能证明缺失。
        KswordArkCallbackRecordRemoveQueryFailure(Builder, KSWORD_ARK_CALLBACK_ENUM_CLASS_IMAGE_VERIFICATION, moduleStatus); // 相关模块查询失败不能证明缺失。
        KswordArkCallbackEnumFreeModuleCache(&moduleCache);
        return;
    }

    KswordArkCallbackExtendedAddNamedCallbackObjects(Builder, &moduleCache);
    KswordArkCallbackExtendedAddImageVerificationCallbacks(Builder, &moduleCache);
    KswordArkCallbackEnumFreeModuleCache(&moduleCache);
}
