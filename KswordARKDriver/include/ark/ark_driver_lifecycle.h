#pragma once

#include <ntddk.h>
#include <wdf.h>

EXTERN_C_START
BOOLEAN KswordARKDriverCoreEnterRequest(VOID);
VOID KswordARKDriverCoreLeaveRequest(VOID);
NTSTATUS KswordARKDriverCoreAttachController(_In_ WDFDEVICE Device);
VOID KswordARKDriverCoreDetachController(_In_ WDFDEVICE Device);
BOOLEAN KswordARKDriverCoreIsControllerDevice(_In_ WDFDEVICE Device);
EXTERN_C_END
