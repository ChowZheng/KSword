#pragma once

#include <ntddk.h>
#include <wdf.h>

EXTERN_C_START
EVT_WDF_DRIVER_DEVICE_ADD KsccEvtDeviceAdd;
BOOLEAN KswordARKStorageControllerIsDevice(_In_ WDFDEVICE Device);
VOID KswordARKStorageControllerQuery(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
VOID KswordARKStorageControllerControl(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
VOID KswordARKStorageControllerTransfer(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
VOID KswordARKStorageControllerAudit(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
EXTERN_C_END
