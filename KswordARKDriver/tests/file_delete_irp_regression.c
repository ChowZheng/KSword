// 离线执行生产删除逻辑；kernel API 由有界模拟替换，不加载驱动或删除真实文件。
#include <ntifs.h>
#include <stdio.h>
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

int main(void)
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
