/* Root identity and lease lifetime are separate from Windows logical CPU identity. */
#include "hvm_svm_cache.h"
#include "hvm_svm_nested_runtime.h"
#include "hvm_svm_watch.h"
#include "../../platform/pool_compat.h"
NTSTATUS KswordSvmCachePrepare(KSW_SVM_STATE* State)
{
    KSW_SVM_CACHE_DOMAIN* domain;
    PHYSICAL_ADDRESS highest;
    ULONG root, page;
    /* Identity root, watch clone/splits, CPU-local fallback roots and shared roots together stay within 64 MiB. */
    ULONG identityPages = State->Npt.PageCount * 2U + KSW_SVM_WATCH_SPLITS;
    if (State->Count > (KSW_NPT_MAX_PAGES - identityPages) / (3U * 64U)) { return STATUS_INSUFFICIENT_RESOURCES; }
    domain = KswordARKAllocateNonPagedPool(sizeof(*domain), 'kSvK');
    if (!domain) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(domain, sizeof(*domain)); State->Caches = domain; domain->Count = State->Count * 2U;
    domain->Roots = KswordARKAllocateNonPagedPool(sizeof(*domain->Roots) * domain->Count, 'kSvK');
    if (!domain->Roots) { return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(domain->Roots, sizeof(*domain->Roots) * domain->Count);
    highest.QuadPart = (LONGLONG)(State->Npt.Limit - 1ULL); domain->InvalidGeneration = 1;
    for (root = 0; root < domain->Count; ++root) {
        KSW_SVM_CACHE_ROOT* cache = &domain->Roots[root];
        cache->LastCpu = MAXULONG;
        for (page = 0; page < 64U; ++page) {
            cache->Pages[page].Words = MmAllocateContiguousMemory(4096, highest);
            if (!cache->Pages[page].Words) { return STATUS_INSUFFICIENT_RESOURCES; }
            ++cache->Allocated;
            cache->Pages[page].Physical = (ULONGLONG)MmGetPhysicalAddress(cache->Pages[page].Words).QuadPart;
        }
        if (KswSvmNestedShadowInitialize(&cache->Shadow, cache->Pages, 64U, State->Cpus[0].Caps.PhysicalBits)) { return STATUS_DATA_ERROR; }
    }
    return STATUS_SUCCESS;
}
VOID KswordSvmCacheRelease(KSW_SVM_STATE* State)
{
    KSW_SVM_CACHE_DOMAIN* domain = State->Caches;
    ULONG root, page;
    if (!domain) { return; }
    if (domain->Roots) {
        for (root = 0; root < domain->Count; ++root) {
            for (page = domain->Roots[root].Allocated; page; --page) { MmFreeContiguousMemory(domain->Roots[root].Pages[page - 1U].Words); }
        }
        ExFreePoolWithTag(domain->Roots, 'kSvK');
    }
    ExFreePoolWithTag(domain, 'kSvK'); State->Caches = NULL;
}
void KswordSvmCacheBind(void* Context, KSW_NSVM_SESSION_IO* Io, const KSW_NSVM_LEASE* Lease)
{
    KSW_SVM_NESTED* nested = Context;
    KSW_SVM_STATE* state = nested->Cpu->Runtime->BackendContext;
    KSW_SVM_CACHE_DOMAIN* domain = state->Caches;
    KSW_SVM_CACHE_ROOT* chosen = NULL;
    ULONG root;
    /* Always start from the private fallback; failure to claim a shared slot cannot block guest execution. */
    Io->Shadow = &nested->Shadow; Io->SharedCache = NULL; nested->SharedRoot = NULL;
    if (!domain || !Lease->Token) { return; }
    /* The slot's live execution token authenticates this bind rather than a remote CPU state count. */
    if ((ULONGLONG)InterlockedCompareExchange64(&Io->Owners->Slots[Lease->Slot].Token, 0, 0) != Lease->Token) { return; }
    for (root = 0; root < domain->Count; ++root) {
        LONG64 identity = InterlockedCompareExchange64(&domain->Roots[root].Identity, 0, 0);
        if ((ULONGLONG)identity == Lease->HostPa + 1ULL) { chosen = &domain->Roots[root]; break; }
    }
    if (!chosen) {
        for (root = 0; root < domain->Count; ++root) {
            if (!InterlockedCompareExchange64(&domain->Roots[root].Identity, (LONG64)(Lease->HostPa + 1ULL), 0)) { chosen = &domain->Roots[root]; break; }
        }
    }
    /* A full permanent registry preserves the per-CPU software path without eviction races. */
    if (!chosen) { return; }
    if (chosen->LastCpu != MAXULONG && chosen->LastCpu != Io->CpuIdentity && nested->CacheMigrations != ~0ULL) { ++nested->CacheMigrations; }
    chosen->LastCpu = Io->CpuIdentity; nested->SharedRoot = chosen;
    /* Apply invalidation before capture/key publication instead of discarding freshly built work on first entry. */
    {
        ULONGLONG generation = (ULONGLONG)InterlockedCompareExchange64(&domain->InvalidGeneration, 0, 0);
        if (domain->Saturated || chosen->InvalidGeneration != generation) {
            if (KswSvmNestedShadowReset(&chosen->Shadow) != KSW_NSHADOW_OK) { nested->SharedRoot = NULL; return; }
            chosen->Cache.Valid = 0; chosen->InvalidGeneration = generation;
        }
    }
    Io->Shadow = &chosen->Shadow; Io->SharedCache = &chosen->Cache;
    nested->LastSharedRootPa = chosen->Pages[0].Physical;
}
void KswordSvmCacheUnbind(void* Context, KSW_NSVM_SESSION_IO* Io)
{
    KSW_SVM_NESTED* nested = Context;
    /* Snapshot diagnostics while the root is still protected by its real execution lease. */
    nested->LastShadowPages = Io->Shadow->Used; nested->LastShadowEpoch = Io->Shadow->Epoch;
    nested->LastDependencyRetirements = Io->Shadow->DependencyRetirements;
    /* Remote queries on an idle CPU must never read a shared root now owned by another CPU. */
    Io->Shadow = &nested->Shadow; Io->SharedCache = NULL; nested->SharedRoot = NULL;
}
void KswordSvmCacheInvalidate(void* Context)
{
    KSW_SVM_NESTED* nested = Context;
    KSW_SVM_CACHE_DOMAIN* domain = ((KSW_SVM_STATE*)nested->Cpu->Runtime->BackendContext)->Caches;
    if (domain) {
        /* Other holders observe this only on their own CPU, never through remotely edited active tables. */
        if (InterlockedIncrement64(&domain->InvalidGeneration) <= 0) { InterlockedExchange(&domain->Saturated, 1); KswordSvmWatchDisable(nested->Cpu); }
        /* Break integer fast loops so each CPU reaches its complete hardware-flush coordinator. */
        if (nested->Watch) { InterlockedIncrement64(&nested->Watch->Generation); }
    }
}
VOID KswordSvmCacheEntry(KSW_SVM_CPU* Cpu)
{
    KSW_SVM_NESTED* nested = Cpu->Nested;
    if (nested->SharedRoot) {
        KSW_SVM_CACHE_DOMAIN* domain = ((KSW_SVM_STATE*)Cpu->Runtime->BackendContext)->Caches;
        ULONGLONG generation = (ULONGLONG)InterlockedCompareExchange64(&domain->InvalidGeneration, 0, 0);
        if (domain->Saturated || nested->SharedRoot->InvalidGeneration != generation) {
            /* The exclusive VMCB lease also protects the shared software composition root. */
            if (KswSvmNestedShadowReset(nested->GeneralIo.Shadow) != KSW_NSHADOW_OK) { nested->GeneralMachine.LastAction = KSW_NSVM_MACHINE_FAULT; return; }
            nested->SharedRoot->Cache.Valid = 0; nested->SharedRoot->InvalidGeneration = generation;
            nested->Config.Epoch = nested->GeneralIo.Shadow->Epoch; Cpu->NestedTlbControl = 1U;
        }
    }
    nested->EntryNptRoot = KswSvmRead64(Cpu->Guest, KSW_VMCB_NCR3);
    /* Separate ASID 1/L1 and private ASID/L2 retain independent hardware root provenance. */
    nested->EntryNptLevel = nested->Session.Phase == KSW_NSVM_SESSION_L2;
    /* Identical physical ASIDs cannot retain translations from another VMCB02/NPT02 root. */
    if (nested->EntryNptRoot != nested->LastNptRoot[nested->EntryNptLevel]) { Cpu->NestedTlbControl = 1U; }
}
VOID KswordSvmCacheComplete(KSW_SVM_CPU* Cpu, ULONGLONG ExitCode)
{
    if (ExitCode != KSW_SVM_EXIT_INVALID) { Cpu->Nested->LastNptRoot[Cpu->Nested->EntryNptLevel] = Cpu->Nested->EntryNptRoot; }
}
