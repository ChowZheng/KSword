/* CPU write trapping uses a private NPT01 clone, leaving software walks structurally immutable. */
#include "hvm_svm_watch.h"
#include "hvm_svm_nested_runtime.h"
#include "hvm_svm_cache.h"
#include "../../platform/pool_compat.h"
#include <intrin.h>

/* Executes in the normal Windows guest, never on an SVM host stack. */
static VOID KswWatchPoke(KDPC* Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    KSW_SVM_CPU* cpu = Context;
    KIRQL oldIrql;
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(Arg1); UNREFERENCED_PARAMETER(Arg2);
    /* Stop IPIs cannot preempt between Active readback and the private VMMCALL. */
    KeRaiseIrql(HIGH_LEVEL, &oldIrql);
    if (cpu->Active && cpu->Nested && cpu->Nested->Watch && !cpu->Nested->Watch->Disabled && !cpu->StopRequested) {
        /* First call requests the full generation flush; the second observes its hardware completion. */
        (VOID)KswordSvmAsmCall(KSW_SVM_CALL_QUERY);
        (VOID)KswordSvmAsmCall(KSW_SVM_CALL_QUERY);
        InterlockedIncrement64(&cpu->Nested->Watch->PokeCalls);
    }
    KeLowerIrql(oldIrql);
}
/* Queue individual CPUs; a closed-GIF processor may postpone its own vote without blocking peers. */
static VOID KswWatchTimer(KDPC* Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    KSW_SVM_WATCH_TABLE* watch = Context;
    ULONG cpu;
    LONG64 generation;
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(Arg1); UNREFERENCED_PARAMETER(Arg2);
    if (watch->Disabled) { return; }
    generation = watch->Generation; _ReadBarrier();
    for (cpu = 0; cpu < watch->Owner->Count; ++cpu) {
        if (watch->Acknowledged[cpu] != generation) {
            /* Already queued DPCs are naturally coalesced, with no root spin or cross-CPU wait. */
            (VOID)KeInsertQueueDpc(&watch->Pokes[cpu], NULL, NULL);
        }
    }
}

