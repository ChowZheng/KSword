#include "debugger.h"
#include "../hvm/hvm_runtime.h"
#include "../hvm/hvm_ept_view.h"
#include "../../platform/pool_compat.h"
#define KSW_SHADOW_MEM_IMAGE 0x01000000UL
NTKERNELAPI VOID KeStackAttachProcess(PVOID Process, PVOID State);
NTKERNELAPI VOID KeUnstackDetachProcess(PVOID State);

/* Every shadow retains backing, target identity and the real WDF requestor. */
typedef struct _KSW_DEBUG_SHADOW {
    ULONG OwnerId, TargetId, ViewId;
    PEPROCESS Owner, Target;
    PMDL Mdl;
    PVOID Mapping;
    ULONGLONG VirtualPage, PhysicalPage;
    UCHAR Breakpoints[PAGE_SIZE / 8];
    UCHAR PatchMask[PAGE_SIZE / 8];
    UCHAR PatchBytes[PAGE_SIZE];
} KSW_DEBUG_SHADOW;
static KSW_DEBUG_SHADOW g_Shadow[KSWORD_ARK_HVM_MAX_VIEWS];
typedef struct _KSW_DEBUG_QUARANTINE { PEPROCESS Owner; ULONG OwnerId, TargetId; } KSW_DEBUG_QUARANTINE;
static KSW_DEBUG_QUARANTINE g_Quarantine[KSWORD_ARK_HVM_MAX_VIEWS];
static EX_PUSH_LOCK g_ShadowLock;
NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId(HANDLE Id, PEPROCESS* Process);
NTKERNELAPI NTSTATUS NTAPI PsAcquireProcessExitSynchronization(PEPROCESS Process);
NTKERNELAPI VOID NTAPI PsReleaseProcessExitSynchronization(PEPROCESS Process);
NTKERNELAPI NTSTATUS ObOpenObjectByPointer(PVOID Object, ULONG Attributes, PACCESS_STATE AccessState,
    ACCESS_MASK Access, POBJECT_TYPE Type, KPROCESSOR_MODE Mode, PHANDLE Handle);
NTSYSAPI NTSTATUS NTAPI ZwQueryVirtualMemory(HANDLE Process, PVOID Base, ULONG InformationClass,
    PVOID Information, SIZE_T Bytes, PSIZE_T Returned);
typedef struct _KSW_SHADOW_BASIC_INFORMATION {
    PVOID BaseAddress, AllocationBase;
    ULONG AllocationProtect;
    SIZE_T RegionSize;
    ULONG State, Protect, Type;
} KSW_SHADOW_BASIC_INFORMATION;

static NTSTATUS ShadowValidateViewsLocked(ULONG OwnerProcessId)
{
    ULONG index, offset;
    NTSTATUS status = STATUS_SUCCESS;
    for (index = 0; index < RTL_NUMBER_OF(g_Quarantine); ++index) {
        if (g_Quarantine[index].Owner != NULL && (OwnerProcessId == 0UL || g_Quarantine[index].OwnerId == OwnerProcessId)) {
            return STATUS_INVALID_DEVICE_STATE;
        }
    }
    for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) {
        const KSW_DEBUG_SHADOW* page = &g_Shadow[index];
        if (page->Mdl == NULL || page->ViewId != 0UL || (OwnerProcessId != 0UL && page->OwnerId != OwnerProcessId)) { continue; }
        for (offset = 0; offset < sizeof(page->Breakpoints); ++offset) {
            if ((page->Breakpoints[offset] | page->PatchMask[offset]) != 0U) { status = STATUS_INVALID_DEVICE_STATE; break; }
        }
        if (!NT_SUCCESS(status)) { break; }
    }
    return status;
}

NTSTATUS KswordARKDebuggerShadowValidateViews(ULONG OwnerProcessId)
{
    NTSTATUS status;
    KeEnterCriticalRegion(); KswordARKAcquirePushLockShared(&g_ShadowLock);
    status = ShadowValidateViewsLocked(OwnerProcessId);
    KswordARKReleasePushLockShared(&g_ShadowLock); KeLeaveCriticalRegion();
    return status;
}

