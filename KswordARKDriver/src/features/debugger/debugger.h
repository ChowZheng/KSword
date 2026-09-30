#pragma once

#include "ark/ark_driver.h"
#include "driver/KswordArkDebuggerIoctl.h"

/* Execute a validated, ID-scoped user-process debugger operation at PASSIVE_LEVEL. */
NTSTATUS KswordARKDebuggerControl(WDFDEVICE Device, const KSWORD_ARK_DEBUGGER_REQUEST* Request,
    KSWORD_ARK_DEBUGGER_RESPONSE* Response);
