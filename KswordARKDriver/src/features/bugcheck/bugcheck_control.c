/*++

Module Name:

    bugcheck_control.c

Abstract:

    按需安装蓝屏诊断的控制层。驱动加载只初始化本文件的同步状态，
    不扫描 ntoskrnl、不解析 BGP 私有函数，也不注册 BugCheck 回调。

Environment:

    Kernel-mode Driver Framework

--*/

#include "bugcheck_internal.h"
#include "bugcheck_bgp.h"

// 生命周期状态只允许由控制锁持有者修改，避免并发 IOCTL 重复排队或重复清理。
#define KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE     0L
#define KSWORD_ARK_BUGCHECK_CONTROL_INSTALLING   1L
#define KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED    2L
#define KSWORD_ARK_BUGCHECK_CONTROL_UNINSTALLING 3L

// BGP 私有函数解析和矩形预生成必须有内核侧硬预算。R3 超时不能中止已经进入
// 内核的同步 IOCTL，因此预算与取消状态必须由实际执行准备工作的 R0 控制器拥有。
#define KSWORD_ARK_BUGCHECK_INSTALL_TIMEOUT_100NS (30ULL * 1000ULL * 10000ULL)

// 控制锁在 DriverEntry 阶段建立，不放入会被完整初始化例程清零的诊断状态结构。
// 锁只保护排队和终态发布，绝不覆盖耗时的 BGP 初始化或资源销毁。
static FAST_MUTEX g_KswordArkBugcheckControlLock;
static volatile LONG g_KswordArkBugcheckControlReady = 0L;
// 模式独立于会被初始化清零的诊断结构，安装前选择不会被工作项覆盖。
static volatile LONG g_KswordArkBugcheckControlRenderMode =
    KSWORD_ARK_BUGCHECK_RENDER_MODE_DIAGNOSTIC;
static volatile LONG g_KswordArkBugcheckControlLifecycle =
    KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE;
static volatile LONG g_KswordArkBugcheckControlCancelRequested = 0L;
static volatile LONG g_KswordArkBugcheckControlLastStatus = STATUS_SUCCESS;
static volatile LONG g_KswordArkBugcheckControlProtocolStatus =
    KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_INACTIVE;
static volatile LONG64 g_KswordArkBugcheckControlDeadline = 0LL;
static PDRIVER_OBJECT g_KswordArkBugcheckControlDriverObject = NULL;
static WDFDEVICE g_KswordArkBugcheckControlDevice = WDF_NO_HANDLE;
static WDFWORKITEM g_KswordArkBugcheckControlWorkItem = WDF_NO_HANDLE;

// 跨安装代次的同步对象保持常驻，绝不能随 g_KswordArkBugcheckState 一起清零。
static EX_RUNDOWN_REF g_KswordArkBugcheckTrackingRundown;
static volatile LONG g_KswordArkBugcheckTrackingInitialized = 0L; // 初次通知早于控制器初始化时必须短路。
static volatile LONG g_KswordArkBugcheckTrackingAccepting = 0L; // 门禁不放入重装过程中正在清零的结构。

VOID
KswordARKBugcheckTrackingInitialize(VOID)
{
    ExInitializeRundownProtection(&g_KswordArkBugcheckTrackingRundown); // DriverEntry 仅建立一次独立同步对象。
    ExWaitForRundownProtectionRelease(&g_KswordArkBugcheckTrackingRundown); // 初始空代次保持关闭，首轮安装才重开。
    InterlockedExchange(&g_KswordArkBugcheckTrackingInitialized, 1L); // 初始关闭完成后才允许读取 rundown。
}

VOID
KswordARKBugcheckTrackingStart(VOID)
{
    ExReInitializeRundownProtection(&g_KswordArkBugcheckTrackingRundown); // 控制器已等待旧代次排空，之后才复用同步对象。
    InterlockedExchange(&g_KswordArkBugcheckState.TrackingReady, 1L); // 缓存与 evidence 锁已完整建立。
    InterlockedExchange(&g_KswordArkBugcheckTrackingAccepting, 1L); // 最后发布独立准入，写者不会观察初始化中的状态。
}

