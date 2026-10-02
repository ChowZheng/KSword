/* Vendor dispatch without introducing a function-pointer call on every VMEXIT. */
#include "hvm_backend.h"
#if defined(_M_AMD64)
#include "hvm_svm.h"
/* Baseline AMD lifecycle; optional Intel features never reach these operations. */
static const KSW_HVM_BACKEND_OPS KswSvmOps = {
    /* Hardware probe and strict option validator. */
    KswordSvmProbe, KswordSvmValidateFlags,
    /* Independent resource ownership and release. */
    KswordSvmPrepare, KswordSvmRelease,
    /* Shared-phase CPU self-test, start and stop. */
    KswordSvmSelfTest, KswordSvmStart, KswordSvmStop
};
#endif

/* Backend choice is explicit; non-x64 architectures never link SVM assembly. */
const KSW_HVM_BACKEND_OPS* KswordHvmBackend(ULONG Backend)
{
#if defined(_M_AMD64)
    /* Only SVM takes the new execution boundary in this increment. */
    if (Backend == KSWORD_ARK_HVM_BACKEND_SVM) { return &KswSvmOps; }
#else
    /* ARM64 has no AMD64 SVM backend. */
    UNREFERENCED_PARAMETER(Backend);
#endif
    /* Intel remains handled by existing VMX functions and tests. */
    return NULL;
}

/* Caller holds the runtime lifetime lock; CPU-local counters may still change. */
VOID KswordHvmBackendQuery(KSW_HVM_RUNTIME* Runtime, KSWORD_ARK_QUERY_HVM_RESPONSE* Response)
{
    /* Always identify the architecture even before prepare. */
    Response->backend = Runtime->BackendId;
    /* Consumers must not mistake a successful control for a power transition. */
    Response->powerGeneration = (ULONG)Runtime->PowerTransitionGeneration;
    /* Missing privileged evidence never becomes an implicit zero-valued success. */
    Response->svmCapabilities = Runtime->SvmCapabilities;
    /* No active backend means no claimed translation implementation. */
    Response->slatType = Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_SVM ? KSWORD_ARK_HVM_SLAT_NPT :
        (Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_VMX ? KSWORD_ARK_HVM_SLAT_EPT : KSWORD_ARK_HVM_SLAT_NONE);
    /* Generic readiness follows real prepared ownership. */
    Response->slatReady = (Runtime->StateFlags & KSWORD_ARK_HVM_STATE_RESOURCES_READY) != 0;
    /* Retain a backend-specific status without overloading VM-instruction error. */
    Response->backendStatus = (ULONG)Runtime->LastStatus;
    /* Stopped, partial or faulted residency cannot advertise all-core Intel activation. */
    if (!Response->processorCount || Response->residentProcessorCount != Response->processorCount ||
        !(Response->stateFlags & KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE) ||
        (Response->stateFlags & (KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED))) {
        /* Keep durable software capability separate from current activation. */
        Response->featureFlags &= ~KSWORD_ARK_HVM_FEATURE_NESTED_VMX_ARMED;
    }
    /* Identity hiding is observed from the active runtime rather than the GUI preference. */
    if (Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_VMX &&
        (Response->featureFlags & KSWORD_ARK_HVM_FEATURE_NESTED_VMX_ARMED) &&
        InterlockedCompareExchange(&Runtime->HideHypervisorCpuid, 0, 0)) {
        /* This readback must be absent for old drivers or failed/partial starts. */
        Response->featureFlags |= KSWORD_ARK_HVM_FEATURE_HYPERVISOR_IDENTITY_HIDDEN;
    }
#if defined(_M_AMD64)
    /* AMD rows describe SVM state, not a zero-valued VMX result. */
    /* Software support does not imply resource preparation or inner-OS acceptance. */
    if (Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_SVM) {
        /* Keep experimental dispatch separate from Intel capability bits. */
        Response->featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_DISPATCH;
    }
    if (Runtime->BackendId == KSWORD_ARK_HVM_BACKEND_SVM && Runtime->BackendContext != NULL) {
        /* Resources are kept alive by the query caller's shared lock. */
        KSW_SVM_STATE* state = Runtime->BackendContext;
        /* Traverse the exact prepared CPU set. */
        ULONG index;
        /* Require full-set residency without retained faults before publishing activation. */
        BOOLEAN armed = state->Count != 0 && Response->processorCount == state->Count && Response->residentProcessorCount == state->Count &&
            (Response->stateFlags & KSWORD_ARK_HVM_STATE_RESIDENT_ACTIVE) != 0 &&
            (Response->stateFlags & (KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED)) == 0;
        /* NPT readiness requires an actual complete root. */
        Response->slatReady = state->Npt.RootPa != 0;
        /* Publish immutable mode only after a complete successful general PREPARE. */
        if (Response->slatReady && (Response->stateFlags & KSWORD_ARK_HVM_STATE_RESOURCES_READY) &&
            Response->processorCount == state->Count && Runtime->PreparedProcessorCount == state->Count &&
            (state->PreparedFlags & KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM)) {
            /* Bounded probe allocations never satisfy the general selector. */
            Response->featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_PREPARED;
        }
        /* Read public row additions without dereferencing unrelated Intel contexts. */
        for (index = 0; index < state->Count; ++index) {
            /* CPU-local stages are naturally aligned atomic words. */
            Response->processors[index].executionStage = (ULONG)state->Cpus[index].Stage;
            /* Make the correct decoder explicit per CPU. */
            Response->processors[index].backend = KSWORD_ARK_HVM_BACKEND_SVM;
            /* Atomic activation reads cannot borrow another CPU's successful binding. */
            if (!state->Cpus[index].GeneralRequested ||
                !InterlockedCompareExchange(&state->Cpus[index].Active, 0, 0) ||
                !InterlockedCompareExchange64((volatile LONG64*)&state->Cpus[index].NestedEntryEnabled, 0, 0)) { armed = FALSE; }
        }
        /* Experimental implementation maturity remains PARTIAL while dispatch is armed. */
        if (armed && (Response->featureFlags & KSWORD_ARK_HVM_FEATURE_NESTED_SVM_PREPARED)) {
            /* This is all-core execution evidence, not proof of an inner OS boot. */
            Response->featureFlags |= KSWORD_ARK_HVM_FEATURE_NESTED_SVM_ARMED;
        }
    }
#endif
}