static KSW_SVM_WATCH_TABLE* KswWatch(KSW_SVM_CPU* Cpu)
{
    /* Backend state remains pinned by the common residency/unload interlock. */
    return ((KSW_SVM_STATE*)Cpu->Runtime->BackendContext)->Watch;
}
static KSW_SVM_WATCH_PAGE* KswWatchSlot(KSW_SVM_WATCH_TABLE* Watch, ULONGLONG Page, BOOLEAN Reserve)
{
    /* Linear probing is bounded and no occupied identity is reused while any CPU is resident. */
    ULONG probe, start = (ULONG)(((Page >> 12) * 11400714819323198485ULL) & (KSW_SVM_WATCH_SLOTS - 1U));
    /* Page-zero is represented by identity one; all physical widths are below overflow. */
    LONG64 identity = (LONG64)((Page & ~4095ULL) + 1ULL);
    /* A full hash table declines optimization instead of losing write provenance. */
    for (probe = 0; probe < KSW_SVM_WATCH_SLOTS; ++probe) {
        /* Every slot remains in its original allocation for the prepared lifetime. */
        KSW_SVM_WATCH_PAGE* slot = &Watch->Slots[(start + probe) & (KSW_SVM_WATCH_SLOTS - 1U)];
        /* Acquire publication before reading State/Pte. */
        LONG64 found = slot->Identity;
        /* MSVC acquire compiler ordering plus x64 TSO needs no locked RMW for a published aligned identity. */
        _ReadBarrier();
        /* Existing identity is returned even when its proof was permanently revoked. */
        if (found == identity) { return slot; }
        /* Only arm callers may acquire a vacant slot; readers never mutate the ledger. */
        if (!found) {
            /* A concurrent arm either supplies this identity or leaves us trying a later slot. */
            if (!Reserve) { return NULL; }
            /* Reserve identity before writing its private leaf descriptor. */
            found = InterlockedCompareExchange64(&slot->Identity, identity, 0);
            /* The winning reservation starts unarmed, never implicitly stable. */
            if (!found || found == identity) { return slot; }
        }
    }
    /* No table eviction occurs during root execution. */
    return NULL;
}
static PULONGLONG KswWatchTable(KSW_SVM_WATCH_TABLE* Watch, ULONGLONG Pa)
{
    /* Prepared physical identities replace MmGetPhysicalAddress or guest-PA dereferences in root. */
    ULONG index;
    /* The owned prefix includes immutable clone pages and already published split pages. */
    for (index = 0; index < Watch->Used; ++index) { if (Watch->Pas[index] == Pa) { return Watch->Pages[index]; } }
    /* No unowned or cyclic pointer can be followed. */
    return NULL;
}
static volatile LONG64* KswWatchLeaf(KSW_SVM_WATCH_TABLE* Watch, ULONGLONG Page)
{
    /* The arm gate serializes publication of preallocated split tables. */
    PULONGLONG table = KswWatchTable(Watch, Watch->RootPa);
    /* Identity-map splitting never changes the original memory type or frame. */
    ULONG level;
    /* Four fixed levels cover the admitted <=48-bit address space. */
    for (level = 4; table && level; --level) {
        /* Select the word using the original host/L1 identity address. */
        ULONG index = (ULONG)((Page >> (12U + 9U * (level - 1U))) & 511ULL);
        /* Atomic reads coexist with hardware A/D updates. */
        ULONGLONG value = (ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&table[index], 0, 0);
        /* Only present original identity translations may be protected. */
        if (!(value & 1ULL)) { return NULL; }
        /* Return exactly the owned 4-KiB leaf, never a guest page-table pointer. */
        if (level == 1U) { return (volatile LONG64*)&table[index]; }
        /* Intermediate pages already have a prepared owned physical identity. */
        if (!(value & 0x80ULL)) { table = KswWatchTable(Watch, value & Watch->Owner->Npt.AddressMask); continue; }
        /* Only architectural 1-GiB/2-MiB leaves can be split. */
        if (level > 3U || Watch->Used == Watch->Capacity) { return NULL; }
        {
            /* Children are initialized completely before the new parent becomes visible. */
            ULONG word, child = Watch->Used++;
            /* Keep low cache bits, NX, A/D and the correct large-page PAT encoding. */
            ULONGLONG flags = value & ~Watch->Owner->Npt.AddressMask;
            /* Parent frame alignment discards PAT/reserved low address bits. */
            ULONGLONG base = value & Watch->Owner->Npt.AddressMask & ~((1ULL << (12U + 9U * (level - 1U))) - 1ULL);
            /* Child page spans are one level smaller. */
            ULONGLONG span = 1ULL << (12U + 9U * (level - 2U));
            /* A 2-MiB split moves PAT bit12 to ordinary PTE bit7. */
            flags &= ~0x80ULL;
            /* A 1-GiB split retains PAT bit12 in its 2-MiB leaf children. */
            flags |= level == 3U ? 0x80ULL | (value & 0x1000ULL) : ((value & 0x1000ULL) ? 0x80ULL : 0ULL);
            /* No allocator or callback can run inside this bounded publication. */
            for (word = 0; word < 512U; ++word) { ((PULONGLONG)Watch->Pages[child])[word] = base + word * span | flags; }
            /* Child contents and ledger precede atomic parent replacement. */
            KeMemoryBarrier();
            /* Ignore old A/D updates only: the arm gate owns structural changes. */
            InterlockedExchange64((volatile LONG64*)&table[index], (LONG64)(Watch->Pas[child] | 7ULL));
            /* Descend into the owned child; its later protection is at 4-KiB granularity. */
            table = Watch->Pages[child];
        }
    }
    /* An unexpected structural pointer declines only the optimization. */
    return NULL;
}
NTSTATUS KswordSvmWatchPrepare(KSW_SVM_STATE* State)
{
    /* All allocation and physical-address resolution occurs at PASSIVE_LEVEL before SVM entry. */
    KSW_SVM_WATCH_TABLE* watch;
    ULONG index, word, child;
    ULONGLONG* original;
    PHYSICAL_ADDRESS highest;
    /* The immutable root plus clone and split budget together stay below 64 MiB. */
    if (State->Npt.PageCount > (KSW_NPT_MAX_PAGES - KSW_SVM_WATCH_SPLITS) / 2U) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Publish every partial allocation immediately into the common release ledger. */
    watch = KswordARKAllocateNonPagedPool(sizeof(*watch), 'wSvK');
    /* Missing ownership cannot be replaced by an empty proof. */
    if (!watch) { return STATUS_INSUFFICIENT_RESOURCES; }
    /* Every free slot starts unarmed; acknowledgements start at zero. */
    RtlZeroMemory(watch, sizeof(*watch)); State->Watch = watch; watch->Owner = State;
    /* Clone exactly the already-built baseline tree; reserved split pages are allocated but unpublished. */
    watch->BaseCount = State->Npt.PageCount; watch->Capacity = watch->BaseCount + KSW_SVM_WATCH_SPLITS;
    /* Prepared mappings and physical identities have separate lifetime ownership. */
    watch->Pages = KswordARKAllocateNonPagedPool(sizeof(PVOID) * watch->Capacity, 'wSvK');
    watch->Pas = KswordARKAllocateNonPagedPool(sizeof(ULONGLONG) * watch->Capacity, 'wSvK');
    original = KswordARKAllocateNonPagedPool(sizeof(ULONGLONG) * watch->BaseCount, 'wSvK');
    /* Uninitialized metadata is never followed by a later release. */
    if (watch->Pages) { RtlZeroMemory(watch->Pages, sizeof(PVOID) * watch->Capacity); }
    /* Temporary baseline identities are required only while cloning at PASSIVE_LEVEL. */
    if (!watch->Pages || !watch->Pas || !original) { if (original) { ExFreePoolWithTag(original, 'wSvK'); } return STATUS_INSUFFICIENT_RESOURCES; }
    highest.QuadPart = (LONGLONG)(State->Npt.Limit - 1ULL);
    /* Record each allocation before deriving or publishing its address. */
    for (index = 0; index < watch->Capacity; ++index) {
        watch->Pages[index] = MmAllocateContiguousMemory(4096, highest);
        /* The enclosing release function handles any acquired prefix. */
        if (!watch->Pages[index]) { ExFreePoolWithTag(original, 'wSvK'); return STATUS_INSUFFICIENT_RESOURCES; }
        ++watch->Allocated; RtlZeroMemory(watch->Pages[index], 4096);
        watch->Pas[index] = (ULONGLONG)MmGetPhysicalAddress(watch->Pages[index]).QuadPart;
    }
    /* Original table frames are captured before reading their parent entries. */
    for (index = 0; index < watch->BaseCount; ++index) { original[index] = (ULONGLONG)MmGetPhysicalAddress(State->Npt.Pages[index]).QuadPart; }
    /* Rewrite only references that match owned table frames; RAM data leaves retain their identity. */
    for (index = 0; index < watch->BaseCount; ++index) {
        RtlCopyMemory(watch->Pages[index], State->Npt.Pages[index], 4096);
    }
    /* Clone traversal distinguishes ordinary 4-KiB data leaves from nonleaf table references. */
    for (index = 0; index < watch->BaseCount; ++index) {
        /* Original allocations form a forward tree; determine each page's level from its parents. */
        ULONG level = index == 0 ? 4U : 0U, parent, position;
        for (parent = 0; parent < index && !level; ++parent) {
            /* Parent level is stored in unused temporary metadata's high byte, not in hardware entries. */
            ULONG parentLevel = parent == 0 ? 4U : (ULONG)(original[parent] >> 56);
            if (parentLevel <= 1U) { continue; }
            for (position = 0; position < 512U; ++position) {
                ULONGLONG entry = ((PULONGLONG)State->Npt.Pages[parent])[position];
                if ((entry & 1ULL) && !(entry & 0x80ULL) && (entry & State->Npt.AddressMask) == (original[index] & State->Npt.AddressMask)) { level = parentLevel - 1U; break; }
            }
        }
        /* Missing level would make ordinary RAM aliases unsafe to rewrite. */
        if (!level) { ExFreePoolWithTag(original, 'wSvK'); return STATUS_DATA_ERROR; }
        original[index] |= (ULONGLONG)level << 56;
        if (level == 1U) { continue; }
        for (word = 0; word < 512U; ++word) {
            ULONGLONG entry = ((PULONGLONG)watch->Pages[index])[word];
            if (!(entry & 1ULL) || (entry & 0x80ULL)) { continue; }
            for (child = 0; child < watch->BaseCount; ++child) { if ((original[child] & State->Npt.AddressMask) == (entry & State->Npt.AddressMask)) { break; } }
            if (child == watch->BaseCount) { ExFreePoolWithTag(original, 'wSvK'); return STATUS_DATA_ERROR; }
            ((PULONGLONG)watch->Pages[index])[word] = watch->Pas[child] | (entry & ~State->Npt.AddressMask);
        }
    }
    /* No transient metadata is retained by root callbacks. */
    ExFreePoolWithTag(original, 'wSvK'); watch->Used = watch->BaseCount; watch->RootPa = watch->Pas[0]; watch->Generation = 1;
    /* Targets come from the same frozen topology used by resident entry/stop. */
    KeInitializeTimerEx(&watch->Timer, NotificationTimer); KeInitializeDpc(&watch->TimerDpc, KswWatchTimer, watch);
    watch->TimerInitialized = 1;
    for (index = 0; index < State->Count; ++index) {
        PROCESSOR_NUMBER target;
        target.Group = State->Cpus[index].Resource->Row.processorGroup;
        target.Number = State->Cpus[index].Resource->Row.processorNumber; target.Reserved = 0;
        KeInitializeDpc(&watch->Pokes[index], KswWatchPoke, &State->Cpus[index]); ++watch->PokesInitialized;
        if (!NT_SUCCESS(KeSetTargetProcessorDpcEx(&watch->Pokes[index], &target))) { return STATUS_NOT_SUPPORTED; }
    }
    return STATUS_SUCCESS;
}
VOID KswordSvmWatchRelease(KSW_SVM_STATE* State)
{
    /* The caller already proved every processor native before releasing either NPT root. */
    KSW_SVM_WATCH_TABLE* watch = State->Watch;
    ULONG index;
    if (!watch) { return; }
    /* All CPUs are native: queued callbacks can no longer reach any SVM guest instruction. */
    if (watch->TimerInitialized) {
        InterlockedExchange(&watch->Disabled, 1);
        (VOID)KeCancelTimer(&watch->Timer); (VOID)KeRemoveQueueDpc(&watch->TimerDpc);
        /* Drain the producer before removing its targeted children; no new timer can queue another batch. */
        KeFlushQueuedDpcs();
        for (index = 0; index < watch->PokesInitialized; ++index) { (VOID)KeRemoveQueueDpc(&watch->Pokes[index]); }
        /* All producers are gone; active child callbacks finish before any allocation is freed. */
        KeFlushQueuedDpcs();
    }
    for (index = watch->Allocated; index; --index) { MmFreeContiguousMemory(watch->Pages[index - 1U]); }
    if (watch->Pages) { ExFreePoolWithTag(watch->Pages, 'wSvK'); }
    if (watch->Pas) { ExFreePoolWithTag(watch->Pas, 'wSvK'); }
    ExFreePoolWithTag(watch, 'wSvK'); State->Watch = NULL;
}
int KswordSvmWatchArm(void* Context, KSW_SVM_U64 Page)
{
    /* Optional callbacks receive only their prepared per-CPU descriptor. */
    KSW_SVM_NESTED* nested = Context;
    KSW_SVM_WATCH_TABLE* watch = nested->Watch;
    KSW_SVM_WATCH_PAGE* slot;
    volatile LONG64* pte;
    Page &= ~4095ULL;
    if (!watch || watch->Disabled || !KswordSvmNestedRamRange(nested, Page, 4096)) { return 0; }
    slot = KswWatchSlot(watch, Page, TRUE);
    if (!slot) { InterlockedIncrement64(&watch->Declines); return 0; }
    if (slot->State) { return slot->State == 1; }
    /* No exit path waits for another CPU or a preempted publisher. */
    if (InterlockedCompareExchange(&watch->ArmGate, 1, 0)) { return 0; }
    if (!slot->State && !watch->Disabled && watch->Generation < MAXLONGLONG) {
        pte = KswWatchLeaf(watch, Page);
        if (pte) {
            /* The leaf is published before any CPU can acknowledge the corresponding flush generation. */
            slot->Pte = pte; InterlockedExchange(&slot->State, 3); InterlockedAnd64(pte, ~2LL);
            /* Existing RW TLB translations remain untrusted until every participating CPU flushes. */
            InterlockedIncrement64(&watch->Generation);
            /* A concurrent write NPF may already have revoked this arming attempt. */
            if (InterlockedCompareExchange(&slot->State, 1, 3) == 2) { InterlockedOr64(pte, 2LL); }
            InterlockedIncrement64(&watch->Arms);
        } else { InterlockedExchange(&slot->State, 2); InterlockedIncrement64(&watch->Declines); }
    }
    InterlockedExchange(&watch->ArmGate, 0); return slot->State == 1;
}
void KswordSvmWatchBegin(void* Context)
{
    /* Proof availability is frozen for one capture/validation attempt, never refreshed mid-copy. */
    KSW_SVM_NESTED* nested = Context;
    KSW_SVM_WATCH_TABLE* watch = nested->Watch;
    ULONG cpu;
    nested->WatchProof = 0;
    if (!watch || watch->Disabled) { return; }
    nested->WatchProof = (ULONGLONG)watch->Generation;
    /* An inactive/native CPU could write without traps; partial residency therefore never supplies proof. */
    for (cpu = 0; cpu < watch->Owner->Count; ++cpu) {
        if (!watch->Owner->Cpus[cpu].Active || (ULONGLONG)watch->Acknowledged[cpu] != nested->WatchProof) { nested->WatchProof = 0; break; }
    }
}
int KswordSvmWatchStable(void* Context, KSW_SVM_U64 Address)
{
    /* Address/data proof is valid only in the original identity-map lifetime. */
    KSW_SVM_NESTED* nested = Context;
    KSW_SVM_WATCH_TABLE* watch = nested->Watch;
    KSW_SVM_WATCH_PAGE* slot;
    _ReadBarrier();
    if (!watch || !nested->WatchProof || watch->Disabled || (ULONGLONG)watch->Generation != nested->WatchProof) { return 0; }
    slot = KswWatchSlot(watch, Address, FALSE);
    /* Dirty slots are never rearmed by root code, so neither identity nor state can undergo ABA. */
    if (!slot || slot->State != 1 || !slot->Pte) { return 0; }
    /* Check generation again after consuming the slot publication. */
    _ReadBarrier();
    if ((ULONGLONG)watch->Generation != nested->WatchProof) { return 0; }
    if (nested->WatchProofHits != ~0ULL) { ++nested->WatchProofHits; }
    return 1;
}
VOID KswordSvmWatchWrite(KSW_SVM_CPU* Cpu, ULONGLONG HostPa)
{
    /* L0 commits and guest write NPFs both revoke the same prepared identity. */
    KSW_SVM_WATCH_TABLE* watch = KswWatch(Cpu);
    KSW_SVM_WATCH_PAGE* slot;
    if (!watch || !(slot = KswWatchSlot(watch, HostPa, FALSE)) || !slot->Pte) { return; }
    /* Revocation precedes the actual store and permanently prevents unobserved later writes being reused. */
    {
        /* Arming pages also have an owned read-only fault path before global proof publication. */
        LONG previous = InterlockedExchange(&slot->State, 2);
        if (previous == 1 || previous == 3) { InterlockedIncrement64(&watch->Generation); InterlockedIncrement64(&watch->Writes); }
    }
    /* This exact identity-map page may be writable again; no L1 NPT12 permission is widened. */
    InterlockedOr64(slot->Pte, 2LL);
}
void KswordSvmWatchMapping(void* Context, KSW_NMMU_RESULT* Mapping)
{
    /* Any L2 alias of an armed host page must trap CPU writes as well as the L1 identity map. */
    KSW_SVM_NESTED* nested = Context;
    KSW_SVM_WATCH_TABLE* watch = nested->Watch;
    KSW_SVM_WATCH_PAGE* slot;
    ULONG index, shift;
    ULONGLONG base, span;
    if (!watch || watch->Disabled) { return; }
    /* A large NPT02 leaf cannot bypass a protected 4-KiB page anywhere in its covered host span. */
    shift = Mapping->Inner.LeafShift < Mapping->Outer.LeafShift ? Mapping->Inner.LeafShift : Mapping->Outer.LeafShift;
    shift = shift >= 30U ? 30U : shift >= 21U ? 21U : 12U; span = 1ULL << shift; base = Mapping->Outer.Address & ~(span - 1ULL);
    if (shift > 12U) {
        for (index = 0; index < KSW_SVM_WATCH_SLOTS; ++index) {
            LONG64 identity = watch->Slots[index].Identity;
            if (identity && (watch->Slots[index].State == 1 || watch->Slots[index].State == 3) && (ULONGLONG)(identity - 1) >= base && (ULONGLONG)(identity - 1) - base < span) { Mapping->Outer.LeafShift = 12U; break; }
        }
    }
    slot = KswWatchSlot(watch, Mapping->Outer.Address, FALSE);
    /* Keep all source permissions; remove only RW from a known armed destination leaf. */
    if (slot && (slot->State == 1 || slot->State == 3)) { Mapping->Leaf &= ~2ULL; }
}
/* Per-CPU NPT02 edits retire only destination aliases covered by changed protection state. */
static unsigned KswWatchRange(void* Context, KSW_SVM_U64 Base, KSW_SVM_U64 Bytes)
{
    KSW_SVM_WATCH_TABLE* watch = Context;
    ULONG index;
    KSW_SVM_WATCH_PAGE* slot;
    if (Bytes == 4096ULL) {
        slot = KswWatchSlot(watch, Base, FALSE);
        if (!slot || !slot->Pte) { return 0; }
        return slot->State == 2 ? 2U : slot->State ? 1U : 0U;
    }
    for (index = 0; index < KSW_SVM_WATCH_SLOTS; ++index) {
        LONG64 identity = watch->Slots[index].Identity;
        if (identity && watch->Slots[index].Pte && watch->Slots[index].State &&
            (ULONGLONG)(identity - 1) >= Base && (ULONGLONG)(identity - 1) - Base < Bytes) { return 1; }
    }
    return 0;
}
VOID KswordSvmWatchEntry(KSW_SVM_CPU* Cpu)
{
    /* Acknowledgement belongs to this exact CPU's next physical VMRUN. */
    KSW_SVM_WATCH_TABLE* watch = KswWatch(Cpu);
    ULONGLONG generation;
    if (!watch) { return; }
    generation = (ULONGLONG)watch->Generation; Cpu->Nested->WatchEntryGeneration = generation;
    if ((ULONGLONG)watch->Acknowledged[Cpu->Nested->WatchCpuIndex] != generation ||
        (Cpu->Nested->GeneralIo.SharedCache && Cpu->Nested->GeneralIo.SharedCache->WatchGeneration != generation)) {
        /* Retire previously writable L2 aliases before acknowledging any newly protected pages. */
        KswSvmNestedShadowRestrict(Cpu->Nested->GeneralIo.Shadow, KswWatchRange, watch);
        /* Idle shared roots also need permission tightening when later rebound after a migration. */
        if (Cpu->Nested->GeneralIo.SharedCache) { Cpu->Nested->GeneralIo.SharedCache->WatchGeneration = generation; }
        Cpu->NestedTlbControl = 1U;
    }
}
VOID KswordSvmWatchComplete(KSW_SVM_CPU* Cpu, ULONGLONG ExitCode)
{
    /* INVALID does not consume a flush or establish cross-CPU protection. */
    KSW_SVM_WATCH_TABLE* watch = KswWatch(Cpu);
    if (watch && ExitCode != KSW_SVM_EXIT_INVALID && Cpu->NestedTlbControl == 1U) {
        InterlockedExchange64(&watch->Acknowledged[Cpu->Nested->WatchCpuIndex], (LONG64)Cpu->Nested->WatchEntryGeneration);
    }
}
unsigned KswordSvmWatchFault(void* Context, struct _KSW_NSVM_EXECUTION* Execution)
{
    /* This callback runs after interrupt/injection overlays were consumed, with RIP unchanged. */
    KSW_SVM_CPU* cpu = Context;
    KSW_SVM_WATCH_TABLE* watch = KswWatch(cpu);
    ULONGLONG info = KswSvmRead64(Execution->Current, KSW_VMCB_EXITINFO1), page = KswSvmRead64(Execution->Current, KSW_VMCB_EXITINFO2);
    KSW_SVM_WATCH_PAGE* slot;
    KSW_NNPT_WALK walk;
    if (!watch || (info & 3ULL) != 3ULL || (info & 0x18ULL)) { return KSW_NSVM_MACHINE_NOT_CONTROL; }
    if (Execution->Session->Phase == KSW_NSVM_SESSION_L2) {
        /* Resolve the actually installed read-only NPT02 leaf; do not bypass genuine NPT12 faults. */
        if (KswSvmNestedNptWalk(cpu->Nested->GeneralIo.Shadow->Pages[0].Physical, page, cpu->Caps.PhysicalBits,
            cpu->Caps.Page1Gb, 1, 0, KswordSvmNestedRead, cpu->Nested, &walk) != KSW_NNPT_OK) { return KSW_NSVM_MACHINE_NOT_CONTROL; }
        page = walk.Address;
    }
    slot = KswWatchSlot(watch, page, FALSE);
    if (!slot || !slot->Pte || !slot->State) { return KSW_NSVM_MACHINE_NOT_CONTROL; }
    /* Dirty slots may still have stale read-only TLB entries, which also require a retry/flush. */
    KswordSvmWatchWrite(cpu, page); cpu->Nested->FirstEntryFlush = 1U;
    if (Execution->Session->Phase == KSW_NSVM_SESSION_L2) {
        /* Re-resolve from NPT12 rather than granting write access to an L1-read-only mapping. */
        KswSvmNestedShadowRestrict(cpu->Nested->GeneralIo.Shadow, KswWatchRange, watch);
    }
    /* Interrupted event delivery keeps its original token/NRIP contract. */
    return KswSvmNestedResumeNpfEvent(Execution->Current, &Execution->EventEntry,
        Execution->Session->Phase == KSW_NSVM_SESSION_L2 ? Execution->Session->Lease.Token : 0) == KSW_NSVM_EVENT_OK ? KSW_NSVM_MACHINE_READY : KSW_NSVM_MACHINE_FAULT;
}
VOID KswordSvmWatchDisable(KSW_SVM_CPU* Cpu)
{
    /* Once any CPU returns natively it can write without NPT, so global proofs stop immediately. */
    KSW_SVM_WATCH_TABLE* watch = KswWatch(Cpu);
    if (watch && !InterlockedExchange(&watch->Disabled, 1)) { InterlockedIncrement64(&watch->Generation); }
}