VOID
KswordARKBugcheckTrackingStop(VOID)
{
    InterlockedExchange(&g_KswordArkBugcheckTrackingAccepting, 0L); // 先拒绝新运行期写者与延迟的旧代次观察者。
    InterlockedExchange(&g_KswordArkBugcheckState.TrackingReady, 0L); // 既有发布函数同步停止新缓存提交。
    if (InterlockedCompareExchange(&g_KswordArkBugcheckTrackingInitialized, 0L, 0L) != 0L) { // 初始化前不得使用 opaque rundown。
        ExWaitForRundownProtectionRelease(&g_KswordArkBugcheckTrackingRundown); // 已进入者全部退场后才可清零缓存或重置锁。
    }
}

BOOLEAN
KswordARKBugcheckTrackingAcquire(VOID)
{
    if (KeGetCurrentIrql() > APC_LEVEL || // 此引用只用于正常通知与运行期栈采样，不在 BugCheck 现场获取。
        InterlockedCompareExchange(&g_KswordArkBugcheckTrackingAccepting, 0L, 0L) == 0L) { // 初次初始化前接受位始终为零。
        return FALSE; // 不接触尚未初始化或已经排空的同步对象。
    }
    if (!ExAcquireRundownProtection(&g_KswordArkBugcheckTrackingRundown)) { // Stop 已封闭旧代次时不再建立引用。
        return FALSE; // 不能访问后续可重置的状态和锁。
    }
    if (InterlockedCompareExchange(&g_KswordArkBugcheckTrackingAccepting, 0L, 0L) == 0L) { // 取得引用后重新检查关闭竞态。
        ExReleaseRundownProtection(&g_KswordArkBugcheckTrackingRundown); // 交还未用于缓存访问的临时引用。
        return FALSE; // 停止时仅接受已有实际访问者的排空。
    }
    return TRUE; // 引用覆盖缓存、evidence 与操作栈的全部访问窗口。
}

VOID
KswordARKBugcheckTrackingRelease(VOID)
{
    ExReleaseRundownProtection(&g_KswordArkBugcheckTrackingRundown); // 释放后调用者不能继续访问可重置状态。
}

static VOID
KswordARKBugcheckControlInstallWorker(
    _In_ WDFWORKITEM WorkItem
    );

// 调用方无需控制锁；返回单次原子快照，崩溃路径不访问分页状态或同步等待。
ULONG
KswordARKBugcheckControlGetRenderMode(
    VOID
    )
{
    // 固定 LONG 读取与配置端 InterlockedExchange 配对，避免两个后端读取撕裂状态。
    return (ULONG)InterlockedCompareExchange(
        &g_KswordArkBugcheckControlRenderMode,
        0L,
        0L);
}

static ULONG
KswordARKBugcheckControlCallbackMask(
    VOID
    )
{
    ULONG callbackMask = 0UL;

    // 四种 Windows BugCheck 回调都完成注册，才向 R3 报告完整回调集合可用。
    if (g_KswordArkBugcheckState.ClassicRegistered) {
        callbackMask |= 0x00000001UL;
    }
    if (g_KswordArkBugcheckState.SecondaryRegistered) {
        callbackMask |= 0x00000002UL;
    }
    if (g_KswordArkBugcheckState.DumpIoRegistered) {
        callbackMask |= 0x00000004UL;
    }
    if (g_KswordArkBugcheckState.TriageRegistered) {
        callbackMask |= 0x00000008UL;
    }
    return callbackMask;
}

