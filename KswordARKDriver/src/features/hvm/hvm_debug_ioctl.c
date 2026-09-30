#include "hvm_debug.h"
/* Derive ownership from the original IRP even when WDF dispatches on a worker. */
NTKERNELAPI ULONG IoGetRequestorProcessId(PIRP Irp);

/* WDF owns validation and completion; the debugger module owns the transaction. */
NTSTATUS KswordARKHvmIoctlDebug(WDFDEVICE Device, WDFREQUEST Request,
    size_t InputBufferLength, size_t OutputBufferLength, size_t* BytesReturned)
{
    /* Preserve METHOD_BUFFERED input before the response overwrites it. */
    KSWORD_ARK_HVM_DEBUG_REQUEST snapshot;
    /* Bind input and output only after WDF validates their capacities. */
    PVOID input = NULL, output = NULL;
    /* Preserve WDF-reported buffer lengths. */
    size_t inputBytes = 0U, outputBytes = 0U;
    /* Preserve the exact validation or policy outcome. */
    NTSTATUS status;
    /* Reject an incomplete dispatcher completion contract. */
    if (BytesReturned == NULL) { return STATUS_INVALID_PARAMETER; }
    /* Every rejected request starts with zero completed bytes. */
    *BytesReturned = 0U;
    /* Retrieve the complete fixed debugger request. */
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(snapshot), &input, &inputBytes);
    /* Reject truncation before reading any request field. */
    if (!NT_SUCCESS(status)) { return status; }
    /* Validate both dispatcher and WDF capacities. */
    if (InputBufferLength != sizeof(snapshot) || inputBytes < sizeof(snapshot)) { return STATUS_INFO_LENGTH_MISMATCH; }
    /* Preserve the exact validated caller input. */
    RtlCopyMemory(&snapshot, input, sizeof(snapshot));
    /* Retrieve the fixed protocol response. */
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KSWORD_ARK_HVM_DEBUG_RESPONSE), &output, &outputBytes);
    /* Reject output retrieval failure before touching its storage. */
    if (!NT_SUCCESS(status)) { return status; }
    /* Require one complete debugger response. */
    if (OutputBufferLength < sizeof(KSWORD_ARK_HVM_DEBUG_RESPONSE) || outputBytes < sizeof(KSWORD_ARK_HVM_DEBUG_RESPONSE)) { return STATUS_BUFFER_TOO_SMALL; }
    /* Keep new EPT mutations inside the existing central safety policy. */
    if (snapshot.operation == KSWORD_ARK_HVM_DEBUG_ADD || snapshot.operation == KSWORD_ARK_HVM_DEBUG_REMOVE) {
        /* Initialize central policy evidence before publishing any permissions. */
        KSWORD_ARK_SAFETY_CONTEXT safety = { 0 };
        /* Use the same operation class as ordinary EPT rules. */
        safety.Operation = KSWORD_ARK_SAFETY_OPERATION_KERNEL_PATCH;
        /* Preserve the user's debugger-backend confirmation. */
        safety.ContextFlags = (snapshot.flags & KSWORD_ARK_HVM_DEBUG_FLAG_CONFIRMED) != 0UL ? KSWORD_ARK_SAFETY_CONTEXT_FLAG_UI_CONFIRMED : 0UL;
        /* Describe the concrete transaction for the policy audit. */
        safety.TargetText = L"Thread-scoped EPT debugger stop and pinned backing page";
        /* Publish the exact bounded text size. */
        safety.TargetTextChars = (USHORT)(RTL_NUMBER_OF(L"Thread-scoped EPT debugger stop and pinned backing page") - 1U);
        /* Apply policy before entering the mutation implementation. */
        status = KswordARKSafetyEvaluate(Device, &safety);
        /* Preserve a central-policy refusal without weakening confirmation. */
        if (!NT_SUCCESS(status)) { return status; }
    }
    /* Bind ownership to the authoritative IRP requestor rather than a supplied PID. */
    status = KswordARKHvmDebugControl(&snapshot, (KSWORD_ARK_HVM_DEBUG_RESPONSE*)output,
        IoGetRequestorProcessId(WdfRequestWdmGetIrp(Request)));
    /* Publish completion only after the protocol packet is initialized. */
    if (NT_SUCCESS(status)) { *BytesReturned = sizeof(KSWORD_ARK_HVM_DEBUG_RESPONSE); }
    /* Return the transport-level completion status. */
    return status;
}
