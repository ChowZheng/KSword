/*++

Module Name:

    file_actions.c

Abstract:

    This file contains kernel file operations, including deletion and
    read-only Phase-10 file information queries.

Environment:

    Kernel-mode Driver Framework

--*/

#include <ntifs.h>
#include "ark/ark_driver.h"
#include "ark/ark_file_irp.h"

#include <ntstrsafe.h>

#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000UL
#endif

#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000UL
#endif

#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001UL
#endif

#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040UL
#endif

#ifndef ACCESS_SYSTEM_SECURITY
#define ACCESS_SYSTEM_SECURITY 0x01000000L
#endif

#ifndef LABEL_SECURITY_INFORMATION
#define LABEL_SECURITY_INFORMATION 0x00000010L
#endif

#ifndef STATUS_INVALID_SID
#define STATUS_INVALID_SID ((NTSTATUS)0xC0000078L)
#endif

#ifndef STATUS_INVALID_LABEL
#define STATUS_INVALID_LABEL ((NTSTATUS)0xC0000446L)
#endif

#ifndef SECURITY_MANDATORY_UNTRUSTED_RID
#define SECURITY_MANDATORY_UNTRUSTED_RID 0x00000000UL
#endif

#ifndef SECURITY_MANDATORY_LOW_RID
#define SECURITY_MANDATORY_LOW_RID 0x00001000UL
#endif

#ifndef SECURITY_MANDATORY_MEDIUM_RID
#define SECURITY_MANDATORY_MEDIUM_RID 0x00002000UL
#endif

#ifndef SECURITY_MANDATORY_MEDIUM_PLUS_RID
#define SECURITY_MANDATORY_MEDIUM_PLUS_RID (SECURITY_MANDATORY_MEDIUM_RID + 0x100UL)
#endif

#ifndef SECURITY_MANDATORY_HIGH_RID
#define SECURITY_MANDATORY_HIGH_RID 0x00003000UL
#endif

#ifndef SECURITY_MANDATORY_SYSTEM_RID
#define SECURITY_MANDATORY_SYSTEM_RID 0x00004000UL
#endif

#ifndef FILE_DISPOSITION_DELETE
#define FILE_DISPOSITION_DELETE 0x00000001UL
#endif

#ifndef FILE_DISPOSITION_POSIX_SEMANTICS
#define FILE_DISPOSITION_POSIX_SEMANTICS 0x00000002UL
#endif

#ifndef FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK
#define FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK 0x00000004UL
#endif

#ifndef FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE 0x00000010UL
#endif

// Use the documented value 64 for FileDispositionInformationEx to support
// older WDK headers where the enum constant may be missing.
#define KSWORD_FILE_DISPOSITION_INFORMATION_EX_CLASS_VALUE ((FILE_INFORMATION_CLASS)64)

typedef struct _KSWORD_FILE_DISPOSITION_INFORMATION_EX
{
    ULONG Flags;
} KSWORD_FILE_DISPOSITION_INFORMATION_EX, *PKSWORD_FILE_DISPOSITION_INFORMATION_EX;

// Resolve the optional create entry point at runtime so old Windows 10 builds
// never gain a new loader-time import dependency.
typedef NTSTATUS (NTAPI* KSWORD_ARK_IO_CREATE_FILE_EX_FN)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG,
    CREATE_FILE_TYPE, PVOID, ULONG, PIO_DRIVER_CREATE_CONTEXT);

typedef PVOID
(NTAPI* KSWORD_ARK_FILE_EX_ALLOCATE_POOL2_FN)(
    _In_ POOL_FLAGS Flags,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag
    );

NTKERNELAPI
NTSTATUS
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
    );

static PVOID
KswordARKDriverFileAllocateNonPaged(
    _In_ SIZE_T BufferBytes
    )
/*++

Routine Description:

    分配文件查询临时缓冲。中文说明：优先动态解析 ExAllocatePool2，旧系统或
    旧 WDK 兼容路径回退到 ExAllocatePoolWithTag，避免驱动因为导入缺失加载失败。

Arguments:

    BufferBytes - 需要分配的字节数。

Return Value:

    非分页池指针，失败返回 NULL。

--*/
{
    static volatile LONG allocatorResolved = 0;
    static KSWORD_ARK_FILE_EX_ALLOCATE_POOL2_FN exAllocatePool2Fn = NULL;

    if (BufferBytes == 0U) {
        return NULL;
    }

    if (InterlockedCompareExchange(&allocatorResolved, 1L, 0L) == 0L) {
        UNICODE_STRING routineName;
        RtlInitUnicodeString(&routineName, L"ExAllocatePool2");
        exAllocatePool2Fn = (KSWORD_ARK_FILE_EX_ALLOCATE_POOL2_FN)MmGetSystemRoutineAddress(&routineName);
    }

    if (exAllocatePool2Fn != NULL) {
        return exAllocatePool2Fn(POOL_FLAG_NON_PAGED, BufferBytes, 'fOsK');
    }

#pragma warning(push)
#pragma warning(disable:4996)
    return ExAllocatePoolWithTag(NonPagedPoolNx, BufferBytes, 'fOsK');
#pragma warning(pop)
}

static VOID
KswordARKDriverCopyWideStringToFixedBuffer(
    _Out_writes_(DestinationChars) PWSTR Destination,
    _In_ USHORT DestinationChars,
    _In_reads_opt_(SourceChars) PCWSTR Source,
    _In_ USHORT SourceChars
    )
/*++

Routine Description:

    把内核 UNICODE_STRING 或请求路径复制到固定响应缓冲。中文说明：复制时
    始终保留 NUL 结尾，长度不足时截断，避免 R3 解析固定数组越界。

Arguments:

    Destination - 响应包中的固定宽字符数组。
    DestinationChars - Destination 的元素数量。
    Source - 来源宽字符指针，可为空。
    SourceChars - 来源字符数，不包含 NUL。

Return Value:

    None. 本函数没有返回值。

--*/
{
    USHORT copyChars = 0U;

    if (Destination == NULL || DestinationChars == 0U) {
        return;
    }

    Destination[0] = L'\0';
    if (Source == NULL || SourceChars == 0U) {
        return;
    }

    copyChars = SourceChars;
    if (copyChars >= DestinationChars) {
        copyChars = DestinationChars - 1U;
    }

    RtlCopyMemory(Destination, Source, (SIZE_T)copyChars * sizeof(WCHAR));
    Destination[copyChars] = L'\0';
}

static NTSTATUS
KswordARKDriverQueryFileObjectName(
    _In_ PVOID Object,
    _Out_writes_(ObjectNameChars) PWSTR ObjectName,
    _In_ USHORT ObjectNameChars
    )
