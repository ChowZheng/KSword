/* Remote user-handle close. Object addresses are comparison values, never pointers to trust. */
#include "ark/ark_driver.h"
#include "../../platform/process_resolver.h"

/* Export declarations match the existing enumeration and process-identity paths. */
NTKERNELAPI LONGLONG NTAPI PsGetProcessCreateTimeQuadPart(_In_ PEPROCESS Process);
NTKERNELAPI NTSTATUS NTAPI PsAcquireProcessExitSynchronization(_In_ PEPROCESS Process);
NTKERNELAPI VOID NTAPI PsReleaseProcessExitSynchronization(_In_ PEPROCESS Process);
NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId(_In_ HANDLE ProcessId, _Outptr_ PEPROCESS* Process);
NTKERNELAPI VOID KeStackAttachProcess(_Inout_ PVOID Process, _Out_ PVOID ApcState);
NTKERNELAPI VOID KeUnstackDetachProcess(_In_ PVOID ApcState);

/* Caller validates the packet and safety policy; this routine owns all acquired resources. */
NTSTATUS
KswordARKDriverCloseHandle(
    _In_ const KSWORD_ARK_CLOSE_HANDLE_REQUEST* Request,
    _Out_ NTSTATUS* ResumeStatus
    )
{
    PEPROCESS process = NULL; /* Referenced owner, independent of subsequent PID reuse. */
    PVOID object = NULL; /* Pin the expected object while validating and closing. */
    DECLSPEC_ALIGN(16) UCHAR attachState[128] = { 0 }; /* Same attach ABI as handle enumeration. */
    KSWORD_PS_SUSPEND_PROCESS_FN suspendProcess = NULL; /* Resolve before changing target state. */
    KSWORD_PS_RESUME_PROCESS_FN resumeProcess = NULL; /* Recovery must be available before suspend. */
    BOOLEAN attached = FALSE; /* Exception cleanup tracks the active address-space attachment. */
    NTSTATUS status = STATUS_SUCCESS; /* Preserve the close/identity failure independently of resume. */

    *ResumeStatus = STATUS_SUCCESS; /* No recovery is needed unless suspend succeeds. */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { /* Suspension and object close require passive execution. */
        return STATUS_INVALID_DEVICE_STATE; /* Refuse the request before acquiring resources. */
    }
    suspendProcess = KswordARKDriverResolvePsSuspendProcess(); /* Reuse the repository's native resolver. */
    resumeProcess = KswordARKDriverResolvePsResumeProcess(); /* Resolve the matched recovery routine. */
    if (suspendProcess == NULL || resumeProcess == NULL) { /* Never suspend without a recovery path. */
        return STATUS_NOT_SUPPORTED; /* Leave the handle and process unchanged. */
    }
    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &process); /* Pin the owner. */
    if (!NT_SUCCESS(status)) { /* An exited/unknown PID cannot identify a target. */
        return status; /* No resources were acquired on lookup failure. */
    }
    if ((ULONG64)PsGetProcessCreateTimeQuadPart(process) != Request->expectedCreateTime100ns) { /* Reject reused PIDs. */
        ObDereferenceObject(process); /* Release the mismatched owner. */
        return STATUS_INVALID_CID; /* Do not touch its handle table. */
    }
    status = PsAcquireProcessExitSynchronization(process); /* Keep the table alive until recovery completes. */
    if (!NT_SUCCESS(status)) { /* A process already exiting cannot be suspended safely. */
        ObDereferenceObject(process); /* Drop the lookup reference. */
        return status; /* Leave its handles unchanged. */
    }
    status = suspendProcess(process); /* Quiesce owner threads to narrow handle-value reuse races. */
    if (NT_SUCCESS(status)) { /* Pair exactly one successful suspension with one resume. */
        __try { /* Capture object-manager exceptions without leaving the owner suspended. */
            KeStackAttachProcess(process, attachState); /* Resolve user handles in their owning table. */
            attached = TRUE; /* Remember attachment for the exception path. */
            status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)Request->handleValue,
                0, NULL, UserMode, &object, NULL); /* Never accept kernel or pseudo handles as user identities. */
            if (NT_SUCCESS(status)) { /* Only a live reference may be compared with the captured address. */
                if ((ULONG64)(ULONG_PTR)object != Request->expectedObjectAddress) { /* Reject stale/reused handles. */
                    status = STATUS_INVALID_HANDLE; /* Do not close the newly assigned object. */
                }
                else { /* The referenced object matches the enumerated row. */
                    status = ObCloseHandle((HANDLE)(ULONG_PTR)Request->handleValue, UserMode); /* Honor protect-from-close. */
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* Convert exceptions into the response's close status. */
            status = GetExceptionCode(); /* Preserve the native failure for the client. */
        }
        if (attached) { /* Detach before releasing references or resuming the target. */
            KeUnstackDetachProcess(attachState); /* Restore the caller's address space. */
        }
        if (object != NULL) { /* An object reference can survive both close and exceptions. */
            ObDereferenceObject(object); /* Release exactly the reference acquired above. */
        }
        *ResumeStatus = resumeProcess(process); /* Report recovery separately even when close failed. */
    }
    PsReleaseProcessExitSynchronization(process); /* Allow process teardown after recovery. */
    ObDereferenceObject(process); /* Release the pinned owner on every completed path. */
    return status; /* Return the actual mutation result. */
}
