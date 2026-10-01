#pragma once

#include "ark/ark_driver.h"
#include "driver/KswordArkDebuggerIoctl.h"

/* Execute a validated, ID-scoped user-process debugger operation at PASSIVE_LEVEL. */
NTSTATUS KswordARKDebuggerControl(WDFDEVICE Device, ULONG OwnerProcessId, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response);
NTSTATUS KswordARKDebuggerShadow(ULONG OwnerProcessId, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response);
VOID KswordARKDebuggerProcessExit(ULONG ProcessId);
VOID KswordARKDebuggerShutdown(VOID);