/*++

Routine Description:

    查询 FILE_OBJECT 对象名。中文说明：采用 ObQueryNameString 两段式查询，
    第一段获取长度，第二段分配 NonPagedPoolNx 并复制到响应固定数组。

Arguments:

    Object - 已引用的内核对象，通常是 FILE_OBJECT 或 DEVICE_OBJECT。
    ObjectName - 响应包对象名数组。
    ObjectNameChars - ObjectName 数组长度。

Return Value:

    STATUS_SUCCESS 或 ObQueryNameString/内存分配错误。

--*/
{
    POBJECT_NAME_INFORMATION nameInfo = NULL;
    ULONG requiredBytes = 0UL;
    ULONG allocationBytes = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (ObjectName != NULL && ObjectNameChars > 0U) {
        ObjectName[0] = L'\0';
    }
    if (Object == NULL || ObjectName == NULL || ObjectNameChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ObQueryNameString(Object, NULL, 0UL, &requiredBytes);
    if (status != STATUS_INFO_LENGTH_MISMATCH &&
        status != STATUS_BUFFER_OVERFLOW &&
        status != STATUS_BUFFER_TOO_SMALL &&
        !NT_SUCCESS(status)) {
        return status;
    }
    if (requiredBytes < sizeof(OBJECT_NAME_INFORMATION)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    allocationBytes = requiredBytes + sizeof(WCHAR);
    nameInfo = (POBJECT_NAME_INFORMATION)KswordARKDriverFileAllocateNonPaged(allocationBytes);
    if (nameInfo == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(nameInfo, allocationBytes);
    status = ObQueryNameString(Object, nameInfo, allocationBytes, &requiredBytes);
    if (NT_SUCCESS(status)) {
        const USHORT sourceChars = (USHORT)(nameInfo->Name.Length / sizeof(WCHAR));
        KswordARKDriverCopyWideStringToFixedBuffer(
            ObjectName,
            ObjectNameChars,
            nameInfo->Name.Buffer,
            sourceChars);
    }

    ExFreePoolWithTag(nameInfo, 'fOsK');
    return status;
}

static VOID
KswordARKDriverFillFileObjectAuditFields(
    _In_ PFILE_OBJECT FileObject,
    _Inout_ KSWORD_ARK_QUERY_FILE_INFO_RESPONSE* Response
    )
/*++

Routine Description:

    Copy public FILE_OBJECT audit fields into the response. 中文说明：该函数只读
    FILE_OBJECT 的 WDK 公开字段；所有地址仅用于诊断展示，不作为后续操作凭据。

Arguments:

    FileObject - 已引用的 FILE_OBJECT。
    Response - 可写响应包。

Return Value:

    None. 本函数没有返回值。

--*/
{
    if (FileObject == NULL || Response == NULL) {
        return;
    }

    __try {
        Response->deviceObjectAddress = (ULONG64)(ULONG_PTR)FileObject->DeviceObject;
        Response->vpbAddress = (ULONG64)(ULONG_PTR)FileObject->Vpb;
        Response->fsContextAddress = (ULONG64)(ULONG_PTR)FileObject->FsContext;
        Response->fsContext2Address = (ULONG64)(ULONG_PTR)FileObject->FsContext2;
        Response->deletePending = (FileObject->DeletePending != FALSE) ? 1UL : 0UL;
        Response->readAccess = (FileObject->ReadAccess != FALSE) ? 1UL : 0UL;
        Response->writeAccess = (FileObject->WriteAccess != FALSE) ? 1UL : 0UL;
        Response->deleteAccess = (FileObject->DeleteAccess != FALSE) ? 1UL : 0UL;
        Response->sharedRead = (FileObject->SharedRead != FALSE) ? 1UL : 0UL;
        Response->sharedWrite = (FileObject->SharedWrite != FALSE) ? 1UL : 0UL;
        Response->sharedDelete = (FileObject->SharedDelete != FALSE) ? 1UL : 0UL;
        Response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_SHARE_ACCESS_PRESENT;
        if (FileObject->DeviceObject != NULL) {
            Response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DEVICE_OBJECT_PRESENT;
        }
        if (FileObject->Vpb != NULL) {
            Response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_VPB_PRESENT;
        }
        if (FileObject->FsContext != NULL || FileObject->FsContext2 != NULL) {
            Response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_FS_CONTEXT_PRESENT;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Response->objectStatus = GetExceptionCode();
        if (Response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
            Response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED;
        }
    }
}

static VOID
KswordARKDriverFillFileVpbAuditFields(
    _In_ PFILE_OBJECT FileObject,
    _Inout_ KSWORD_ARK_QUERY_FILE_INFO_RESPONSE* Response
    )
/*++

Routine Description:

    Copy public VPB fields for the file object's mounted volume. 中文说明：VPB
    可能不存在或正在变化，因此读取放在 SEH 中，失败只降级当前响应。

Arguments:

    FileObject - 已引用的 FILE_OBJECT。
    Response - 可写响应包。

Return Value:

    None. 本函数没有返回值。

--*/
{
    PVPB vpb = NULL;
    USHORT labelChars = 0U;

    if (FileObject == NULL || Response == NULL) {
        return;
    }

    __try {
        vpb = FileObject->Vpb;
        if (vpb == NULL) {
            return;
        }
        Response->vpbFlags = (ULONG)vpb->Flags;
        Response->vpbSerialNumber = vpb->SerialNumber;
        labelChars = (USHORT)(vpb->VolumeLabelLength / sizeof(WCHAR));
        if (labelChars > RTL_NUMBER_OF(vpb->VolumeLabel)) {
            labelChars = (USHORT)RTL_NUMBER_OF(vpb->VolumeLabel);
        }
        if (labelChars >= KSWORD_ARK_FILE_INFO_VOLUME_LABEL_MAX_CHARS) {
            labelChars = KSWORD_ARK_FILE_INFO_VOLUME_LABEL_MAX_CHARS - 1U;
        }
        KswordARKDriverCopyWideStringToFixedBuffer(
            Response->volumeLabel,
            KSWORD_ARK_FILE_INFO_VOLUME_LABEL_MAX_CHARS,
            vpb->VolumeLabel,
            labelChars);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Response->objectStatus = GetExceptionCode();
        if (Response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
            Response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED;
        }
    }
}

static NTSTATUS
KswordARKDriverOpenFileForQuery(
    _In_reads_(PathLengthChars) PCWSTR PathText,
    _In_ USHORT PathLengthChars,
    _In_ ULONG Flags,
    _Out_ HANDLE* FileHandleOut
    )
/*++

Routine Description:

    为只读文件信息查询打开目标路径。中文说明：打开权限只包含
    FILE_READ_ATTRIBUTES/SYNCHRONIZE，并允许 READ/WRITE/DELETE 共享，避免
    查询动作本身改变占用关系或阻塞被其它进程持有的文件。

Arguments:

    PathText - NT 路径，通常是 \??\C:\...。
    PathLengthChars - 路径字符数，不含 NUL。
    Flags - KSWORD_ARK_QUERY_FILE_INFO_FLAG_*。
    FileHandleOut - 接收内核句柄，调用方负责 ZwClose。

Return Value:

    ZwCreateFile 返回的 NTSTATUS。

--*/
{
    UNICODE_STRING targetPath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    ULONG createOptions = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileHandleOut = NULL;
    if (PathText == NULL || PathLengthChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    /*
     * 长度上界必须在构造 UNICODE_STRING 之前复核。targetPath.Buffer 直接指向调用方
     * 的缓冲，Length 一旦超出该缓冲的实际容量，ObpCaptureObjectName 会按 Length
     * 拷贝并读到分配之外的页，直接 PAGE_FAULT_IN_NONPAGED_AREA。这里不假设调用方
     * 已经校验过，保证任何长度错误都退化成 STATUS_INVALID_PARAMETER 而不是 bugcheck。
     */
    if (PathLengthChars >= KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&targetPath, sizeof(targetPath));
    targetPath.Buffer = (PWCH)PathText;
    targetPath.Length = (USHORT)(PathLengthChars * sizeof(WCHAR));
    targetPath.MaximumLength = targetPath.Length + sizeof(WCHAR);

    InitializeObjectAttributes(
        &objectAttributes,
        &targetPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    createOptions = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT;
    if ((Flags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_OPEN_REPARSE_POINT) != 0UL) {
        createOptions |= FILE_OPEN_REPARSE_POINT;
    }
    if ((Flags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_DIRECTORY) != 0UL) {
        createOptions |= FILE_DIRECTORY_FILE;
    }
    else {
        createOptions |= FILE_NON_DIRECTORY_FILE;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        FileHandleOut,
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        createOptions,
        NULL,
        0U);

    return status;
}

typedef struct _KSWORD_ARK_FILE_LABEL_SD_BUFFER
{
    SECURITY_DESCRIPTOR_RELATIVE SecurityDescriptor;
    UCHAR AclBuffer[sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE];
} KSWORD_ARK_FILE_LABEL_SD_BUFFER, *PKSWORD_ARK_FILE_LABEL_SD_BUFFER;

static BOOLEAN
KswordARKDriverIsSupportedFileMandatoryIntegrityRid(
    _In_ ULONG IntegrityRid
    )
/*++

Routine Description:

    Validate a mandatory-integrity RID before building a file-object label.
    中文说明：ProtectedProcess/SecureProcess 这类 RID 是令牌/进程完整性语义，
    不是文件 SACL 可接受的 Mandatory Label。提前拒绝可避免把无效 SID 交给
    ZwSetSecurityObject 后只得到较晚的 STATUS_INVALID_LABEL。

Arguments:

    IntegrityRid - S-1-16-* 的最后一级 RID。

Return Value:

    TRUE 表示该 RID 可用于文件/目录 LABEL_SECURITY_INFORMATION；FALSE 表示
    请求方传入了不适合文件对象的完整性级别。

--*/
{
    switch (IntegrityRid) {
    case SECURITY_MANDATORY_UNTRUSTED_RID:
    case SECURITY_MANDATORY_LOW_RID:
    case SECURITY_MANDATORY_MEDIUM_RID:
    case SECURITY_MANDATORY_MEDIUM_PLUS_RID:
    case SECURITY_MANDATORY_HIGH_RID:
    case SECURITY_MANDATORY_SYSTEM_RID:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS
KswordARKDriverBuildFileMandatoryIntegritySid(
    _In_ ULONG IntegrityRid,
    _Out_writes_bytes_(SECURITY_MAX_SID_SIZE) PSID SidBuffer
    )
/*++

Routine Description:

    Build an S-1-16-* mandatory integrity SID for file-object label assignment.

Arguments:

    IntegrityRid - Mandatory label RID.
    SidBuffer - Caller-provided SECURITY_MAX_SID_SIZE storage.

Return Value:

    STATUS_SUCCESS or an RTL SID construction status.

--*/
{
    SID_IDENTIFIER_AUTHORITY mandatoryLabelAuthority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PULONG subAuthority = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (SidBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!KswordARKDriverIsSupportedFileMandatoryIntegrityRid(IntegrityRid)) {
        return STATUS_INVALID_LABEL;
    }

    RtlZeroMemory(SidBuffer, SECURITY_MAX_SID_SIZE);
    status = RtlInitializeSid(SidBuffer, &mandatoryLabelAuthority, 1);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    subAuthority = RtlSubAuthoritySid(SidBuffer, 0);
    if (subAuthority == NULL) {
        return STATUS_INVALID_SID;
    }
    *subAuthority = IntegrityRid;

    return RtlValidSid(SidBuffer) ? STATUS_SUCCESS : STATUS_INVALID_SID;
}

static NTSTATUS
KswordARKDriverBuildLabelSecurityDescriptor(
    _In_ ULONG IntegrityRid,
    _Out_ KSWORD_ARK_FILE_LABEL_SD_BUFFER* DescriptorBuffer
    )
/*++

Routine Description:

    Build a self-relative security descriptor containing only a SACL with one
    SYSTEM_MANDATORY_LABEL_ACE. 中文说明：该描述符只用于
    ZwSetSecurityObject(LABEL_SECURITY_INFORMATION)，不会携带 Owner/DACL。

Arguments:

    IntegrityRid - Mandatory label RID.
    DescriptorBuffer - Writable descriptor+ACL storage.

Return Value:

    STATUS_SUCCESS or RTL ACL/SID construction failure.

--*/
{
    UCHAR sidBuffer[SECURITY_MAX_SID_SIZE] = { 0 };
    UCHAR aceBuffer[sizeof(SYSTEM_MANDATORY_LABEL_ACE) + SECURITY_MAX_SID_SIZE] = { 0 };
    PACL sacl = NULL;
    PSYSTEM_MANDATORY_LABEL_ACE mandatoryAce = (PSYSTEM_MANDATORY_LABEL_ACE)aceBuffer;
    ULONG sidLength = 0UL;
    ULONG aceLength = 0UL;
    ULONG aclLength = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (DescriptorBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(DescriptorBuffer, sizeof(*DescriptorBuffer));
    status = KswordARKDriverBuildFileMandatoryIntegritySid(IntegrityRid, (PSID)sidBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    sidLength = RtlLengthSid((PSID)sidBuffer);
    aceLength = FIELD_OFFSET(SYSTEM_MANDATORY_LABEL_ACE, SidStart) + sidLength;
    aclLength = sizeof(ACL) + aceLength;
    if (aclLength > sizeof(DescriptorBuffer->AclBuffer) || aceLength > 0xFFFFUL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    sacl = (PACL)DescriptorBuffer->AclBuffer;
    status = RtlCreateAcl(sacl, aclLength, ACL_REVISION);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    mandatoryAce->Header.AceType = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
    mandatoryAce->Header.AceFlags = 0;
    mandatoryAce->Header.AceSize = (USHORT)aceLength;
    mandatoryAce->Mask = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
    status = RtlCopySid(
        sidLength,
        (PSID)&mandatoryAce->SidStart,
        (PSID)sidBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = RtlAddAce(sacl, ACL_REVISION, MAXULONG, mandatoryAce, aceLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Build SECURITY_DESCRIPTOR_RELATIVE manually instead of depending on
     * RtlCreateSecurityDescriptorRelative, which is not present in every WDK
     * header set.  The descriptor carries only a SACL because
     * ZwSetSecurityObject(LABEL_SECURITY_INFORMATION) consumes just the
     * mandatory-label information for this operation.
     */
    DescriptorBuffer->SecurityDescriptor.Revision = SECURITY_DESCRIPTOR_REVISION;
    DescriptorBuffer->SecurityDescriptor.Sbz1 = 0;
    DescriptorBuffer->SecurityDescriptor.Control =
        (SECURITY_DESCRIPTOR_CONTROL)(SE_SELF_RELATIVE | SE_SACL_PRESENT);
    DescriptorBuffer->SecurityDescriptor.Owner = 0;
    DescriptorBuffer->SecurityDescriptor.Group = 0;
    DescriptorBuffer->SecurityDescriptor.Sacl =
        FIELD_OFFSET(KSWORD_ARK_FILE_LABEL_SD_BUFFER, AclBuffer);
    DescriptorBuffer->SecurityDescriptor.Dacl = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS
KswordARKDriverOpenFileForIntegritySet(
    _In_reads_(PathLengthChars) PCWSTR PathText,
    _In_ USHORT PathLengthChars,
    _In_ ULONG Flags,
    _Out_ HANDLE* FileHandleOut
    )
/*++

Routine Description:

    Open a file or directory for mandatory label writes.

Arguments:

    PathText - NT path.
    PathLengthChars - Character length excluding NUL.
    Flags - KSWORD_ARK_FILE_INTEGRITY_FLAG_*.
    FileHandleOut - Receives a kernel handle.

Return Value:

    ZwCreateFile status.

--*/
{
    UNICODE_STRING targetPath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    ULONG createOptions = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (FileHandleOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *FileHandleOut = NULL;
    if (PathText == NULL || PathLengthChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    /*
     * 与 KswordARKDriverOpenFileForQuery 同理：超长 Length 会让内核在捕获
     * ObjectName 时读出调用方缓冲之外的页。上界在此复核，不依赖 handler 校验。
     */
    if (PathLengthChars >= KSWORD_ARK_FILE_INTEGRITY_PATH_MAX_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&targetPath, sizeof(targetPath));
    targetPath.Buffer = (PWCH)PathText;
    targetPath.Length = (USHORT)(PathLengthChars * sizeof(WCHAR));
    targetPath.MaximumLength = targetPath.Length + sizeof(WCHAR);

    InitializeObjectAttributes(
        &objectAttributes,
        &targetPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    createOptions = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT;
    if ((Flags & KSWORD_ARK_FILE_INTEGRITY_FLAG_DIRECTORY) != 0UL) {
        createOptions |= FILE_DIRECTORY_FILE;
    }
    else {
        createOptions |= FILE_NON_DIRECTORY_FILE;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        FileHandleOut,
        WRITE_OWNER | READ_CONTROL | ACCESS_SYSTEM_SECURITY | SYNCHRONIZE,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        createOptions,
        NULL,
        0U);
    return status;
}

NTSTATUS
KswordARKDriverSetFileIntegrity(
    _In_ const KSWORD_ARK_SET_FILE_INTEGRITY_REQUEST* Request
    )
/*++

Routine Description:

    Set a file or directory mandatory integrity label using kernel security
    APIs. 中文说明：本函数只调用 ZwCreateFile 与
    ZwSetSecurityObject(LABEL_SECURITY_INFORMATION)，不改文件系统私有结构。

Arguments:

    Request - Validated IOCTL request.

Return Value:

    NTSTATUS from open, descriptor construction, or ZwSetSecurityObject.

--*/
{
    HANDLE fileHandle = NULL;
    KSWORD_ARK_FILE_LABEL_SD_BUFFER descriptorBuffer;
    NTSTATUS status = STATUS_SUCCESS;

    if (Request == NULL ||
        Request->pathLengthChars == 0U ||
        Request->pathLengthChars >= KSWORD_ARK_FILE_INTEGRITY_PATH_MAX_CHARS ||
        Request->path[Request->pathLengthChars] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    status = KswordARKDriverBuildLabelSecurityDescriptor(
        Request->integrityRid,
        &descriptorBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDriverOpenFileForIntegritySet(
        Request->path,
        Request->pathLengthChars,
        Request->flags,
        &fileHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwSetSecurityObject(
        fileHandle,
        LABEL_SECURITY_INFORMATION,
        &descriptorBuffer.SecurityDescriptor);

    ZwClose(fileHandle);
    return status;
}

NTSTATUS
KswordARKDriverQueryFileInfo(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_QUERY_FILE_INFO_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    )
/*++

Routine Description:

    查询文件基础信息。中文说明：本函数是 Phase-10 文件对象信息的 R0 后端，
    只做只读属性查询；对象地址和 SectionObjectPointer 仅用于 UI 诊断展示，
    不允许 R3 在后续 IOCTL 中把这些地址作为凭据传回。

Arguments:

    OutputBuffer - 响应包缓冲区。
    OutputBufferLength - 响应包缓冲区长度。
    Request - 请求包，包含 NT 路径和查询 flags。
    BytesWrittenOut - 接收 sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE)。

Return Value:

    STATUS_SUCCESS 表示响应包有效；失败细节写入 response->queryStatus。

--*/
{
    KSWORD_ARK_QUERY_FILE_INFO_RESPONSE* response = NULL;
    HANDLE fileHandle = NULL;
    PFILE_OBJECT fileObject = NULL;
    FILE_BASIC_INFORMATION basicInformation;
    FILE_STANDARD_INFORMATION standardInformation;
    IO_STATUS_BLOCK ioStatusBlock;
    PSECTION_OBJECT_POINTERS sectionPointers = NULL;
    ULONG requestFlags = 0UL;
    NTSTATUS status = STATUS_SUCCESS;

    if (OutputBuffer == NULL || Request == NULL || BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;
    if (OutputBufferLength < sizeof(KSWORD_ARK_QUERY_FILE_INFO_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Request->pathLengthChars == 0U ||
        Request->pathLengthChars >= KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS ||
        Request->path[Request->pathLengthChars] != L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(OutputBuffer, OutputBufferLength);
    RtlZeroMemory(&basicInformation, sizeof(basicInformation));
    RtlZeroMemory(&standardInformation, sizeof(standardInformation));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    response = (KSWORD_ARK_QUERY_FILE_INFO_RESPONSE*)OutputBuffer;
    response->version = KSWORD_ARK_FILE_PROTOCOL_VERSION;
    response->size = sizeof(*response);
    response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE;
    response->openStatus = STATUS_SUCCESS;
    response->basicStatus = STATUS_NOT_SUPPORTED;
    response->standardStatus = STATUS_NOT_SUPPORTED;
    response->objectStatus = STATUS_NOT_SUPPORTED;
    response->nameStatus = STATUS_NOT_SUPPORTED;
    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_REQUEST_PATH_PRESENT;
    KswordARKDriverCopyWideStringToFixedBuffer(
        response->ntPath,
        KSWORD_ARK_FILE_INFO_PATH_MAX_CHARS,
        Request->path,
        Request->pathLengthChars);

    requestFlags = Request->flags;
    if (requestFlags == 0UL) {
        requestFlags = KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_ALL;
    }

    status = KswordARKDriverOpenFileForQuery(
        Request->path,
        Request->pathLengthChars,
        requestFlags,
        &fileHandle);
    response->openStatus = status;
    if (!NT_SUCCESS(status)) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OPEN_FAILED;
        *BytesWrittenOut = sizeof(*response);
        return STATUS_SUCCESS;
    }

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatusBlock,
        &basicInformation,
        (ULONG)sizeof(basicInformation),
        FileBasicInformation);
    response->basicStatus = status;
    if (NT_SUCCESS(status)) {
        response->fileAttributes = basicInformation.FileAttributes;
        response->creationTime = basicInformation.CreationTime.QuadPart;
        response->lastAccessTime = basicInformation.LastAccessTime.QuadPart;
        response->lastWriteTime = basicInformation.LastWriteTime.QuadPart;
        response->changeTime = basicInformation.ChangeTime.QuadPart;
        response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_BASIC_PRESENT;
        if ((basicInformation.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0UL) {
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DIRECTORY;
        }
    }
    else {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_BASIC_FAILED;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatusBlock,
        &standardInformation,
        (ULONG)sizeof(standardInformation),
        FileStandardInformation);
    response->standardStatus = status;
    if (NT_SUCCESS(status)) {
        response->allocationSize = standardInformation.AllocationSize.QuadPart;
        response->endOfFile = standardInformation.EndOfFile.QuadPart;
        response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_STANDARD_PRESENT;
        if (standardInformation.Directory != FALSE) {
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DIRECTORY;
        }
    }
    else if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_STANDARD_FAILED;
    }

    if ((requestFlags & (KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME |
        KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS)) != 0UL) {
        status = ObReferenceObjectByHandle(
            fileHandle,
            0,
            *IoFileObjectType,
            KernelMode,
            (PVOID*)&fileObject,
            NULL);
        response->objectStatus = status;
        if (NT_SUCCESS(status)) {
            response->fileObjectAddress = (ULONG64)(ULONG_PTR)fileObject;
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_FILE_OBJECT_PRESENT;
            KswordARKDriverFillFileObjectAuditFields(fileObject, response);
            KswordARKDriverFillFileVpbAuditFields(fileObject, response);
            if (fileObject->DeviceObject != NULL) {
                (VOID)KswordARKDriverQueryFileObjectName(
                    fileObject->DeviceObject,
                    response->deviceName,
                    KSWORD_ARK_FILE_INFO_DEVICE_NAME_MAX_CHARS);
            }
        }
        else if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED;
        }
    }

    if (fileObject != NULL &&
        (requestFlags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_OBJECT_NAME) != 0UL) {
        status = KswordARKDriverQueryFileObjectName(
            fileObject,
            response->objectName,
            KSWORD_ARK_FILE_INFO_OBJECT_NAME_MAX_CHARS);
        response->nameStatus = status;
        if (NT_SUCCESS(status)) {
            response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_OBJECT_NAME_PRESENT;
        }
        else if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
            response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_NAME_FAILED;
        }
    }

    if (fileObject != NULL &&
        (requestFlags & KSWORD_ARK_QUERY_FILE_INFO_FLAG_INCLUDE_SECTION_POINTERS) != 0UL) {
        __try {
            sectionPointers = fileObject->SectionObjectPointer;
            response->sectionObjectPointersAddress = (ULONG64)(ULONG_PTR)sectionPointers;
            if (sectionPointers != NULL) {
                response->dataSectionObjectAddress = (ULONG64)(ULONG_PTR)sectionPointers->DataSectionObject;
                response->imageSectionObjectAddress = (ULONG64)(ULONG_PTR)sectionPointers->ImageSectionObject;
                response->sharedCacheMapAddress = (ULONG64)(ULONG_PTR)sectionPointers->SharedCacheMap;
                response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_SECTION_POINTERS_PRESENT;
                if (sectionPointers->DataSectionObject != NULL) {
                    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_DATA_SECTION_PRESENT;
                }
                if (sectionPointers->ImageSectionObject != NULL) {
                    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_IMAGE_SECTION_PRESENT;
                }
                if (sectionPointers->SharedCacheMap != NULL) {
                    response->fieldFlags |= KSWORD_ARK_FILE_INFO_FIELD_SHARED_CACHE_MAP_PRESENT;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            response->objectStatus = GetExceptionCode();
            if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
                response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OBJECT_FAILED;
            }
        }
    }

    if (response->queryStatus == KSWORD_ARK_FILE_INFO_STATUS_UNAVAILABLE) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_OK;
    }
    else if (response->fieldFlags != 0UL &&
        response->queryStatus != KSWORD_ARK_FILE_INFO_STATUS_OK) {
        response->queryStatus = KSWORD_ARK_FILE_INFO_STATUS_PARTIAL;
    }

    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
    ZwClose(fileHandle);
    *BytesWrittenOut = sizeof(*response);
    return STATUS_SUCCESS;
}

static BOOLEAN
KswordARKDriverShouldRetryDeleteWithDispositionEx(
    _In_ NTSTATUS deleteStatus,
    _In_ BOOLEAN isDirectory
    )
/*++

Routine Description:

    Decide whether delete failure should trigger FileDispositionInformationEx
    fallback. This fallback is file-only and targets common "in-use" failures.

Arguments:

    deleteStatus - First delete attempt status.
    isDirectory - TRUE when target is directory.

Return Value:

    BOOLEAN

--*/
{
    if (isDirectory) {
        return FALSE;
    }

    switch (deleteStatus) {
    case STATUS_ACCESS_DENIED:
    case STATUS_SHARING_VIOLATION:
    case STATUS_CANNOT_DELETE:
    case STATUS_USER_MAPPED_FILE:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN
KswordARKDriverIsDispositionExUnsupportedStatus(
    _In_ NTSTATUS dispositionStatus
    )
/*++

Routine Description:

    判断 FileDispositionInformationEx 是否因为系统/文件系统不支持而失败。
    中文说明：这类失败不代表目标文件不可删除，调用方应保留传统
    FileDispositionInformation 的原始状态，避免把诊断误导成参数错误。

Arguments:

    dispositionStatus - ZwSetInformationFile(FileDispositionInformationEx) 返回值。

Return Value:

    TRUE 表示应视为 Ex 删除路径不可用；FALSE 表示这是一次真实删除失败。

--*/
{
    switch (dispositionStatus) {
    case STATUS_INVALID_INFO_CLASS:
    case STATUS_NOT_SUPPORTED:
    case STATUS_INVALID_PARAMETER:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS
KswordARKDriverSetFileDispositionExFlags(
    _In_ HANDLE fileHandle,
    _In_ ULONG dispositionFlags
    )
/*++

Routine Description:

    使用调用方指定的 FileDispositionInformationEx flags 标记文件删除。
    中文说明：把 ZwSetInformationFile 的薄封装拆出来，便于主删除流程先尝试
    POSIX unlink 语义，再在兼容性需要时尝试旧的 force-image-check 组合。

Arguments:

    fileHandle - 已用 DELETE 权限打开的文件句柄。
    dispositionFlags - FILE_DISPOSITION_* 标志组合。

Return Value:

    ZwSetInformationFile 返回的 NTSTATUS。

--*/
{
    KSWORD_FILE_DISPOSITION_INFORMATION_EX dispositionInformationEx;
    IO_STATUS_BLOCK ioStatusBlock;

    RtlZeroMemory(&dispositionInformationEx, sizeof(dispositionInformationEx));
    dispositionInformationEx.Flags = dispositionFlags;

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    return ZwSetInformationFile(
        fileHandle,
        &ioStatusBlock,
        &dispositionInformationEx,
        (ULONG)sizeof(dispositionInformationEx),
        KSWORD_FILE_DISPOSITION_INFORMATION_EX_CLASS_VALUE);
}

static NTSTATUS
KswordARKDriverFlushImageSectionForDelete(
    _In_ HANDLE fileHandle
    )
/*++

Routine Description:

    Ask the memory manager to flush the target file's image section before a
    delete retry. 中文说明：运行中 EXE/DLL 常见失败码是 STATUS_CANNOT_DELETE，
    根因通常是 FileObject->SectionObjectPointer->ImageSectionObject 仍存在。
    MmFlushImageSection(MmFlushForDelete) 是公开的安全删除前置检查：如果映像
    section 已经没有活动映射，它会清理并允许后续删除；如果进程仍在运行，它会
    返回 FALSE，本函数保留 STATUS_CANNOT_DELETE，不做不安全的 ControlArea 手术。

Arguments:

    fileHandle - 已打开的目标文件句柄。

Return Value:

    STATUS_SUCCESS 表示 image section 已可安全清理或无需清理；
    STATUS_CANNOT_DELETE 表示仍有活动映像 section，调用方应提示先结束进程或重启删除；
    其它 NTSTATUS 表示引用 FILE_OBJECT 失败。

--*/
{
    PFILE_OBJECT fileObject = NULL;
    NTSTATUS status;

    status = ObReferenceObjectByHandle(
        fileHandle,
        0,
        *IoFileObjectType,
        KernelMode,
        (PVOID*)&fileObject,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDriverFlushFileObjectForDelete(fileObject); // 交由 MM 清理共享映像状态并检查数据 section 的写探针。
    ObDereferenceObject(fileObject); // 刷新完成后释放本次引用。
    return status; // 活动映射仍返回真实失败。
}

static NTSTATUS
KswordARKDriverDeleteFileWithDispositionEx(
    _In_ HANDLE fileHandle,
    _In_ BOOLEAN IgnoreReadOnly,
    _In_ BOOLEAN AllowLegacyFallback
    )
/*++

Routine Description:

    使用 FileDispositionInformationEx 请求 POSIX 删除。普通 POSIX 后端先不请求
    IGNORE_READONLY_ATTRIBUTE；只读文件的针对性重试才启用它。高风险后端不回退
    到 FORCE_IMAGE_SECTION_CHECK；其它后端仅在 Ex 参数不受支持时兼容旧组合。

Arguments:

    fileHandle - 已用 DELETE 权限打开的文件句柄。

Return Value:

    NTSTATUS。成功表示目标已被标记删除；不支持 Ex 时返回首选尝试的状态，
    上层会继续使用传统删除的 firstDeleteStatus 作为最终诊断。

--*/
{
    const ULONG preferredDispositionFlags =
        FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS |
        (IgnoreReadOnly ? FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE : 0UL);
    const ULONG legacyDispositionFlags =
        preferredDispositionFlags
        | FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK;
    NTSTATUS status;
    NTSTATUS legacyStatus;

    status = KswordARKDriverSetFileDispositionExFlags(fileHandle, preferredDispositionFlags);
    if (NT_SUCCESS(status)) {
        return status;
    }

    if (!AllowLegacyFallback ||
        !KswordARKDriverIsDispositionExUnsupportedStatus(status)) {
        return status;
    }

    legacyStatus = KswordARKDriverSetFileDispositionExFlags(fileHandle, legacyDispositionFlags);
    if (NT_SUCCESS(legacyStatus)) {
        return legacyStatus;
    }

    if (!KswordARKDriverIsDispositionExUnsupportedStatus(legacyStatus)) {
        return legacyStatus;
    }

    return status;
}

static NTSTATUS
KswordARKDriverNormalizeReadOnlyAttribute(
    _In_ HANDLE fileHandle
    )
/*++

Routine Description:

    Clear FILE_ATTRIBUTE_READONLY before delete so driver delete can handle
    read-only files without an extra user-mode retry.

Arguments:

    fileHandle - Open file or directory handle.

Return Value:

    NTSTATUS

--*/
{
    FILE_BASIC_INFORMATION basicInformation;
    IO_STATUS_BLOCK ioStatusBlock;
    NTSTATUS status;

    RtlZeroMemory(&basicInformation, sizeof(basicInformation));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatusBlock,
        &basicInformation,
        (ULONG)sizeof(basicInformation),
        FileBasicInformation);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if ((basicInformation.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0U) {
        return STATUS_SUCCESS;
    }

    basicInformation.FileAttributes &= ~((ULONG)FILE_ATTRIBUTE_READONLY);
    if (basicInformation.FileAttributes == 0U) {
        basicInformation.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    return ZwSetInformationFile(
        fileHandle,
        &ioStatusBlock,
        &basicInformation,
        (ULONG)sizeof(basicInformation),
        FileBasicInformation);
}

static NTSTATUS
KswordARKDriverDeletePathByIrp(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory,
    _Inout_opt_ KSWORD_ARK_DELETE_PATH_RESPONSE* Details
    )
/*++

Routine Description:

    使用现有通用 IRP 引擎的 BASE_FS CREATE 和配对 CLEANUP/CLOSE。
    删除优先 Ex/POSIX + 忽略只读，旧文件系统回退传统类；受阻时直接清空
    section 三成员再重试。本后端不会回退到 ZwSetInformationFile。

Arguments:

    pathText/pathLengthChars - 已校验的 NT 路径。
    isDirectory - TRUE 表示 CREATE 阶段要求目录语义。
    Details - 可选 v2 删除回执，记录 CREATE 与目标 IRP 的实际状态。

Return Value:

    CREATE、目标 IRP 或收尾阶段的首个失败 NTSTATUS；全部完成时返回 STATUS_SUCCESS。

--*/
{
    KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST* request = NULL;
    KSWORD_ARK_FILE_IRP_SUBMIT_RESPONSE* response = NULL;
    FILE_DISPOSITION_INFORMATION dispositionInformation;
    const SIZE_T requestBytes = KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST_HEADER_SIZE;
    const SIZE_T responseBytes = KSWORD_ARK_FILE_IRP_SUBMIT_RESPONSE_HEADER_SIZE;
    size_t bytesWritten = 0U;
    NTSTATUS transportStatus;
    NTSTATUS resultStatus = STATUS_UNSUCCESSFUL;

    if (pathText == NULL || pathLengthChars == 0U ||
        pathLengthChars >= KSWORD_ARK_FILE_IRP_PATH_MAX_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }

    request = (KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST*)
        KswordARKDriverFileAllocateNonPaged(requestBytes);
    response = (KSWORD_ARK_FILE_IRP_SUBMIT_RESPONSE*)
        KswordARKDriverFileAllocateNonPaged(responseBytes);
    if (request == NULL || response == NULL) {
        resultStatus = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(request, requestBytes);
    request->version = KSWORD_ARK_FILE_IRP_PROTOCOL_VERSION;
    request->size = KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST_HEADER_SIZE;
    request->flags =
        KSWORD_ARK_FILE_IRP_FLAG_UI_CONFIRMED |
        KSWORD_ARK_FILE_IRP_FLAG_OPEN_REPARSE_POINT;
    if (isDirectory) {
        request->flags |= KSWORD_ARK_FILE_IRP_FLAG_DIRECTORY_INTENT;
    }
    request->confirmationToken = KSWORD_ARK_FILE_IRP_CONFIRMATION_TOKEN;
    request->majorFunction = IRP_MJ_SET_INFORMATION;
    request->targetLayer = KSWORD_ARK_FILE_IRP_LAYER_BASE_FS; // CREATE 和删除均使用现有基础文件系统目标层。
    request->timeoutMs = KSWORD_ARK_FILE_IRP_DEFAULT_TIMEOUT_MS;
    // 删除只需 DELETE；Ex 的忽略只读位不要求增加属性写权限，避免 CREATE 共享冲突。
    request->desiredAccess = DELETE | SYNCHRONIZE;
    request->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    request->createDisposition = FILE_OPEN;
    request->createOptions = isDirectory ? 0UL : FILE_NON_DIRECTORY_FILE;
    request->fileAttributes = FILE_ATTRIBUTE_NORMAL;
    request->informationClass = FileDispositionInformation;
    request->inputBytes = (ULONG)sizeof(dispositionInformation);
    request->pathLengthChars = pathLengthChars;
    RtlCopyMemory(
        request->path,
        pathText,
        (SIZE_T)pathLengthChars * sizeof(WCHAR));
    request->path[pathLengthChars] = L'\0';

    RtlZeroMemory(&dispositionInformation, sizeof(dispositionInformation));
    dispositionInformation.DeleteFile = TRUE;
    transportStatus = KswordARKDriverSubmitFileDeleteIrp( // 仅删除预设启用强制 section 清空与 Ex/传统类兼容。
        response,
        responseBytes,
        request,
        &dispositionInformation,
        (ULONG)sizeof(dispositionInformation),
        &bytesWritten);
    if (!NT_SUCCESS(transportStatus)) {
        resultStatus = transportStatus;
        goto Exit;
    }
    if (bytesWritten < KSWORD_ARK_FILE_IRP_SUBMIT_RESPONSE_HEADER_SIZE) {
        resultStatus = STATUS_INFO_LENGTH_MISMATCH;
        goto Exit;
    }
    if (Details != NULL) {
        if ((response->stageFlags & KSWORD_ARK_FILE_IRP_STAGE_CREATE) != 0UL) {
            Details->openStatus = response->createStatus;
        }
        if ((response->stageFlags & KSWORD_ARK_FILE_IRP_STAGE_OPERATION) != 0UL) {
            Details->dispositionStatus = response->operationStatus;
        }
    }
    if (!NT_SUCCESS(response->createStatus)) {
        resultStatus = response->createStatus;
        goto Exit;
    }
    if ((response->stageFlags & KSWORD_ARK_FILE_IRP_STAGE_OPERATION) == 0UL) {
        resultStatus = response->operationStatus;
        goto Exit;
    }
    if (!NT_SUCCESS(response->operationStatus)) {
        resultStatus = response->operationStatus;
        goto Exit;
    }
    if ((response->stageFlags & KSWORD_ARK_FILE_IRP_STAGE_CLEANUP) != 0UL &&
        !NT_SUCCESS(response->cleanupStatus)) {
        resultStatus = response->cleanupStatus;
        goto Exit;
    }
    if ((response->stageFlags & KSWORD_ARK_FILE_IRP_STAGE_CLOSE) != 0UL &&
        !NT_SUCCESS(response->closeStatus)) {
        resultStatus = response->closeStatus;
        goto Exit;
    }

    resultStatus = STATUS_SUCCESS;

Exit:
    if (response != NULL) {
        ExFreePoolWithTag(response, 'fOsK');
    }
    if (request != NULL) {
        ExFreePoolWithTag(request, 'fOsK');
    }
    return resultStatus;
}

static BOOLEAN
KswordARKDriverQueryFileIndex(
    _In_ HANDLE FileHandle,
    _Out_ PLARGE_INTEGER Index
    )
{
    FILE_INTERNAL_INFORMATION information;
    IO_STATUS_BLOCK ioStatusBlock;
    RtlZeroMemory(&information, sizeof(information));
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    if (!NT_SUCCESS(ZwQueryInformationFile(
            FileHandle, &ioStatusBlock, &information,
            sizeof(information), FileInternalInformation))) {
        return FALSE;
    }
    *Index = information.IndexNumber;
    return TRUE;
}

static NTSTATUS
KswordARKDriverVerifyDeleteAfterClose(
    _In_ PUNICODE_STRING Path,
    _Inout_opt_ KSWORD_ARK_DELETE_PATH_RESPONSE* Details
    )
{
    OBJECT_ATTRIBUTES attributes;
    FILE_NETWORK_OPEN_INFORMATION pathInformation; // 使用 WDK 声明的路径属性查询输出。
    NTSTATUS status;

    InitializeObjectAttributes(&attributes, Path,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    RtlZeroMemory(&pathInformation, sizeof(pathInformation)); // 清空完整路径属性结构。
    status = ZwQueryFullAttributesFile(&attributes, &pathInformation); // 用通用 DDI 复核路径是否仍存在。
    if (Details != NULL) {
        Details->verifyStatus = status;
    }
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND) {
        if (Details != NULL) {
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_REMOVED;
        }
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        if (Details != NULL) {
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_NOT_VERIFIED;
        }
        return STATUS_UNSUCCESSFUL;
    }

    // The selected path must be absent. A new file at the same path is still
    // visible and therefore cannot be reported as a successful path removal.
    if (Details != NULL) {
        Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_STILL_VISIBLE;
    }
    return STATUS_CANNOT_DELETE;
}

static NTSTATUS
KswordARKDriverOpenPosixDeleteHandle(
    _In_ POBJECT_ATTRIBUTES Attributes,
    _In_ BOOLEAN IgnoreShare,
    _In_ BOOLEAN WriteAttributes,
    _In_ BOOLEAN IsDirectory,
    _Out_ PHANDLE FileHandle
    )
{
    IO_STATUS_BLOCK ioStatusBlock;
    const ACCESS_MASK access = DELETE | SYNCHRONIZE |
        (WriteAttributes ? FILE_WRITE_ATTRIBUTES : 0UL);
    const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT |
        FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT |
        (IsDirectory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    if (IgnoreShare) {
        UNICODE_STRING routineName;
        KSWORD_ARK_IO_CREATE_FILE_EX_FN createFileEx;
        RtlInitUnicodeString(&routineName, L"IoCreateFileEx");
        createFileEx = (KSWORD_ARK_IO_CREATE_FILE_EX_FN)
            MmGetSystemRoutineAddress(&routineName);
        if (createFileEx == NULL) {
            return STATUS_NOT_SUPPORTED;
        }
        return createFileEx(FileHandle, access, Attributes, &ioStatusBlock,
            NULL, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            FILE_OPEN, options, NULL, 0U, CreateFileTypeNone, NULL,
            IO_IGNORE_SHARE_ACCESS_CHECK, NULL);
    }
    return ZwCreateFile(FileHandle, access, Attributes, &ioStatusBlock,
        NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN, options, NULL, 0U);
}

static NTSTATUS
KswordARKDriverDeletePathPosix(
    _In_ PUNICODE_STRING Path,
    _In_ BOOLEAN IsDirectory,
    _In_ BOOLEAN IgnoreShare,
    _Inout_opt_ KSWORD_ARK_DELETE_PATH_RESPONSE* Details
    )
{
    OBJECT_ATTRIBUTES attributes;
    FILE_NETWORK_OPEN_INFORMATION pathInformation; // 只读重试沿用 WDK 支持的路径属性查询。
    HANDLE fileHandle = NULL;
    HANDLE retryHandle = NULL;
    LARGE_INTEGER originalIndex = { 0 };
    LARGE_INTEGER retryIndex;
    BOOLEAN indexValid;
    NTSTATUS status;

    InitializeObjectAttributes(&attributes, Path,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = KswordARKDriverOpenPosixDeleteHandle(
        &attributes, IgnoreShare, FALSE, IsDirectory, &fileHandle);
    if (Details != NULL) {
        Details->openStatus = status;
    }
    if (!NT_SUCCESS(status)) {
        if (Details != NULL) {
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_FAILED;
        }
        return status;
    }
    indexValid = KswordARKDriverQueryFileIndex(fileHandle, &originalIndex);
    status = KswordARKDriverDeleteFileWithDispositionEx(
        fileHandle, FALSE, !IgnoreShare);
    if (IgnoreShare && KswordARKDriverShouldRetryDeleteByIrp(status)) { // 打开成功后的过滤层拒绝要在同一对象上重试。
        status = KswordARKDriverDeleteHandleByIrp(fileHandle, FALSE); // 不按路径重开，保留原文件身份和忽略共享打开语义。
    }
    if (Details != NULL) {
        Details->dispositionStatus = status;
    }

    if (status == STATUS_CANNOT_DELETE || status == STATUS_ACCESS_DENIED) {
        RtlZeroMemory(&pathInformation, sizeof(pathInformation)); // 清空按路径查询的属性输出。
        if (NT_SUCCESS(ZwQueryFullAttributesFile(&attributes, &pathInformation)) && // 使用通用 DDI 查询只读位。
            (pathInformation.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0UL && // 仅在仍为只读时重开属性写入句柄。
            indexValid) {
            NTSTATUS retryStatus = KswordARKDriverOpenPosixDeleteHandle(
                &attributes, IgnoreShare, TRUE, IsDirectory, &retryHandle);
            if (Details != NULL) {
                Details->openStatus = retryStatus;
            }
            if (NT_SUCCESS(retryStatus)) {
                if (KswordARKDriverQueryFileIndex(retryHandle, &retryIndex) &&
                    retryIndex.QuadPart == originalIndex.QuadPart) {
                    status = KswordARKDriverDeleteFileWithDispositionEx(
                        retryHandle, TRUE, !IgnoreShare);
                    if (IgnoreShare && KswordARKDriverShouldRetryDeleteByIrp(status)) { // 只读重试同样需要绕过 SET_INFORMATION 过滤层。
                        status = KswordARKDriverDeleteHandleByIrp(retryHandle, TRUE); // 沿用已经复核身份的句柄并请求忽略只读位。
                    }
                    if ((KswordARKDriverIsDispositionExUnsupportedStatus(status) || // 旧文件系统未支持 Ex 时仍可清理只读位。
                        (IgnoreShare && (status == STATUS_CANNOT_DELETE || status == STATUS_ACCESS_DENIED))) && // 基础层传统类也可能被只读位阻碍。
                        NT_SUCCESS(KswordARKDriverNormalizeReadOnlyAttribute(retryHandle))) {
                        status = KswordARKDriverDeleteFileWithDispositionEx(
                            retryHandle, FALSE, !IgnoreShare);
                        if (IgnoreShare && KswordARKDriverShouldRetryDeleteByIrp(status)) { // 旧文件系统归一化属性后再尝试基础层删除。
                            status = KswordARKDriverDeleteHandleByIrp(retryHandle, FALSE); // 同一重试句柄保持文件身份不变。
                        }
                    }
                    if (Details != NULL) {
                        Details->dispositionStatus = status;
                    }
                }
                else {
                    status = STATUS_FILE_INVALID;
                }
                ZwClose(retryHandle);
            }
            else {
                status = retryStatus;
            }
        }
    }
    ZwClose(fileHandle);
    if (!NT_SUCCESS(status)) {
        if (Details != NULL) {
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_FAILED;
        }
        return status;
    }
    return KswordARKDriverVerifyDeleteAfterClose(Path, Details);
}

NTSTATUS
KswordARKDriverDeletePath(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory
    )
{
    return KswordARKDriverDeletePathWithFlags(
        pathText,
        pathLengthChars,
        isDirectory,
        0UL);
}

NTSTATUS
KswordARKDriverDeletePathWithFlags(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory,
    _In_ ULONG deleteFlags
    )
{
    return KswordARKDriverDeletePathWithDetails(
        pathText, pathLengthChars, isDirectory, deleteFlags, NULL);
}

NTSTATUS
KswordARKDriverDeletePathWithDetails(
    _In_reads_(pathLengthChars) PCWSTR pathText,
    _In_ USHORT pathLengthChars,
    _In_ BOOLEAN isDirectory,
    _In_ ULONG deleteFlags,
    _Inout_opt_ KSWORD_ARK_DELETE_PATH_RESPONSE* Details
    )
/*++

Routine Description:

    按 deleteFlags 选择底层 Zw*、自建 IRP 或 POSIX unlink 后端删除单一 NT 路径。
    目录必须已为空；递归后序调度由 file_delete_recursive.c 统一处理。

Arguments:

    pathText - Target NT path, for example \??\C:\Temp\a.txt.
    pathLengthChars - Character length excluding trailing null.
    isDirectory - TRUE when target should be opened as directory.
    deleteFlags - KSWORD_ARK_DELETE_PATH_FLAG_BACKEND_MASK 中的互斥后端标志。

Return Value:

    NTSTATUS

--*/
{
    UNICODE_STRING targetPath;
    OBJECT_ATTRIBUTES objectAttributes;
    IO_STATUS_BLOCK ioStatusBlock;
    HANDLE fileHandle = NULL;
    FILE_DISPOSITION_INFORMATION dispositionInformation;
    ACCESS_MASK desiredAccess;
    ULONG createOptions;
    NTSTATUS status;
    NTSTATUS firstDeleteStatus = STATUS_SUCCESS;

    if (Details != NULL) {
        Details->openStatus = STATUS_NOT_SUPPORTED;
        Details->dispositionStatus = STATUS_NOT_SUPPORTED;
        Details->verifyStatus = STATUS_NOT_SUPPORTED;
        Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_UNKNOWN;
    }

    if (pathText == NULL || pathLengthChars == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((deleteFlags & (~KSWORD_ARK_DELETE_PATH_FLAG_BACKEND_MASK)) != 0UL ||
        (deleteFlags != 0UL && (deleteFlags & (deleteFlags - 1UL)) != 0UL)) {
        return STATUS_INVALID_PARAMETER;
    }

    if ((deleteFlags & KSWORD_ARK_DELETE_PATH_FLAG_BACKEND_IRP) != 0UL) {
        status = KswordARKDriverDeletePathByIrp(
            pathText, pathLengthChars, isDirectory, Details);
        if (Details != NULL) {
            if (!NT_SUCCESS(status)) {
                Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_FAILED;
            }
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
        RtlInitUnicodeString(&targetPath, pathText);
        return KswordARKDriverVerifyDeleteAfterClose(&targetPath, Details);
    }

    RtlInitUnicodeString(&targetPath, pathText);
    if (targetPath.Length == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (targetPath.Length != (USHORT)(pathLengthChars * sizeof(WCHAR))) {
        return STATUS_INVALID_PARAMETER;
    }

    if ((deleteFlags & (KSWORD_ARK_DELETE_PATH_FLAG_BACKEND_POSIX |
            KSWORD_ARK_DELETE_PATH_FLAG_BACKEND_IGNORE_SHARE_POSIX)) != 0UL) {
        return KswordARKDriverDeletePathPosix(
            &targetPath, isDirectory,
            (deleteFlags & KSWORD_ARK_DELETE_PATH_FLAG_BACKEND_IGNORE_SHARE_POSIX) != 0UL,
            Details);
    }

    InitializeObjectAttributes(
        &objectAttributes,
        &targetPath,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    desiredAccess = DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    createOptions = FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT;
    if (isDirectory) {
        createOptions |= FILE_DIRECTORY_FILE;
    }
    else {
        createOptions |= FILE_NON_DIRECTORY_FILE;
    }

    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwCreateFile(
        &fileHandle,
        desiredAccess,
        &objectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_OPEN,
        createOptions,
        NULL,
        0U);
    if (Details != NULL) {
        Details->openStatus = status;
        if (!NT_SUCCESS(status)) {
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_FAILED;
        }
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KswordARKDriverNormalizeReadOnlyAttribute(fileHandle);
    if (!NT_SUCCESS(status) && status != STATUS_INVALID_PARAMETER) {
        if (Details != NULL) {
            Details->dispositionStatus = status;
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_FAILED;
        }
        ZwClose(fileHandle);
        return status;
    }

    dispositionInformation.DeleteFile = TRUE;
    RtlZeroMemory(&ioStatusBlock, sizeof(ioStatusBlock));
    status = ZwSetInformationFile(
        fileHandle,
        &ioStatusBlock,
        &dispositionInformation,
        (ULONG)sizeof(dispositionInformation),
        FileDispositionInformation);
    firstDeleteStatus = status;

    if (!NT_SUCCESS(status) &&
        KswordARKDriverShouldRetryDeleteWithDispositionEx(status, isDirectory)) {
        NTSTATUS fallbackStatus = KswordARKDriverDeleteFileWithDispositionEx(
            fileHandle, TRUE, TRUE);
        if (!NT_SUCCESS(fallbackStatus) &&
            (fallbackStatus == STATUS_CANNOT_DELETE || fallbackStatus == STATUS_USER_MAPPED_FILE)) {
            const NTSTATUS flushStatus = KswordARKDriverFlushImageSectionForDelete(fileHandle);
            if (NT_SUCCESS(flushStatus)) {
                fallbackStatus = KswordARKDriverDeleteFileWithDispositionEx(
                    fileHandle, TRUE, TRUE);
            }
            else if (fallbackStatus == STATUS_CANNOT_DELETE) {
                fallbackStatus = flushStatus;
            }
        }
        if (NT_SUCCESS(fallbackStatus)) {
            status = fallbackStatus;
        }
        else if (fallbackStatus != STATUS_INVALID_INFO_CLASS &&
            fallbackStatus != STATUS_NOT_SUPPORTED &&
            fallbackStatus != STATUS_INVALID_PARAMETER) {
            // Return explicit fallback failure; keep first status for unsupported cases.
            status = fallbackStatus;
        }
        else {
            status = firstDeleteStatus;
        }
    }

    if (Details != NULL) {
        Details->dispositionStatus = status;
        if (!NT_SUCCESS(status)) {
            Details->outcome = KSWORD_ARK_DELETE_PATH_OUTCOME_FAILED;
        }
    }
    ZwClose(fileHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return KswordARKDriverVerifyDeleteAfterClose(&targetPath, Details);
}
