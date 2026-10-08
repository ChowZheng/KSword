/* CPU-private VMRUN transaction. Scheduling, virtual GIF and event delivery remain external. */
#pragma once
#include "hvm_svm_nested_entry.h"
#include "hvm_svm_nested_writeback.h"
#include "hvm_svm_nested_shadow.h"
#include "hvm_svm_nested_owner.h"
#include "../../../../shared/driver/KswordArkHvmNptCacheStats.h"
#include "../../../../shared/driver/KswordArkHvmPerf.h"

/* Phases describe retained ownership, not public resident capability. */
#define KSW_NSVM_SESSION_IDLE 0U
#define KSW_NSVM_SESSION_L2 1U
#define KSW_NSVM_SESSION_FAULTED 2U
/* Actions distinguish architectural return from failure of the monitor's implementation. */
#define KSW_NSVM_ACTION_ENTER 0U
#define KSW_NSVM_ACTION_RETURN 1U
#define KSW_NSVM_ACTION_INVALID 2U
#define KSW_NSVM_ACTION_UNSUPPORTED 3U
#define KSW_NSVM_ACTION_FAULT 4U

/* Shared metadata is mutated only under the VMCB execution lease, independent of host CPU placement. */
typedef struct _KSW_NSVM_CACHE {
    KSW_SVM_U64 Key[13], Epoch, OwnerToken, WatchGeneration;
    unsigned Valid;
} KSW_NSVM_CACHE;

typedef struct _KSW_NSVM_SESSION {
    /* All images are owned nonpaged storage, never a mapped guest pointer. */
    KSW_SVM_VMCB Vmcb12, L1;
    /* General residency retains the physical L1 image instead of copying its whole page. */
    KSW_SVM_VMCB* HostImage;
    /* The retained hardware L1 page is advanced only after the virtual instruction transaction commits. */
    KSW_SVM_U64 HostNextRip;
    /* Original inner permission maps are retained for exit ownership decisions. */
    KSW_NSVM_PERMISSION_IMAGE Permissions;
    /* Both identities bind reflection to exactly the operand captured at entry. */
    KSW_SVM_U64 OperandPa, OperandHostPa;
    /* A live lease persists across hardware execution and partial architectural writeback. */
    KSW_NSVM_LEASE Lease;
    /* Busy, exhausted and invalid ownership are retained separately from VMCB admission. */
    unsigned OwnerStatus;
    /* Virtual invalidations are distinct from assembly's physical flush-on-every-entry counter. */
    KSW_SVM_U64 Invalidations, LastInvalidationLinear, LastInvalidationEpoch;
    /* Guest ASIDs are diagnostic inputs; physical ASIDs are always owned separately by L0. */
    unsigned LastInvalidationAsid;
    /* Completion counters include only committed transitions. */
    KSW_SVM_U64 Entries, Returns, InvalidEntries;
    /* Entry failure and physical IO failure must remain distinguishable. */
    KSW_NSVM_ENTRY_RESULT Admission;
    KSW_NSVM_OPERAND_RESULT OperandResult;
    /* Caller must hold resources if a physical commit failed or L2 still owns the CPU. */
    unsigned int Phase, VirtualGif;
    KSW_SVM_U64 CacheKey[13], CacheEpoch, CacheOwnerToken;
    unsigned CacheValid;
    KSWORD_HVM_NPT_CACHE_STATS CacheStats;
    /* A virtual VMRUN flush request remains pending until a real successful hardware entry. */
    unsigned PendingTlbControl;
    /* Every rewritten merged bitmap starts a new hardware permission-cache generation. */
    KSW_SVM_U64 PermissionsGeneration;
    /* Permission captures have their own protected-page identity and fresh-baseline proof. */
    KSW_SVM_U64 PermissionKey[4];
    unsigned PermissionsReusable;
} KSW_NSVM_SESSION;

