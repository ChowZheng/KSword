// 离线执行生产删除逻辑；kernel API 由有界模拟替换，不加载驱动或删除真实文件。
#include <ntifs.h>
#include <stdio.h>
#include <stdlib.h> // 提交回放的池分配用用户态堆模拟。
#include "ark/ark_file_irp.h"

static FILE_OBJECT testFile;
static SECTION_OBJECT_POINTERS testSections;
static POBJECT_TYPE testFileType;
static KIRQL testIrql;
static NTSTATUS referenceStatus;
static NTSTATUS responses[8];
static BOOLEAN cancellations[8];
static ULONG callClasses[8];
static ULONG callFlags[8];
static BOOLEAN callHadSections[8];
static ULONG calls;
static ULONG references;
static ULONG dereferences;
static ULONG flushes;
static BOOLEAN flushResult;
static ULONG failures;

static KIRQL TestIrql(VOID) { return testIrql; }
static NTSTATUS TestReference(HANDLE Handle, ACCESS_MASK Access, POBJECT_TYPE Type,
    KPROCESSOR_MODE Mode, PVOID* Object, POBJECT_HANDLE_INFORMATION Info)
{
    UNREFERENCED_PARAMETER(Handle); UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Mode); UNREFERENCED_PARAMETER(Info);
    ++references;
    if (Access != DELETE) { ++failures; }
    if (NT_SUCCESS(referenceStatus)) { *Object = &testFile; }
    return referenceStatus;
}
static VOID TestDereference(PVOID Object)
{
    if (Object != &testFile) { ++failures; }
    ++dereferences;
}
static BOOLEAN TestFlush(PSECTION_OBJECT_POINTERS Sections, MMFLUSH_TYPE Type)
{
    if (Sections != &testSections || Type != MmFlushForDelete) { ++failures; }
    ++flushes;
    return flushResult;
}
static NTSTATUS TestDisposition(PFILE_OBJECT File, ULONG Class, const void* Input,
    ULONG Bytes, PBOOLEAN Cancelled)
{
    ULONG index = calls++;
    if (index >= RTL_NUMBER_OF(responses) || File != &testFile) { ++failures; return STATUS_DATA_ERROR; }
    callClasses[index] = Class;
    callHadSections[index] = testSections.ImageSectionObject != NULL ||
        testSections.DataSectionObject != NULL || testSections.SharedCacheMap != NULL;
    if (Class == 64UL && Bytes == sizeof(ULONG)) { RtlCopyMemory(&callFlags[index], Input, Bytes); }
    else if (Class == (ULONG)FileDispositionInformation && Bytes == sizeof(FILE_DISPOSITION_INFORMATION)) {
        callFlags[index] = ((const FILE_DISPOSITION_INFORMATION*)Input)->DeleteFile;
    } else { ++failures; }
    *Cancelled = cancellations[index];
    return responses[index];
}

#undef KeGetCurrentIrql
#undef ObDereferenceObject
#define KeGetCurrentIrql TestIrql
#define ObReferenceObjectByHandle TestReference
#define ObDereferenceObject TestDereference
#define IoFileObjectType (&testFileType)
#define MmFlushImageSection TestFlush
#define KswordARKDriverSetDispositionByIrp TestDisposition
#include "../src/features/file/file_delete_irp.c"

static VOID Reset(VOID)
{
    RtlZeroMemory(&testFile, sizeof(testFile));
    RtlZeroMemory(&testSections, sizeof(testSections));
    RtlZeroMemory(responses, sizeof(responses));
    RtlZeroMemory(cancellations, sizeof(cancellations));
    RtlZeroMemory(callClasses, sizeof(callClasses));
    RtlZeroMemory(callFlags, sizeof(callFlags));
    RtlZeroMemory(callHadSections, sizeof(callHadSections));
    testFile.SectionObjectPointer = &testSections;
    testSections.ImageSectionObject = (PVOID)(ULONG_PTR)1U;
    testSections.DataSectionObject = (PVOID)(ULONG_PTR)2U;
    testSections.SharedCacheMap = (PVOID)(ULONG_PTR)3U;
    testIrql = PASSIVE_LEVEL; referenceStatus = STATUS_SUCCESS;
    calls = references = dereferences = flushes = 0UL; flushResult = FALSE;
}
static VOID Expect(const char* Name, BOOLEAN Passed)
{
    printf("%s %s\n", Passed ? "PASS" : "FAIL", Name);
    if (!Passed) { ++failures; }
}
static NTSTATUS Delete(BOOLEAN IgnoreReadOnly)
{
    return KswordARKDriverDeleteHandleByIrp((HANDLE)(ULONG_PTR)1U, IgnoreReadOnly);
}

