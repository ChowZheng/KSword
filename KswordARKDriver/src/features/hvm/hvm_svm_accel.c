/* Hardware acceleration is separate from guest-advertised SVM extension support. */
#include "hvm_svm_accel.h"
void KswSvmAccelApply(KSW_SVM_ACCEL* Accel, KSW_SVM_VMCB* Current,
    unsigned Inner, KSW_SVM_U64 VirtualEfer, unsigned HardwareGif)
{
    /* Never remove a source intercept when no matching prepared hardware contract exists. */
    unsigned misc;
    /* Hardware vGIF does not mask physical interrupts for this transparent Windows host. */
    (void)HardwareGif;
    /* This overlay is consumed exactly once after real hardware return. */
    Accel->Misc2 = (unsigned)KswSvmRead64(Current, KSW_VMCB_MISC2);
    /* Keep extension ownership separate from the baseline intercept dword. */
    Accel->MiscCtl2 = KswSvmRead64(Current, 0x0b8U); Accel->Applied = 1; Accel->VlsActive = 0;
    /* Physical VLS requires long mode, NPT, support, and virtual SVME. */
    misc = Accel->Misc2;
    /* No L2 SVM passthrough is granted by this physical-only optimization. */
    if (Accel->Enabled && !Inner && (VirtualEfer & KSW_SVM_EFER_SVME)) {
        /* Preserve software VMLOAD for legacy-mode guests and hardware without VLS. */
        if ((Accel->Features & (1U << 15)) && KswSvmRead64(Current, KSW_VMCB_NP) == 1ULL &&
            (KswSvmRead64(Current, KSW_VMCB_EFER) & (1ULL << 10)) &&
            (((const unsigned char*)Current)[KSW_VMCB_CS + 3U] & 2U)) {
            /* Hardware translates guest operands through NPT01 instead of treating GPA as host PA. */
            KswSvmWrite64(Current, 0x0b8U, Accel->MiscCtl2 | 2ULL);
            /* Only the VMLOAD/VMSAVE instruction intercepts are eliminated. */
            misc &= ~12U; Accel->VlsActive = 1;
            /* Saturating diagnostics cannot wrap into apparent inactivity. */
            if (Accel->VlsEntries != ~0ULL) { ++Accel->VlsEntries; }
        }
        /* CLGI/STGI stay intercepted so every physical GIF transition reaches the coordinator. */
    }
    /* Unknown and L1-owned intercepts, including CLGI/STGI, retain their original owners. */
    KswSvmWrite32(Current, KSW_VMCB_MISC2, misc);
}
int KswSvmAccelRestore(KSW_SVM_ACCEL* Accel, KSW_SVM_VMCB* Current)
{
    /* A missing applied transaction is an internal error, not a silent best-effort restore. */
    if (!Accel || !Current || !Accel->Applied) { return 0; }
    /* Routing must use the immutable source intercepts after every hardware exit. */
    KswSvmWrite32(Current, KSW_VMCB_MISC2, Accel->Misc2);
    /* VLS ownership never leaks into VMCB12 or native Windows. */
    KswSvmWrite64(Current, 0x0b8U, Accel->MiscCtl2); Accel->Applied = 0; return 1;
}
unsigned KswSvmAccelCleanPrepare(KSW_SVM_ACCEL* Accel, const KSW_SVM_VMCB* Current, KSW_SVM_U64 Pa)
{
    /* Interrupt/CR/DR/segment/CET groups remain dirty while their independent restore paths are evolving. */
    static const unsigned offsets[9] = {0,8,16,0x40,0x48,0x58,0x90,0xb0,KSW_VMCB_PAT};
    /* A byte-identical group, not a hash, is the proof of unchanged controls. */
    unsigned index, clean = 0, intercept = 1, maps = 1, np = 1;
    /* Any switch/new lifetime/INVALID forbids reusing hardware's prior cached image. */
    unsigned reusable = Accel->Enabled && (Accel->Features & 32U) && Accel->CleanValid && Accel->LastPa == Pa;
    /* Compare all controls in each whitelisted architectural group before refreshing the private snapshot. */
    for (index = 0; index < 9; ++index) {
        /* All reads are from the executable owned VMCB after final overlays, never from VMCB12. */
        KSW_SVM_U64 value = KswSvmRead64(Current, offsets[index]);
        /* Any group member difference forces that complete group dirty. */
        if (value != Accel->Controls[index]) {
            /* The intercept group includes all three implemented intercept qwords. */
            if (index < 3) { intercept = 0; }
            /* MSRPM and IOPM identities share the permission-map clean group. */
            else if (index < 5) { maps = 0; }
            /* NPT control/root/PAT all participate in the conservative NPT group. */
            else if (index > 5) { np = 0; }
        }
        /* ASID/TLB is compared independently before overwriting this entry's prior value. */
        if (index == 5 && reusable && value == Accel->Controls[index]) { clean |= 4U; }
        /* Completion later decides whether hardware successfully consumed these controls. */
        Accel->Controls[index] = value;
    }
    /* No source clean bit can enter this result. */
    if (reusable) { clean |= (intercept ? 1U : 0U) |
        (maps && Accel->MapsGeneration == Accel->LastMapsGeneration ? 2U : 0U) | (np ? 16U : 0U); }
    /* A recapture/merge must invalidate hardware permission caching even at identical addresses. */
    Accel->LastMapsGeneration = Accel->MapsGeneration;
    /* Publication stays invalid until a real non-INVALID VMEXIT acknowledges entry. */
    Accel->LastPa = Pa; Accel->CleanValid = 0; Accel->CleanMask = clean;
    /* Count actual selected clean contracts without claiming a speedup. */
    if (clean && Accel->CleanEntries != ~0ULL) { ++Accel->CleanEntries; }
    /* Assembly must still write this exact mask before hardware VMRUN. */
    return clean;
}
void KswSvmAccelComplete(KSW_SVM_ACCEL* Accel, KSW_SVM_U64 ExitCode)
{
    /* The physical VMCB cache is reusable only after successful entry consumed the prepared fields. */
    Accel->CleanValid = ExitCode != KSW_SVM_EXIT_INVALID;
}
