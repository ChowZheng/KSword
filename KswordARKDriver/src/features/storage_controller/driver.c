#include "controller.h"

PVOID
KsccAllocatePool(
    _In_ SIZE_T Length)
{
    PVOID buffer;

    buffer = KswordARKAllocateNonPagedPool(Length, KSCC_POOL_TAG);
    if (buffer != NULL) {
        RtlZeroMemory(buffer, Length);
    }
    return buffer;
}

/*
 * DeviceAdd installs PnP resource callbacks and one sequential IOCTL queue.
 * Sequential dispatch combines with IoLock to keep command queues single-owner.
 */
NTSTATUS
KsccEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDFDEVICE device;
    KSCC_DEVICE_CONTEXT* context;
    static const GUID interfaceGuid =
        KSWORD_ARK_STORAGE_CONTROLLER_INTERFACE_GUID_INIT;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WDF_FILEOBJECT_CONFIG_INIT(
        &fileConfig,
        WDF_NO_EVENT_CALLBACK,
        WDF_NO_EVENT_CALLBACK,
        KsccEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, KSCC_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &attributes);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = KsccEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = KsccEvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = KsccEvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = KsccEvtD0Exit;
    pnpCallbacks.EvtDeviceSurpriseRemoval = KsccEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, KSCC_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = KsccEvtContextCleanup;
    /*
     * Controller requests take a WDFWAITLOCK and audit the caller token.
     * Both operations require PASSIVE_LEVEL, so make the device callback
     * contract explicit instead of relying on the queue's caller IRQL.
     */
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = KsccGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device = device;
    InitializeListHead(&context->CoreLink);
    context->Ownership = KSWORD_ARK_STORAGE_OWNERSHIP_NONE;
    context->Coherency = KSWORD_ARK_STORAGE_COHERENCY_UNKNOWN;
    context->RiskFlags =
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_SYSTEM_DISK_UNKNOWN |
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_LIVE_VOLUMES_UNKNOWN |
        KSWORD_ARK_STORAGE_CONTROLLER_RISK_NO_RECOVERY_GUARANTEE;
    context->LogicalSectorSize = 512U;
    context->PhysicalSectorSize = 512U;
    context->MaximumTransferBytes = KSWORD_ARK_STORAGE_CONTROLLER_MAX_TRANSFER_BYTES;
    context->PciSegment = MAXULONG;
    context->PciBus = MAXULONG;
    context->PciDevice = MAXULONG;
    context->PciFunction = MAXULONG;
    context->LastControllerStatus = MAXULONG;
    context->LastStatus = STATUS_NOT_FOUND;
    context->Rollback = KsccAllocatePool(KSCC_BACKUP_BYTES);
    if (context->Rollback == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeSpinLock(&context->Audit.Lock);

    /*
     * Determine class/subclass/prog-if from PnP compatible IDs before
     * PrepareHardware is allowed to touch any translated register resource.
     */
    status = KsccDetermineControllerType(context);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &context->IoLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &interfaceGuid, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = KsccEvtIoDeviceControl;
    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return KswordARKDriverCoreAttachController(device);
}

/* A closing/crashed owner must not leave the next exclusive opener BUSY. */
VOID
KsccEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    KSCC_DEVICE_CONTEXT* context;

    InterlockedExchange(&KsccGetFileContext(FileObject)->Closing, TRUE);
    context = KsccGetContext(WdfFileObjectGetDevice(FileObject));
    WdfWaitLockAcquire(context->IoLock, NULL);
    if (context->Acquired || context->SessionId != 0ULL ||
        context->RollbackValid) {
        KsccClearSessionState(context);
        KsccAdvanceGeneration(context);
    }
    /* Keep RequiresReset and write uncertainty until verified hardware reset. */
    WdfWaitLockRelease(context->IoLock);
}

/* Requests already queued when cleanup starts cannot reacquire the session. */
BOOLEAN
KsccRequestOwnerClosed(
    _In_ WDFREQUEST Request)
{
    WDFFILEOBJECT fileObject;

    fileObject = WdfRequestGetFileObject(Request);
    return fileObject == WDF_NO_HANDLE ||
        KsccGetFileContext(fileObject)->Closing != FALSE;
}

/* Release the non-WDF rollback snapshot when the PnP device object is deleted. */
VOID
KsccEvtContextCleanup(
    _In_ WDFOBJECT Object)
{
    KSCC_DEVICE_CONTEXT* context;

    context = KsccGetContext((WDFDEVICE)Object);
    KswordARKDriverCoreDetachController((WDFDEVICE)Object);
    if (context->Rollback != NULL) {
        RtlSecureZeroMemory(context->Rollback, KSCC_BACKUP_BYTES);
        ExFreePoolWithTag(context->Rollback, KSCC_POOL_TAG);
        context->Rollback = NULL;
    }
}

BOOLEAN
KswordARKStorageControllerIsDevice(
    _In_ WDFDEVICE Device)
{
    return KswordARKDriverCoreIsControllerDevice(Device);
}
