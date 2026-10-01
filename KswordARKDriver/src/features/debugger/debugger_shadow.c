#include "debugger.h"
#include "../hvm/hvm_runtime.h"
#include "../hvm/hvm_ept_view.h"
#include "../../platform/pool_compat.h"
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
} KSW_DEBUG_SHADOW;
static KSW_DEBUG_SHADOW g_Shadow[KSWORD_ARK_HVM_MAX_VIEWS];
static EX_PUSH_LOCK g_ShadowLock;
NTSYSAPI NTSTATUS NTAPI PsLookupProcessByProcessId(HANDLE Id, PEPROCESS* Process);

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
        if ((Page->Breakpoints[offset / 8] & (1U << (offset % 8))) != 0U) { request->shadow[offset] = 0xCC; }
    }
    status = KswordARKHvmEptViewControl(request, &response);
    ExFreePoolWithTag(request, 'sSDK');
    if (NT_SUCCESS(status)) { status = response.lastStatus; }
    if (NT_SUCCESS(status)) { Page->ViewId = response.viewId; }
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
    UCHAR previous;
    BOOLEAN empty = TRUE, newPage = FALSE;
    NTSTATUS status;
    if (OwnerProcessId <= 4UL || Request->processId <= 4UL || Request->processId == OwnerProcessId ||
        Request->address == 0ULL || base > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress - PAGE_SIZE ||
        Request->bytes != 1ULL || (Request->flags & KSWORD_ARK_DEBUGGER_FLAG_CONFIRMED) == 0UL) { return STATUS_INVALID_PARAMETER; }
    status = PsLookupProcessByProcessId(ULongToHandle(OwnerProcessId), &owner);
    if (!NT_SUCCESS(status)) { return status; }
    status = PsLookupProcessByProcessId(ULongToHandle(Request->processId), &target);
    if (!NT_SUCCESS(status)) { ObDereferenceObject(owner); return status; }
    KeEnterCriticalRegion(); KswordARKAcquirePushLockExclusive(&g_ShadowLock);
    for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) {
        if (g_Shadow[index].Mdl == NULL && freePage == NULL) { freePage = &g_Shadow[index]; }
        if (g_Shadow[index].Owner == owner && g_Shadow[index].Target == target && g_Shadow[index].VirtualPage == base) { page = &g_Shadow[index]; }
    }
    if (page == NULL) {
        /* Opaque, naturally aligned storage, matching the existing HVM pin path. */
        ULONG_PTR attach[16] = { 0 };
        if (Request->operation != KSWORD_ARK_DEBUGGER_SHADOW_ADD) { status = STATUS_NOT_FOUND; goto Complete; }
        if (freePage == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Complete; }
        page = freePage; newPage = TRUE;
        page->Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)base, PAGE_SIZE, FALSE, FALSE, NULL);
        if (page->Mdl == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto Complete; }
        KeStackAttachProcess(target, attach);
        __try { MmProbeAndLockPages(page->Mdl, UserMode, IoReadAccess); status = STATUS_SUCCESS; }
        __except (EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
        KeUnstackDetachProcess(attach);
        if (!NT_SUCCESS(status)) { IoFreeMdl(page->Mdl); page->Mdl = NULL; goto Complete; }
        page->Owner = owner; page->Target = target; owner = NULL; target = NULL;
        page->OwnerId = OwnerProcessId; page->TargetId = Request->processId;
        page->VirtualPage = base; page->PhysicalPage = (ULONGLONG)MmGetMdlPfnArray(page->Mdl)[0] << PAGE_SHIFT;
        page->Mapping = MmGetSystemAddressForMdlSafe(page->Mdl, NormalPagePriority | MdlMappingNoExecute);
        if (page->Mapping == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; ShadowFree(page); goto Complete; }
    }
    previous = page->Breakpoints[offset / 8];
    if (Request->operation == KSWORD_ARK_DEBUGGER_SHADOW_ADD) { page->Breakpoints[offset / 8] |= (UCHAR)(1U << (offset % 8)); }
    else { page->Breakpoints[offset / 8] &= (UCHAR)~(1U << (offset % 8)); }
    status = ShadowRemoveView(page);
    if (!NT_SUCCESS(status)) { page->Breakpoints[offset / 8] = previous; goto Complete; }
    for (index = 0; index < sizeof(page->Breakpoints); ++index) { if (page->Breakpoints[index] != 0U) { empty = FALSE; break; } }
    if (!empty) { status = ShadowInstall(page); }
    if (!NT_SUCCESS(status)) {
        page->Breakpoints[offset / 8] = previous;
        if (!newPage) { (void)ShadowInstall(page); }
        else if (page->ViewId == 0UL) { ShadowFree(page); }
        goto Complete;
    }
    Response->address = page->PhysicalPage; Response->reserved0 = page->ViewId;
    if (empty) { ShadowFree(page); }
Complete:
    KswordARKReleasePushLockExclusive(&g_ShadowLock); KeLeaveCriticalRegion();
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
    }
    KswordARKReleasePushLockExclusive(&g_ShadowLock); KeLeaveCriticalRegion();
}

/* Called after HVM uninitialization has removed all execution mappings. */
VOID KswordARKDebuggerShutdown(VOID)
{
    ULONG index;
    for (index = 0; index < RTL_NUMBER_OF(g_Shadow); ++index) { ShadowFree(&g_Shadow[index]); }
}
