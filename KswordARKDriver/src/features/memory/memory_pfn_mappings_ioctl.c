// Read-only bounded VA->PFN batches, using the existing physical PTE walker.
#include "ark/ark_driver.h"
#include "ark/ark_memory.h"
#include "driver/KswordArkPfnIoctl.h"
#include "../../dispatch/ioctl_validation.h"
#include "../../platform/pool_compat.h"

// Export declarations match the existing process-protection and PTE modules.
NTKERNELAPI LONGLONG NTAPI PsGetProcessCreateTimeQuadPart(_In_ PEPROCESS Process);
NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId(_In_ HANDLE ProcessId, _Outptr_ PEPROCESS* Process);

NTSTATUS KswordARKMemoryIoctlQueryPfnMappings(
    WDFDEVICE Device, WDFREQUEST Request, size_t InputBufferLength,
    size_t OutputBufferLength, size_t* BytesReturned)
{
    PVOID input = NULL; // Retrieved WDF input only.
    PVOID output = NULL; // Retrieved WDF output only.
    size_t actual = 0; // Buffer length returned by WDF.
    KSWORD_ARK_PFN_MAPPING_REQUEST* copy = NULL; // Avoid a large kernel stack request.
    KSWORD_ARK_PFN_MAPPING_RESPONSE* response = NULL; // Normalized output.
    PEPROCESS process = NULL; // Retain the process identity for this batch.
    NTSTATUS status; // Preserve the exact failed operation.
    ULONGLONG started; // Time budget bounds each device call.
    UNREFERENCED_PARAMETER(Device); // No device state mutation.
    *BytesReturned = 0; // No partial packet on validation failure.
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { return STATUS_INVALID_DEVICE_STATE; } // Walker may attach.
    if (InputBufferLength != sizeof(*copy) || OutputBufferLength < sizeof(*response)) {
        return STATUS_INFO_LENGTH_MISMATCH; // Fixed versioned ABI only.
    }
    status = KswordARKValidateDeviceIoControlWriteAccess(Request); // Enforce driver access policy.
    if (!NT_SUCCESS(status)) { return status; } // Do not inspect unauthorized processes.
    status = KswordARKRetrieveRequiredInputBuffer(Request, sizeof(*copy), &input, &actual); // Validate input.
    if (!NT_SUCCESS(status)) { return status; } // No access to failed buffer.
    copy = KswordARKAllocateNonPagedPool(sizeof(*copy), 'mPsK'); // Stable copy for aliased METHOD_BUFFERED input.
    if (copy == NULL) { return STATUS_INSUFFICIENT_RESOURCES; } // No unbounded fallback.
    RtlCopyMemory(copy, input, sizeof(*copy)); // Snapshot request before writing output.
    if (copy->version != KSWORD_ARK_PFN_VERSION || copy->reserved != 0 ||
        copy->count == 0 || copy->count > KSWORD_ARK_PFN_MAX_MAPPINGS || copy->createTime == 0) {
        status = STATUS_INVALID_PARAMETER; // Reject malformed batches and missing identity.
        goto Cleanup; // Always release the request copy.
    }
    status = PsLookupProcessByProcessId(ULongToHandle(copy->processId), &process); // Reference exact current process.
    if (!NT_SUCCESS(status)) { goto Cleanup; } // Process may have exited.
    if ((ULONGLONG)PsGetProcessCreateTimeQuadPart(process) != copy->createTime) {
        status = STATUS_INVALID_CID; // Reused PID must not be interpreted as the original process.
        goto Cleanup; // Preserve ownership boundary.
    }
    status = KswordARKRetrieveRequiredOutputBuffer(Request, sizeof(*response), &output, &actual); // Validate output.
    if (!NT_SUCCESS(status)) { goto Cleanup; } // Avoid an invalid output access.
    response = (KSWORD_ARK_PFN_MAPPING_RESPONSE*)output; // Output no longer aliases live input.
    RtlZeroMemory(response, sizeof(*response)); // Prevent uninitialized kernel data exposure.
    response->version = KSWORD_ARK_PFN_VERSION; // Caller validates the response version.
    started = KeQueryInterruptTime(); // Bound total batch work to approximately 50 ms.
    for (ULONG i = 0; i < copy->count; ++i) { // No more than 256 page-table walks.
        KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_REQUEST query = { 0 }; // Existing read-only protocol.
        KSWORD_ARK_TRANSLATE_VIRTUAL_ADDRESS_RESPONSE translated = { 0 }; // Fixed small walker output.
        size_t written = 0; // Exact walker result extent.
        KSWORD_ARK_PFN_MAPPING* row = &response->entries[i]; // Fixed validated output slot.
        row->pfn = ~0ULL; // Failed mappings cannot become PFN zero.
        row->status = STATUS_INVALID_ADDRESS; // Default for disallowed addresses.
        if (copy->addresses[i] <= (ULONGLONG)(ULONG_PTR)MmHighestUserAddress) { // User mappings only.
            query.processId = copy->processId; // Identity has been pinned above.
            query.virtualAddress = copy->addresses[i]; // Only a read-only page-table walk.
            row->status = KswordARKDriverTranslateVirtualAddress(&translated, sizeof(translated), &query, &written); // Existing validated walker.
            if (NT_SUCCESS(row->status) && written == sizeof(translated) && translated.info.resolved != 0) {
                row->pfn = translated.info.physicalAddress >> PAGE_SHIFT; // One physical page identity.
                row->pageSize = translated.info.pageSize; // Large-page geometry is explicit evidence.
            } else if (NT_SUCCESS(row->status)) { row->status = STATUS_NOT_FOUND; } // Unresolved is not PFN zero.
        }
        response->count = i + 1; // Publish only processed entries.
        if (KeQueryInterruptTime() - started >= 500000ULL) { break; } // Caller resumes the unprocessed suffix.
    }
    // Recheck current PID identity after walking; discard a batch crossing process exit/reuse.
    {
        PEPROCESS current = NULL; // Temporary reference for the final identity check.
        status = PsLookupProcessByProcessId(ULongToHandle(copy->processId), &current); // Re-resolve current PID.
        if (NT_SUCCESS(status)) { // Only compare valid referenced objects.
            if (current != process) { status = STATUS_INVALID_CID; } // Reject replacement process evidence.
            ObDereferenceObject(current); // Release final-check reference.
        }
    }
    if (NT_SUCCESS(status)) { *BytesReturned = sizeof(*response); } // Full initialized packet, explicit processed count.
Cleanup:
    if (process != NULL) { ObDereferenceObject(process); } // Release retained process.
    ExFreePoolWithTag(copy, 'mPsK'); // Release immutable input copy.
    return status; // Surface lookup, validation, and reuse failures.
}