/* Ordinary start/stop cycles may reuse allocations only after every CPU acknowledged native execution. */
NTSTATUS KswordSvmWatchReset(KSW_SVM_STATE* State)
{
    KSW_SVM_WATCH_TABLE* watch = State->Watch;
    ULONG cpu, slot;
    if (!watch) { return STATUS_SUCCESS; }
    for (cpu = 0; cpu < State->Count; ++cpu) { if (State->Cpus[cpu].Active) { return STATUS_DEVICE_BUSY; } }
    /* Cached values captured before native execution cannot inherit a newly armed proof next residency. */
    if (State->Caches && State->Caches->Roots) {
        for (slot = 0; slot < State->Caches->Count; ++slot) {
            KSW_SVM_CACHE_ROOT* root = &State->Caches->Roots[slot];
            if (KswSvmNestedShadowReset(&root->Shadow) != KSW_NSHADOW_OK) { return STATUS_INVALID_DEVICE_STATE; }
            RtlZeroMemory(&root->Cache, sizeof(root->Cache)); root->Identity = 0; root->LastCpu = MAXULONG; root->InvalidGeneration = 0;
        }
    }
    if (watch->ArmGate || watch->Generation == MAXLONGLONG) { return STATUS_INVALID_DEVICE_STATE; }
    for (slot = 0; slot < KSW_SVM_WATCH_SLOTS; ++slot) {
        /* Restore the exact identity-map RW permission while no CPU executes either root. */
        if (watch->Slots[slot].Pte) { InterlockedOr64(watch->Slots[slot].Pte, 2LL); }
    }
    RtlZeroMemory(watch->Slots, sizeof(watch->Slots));
    RtlZeroMemory((PVOID)watch->Acknowledged, sizeof(watch->Acknowledged));
    InterlockedIncrement64(&watch->Generation); InterlockedExchange(&watch->Disabled, 0);
    return STATUS_SUCCESS;
}

/* These Windows timer operations execute only in guest lifecycle code, never in VMEXIT. */
VOID KswordSvmWatchStartPokes(KSW_SVM_STATE* State)
{
    LARGE_INTEGER due;
    if (!State || !State->Watch || !State->Watch->TimerInitialized || State->Watch->Disabled) { return; }
    due.QuadPart = -1000000LL;
    (VOID)KeSetTimerEx(&State->Watch->Timer, due, 100, &State->Watch->TimerDpc);
}
VOID KswordSvmWatchStopPokes(KSW_SVM_STATE* State)
{
    if (State && State->Watch && State->Watch->TimerInitialized) { (VOID)KeCancelTimer(&State->Watch->Timer); }
}
