/* Production C replay. Hardware, KMDF, pool, token, and time APIs are bounded
 * substitutes: these tests never bind a device, load a driver, or touch disks. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "shared/driver/KswordArkStorageControllerIoctl.h"

typedef LONG NTSTATUS;
typedef LARGE_INTEGER PHYSICAL_ADDRESS;
typedef ULONG_PTR KSPIN_LOCK;
typedef UCHAR KIRQL;
typedef PVOID WDFDEVICE;
typedef PVOID WDFWAITLOCK;
typedef PVOID WDFDMAENABLER;
typedef PVOID WDFCOMMONBUFFER;
typedef PVOID WDFQUEUE;
typedef PVOID WDFOBJECT;
typedef PVOID PACCESS_TOKEN;
typedef PVOID PEPROCESS;
typedef ULONG WDF_POWER_DEVICE_STATE;
#define MAXULONG 0xFFFFFFFFUL
#define MAXUSHORT 0xFFFFU
#define PAGE_SIZE 4096U
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define WDF_NO_HANDLE NULL
#define WdfPowerDevicePrepareForHibernation 100U
#define WdfDeviceFailedNoRestart 1U
#define POOL_FLAG_NON_PAGED 0U
#define RtlSecureZeroMemory(d,n) SecureZeroMemory((d),(n))
#include "controller_context_replay.h"
typedef struct { KSCC_DEVICE_CONTEXT* device; KSCC_FILE_CONTEXT state; } TEST_FILE;
typedef TEST_FILE* WDFFILEOBJECT;
typedef struct {
    UCHAR buffer[KSCC_BACKUP_BYTES + 1024U];
    size_t inputBytes;
    size_t outputBytes;
    size_t completedBytes;
    NTSTATUS completedStatus;
    TEST_FILE* file;
    PEPROCESS requestor;
    ULONG requestorPid;
} TEST_REQUEST;
typedef TEST_REQUEST* WDFREQUEST;
#include "controller_api_replay.h"

static KSCC_DEVICE_CONTEXT context;
static TEST_FILE owner;
static TEST_REQUEST request;
static UCHAR backup[KSCC_BACKUP_BYTES];
static UCHAR media[1024U];
static UCHAR registers[0x2000U];
static UCHAR commandBuffer[4096U], completionBuffer[4096U], auxiliary[8192U];
static UCHAR dataBuffer[KSCC_BACKUP_BYTES], prpBuffer[4096U];
static UCHAR taskFile[8U];
static UCHAR sidBytes[4U] = { 1, 2, 3, 4 };
static ULONG failures, cases, transfers, flushes, writes, barriers, locks;
static ULONG registerReads, registerWrites, stalls, tokenQueries, requestorReferences;
static ULONG dataCycles;
static ULONGLONG ticks;
static NTSTATUS transferFault, flushFault, stopFault;
static ULONG faultCall;
static BOOLEAN corruptVerification, removeOnStall, ideMode, ideBusyAfterCommand;
static ULONG ideCommandPending;
static BOOLEAN ahciComplete;
static ULONG ahciBytes;
static KSCC_DEVICE_CONTEXT* KsccGetContext(WDFDEVICE device) { return device; }
static KSCC_FILE_CONTEXT* KsccGetFileContext(WDFFILEOBJECT file) { return &file->state; }
static WDFDEVICE WdfFileObjectGetDevice(WDFFILEOBJECT file) { return file->device; }
static WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST req) { return req->file; }
static WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE queue) { return queue; }
static NTSTATUS WdfWaitLockAcquire(WDFWAITLOCK lock, PVOID timeout)
{ (void)lock; (void)timeout; ++locks; return STATUS_SUCCESS; }
static VOID WdfWaitLockRelease(WDFWAITLOCK lock) { (void)lock; --locks; }
static VOID WdfDeviceSetFailed(WDFDEVICE device, ULONG action) { (void)device; (void)action; }
static NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST req, size_t minimum, PVOID* out, size_t* bytes)
{ if (req->inputBytes < minimum) return STATUS_BUFFER_TOO_SMALL; *out=req->buffer; *bytes=req->inputBytes; return STATUS_SUCCESS; }
static NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST req, size_t minimum, PVOID* out, size_t* bytes)
{ if (req->outputBytes < minimum) return STATUS_BUFFER_TOO_SMALL; *out=req->buffer; *bytes=req->outputBytes; return STATUS_SUCCESS; }
static VOID WdfRequestComplete(WDFREQUEST req, NTSTATUS status)
{ req->completedStatus=status; req->completedBytes=0; }
static VOID WdfRequestCompleteWithInformation(WDFREQUEST req, NTSTATUS status, size_t bytes)
{ req->completedStatus=status; req->completedBytes=bytes; }
static PVOID KswordARKAllocateNonPagedPool(SIZE_T bytes, ULONG tag)
{ PVOID buffer=malloc(bytes); (void)tag; if (buffer) memset(buffer,0xCC,bytes); return buffer; }
static VOID ExFreePoolWithTag(PVOID p, ULONG tag) { (void)tag; free(p); }
static VOID ExFreePool(PVOID p) { free(p); }
static LARGE_INTEGER KeQueryPerformanceCounter(PLARGE_INTEGER frequency)
{ LARGE_INTEGER result; (void)frequency; result.QuadPart=10000; return result; }
static ULONGLONG KeQueryInterruptTime(VOID) { return ticks; }
static VOID KeStallExecutionProcessor(ULONG us)
{
    ticks += (ULONGLONG)us*10U; ++stalls;
    if (removeOnStall) context.SurpriseRemoved=TRUE;
    if (ideMode && ideCommandPending && us >= 1U) {
        taskFile[7]=ideBusyAfterCommand ? 0x80U : 0x08U;
        ideCommandPending=0U;
    }
}
#define KeMemoryBarrier() (++barriers)
static ULONG TestReadRegister(volatile ULONG* p) { ++registerReads; return *p; }
static VOID TestWriteRegister(volatile ULONG* p, ULONG value)
{
    ++registerWrites; *p=value;
    if (ahciComplete && ((PUCHAR)p==registers+0x110U || (PUCHAR)p==registers+0x130U)) *p=0U;
    if (ahciComplete && (PUCHAR)p==registers+0x138U) {
        *(volatile ULONG*)(commandBuffer+4U)=ahciBytes;
        *p=0U;
    }
}
#define READ_REGISTER_ULONG TestReadRegister
#define WRITE_REGISTER_ULONG TestWriteRegister
static UCHAR TestReadPort(PUCHAR p) { ++registerReads; return *p; }
static VOID TestWritePort(PUCHAR p, UCHAR value)
{
    ++registerWrites;
    if (ideMode && p==taskFile+7U) { ideCommandPending=value; return; }
    *p=value;
}
static VOID TestReadPortBuffer(PUSHORT port, PUSHORT data, ULONG words)
{ (void)port; memset(data,0x44,(size_t)words*2U); ++dataCycles; ticks+=6000U; taskFile[7]=0x08U; }
static VOID TestWritePortBuffer(PUSHORT port, PUSHORT data, ULONG words)
{ (void)port; (void)data; (void)words; ++dataCycles; ticks+=6000U; taskFile[7]=0x08U; }
#define READ_PORT_UCHAR TestReadPort
#define WRITE_PORT_UCHAR TestWritePort
#define READ_PORT_BUFFER_USHORT TestReadPortBuffer
#define WRITE_PORT_BUFFER_USHORT TestWritePortBuffer
static NTSTATUS RtlStringCchCopyW(PWSTR to, SIZE_T chars, PCWSTR from)
{ if (!chars) return STATUS_INVALID_PARAMETER; wcsncpy(to,from,chars-1U); to[chars-1U]=0; return STATUS_SUCCESS; }
static VOID KeAcquireSpinLock(KSPIN_LOCK* lock, KIRQL* irql) { (void)lock; *irql=0U; }
static VOID KeReleaseSpinLock(KSPIN_LOCK* lock, KIRQL irql) { (void)lock; (void)irql; }
static PVOID WdfRequestWdmGetIrp(WDFREQUEST req) { return req; }
static PEPROCESS IoGetRequestorProcess(PVOID irp) { return ((WDFREQUEST)irp)->requestor; }
static ULONG IoGetRequestorProcessId(PVOID irp) { return ((WDFREQUEST)irp)->requestorPid; }
static PACCESS_TOKEN PsReferencePrimaryToken(PEPROCESS process)
{ if (process!=request.requestor) ++failures; ++requestorReferences; return process; }
static VOID PsDereferencePrimaryToken(PACCESS_TOKEN token) { (void)token; }
static NTSTATUS SeQueryInformationToken(PACCESS_TOKEN token, TOKEN_INFORMATION_CLASS kind, PVOID* result)
{ PTOKEN_USER user=calloc(1,sizeof(TOKEN_USER)); (void)token; (void)kind; ++tokenQueries; user->User.Sid=sidBytes; *result=user; return STATUS_SUCCESS; }
static BOOLEAN RtlValidSid(PSID sid) { return sid==sidBytes; }
static ULONG RtlLengthSid(PSID sid) { (void)sid; return sizeof(sidBytes); }

NTSTATUS KsccHardwareTransfer(KSCC_DEVICE_CONTEXT* ctx, BOOLEAN write, ULONGLONG offset,
    UCHAR* buffer, ULONG length, ULONG flags, ULONG timeout, ULONG* controllerStatus)
{
    (void)flags; (void)timeout; ++transfers; *controllerStatus=0U;
    if (transfers==faultCall) {
        if (transferFault==STATUS_IO_TIMEOUT) ctx->RequiresReset=TRUE;
        return transferFault;
    }
    if (offset+length>sizeof(media)) return STATUS_INVALID_PARAMETER;
    if (write) { memcpy(media+(size_t)offset,buffer,length); ++writes; }
    else { memcpy(buffer,media+(size_t)offset,length); if (corruptVerification && writes) buffer[0]^=1U; }
    return STATUS_SUCCESS;
}
NTSTATUS KsccHardwareFlush(KSCC_DEVICE_CONTEXT* ctx, ULONG timeout, ULONG* controllerStatus)
{ (void)ctx; (void)timeout; *controllerStatus=0U; ++flushes; return flushFault; }
NTSTATUS KsccStopController(KSCC_DEVICE_CONTEXT* ctx)
{ if (NT_SUCCESS(stopFault)) ctx->HardwareActivated=FALSE; return stopFault; }
NTSTATUS KsccDetectAndInitialize(KSCC_DEVICE_CONTEXT* ctx)
{ ctx->HardwareActivated=TRUE; ctx->LogicalSectorSize=512; ctx->CapacityBytes=sizeof(media); return STATUS_SUCCESS; }
NTSTATUS KsccAllocateDmaRegion(KSCC_DEVICE_CONTEXT* ctx, KSCC_DMA_REGION* region, SIZE_T bytes)
{ (void)ctx; if (!region->Virtual) region->Virtual=calloc(1,bytes); region->Length=bytes; region->Logical.QuadPart=(LONGLONG)(ULONG_PTR)region->Virtual; return region->Virtual ? STATUS_SUCCESS:STATUS_INSUFFICIENT_RESOURCES; }

static VOID KswordARKDriverDispatchDeviceControl(WDFDEVICE device, WDFQUEUE queue,
    WDFREQUEST req, size_t outputBytes, size_t inputBytes, ULONG code)
{ (void)device; (void)queue; (void)req; (void)outputBytes; (void)inputBytes; (void)code; }
#include "core_lifecycle_test_support.h"

#include "controller_source_replay.h"
#include "core_lifecycle_replay.h"

#define CHECK(name, expr) do { ++cases; if (!(expr)) { ++failures; printf("FAIL: %s (line %d)\n",name,__LINE__); } } while(0)
static VOID Reset(VOID)
{
    memset(&context,0,sizeof(context)); memset(&request,0,sizeof(request)); memset(&owner,0,sizeof(owner));
    memset(backup,0xAB,sizeof(backup)); memset(media,0x11,sizeof(media)); memset(registers,0,sizeof(registers));
    memset(commandBuffer,0,sizeof(commandBuffer)); memset(completionBuffer,0,sizeof(completionBuffer));
    memset(auxiliary,0,sizeof(auxiliary)); memset(taskFile,0,sizeof(taskFile));
    context.Device=&context; context.IoLock=&context; context.Rollback=backup;
    context.Prepared=TRUE; context.ResourcesPrepared=TRUE; context.HardwareActivated=TRUE;
    context.Acquired=TRUE; context.SessionId=7; context.Generation=4; context.LogicalSectorSize=512;
    context.PhysicalSectorSize=512; context.MaximumTransferBytes=KSCC_BACKUP_BYTES; context.CapacityBytes=sizeof(media);
    context.ControllerType=KSWORD_ARK_STORAGE_CONTROLLER_TYPE_NVME;
    context.Bars[0]=registers; context.BarCount=1; context.BarLengths[0]=sizeof(registers);
    context.Command.Virtual=commandBuffer; context.Command.Length=sizeof(commandBuffer);
    context.Completion.Virtual=completionBuffer; context.Completion.Length=sizeof(completionBuffer);
    context.Auxiliary.Virtual=auxiliary; context.Auxiliary.Length=sizeof(auxiliary);
    context.Data.Virtual=dataBuffer; context.Data.Length=sizeof(dataBuffer); context.Data.Logical.QuadPart=0x100000;
    context.Prp.Virtual=prpBuffer; context.Prp.Length=sizeof(prpBuffer); context.Prp.Logical.QuadPart=0x200000;
    context.IoPorts[0]=taskFile; context.IoPorts[1]=taskFile+1; context.IoPortCount=2;
    context.IoPortLengths[0]=8; context.IoPortLengths[1]=1; context.PortOrNamespace=0;
    context.NvmeAdminPhase=1; context.NvmeIoPhase=1;
    owner.device=&context; request.file=&owner; request.requestor=(PVOID)2; request.requestorPid=1234;
    transfers=flushes=writes=barriers=locks=registerReads=registerWrites=stalls=0;
    tokenQueries=requestorReferences=dataCycles=0; ticks=0; faultCall=0;
    transferFault=flushFault=stopFault=STATUS_SUCCESS; corruptVerification=removeOnStall=ideMode=ideBusyAfterCommand=FALSE;
    ideCommandPending=0;
    ahciComplete=FALSE; ahciBytes=512U;
}
static KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE* Transfer(ULONG operation, ULONG generation)
{
    KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST* input=(PVOID)request.buffer;
    memset(request.buffer,0,sizeof(request.buffer)); input->version=1;
    request.inputBytes=KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE+(operation==2U?512U:0U);
    request.outputBytes=KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE+(operation==1U?512U:0U);
    input->size=(ULONG)request.inputBytes; input->operation=operation; input->length=512;
    input->expectedGeneration=generation; input->sessionId=7; input->timeoutMilliseconds=1;
    if (operation!=1U) { input->flags=KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_UI_CONFIRMED; input->confirmationToken=KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN; }
    memset(input->data,0x22,512);
    KsccHandleTransfer(&request,&context); return (PVOID)request.buffer;
}
static VOID TestTransactions(VOID)
{
    KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE* result;
    Reset(); result=Transfer(2U,4U);
    CHECK("write flushes and verifies aliased buffered input", result->status==0 && writes==1 && flushes==1 && transfers==3 && media[0]==0x22 && result->generation==5);
    CHECK("audit uses originating PID and primary token", context.Audit.Rows[0].processId==1234 && tokenQueries==1 && requestorReferences==1);
    result=Transfer(3U,5U); CHECK("conditional rollback restores original bytes", result->status==0 && media[0]==0x11 && result->generation==6 && !context.RollbackValid);
    Reset(); result=Transfer(2U,3U); CHECK("stale generation never reaches hardware", result->status==7 && transfers==0 && context.Generation==4);
    Reset(); context.LogicalSectorSize=0; result=Transfer(1U,4U); CHECK("zero geometry fails before division", result->status==3 && transfers==0);
    Reset(); owner.state.Closing=TRUE; result=Transfer(2U,4U); CHECK("queued write after cleanup cannot execute", result->status==3 && transfers==0);
    Reset(); context.SurpriseRemoved=TRUE; result=Transfer(1U,4U); CHECK("removed device read blocked", result->status==3 && transfers==0);
    Reset(); faultCall=2; transferFault=STATUS_IO_TIMEOUT; result=Transfer(2U,4U);
    CHECK("write timeout advances generation and retains recovery", result->status==15 && result->generation==5 && context.RollbackValid && backup[0]==0x11 && context.RequiresReset);
    Reset(); flushFault=STATUS_IO_DEVICE_ERROR; result=Transfer(2U,4U);
    CHECK("flush failure cannot become successful write", result->status==13 && result->bytesTransferred==0 && context.RollbackValid && transfers==2 && context.LastStatus==STATUS_IO_DEVICE_ERROR);
    Reset(); corruptVerification=TRUE; result=Transfer(2U,4U);
    CHECK("reread corruption fails hash verification", result->status==14 && result->bytesTransferred==0 && context.RollbackValid);
    Reset(); Transfer(2U,4U); media[0]^=1U; result=Transfer(3U,5U);
    CHECK("rollback rejects media changed after write", result->status==12 && writes==1 && context.Generation==5);
    Reset(); context.Generation=MAXULONG; result=Transfer(2U,MAXULONG);
    CHECK("generation wraps past reserved zero", result->status==0 && result->generation==1);
    Reset(); request.requestor=NULL; Transfer(1U,4U);
    CHECK("missing requestor never hashes a system worker token", tokenQueries==0 && requestorReferences==0 && context.Audit.Rows[0].actorSidHash[0]==0);
    CHECK("locks balanced", locks==0);
}
static VOID TestOwnerAndPower(VOID)
{
    KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_REQUEST* input;
    KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_RESPONSE* output;
    Reset(); context.RollbackValid=TRUE; context.RollbackLength=512; context.RequiresReset=TRUE;
    KsccEvtFileCleanup(&owner);
    CHECK("crashed owner releases session and erases recovery", !context.Acquired && !context.RollbackValid && context.SessionId==0 && backup[0]==0 && backup[sizeof(backup)-1]==0 && context.Generation==5);
    CHECK("cleanup retains timeout latch and rejects old requests", context.RequiresReset && KsccRequestOwnerClosed(&request));
    KsccEvtFileCleanup(&owner); CHECK("repeated cleanup does not advance generation", context.Generation==5);
    Reset(); KsccEvtFileCleanup(&owner);
    input=(PVOID)request.buffer; memset(input,0,sizeof(*input)); input->version=1; input->size=sizeof(*input);
    input->command=1; input->flags=9; input->expectedGeneration=context.Generation; input->confirmationToken=KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN;
    request.inputBytes=sizeof(*input); request.outputBytes=sizeof(*output);
    KsccHandleControl(&request,&context); output=(PVOID)request.buffer;
    CHECK("queued acquire cannot revive a closed owner", output->status==3 && !context.Acquired);
    Reset(); KsccEvtFileCleanup(&owner); owner.state.Closing=FALSE; context.RequiresReset=FALSE;
    memset(input,0,sizeof(*input)); input->version=1; input->size=sizeof(*input); input->command=1; input->flags=9;
    input->expectedGeneration=context.Generation; input->confirmationToken=KSWORD_ARK_STORAGE_CONTROLLER_CONFIRMATION_TOKEN;
    request.inputBytes=sizeof(*input); request.outputBytes=sizeof(*output); KsccHandleControl(&request,&context);
    CHECK("next opener can acquire", output->status==0 && context.Acquired && context.SessionId!=0);
    Reset(); KsccEvtSurpriseRemoval(&context); KsccEvtD0Exit(&context,3);
    CHECK("surprise removal exits without register access", !context.Prepared && !context.Acquired && !context.HardwareActivated && registerReads==0 && registerWrites==0);
    CHECK("removed device cannot return to D0", KsccEvtD0Entry(&context,3)==STATUS_DEVICE_NOT_READY);
    Reset(); context.RollbackValid=TRUE; KsccEvtD0Exit(&context,WdfPowerDevicePrepareForHibernation);
    CHECK("hibernation pseudo state preserves active ownership", context.Prepared && context.Acquired && context.RollbackValid && context.Generation==4);
    Reset(); stopFault=STATUS_IO_TIMEOUT; KsccEvtD0Exit(&context,3);
    CHECK("failed stop preserves recovery and latches failure", context.RequiresReset && context.HardwareActivated && context.Acquired && !context.Prepared);
    CHECK("power and cleanup locks balanced", locks==0);
}
static VOID TestGeometry(VOID)
{
    CHECK("512 sector geometry", KsccValidateAtaGeometry(512,4096,100,262144)==0);
    CHECK("4K geometry", KsccValidateAtaGeometry(4096,4096,100,262144)==0);
    CHECK("invalid tiny sector", !NT_SUCCESS(KsccValidateAtaGeometry(2,512,100,262144)));
    CHECK("invalid nonpower sector", !NT_SUCCESS(KsccValidateAtaGeometry(768,4096,100,262144)));
    CHECK("sector exceeds transfer capacity", !NT_SUCCESS(KsccValidateAtaGeometry(524288,524288,100,262144)));
    CHECK("zero media", !NT_SUCCESS(KsccValidateAtaGeometry(512,512,0,262144)));
    CHECK("ATA upper bits cannot alias media LBA", !NT_SUCCESS(KsccValidateAtaGeometry(512,512,(1ULL<<48)+1,262144)));
    CHECK("ATA maximum sector count", KsccValidateAtaGeometry(512,512,1ULL<<48,262144)==0);
    CHECK("capacity multiplication overflow", KsccValidateAtaGeometry(262144,262144,1ULL<<48,262144)==STATUS_INTEGER_OVERFLOW);
    CHECK("physical sector smaller than logical", !NT_SUCCESS(KsccValidateAtaGeometry(4096,512,100,262144)));
}
static VOID TestProtocolLengths(VOID)
{
    KSWORD_ARK_QUERY_STORAGE_CONTROLLER_REQUEST* query;
    KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_REQUEST* control;
    KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT_REQUEST* audit;
    KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST* transfer;
    Reset(); query=(PVOID)request.buffer; query->version=1; query->size=sizeof(*query)+1U;
    request.inputBytes=sizeof(*query); request.outputBytes=sizeof(KSWORD_ARK_QUERY_STORAGE_CONTROLLER_RESPONSE);
    KsccHandleQuery(&request,&context);
    CHECK("query cannot declare bytes beyond actual input", request.completedStatus==STATUS_INVALID_PARAMETER && request.completedBytes==0);
    Reset(); control=(PVOID)request.buffer; control->version=1; control->size=sizeof(*control)+1U;
    request.inputBytes=sizeof(*control); request.outputBytes=sizeof(KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_RESPONSE);
    KsccHandleControl(&request,&context);
    CHECK("control cannot declare bytes beyond actual input", ((KSWORD_ARK_CONTROL_STORAGE_CONTROLLER_RESPONSE*)request.buffer)->status==1 && context.Generation==4);
    Reset(); audit=(PVOID)request.buffer; audit->version=1; audit->size=sizeof(*audit)+1U;
    request.inputBytes=sizeof(*audit); request.outputBytes=sizeof(KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT_RESPONSE);
    KsccHandleAudit(&request,&context);
    CHECK("audit cannot declare bytes beyond actual input", ((KSWORD_ARK_QUERY_STORAGE_CONTROLLER_AUDIT_RESPONSE*)request.buffer)->status==1);
    Reset(); transfer=(PVOID)request.buffer; transfer->version=1; transfer->size=KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_REQUEST_HEADER_SIZE;
    transfer->operation=2; transfer->length=512; transfer->flags=KSWORD_ARK_STORAGE_CONTROLLER_TRANSFER_FLAG_UI_CONFIRMED;
    request.inputBytes=transfer->size+512; request.outputBytes=KSWORD_ARK_TRANSFER_STORAGE_CONTROLLER_RESPONSE_HEADER_SIZE;
    KsccHandleTransfer(&request,&context);
    CHECK("write payload must fit declared input span", request.completedStatus==STATUS_BUFFER_TOO_SMALL && transfers==0);
    Reset(); query=(PVOID)request.buffer; query->version=1; query->size=sizeof(*query);
    request.inputBytes=sizeof(*query); request.outputBytes=sizeof(KSWORD_ARK_QUERY_STORAGE_CONTROLLER_RESPONSE);
    context.RequiresReset=TRUE; KsccHandleQuery(&request,&context);
    CHECK("reset latch yields valid NOT_READY identity response", request.completedStatus==STATUS_SUCCESS && ((KSWORD_ARK_QUERY_STORAGE_CONTROLLER_RESPONSE*)request.buffer)->status==3 && request.completedBytes==request.outputBytes && locks==0);
}
static VOID TestNvme(VOID)
{
    KSCC_NVME_COMMAND command={0}; KSCC_NVME_COMPLETION* row; ULONG controllerStatus; NTSTATUS status;
    Reset(); row=(PVOID)completionBuffer; row->Status=1; row->CommandId=0; row->SqId=0; row->SqHead=1;
    status=KsccNvmeAdminCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe matching admin completion", status==0 && context.NvmeAdminHead==1 && barriers>=2);
    Reset(); row=(PVOID)completionBuffer; row->Status=1; row->CommandId=99;
    status=KsccNvmeAdminCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe wrong command cannot become success", status==STATUS_DEVICE_DATA_ERROR && context.RequiresReset && context.NvmeAdminHead==0);
    Reset(); row=(PVOID)(auxiliary+4096); row->Status=1; row->SqId=0;
    status=KsccNvmeIoCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe wrong submission queue rejected", status==STATUS_DEVICE_DATA_ERROR && context.RequiresReset);
    Reset(); context.NvmeIoHead=63; context.NvmeIoTail=63; row=(PVOID)(auxiliary+4096); row+=63;
    row->Status=1; row->CommandId=63; row->SqId=1; row->SqHead=0;
    status=KsccNvmeIoCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe ring wrap changes phase", status==0 && context.NvmeIoHead==0 && context.NvmeIoTail==0 && context.NvmeIoPhase==0);
    Reset(); row=(PVOID)(auxiliary+4096); row->Status=1; row->SqId=1; row->SqHead=64;
    status=KsccNvmeIoCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe invalid hardware head rejected", status==STATUS_DEVICE_DATA_ERROR && context.RequiresReset);
    Reset(); status=KsccNvmeIoCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe timeout bounded and latched", status==STATUS_IO_TIMEOUT && context.RequiresReset && stalls==20);
    Reset(); removeOnStall=TRUE; status=KsccNvmeIoCommand(&context,&command,1,&controllerStatus);
    CHECK("NVMe removal aborts polling", status==STATUS_DEVICE_NOT_CONNECTED && stalls==1);
    Reset(); context.SurpriseRemoved=TRUE; status=KsccNvmeIoCommand(&context,&command,1,&controllerStatus);
    CHECK("removed NVMe never touches a doorbell", status==STATUS_DEVICE_NOT_CONNECTED && registerWrites==0 && registerReads==0);
}
static VOID TestIdeAndAhci(VOID)
{
    UCHAR bytes[2048]; ULONG controllerStatus; NTSTATUS status; PUCHAR port;
    Reset(); ideMode=TRUE; status=KsccIdeTransfer(&context,FALSE,0,bytes,sizeof(bytes),0,1,&controllerStatus);
    CHECK("IDE one deadline covers all data sectors", status==STATUS_IO_TIMEOUT && dataCycles==2 && ticks<20000);
    Reset(); ideMode=TRUE; ideBusyAfterCommand=TRUE; status=KsccIdeFlush(&context,1,&controllerStatus);
    CHECK("IDE command settle prevents false flush success", status==STATUS_IO_TIMEOUT && stalls>1 && ideCommandPending==0);
    Reset(); ideMode=TRUE; ideBusyAfterCommand=TRUE; taskFile[7]=IDE_STATUS_ERROR;
    status=KsccIdeFlush(&context,1,&controllerStatus);
    CHECK("IDE stale previous error does not suppress new command", status==STATUS_IO_TIMEOUT && registerWrites==1 && stalls>1);
    Reset(); context.SurpriseRemoved=TRUE; status=KsccIdeTransfer(&context,FALSE,0,bytes,512,0,1,&controllerStatus);
    CHECK("removed IDE returns initialized status without ports", status==STATUS_DEVICE_NOT_CONNECTED && controllerStatus==0 && registerReads==0 && registerWrites==0);
    Reset(); status=KsccIdeTransfer(&context,FALSE,(1ULL<<48)*512ULL,bytes,512,0,1,&controllerStatus);
    CHECK("IDE LBA upper bits fail before register writes", status==STATUS_INVALID_PARAMETER && registerWrites==0);
    Reset(); context.BarLengths[0]=0x180;
    CHECK("AHCI first complete port fits BAR", KsccAhciGetPortWindow(&context,0,&port));
    CHECK("AHCI later port cannot escape BAR", !KsccAhciGetPortWindow(&context,1,&port));
    Reset(); context.SurpriseRemoved=TRUE; status=KsccAhciStop(&context);
    CHECK("removed AHCI stop cannot touch MMIO", status==STATUS_DEVICE_NOT_CONNECTED && registerReads==0 && registerWrites==0);
    Reset(); status=KsccAhciTransfer(&context,FALSE,(1ULL<<48)*512ULL,bytes,512,0,1,&controllerStatus);
    CHECK("AHCI LBA upper bits fail before register writes", status==STATUS_INVALID_PARAMETER && registerWrites==0);
    Reset(); ahciComplete=TRUE; ahciBytes=256U;
    status=KsccAhciTransfer(&context,FALSE,0,bytes,512,0,1,&controllerStatus);
    CHECK("AHCI short DMA cannot become full read", status==STATUS_DEVICE_DATA_ERROR);
    Reset(); ahciComplete=TRUE;
    status=KsccAhciTransfer(&context,FALSE,0,bytes,512,0,1,&controllerStatus);
    CHECK("AHCI full DMA succeeds after barriers", status==STATUS_SUCCESS && barriers>=2);
}
static VOID ResetCoreSpies(BOOLEAN pnp)
{
    Reset();
    testWppInitialize=testWppCleanup=testCoreInitializeCalls=testCoreUninitializeCalls=0;
    testWdfCreates=testControlCreates=testControlDeletes=testControlPurges=0;
    testHvmGuardCalls=testWdfFlags=testShutdownWaits=testProfileOpens=0;
    testWdfHasAddDevice=testControlAlive=testLateRuntimeStarted=FALSE;
    testProfilePresent=testProfileValuePresent=TRUE;
    testProfileValue=pnp; testProfileType=REG_DWORD; testProfileBytes=sizeof(ULONG);
    testWdfCreateStatus=testControlCreateStatus=testProfileOpenStatus=STATUS_SUCCESS;
    testDrainCallback=NULL; testDeniedDuringDrain=FALSE;
}
static VOID CompleteAdmittedRequestDuringDrain(VOID)
{
    testDeniedDuringDrain=!KswordARKDriverCoreEnterRequest();
    KswordARKDriverCoreLeaveRequest();
}
static VOID TestCoreLifecycle(VOID)
{
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\KswordARK");
    KSCC_DEVICE_CONTEXT second;
    ULONG before;
    NTSTATUS status;
    PVOID allocated;
    ResetCoreSpies(FALSE); status=DriverEntry((PVOID)0x300,&path);
    CHECK("legacy default retains NonPnp and eager CDO", status==0 && testWdfFlags==WdfDriverInitNonPnpDriver && !testWdfHasAddDevice && testControlCreates==1 && g_KswordArkCore.Ready);
    CHECK("legacy profile alone permits resident guard setup", testHvmGuardCalls==1);
    CHECK("published core admits request with rundown", KswordARKDriverCoreEnterRequest() && g_KswordArkCore.Requests.Count==1);
    KswordARKDriverCoreLeaveRequest(); KswordARKDriverEvtDriverUnload((PVOID)0x200);
    before=testCoreUninitializeCalls; KswordARKDriverEvtDriverUnload((PVOID)0x200);
    CHECK("core unload is terminal and idempotent", before>0 && testCoreUninitializeCalls==before && !KswordARKDriverCoreEnterRequest() && testControlDeletes==0);
    KswordARKDriverEvtDriverContextCleanup((PVOID)0x200); KswordARKDriverEvtDriverContextCleanup((PVOID)0x200);
    CHECK("WPP cleanup runs once", testWppInitialize==1 && testWppCleanup==1);

    ResetCoreSpies(TRUE); status=DriverEntry((PVOID)0x300,&path);
    CHECK("PnP startup registers AddDevice without NonPnp", status==0 && testWdfFlags==0 && testWdfHasAddDevice);
    CHECK("PnP startup leaves no CDO orphan", testControlCreates==0 && !g_KswordArkCore.Ready && !KswordARKDriverCoreEnterRequest());
    CHECK("PnP startup keeps resident unload guard unavailable", testHvmGuardCalls==0);
    status=KswordARKDriverCoreAttachController(&context);
    CHECK("first controller starts and publishes main core", status==0 && testControlCreates==1 && g_KswordArkCore.Ready && g_KswordArkCore.ControllerCount==1);
    CHECK("role lookup does not confuse control and FDO", KswordARKDriverCoreIsControllerDevice(&context) && !KswordARKDriverCoreIsControllerDevice((PVOID)0x100));
    second=context; second.Device=&second; second.CoreRegistered=FALSE; InitializeListHead(&second.CoreLink);
    status=KswordARKDriverCoreAttachController(&second);
    CHECK("second controller shares one core CDO", status==0 && testControlCreates==1 && g_KswordArkCore.ControllerCount==2);
    KswordARKDriverCoreDetachController(&context);
    CHECK("removing one controller preserves shared core", testControlDeletes==0 && testCoreUninitializeCalls==0 && g_KswordArkCore.ControllerCount==1 && !KswordARKDriverCoreIsControllerDevice(&context));
    CHECK("last controller remains registered", KswordARKDriverCoreIsControllerDevice(&second));
    KswordARKDriverCoreDetachController(&second);
    CHECK("last controller drains globals then purges/deletes CDO", testControlDeletes==1 && testControlPurges==1 && testCoreUninitializeCalls>0 && g_KswordArkCore.ShutdownDone.Signaled && !testControlAlive && g_KswordArkCore.RegistryPath.Buffer==NULL);
    CHECK("removed role and core admission are closed", !KswordARKDriverCoreIsControllerDevice(&second) && !KswordARKDriverCoreEnterRequest());
    before=testCoreUninitializeCalls; KswordARKDriverCoreDetachController(&second);
    CHECK("duplicate cleanup cannot underflow controller count", g_KswordArkCore.ControllerCount==0 && testCoreUninitializeCalls==before && testControlDeletes==1);
    status=KswordARKDriverCoreAttachController(&context);
    CHECK("new FDO cannot restart a retiring image", status==STATUS_DELETE_PENDING && !context.CoreRegistered && testControlCreates==1);
    KswordARKDriverEvtDriverUnload((PVOID)0x200);
    CHECK("framework unload after last FDO cannot repeat teardown", testCoreUninitializeCalls==before && testControlDeletes==1);
    KswordARKDriverEvtDriverContextCleanup((PVOID)0x200);

    ResetCoreSpies(TRUE); status=DriverEntry((PVOID)0x300,&path);
    status=KswordARKDriverCoreAttachController(&context);
    CHECK("admit request before final device removal", status==0 && KswordARKDriverCoreEnterRequest());
    testDrainCallback=CompleteAdmittedRequestDuringDrain;
    KswordARKDriverCoreDetachController(&context);
    CHECK("rundown blocks new admission while old handler completes", testDeniedDuringDrain && g_KswordArkCore.Requests.Count==0 && testDrainCallback==NULL);
    CHECK("CDO deletion follows admitted request drain", testControlDeletes==1 && testControlPurges==1 && g_KswordArkCore.ShutdownDone.Signaled);
    KswordARKDriverEvtDriverContextCleanup((PVOID)0x200);

    ResetCoreSpies(TRUE); status=DriverEntry((PVOID)0x300,&path);
    testControlCreateStatus=STATUS_INSUFFICIENT_RESOURCES;
    status=KswordARKDriverCoreAttachController(&context);
    CHECK("failed first core startup unwinds early globals", status==STATUS_INSUFFICIENT_RESOURCES && testCoreUninitializeCalls>0 && g_KswordArkCore.Retiring && !g_KswordArkCore.Ready);
    KswordARKDriverCoreDetachController(&context);
    CHECK("failed startup leaves no orphan CDO", !testControlAlive && testControlDeletes==0 && g_KswordArkCore.ControllerCount==0 && g_KswordArkCore.RegistryPath.Buffer==NULL);
    KswordARKDriverEvtDriverContextCleanup((PVOID)0x200);

    ResetCoreSpies(FALSE); testWdfCreateStatus=STATUS_INSUFFICIENT_RESOURCES;
    status=DriverEntry((PVOID)0x300,&path);
    CHECK("framework creation failure tears down early state and trace", status==STATUS_INSUFFICIENT_RESOURCES && testCoreUninitializeCalls>0 && testControlCreates==0 && testWppCleanup==1 && g_KswordArkCore.RegistryPath.Buffer==NULL);
    ResetCoreSpies(FALSE); testProfilePresent=FALSE; status=DriverEntry((PVOID)0x300,&path);
    CHECK("absent optional parameters selects legacy", status==0 && !testWdfHasAddDevice && testWdfFlags==WdfDriverInitNonPnpDriver);
    KswordARKDriverEvtDriverUnload((PVOID)0x200); KswordARKDriverEvtDriverContextCleanup((PVOID)0x200);
    ResetCoreSpies(FALSE); testProfileValue=2; status=DriverEntry((PVOID)0x300,&path);
    CHECK("invalid profile cannot create framework or callbacks", status==STATUS_INVALID_PARAMETER && testWdfCreates==0 && testCoreInitializeCalls==0 && testWppCleanup==1);
    ResetCoreSpies(FALSE); testProfileType=REG_SZ; status=DriverEntry((PVOID)0x300,&path);
    CHECK("profile type must be DWORD", status==STATUS_INVALID_PARAMETER && testWdfCreates==0);
    ResetCoreSpies(FALSE); testProfileBytes=1; status=DriverEntry((PVOID)0x300,&path);
    CHECK("profile length must fit DWORD", status==STATUS_INVALID_PARAMETER && testWdfCreates==0);
    ResetCoreSpies(FALSE); testProfileOpenStatus=STATUS_ACCESS_DENIED; status=DriverEntry((PVOID)0x300,&path);
    CHECK("profile query error cannot silently enable a mode", status==STATUS_ACCESS_DENIED && testWdfCreates==0 && testWppCleanup==1);
    allocated=KsccAllocatePool(512);
    CHECK("compat pool helper zeroes legacy allocations", allocated && ((UCHAR*)allocated)[0]==0 && ((UCHAR*)allocated)[511]==0);
    free(allocated);
}
int main(VOID)
{
    CHECK("native LLP64 protocol ABI", sizeof(ULONG)==4 && sizeof(ULONGLONG)==8 && sizeof(WCHAR)==2);
    TestTransactions(); TestOwnerAndPower(); TestGeometry(); TestProtocolLengths(); TestNvme(); TestIdeAndAhci();
    TestCoreLifecycle();
    printf("Controller production regression: %lu cases, %lu failures\n",cases,failures);
    return failures ? 1 : 0;
}
