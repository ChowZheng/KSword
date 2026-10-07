/* Shared CPU-write provenance for transparent NPT12/permission caches. */
#pragma once
#include "hvm_svm.h"
#include "hvm_svm_nested_mmu.h"
#define KSW_SVM_WATCH_SLOTS 1024U
#define KSW_SVM_WATCH_SPLITS 256U
typedef struct _KSW_SVM_WATCH_PAGE {
    /* Zero means vacant; page+1 allows a real physical page zero without ABA reuse. */
    volatile LONG64 Identity;
    /* 0 reserved, 1 armed read-only, 2 permanently dirty/untracked until all-native reset. */
    volatile LONG State;
    /* Leaf belongs to the private hardware NPT01 clone and is never released while resident. */
    volatile LONG64* Pte;
} KSW_SVM_WATCH_PAGE;
typedef struct _KSW_SVM_WATCH_TABLE {
    /* Original software translation root remains immutable; only the private hardware clone is edited. */
    KSW_SVM_STATE* Owner;
    PVOID* Pages;
    ULONGLONG* Pas;
    ULONG BaseCount, Allocated, Used, Capacity;
    ULONGLONG RootPa;
    /* Arm is try-only; write-fault processing never acquires or waits for this gate. */
    volatile LONG ArmGate, Disabled;
    /* New protections and write faults invalidate every previously published global proof. */
    volatile LONG64 Generation;
    volatile LONG64 Acknowledged[KSWORD_ARK_HVM_MAX_PROCESSORS];
    KSW_SVM_WATCH_PAGE Slots[KSW_SVM_WATCH_SLOTS];
    /* Low-frequency private diagnostics; no new public IOCTL structures are duplicated here. */
    volatile LONG64 Arms, Writes, Declines;
    /* Guest-side targeted DPCs obtain acknowledgements without root waits or a synchronous broadcast. */
    KTIMER Timer;
    KDPC TimerDpc, Pokes[KSWORD_ARK_HVM_MAX_PROCESSORS];
    ULONG TimerInitialized, PokesInitialized;
    volatile LONG64 PokeCalls;
} KSW_SVM_WATCH_TABLE;
/* Preparation/release execute only under the existing all-native resource owner. */
NTSTATUS KswordSvmWatchPrepare(KSW_SVM_STATE* State);
VOID KswordSvmWatchRelease(KSW_SVM_STATE* State);
/* No callback allocates, waits, calls Windows handlers, or retains physical-window mappings. */
int KswordSvmWatchArm(void* Context, KSW_SVM_U64 Page);
void KswordSvmWatchBegin(void* Context);
int KswordSvmWatchStable(void* Context, KSW_SVM_U64 Address);
void KswordSvmWatchMapping(void* Context, KSW_NMMU_RESULT* Mapping);
/* Explicit L0 writes must revoke CPU-write provenance before touching monitored RAM. */
VOID KswordSvmWatchWrite(KSW_SVM_CPU* Cpu, ULONGLONG HostPa);
/* Entry flushes are acknowledged only after a real non-INVALID hardware return. */
VOID KswordSvmWatchEntry(KSW_SVM_CPU* Cpu);
VOID KswordSvmWatchComplete(KSW_SVM_CPU* Cpu, ULONGLONG ExitCode);
/* Called after event overlays are restored; returns NOT_CONTROL for unowned NPFs. */
struct _KSW_NSVM_EXECUTION;
unsigned KswordSvmWatchFault(void* Context, struct _KSW_NSVM_EXECUTION* Execution);
/* A native CPU no longer traps writes, so all proofs must retire before its first native instruction. */
VOID KswordSvmWatchDisable(KSW_SVM_CPU* Cpu);
/* Reset only under the all-native resident transition; no live identity ever undergoes ABA. */
NTSTATUS KswordSvmWatchReset(KSW_SVM_STATE* State);
/* Guest-only timer lifecycle is coupled to common active/stop/release transitions. */
VOID KswordSvmWatchStartPokes(KSW_SVM_STATE* State);
VOID KswordSvmWatchStopPokes(KSW_SVM_STATE* State);
