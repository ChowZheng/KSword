/* The no-XSTATE leaf owns no guest memory callbacks or Windows services. */
#pragma once
#include "hvm_svm_perf.h"
#include "hvm_svm_hotspots.h"
#include "hvm_svm_nested_machine.h"

/* A short bridge may retain only a complete L1 overlay with no software event ownership. */
static __inline unsigned KswSvmFastEligible(const KSW_NSVM_MACHINE* Machine, unsigned Tlb)
{
    /* The predicate is portable and tested independently of the Windows binding. */
    const KSW_NSVM_EXECUTION* execution;
    /* Partial descriptors never authorize skipping state restoration. */
    if (!Machine || !(execution = Machine->Execution) || !execution->Session || !execution->Current) { return 0; }
    /* Every retained owner, injection or observation window needs the full coordinator. */
    return !Tlb && execution->Session->Phase == KSW_NSVM_SESSION_IDLE && !execution->Session->Lease.Token &&
        execution->Gif <= 1 && !execution->Pending.Count && !execution->RetryEventToken &&
        !Machine->ArmedToken && !Machine->ArmedObservation && !Machine->PhysicalNmiToken &&
        !Machine->HeldNmiGuard && !Machine->NmiCount && !Machine->NmiHardwareMask && !Machine->NmiBlocked &&
        !Machine->IrqWindow.Applied && !Machine->NmiWindow.Applied && !Machine->Iret.Applied && !Machine->Iret.Requested &&
        Machine->Overlay.Applied && !Machine->Overlay.Inner && !Machine->Overlay.SuppressedVirq &&
        Machine->Overlay.ForcedMask == (execution->Gif == 0 ? 1U : 0U) &&
        !KswSvmRead64(execution->Current, KSW_VMCB_EVENT) && !KswSvmRead64(execution->Current, 0x068U) &&
        !(KswSvmRead64(execution->Current, KSW_VMCB_INTCTL) & (1ULL << 8));
}

typedef struct _KSW_SVM_FAST {
    /* Published only by a completed, event-free L1 entry coordinator. */
    KSW_SVM_U64 Enabled, Efer, Hsave, Xss, IntCtl, PhysicalTpr, XsaveFeatures;
    /* EFER.LMA follows current hardware mode even when no intercepted WRMSR occurred. */
    KSW_SVM_U64* VirtualEfer;
    /* Fast and ordinary observations serialize against the same raw-counter readers. */
    volatile KSW_SVM_U64* HotSequence;
    volatile KSW_SVM_U64* GeneralSequence;
    /* All pointers are prepared CPU-local resident allocations, never guest operands. */
    KSWORD_HVM_HOTSPOTS* Hot;
    KSW_SVM_U64 *GeneralExits, *GeneralLastExit, *VmExits, *RawExit, *LegacyTlb;
    /* Preserve coordinator diagnostics while retaining the unchanged entry overlay. */
    KSW_SVM_U64 *Transitions, *MachineLastExit;
    unsigned* MachineLastAction;
    /* The experimental mode requires profiling, including direct fast-path counters. */
    KSW_SVM_PERF* Perf;
    /* Shared write-protection changes force the complete flush/event coordinator without SIMD or calls. */
    volatile KSW_SVM_U64* GuardEpoch;
    KSW_SVM_U64 GuardGeneration;
} KSW_SVM_FAST;

/* Returns zero with no mutation unless the complete integer-only instruction is admissible. */
unsigned KswSvmFastTry(KSW_SVM_FAST* Fast, KSW_SVM_VMCB* Current,
    KSW_SVM_U64* Gpr, KSW_SVM_U64 PhysicalTpr);