NTSTATUS KswordARKDebuggerShadowAcquireStartLease(VOID)
{
    NTSTATUS status;
    KeEnterCriticalRegion(); KswordARKAcquirePushLockShared(&g_ShadowLock);
    status = ShadowValidateViewsLocked(0UL);
    if (!NT_SUCCESS(status)) { KswordARKReleasePushLockShared(&g_ShadowLock); KeLeaveCriticalRegion(); }
    return status;
}

VOID KswordARKDebuggerShadowReleaseStartLease(VOID)
{
    KswordARKReleasePushLockShared(&g_ShadowLock); KeLeaveCriticalRegion();
}

static VOID ShadowClearQuarantine(PEPROCESS Owner, ULONG TargetId)
{
    ULONG index;
    for (index = 0; index < RTL_NUMBER_OF(g_Quarantine); ++index) {
        if (g_Quarantine[index].Owner == Owner && g_Quarantine[index].TargetId == TargetId) {
            ObDereferenceObject(g_Quarantine[index].Owner); RtlZeroMemory(&g_Quarantine[index], sizeof(g_Quarantine[index]));
        }
    }
}

static VOID ShadowFree(KSW_DEBUG_SHADOW* Page)
{
    if (Page->Mdl != NULL) { MmUnlockPages(Page->Mdl); IoFreeMdl(Page->Mdl); }
    if (Page->Target != NULL) { ObDereferenceObject(Page->Target); }
    if (Page->Owner != NULL) { ObDereferenceObject(Page->Owner); }
    RtlZeroMemory(Page, sizeof(*Page));
}

