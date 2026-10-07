/* Windows platform binding for the portable nested execution/event transactions. */
#include "hvm_svm_nested_runtime.h"
#include "hvm_svm_watch.h"
#include "hvm_svm_cache.h"
#include <intrin.h>

/* Optional detail clock runs only while the bridge has saved the guest extended state. */
static KSW_SVM_U64 KswNsvmPerfClock(void* Context)
{
    /* All timestamp attribution is CPU-local; no Windows clock service is required. */
    KSW_SVM_U64 tick;
    /* The root transport context is unused by the clock. */
    UNREFERENCED_PARAMETER(Context);
    /* Serialize prior root work before sampling the invariant counter. */
    _mm_lfence(); tick = __rdtsc();
    /* Prevent following work from crossing the timestamp. */
    _mm_lfence(); return tick;
}

/* Called inside GeneralSequence while this processor exclusively owns both images. */
static VOID KswNsvmObserve(KSW_SVM_CPU* Cpu, ULONG Kind, ULONG Reason, ULONG Timing)
{
    /* No additional allocation, intercept or wait is introduced in the execution path. */
    KSW_SVM_NESTED* nested = Cpu->Nested;
    /* A frozen incident survives later normal execution and successful stop. */
    if (nested->Flight.latched) { return; }
    /* Keep only inner activity unless an outer/internal terminal observation needs attribution. */
    if (nested->Session.Phase == KSW_NSVM_SESSION_L2 || Reason) {
        /* Bind this record to the current lifecycle generation and captured operand identity. */
        KswSvmFlightRecord(&nested->Flight, Cpu->Guest, Kind, nested->Session.Phase,
            nested->GeneralMachine.LastAction, Cpu->Runtime->Generation, __rdtsc(),
            nested->Session.OperandHostPa, nested->Session.Lease.Token);
    }
    /* Capture SHUTDOWN/INVALID before reflection even when the coordinator treats reflection as success. */
    if (Reason) {
        /* An idle or released session cannot authenticate the stale Vmcb12 member. */
        KswSvmFlightLatch(&nested->Flight, Cpu->Guest,
            nested->Session.Phase == KSW_NSVM_SESSION_L2 ? &nested->Session.Vmcb12 : NULL, Reason, Timing);
        /* Publish a fully written immutable snapshot independently from the busy general seqlock. */
        InterlockedExchange(&nested->FlightFrozen, 1);
    }
}

/* The coordinator never calls an OS interrupt handler to discover the current priority. */
static unsigned KswNsvmReadTpr(void* Context)
{
    /* The current CPU is pinned by the common entry/rendezvous lifecycle. */
    UNREFERENCED_PARAMETER(Context);
    /* CR8 contains only the architectural four-bit task-priority class. */
    return (unsigned)__readcr8();
}

/* This callback is used only in complete root state with physical GIF and IF closed. */
static int KswNsvmWriteTpr(void* Context, unsigned Value)
{
    /* No guest pointer or APIC MMIO address is accepted as an operand. */
    UNREFERENCED_PARAMETER(Context);
    /* A failed read must not be silently truncated to a valid interrupt priority. */
    if (Value > 15) { return 0; }
    /* Preserve native/virtual CR8 semantics without calling pageable Windows services. */
    __writecr8(Value);
    /* The control register is read back on the same processor. */
    return __readcr8() == Value;
}

/* The acknowledgement leaf retains its count until the queue explicitly accepts it. */
static unsigned KswNsvmAcknowledgeNmi(void* Context)
{
    /* This descriptor belongs to the current CPU's prepared nonpageable allocation. */
    KSW_SVM_CPU* cpu = Context;
    /* Missing/already active captures are not one successfully acknowledged NMI. */
    if (!cpu || !cpu->Nested || !cpu->Nested->Nmi.Ready || cpu->Nested->Nmi.Armed || cpu->Nested->Nmi.Count) { return ~0U; }
    /* Execute only after VMEXIT has restored host state and closed IF; no C callback is made by the leaf. */
    (VOID)KswordSvmAsmAcknowledgeNmi(&cpu->Nested->Nmi);
    /* Return the exact retained hardware observation, including zero/multiple/saturation. */
    return cpu->Nested->Nmi.Count;
}

/* This operation is not a physical acknowledgement; it transfers an already captured record. */
static int KswNsvmCommitNmi(void* Context, unsigned Count)
{
    /* Queue acceptance and this transfer both execute in the same root CPU window. */
    KSW_SVM_CPU* cpu = Context;
    /* Never clear evidence after a count mismatch or while the private IDT is still installed. */
    if (!cpu || !cpu->Nested || cpu->Nested->Nmi.Armed || Count != 1 || cpu->Nested->Nmi.Count != Count) { return 0; }
    /* The coordinator retains the event token before reaching this statement. */
    cpu->Nested->Nmi.Count = 0; return 1;
}

/* CPUID executes on the same prepared processor as VMRUN, never a worker with different affinity. */
static void KswNsvmGeneralCpuid(void* Context, unsigned Leaf, unsigned Subleaf, unsigned Words[4])
{
    /* MSVC intrinsics use signed integers; result bit patterns are converted explicitly. */
    int values[4];
    /* No callback state is needed for raw processor enumeration. */
    UNREFERENCED_PARAMETER(Context);
    /* The portable filter applies guest XSTATE/control/SVM policy afterwards. */
    __cpuidex(values, (int)Leaf, (int)Subleaf);
    /* CPUID always returns four zero-extended architectural dwords. */
    Words[0] = (unsigned)values[0]; Words[1] = (unsigned)values[1];
    /* No host-side sign extension can enter the guest's GPRs. */
    Words[2] = (unsigned)values[2]; Words[3] = (unsigned)values[3];
}