typedef struct _KSW_NSVM_SESSION_IO {
    /* Trusted outer capability and translation evidence, frozen before root entry. */
    KSW_NSVM_ENTRY_POLICY Policy;
    KSW_NSVM_OPERAND_IO Operand;
    KSW_NSVM_COMMIT_VMCB Commit;
    /* One shared identity domain for all CPUs using this immutable NPT01 lifetime. */
    KSW_NSVM_OWNER_TABLE* Owners;
    /* Windows processor group:number identity frozen by prepare. */
    unsigned CpuIdentity;
    /* Optional for fixed probes; general execution persists deferred events under the VMCB lease. */
    KSW_NSVM_PENDING* Pending;
    /* Per-CPU preallocated output maps and their already validated host identities. */
    unsigned char* MergedMsr;
    unsigned char* MergedIo;
    KSW_SVM_U64 MsrPa, IoPa;
    unsigned int Asid;
    /* L0 maps are immutable; L1 cannot weaken any bit while requesting its own exits. */
    KSW_NSVM_PERMISSION_VIEW OuterPermissions;
    /* No allocation or shared-cache lock is taken by a nested transition. */
    KSW_NSHADOW* Shadow;
    KSW_NMMU_CONFIG* Mmu;
    unsigned ReuseNpt;
    /* Optional preallocated page for one-call source checks; never proof across VMRUNs. */
    unsigned char* SourceSyncPage;
    /* Optional independent physical VMCBs; bounded probes keep the original in-place contract. */
    KSW_SVM_VMCB* StableL1;
    KSW_SVM_VMCB* StableL2;
    /* Optional CPU-write provenance, never supplied by the virtual VMM. */
    void (*BeginWatch)(void* Context);
    int (*ArmSource)(void* Context, KSW_SVM_U64 Page);
    int (*SourceStable)(void* Context, KSW_SVM_U64 Address);
    void (*ProtectMapping)(void* Context, KSW_NMMU_RESULT* Mapping);
    /* An exhausted shared cache registry leaves Shadow on its private fallback. */
    void (*BindCache)(void* Context, struct _KSW_NSVM_SESSION_IO* Io, const KSW_NSVM_LEASE* Lease);
    void (*UnbindCache)(void* Context, struct _KSW_NSVM_SESSION_IO* Io);
    void (*InvalidateCaches)(void* Context);
    KSW_NSVM_CACHE* SharedCache;
    /* Optional root clock and sampled row; unset in ordinary execution and portable fixtures. */
    KSW_SVM_U64 (*PerfClock)(void* Context);
    /* Output belongs to the current odd per-CPU sample sequence, never a retained guest pointer. */
    KSWORD_HVM_PERF_ROW* PerfRow;
    /* Diagnostic saturation cannot affect architectural execution. */
    KSW_SVM_U64* PerfSaturated;
} KSW_NSVM_SESSION_IO;

/* The fixed probe still uses its embedded image; general execution keeps VMCB01 live. */
static __inline KSW_SVM_VMCB* KswSvmNestedHostImage(const KSW_NSVM_SESSION* Session)
{
    /* A null binding is the original portable/probe continuation contract. */
    return Session->HostImage ? Session->HostImage : (KSW_SVM_VMCB*)&Session->L1;
}

/* No timestamp instruction executes when profiling is disabled or this exit is unselected. */
static __inline KSW_SVM_U64 KswSvmPerfBegin(const KSW_NSVM_SESSION_IO* Io)
{
    /* The callback uses the same CPU-private context as the operand transport. */
    return Io && Io->PerfRow && Io->PerfClock ? Io->PerfClock(Io->Operand.Context) : 0;
}

