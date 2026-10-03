/*++
    文件删除的基础文件系统重试与映像 section 清理。
    只借用已打开对象；关闭、路径复核和只读属性重开由 file_actions.c 管理。
--*/
#include <ntifs.h>
#include "ark/ark_file_irp.h"

BOOLEAN
KswordARKDriverShouldRetryDeleteByIrp(_In_ NTSTATUS Status)
{
    // 只重试删除阻碍和旧文件系统不支持 Ex 的状态，保留资源、路径等原始错误。
    switch (Status) {
    case STATUS_ACCESS_DENIED:
    case STATUS_SHARING_VIOLATION:
    case STATUS_CANNOT_DELETE:
    case STATUS_USER_MAPPED_FILE:
    case STATUS_INVALID_INFO_CLASS:
    case STATUS_NOT_SUPPORTED:
    case STATUS_INVALID_PARAMETER:
        return TRUE; // 基础层重试可能避开过滤层拒绝或提供传统类兼容。
    default:
        return FALSE; // 其它失败不重复投递删除。
    }
}

NTSTATUS
KswordARKDriverFlushFileObjectForDelete(_In_ PFILE_OBJECT FileObject)
{
    if (FileObject == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) { // 删除入口只允许 PASSIVE_LEVEL 的已引用对象。
        return STATUS_INVALID_DEVICE_STATE; // 不在错误 IRQL 访问文件系统状态。
    }
    if (FileObject->SectionObjectPointer == NULL) { // 无 section 结构时无需清理。
        return STATUS_SUCCESS; // 文件系统仍负责最终删除检查。
    }
    // 常规删除仅使用 MM 清理；显式强制后端的直接清空由另一个入口执行。
    // 即使 ImageSectionObject 为空，也让 MM 检查数据 section 中尚未完成的写探针。
    return MmFlushImageSection(FileObject->SectionObjectPointer, MmFlushForDelete)
        ? STATUS_SUCCESS : STATUS_CANNOT_DELETE; // 活动映像/写探针存在时保留失败。
}

static NTSTATUS
KswordArkDeleteObjectByIrp(
    _In_ PFILE_OBJECT FileObject,
    _In_ BOOLEAN IgnoreReadOnly,
    _Out_ PBOOLEAN CancelledOut)
{
    // FileDispositionInformationEx 的 ABI 是一个 ULONG；64 是公开的信息类编号。
    const ULONG dispositionFlags = 0x00000001UL /* DELETE */ |
        0x00000002UL /* POSIX_SEMANTICS */ |
        (IgnoreReadOnly ? 0x00000010UL /* IGNORE_READONLY_ATTRIBUTE */ : 0UL);
    FILE_DISPOSITION_INFORMATION legacyDisposition; // 仅用于不支持 Ex 的旧文件系统。
    NTSTATUS status; // 保存实际删除 IRP 的状态。

    status = KswordARKDriverSetDispositionByIrp(FileObject, 64UL,
        &dispositionFlags, (ULONG)sizeof(dispositionFlags), CancelledOut); // 同一对象上优先 POSIX unlink。
    if (*CancelledOut || (status != STATUS_INVALID_INFO_CLASS &&
        status != STATUS_NOT_SUPPORTED && status != STATUS_INVALID_PARAMETER)) { // 真实失败不掩盖为兼容成功。
        return status; // 已超时的请求不得自动重复。
    }
    RtlZeroMemory(&legacyDisposition, sizeof(legacyDisposition)); // 不传入未初始化的 ABI 填充。
    legacyDisposition.DeleteFile = TRUE; // 传统类仅请求删除。
    return KswordARKDriverSetDispositionByIrp(FileObject, (ULONG)FileDispositionInformation,
        &legacyDisposition, (ULONG)sizeof(legacyDisposition), CancelledOut); // 兼容删除仍直达基础文件系统。
}