/* Stop remains a private L0 lifecycle operation, never an L2 VMMCALL escape. */
static unsigned KswNsvmGeneralControl(void* Context, KSW_NSVM_MACHINE* Machine)
{
    /* The common lifecycle supplies this exact processor's context. */
    KSW_SVM_CPU* cpu = Context;
    /* Return data is not a native-return decision until every prerequisite succeeds. */
    ULONGLONG value, next;
    /* Ordinary VMMCALLs follow the nested ownership/exception dispatcher. */
    unsigned native = 0;
    /* Do not consume a virtual inner VMM's own hypercall or an unprivileged request. */
    if (!cpu || !cpu->Nested || Machine->Execution->Session->Phase != KSW_NSVM_SESSION_IDLE ||
        Machine->LastExit != KSW_SVM_EXIT_VMMCALL || cpu->Gpr[1] != KSW_SVM_CALL_SIGNATURE ||
        ((PUCHAR)cpu->Guest)[KSW_VMCB_CPL]) { return KSW_NSVM_MACHINE_NOT_CONTROL; }
    /* A private control must have a complete architectural continuation before changing anything. */
    next = KswSvmRead64(cpu->Guest, KSW_VMCB_NRIP);
    /* Interrupted IDT delivery cannot masquerade as a completed hypercall. */
    if ((KswSvmRead64(cpu->Guest, KSW_VMCB_EXITINTINFO) & (1ULL << 31)) ||
        !KswSvmNextRipValid(KswSvmRead64(cpu->Guest, KSW_VMCB_RIP), next)) { return KSW_NSVM_MACHINE_FAULT; }
    /* Queries retain their established private response signature. */
    if (cpu->Gpr[2] == KSW_SVM_CALL_QUERY) { value = KSW_SVM_CALL_SIGNATURE; }
    /* Vote inspects the live session without changing residency or clearing virtual SVM ownership. */
    else if (cpu->Gpr[2] == KSW_SVM_CALL_QUIESCE && cpu->StopRequested == 2 && cpu->Active) {
        /* The caller waits for all votes in Windows IPI execution, never on this root stack. */
        value = KswSvmNestedMachineCanStop(Machine) == KSW_NSVM_STOP_READY ? 0 : (ULONG)STATUS_DEVICE_BUSY;
    }
    /* Only the IPI stop owner may request native execution on an active CPU. */
    else if (cpu->Gpr[2] == KSW_SVM_CALL_STOP && cpu->StopRequested == 1 && cpu->Active) {
        /* Busy retains resident ownership; a nonzero result prevents the caller's native readback path. */
        native = KswSvmNestedMachineCanStop(Machine) == KSW_NSVM_STOP_READY;
        /* Never clear virtual SVM ownership or discard pending events to make stop appear successful. */
        value = native ? 0 : (ULONG)STATUS_DEVICE_BUSY;
    } else { return KSW_NSVM_MACHINE_NOT_CONTROL; }
    /* Only a completed control writes its return value and advances RIP. */
    KswSvmWrite64(cpu->Guest, KSW_VMCB_RAX, value); KswSvmWrite64(cpu->Guest, KSW_VMCB_RIP, next);
    /* A completed instruction consumes a preceding single-instruction shadow. */
    KswSvmWrite64(cpu->Guest, 0x068U, 0); KswSvmWrite64(cpu->Guest, KSW_VMCB_EVENT, 0);
    /* Native assembly still has to restore the current guest state and acknowledge it separately. */
    return native ? KSW_NSVM_MACHINE_NATIVE : KSW_NSVM_MACHINE_READY;
}

