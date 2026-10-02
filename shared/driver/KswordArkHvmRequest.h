#pragma once
#include "KswordArkHvmIoctl.h"

/* Hardware-only prerequisites; lifecycle guards are checked after their registration. */
static __inline unsigned long long KswordArkHvmHardwareFeatures(unsigned long backend)
{
    /* NPT and NRIP must not be replaced by Intel EPT or MSR-bitmap evidence. */
    if (backend == KSWORD_ARK_HVM_BACKEND_SVM) {
        /* ASID and privileged state validity are separately established by SVM discovery. */
        return KSWORD_ARK_HVM_FEATURE_AMD | KSWORD_ARK_HVM_FEATURE_SVM |
            KSWORD_ARK_HVM_FEATURE_NPT | KSWORD_ARK_HVM_FEATURE_SVM_NRIP;
    }
    /* Unknown backends cannot satisfy an all-ones mask. */
    if (backend != KSWORD_ARK_HVM_BACKEND_VMX) { return ~0ULL; }
    /* Preserve the complete Intel hardware admission contract. */
    return KSWORD_ARK_HVM_FEATURE_INTEL | KSWORD_ARK_HVM_FEATURE_VMX |
        KSWORD_ARK_HVM_FEATURE_FEATURE_CONTROL_LOCKED | KSWORD_ARK_HVM_FEATURE_VMX_OUTSIDE_SMX |
        KSWORD_ARK_HVM_FEATURE_EPT | KSWORD_ARK_HVM_FEATURE_EPT_WB |
        KSWORD_ARK_HVM_FEATURE_EPT_4_LEVEL | KSWORD_ARK_HVM_FEATURE_EPT_2MB |
        KSWORD_ARK_HVM_FEATURE_INVEPT | KSWORD_ARK_HVM_FEATURE_INVEPT_SINGLE;
}

/* A shared, read-only admission result for all GUI entrypoints and offline fixtures. */
enum KSWORD_HVM_RESIDENT_GATE {
    KswHvmGateReady, KswHvmGateUnsupported, KswHvmGateFirmware,
    KswHvmGateOuterConflict, KswHvmGateOuterOptIn, KswHvmGateFault,
    KswHvmGatePower, KswHvmGateBusy
};

/* Cleanup never uses this entry gate: retained or partially resident CPUs must be stoppable. */
static __inline unsigned long KswordArkHvmResidentGate(
    const KSWORD_ARK_QUERY_HVM_RESPONSE* r, int allowNested)
{
    /* Common evidence must not be inferred from menu preferences. */
    unsigned long long required;
    /* Read the bounded fixed vendor buffer without assuming NUL termination. */
    static const char vmware[12] = {'V','M','w','a','r','e','V','M','w','a','r','e'};
    /* Use a scalar loop so this header stays portable to kernel and C test builds. */
    unsigned long i;
    /* Unknown backends remain closed even if a malformed packet sets every feature bit. */
    if (r->backend != KSWORD_ARK_HVM_BACKEND_VMX && r->backend != KSWORD_ARK_HVM_BACKEND_SVM) { return KswHvmGateUnsupported; }
    /* Retained ownership takes priority over stale capability discovery. */
    if (r->stateFlags & (KSWORD_ARK_HVM_STATE_FAULTED | KSWORD_ARK_HVM_STATE_ROLLBACK_REQUIRED)) { return KswHvmGateFault; }
    /* Power epochs cannot be overridden by FORCE or a UI preference. */
    if (r->stateFlags & KSWORD_ARK_HVM_STATE_POWER_TRANSITION_PENDING) { return KswHvmGatePower; }
    /* Do not initiate another hardware transition while one is in progress. */
    if (r->stateFlags & KSWORD_ARK_HVM_STATE_BUSY) { return KswHvmGateBusy; }
    /* Firmware and host-state refusals are authoritative. */
    if (r->queryStatus == KSWORD_ARK_HVM_QUERY_STATUS_FIRMWARE_DISABLED) { return KswHvmGateFirmware; }
    /* An incompatible outer owner is not resolved by enabling nested opt-in. */
    if (r->queryStatus == KSWORD_ARK_HVM_QUERY_STATUS_HYPERVISOR_CONFLICT) { return KswHvmGateOuterConflict; }
    /* Any other unsuccessful discovery cannot authorize entry. */
    if (r->queryStatus != KSWORD_ARK_HVM_QUERY_STATUS_OK) { return KswHvmGateUnsupported; }
    /* Both backends need the same finalized lifecycle ownership guards. */
    required = KswordArkHvmHardwareFeatures(r->backend) | KSWORD_ARK_HVM_FEATURE_RESIDENT_VMM |
        KSWORD_ARK_HVM_FEATURE_RESIDENT_LIFECYCLE_GUARDED;
    /* Intel residency additionally requires its actually allocated MSR interception path. */
    if (r->backend == KSWORD_ARK_HVM_BACKEND_VMX) { required |= KSWORD_ARK_HVM_FEATURE_MSR_BITMAP; }
    /* A capability-only lifecycle is usable, while UNSUPPORTED is not. */
    if ((r->featureFlags & required) != required || r->residentImplementation == KSWORD_ARK_HVM_IMPLEMENTATION_UNSUPPORTED) { return KswHvmGateUnsupported; }
    /* These raw AMD observations are only valid after a complete successful probe. */
    if (r->backend == KSWORD_ARK_HVM_BACKEND_SVM &&
        (r->svmCapabilities.asidCount < 2 || r->svmCapabilities.msrValidMask != 15 ||
         r->svmCapabilities.rejectReason != KSWORD_ARK_SVM_REJECT_NONE)) { return KswHvmGateUnsupported; }
    /* Intel's clipped identity map must never be reused for residency. */
    if (r->backend == KSWORD_ARK_HVM_BACKEND_VMX && (r->stateFlags & KSWORD_ARK_HVM_STATE_EPT_TRUNCATED)) { return KswHvmGateUnsupported; }
    /* A stopped runtime retaining unload ownership needs recovery before a new start. */
    if (!r->residentProcessorCount && (r->stateFlags & KSWORD_ARK_HVM_STATE_UNLOAD_GUARD_ARMED)) { return KswHvmGateFault; }
    /* AMD only implements an explicitly permitted VMware outer environment. */
    if (r->featureFlags & KSWORD_ARK_HVM_FEATURE_HYPERVISOR_PRESENT) {
        /* Reject an unknown outer even when the preference is on. */
        if (r->backend == KSWORD_ARK_HVM_BACKEND_SVM) {
            /* Compare precisely twelve bytes of the published host identity. */
            for (i = 0; i < 12; ++i) { if (r->hypervisorVendor[i] != vmware[i]) { return KswHvmGateOuterConflict; } }
        }
        /* Host identity acceptance does not imply user opt-in. */
        if (!allowNested) { return KswHvmGateOuterOptIn; }
    }
    /* Readiness for entry still requires PREPARE and full-set SELF_TEST in the driver. */
    return KswHvmGateReady;
}

