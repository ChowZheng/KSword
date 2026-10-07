/* Permanent VMCB-keyed NPT02 roots shared under the existing exclusive execution lease. */
#pragma once
#include "hvm_svm.h"
#include "hvm_svm_nested_session.h"
typedef struct _KSW_SVM_CACHE_ROOT {
    /* Identity is assigned once; no eviction or ABA occurs while resident. */
    volatile LONG64 Identity;
    KSW_NSVM_CACHE Cache;
    KSW_NSHADOW Shadow;
    KSW_NSHADOW_PAGE Pages[64];
    /* Physical tables and provenance follow a virtual VMCB across host CPU scheduling. */
    ULONG Allocated, LastCpu;
    ULONGLONG InvalidGeneration;
} KSW_SVM_CACHE_ROOT;
typedef struct _KSW_SVM_CACHE_DOMAIN {
    KSW_SVM_CACHE_ROOT* Roots;
    ULONG Count;
    /* INVLPGA retires root reuse without touching another CPU's active page tables. */
    volatile LONG64 InvalidGeneration;
    /* Counter saturation permanently falls back to rebuilding, never to an earlier generation. */
    volatile LONG Saturated;
} KSW_SVM_CACHE_DOMAIN;
NTSTATUS KswordSvmCachePrepare(KSW_SVM_STATE* State);
VOID KswordSvmCacheRelease(KSW_SVM_STATE* State);
/* Called only with the actual physical VMCB execution lease held. */
void KswordSvmCacheBind(void* Context, KSW_NSVM_SESSION_IO* Io, const KSW_NSVM_LEASE* Lease);
void KswordSvmCacheUnbind(void* Context, KSW_NSVM_SESSION_IO* Io);
void KswordSvmCacheInvalidate(void* Context);
/* Current root changes require hardware ASID flush even when its software composition remains valid. */
VOID KswordSvmCacheEntry(KSW_SVM_CPU* Cpu);
VOID KswordSvmCacheComplete(KSW_SVM_CPU* Cpu, ULONGLONG ExitCode);