/* Bind the prepared resource graph without allocating inside an entry/exit transition. */
NTSTATUS KswordSvmNestedInitializeGeneral(KSW_SVM_CPU* Cpu)
{
    /* All subordinate descriptors are embedded in the existing per-CPU allocation. */
    KSW_SVM_NESTED* nested;
    /* A packed IDTR operand is built from the trusted, current-CPU capture. */
    UCHAR idtr[16] = {0};
    /* Current Windows segment records were validated by the ordinary VMCB builder. */
    const KSW_SVM_SEGMENT* table;
    /* Every field gets an explicit owner rather than borrowing the bounded test's original snapshot. */
    KSW_NSVM_SESSION_IO* io;
    /* No partial or previously live state may be reinitialized. */
    if (!Cpu || !(nested = Cpu->Nested) || Cpu->SelfTest || Cpu->Active || nested->GeneralInitialized ||
        nested->Session.Lease.Token || nested->Session.Phase != KSW_NSVM_SESSION_IDLE ||
        !nested->Outer || !nested->Window || !Cpu->XstateLayout.Ready || (nested->GeneralSequence & 1)) { return STATUS_INVALID_DEVICE_STATE; }
    /* An incomplete initialization remains an odd, invalid diagnostic snapshot. */
    InterlockedIncrement64(&nested->GeneralSequence);
    /* The software register owner starts from native SVM-free Windows state. */
    nested->Msrs.Efer = Cpu->Caps.Efer & ~KSW_SVM_EFER_SVME; nested->Msrs.Hsave = 0;
    /* Firmware disable/lock and the physical-width mask remain read-only virtual capabilities. */
    nested->Msrs.VmCr = Cpu->Caps.VmCr; nested->Msrs.AddressMask = nested->Outer->AddressMask;
    /* NPT01 is immutable for the complete prepared lifetime. */
    nested->Config.OuterRoot = nested->Outer->RootPa; nested->Config.OuterPat = Cpu->Caps.Pat;
    nested->Config.OuterImmutable = 1;
    /* Both translations use the unchanged hardware PAT encoding. */
    nested->Config.HardwarePat = Cpu->Caps.Pat; nested->Config.OuterBits = Cpu->Caps.PhysicalBits;
    /* NPT address/large-page/NX policy comes from this same processor's admitted capabilities. */
    nested->Config.OuterPage1Gb = Cpu->Caps.Page1Gb; nested->Config.OuterNx = (Cpu->Caps.Efer & (1ULL << 11)) != 0;
    /* Session operands and page-table words all use the same validated RAM window. */
    io = &nested->GeneralIo; RtlZeroMemory(io, sizeof(*io));
    /* The operand snapshot must use L0 translation, never an inner NCR3 directly. */
    io->Operand.Root = nested->Config.OuterRoot; io->Operand.Pat = Cpu->Caps.Pat;
    /* These features match the outer NPT builder and the CPUID contract below. */
    io->Operand.PhysicalBits = Cpu->Caps.PhysicalBits; io->Operand.Page1Gb = Cpu->Caps.Page1Gb;
    /* No physical callback allocates or retains a mapped pointer across calls. */
    io->Operand.Nx = nested->Config.OuterNx; io->Operand.Read = KswordSvmNestedRead; io->Operand.Context = nested;
    io->Operand.ReadPage = KswordSvmNestedReadPage;
    /* Virtual capabilities cannot exceed what this prepared CPU can restore natively. */
    io->Policy.PhysicalBits = Cpu->Caps.PhysicalBits; io->Policy.AsidCount = Cpu->Caps.AsidCount;
    /* This first general contract still explicitly rejects unsupported CR4/EFER extensions. */
    io->Policy.EferSupported = Cpu->Caps.Efer | KSW_SVM_EFER_SVME; io->Policy.Cr4Supported = Cpu->Caps.Cr4;
    /* Exact writeback and cross-CPU VMCB authority are shared with the bounded probe. */
    io->Commit = KswordSvmNestedCommitVmcb; io->Owners = nested->Owners; io->CpuIdentity = nested->CpuIdentity;
    /* Deferred inner events follow the shared VMCB identity across later CPU scheduling. */
    io->Pending = &nested->GeneralExecution.Pending;
    /* Both merged maps are hardware-contiguous prepared buffers. */
    io->MergedMsr = nested->MergedMaps; io->MergedIo = nested->MergedMaps + KSW_NSVM_MSRPM_BYTES;
    /* Guest-provided addresses are never substituted for these physical map identities. */
    io->MsrPa = nested->MergedMapsPa; io->IoPa = nested->MergedMapsPa + KSW_NSVM_MSRPM_BYTES;
    if (io->Policy.AsidCount < 4U) { return STATUS_NOT_SUPPORTED; }
    /* L1 uses hardware ASID 1. Give each CPU's L2 a stable private ASID so
       NPT01 translations can never be reused for NPT02. */
    io->Asid = 2U + ((ULONG)Cpu->Resource->Row.processorGroup << 8) + Cpu->Resource->Row.processorNumber;
    if (!KswSvmAsidValid(io->Policy.AsidCount, io->Asid)) { io->Asid = 2U + (nested->CpuIdentity % (io->Policy.AsidCount - 2U)); }
    /* The initial L0 intercept set is immutable even when per-entry masking overlays change MISC1. */
    io->OuterPermissions.Flags = (unsigned)KswSvmRead64(Cpu->Guest, KSW_VMCB_MISC1);
    /* L1 cannot weaken a bit in either immutable outer bitmap. */
    io->OuterPermissions.Msr = Cpu->Msrpm; io->OuterPermissions.Io = Cpu->Iopm;
    /* Every virtual CPU uses its own cache epoch and composition pages. */
    io->Shadow = &nested->Shadow; io->Mmu = &nested->Config;
    io->ReuseNpt = 1;
    /* Fresh local page reads replace repeated per-PTE NPT01 walks; no persistent cache is retained. */
    io->SourceSyncPage = nested->SourceSyncPage;
    /* A physical image remains assigned to one layer for the complete prepared lifetime. */
    io->StableL1 = nested->Vmcb01; io->StableL2 = nested->Vmcb02;
    /* Proof callbacks are bound only when the independent write-watch option allocated its full ledger. */
    if (nested->Watch) {
        /* Hardware L1 permission tightening must not alter software translation/PAT provenance. */
        KswSvmWrite64(Cpu->Guest, KSW_VMCB_NCR3, nested->Watch->RootPa);
        io->BeginWatch = KswordSvmWatchBegin; io->ArmSource = KswordSvmWatchArm;
        io->SourceStable = KswordSvmWatchStable; io->ProtectMapping = KswordSvmWatchMapping;
        /* Each shared root is selected only after acquiring its real translated VMCB execution lease. */
        io->BindCache = KswordSvmCacheBind; io->UnbindCache = KswordSvmCacheUnbind; io->InvalidateCaches = KswordSvmCacheInvalidate;
    }
    /* Bind the ordinary register image instead of a driver-owned fixed probe marker. */
    RtlZeroMemory(&nested->GeneralExecution, sizeof(nested->GeneralExecution));
    /* Hardware RAX/RSP remain in VMCB, while other GPRs use the established assembly prefix. */
    nested->GeneralExecution.Current = Cpu->Guest; nested->GeneralExecution.Gpr = Cpu->Gpr;
    /* The same live transaction handles every arbitrary admitted VMCB12 operand. */
    nested->GeneralExecution.Session = &nested->Session; nested->GeneralExecution.Io = io;
    /* XCR0/XSS write policies are bounded by the original allocation geometry. */
    nested->GeneralExecution.GuestXcr0 = &Cpu->GuestXcr0; nested->GeneralExecution.PreparedXcr0 = Cpu->HostXcr0;
    /* Register state is virtual except for the fixed root cache/state restore contract. */
    nested->GeneralExecution.Registers.Current = Cpu->Guest; nested->GeneralExecution.Registers.Svm = &nested->Msrs;
    /* XSAVE format and capacity never shrink when the guest chooses fewer enabled components. */
    nested->GeneralExecution.Registers.GuestXss = &Cpu->GuestXss; nested->GeneralExecution.Registers.PreparedXss = Cpu->HostXss;
    /* Cache/MSR support matches admission and CPUID enumeration. */
    nested->GeneralExecution.Registers.RootPat = Cpu->Caps.Pat; nested->GeneralExecution.Registers.EferAllowed = io->Policy.EferSupported;
    /* Supervisor CET remains excluded by the underlying entry/restore contract. */
    nested->GeneralExecution.Registers.XsaveFeatures = Cpu->Caps.XsaveFeatures; nested->GeneralExecution.Registers.CetPresent = Cpu->Caps.CetPresent;
    /* Exposure here is internal policy only; this function does not activate the public resident path. */
    nested->GeneralExecution.Registers.ExposeSvm = 1;
    /* CPUID capabilities refer to preallocated state and this virtual ASID namespace. */
    nested->GeneralExecution.Cpuid.Xstate = &Cpu->XstateLayout; nested->GeneralExecution.Cpuid.ExposeSvm = 1;
    /* Only implemented feature bits are published by the portable CPUID filter. */
    nested->GeneralExecution.Cpuid.AsidCount = Cpu->Caps.AsidCount; nested->GeneralExecution.Cpuid.SvmFeatures = Cpu->Caps.Features;
    /* Physical and CR4 widths agree with VMCB/operand validation. */
    nested->GeneralExecution.Cpuid.PhysicalBits = Cpu->Caps.PhysicalBits; nested->GeneralExecution.Cpuid.Cr4Supported = io->Policy.Cr4Supported;
    /* Raw topology/vendor data remains tied to this CPU rather than a cached different core. */
    nested->GeneralExecution.Cpuid.Read = KswNsvmGeneralCpuid; nested->GeneralExecution.Cpuid.Context = Cpu;
    /* Source table A/D changes use compare-exchange under the same physical RAM admission. */
    nested->GeneralExecution.MmuIo.Read = KswordSvmNestedRead; nested->GeneralExecution.MmuIo.CompareOr = KswordSvmNestedCompareOr;
    /* No global root lock or guest pointer is passed to the walker. */
    nested->GeneralExecution.MmuIo.Context = nested;
    /* NMI table preparation uses the trusted original host IDTR, never an inner guest image. */
    table = (const KSW_SVM_SEGMENT*)((const UCHAR*)Cpu->Guest + KSW_VMCB_IDTR);
    /* Keep the hardware's packed descriptor format explicit. */
    RtlCopyMemory(idtr, &table->limit, sizeof(USHORT)); RtlCopyMemory(idtr + 2, &table->base, sizeof(ULONGLONG));
    /* A stale/unreadable trusted table refuses admission before any real SVM ownership changes. */
    __try {
        /* The private NMI gate uses this same CPU's captured kernel code selector. */
        if (!KswordSvmNmiInitialize(&nested->Nmi, idtr, ((KSW_SVM_SEGMENT*)((PUCHAR)Cpu->Guest + KSW_VMCB_CS))->selector)) { return STATUS_NOT_SUPPORTED; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    /* Binding callbacks is separate from issuing any physical entry or NMI acknowledgement. */
    RtlZeroMemory(&nested->GeneralMachine, sizeof(nested->GeneralMachine));
    /* Every state object remains embedded in the retained per-CPU resource allocation. */
    nested->GeneralMachine.Execution = &nested->GeneralExecution; nested->GeneralMachine.Io.Context = Cpu;
    /* Physical acknowledgement is exactly the bounded root leaf, followed by explicit queue handoff. */
    nested->GeneralMachine.Io.AcknowledgeNmi = KswNsvmAcknowledgeNmi; nested->GeneralMachine.Io.CommitNmi = KswNsvmCommitNmi;
    /* CR8 synchronization and private lifecycle controls have their own narrow callbacks. */
    nested->GeneralMachine.Io.ReadTpr = KswNsvmReadTpr; nested->GeneralMachine.Io.WriteTpr = KswNsvmWriteTpr;
    /* Private control cannot run until raw overlay/event state has been processed. */
    nested->GeneralMachine.Io.PrivateControl = KswNsvmGeneralControl;
    /* Only write NPFs owned by the private hardware clone may bypass ordinary nested fault routing. */
    nested->GeneralMachine.Io.ProtectionFault = nested->Watch ? KswordSvmWatchFault : NULL;
    /* Hardware acceleration is independently requested and bounded by this CPU's real features. */
    RtlZeroMemory(&nested->Accel, sizeof(nested->Accel)); nested->Accel.Features = Cpu->Caps.Features;
    /* Missing VLS/vGIF/clean support keeps the same software execution contract. */
    nested->Accel.Enabled = ((((KSW_SVM_STATE*)Cpu->Runtime->BackendContext)->PreparedFlags & KSWORD_ARK_HVM_CONTROL_FLAG_SVM_ACCEL) != 0);
    /* The coordinator declines vGIF whenever software-owned pending events need an STGI notification. */
    nested->GeneralMachine.HardwareGifAllowed = nested->Accel.Enabled && ((Cpu->Caps.Features & (1U << 16)) != 0);
    /* Capture actual initial TPR, but do not create an executable overlay before assembly sets final RIP/RFLAGS. */
    if (KswSvmNestedMachineInitialize(&nested->GeneralMachine) != KSW_NSVM_MACHINE_READY) { return STATUS_NOT_SUPPORTED; }
    /* This is bound-resource readiness; public activation and hardware success are separate evidence. */
    nested->GeneralHardwareExits = nested->GeneralLastHardwareExit = 0;
    /* Physical ASIDs may contain translations from a prior prepared resource lifetime. */
    nested->FirstEntryFlush = 1;
    /* A new residency has no hardware ASID provenance or idle shared-root telemetry. */
    nested->LastNptRoot[0] = nested->LastNptRoot[1] = 0; nested->SharedRoot = NULL;
    nested->LastSharedRootPa = nested->CacheMigrations = nested->LastShadowEpoch = nested->LastDependencyRetirements = 0;
    nested->LastShadowPages = 0; nested->WatchProofHits = 0;
    /* Reset sampled timings before publishing any executable pointer. */
    RtlZeroMemory(&nested->Perf, sizeof(nested->Perf));
    /* Profiling is an explicit prepared-mode option, never enabled by a query. */
    nested->Perf.Mask = 63;
    /* Earlier prefixes are stable; assembly only follows this pointer when nonnull. */
    Cpu->Perf = (((KSW_SVM_STATE*)Cpu->Runtime->BackendContext)->PreparedFlags &
        KSWORD_ARK_HVM_CONTROL_FLAG_SVM_PROFILE) ? &nested->Perf : NULL;
    /* New residency starts its own zeroed hotspot epoch under lifecycle exclusion. */
    RtlZeroMemory(&nested->Hotspots, sizeof(nested->Hotspots));
    /* Initialize stable fast MSR identity slots; no dynamic lookup is needed by the leaf. */
    nested->Hotspots.levels[0].msrUsed = 3;
    /* Zero observations do not imply these registers have executed. */
    nested->Hotspots.levels[0].msrs[0].number = 0xc0000080U;
    /* The virtual save-area register remains fully intercepted. */
    nested->Hotspots.levels[0].msrs[1].number = 0xc0010117U;
    /* An absent XSAVES feature is still refused by the integer leaf. */
    nested->Hotspots.levels[0].msrs[2].number = 0xda0U;
    /* No count is readable until the first complete observation. */
    nested->HotSequence = 0;
    /* Bind the optional scalar leaf before publishing its executable CPU pointer. */
    RtlZeroMemory(&nested->Fast, sizeof(nested->Fast));
    /* Every pointer names the same retained CPU-private resource lifetime. */
    nested->Fast.HotSequence = (volatile KSW_SVM_U64*)&nested->HotSequence;
    /* General metrics remain coherent across both dispatcher forms. */
    nested->Fast.GeneralSequence = (volatile KSW_SVM_U64*)&nested->GeneralSequence;
    /* Raw accounting is unchanged even though the general C bridge may be skipped. */
    nested->Fast.Hot = &nested->Hotspots; nested->Fast.GeneralExits = &nested->GeneralHardwareExits;
    /* Last-exit fields retain the real physical MSR exit code. */
    nested->Fast.GeneralLastExit = &nested->GeneralLastHardwareExit;
    /* CPU-local lifetime counters must include every fast return. */
    nested->Fast.VmExits = &Cpu->Resource->Row.vmExitCount;
    /* This protocol field is AMD-specific, never a VMX reason. */
    nested->Fast.RawExit = &Cpu->Resource->Row.svmExitCode; nested->Fast.LegacyTlb = &Cpu->TlbRequests;
    /* READY event-free returns preserve the existing overlay and machine diagnostics. */
    nested->Fast.Transitions = &nested->GeneralMachine.Transitions;
    /* A same-value EFER write still updates the software mirror's hardware-owned LMA. */
    nested->Fast.VirtualEfer = &nested->Msrs.Efer;
    /* The coordinator's last action must describe the latest physical return. */
    nested->Fast.MachineLastExit = &nested->GeneralMachine.LastExit;
    /* READY has the same numeric meaning in the scalar leaf and coordinator. */
    nested->Fast.MachineLastAction = &nested->GeneralMachine.LastAction;
    /* Timing/count storage remains embedded and allocated before root entry. */
    nested->Fast.Perf = &nested->Perf;
    /* Fast mode always includes profiling; ordinary general preparation stays unmodified. */
    Cpu->Fast = (((KSW_SVM_STATE*)Cpu->Runtime->BackendContext)->PreparedFlags &
        KSWORD_ARK_HVM_CONTROL_FLAG_SVM_FAST_MSR) ? &nested->Fast : NULL;
    /* Counters now describe this binding lifetime, matching the new machine transition counter. */
    nested->GeneralInitialized = 1;
    /* Assembly can observe this only after every platform callback and state owner has been initialized. */
    Cpu->NestedEntryEnabled = 1;
    /* A nonzero even sequence publishes binding readiness, never hardware execution evidence. */
    InterlockedIncrement64(&nested->GeneralSequence); return STATUS_SUCCESS;
}

/* The assembly-facing caller is responsible for refusing every non-READY action. */
ULONG KswordSvmNestedGeneralEntry(KSW_SVM_CPU* Cpu)
{
    /* No dormant/unbound descriptor may be used as a hardware entry request. */
    ULONG action;
    /* Do not turn a missing optional resource into baseline residency silently. */
    if (!Cpu || !Cpu->Nested || !Cpu->Nested->GeneralInitialized) { return KSW_NSVM_MACHINE_FAULT; }
    /* Queries may not sample partially installed entry controls as a coherent machine. */
    InterlockedIncrement64(&Cpu->Nested->GeneralSequence);
    /* This writes only processor-owned state and invokes the closed-root callbacks above. */
    action = KswSvmNestedMachineEntry(&Cpu->Nested->GeneralMachine);
    /* Entry-side event reflection may also switch layers before physical VMRUN. */
    Cpu->Guest = Cpu->Nested->GeneralExecution.Current;
    /* Both retained execution operands were pre-resolved before residency. */
    Cpu->GuestPa = Cpu->Guest == Cpu->Nested->Vmcb01 ? Cpu->Nested->Vmcb01Pa : Cpu->Nested->Vmcb02Pa;
    /* Observe actual installed entry controls; terminal entry errors are explicitly post-dispatch. */
    KswNsvmObserve(Cpu, KSW_HVM_FLIGHT_ENTRY,
        (action == KSW_NSVM_MACHINE_FAULT || action == KSW_NSVM_MACHINE_UNSUPPORTED ||
         action == KSW_NSVM_MACHINE_SHUTDOWN) ? KSW_HVM_FLIGHT_INTERNAL : 0U, 2U);
    /* Host IF is installed by assembly while physical GIF remains closed. */
    if (action == KSW_NSVM_MACHINE_READY) { Cpu->HostInterruptsAllowed = Cpu->Nested->GeneralMachine.Overlay.HostIf; }
    /* Flush only after NPT02 publication/reset or an L1 TLB_CONTROL request. */
    if (action == KSW_NSVM_MACHINE_READY) {
        /* Selection is separate from completion: INVALID preserves all pending work. */
        Cpu->NestedTlbControl = Cpu->Nested->FirstEntryFlush ? 1U :
            KswSvmNestedSessionTlbSelect(&Cpu->Nested->Session,
                Cpu->Nested->GeneralIo.Shadow, (Cpu->Caps.Features & 64U) != 0);
    } else {
        Cpu->NestedTlbControl = 1U;
    }
    /* Recompute fast eligibility only after full event preparation succeeded. */
    if (action == KSW_NSVM_MACHINE_READY) {
        /* New protections retire only affected destination aliases and require full hardware TLB acknowledgement. */
        KswordSvmWatchEntry(Cpu);
        /* Root migration/virtual invalidation preserves software data only when its generation remains valid. */
        KswordSvmCacheEntry(Cpu);
        /* A failed bounded root reset never permits a blind VMRUN. */
        if (Cpu->Nested->GeneralMachine.LastAction == KSW_NSVM_MACHINE_FAULT) { action = KSW_NSVM_MACHINE_FAULT; }
        /* Original intercepts are restored before the next exit's route/reflect transaction. */
        KswSvmAccelApply(&Cpu->Nested->Accel, Cpu->Guest,
            Cpu->Nested->Session.Phase == KSW_NSVM_SESSION_L2, Cpu->Nested->Msrs.Efer,
            Cpu->Nested->GeneralMachine.Overlay.HardwareGif);
        /* Changes to merged map contents must invalidate permission hardware caching. */
        Cpu->Nested->Accel.MapsGeneration = Cpu->Nested->Session.PermissionsGeneration;
        /* Only exact unchanged private control groups become hardware clean. */
        Cpu->HardwareCleanMask = KswSvmAccelCleanPrepare(&Cpu->Nested->Accel, Cpu->Guest, Cpu->GuestPa);
    } else { Cpu->HardwareCleanMask = 0; }
    /* Recompute fast eligibility only after full event preparation succeeded. */
    if (Cpu->Fast) {
        /* No virtual execution owner or pending event can be hidden by the short bridge. */
        KSW_NSVM_MACHINE* machine = &Cpu->Nested->GeneralMachine;
        /* Zero disables the leaf on every failure, layer switch or state transition. */
        Cpu->Fast->Enabled = 0;
        /* Fixed virtual GIF and no injection/window make the current hardware overlay reusable. */
        if (action == KSW_NSVM_MACHINE_READY && KswSvmFastEligible(machine, Cpu->NestedTlbControl)) {
            /* Snapshot only virtual values, never physical SVM ownership registers. */
            Cpu->Fast->Efer = Cpu->Nested->Msrs.Efer; Cpu->Fast->Hsave = Cpu->Nested->Msrs.Hsave;
            /* Guest mask changes use the full path, then refresh this immutable entry snapshot. */
            Cpu->Fast->Xss = Cpu->GuestXss; Cpu->Fast->XsaveFeatures = Cpu->Caps.XsaveFeatures;
            /* A later CR8/control change causes the integer leaf to decline without mutation. */
            Cpu->Fast->IntCtl = KswSvmRead64(Cpu->Guest, KSW_VMCB_INTCTL);
            /* Physical priority is read while GIF/IF are closed on the owner CPU. */
            Cpu->Fast->PhysicalTpr = __readcr8(); Cpu->Fast->Enabled = 1;
            /* A newly armed/revoked page must interrupt the otherwise unchanged integer bridge. */
            Cpu->Fast->GuardEpoch = Cpu->Nested->Watch ? (volatile KSW_SVM_U64*)&Cpu->Nested->Watch->Generation : NULL;
            Cpu->Fast->GuardGeneration = Cpu->Nested->WatchEntryGeneration;
        }
    }
    /* Publish the complete result, including a retained failure/window action. */
    InterlockedIncrement64(&Cpu->Nested->GeneralSequence);
    /* WINDOW/FAULT/UNSUPPORTED never authorize a blind VMRUN. */
    return action;
}

/* Raw hardware exit accounting remains owned by the common SVM dispatcher. */
ULONG KswordSvmNestedGeneralExit(KSW_SVM_CPU* Cpu)
{
    /* Initialization and activation are deliberately separate contracts. */
    if (!Cpu || !Cpu->Nested || !Cpu->Nested->GeneralInitialized) { return KSW_NSVM_MACHINE_FAULT; }
    /* A real hardware exit must be counted independently from entry preparation. */
    InterlockedIncrement64(&Cpu->Nested->GeneralSequence);
    /* Counter exhaustion remains a retained fault rather than reused execution evidence. */
    if (Cpu->Nested->GeneralHardwareExits == ~0ULL) {
        /* Publish the retained exhaustion without authorizing a further VMRUN. */
        Cpu->Nested->GeneralMachine.LastAction = KSW_NSVM_MACHINE_FAULT;
        /* Retain counter exhaustion without replacing an earlier first incident. */
        KswNsvmObserve(Cpu, KSW_HVM_FLIGHT_EXIT, KSW_HVM_FLIGHT_INTERNAL, 1U);
        /* A completed diagnostic record may describe failure as well as success. */
        InterlockedIncrement64(&Cpu->Nested->GeneralSequence); return KSW_NSVM_MACHINE_FAULT;
    }
    /* This wrapper is reached only from the physical VMEXIT dispatcher. */
    ++Cpu->Nested->GeneralHardwareExits;
    /* Preserve raw hardware input even if a missing overlay makes machine dispatch fail before classification. */
    Cpu->Nested->GeneralLastHardwareExit = KswSvmRead64(Cpu->Guest, KSW_VMCB_EXITCODE);
    /* Cross-CPU proof includes only generations whose full hardware flush actually completed. */
    KswordSvmWatchComplete(Cpu, Cpu->Nested->GeneralLastHardwareExit);
    /* Hardware root/ASID provenance is also consumed only by actual non-INVALID execution. */
    KswordSvmCacheComplete(Cpu, Cpu->Nested->GeneralLastHardwareExit);
    /* Charge a sampled interval to its original level and reason before any reflection. */
    if (Cpu->Perf && Cpu->Perf->Selected && Cpu->Nested->Session.Phase <= KSW_NSVM_SESSION_L2) {
        /* This writes no SIMD state and the full bridge has already saved guest XSTATE. */
        Cpu->Perf->Row = &Cpu->Perf->Metrics.rows[Cpu->Nested->Session.Phase]
            [KswSvmPerfBucket(Cpu->Nested->GeneralLastHardwareExit)];
    }
    /* Clear the previous sampled destination even when the new exit was not selected. */
    Cpu->Nested->GeneralIo.PerfRow = Cpu->Perf ? Cpu->Perf->Row : NULL;
    /* Callbacks are harmless when the destination row is absent. */
    Cpu->Nested->GeneralIo.PerfClock = Cpu->Perf ? KswNsvmPerfClock : NULL;
    /* Any counter exhaustion invalidates only profiling evidence. */
    Cpu->Nested->GeneralIo.PerfSaturated = Cpu->Perf ? &Cpu->Perf->Saturated : NULL;
    /* Count actual successfully entered hardware flush controls, not merely requested virtual flushes. */
    if (Cpu->Perf && Cpu->Nested->GeneralLastHardwareExit != KSW_SVM_EXIT_INVALID) {
        /* Unselected exits borrow a short telemetry sequence; selected exits already own it. */
        ULONG index = Cpu->NestedTlbControl == 1 ? 1U : Cpu->NestedTlbControl == 3 ? 2U : Cpu->NestedTlbControl == 7 ? 3U : 0U;
        /* A selected full-interval writer is already odd. */
        if (!Cpu->Perf->Selected) { InterlockedIncrement64((volatile LONG64*)&Cpu->Perf->Sequence); }
        /* Never wrap a public flush count into apparent idle evidence. */
        if (Cpu->Perf->Metrics.tlbIssued[index] == ~0ULL) { Cpu->Perf->Saturated = 1; }
        /* Only real non-INVALID hardware returns prove consumption. */
        else { ++Cpu->Perf->Metrics.tlbIssued[index]; }
        /* Short unselected updates publish before ordinary dispatch begins. */
        if (!Cpu->Perf->Selected) { InterlockedIncrement64((volatile LONG64*)&Cpu->Perf->Sequence); }
    }
    /* Retire the request before dispatch can install a new NPT02 leaf or change the running level. */
    KswSvmNestedSessionTlbComplete(&Cpu->Nested->Session, Cpu->Nested->GeneralIo.Shadow,
        Cpu->NestedTlbControl, Cpu->Nested->GeneralLastHardwareExit);
    /* A non-INVALID hardware return proves the initial resource-lifetime flush executed. */
    if (Cpu->Nested->GeneralLastHardwareExit != KSW_SVM_EXIT_INVALID) { Cpu->Nested->FirstEntryFlush = 0; }
    /* Publish raw per-level counters before the dispatcher mutates RCX, RIP or session ownership. */
    InterlockedIncrement64(&Cpu->Nested->HotSequence);
    /* The shared short transaction does no guest memory access or root event processing. */
    KswSvmHotObserve(&Cpu->Nested->Hotspots, Cpu->Nested->Session.Phase,
        Cpu->Nested->GeneralLastHardwareExit, KswSvmRead64(Cpu->Guest, KSW_VMCB_RIP),
        KswSvmRead64(Cpu->Guest, KSW_VMCB_EXITINFO1), KswSvmRead64(Cpu->Guest, KSW_VMCB_EXITINFO2),
        (ULONG)Cpu->Gpr[1]);
    /* Release the independently coherent counters before expensive general exit handling. */
    InterlockedIncrement64(&Cpu->Nested->HotSequence);
    /* Raw terminal exits must be copied before MachineExit restores controls or reflects to L1. */
    KswNsvmObserve(Cpu, KSW_HVM_FLIGHT_EXIT,
        Cpu->Nested->GeneralLastHardwareExit == 0x7fULL ? KSW_HVM_FLIGHT_SHUTDOWN :
        (Cpu->Nested->GeneralLastHardwareExit == KSW_SVM_EXIT_INVALID ? KSW_HVM_FLIGHT_INVALID : 0U), 1U);
    /* Hardware clean provenance is acknowledged only for an actually executed physical entry. */
    KswSvmAccelComplete(&Cpu->Nested->Accel, Cpu->Nested->GeneralLastHardwareExit);
    /* Route/reflect sees the source intercepts, not the acceleration overlay. */
    if (!KswSvmAccelRestore(&Cpu->Nested->Accel, Cpu->Guest)) {
        /* Keep the complete retained resource graph on a missing acceleration transaction. */
        InterlockedIncrement64(&Cpu->Nested->GeneralSequence); return KSW_NSVM_MACHINE_FAULT;
    }
    /* Native return is requested only by the private, quiescent stop callback. */
    {
        /* Keep the exact coordinator outcome after all mutation is complete. */
        ULONG action = KswSvmNestedMachineExit(&Cpu->Nested->GeneralMachine);
        /* Native Windows bypasses NPT, so global proof must retire before any actual native instruction. */
        if (action == KSW_NSVM_MACHINE_NATIVE) { KswordSvmWatchDisable(Cpu); }
        /* Commit the physical/virtual pair only after the coordinator completed its layer transaction. */
        Cpu->Guest = Cpu->Nested->GeneralExecution.Current;
        /* Both physical operands were derived from owned contiguous pages at PASSIVE_LEVEL. */
        Cpu->GuestPa = Cpu->Guest == Cpu->Nested->Vmcb01 ? Cpu->Nested->Vmcb01Pa : Cpu->Nested->Vmcb02Pa;
        /* Internal failures have a separately labelled post-dispatch snapshot; raw faults already won. */
        if (action == KSW_NSVM_MACHINE_FAULT || action == KSW_NSVM_MACHINE_UNSUPPORTED || action == KSW_NSVM_MACHINE_SHUTDOWN) {
            /* Preserve retained owners and failure output even when no hardware SHUTDOWN was seen. */
            KswNsvmObserve(Cpu, KSW_HVM_FLIGHT_EXIT, KSW_HVM_FLIGHT_INTERNAL, 2U);
        }
        /* Root NMI acknowledgement, queue transfer and reflection all belong to this same sequence. */
        InterlockedIncrement64(&Cpu->Nested->GeneralSequence); return action;
    }
}

/* Do not release the host stack/HSAVE simply because a nested sub-release refused to free itself. */
BOOLEAN KswordSvmNestedBusy(const KSW_SVM_CPU* Cpu)
{
    /* Partial preparation is releasable when no owner exists. */
    const KSW_SVM_NESTED* nested;
    /* The common CPU active bit remains authoritative for ordinary residency as well. */
    if (!Cpu || Cpu->Active) { return TRUE; }
    /* No optional allocation means there is no nested owner to retain. */
    if (!(nested = Cpu->Nested)) { return FALSE; }
    /* Both general and bounded-probe VMCB ownership must be gone before release. */
    if (nested->RunningL2 || nested->Session.Lease.Token || nested->Session.Phase == KSW_NSVM_SESSION_L2 ||
        nested->Nmi.Armed || nested->Nmi.Count) { return TRUE; }
    /* Acknowledged events or executable overlay windows retain their containing CPU resources. */
    return nested->GeneralExecution.Pending.Count || nested->GeneralMachine.ArmedToken ||
        nested->GeneralMachine.NmiCount || nested->GeneralMachine.Overlay.Applied || nested->GeneralMachine.IrqWindow.Applied ||
        nested->GeneralMachine.NmiHardwareMask || nested->GeneralMachine.NmiBlocked || nested->GeneralMachine.HeldNmiGuard ||
        nested->GeneralMachine.Iret.Applied || nested->GeneralMachine.Iret.Requested ||
        nested->GeneralMachine.NmiWindow.Applied;
}

/* The caller already verified native EFER/HSAVE/current extended state on this exact CPU. */
BOOLEAN KswordSvmNestedCancelFirstEntry(KSW_SVM_CPU* Cpu)
{
    /* A failed initial entry owns a constructed overlay, but no executed guest event. */
    KSW_SVM_NESTED* nested;
    /* No later INVALID or failed stop can use this narrow cancellation path. */
    if (!Cpu || !(nested = Cpu->Nested) || Cpu->Active || Cpu->SelfTest || !Cpu->Resource ||
        Cpu->Stage != KSWORD_ARK_HVM_STAGE_FAILED || Cpu->Resource->Row.vmExitCount != 1 ||
        Cpu->Resource->Row.svmExitCode != KSW_SVM_EXIT_INVALID || !nested->GeneralInitialized ||
        !Cpu->NestedEntryEnabled || (nested->GeneralSequence & 1) || nested->GeneralHardwareExits ||
        nested->GeneralMachine.Transitions != 1 || nested->Session.Phase != KSW_NSVM_SESSION_IDLE ||
        nested->Session.Lease.Token || nested->GeneralExecution.Pending.Count || nested->GeneralMachine.ArmedToken ||
        nested->GeneralMachine.NmiCount || nested->GeneralMachine.NmiHardwareMask || nested->GeneralMachine.NmiBlocked ||
        nested->GeneralMachine.Iret.Applied || nested->GeneralMachine.Iret.Requested || nested->Nmi.Armed || nested->Nmi.Count) { return FALSE; }
    /* Publish native cancellation separately from any successful general hardware cycle. */
    InterlockedIncrement64(&nested->GeneralSequence);
    /* INVALID did not consume the executable interrupt overlay. No guest state is rolled back here. */
    nested->GeneralMachine.Overlay.Applied = 0; nested->GeneralMachine.LastAction = KSW_NSVM_MACHINE_FAULT;
    /* Preserve the rejected hardware result for subsequent read-only diagnostics. */
    nested->GeneralLastHardwareExit = KSW_SVM_EXIT_INVALID; nested->GeneralHardwareExits = 1;
    /* Only the independently proven native caller may make this allocation releasable. */
    Cpu->NestedEntryEnabled = 0; Cpu->HostInterruptsAllowed = 0; nested->GeneralInitialized = 0;
    /* The failed stage/result remain unchanged; this is not a successful resident entry. */
    InterlockedIncrement64(&nested->GeneralSequence); return TRUE;
}

/* Called only after STOP returned natively and the common worker verified current CPU registers. */
BOOLEAN KswordSvmNestedCompleteNative(KSW_SVM_CPU* Cpu)
{
    /* Retain diagnostics until a later explicit reinitialization or teardown. */
    KSW_SVM_NESTED* nested;
    /* An absent coordinator is the ordinary residency path, with no general binding to retire. */
    if (!Cpu || !(nested = Cpu->Nested)) { return Cpu != NULL; }
    /* A partially published mode cannot be silently reset into ordinary execution. */
    if (!Cpu->NestedEntryEnabled && !nested->GeneralInitialized) { return TRUE; }
    /* Only an actual successful general private stop is eligible for retirement. */
    if (!Cpu->NestedEntryEnabled || !nested->GeneralInitialized || (nested->GeneralSequence & 1) ||
        nested->GeneralMachine.LastAction != KSW_NSVM_MACHINE_NATIVE ||
        KswSvmNestedMachineCanStop(&nested->GeneralMachine) != KSW_NSVM_STOP_READY ||
        nested->Nmi.Armed || nested->Nmi.Count || nested->GeneralMachine.IrqWindow.Applied) { return FALSE; }
    /* No general mode reentry may race a partially retired diagnostic snapshot. */
    InterlockedIncrement64(&nested->GeneralSequence);
    /* The subsequent fresh start must bind the new Windows continuation instead of old virtual registers. */
    Cpu->NestedEntryEnabled = 0; Cpu->HostInterruptsAllowed = 0; nested->GeneralInitialized = 0;
    /* Do not clear counters, raw exits, failures, maps or NMI descriptors on the acknowledgement path. */
    InterlockedIncrement64(&nested->GeneralSequence); return TRUE;
}

