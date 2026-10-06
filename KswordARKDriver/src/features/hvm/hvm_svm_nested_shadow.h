/* CPU-owned sparse NPT02 using only pages allocated before residency. */
#pragma once
#include "hvm_svm_nested_mmu.h"

/* Distinguish budget exhaustion from corrupt ownership and stale translation. */
#define KSW_NSHADOW_OK 0U
/* The caller may recycle only while the owning CPU is outside its nested guest. */
#define KSW_NSHADOW_FULL 1U
/* Invalid input or ownership must never produce a hardware table pointer. */
#define KSW_NSHADOW_INVALID 2U
/* A changed epoch requires resolving the source paths again. */
#define KSW_NSHADOW_STALE 3U
/* Bound lookup cost and table memory to one MiB per prepared CPU. */
#define KSW_NSHADOW_MAX_PAGES 256U
/* Distinct committed NPT12 source words; overflow falls back to whole-root synchronization. */
#define KSW_NSHADOW_SOURCE_WORDS 512U

/* The allocator owns these pages; this module neither allocates nor frees them. */
typedef struct _KSW_NSHADOW_PAGE {
    /* Nonpaged, 4-KiB aligned kernel mapping. */
    KSW_SVM_U64* Words;
    /* Validated physical address within the CPU's physical-width contract. */
    KSW_SVM_U64 Physical;
} KSW_NSHADOW_PAGE;

/* Access is serialized by CPU ownership, never by waiting on an exit-path lock. */
typedef struct _KSW_NSHADOW {
    /* Stable allocation ledger captured during prepare. */
    KSW_NSHADOW_PAGE* Pages;
    /* No published entry may refer beyond Used. */
    unsigned int Capacity, Used;
    /* Nonzero means the next hardware VMRUN must flush its TLB. */
    unsigned int FlushPending;
    /* Candidate leaves must name exactly this generation. */
    KSW_SVM_U64 Epoch;
    /* Physical and GPA widths are deliberately limited to four-level NPT. */
    KSW_SVM_U64 AddressMask;
    /* Source GPA/value pairs preserve the provenance of every published leaf. */
    KSW_SVM_U64 SourceAddress[KSW_NSHADOW_SOURCE_WORDS], SourceValue[KSW_NSHADOW_SOURCE_WORDS];
    /* Untracked provenance can never authorize reuse across a virtual invalidation. */
    unsigned SourceCount, SourceUntracked;
} KSW_NSHADOW;

/* Preparation only: verifies all mappings/physical frames and creates an empty root. */
unsigned int KswSvmNestedShadowInitialize(KSW_NSHADOW* Shadow,
    KSW_NSHADOW_PAGE* Pages, unsigned int Count, unsigned int PhysicalBits);
/* Owning CPU must be outside VMRUN, and must request a hardware flush before reentry. */
unsigned int KswSvmNestedShadowReset(KSW_NSHADOW* Shadow);
/* Installs a fully committed MMU result; a full pool leaves the tables unchanged. */
unsigned int KswSvmNestedShadowInstall(KSW_NSHADOW* Shadow, const KSW_NMMU_RESULT* Result);
/* Revalidate all captured NPT12 words; unchanged mappings may survive an ASID flush. */
int KswSvmNestedShadowSourcesMatch(const KSW_NSHADOW* Shadow,
    KSW_NNPT_READ ReadGuestWord, void* Context);