static NTSTATUS ShadowRemoveView(KSW_DEBUG_SHADOW* Page)
{
    KSWORD_ARK_HVM_VIEW_REQUEST* request;
    KSWORD_ARK_HVM_VIEW_RESPONSE response;
    NTSTATUS status;
    if (Page->ViewId == 0UL) { return STATUS_SUCCESS; }
    request = (KSWORD_ARK_HVM_VIEW_REQUEST*)KswordARKAllocateNonPagedPool(sizeof(*request), 'sSDK');
    if (request == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(request, sizeof(*request));
    request->version = KSWORD_ARK_HVM_VIEW_PROTOCOL_VERSION; request->size = sizeof(*request);
    request->operation = KSWORD_ARK_HVM_VIEW_OP_REMOVE; request->viewId = Page->ViewId;
    request->flags = KSWORD_ARK_HVM_VIEW_FLAG_UI_CONFIRMED;
    request->confirmationToken = KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN;
    status = KswordARKHvmEptViewControl(request, &response);
    ExFreePoolWithTag(request, 'sSDK');
    if (NT_SUCCESS(status)) { status = response.lastStatus; }
    if (NT_SUCCESS(status) || status == STATUS_NOT_FOUND) { Page->ViewId = 0UL; return STATUS_SUCCESS; }
    return status;
}

static NTSTATUS ShadowInstall(KSW_DEBUG_SHADOW* Page)
{
    KSWORD_ARK_HVM_VIEW_REQUEST* request;
    KSWORD_ARK_HVM_VIEW_RESPONSE response;
    NTSTATUS status;
    ULONG offset;
    request = (KSWORD_ARK_HVM_VIEW_REQUEST*)KswordARKAllocateNonPagedPool(sizeof(*request), 'sSDK');
    if (request == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(request, sizeof(*request));
    request->version = KSWORD_ARK_HVM_VIEW_PROTOCOL_VERSION; request->size = sizeof(*request);
    request->operation = KSWORD_ARK_HVM_VIEW_OP_ADD; request->kind = KSWORD_ARK_HVM_VIEW_KIND_HOOK;
    request->flags = KSWORD_ARK_HVM_VIEW_FLAG_UI_CONFIRMED;
    request->confirmationToken = KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN;
    request->physicalAddress = Page->PhysicalPage;
    RtlCopyMemory(request->shadow, Page->Mapping, PAGE_SIZE);
    for (offset = 0; offset < PAGE_SIZE; ++offset) {
        if ((Page->PatchMask[offset / 8] & (1U << (offset % 8))) != 0U) { request->shadow[offset] = Page->PatchBytes[offset]; }
        if ((Page->Breakpoints[offset / 8] & (1U << (offset % 8))) != 0U) { request->shadow[offset] = 0xCC; }
    }
    status = KswordARKHvmEptViewControl(request, &response);
    ExFreePoolWithTag(request, 'sSDK');
    if (NT_SUCCESS(status)) { status = response.lastStatus; }
    if (NT_SUCCESS(status)) { Page->ViewId = response.viewId; }
    return status;
}

/* A failed update restores both masks before re-installing the previous view.
 * The original mapping is never written, including when patches overlap INT3. */
static NTSTATUS ShadowUpdate(KSW_DEBUG_SHADOW* Page, ULONG Operation, ULONG Offset, ULONG Bytes,
    const UCHAR* Data, BOOLEAN NewPage)
{
    KSW_DEBUG_SHADOW* saved;
    ULONG index;
    BOOLEAN empty = TRUE;
    NTSTATUS status, rollback;
    saved = (KSW_DEBUG_SHADOW*)KswordARKAllocateNonPagedPool(sizeof(*saved), 'bSDK');
    if (saved == NULL) { if (NewPage) { ShadowFree(Page); } return STATUS_INSUFFICIENT_RESOURCES; }
    RtlCopyMemory(saved, Page, sizeof(*saved));
    for (index = Offset; index < Offset + Bytes; ++index) {
        UCHAR bit = (UCHAR)(1U << (index % 8));
        if (Operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD) { Page->Breakpoints[index / 8] |= bit; }
        else if (Operation == KSWORD_ARK_DEBUGGER_SHADOW_REMOVE) { Page->Breakpoints[index / 8] &= (UCHAR)~bit; }
        else if (Operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE) {
            Page->PatchMask[index / 8] |= bit; Page->PatchBytes[index] = Data[index - Offset];
        } else { Page->PatchMask[index / 8] &= (UCHAR)~bit; }
    }
    status = ShadowRemoveView(Page);
    if (NT_SUCCESS(status)) {
        for (index = 0; index < sizeof(Page->Breakpoints); ++index) {
            if ((Page->Breakpoints[index] | Page->PatchMask[index]) != 0U) { empty = FALSE; break; }
        }
        if (!empty) { status = ShadowInstall(Page); }
    }
    if (!NT_SUCCESS(status)) {
        RtlCopyMemory(Page->Breakpoints, saved->Breakpoints, sizeof(Page->Breakpoints));
        RtlCopyMemory(Page->PatchMask, saved->PatchMask, sizeof(Page->PatchMask));
        RtlCopyMemory(Page->PatchBytes, saved->PatchBytes, sizeof(Page->PatchBytes));
        if (!NewPage && Page->ViewId == 0UL) {
            rollback = ShadowInstall(Page);
            if (!NT_SUCCESS(rollback)) { status = rollback; }
        } else if (NewPage && Page->ViewId == 0UL) { ShadowFree(Page); }
    } else if (empty) { ShadowFree(Page); }
    ExFreePoolWithTag(saved, 'bSDK');
    return status;
}

/* Execution shadows cannot implement a data freeze: ordinary loads continue
 * reading the original page. Require a committed executable user page. */
static NTSTATUS ShadowExecutable(PEPROCESS Process, ULONGLONG Address, ULONGLONG* Evidence)
{
    HANDLE handle = NULL;
    KSW_SHADOW_BASIC_INFORMATION basic;
    SIZE_T returned = 0;
    NTSTATUS status = ObOpenObjectByPointer(Process, OBJ_KERNEL_HANDLE, NULL,
        0x0400UL, *PsProcessType, KernelMode, &handle);
    if (!NT_SUCCESS(status)) { return status; }
    status = ZwQueryVirtualMemory(handle, (PVOID)(ULONG_PTR)Address, 0UL,
        &basic, sizeof(basic), &returned);
    if (NT_SUCCESS(status) && (returned < sizeof(basic) || basic.State != MEM_COMMIT ||
        (basic.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0UL ||
        (basic.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0UL)) {
        status = STATUS_NOT_SUPPORTED;
    }
    /* VirtualQuery still reports MEM_IMAGE/MEM_MAPPED after a page becomes
     * private through COW. Both code patches and INT3 use the pinned working-
     * set Shared proof below; the allocation type alone is not that proof.
     * EPT views select physical pages, not a PID/CR3. */
    if (NT_SUCCESS(status) && basic.Type != MEM_PRIVATE &&
        basic.Type != KSW_SHADOW_MEM_IMAGE && basic.Type != MEM_MAPPED) { status = STATUS_NOT_SUPPORTED; }
    if (NT_SUCCESS(status)) {
        struct { PVOID Address; ULONG_PTR Attributes; } workingSet;
        workingSet.Address = (PVOID)(ULONG_PTR)Address; workingSet.Attributes = 0;
        status = ZwQueryVirtualMemory(handle, NULL, 4UL, &workingSet, sizeof(workingSet), &returned);
        if (NT_SUCCESS(status)) {
            if (returned < sizeof(workingSet) || (workingSet.Attributes & 1ULL) == 0ULL) {
                *Evidence = KSWORD_ARK_DEBUGGER_SHADOW_BACKING_UNVERIFIABLE; status = STATUS_NOT_SUPPORTED;
            } else if ((workingSet.Attributes & (1ULL << 15)) != 0ULL) {
                *Evidence = KSWORD_ARK_DEBUGGER_SHADOW_BACKING_SHARED; status = STATUS_NOT_SUPPORTED;
            }
        }
    }
    (void)ZwClose(handle);
    return status;
}

/* Re-probe the current virtual mapping while its page is pinned. An old MDL
 * alone does not prove that a reused virtual address still names that PFN. */
static NTSTATUS ShadowValidateBacking(KSW_DEBUG_SHADOW* Page, ULONGLONG Address, ULONGLONG* Evidence)
{
    PMDL current = IoAllocateMdl((PVOID)(ULONG_PTR)Page->VirtualPage, PAGE_SIZE, FALSE, FALSE, NULL);
    ULONG_PTR attach[16] = { 0 };
    NTSTATUS status;
    if (current == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    KeStackAttachProcess(Page->Target, attach);
    __try { MmProbeAndLockPages(current, UserMode, IoReadAccess); status = STATUS_SUCCESS; }
    __except (EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
    KeUnstackDetachProcess(attach);
    if (NT_SUCCESS(status)) {
        if (((ULONGLONG)MmGetMdlPfnArray(current)[0] << PAGE_SHIFT) != Page->PhysicalPage) {
            *Evidence = KSWORD_ARK_DEBUGGER_SHADOW_MAPPING_CHANGED; status = STATUS_INVALID_ADDRESS;
        } else { status = ShadowExecutable(Page->Target, Address, Evidence); }
        MmUnlockPages(current);
    }
    IoFreeMdl(current);
    return status;
}

NTSTATUS KswordARKDebuggerShadow(ULONG OwnerProcessId, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response)
{
    KSW_DEBUG_SHADOW* page = NULL;
    KSW_DEBUG_SHADOW* freePage = NULL;
    PEPROCESS owner = NULL, target = NULL;
    ULONGLONG base = Request->address & ~0xFFFULL;
    ULONG index, offset = (ULONG)(Request->address & 0xFFFULL);
    BOOLEAN newPage = FALSE;
    BOOLEAN ownerLocked = FALSE, targetLocked = FALSE;
    BOOLEAN restoreAll = Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_RESTORE &&
        Request->address == 0ULL && Request->bytes == 0ULL;
    BOOLEAN quarantine = Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_QUARANTINE;
    NTSTATUS status;
    if (OwnerProcessId <= 4UL || Request->processId <= 4UL || Request->processId == OwnerProcessId ||
        (quarantine && (Request->address != 0ULL || Request->bytes != 0ULL)) ||
        (!restoreAll && !quarantine && (Request->address == 0ULL || base > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress - PAGE_SIZE ||
            Request->bytes == 0ULL || Request->bytes > PAGE_SIZE - offset)) ||
        ((Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD || Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_REMOVE) && Request->bytes != 1ULL) ||
        (Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE && Request->bytes > KSWORD_ARK_DEBUGGER_CONTEXT_BYTES) ||
        (Request->flags & KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED) == 0UL) { return STATUS_INVALID_PARAMETER; }
    status = PsLookupProcessByProcessId(ULongToHandle(OwnerProcessId), &owner);
    if (!NT_SUCCESS(status)) { return status; }
    if (Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_RESTORE && Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_REMOVE && !quarantine) {
        status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &target);
        if (!NT_SUCCESS(status)) { ObDereferenceObject(owner); return status; }
    }
    /* Serialize publication against process rundown before taking the page
     * registry lock. The exit callback cannot run ahead of an unpublished pin. */
    status = PsAcquireProcessExitSynchronization(owner);
    if (!NT_SUCCESS(status)) { goto ReleaseObjects; }
    ownerLocked = TRUE;
    if (target != NULL) {
        status = PsAcquireProcessExitSynchronization(target);
        if (!NT_SUCCESS(status)) { goto ReleaseObjects; }
        targetLocked = TRUE;
    }
    KeEnterCriticalRegion(); KswordARKAcquirePushLockExclusive(&g_ShadowLock);
    if (PsGetProcessExitStatus(owner) != STATUS_PENDING || (target != NULL && PsGetProcessExitStatus(target) != STATUS_PENDING)) {
        status = STATUS_PROCESS_IS_TERMINATING; goto Complete;
    }
    if (quarantine) {
        KSW_DEBUG_QUARANTINE* freeEntry = NULL;
        for (index = 0; index < RTL_NUMBER_OF(g_Quarantine); ++index) {
            if (g_Quarantine[index].Owner == owner && g_Quarantine[index].TargetId == Request->processId) { status = STATUS_SUCCESS; goto Complete; }
            if (g_Quarantine[index].Owner == NULL && freeEntry == NULL) { freeEntry = &g_Quarantine[index]; }
        }
        if (freeEntry == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Complete; }
        ObReferenceObject(owner); freeEntry->Owner = owner; freeEntry->OwnerId = OwnerProcessId; freeEntry->TargetId = Request->processId;
        status = STATUS_SUCCESS; goto Complete;
    }
    if (restoreAll) {
        status = STATUS_SUCCESS;
        for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) {
            KSW_DEBUG_SHADOW* current = &g_Shadow[index];
            if (current->Mdl != NULL && current->Owner == owner && current->TargetId == Request->processId) {
                status = ShadowUpdate(current, Request->operation, 0, PAGE_SIZE, NULL, FALSE);
                if (!NT_SUCCESS(status)) { break; }
            }
        }
        if (NT_SUCCESS(status)) { ShadowClearQuarantine(owner, Request->processId); }
        goto Complete;
    }
    for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) {
        if (g_Shadow[index].Mdl == NULL && freePage == NULL) { freePage = &g_Shadow[index]; }
        if (g_Shadow[index].Owner == owner && g_Shadow[index].TargetId == Request->processId &&
            (target == NULL || g_Shadow[index].Target == target) && g_Shadow[index].VirtualPage == base) { page = &g_Shadow[index]; }
    }
    if (page == NULL) {
        /* Opaque, naturally aligned storage, matching the existing HVM pin path. */
        ULONG_PTR attach[16] = { 0 };
        if (Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_ADD && Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_WRITE) { status = STATUS_NOT_FOUND; goto Complete; }
        if (freePage == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Complete; }
        page = freePage; newPage = TRUE;
        page->Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)base, PAGE_SIZE, FALSE, FALSE, NULL);
        if (page->Mdl == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Complete; }
        KeStackAttachProcess(target, attach);
        __try { MmProbeAndLockPages(page->Mdl, UserMode, IoReadAccess); status = STATUS_SUCCESS; }
        __except (EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
        KeUnstackDetachProcess(attach);
        if (!NT_SUCCESS(status)) { IoFreeMdl(page->Mdl); page->Mdl = NULL; goto Complete; }
        page->Owner = owner; page->Target = target;
        ObReferenceObject(owner); ObReferenceObject(target);
        page->OwnerId = OwnerProcessId; page->TargetId = Request->processId;
        page->VirtualPage = base; page->PhysicalPage = (ULONGLONG)MmGetMdlPfnArray(page->Mdl)[0] << PAGE_SHIFT;
        page->Mapping = MmGetSystemAddressForMdlSafe(page->Mdl, NormalPagePriority | MdlMappingNoExecute);
        if (page->Mapping == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; ShadowFree(page); goto Complete; }
    }
    if (Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_WRITE || Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD) {
        status = ShadowValidateBacking(page, Request->address, &Response->reserved1);
        if (!NT_SUCCESS(status) || PsGetProcessExitStatus(page->Owner) != STATUS_PENDING || PsGetProcessExitStatus(page->Target) != STATUS_PENDING) {
            if (NT_SUCCESS(status)) { status = STATUS_PROCESS_IS_TERMINATING; }
            if (newPage) { ShadowFree(page); }
            goto Complete;
        }
    }
    status = ShadowUpdate(page, Request->operation, offset, (ULONG)Request->bytes, Request->context, newPage);
    if (!NT_SUCCESS(status)) { goto Complete; }
    Response->address = page->PhysicalPage; Response->reserved0 = page->ViewId;
    Response->bytes = Request->bytes;
Complete:
    KswordARKReleasePushLockExclusive(&g_ShadowLock); KeLeaveCriticalRegion();
ReleaseObjects:
    if (targetLocked) { PsReleaseProcessExitSynchronization(target); }
    if (ownerLocked) { PsReleaseProcessExitSynchronization(owner); }
    if (owner != NULL) { ObDereferenceObject(owner); }
    if (target != NULL) { ObDereferenceObject(target); }
    return status;
}

/* A dead debugger or target cannot retain an execution shadow or a page pin. */
VOID KswordARKDebuggerProcessExit(ULONG ProcessId)
{
    ULONG index;
    BOOLEAN found = FALSE;
    KSWORD_ARK_CONTROL_HVM_REQUEST request = { 0 };
    KSWORD_ARK_CONTROL_HVM_RESPONSE response;
    KeEnterCriticalRegion(); KswordARKAcquirePushLockExclusive(&g_ShadowLock);
    for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) {
        if (g_Shadow[index].Mdl != NULL && (g_Shadow[index].OwnerId == ProcessId || g_Shadow[index].TargetId == ProcessId)) { found = TRUE; break; }
    }
    for (index = 0; index < RTL_NUMBER_OF(g_Quarantine); ++index) {
        if (g_Quarantine[index].Owner != NULL && (g_Quarantine[index].OwnerId == ProcessId || g_Quarantine[index].TargetId == ProcessId)) { found = TRUE; break; }
    }
    if (found) {
        request.version = KSWORD_ARK_HVM_PROTOCOL_VERSION; request.size = sizeof(request);
        request.command = KSWORD_ARK_HVM_CONTROL_STOP_RESIDENT;
        request.flags = KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED;
        request.confirmationToken = KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN;
        (void)KswordARKHvmControl(&request, &response);
        for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) {
            KSW_DEBUG_SHADOW* page = &g_Shadow[index];
            if (page->Mdl != NULL && (page->OwnerId == ProcessId || page->TargetId == ProcessId) && NT_SUCCESS(ShadowRemoveView(page))) { ShadowFree(page); }
        }
        for (index = 0; index < RTL_NUMBER_OF(g_Quarantine); ++index) {
            if (g_Quarantine[index].Owner != NULL && (g_Quarantine[index].OwnerId == ProcessId || g_Quarantine[index].TargetId == ProcessId)) {
                ObDereferenceObject(g_Quarantine[index].Owner); RtlZeroMemory(&g_Quarantine[index], sizeof(g_Quarantine[index]));
            }
        }
    }
    KswordARKReleasePushLockExclusive(&g_ShadowLock); KeLeaveCriticalRegion();
}

/* Called after HVM uninitialization has removed all execution mappings. */
VOID KswordARKDebuggerShutdown(VOID)
{
    ULONG index;
    for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) { ShadowFree(&g_Shadow[index]); }
    for (index = 0; index < RTL_NUMBER_OF(g_Quarantine); ++index) {
        if (g_Quarantine[index].Owner != NULL) { ObDereferenceObject(g_Quarantine[index].Owner); }
        RtlZeroMemory(&g_Quarantine[index], sizeof(g_Quarantine[index]));
    }
}
