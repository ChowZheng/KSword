/* CPU-private hardware acceleration; never trust VMCB12 clean bits or real SVM MSRs. */
#pragma once
#include "hvm_svm_arch.h"
#define KSW_SVM_VGIF (1ULL << 9)
#define KSW_SVM_VGIF_ENABLE (1ULL << 25)
typedef struct _KSW_SVM_ACCEL {
    /* Capability/option gates are fixed by the admitted current processor. */
    unsigned Features, Enabled, Applied, VlsActive;
    /* Raw intercepts must be restored before nested routing sees the hardware result. */
    unsigned Misc2;
    KSW_SVM_U64 MiscCtl2;
    /* Last physical operand must match before hardware clean-cache reuse is allowed. */
    KSW_SVM_U64 LastPa, Controls[9];
    unsigned CleanValid, CleanMask;
    /* Equal physical pointers alone do not prove unchanged permission-map contents. */
    KSW_SVM_U64 MapsGeneration, LastMapsGeneration;
    /* Entry counters are private diagnostics, not claims of guest instruction execution. */
    KSW_SVM_U64 VlsEntries, VgifEntries, CleanEntries;
} KSW_SVM_ACCEL;
/* Select only L1 VLS; deeper SVM remains intercepted by the ordinary owner policy. */
void KswSvmAccelApply(KSW_SVM_ACCEL* Accel, KSW_SVM_VMCB* Current,
    unsigned Inner, KSW_SVM_U64 VirtualEfer, unsigned HardwareGif);
/* Remove only this monitor's VLS/STGI overlays before normal exit ownership dispatch. */
int KswSvmAccelRestore(KSW_SVM_ACCEL* Accel, KSW_SVM_VMCB* Current);
/* Whitelisted clean groups use exact comparisons of private prior-entry controls. */
unsigned KswSvmAccelCleanPrepare(KSW_SVM_ACCEL* Accel, const KSW_SVM_VMCB* Current, KSW_SVM_U64 Pa);
/* INVALID is never evidence that hardware consumed a new control image. */
void KswSvmAccelComplete(KSW_SVM_ACCEL* Accel, KSW_SVM_U64 ExitCode);