NTSTATUS
KswordARKDriverClearFileObjectSectionsForDelete(_In_ PFILE_OBJECT FileObject)
{
    PSECTION_OBJECT_POINTERS sectionPointers; // 指向文件系统为该流共享的 section 状态。

    if (FileObject == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) { // 强制写入也必须持有有效对象并处于被动级。
        return STATUS_INVALID_DEVICE_STATE; // 非法调用不写内核状态。
    }
    sectionPointers = FileObject->SectionObjectPointer; // 保留结构地址，避免破坏 FILE_OBJECT 的结构关联。
    if (sectionPointers != NULL) { // 无 section 结构时不存在需要清空的成员。
        // 显式强制删除语义：此结构与其它打开实例共享；不释放这些不透明对象，也不恢复旧指针。
        sectionPointers->ImageSectionObject = NULL; // 清空映像 section 的删除阻碍。
        sectionPointers->DataSectionObject = NULL; // 清空数据映射 section 的删除阻碍。
        sectionPointers->SharedCacheMap = NULL; // 清空共享缓存关联。
    }
    return STATUS_SUCCESS; // 仍需文件系统接受后续删除 IRP。
}

NTSTATUS
KswordARKDriverDeleteFileObjectByIrp(
    _In_ PFILE_OBJECT FileObject,
    _In_ BOOLEAN IgnoreReadOnly,
    _Out_ PBOOLEAN CancelledOut)
{
    NTSTATUS status; // 强制删除阶段最终结果。

    if (CancelledOut == NULL) { // 所有调用都必须接收超时取消状态。
        return STATUS_INVALID_PARAMETER; // 缺少输出地址时不投递请求。
    }
    *CancelledOut = FALSE; // 首次调用前初始化取消状态。
    if (FileObject == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) { // 直接对象入口也校验生命周期前置条件。
        return STATUS_INVALID_DEVICE_STATE; // 无有效对象或错误 IRQL 时不执行删除。
    }
    status = KswordArkDeleteObjectByIrp(FileObject, IgnoreReadOnly, CancelledOut); // 先尝试正常的基础层 POSIX/传统类删除。
    if (!*CancelledOut && (status == STATUS_ACCESS_DENIED || status == STATUS_SHARING_VIOLATION ||
        status == STATUS_CANNOT_DELETE || status == STATUS_USER_MAPPED_FILE)) { // 只有删除受阻时才修改共享 section。
        (VOID)KswordARKDriverFlushFileObjectForDelete(FileObject); // 先让 MM 释放能够正常清理的映像资源。
        if (NT_SUCCESS(KswordARKDriverClearFileObjectSectionsForDelete(FileObject))) { // 按强制模式要求直接清空三个成员。
            status = KswordArkDeleteObjectByIrp(FileObject, IgnoreReadOnly, CancelledOut); // 复用对象至多再执行一轮。
        }
    }
    return status; // 不把清空成功当作文件删除成功。
}

NTSTATUS
KswordARKDriverDeleteHandleByIrp(
    _In_ HANDLE FileHandle,
    _In_ BOOLEAN IgnoreReadOnly)
{
    PFILE_OBJECT fileObject = NULL; // 持有原句柄对象，禁止通过路径重新解析。
    BOOLEAN cancelled = FALSE; // 超时取消后排空但不重试。
    NTSTATUS status; // 最终删除阶段状态。

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { // 引用句柄和文件系统请求都要求被动级。
        return STATUS_INVALID_DEVICE_STATE; // 非法调用不进入文件系统。
    }
    status = ObReferenceObjectByHandle(FileHandle, DELETE, *IoFileObjectType,
        KernelMode, (PVOID*)&fileObject, NULL); // 额外引用覆盖全部同步 IRP 的生命周期。
    if (!NT_SUCCESS(status)) { // 引用失败时没有对象需要释放。
        return status; // 回传句柄错误。
    }
    status = KswordARKDriverDeleteFileObjectByIrp(fileObject, IgnoreReadOnly, &cancelled); // 高风险后端复用同一对象完成强制删除。
    ObDereferenceObject(fileObject); // 借用结束，原句柄仍由上层关闭并复核路径。
    return status; // 成功仅表示标记删除，不能替代关闭后复核。
}
