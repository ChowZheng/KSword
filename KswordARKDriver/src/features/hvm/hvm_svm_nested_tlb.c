/* Processor-local virtual TLB invalidation; physical TLB_CONTROL=1 remains mandatory. */
#include "hvm_svm_nested_session.h"

/* A shadow publication requires a global-capable ASID flush even when L1 requested local-only. */
unsigned KswSvmNestedSessionTlbSelect(const KSW_NSVM_SESSION* Session,
    const KSW_NSHADOW* Shadow, unsigned FlushByAsid)
{
    /* Only the running nested context can consume this shadow's hardware translations. */
    if (!Session || !Shadow || Session->Phase != KSW_NSVM_SESSION_L2) { return 0; }
    /* Full virtual invalidation must not be reduced to a single physical ASID. */
    if (Session->PendingTlbControl == 1U) { return 1U; }
    /* Shadow changes and ASID-wide requests include global translations. */
    if (Shadow->FlushPending || Session->PendingTlbControl == 3U) { return FlushByAsid ? 3U : 1U; }
    /* Older hardware can conservatively perform a complete flush. */
    if (Session->PendingTlbControl == 7U) { return FlushByAsid ? 7U : 1U; }
    /* Stable NPT02 reentry has no architectural invalidation work. */
    return 0;
}

/* Keep INVALID and unexecuted attempts retryable without claiming flush completion. */
void KswSvmNestedSessionTlbComplete(KSW_NSVM_SESSION* Session,
    KSW_NSHADOW* Shadow, unsigned Issued, KSW_SVM_U64 ExitCode)
{
    /* Entry failure cannot retire either source's outstanding invalidation. */
    if (!Session || !Shadow || Session->Phase != KSW_NSVM_SESSION_L2 ||
        ExitCode == KSW_SVM_EXIT_INVALID || !Issued) { return; }
    /* A full flush or global-capable ASID flush satisfies shadow publication. */
    if (Issued == 1U || Issued == 3U) { Shadow->FlushPending = 0; }
    /* Every legal selected flush satisfies the captured virtual request. */
    if (Issued == 1U || Issued == 3U || Issued == 7U) { Session->PendingTlbControl = 0; }
}

/* Caller checks virtual SVME/CPL and validates NRIP before this completed instruction action. */
unsigned int KswSvmNestedSessionInvalidate(KSW_NSVM_SESSION* Session,
    KSW_NSVM_SESSION_IO* Io, KSW_SVM_U64 Linear, unsigned Asid)
{
    /* An active inner guest or retained VMCB writeback cannot be reset as an L1 instruction. */
    if (!Session || !Io || !Io->Shadow || !Io->Mmu || Session->Lease.Token ||
        Session->Phase != KSW_NSVM_SESSION_IDLE) { return KSW_NSVM_ACTION_UNSUPPORTED; }
    /* Invalidation counters cannot wrap into a prior diagnostic generation. */
    if (Session->Invalidations == ~0ULL) { return KSW_NSVM_ACTION_FAULT; }
    /* Shared roots observe a conservative virtual invalidation on their owning CPU at the next entry. */
    if (Io->InvalidateCaches) { Io->InvalidateCaches(Io->Operand.Context); }
    /* Discard the entire NPT02 cache instead of attempting an incomplete linear-address walk. */
    if (KswSvmNestedShadowReset(Io->Shadow) != KSW_NSHADOW_OK) {
        /* The current virtual CPU must not enter a root with uncertain cache ownership. */
        Session->Phase = KSW_NSVM_SESSION_FAULTED; return KSW_NSVM_ACTION_FAULT;
    }
    /* A future installation must carry the new epoch; old walk results become inadmissible. */
    Io->Mmu->Epoch = Io->Shadow->Epoch;
    /* Record exact virtual operands without using the guest ASID as a hardware ASID. */
    Session->LastInvalidationLinear = Linear; Session->LastInvalidationAsid = Asid;
    /* Both the cache and its public-to-root metadata now refer to one generation. */
    Session->LastInvalidationEpoch = Io->Shadow->Epoch; ++Session->Invalidations;
    /* Full hardware flush on the next VMRUN removes translations of discarded NPT02 leaves. */
    return KSW_NSVM_ACTION_RETURN;
}