NTSTATUS
KswordARKBugcheckControlCheckAbort(
    VOID
    )
{
#if !KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED
    return STATUS_NOT_SUPPORTED;
#else
    ULONGLONG deadline;

    if (InterlockedCompareExchange(
            &g_KswordArkBugcheckControlCancelRequested,
            0L,
            0L) != 0L ||
        InterlockedCompareExchange(
            &g_KswordArkBugcheckControlReady,
            0L,
            0L) == 0L) {
        return STATUS_CANCELLED;
    }

    deadline = (ULONGLONG)InterlockedCompareExchange64(
        &g_KswordArkBugcheckControlDeadline,
        0LL,
        0LL);
    if (deadline != 0ULL && KeQueryInterruptTime() >= deadline) {
        return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
#endif
}

static VOID
KswordARKBugcheckControlFillResponse(
    _Out_ KSWORD_ARK_BUGCHECK_DIAGNOSTICS_RESPONSE* Response,
    _In_ ULONG ProtocolStatus,
    _In_ NTSTATUS LastStatus
    )
{
    KSWORD_ARK_BGP_DUMP_STATE bgpSnapshot;
    ULONG callbackMask = 0UL;
    LONG lifecycle = KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE;

    // 所有响应字段先归零，防止失败路径向 R3 泄漏未初始化的内核栈内容。
    RtlZeroMemory(Response, sizeof(*Response));
    Response->size = sizeof(*Response);
    Response->version = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_PROTOCOL_VERSION;
    Response->status = ProtocolStatus;
    Response->lastStatus = (LONG)LastStatus;
    Response->renderMode = KswordARKBugcheckControlGetRenderMode(); // 任何终态都回读实际模式。

    // BGP 快照只读取已发布的非分页状态；安装工作项运行时可用于轮询准备阶段。
    RtlZeroMemory(&bgpSnapshot, sizeof(bgpSnapshot));
    KswordARKBugcheckBgpSnapshot(&bgpSnapshot);
    Response->bgpState = bgpSnapshot.State;
    Response->bgpPreparationStage = bgpSnapshot.PreparationStage;
    Response->bgpPreparationStatus = (LONG)bgpSnapshot.PreparationStatus;
    Response->panelStatus = (LONG)bgpSnapshot.PreparationStatus;

    lifecycle = InterlockedCompareExchange(
        &g_KswordArkBugcheckControlLifecycle,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE);
    callbackMask = KswordARKBugcheckControlCallbackMask();
    Response->callbackMask = callbackMask;
    if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED) {
        Response->stateFlags |= KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATE_INSTALLED;
    }
    if (callbackMask == 0x0000000FUL) {
        Response->stateFlags |= KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATE_CALLBACKS_READY;
    }
    if (bgpSnapshot.State == KswordArkBgpStateReady ||
        bgpSnapshot.State == KswordArkBgpStateArmed ||
        bgpSnapshot.State == KswordArkBgpStateDrawn) {
        Response->stateFlags |= KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATE_BGP_BACKEND_READY;
    }
    if (bgpSnapshot.State == KswordArkBgpStateArmed ||
        bgpSnapshot.State == KswordArkBgpStateDrawn) {
        Response->stateFlags |= KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATE_PANEL_READY;
    }
}

static VOID
KswordARKBugcheckControlInstallWorker(
    _In_ WDFWORKITEM WorkItem
    )
{
    NTSTATUS installStatus;
    NTSTATUS abortStatus;

    UNREFERENCED_PARAMETER(WorkItem);
    installStatus = KswordARKBugcheckControlCheckAbort();
    if (NT_SUCCESS(installStatus)) {
        installStatus = KswordARKBugcheckInitialize(
            g_KswordArkBugcheckControlDriverObject,
            g_KswordArkBugcheckControlDevice);
    }

    // 卸载或预算到期可能与初始化刚好同时发生；终态发布前再检查一次。
    abortStatus = KswordARKBugcheckControlCheckAbort();
    if (NT_SUCCESS(installStatus) && !NT_SUCCESS(abortStatus)) {
        installStatus = abortStatus;
    }

    // 成功终态在锁内发布。卸载若已先撤销 ready，则本工作项负责清掉刚完成的安装。
    if (NT_SUCCESS(installStatus)) {
        ExAcquireFastMutex(&g_KswordArkBugcheckControlLock);
        if (InterlockedCompareExchange(
                &g_KswordArkBugcheckControlReady,
                0L,
                0L) != 0L &&
            InterlockedCompareExchange(
                &g_KswordArkBugcheckControlCancelRequested,
                0L,
                0L) == 0L) {
            InterlockedExchange64(&g_KswordArkBugcheckControlDeadline, 0LL);
            InterlockedExchange(
                &g_KswordArkBugcheckControlLastStatus,
                STATUS_SUCCESS);
            InterlockedExchange(
                &g_KswordArkBugcheckControlLifecycle,
                KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED);
            InterlockedExchange(
                &g_KswordArkBugcheckControlProtocolStatus,
                KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_OK);
            ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);
            return;
        }
        ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);
        installStatus = STATUS_CANCELLED;
    }

    // 初始化失败和取消路径统一清理由当前工作项创建的全部回调与矩形。
    KswordARKBugcheckUninitialize();
    ExAcquireFastMutex(&g_KswordArkBugcheckControlLock);
    InterlockedExchange64(&g_KswordArkBugcheckControlDeadline, 0LL);
    InterlockedExchange(
        &g_KswordArkBugcheckControlLastStatus,
        (LONG)installStatus);
    // 卸载方在 Flush 返回后完成最终状态复位；普通失败允许后续重新安装。
    InterlockedExchange(
        &g_KswordArkBugcheckControlLifecycle,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE);
    InterlockedExchange(
        &g_KswordArkBugcheckControlProtocolStatus,
        KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_PREPARATION_FAILED);
    ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);
}