static int TestDeletionAlgorithm(void) // 保留原有删除算法回归，新增提交路由回归另行执行。
{
    NTSTATUS status;
    Reset();
    status = Delete(FALSE);
    Expect("Ex success keeps sections and balances references", status == STATUS_SUCCESS && calls == 1UL &&
        callClasses[0] == 64UL && callFlags[0] == 3UL && callHadSections[0] && references == 1UL &&
        dereferences == 1UL && flushes == 0UL && testSections.SharedCacheMap != NULL);

    Reset(); responses[0] = STATUS_ACCESS_DENIED;
    status = Delete(TRUE);
    Expect("denied deletion clears all three before same-object retry even when MM refuses", status == STATUS_SUCCESS &&
        calls == 2UL && callHadSections[0] && !callHadSections[1] && callFlags[1] == 19UL && flushes == 1UL &&
        testFile.SectionObjectPointer == &testSections && testSections.ImageSectionObject == NULL &&
        testSections.DataSectionObject == NULL && testSections.SharedCacheMap == NULL && dereferences == 1UL);

    Reset(); responses[0] = STATUS_INVALID_INFO_CLASS;
    status = Delete(FALSE);
    Expect("unsupported Ex uses legacy on the same object", status == STATUS_SUCCESS && calls == 2UL &&
        callClasses[1] == (ULONG)FileDispositionInformation && callFlags[1] == 1UL && flushes == 0UL && callHadSections[1]);

    Reset(); responses[0] = STATUS_NOT_SUPPORTED; responses[1] = STATUS_CANNOT_DELETE;
    responses[2] = STATUS_INVALID_PARAMETER;
    status = Delete(FALSE);
    Expect("legacy denial clears sections and retries Ex then legacy", status == STATUS_SUCCESS && calls == 4UL &&
        callClasses[3] == (ULONG)FileDispositionInformation && !callHadSections[2] && !callHadSections[3] && flushes == 1UL);

    Reset(); responses[0] = STATUS_SHARING_VIOLATION; responses[1] = STATUS_ACCESS_DENIED;
    status = Delete(FALSE);
    Expect("retry failure preserved without a third attempt", status == STATUS_ACCESS_DENIED && calls == 2UL && dereferences == 1UL);

    Reset(); responses[0] = STATUS_CANCELLED; cancellations[0] = TRUE;
    status = Delete(FALSE);
    Expect("cancelled operation does not clear or retry", status == STATUS_CANCELLED && calls == 1UL && flushes == 0UL && callHadSections[0]);

    Reset(); responses[0] = STATUS_INVALID_PARAMETER; cancellations[0] = TRUE;
    status = Delete(FALSE);
    Expect("cancelled unsupported Ex does not fall back", status == STATUS_INVALID_PARAMETER && calls == 1UL && flushes == 0UL);

    Reset(); responses[0] = STATUS_SUCCESS; cancellations[0] = TRUE;
    status = Delete(FALSE);
    Expect("completion winning cancellation preserves success", status == STATUS_SUCCESS && calls == 1UL && dereferences == 1UL);

    Reset(); responses[0] = STATUS_INSUFFICIENT_RESOURCES;
    status = Delete(FALSE);
    Expect("resource failure does not clear or retry", status == STATUS_INSUFFICIENT_RESOURCES && calls == 1UL && flushes == 0UL);

    Reset(); referenceStatus = STATUS_INVALID_HANDLE;
    status = Delete(FALSE);
    Expect("invalid handle never dereferences or sends", status == STATUS_INVALID_HANDLE && calls == 0UL && references == 1UL && dereferences == 0UL);

    Reset(); testIrql = APC_LEVEL;
    status = Delete(FALSE);
    Expect("non-passive call rejected before referencing", status == STATUS_INVALID_DEVICE_STATE && references == 0UL && calls == 0UL);

    Reset(); testFile.SectionObjectPointer = NULL; responses[0] = STATUS_USER_MAPPED_FILE;
    status = Delete(FALSE);
    Expect("null section structure is tolerated", status == STATUS_SUCCESS && calls == 2UL && flushes == 0UL && testFile.SectionObjectPointer == NULL);

    Reset();
    status = KswordARKDriverDeleteFileObjectByIrp(NULL, FALSE, &cancellations[0]);
    Expect("null file object is rejected", status == STATUS_INVALID_DEVICE_STATE && calls == 0UL);
    status = KswordARKDriverDeleteFileObjectByIrp(&testFile, FALSE, NULL);
    Expect("missing cancel output is rejected", status == STATUS_INVALID_PARAMETER && calls == 0UL);

    Reset();
    Expect("retry predicate excludes success and path/resource errors", !KswordARKDriverShouldRetryDeleteByIrp(STATUS_SUCCESS) &&
        !KswordARKDriverShouldRetryDeleteByIrp(STATUS_OBJECT_NAME_NOT_FOUND) &&
        !KswordARKDriverShouldRetryDeleteByIrp(STATUS_INSUFFICIENT_RESOURCES) &&
        KswordARKDriverShouldRetryDeleteByIrp(STATUS_ACCESS_DENIED) &&
        KswordARKDriverShouldRetryDeleteByIrp(STATUS_INVALID_INFO_CLASS));
    printf("failures=%lu\n", failures);
    return failures != 0UL;
}

