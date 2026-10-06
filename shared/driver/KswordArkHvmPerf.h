/* Pointer-free metrics for sampled root software work; not pure hardware VMEXIT latency. */
#pragma once
#define KSW_HVM_PERF_BUCKETS 12U
#define KSW_HVM_PERF_STAGES 5U
#define KSW_HVM_PERF_DETAILS 7U
#define KSW_HVM_PERF_FETCH 0U
#define KSW_HVM_PERF_OPERAND 1U
#define KSW_HVM_PERF_MAPS 2U
#define KSW_HVM_PERF_SYNC 3U
#define KSW_HVM_PERF_WRITEBACK 4U
#define KSW_HVM_PERF_WALK 5U
#define KSW_HVM_PERF_INSTALL 6U
typedef struct _KSWORD_HVM_PERF_ROW {
    /* Exact sample count, root interval sum and largest observed interval. */
    unsigned long long samples, cycles, maximum;
    /* XSTATE save, host-state restore, exit dispatch, entry coordination, guest-state restore. */
    unsigned long long stages[KSW_HVM_PERF_STAGES];
    /* Nonoverlapping leaf work inside dispatch; excluded from additive top-level stages. */
    unsigned long long details[KSW_HVM_PERF_DETAILS];
} KSWORD_HVM_PERF_ROW;
typedef struct _KSWORD_HVM_PERF_METRICS {
    /* Valid requires an even coherent writer sequence; saturated records cannot yield deltas. */
    unsigned long valid, saturated;
    /* Sequence and mask belong to one prepared lifetime; mask=63 selects one in 64 exits. */
    unsigned long long sequence, sampleMask;
    /* Actual hardware-entry TLB_CONTROL requests, distinct from virtual L1 requests. */
    unsigned long long tlbIssued[4];
    /* Stable reason buckets, separately charged to the raw exiting L1/L2 level. */
    KSWORD_HVM_PERF_ROW rows[2][KSW_HVM_PERF_BUCKETS];
} KSWORD_HVM_PERF_METRICS;
