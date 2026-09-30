#pragma once

#include "hvm_resident.h"
#include "driver/KswordArkHvmDebugIoctl.h"

EXTERN_C_START
/* Register lifetime notifications before publishing debugger capability. */
NTSTATUS KswordARKHvmDebugInitialize(VOID);
/* Drain notifications only after every resident processor has stopped. */
VOID KswordARKHvmDebugShutdown(VOID);
/* Release pinned pages and thread references under the stopped runtime lock. */
VOID KswordARKHvmDebugResetLocked(KSW_HVM_RUNTIME* Runtime);
/* Execute one serialized, owner-scoped breakpoint transaction. */
NTSTATUS KswordARKHvmDebugControl(const KSWORD_ARK_HVM_DEBUG_REQUEST* Request,
    KSWORD_ARK_HVM_DEBUG_RESPONSE* Response, ULONG OwnerProcessId);
/* Return debug-register hit bits without any root allocation or OS call. */
ULONG KswordARKHvmDebugMatchExit(KSW_HVM_RESIDENT_VCPU* Context,
    ULONGLONG PhysicalAddress, ULONG Access, ULONGLONG* Teb);
/* Inject a real guest #DB only when the VM-entry event slot is available. */
BOOLEAN KswordARKHvmDebugInject(ULONG Mask);
/* Finish a data stop after the original instruction and EPT restoration. */
BOOLEAN KswordARKHvmDebugMonitorTrap(KSW_HVM_RESIDENT_VCPU* Context);
EXTERN_C_END