// 类型和提交函数由脚本从生产文件原样提取，防止测试维护另一套打开路由。
#include "file_irp_target_replay.h"
static DEVICE_OBJECT testRelatedDevice; // 正常打开的栈顶与删除目标必须可区分。
static DEVICE_OBJECT testBaseDevice; // 基础文件系统模拟对象。
static ULONG managedOpens, manualOpens, closedTargets, genericOperations; // 记录跨阶段行为。
static ULONG openedOptions; // 观察目录和重解析点选项是否保留。
static ACCESS_MASK openedAccess; // 观察打开权限是否被无谓扩大。
static NTSTATUS openResult; // 模拟打开阶段失败。
static BOOLEAN closedManagedObject; // 观察同一托管对象的配对收尾。

static PVOID KswordArkFileIrpAllocate(SIZE_T Bytes) { return calloc(1U, Bytes); } // 模拟零初始化非分页池。
static VOID KswordArkFileIrpFree(PVOID Buffer) { free(Buffer); } // 配对模拟池释放。
static NTSTATUS KswordArkFileIrpOpenManaged(PCWSTR Path, USHORT Chars, ACCESS_MASK Access,
    ULONG Share, ULONG Disposition, ULONG Options, ULONG Attributes, PKSWORD_ARK_FILE_IRP_TARGET Target)
{
    UNREFERENCED_PARAMETER(Path); UNREFERENCED_PARAMETER(Chars); UNREFERENCED_PARAMETER(Share); // 路径不访问真实磁盘。
    UNREFERENCED_PARAMETER(Disposition); UNREFERENCED_PARAMETER(Attributes); // 其余 CREATE 参数不由模拟解释。
    ++managedOpens; openedAccess = Access; openedOptions = Options; // 保存实际生产路由选择。
    if (!NT_SUCCESS(openResult)) { return openResult; } // 失败不能产生可用对象。
    Target->FileObject = &testFile; Target->FileHandle = (HANDLE)(ULONG_PTR)1U; // 模拟正常打开对象及句柄。
    Target->RelatedDevice = &testRelatedDevice; Target->BaseFsDevice = &testBaseDevice; // 区分打开与删除层。
    Target->Manual = FALSE; return STATUS_SUCCESS; // 托管对象不得手工 CLOSE。
}
static NTSTATUS KswordArkFileIrpOpenManual(PCWSTR Path, USHORT Chars, ACCESS_MASK Access,
    ULONG Share, ULONG Disposition, ULONG Options, ULONG Attributes, ULONG Layer,
    ULONG Timeout, PKSWORD_ARK_FILE_IRP_TARGET Target)
{
    NTSTATUS status; // 手工路由在模拟中仍返回 CREATE 成功，重现旧错误的前提。
    UNREFERENCED_PARAMETER(Layer); UNREFERENCED_PARAMETER(Timeout); // 不向真实设备发送请求。
    status = KswordArkFileIrpOpenManaged(Path, Chars, Access, Share, Disposition, Options, Attributes, Target); // 复用对象准备。
    --managedOpens; ++manualOpens; Target->Manual = TRUE; return status; // 记录实际选择了手工 CREATE。
}
static BOOLEAN KswordArkFileIrpRequestHasWriteSemantics(const KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST* Request)
{ return Request->majorFunction == IRP_MJ_SET_INFORMATION; } // 本回放只提交 SET_INFORMATION。
static BOOLEAN KswordArkFileIrpMajorIsDangerous(ULONG Major)
{ UNREFERENCED_PARAMETER(Major); return FALSE; } // 不提交 PnP/电源类 IRP。
static PDEVICE_OBJECT KswordArkFileIrpSelectLayer(PKSWORD_ARK_FILE_IRP_TARGET Target, ULONG Layer, PULONG Resolved)
{ *Resolved = Layer; return Layer == KSWORD_ARK_FILE_IRP_LAYER_BASE_FS ? Target->BaseFsDevice : Target->RelatedDevice; } // 观察实际删除层。
static VOID KswordArkFileIrpCopyObjectName(PVOID Object, PWCHAR Name, ULONG Capacity, PULONG Chars)
{ UNREFERENCED_PARAMETER(Object); UNREFERENCED_PARAMETER(Capacity); Name[0] = L'\0'; *Chars = 0UL; } // 不查询真实内核名。
static NTSTATUS KswordArkFileIrpExecuteOperation(const KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST* Request,
    PKSWORD_ARK_FILE_IRP_TARGET Target, const void* Input, ULONG InputBytes, ULONG Timeout,
    PVOID Output, ULONG Capacity, PULONG Bytes, PULONGLONG Information, PBOOLEAN Cancelled)
{
    UNREFERENCED_PARAMETER(Request); UNREFERENCED_PARAMETER(Target); UNREFERENCED_PARAMETER(Input); // 通用路径只记录调用。
    UNREFERENCED_PARAMETER(InputBytes); UNREFERENCED_PARAMETER(Timeout); UNREFERENCED_PARAMETER(Output); // 不访问真实设备。
    UNREFERENCED_PARAMETER(Capacity); ++genericOperations; *Bytes = 0UL; *Information = 0ULL; *Cancelled = FALSE; // 返回有界结果。
    return STATUS_SUCCESS; // 通用提交的路径和强制入口应当分开。
}
static VOID KswordArkFileIrpCloseTarget(PKSWORD_ARK_FILE_IRP_TARGET Target, ULONG Timeout,
    NTSTATUS* Cleanup, NTSTATUS* Close, PULONG Stages)
{
    UNREFERENCED_PARAMETER(Timeout); ++closedTargets; // 每次打开必须只收尾一次。
    closedManagedObject = !Target->Manual && Target->FileObject == &testFile && Target->FileHandle != NULL; // 核验原托管对象。
    *Cleanup = *Close = STATUS_SUCCESS; *Stages |= KSWORD_ARK_FILE_IRP_STAGE_CLOSE; // 模拟句柄关闭回执。
    Target->FileObject = NULL; Target->FileHandle = NULL; // 收尾完成后不保留借用对象。
}
#include "file_irp_submit_replay.h"

static KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST submitRequest; // 大协议结构放静态存储，避免测试栈膨胀。
static KSWORD_ARK_FILE_IRP_SUBMIT_RESPONSE submitResponse; // 接收真实生产提交函数的回执。
static VOID ResetSubmit(VOID)
{
    Reset(); RtlZeroMemory(&submitRequest, sizeof(submitRequest)); // 重置算法与协议输入。
    managedOpens = manualOpens = closedTargets = genericOperations = 0UL; openResult = STATUS_SUCCESS; // 重置阶段记录。
    openedAccess = openedOptions = 0UL; closedManagedObject = FALSE; // 清除前次观察值。
    submitRequest.majorFunction = IRP_MJ_SET_INFORMATION; submitRequest.targetLayer = KSWORD_ARK_FILE_IRP_LAYER_BASE_FS; // 删除预设。
    submitRequest.informationClass = FileDispositionInformation; submitRequest.desiredAccess = DELETE | SYNCHRONIZE; // 最小删除权限。
    submitRequest.flags = KSWORD_ARK_FILE_IRP_FLAG_UI_CONFIRMED | KSWORD_ARK_FILE_IRP_FLAG_OPEN_REPARSE_POINT; // 保留链接语义。
    submitRequest.confirmationToken = KSWORD_ARK_FILE_IRP_CONFIRMATION_TOKEN; submitRequest.pathLengthChars = 8U; // 满足确认前提。
    submitRequest.createOptions = FILE_NON_DIRECTORY_FILE; // 默认普通文件。
}
static VOID Submit(BOOLEAN Force)
{
    FILE_DISPOSITION_INFORMATION disposition = { TRUE }; size_t bytes = 0U; NTSTATUS status; // 输入只含删除标记。
    if (Force) { status = KswordARKDriverSubmitFileDeleteIrp(&submitResponse, sizeof(submitResponse), &submitRequest,
        &disposition, sizeof(disposition), &bytes); } // 执行真实内部删除入口。
    else { status = KswordARKDriverSubmitFileIrp(&submitResponse, sizeof(submitResponse), &submitRequest,
        &disposition, sizeof(disposition), &bytes); } // 执行真实通用入口，核验兼容行为。
    if (status != STATUS_SUCCESS || bytes < KSWORD_ARK_FILE_IRP_SUBMIT_RESPONSE_HEADER_SIZE) { ++failures; } // 回执必须完整。
}
int main(void)
{
    (VOID)TestDeletionAlgorithm(); // 先执行既有 15 项。
    ResetSubmit(); Submit(TRUE); // 普通文件必须正常打开再直发删除。
    Expect("delete preset uses managed open and closes the same object", managedOpens == 1UL && manualOpens == 0UL &&
        calls == 1UL && genericOperations == 0UL && closedTargets == 1UL && closedManagedObject &&
        submitResponse.createStatus == STATUS_SUCCESS && submitResponse.operationStatus == STATUS_SUCCESS &&
        submitResponse.targetDeviceAddress == (ULONGLONG)(ULONG_PTR)&testBaseDevice &&
        openedAccess == (DELETE | SYNCHRONIZE) && (openedOptions & FILE_OPEN_REPARSE_POINT) != 0UL);
    ResetSubmit(); submitRequest.flags |= KSWORD_ARK_FILE_IRP_FLAG_DIRECTORY_INTENT; submitRequest.createOptions = 0UL; Submit(TRUE); // 目录使用同一路由。
    Expect("directory delete keeps directory and reparse options", managedOpens == 1UL && manualOpens == 0UL &&
        (openedOptions & FILE_DIRECTORY_FILE) != 0UL && (openedOptions & FILE_NON_DIRECTORY_FILE) == 0UL && closedManagedObject);
    ResetSubmit(); openResult = STATUS_SHARING_VIOLATION; Submit(TRUE); // 占用导致打开失败时不重复手工打开。
    Expect("managed open failure preserved without manual fallback", submitResponse.createStatus == STATUS_SHARING_VIOLATION &&
        submitResponse.status == KSWORD_ARK_FILE_IRP_STATUS_OPEN_FAILED && calls == 0UL && manualOpens == 0UL && closedTargets == 0UL);
    ResetSubmit(); responses[0] = responses[1] = STATUS_INVALID_PARAMETER; Submit(TRUE); // 删除阶段失败仍然关闭对象。
    Expect("disposition failure closes managed object and preserves status", submitResponse.operationStatus == STATUS_INVALID_PARAMETER &&
        closedTargets == 1UL && closedManagedObject && flushes == 0UL);
    ResetSubmit(); Submit(FALSE); // 通用 BASE_FS 构造器保留显式手工 CREATE 选择。
    Expect("generic base filesystem submission retains manual open", manualOpens == 1UL && managedOpens == 0UL &&
        genericOperations == 1UL && calls == 0UL && closedTargets == 1UL);
    ResetSubmit(); submitRequest.targetLayer = KSWORD_ARK_FILE_IRP_LAYER_RELATED; Submit(FALSE); // 通用栈顶打开不变。
    Expect("generic related submission retains managed open", managedOpens == 1UL && manualOpens == 0UL && genericOperations == 1UL);
    ResetSubmit(); submitRequest.flags = 0UL; Submit(TRUE); // 新路由仍要求原确认令牌。
    Expect("delete confirmation gate precedes any open", submitResponse.status == KSWORD_ARK_FILE_IRP_STATUS_CONFIRMATION_REQUIRED &&
        managedOpens == 0UL && manualOpens == 0UL && calls == 0UL);
    printf("total failures=%lu\n", failures); return failures != 0UL; // 合并两组回归结果。
}