NTSTATUS
KswordARKBugcheckControlInitialize(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ WDFDEVICE ControlDevice
    )
{
#if !KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(ControlDevice);
    return STATUS_NOT_SUPPORTED;
#else
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG workItemConfig;
    NTSTATUS status;

    // DriverEntry 只调用一次；拒绝无效对象，避免后续安装动作持有空设备指针。
    if (DriverObject == NULL || ControlDevice == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    // 工作项以控制设备为父对象；卸载路径显式 Flush 后再允许设备和驱动映像消失。
    ExInitializeFastMutex(&g_KswordArkBugcheckControlLock);
    WDF_WORKITEM_CONFIG_INIT(
        &workItemConfig,
        KswordARKBugcheckControlInstallWorker);
    // 控制器使用自己的 FAST_MUTEX；工作项不能继承设备级自动串行化并反向阻塞 IOCTL。
    workItemConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = ControlDevice;
    // WDFWORKITEM 回调天然运行在 PASSIVE_LEVEL；该对象类型不允许在对象属性中
    // 自行指定 ExecutionLevel/SynchronizationScope，只能保留 InheritFromParent。
    // 显式写入 Passive/None 会让 WdfWorkItemCreate 返回 WDF 属性无效状态，随后
    // 控制器一直处于 not-ready，并被 R3 误认为驱动没有安装能力。
    status = WdfWorkItemCreate(
        &workItemConfig,
        &attributes,
        &g_KswordArkBugcheckControlWorkItem);
    if (!NT_SUCCESS(status)) {
        g_KswordArkBugcheckControlWorkItem = WDF_NO_HANDLE;
        InterlockedExchange(
            &g_KswordArkBugcheckControlLastStatus,
            (LONG)status);
        InterlockedExchange(
            &g_KswordArkBugcheckControlProtocolStatus,
            KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_PREPARATION_FAILED);
        return status;
    }

    KswordARKBugcheckTrackingInitialize(); // 控制器 ready 发布前建立初始关闭的通知排空同步对象。
    g_KswordArkBugcheckControlDriverObject = DriverObject;
    // 每次驱动生命周期默认原诊断页；R3 的安装或切换请求显式恢复保存的模式。
    InterlockedExchange(
        &g_KswordArkBugcheckControlRenderMode,
        KSWORD_ARK_BUGCHECK_RENDER_MODE_DIAGNOSTIC);
    g_KswordArkBugcheckControlDevice = ControlDevice;
    InterlockedExchange(
        &g_KswordArkBugcheckControlLifecycle,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE);
    InterlockedExchange(
        &g_KswordArkBugcheckControlProtocolStatus,
        KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_INACTIVE);
    InterlockedExchange(
        &g_KswordArkBugcheckControlLastStatus,
        STATUS_SUCCESS);
    InterlockedExchange(
        &g_KswordArkBugcheckControlCancelRequested,
        0L);
    InterlockedExchange64(&g_KswordArkBugcheckControlDeadline, 0LL);
    InterlockedExchange(&g_KswordArkBugcheckControlReady, 1L);
    return STATUS_SUCCESS;
#endif
}

VOID
KswordARKBugcheckControlUninitialize(
    VOID
    )
{
#if !KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED
    return;
#else
    WDFWORKITEM workItem;
    LONG lifecycle;

    // 先撤销 ready 并发布取消，新的配置 IOCTL 不会再排队；控制锁只用于和排队瞬间交接。
    if (InterlockedExchange(&g_KswordArkBugcheckControlReady, 0L) == 0L) {
        return;
    }
    InterlockedExchange(&g_KswordArkBugcheckControlCancelRequested, 1L);
    ExAcquireFastMutex(&g_KswordArkBugcheckControlLock);
    workItem = g_KswordArkBugcheckControlWorkItem;
    ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);

    // Flush 只等待已经排队的工作项；工作项会在扫描块和每次私有位图解析之间响应取消。
    if (workItem != WDF_NO_HANDLE) {
        WdfWorkItemFlush(workItem);
    }

    ExAcquireFastMutex(&g_KswordArkBugcheckControlLock);
    lifecycle = InterlockedCompareExchange(
        &g_KswordArkBugcheckControlLifecycle,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE);
    if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED) {
        InterlockedExchange(
            &g_KswordArkBugcheckControlLifecycle,
            KSWORD_ARK_BUGCHECK_CONTROL_UNINSTALLING);
    }
    ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);

    // 已经撤销 ready 且工作项已排空，资源销毁不需要占用控制锁。
    if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED) {
        KswordARKBugcheckUninitialize();
    }

    ExAcquireFastMutex(&g_KswordArkBugcheckControlLock);
    g_KswordArkBugcheckControlDriverObject = NULL;
    g_KswordArkBugcheckControlDevice = WDF_NO_HANDLE;
    g_KswordArkBugcheckControlWorkItem = WDF_NO_HANDLE;
    InterlockedExchange64(&g_KswordArkBugcheckControlDeadline, 0LL);
    InterlockedExchange(
        &g_KswordArkBugcheckControlLifecycle,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE);
    InterlockedExchange(
        &g_KswordArkBugcheckControlProtocolStatus,
        KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_INACTIVE);
    ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);

    if (workItem != WDF_NO_HANDLE) {
        WdfObjectDelete(workItem);
    }
