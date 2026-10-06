/* This entire translation unit must pass the no-call/no-SIMD machine-code gate. */
#include "hvm_svm_fast.h"
/* Compiler barriers emit no instructions and forbid reordering outside diagnostic sequences. */
#if defined(_MSC_VER)
#include <intrin.h>
#define FAST_BARRIER() _ReadWriteBarrier()
#else
#define FAST_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

/* Volatile aligned scalar accesses prevent struct-copy or vectorized memory helpers. */
#define FAST_WORD(V, O) (*(volatile KSW_SVM_U64*)((unsigned char*)(V) + (O)))

/* No allocation, exceptions, MSR instructions, indirect callbacks, TLS or XSTATE access. */
unsigned KswSvmFastTry(KSW_SVM_FAST* Fast, KSW_SVM_VMCB* Current,
    KSW_SVM_U64* Gpr, KSW_SVM_U64 PhysicalTpr)
{
    /* All automatic values are integers; the generated binary is checked independently. */
    KSW_SVM_U64 direction, value, input, rip, next;
    /* Slot identities are fixed at initialization and never evicted. */
    unsigned msr, slot;
    /* The caller supplies only prepared pointers and saved general registers. */
    KSWORD_HVM_HOT_LEVEL* hot;
    /* Unpublished bindings and changed physical priority require normal event coordination. */
    if (!Fast || !Fast->Enabled || PhysicalTpr != Fast->PhysicalTpr) { return 0; }
    /* Only the original event-free L1 entry can retain its unmodified overlay. */
    if (FAST_WORD(Current, KSW_VMCB_EXITCODE) != 0x7cULL ||
        FAST_WORD(Current, KSW_VMCB_INTCTL) != Fast->IntCtl ||
        FAST_WORD(Current, KSW_VMCB_EVENT) ||
        (FAST_WORD(Current, KSW_VMCB_EXITINTINFO) & (1ULL << 31)) ||
        ((unsigned char*)Current)[KSW_VMCB_CPL] ||
        !(FAST_WORD(Current, KSW_VMCB_CR0) & 1ULL) ||
        (FAST_WORD(Current, KSW_VMCB_RFLAGS) & (1ULL << 17))) { return 0; }
    /* A valid hardware next RIP is mandatory even for idempotent writes. */
    rip = FAST_WORD(Current, KSW_VMCB_RIP); next = FAST_WORD(Current, KSW_VMCB_NRIP);
    /* No wrap, stale continuation or oversized instruction may execute through this leaf. */
    if (next <= rip || next - rip > 15ULL) { return 0; }
    /* Malformed direction remains diagnosable through the ordinary exception policy. */
    direction = FAST_WORD(Current, KSW_VMCB_EXITINFO1);
    /* The architectural MSR operand is ECX, ignoring the upper register half. */
    msr = (unsigned)Gpr[1];
    /* Only read and same-value write semantics are admitted here. */
    if (direction > 1) { return 0; }
    /* Values were captured from the current virtual register policy at entry. */
    if (msr == 0xc0000080U) {
        /* Only the visible SVME bit comes from the software register image. */
        slot = 0; value = (FAST_WORD(Current, KSW_VMCB_EFER) & ~KSW_SVM_EFER_SVME) |
            (Fast->Efer & KSW_SVM_EFER_SVME);
    }
    /* HSAVE remains virtual: this path never reads or overwrites physical VM_HSAVE_PA. */
    else if (msr == 0xc0010117U) { slot = 1; value = Fast->Hsave; }
    /* An unchanged XSS write needs no live-mask or save-layout transition. */
    else if (msr == 0xda0U && (Fast->XsaveFeatures & 8U)) { slot = 2; value = Fast->Xss; }
    /* Unknown or hardware-backed MSRs always use the complete bridge. */
    else { return 0; }
    /* WRMSR takes the low halves of EDX:EAX; changed state is never fast-emulated. */
    input = ((Gpr[2] & 0xffffffffULL) << 32) | (FAST_WORD(Current, KSW_VMCB_RAX) & 0xffffffffULL);
    /* A declined instruction leaves even its raw operands unchanged. */
    if (direction && input != value) { return 0; }
    /* Fixed slots preserve the same raw MSR ownership accounting as slow dispatch. */
    hot = &Fast->Hot->levels[0];
    /* Refuse counter exhaustion before any architectural output changes. */
    if (hot->msrUsed < 3 || hot->msrs[slot].number != msr || hot->total == ~0ULL ||
        hot->codes[0x7c] == ~0ULL || *Fast->GeneralExits == ~0ULL ||
        *Fast->VmExits == ~0ULL || *Fast->LegacyTlb == ~0ULL || *Fast->Transitions == ~0ULL ||
        Fast->Perf->Metrics.tlbIssued[0] == ~0ULL || Fast->Perf->Metrics.fastMsr[slot][direction] == ~0ULL ||
        (direction ? hot->msrs[slot].writes : hot->msrs[slot].reads) == ~0ULL) { return 0; }
    /* An unselected interval uses a short independent telemetry transaction. */
    if (!Fast->Perf->Selected) { ++Fast->Perf->Sequence; }
    /* Sampled raw-level attribution remains L1/MSR even after completing the instruction. */
    if (Fast->Perf->Selected) { Fast->Perf->Row = &Fast->Perf->Metrics.rows[0][0]; }
    /* Publication protects machine counters and exact hotspot counts simultaneously. */
    ++*Fast->GeneralSequence; ++*Fast->HotSequence;
    /* All counter and continuation writes must stay inside the published odd intervals. */
    FAST_BARRIER();
    /* Count every physical return, including those which bypass the C dispatcher. */
    ++*Fast->GeneralExits; ++*Fast->VmExits; ++*Fast->LegacyTlb; ++*Fast->Transitions;
    /* The retained overlay still describes the most recent unchanged hardware attempt. */
    *Fast->GeneralLastExit = *Fast->MachineLastExit = *Fast->RawExit = 0x7cULL;
    /* READY denotes the unchanged event-free continuation, not a new virtual entry. */
    *Fast->MachineLastAction = 0;
    /* Fast-path counts are a subset of hotspot counts, never additional exit weights. */
    ++Fast->Perf->Metrics.fastMsr[slot][direction]; ++Fast->Perf->Metrics.tlbIssued[0];
    /* Preserve the original MSR and direction before modifying the read output. */
    ++hot->total; ++hot->codes[0x7c]; hot->lastMsr = msr;
    /* Keep raw hardware operands so a later query can inspect the last fast return. */
    hot->lastCode = 0x7c; hot->lastRip = rip; hot->lastInfo1 = direction;
    /* EXITINFO2 is diagnostic only and is not fabricated as an MSR operand. */
    hot->lastInfo2 = FAST_WORD(Current, KSW_VMCB_EXITINFO2);
    /* The first three identity slots were initialized before any executable pointer publication. */
    if (direction) {
        /* A same-value EFER write still synchronizes the virtual mirror's current LMA. */
        if (!slot) { *Fast->VirtualEfer = value; }
        /* No live physical register needs a write for these unchanged values. */
        ++hot->msrs[slot].writes;
    }
    /* RDMSR zero-extends both result registers exactly like the ordinary policy. */
    else {
        /* RAX is stored in VMCB; RDX lives in the saved general-register bank. */
        ++hot->msrs[slot].reads; FAST_WORD(Current, KSW_VMCB_RAX) = value & 0xffffffffULL;
        /* Never restore a stale upper half of RDX. */
        Gpr[2] = value >> 32;
    }
    /* Completing the instruction consumes the old single-instruction shadow. */
    FAST_WORD(Current, 0x068U) = 0;
    /* EVENTINJ is already zero by admission, so no injection ownership is discarded. */
    FAST_WORD(Current, KSW_VMCB_RIP) = next;
    /* A remote acquire recheck must never precede any pending output write. */
    FAST_BARRIER();
    /* Make all architectural and diagnostic mutations visible before publishing even sequences. */
    ++*Fast->HotSequence; ++*Fast->GeneralSequence;
    /* Selected root intervals are closed by the integer assembly aggregator. */
    if (!Fast->Perf->Selected) { ++Fast->Perf->Sequence; }
    /* No host/guest VMLOAD state or XSTATE component changed anywhere in this function. */
    return 1;
}