/* Detail sums are nested in dispatch; consumers must not add them to the five root stages. */
static __inline void KswSvmPerfEnd(const KSW_NSVM_SESSION_IO* Io, unsigned Detail, KSW_SVM_U64 Begin)
{
    /* Missing optional telemetry does not modify the instruction result. */
    KSW_SVM_U64 end, delta;
    /* Only complete descriptors can invoke the clock or address a public row. */
    if (!Io || !Io->PerfRow || !Io->PerfClock || Detail >= KSW_HVM_PERF_DETAILS) { return; }
    /* Serializing boundaries are supplied by the platform, not by portable transaction logic. */
    end = Io->PerfClock(Io->Operand.Context);
    /* Reject counter wrap and backwards clocks rather than publishing misleading durations. */
    delta = end - Begin;
    /* Overflow flags invalidate timing evidence but never fault the guest. */
    if (end < Begin || Io->PerfRow->details[Detail] > ~0ULL - delta) {
        /* Saturation remains sticky for the prepared lifetime. */
        if (Io->PerfSaturated) { *Io->PerfSaturated = 1; }
        /* Preserve the last valid total. */
        return;
    }
    /* One selected leaf operation contributes exactly once. */
    Io->PerfRow->details[Detail] += delta;
}

/* Virtual instruction legality/EFER/HSAVE/GIF are checked by the dispatcher first.
   Session must start zeroed. On FAULT, preserve current image and all resources. */
unsigned int KswSvmNestedSessionEnter(KSW_NSVM_SESSION* Session,
    KSW_NSVM_SESSION_IO* Io, KSW_SVM_VMCB* Current, KSW_SVM_U64 OperandPa);
/* Reflect a real or synthesized inner-owned exit before resuming the saved L1 continuation. */
unsigned int KswSvmNestedSessionReflect(KSW_NSVM_SESSION* Session,
    const KSW_NSVM_SESSION_IO* Io, KSW_SVM_VMCB* Current);
/* VMLOAD/VMSAVE use the same translated ownership domain but never start an inner VMRUN.
   Caller checks virtual SVME/CPL/instruction legality and advances RIP only on RETURN. */
unsigned int KswSvmNestedSessionTransfer(KSW_NSVM_SESSION* Session,
    const KSW_NSVM_SESSION_IO* Io, KSW_SVM_VMCB* Current, KSW_SVM_U64 OperandPa,
    unsigned Save);
/* INVLPGA may invalidate more translations than requested. This first implementation
   discards every cached composition on this virtual CPU, independently of guest ASID. */
unsigned int KswSvmNestedSessionInvalidate(KSW_NSVM_SESSION* Session,
    KSW_NSVM_SESSION_IO* Io, KSW_SVM_U64 Linear, unsigned Asid);

/* Select an owned physical flush without consuming virtual or shadow requests prematurely. */
unsigned KswSvmNestedSessionTlbSelect(const KSW_NSVM_SESSION* Session,
    const KSW_NSHADOW* Shadow, unsigned FlushByAsid);
/* Only a non-INVALID hardware VMEXIT proves the selected entry and flush completed. */
void KswSvmNestedSessionTlbComplete(KSW_NSVM_SESSION* Session,
    KSW_NSHADOW* Shadow, unsigned Issued, KSW_SVM_U64 ExitCode);

/* Bound writeback timing includes failed physical commits without changing ownership policy. */
static __inline unsigned KswSvmPerfWriteback(const KSW_NSVM_SESSION_IO* Io,
    KSW_SVM_U64 Pa, KSW_SVM_U64 HostPa, const KSW_SVM_VMCB* Source,
    unsigned Operation, KSW_NSVM_OPERAND_RESULT* Result)
{
    /* A disabled profile produces no clock reads. */
    KSW_SVM_U64 tick = KswSvmPerfBegin(Io);
    /* Reuse the exact architectural whitelist and commit callbacks. */
    unsigned status = KswSvmNestedWriteback(&Io->Operand, Pa, HostPa, Source,
        Operation, 1, Io->Commit, Result);
    /* Failed and partial commits are retained in the timing evidence. */
    KswSvmPerfEnd(Io, KSW_HVM_PERF_WRITEBACK, tick);
    /* The caller still decides whether the owner must remain retained. */
    return status;
}