/* Convert the same GUI preferences to the selected backend's existing wire flags. */
static __inline int KswordArkHvmNestedModeMatches(const KSWORD_ARK_QUERY_HVM_RESPONSE* r, int nested)
{
    /* Intel chooses dispatch at start; AMD preparation has an immutable general-mode contract. */
    if (r->backend != KSWORD_ARK_HVM_BACKEND_SVM) { return 1; }
    /* An old driver must never be treated as implicitly supporting general SVM. */
    if (nested && !(r->featureFlags & KSWORD_ARK_HVM_FEATURE_NESTED_SVM_DISPATCH)) { return 0; }
    /* No resources means the next PREPARE can establish the requested mode. */
    if (!(r->stateFlags & KSWORD_ARK_HVM_STATE_RESOURCES_READY)) { return 1; }
    /* A request change requires explicit stop/release rather than silently replacing resources. */
    return !!nested == !!(r->featureFlags & KSWORD_ARK_HVM_FEATURE_NESTED_SVM_PREPARED);
}

/* Convert only implemented preferences; unsupported commands are rejected separately. */
static __inline unsigned long KswordArkHvmBackendControlFlags(
    unsigned long backend, unsigned long command, unsigned long flags, int nestedDispatch)
{
    /* General AMD mode is fixed at preparation and repeated at start. */
    if (backend == KSWORD_ARK_HVM_BACKEND_SVM) {
        /* Never transmit retained Intel preferences to the SVM flag validator. */
        flags &= KSWORD_ARK_HVM_CONTROL_FLAG_UI_CONFIRMED | KSWORD_ARK_HVM_CONTROL_FLAG_FORCE |
            KSWORD_ARK_HVM_CONTROL_FLAG_ALLOW_NESTED;
        /* Self-test exercises the previously prepared set without changing its mode. */
        if (nestedDispatch && (command == KSWORD_ARK_HVM_CONTROL_PREPARE || command == KSWORD_ARK_HVM_CONTROL_START_RESIDENT)) {
            /* This is a software selection, not hardware execution evidence. */
            flags |= KSWORD_ARK_HVM_CONTROL_FLAG_ENABLE_NESTED_SVM;
        }
    }
    /* Intel's existing per-command flag choices remain intact. */
    return flags;
}

/* One wire-request builder for native GUI controls and the command engine.
 * The driver remains authoritative for allowed flags and lifecycle state. */
static __inline void KswordArkHvmBuildControlRequest(
    KSWORD_ARK_CONTROL_HVM_REQUEST* request,
    unsigned long command, unsigned long flags,
    unsigned long generation, unsigned long parameter)
{
    unsigned long i;
    unsigned char* bytes = (unsigned char*)request;
    for (i = 0; i < (unsigned long)sizeof(*request); ++i) { bytes[i] = 0; }
    request->version = KSWORD_ARK_HVM_PROTOCOL_VERSION;
    request->size = (unsigned long)sizeof(*request);
    request->command = command;
    request->flags = flags;
    if (command == KSWORD_ARK_HVM_CONTROL_LAUNCH_TEST_GUEST)
    {
        request->flags |= KSWORD_ARK_HVM_CONTROL_FLAG_ONE_SHOT_GUEST;
    }
    request->confirmationToken = KSWORD_ARK_HVM_CONTROL_CONFIRMATION_TOKEN;
    request->expectedGeneration = generation;
    if (command == KSWORD_ARK_HVM_CONTROL_SOAK) { request->soakMilliseconds = parameter; }
    if ((flags & KSWORD_ARK_HVM_CONTROL_FLAG_VMREAD_BENCH) != 0)
    {
        request->vmreadBenchIterations = parameter;
    }
}
