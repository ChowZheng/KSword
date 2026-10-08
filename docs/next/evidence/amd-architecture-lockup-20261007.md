# AMD architecture candidate: host hard lock analysis

Source: `781f28ee`; signed candidate: `artifacts/amd-perf-20261007-architecture`.
Raw evidence: `artifacts/amd-perf-20261007-architecture-lockup` and the previous `architecture-live` directory.

## Established execution boundary

- Old bulk driver completed all-core native stop and resource teardown. After the user exited KSword, SCM independently reached STOPPED.
- New candidate loaded, passed serial self-test on 32/32 processors, and committed 32/32 resident processors.
- The last successful metrics snapshot precedes VMware start. Its requested flags are `0xB0005` (general/profile/accel); write-watch, shared-root mode and integer fast mode were not enabled. No lockup-time metrics exist.
- The control journal records a VMware start invocation without a return. VMware's own log records PowerOn, CPL0 monitor, nested paging, a single vCPU and vCPU thread creation, then ends during monitor initialization. This proves the VM start path ran, not that guest BIOS or Windows boot succeeded.
- Kernel-Power 41 records BugcheckCode=0. No new minidump/host dump exists; the existing MEMORY.DMP predates this incident. Event 6008's reported shutdown time conflicts with the journal/VMware timestamps, so do not infer an exact lockup timestamp from it.
- Following recovery and the separately authorized lab reboot, HypervisorPresent=false, VBS=0 and the driver service is STOPPED. Collection did not load a driver or start a VM.

## Confirmed code defect, incident causality still unproven

`hvm_svm_nested_interrupt.c:KswSvmNestedInterruptPrepare` uses `!Gif && !Overlay->HardwareGif` to decide whether to force physical INTR masking and intercept NMI. Enabling hardware vGIF bypasses both protections. `hvm_svm_accel.c:KswSvmAccelApply` additionally removes STGI/CLGI intercepts, so L1 can close virtual GIF without notifying this software coordinator.

This confuses two different interrupt domains. AMD APM Volume 2, section 15.33.2 specifies that vGIF controls virtual interrupts; it does not provide the physical interrupt shielding on which this transparent Windows-host design depends. KVM virtualizes the guest interrupt-controller path and is therefore not a drop-in justification for removing the Windows-host masking protocol.

A small user-mode fixture links the **unchanged production interrupt preparation function**. With closed L1 GIF it reports:

| Entry | ForcedMask | V_INTR_MASKING | NMI intercept | saved host IF |
|---|---:|---:|---:|---:|
| Software GIF | 1 | 1 | 1 | 0 |
| Hardware vGIF | 0 | 0 | 0 | 0 |

Because V_INTR_MASKING=0, closed saved host IF does not replace the guest's IF for physical INTR masking. An L1 critical interval guarded by CLGI can therefore receive physical events before STGI, including while VMware is switching its execution context. This is a confirmed architectural regression and the strongest current explanation for the host lock; there is no retained hardware RIP/exit chain proving it caused this particular incident. VM-start-before metrics with vGIF count zero cannot exclude subsequent selection after VMware sets virtual SVME.

The fixture and output are `vgif-mask-repro.c`, `build-vgif-repro.cmd`, and `vgif-mask-repro.txt` in the raw evidence directory. Compilation and execution succeeded without SVM instructions or any driver operation. Existing accelerator fixtures check register bits but do not test physical-event shielding; their successful compilation was not correctness evidence.

## Next correction and discriminator

1. Retain CLGI/STGI interception and the software physical INTR/NMI masking contract for the transparent L1 Windows host. Do not use a vGIF-only bit as its replacement. Removing these exits requires an implemented physical-event interception/acknowledgement/injection and APIC ownership design, which this candidate does not supply.
2. Keep VLS separately selectable; it is a different hardware facility. Isolate stable VMCB changes, VLS, clean bits and watch/shared-root mode so one experiment does not enable multiple unverified mechanisms together.
3. Add the closed-GIF physical-shielding fixture to the production regression tests. Account for both already-closed entry and guest CLGI with initially open GIF; changing only the conditional is insufficient while CLGI remains un-intercepted.
4. After the correction, capture metrics immediately after resident commit and asynchronously alongside VMware startup, before waiting for `vmrun start` to return. The previous synchronous harness lost the entire interesting window when start did not return. Collection reduces this gap but does not guarantee evidence survives a hard reset.

No source correction or dynamic retry was performed during this analysis.

Reference: [AMD APM Volume 2](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24593.pdf), section 15.33.2; [KVM SVM implementation](https://github.com/torvalds/linux/blob/69f80fef3153299d9c72c53d1d71eef6354b6926/arch/x86/kvm/svm/svm.c), `svm_recalc_intercepts` and virtual-event handling.
