/* Bounded substitutes for global feature startup/teardown and framework state.
 * The admission, profile, first/last-FDO and terminal shutdown C is production. */
#include <winternl.h>
typedef struct { ULONG Held; } EX_PUSH_LOCK;
typedef struct { ULONG Count; BOOLEAN Closed; } EX_RUNDOWN_REF;
typedef struct { BOOLEAN Signaled; } KEVENT;
typedef PVOID PDRIVER_OBJECT;
typedef PVOID WDFDRIVER;
typedef PVOID PWDFDEVICE_INIT;
typedef struct {
    NTSTATUS (*EvtDriverDeviceAdd)(WDFDRIVER, PWDFDEVICE_INIT);
    VOID (*EvtDriverUnload)(WDFDRIVER);
    ULONG DriverInitFlags;
} WDF_DRIVER_CONFIG;
typedef struct { VOID (*EvtCleanupCallback)(WDFOBJECT); } WDF_OBJECT_ATTRIBUTES;
typedef struct { ULONG TitleIndex, Type, DataLength; UCHAR Data[1]; } KEY_VALUE_PARTIAL_INFORMATION;
typedef KEY_VALUE_PARTIAL_INFORMATION* PKEY_VALUE_PARTIAL_INFORMATION;
#define WDF_NO_EVENT_CALLBACK NULL
#define WdfDriverInitNonPnpDriver 1U
#define WDF_DRIVER_CONFIG_INIT(c,fn) memset((c),0,sizeof(*(c))); (c)->EvtDriverDeviceAdd=(fn)
#define WDF_OBJECT_ATTRIBUTES_INIT(a) memset((a),0,sizeof(*(a)))
#define KSWORD_ARK_MINIMUM_SUPPORTED_OS_BUILD 16299U
#define KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED 1
#define RTL_CONSTANT_STRING(s) {sizeof(s)-sizeof(WCHAR),sizeof(s),(PWSTR)(s)}
#define NotificationEvent 0U
#define Executive 0U
#define KernelMode 0U
#define IO_NO_INCREMENT 0U
#define KeyValuePartialInformation 2U
#define PAGED_CODE() ((void)0)
#define TraceEvents(...) ((void)0)
#define WPP_INIT_TRACING(...) (++testWppInitialize)
#define WPP_CLEANUP(...) (++testWppCleanup)

static ULONG testWppInitialize, testWppCleanup, testCoreInitializeCalls, testCoreUninitializeCalls;
static ULONG testWdfCreates, testControlCreates, testControlDeletes, testControlPurges;
static ULONG testHvmGuardCalls, testWdfFlags, testShutdownWaits, testProfileOpens;
static BOOLEAN testWdfHasAddDevice, testControlAlive, testLateRuntimeStarted;
static BOOLEAN testProfilePresent, testProfileValuePresent;
static ULONG testProfileValue, testProfileType, testProfileBytes;
static NTSTATUS testWdfCreateStatus, testControlCreateStatus, testProfileOpenStatus;
static VOID (*testDrainCallback)(VOID);
static BOOLEAN testDeniedDuringDrain;