#endif
}

NTSTATUS
KswordARKBugcheckControlConfigure(
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS_REQUEST* Request,
    _Out_ KSWORD_ARK_BUGCHECK_DIAGNOSTICS_RESPONSE* Response
    )
{
#if !KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ENABLED
    UNREFERENCED_PARAMETER(Request);
    if (Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KswordARKBugcheckControlFillResponse(
        Response,
        KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_UNSUPPORTED,
        STATUS_NOT_SUPPORTED);
    return STATUS_SUCCESS;
#else
    NTSTATUS lastStatus;
    ULONG protocolStatus;
    LONG lifecycle;

    if (Response == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // Handler 已完成 WDF 缓冲区长度检查，这里继续严格校验版本、动作和保留位。
    if (Request == NULL ||
        Request->size != sizeof(*Request) ||
        Request->version != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_PROTOCOL_VERSION ||
        Request->flags != 0UL ||
        Request->renderMode > KSWORD_ARK_BUGCHECK_RENDER_MODE_LINUX_QR ||
        (Request->action == KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_QUERY &&
         Request->renderMode != 0UL) ||
        Request->reserved1 != 0UL ||
        (Request->action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_QUERY &&
         Request->action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_INSTALL &&
         Request->action != KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_SET_RENDER_MODE)) {
        KswordARKBugcheckControlFillResponse(
            Response,
            KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_INVALID_REQUEST,
            STATUS_INVALID_PARAMETER);
        return STATUS_SUCCESS;
    }

    if (InterlockedCompareExchange(
            &g_KswordArkBugcheckControlReady,
            0L,
            0L) == 0L) {
        protocolStatus = (ULONG)InterlockedCompareExchange(
            &g_KswordArkBugcheckControlProtocolStatus,
            0L,
            0L);
        lastStatus = (NTSTATUS)InterlockedCompareExchange(
            &g_KswordArkBugcheckControlLastStatus,
            0L,
            0L);
        if (protocolStatus == KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_INACTIVE) {
            protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_PREPARATION_FAILED;
            lastStatus = STATUS_DEVICE_NOT_READY;
        }
        KswordARKBugcheckControlFillResponse(
            Response,
            protocolStatus,
            lastStatus);
        return STATUS_SUCCESS;
    }

    ExAcquireFastMutex(&g_KswordArkBugcheckControlLock);
    lifecycle = InterlockedCompareExchange(
        &g_KswordArkBugcheckControlLifecycle,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE,
        KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE);
    protocolStatus = (ULONG)InterlockedCompareExchange(
        &g_KswordArkBugcheckControlProtocolStatus,
        0L,
        0L);
    lastStatus = (NTSTATUS)InterlockedCompareExchange(
        &g_KswordArkBugcheckControlLastStatus,
        0L,
        0L);

    if (Request->action == KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_QUERY) {
        if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLING ||
            lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_UNINSTALLING) {
            protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY;
            lastStatus = STATUS_PENDING;
        } else if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED) {
            protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_OK;
            lastStatus = STATUS_SUCCESS;
        }
    } else if (Request->action ==
               KSWORD_ARK_BUGCHECK_DIAGNOSTICS_ACTION_SET_RENDER_MODE) {
        // 切换仅发布一个模式值；不安装资源、不注册回调，也不会触发 BugCheck。
        if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLING ||
            lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_UNINSTALLING) {
            protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY;
            lastStatus = STATUS_DEVICE_BUSY;
        } else {
            // 未安装时也保存期望布局，后续 INSTALL 必须再次明确给出其保存值。
            InterlockedExchange(
                &g_KswordArkBugcheckControlRenderMode,
                (LONG)Request->renderMode);
            protocolStatus = lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED
                ? KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_OK
                : KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_INACTIVE;
            lastStatus = STATUS_SUCCESS;
        }
    } else if (lifecycle == KSWORD_ARK_BUGCHECK_CONTROL_INSTALLED) {
        // 已安装时再次 INSTALL 只确认模式并回读当前资源状态，不重复注册回调。
        InterlockedExchange(
            &g_KswordArkBugcheckControlRenderMode,
            (LONG)Request->renderMode);
        protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_OK;
        lastStatus = STATUS_SUCCESS;
    } else if (lifecycle != KSWORD_ARK_BUGCHECK_CONTROL_INACTIVE) {
        protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY;
        lastStatus = STATUS_DEVICE_BUSY;
    } else if (g_KswordArkBugcheckControlDriverObject == NULL ||
               g_KswordArkBugcheckControlDevice == WDF_NO_HANDLE ||
               g_KswordArkBugcheckControlWorkItem == WDF_NO_HANDLE) {
        protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_UNSUPPORTED;
        lastStatus = STATUS_DEVICE_NOT_READY;
    } else {
        ULONGLONG deadline;

        // 工作项执行完整初始化前先发布选定布局，诊断结构清零不会影响该选择。
        InterlockedExchange(
            &g_KswordArkBugcheckControlRenderMode,
            (LONG)Request->renderMode);

        // IOCTL 只负责原子发布状态并排队。耗时准备不持有控制锁，也不占用 R3 请求。
        deadline = KeQueryInterruptTime() +
            KSWORD_ARK_BUGCHECK_INSTALL_TIMEOUT_100NS;
        InterlockedExchange(
            &g_KswordArkBugcheckControlCancelRequested,
            0L);
        InterlockedExchange64(
            &g_KswordArkBugcheckControlDeadline,
            (LONG64)deadline);
        InterlockedExchange(
            &g_KswordArkBugcheckControlLastStatus,
            STATUS_PENDING);
        InterlockedExchange(
            &g_KswordArkBugcheckControlProtocolStatus,
            KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY);
        InterlockedExchange(
            &g_KswordArkBugcheckControlLifecycle,
            KSWORD_ARK_BUGCHECK_CONTROL_INSTALLING);
        WdfWorkItemEnqueue(g_KswordArkBugcheckControlWorkItem);
        protocolStatus = KSWORD_ARK_BUGCHECK_DIAGNOSTICS_STATUS_BUSY;
        lastStatus = STATUS_PENDING;
    }

    KswordARKBugcheckControlFillResponse(
        Response,
        protocolStatus,
        lastStatus);
    ExReleaseFastMutex(&g_KswordArkBugcheckControlLock);
    return STATUS_SUCCESS;
#endif
}
