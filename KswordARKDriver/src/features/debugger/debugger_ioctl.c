#include "debugger.h"
#include "../../platform/pool_compat.h"

/* Keep WDF transport validation separate from the debugger's process/thread work. */
NTSTATUS KswordARKDebuggerIoctlControl(WDFDEVICE Device, WDFREQUEST Request,
    size_t InputBufferLength, size_t OutputBufferLength, size_t* BytesReturned)
{
    /* Keep the large shared request off the bounded kernel stack. */
    KSWORD_ARK_DEBUGGER_REQUEST* snapshot = NULL;
    /* Obtain both METHOD_BUFFERED views before writing a response. */
    PVOID input = NULL, output = NULL;
    /* Preserve actual WDF capacities for exact validation. */
    size_t inputBytes = 0U, outputBytes = 0U;
    /* Preserve the transport completion status. */
    NTSTATUS status;
    /* Reject an incomplete dispatcher contract. */
    if (BytesReturned == NULL) { return STATUS_INVALID_PARAMETER; }
    /* Failed validation never publishes output bytes. */
    *BytesReturned = 0U;
    /* Require the one exact shared request. */
    if (InputBufferLength != sizeof(KSWORD_ARK_DEBUGGER_REQUEST)) { return STATUS_INFO_LENGTH_MISMATCH; }
    /* Validate input storage before accessing it. */
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(KSWORD_ARK_DEBUGGER_REQUEST), &input, &inputBytes);
    /* Preserve WDF's validation failure. */
    if (!NT_SUCCESS(status)) { return status; }
    /* Require a complete typed output packet. */
    if (OutputBufferLength < sizeof(KSWORD_ARK_DEBUGGER_RESPONSE)) { return STATUS_BUFFER_TOO_SMALL; }
    /* Validate output storage before accessing it. */
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KSWORD_ARK_DEBUGGER_RESPONSE), &output, &outputBytes);
    /* Preserve WDF's validation failure. */
    if (!NT_SUCCESS(status)) { return status; }
    /* Snapshot input before METHOD_BUFFERED output overwrites the same storage. */
    snapshot = (KSWORD_ARK_DEBUGGER_REQUEST*)KswordARKAllocateNonPagedPool(sizeof(*snapshot), 'gdDK');
    /* Report bounded allocation failure without executing an operation. */
    if (snapshot == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Preserve the exact validated input. */
    RtlCopyMemory(snapshot, input, sizeof(*snapshot));
    /* Execute the feature's authoritative user-process operation. */
    status = KswordARKDebuggerControl(Device, snapshot, (KSWORD_ARK_DEBUGGER_RESPONSE*)output);
    /* Release the request snapshot on every operation outcome. */
    ExFreePoolWithTag(snapshot, 'gdDK');
    /* Return a typed packet only when transport completion succeeded. */
    if (NT_SUCCESS(status)) { *BytesReturned = sizeof(KSWORD_ARK_DEBUGGER_RESPONSE); }
    /* Preserve transport failure separately from Response.status. */
    return status;
}