static VOID InitializeListHead(PLIST_ENTRY head) { head->Flink=head->Blink=head; }
static VOID InsertTailList(PLIST_ENTRY head, PLIST_ENTRY entry)
{ entry->Blink=head->Blink; entry->Flink=head; head->Blink->Flink=entry; head->Blink=entry; }
static VOID RemoveEntryList(PLIST_ENTRY entry)
{ entry->Blink->Flink=entry->Flink; entry->Flink->Blink=entry->Blink; }
static VOID ExInitializePushLock(EX_PUSH_LOCK* lock) { lock->Held=0; }
static VOID ExAcquirePushLockExclusive(EX_PUSH_LOCK* lock)
{ if (lock->Held) ++failures; ++lock->Held; }
static VOID ExReleasePushLockExclusive(EX_PUSH_LOCK* lock) { --lock->Held; }
static VOID KeEnterCriticalRegion(VOID) {}
static VOID KeLeaveCriticalRegion(VOID) {}
static VOID KeInitializeSpinLock(KSPIN_LOCK* lock) { *lock=0; }
static VOID ExInitializeRundownProtection(EX_RUNDOWN_REF* ref) { memset(ref,0,sizeof(*ref)); }
static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF* ref)
{ if (ref->Closed) return FALSE; ++ref->Count; return TRUE; }
static VOID ExReleaseRundownProtection(EX_RUNDOWN_REF* ref) { if (!ref->Count) ++failures; else --ref->Count; }
static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF* ref)
{ ref->Closed=TRUE; if (testDrainCallback) { testDrainCallback(); testDrainCallback=NULL; } if (ref->Count) ++failures; }
static VOID KeInitializeEvent(KEVENT* event, ULONG type, BOOLEAN set)
{ (void)type; event->Signaled=set; }
static LONG KeSetEvent(KEVENT* event, ULONG priority, BOOLEAN wait)
{ (void)priority; (void)wait; event->Signaled=TRUE; return 0; }
static NTSTATUS KeWaitForSingleObject(KEVENT* event, ULONG reason, ULONG mode, BOOLEAN alertable, PVOID timeout)
{ (void)reason; (void)mode; (void)alertable; (void)timeout; ++testShutdownWaits; if (!event->Signaled) ++failures; return STATUS_SUCCESS; }
static NTSTATUS ZwOpenKey(HANDLE* key, ULONG access, OBJECT_ATTRIBUTES* attributes)
{
    (void)access; ++testProfileOpens;
    if (!NT_SUCCESS(testProfileOpenStatus)) return testProfileOpenStatus;
    if (attributes->RootDirectory && !testProfilePresent) return STATUS_OBJECT_NAME_NOT_FOUND;
    *key=attributes->RootDirectory ? (HANDLE)2 : (HANDLE)1; return STATUS_SUCCESS;
}
static NTSTATUS ZwQueryValueKey(HANDLE key, PUNICODE_STRING name, ULONG kind, PVOID buffer, ULONG bytes, ULONG* returned)
{
    PKEY_VALUE_PARTIAL_INFORMATION value=buffer;
    (void)key; (void)name; (void)kind;
    if (!testProfileValuePresent) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (bytes < sizeof(*value)+sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
    memset(value,0,bytes); value->Type=testProfileType; value->DataLength=testProfileBytes;
    memcpy(value->Data,&testProfileValue,sizeof(ULONG)); *returned=FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+sizeof(ULONG);
    return STATUS_SUCCESS;
}
static NTSTATUS ZwClose(HANDLE key) { (void)key; return STATUS_SUCCESS; }
static NTSTATUS KsccEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{ (void)driver; (void)init; return STATUS_SUCCESS; }
VOID KswordARKDriverCoreDetachController(WDFDEVICE device);
BOOLEAN KswordARKDriverCoreEnterRequest(VOID);
VOID KswordARKDriverEvtDriverUnload(WDFDRIVER driver);
VOID KswordARKDriverEvtDriverContextCleanup(WDFOBJECT driver);

#include "core_lifecycle_state_replay.h"

static NTSTATUS TestCoreInitialize(PCSTR name)
{ if (strncmp(name,"KswordArkStartup",16)!=0) ++testCoreInitializeCalls; return STATUS_SUCCESS; }
static NTSTATUS TestCoreUninitialize(PCSTR name)
{
    (void)name; ++testCoreUninitializeCalls;
    if (g_KswordArkCore.Ready || g_KswordArkCore.Requests.Count) ++failures;
    if (testLateRuntimeStarted && !testControlAlive) ++failures;
    return STATUS_SUCCESS;
}
static NTSTATUS WdfDriverCreate(PDRIVER_OBJECT object, PUNICODE_STRING path,
    WDF_OBJECT_ATTRIBUTES* attributes, WDF_DRIVER_CONFIG* config, WDFDRIVER* driver)
{
    (void)object; (void)path; (void)attributes; ++testWdfCreates;
    testWdfFlags=config->DriverInitFlags; testWdfHasAddDevice=config->EvtDriverDeviceAdd!=NULL;
    *driver=(PVOID)0x200; return testWdfCreateStatus;
}
static NTSTATUS KswordARKDriverCreateControlDevice(WDFDRIVER driver, WDFDEVICE* device)
{
    (void)driver; ++testControlCreates;
    if (!NT_SUCCESS(testControlCreateStatus)) return testControlCreateStatus;
    *device=(PVOID)0x100; testControlAlive=TRUE; testLateRuntimeStarted=TRUE; return STATUS_SUCCESS;
}
static VOID KswordARKDriverPublishControlDevice(WDFDEVICE device)
{ if (device!=(PVOID)0x100 || !testControlAlive || !g_KswordArkCore.Ready) ++failures; }
static ULONG KswordArkStartupGetOsBuildNumber(VOID) { return 26100; }
static NTSTATUS KswordARKHvmEnableResidentLifecycle(PDRIVER_OBJECT driver)
{ (void)driver; ++testHvmGuardCalls; return STATUS_SUCCESS; }
#define KswordArkStartupFailure(stage,status) (status)
static WDFQUEUE WdfDeviceGetDefaultQueue(WDFDEVICE device) { return device; }
static VOID WdfIoQueuePurgeSynchronously(WDFQUEUE queue)
{ if (queue!=(PVOID)0x100 || !g_KswordArkCore.ShutdownDone.Signaled || g_KswordArkCore.Ready) ++failures; ++testControlPurges; }
static VOID WdfObjectDelete(WDFOBJECT object)
{ if (object!=(PVOID)0x100 || !g_KswordArkCore.ShutdownDone.Signaled || !testCoreUninitializeCalls) ++failures; testControlAlive=FALSE; ++testControlDeletes; }
#include "core_feature_stubs.h"
static VOID TestCoreLifecycle(VOID);
