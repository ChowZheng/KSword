/* Integer-only sampled timing storage; fixed offsets are consumed by MASM. */
#pragma once
#include "hvm_svm_arch.h"
#include "../../../../shared/driver/KswordArkHvmPerf.h"
typedef struct _KSW_SVM_PERF {
    /* Sampling configuration and monotonically selected exit ordinal. */
    KSW_SVM_U64 Mask, Ordinal;
    /* Odd while one selected root interval is being observed. */
    volatile KSW_SVM_U64 Sequence, Selected;
    /* Six boundaries span the five public root software stages. */
    KSW_SVM_U64 Timestamps[6];
    /* The destination row is bound before exit dispatch can change the running layer. */
    KSWORD_HVM_PERF_ROW* Row;
    /* Saturation is published independently from any hardware execution failure. */
    KSW_SVM_U64 Saturated;
    /* Public output contains no internal pointers or raw assembly temporaries. */
    KSWORD_HVM_PERF_METRICS Metrics;
} KSW_SVM_PERF;
/* Fail compilation rather than letting assembly write into a changed C layout. */
typedef char KSW_PERF_ASSERT_TIMES[(offsetof(KSW_SVM_PERF,Timestamps)==32)?1:-1];
/* The last timestamp precedes the output row pointer at byte eighty. */
typedef char KSW_PERF_ASSERT_ROW[(offsetof(KSW_SVM_PERF,Row)==80)?1:-1];
/* Assembly writes only the unchanged first sixty-four bytes of each public row. */
typedef char KSW_PERF_ASSERT_DETAILS[(offsetof(KSWORD_HVM_PERF_ROW,details)==64)?1:-1];
/* Preserve sparse AMD exit codes without treating them as array indices. */
static __inline unsigned KswSvmPerfBucket(KSW_SVM_U64 Code)
{
    /* Unknown reasons are retained in the final bucket. */
    static const KSW_SVM_U64 codes[KSW_HVM_PERF_BUCKETS-1] = {0x7c,0x8d,0x82,0x83,0x84,0x85,0x80,0x72,0x400,0x7b,0x60};
    /* At most eleven integer comparisons are required per sampled exit. */
    unsigned index;
    /* No hardware state or SIMD register is accessed here. */
    for(index=0;index<KSW_HVM_PERF_BUCKETS-1;++index){if(codes[index]==Code){return index;}}
    /* The final bucket accounts for all remaining architectural reasons. */
    return KSW_HVM_PERF_BUCKETS-1;
}
