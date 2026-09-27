/*++

Module Name:

    clipboard_policy_runtime.c

Abstract:

    剪贴板访问策略的存储与查询运行时。驱动本身不判定"这次调用要不要拦截"——
    真正的拦截发生在被注入目标进程的用户态 Agent DLL 里；这里只负责把 R3
    下发的一整张规则表原样存起来，并在被查询时原样吐回去。

Environment:

    Kernel-mode Driver Framework

--*/

#include "clipboard_policy_internal.h"

// 全局单例发布锁：只在 Initialize/Uninitialize 时短暂持有，
// 与运行期高频的 ConfigLock 分开，避免互相排队。
static EX_PUSH_LOCK g_KswordArkClipboardPolicyPublishLock;
// 全局单例状态指针；未初始化或已卸载时为 NULL。
static KSWORD_ARK_CLIPBOARD_POLICY_STATE* g_KswordArkClipboardPolicyState = NULL;

// KswordArkClipboardPolicyGetState：对外暴露的只读访问器，
// 供 clipboard_policy_ioctl.c 取得当前状态指针。
KSWORD_ARK_CLIPBOARD_POLICY_STATE*
KswordArkClipboardPolicyGetState(
    VOID
    )
{
    // 直接返回裸指针：调用方在拿到非 NULL 值后应尽快用完，
    // 不跨越可能触发 Uninitialize 的调用边界长期持有。
    return g_KswordArkClipboardPolicyState;
}

// KswordArkClipboardPolicyLogFormat：格式化后追加到 R3 日志通道；
// State 或 Device 未就绪时直接返回，不影响调用方。
VOID
KswordArkClipboardPolicyLogFormat(
    _In_ KSWORD_ARK_CLIPBOARD_POLICY_STATE* State,
    _In_z_ PCSTR LevelText,
    _In_z_ _Printf_format_string_ PCSTR FormatText,
    ...
    )
{
    // logBuffer：格式化后的单条日志文本，容量与其它模块统一取
    // KSWORD_ARK_LOG_ENTRY_MAX_BYTES，避免各模块各定义一个上限。
    CHAR logBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    // argumentList：可变参数游标，配合 va_start/va_end 使用。
    va_list argumentList;

    // 状态或设备任一为空都意味着日志通道不可用，直接放弃这条日志。
    if (State == NULL || State->Device == WDF_NO_HANDLE) {
        return;
    }

    // 展开可变参数并安全格式化到固定缓冲区，Cb 后缀函数保证不会溢出。
    va_start(argumentList, FormatText);
    if (NT_SUCCESS(RtlStringCbVPrintfA(logBuffer, sizeof(logBuffer), FormatText, argumentList))) {
        // 格式化成功才真正入队；失败大概率是调用方传了非法格式串，不值得再报一次错。
        (VOID)KswordARKDriverEnqueueLogFrame(State->Device, LevelText, logBuffer);
    }
    va_end(argumentList);
}

NTSTATUS
KswordARKClipboardPolicyInitialize(
    _In_ WDFDEVICE Device
    )
/*++

Routine Description:

    分配并发布本模块的全局单例状态。重复调用是安全的空操作（返回成功但不重新
    分配），与 ProcessProtect 的幂等语义保持一致。

--*/
{
    // state：本次要发布的新状态指针，分配失败时保持 NULL。
    KSWORD_ARK_CLIPBOARD_POLICY_STATE* state = NULL;

    // Device 为空说明控制设备还没建好，调用方传错了顺序。
    if (Device == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    // 独占持锁检查+发布，防止两次并发 Initialize 各分配一份、后一次覆盖前一次造成泄漏。
    KswordARKAcquirePushLockExclusive(&g_KswordArkClipboardPolicyPublishLock);
    if (g_KswordArkClipboardPolicyState != NULL) {
        // 已经发布过：直接成功返回，不重复分配。
        KswordARKReleasePushLockExclusive(&g_KswordArkClipboardPolicyPublishLock);
        return STATUS_SUCCESS;
    }

    // 整块状态一次性非分页分配，规则表是内嵌定长数组，跟着一起分配。
    state = (KSWORD_ARK_CLIPBOARD_POLICY_STATE*)KswordArkAllocateNonPaged(
        sizeof(KSWORD_ARK_CLIPBOARD_POLICY_STATE),
        KSWORD_ARK_CLIPBOARD_POLICY_TAG_STATE);
    if (state == NULL) {
        // 分配失败：本模块能力关闭，但不影响驱动其余功能加载。
        KswordARKReleasePushLockExclusive(&g_KswordArkClipboardPolicyPublishLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // 全零初始化，确保规则表、计数、时间戳都是确定的初始值。
    RtlZeroMemory(state, sizeof(*state));
    // 记下控制设备句柄，供后续日志使用。
    state->Device = Device;
    // 初始化规则表的读写锁。
    ExInitializePushLock(&state->ConfigLock);

    // 发布到全局指针，之后 GetState() 才能拿到非 NULL 值。
    g_KswordArkClipboardPolicyState = state;
    KswordARKReleasePushLockExclusive(&g_KswordArkClipboardPolicyPublishLock);
    return STATUS_SUCCESS;
}

VOID
KswordARKClipboardPolicyUninitialize(
    VOID
    )
/*++

Routine Description:

    撤销全局发布并释放状态内存。本模块没有注册任何内核回调，不存在"回调还在
    执行、状态已经释放"的竞态，因此不需要像 ProcessProtect 那样等待巡检线程退出。

--*/
{
    // state：撤销发布后本地持有的指针，用于稍后释放。
    KSWORD_ARK_CLIPBOARD_POLICY_STATE* state = NULL;

    // 独占持锁把全局指针置空，再在锁外释放内存，缩短持锁时间。
    KswordARKAcquirePushLockExclusive(&g_KswordArkClipboardPolicyPublishLock);
    state = g_KswordArkClipboardPolicyState;
    g_KswordArkClipboardPolicyState = NULL;
    KswordARKReleasePushLockExclusive(&g_KswordArkClipboardPolicyPublishLock);

    if (state != NULL) {
        // 释放整块状态，池标签必须与分配时完全一致。
        ExFreePoolWithTag(state, KSWORD_ARK_CLIPBOARD_POLICY_TAG_STATE);
    }
}

static NTSTATUS
KswordArkClipboardPolicyValidateAction(
    _In_ ULONG ActionValue
    )
/*++

Routine Description:

    校验单个方向（读/写/枚举）的动作取值是否是协议里定义的三个值之一。

--*/
{
    // 只接受协议里明确定义的三个动作值，其余一律视为格式错误。
    switch (ActionValue) {
    case KSWORD_ARK_CLIPBOARD_POLICY_ACTION_ALLOW:
    case KSWORD_ARK_CLIPBOARD_POLICY_ACTION_BLOCK:
    case KSWORD_ARK_CLIPBOARD_POLICY_ACTION_LOG_ONLY:
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS
KswordArkClipboardPolicyValidateRule(
    _In_ const KSWORD_ARK_CLIPBOARD_POLICY_RULE* Rule
    )
/*++

Routine Description:

    校验一条规则的字段是否自洽。未启用的规则视为 R3 的草稿行，只做长度裁剪
    （由调用方在这之前完成），不在这里做语义校验，允许 R3 暂存不完整的草稿。

--*/
{
    // 未启用的规则允许任意占位内容，直接放行。
    if ((Rule->flags & KSWORD_ARK_CLIPBOARD_POLICY_RULE_FLAG_ENABLED) == 0UL) {
        return STATUS_SUCCESS;
    }

    // 校验目标匹配方式与配套字段是否自洽。
    switch (Rule->targetKind) {
    case KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_PID:
        // PID 匹配要求进程号非零。
        if (Rule->targetProcessId == 0UL) {
            return STATUS_INVALID_PARAMETER;
        }
        break;
    case KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_IMAGE_NAME:
    case KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_IMAGE_PATH:
        // 映像名/路径匹配要求字符串非空。
        if (Rule->targetImage[0] == L'\0') {
            return STATUS_INVALID_PARAMETER;
        }
        break;
    case KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_ALL:
        // 全局规则不依赖 targetProcessId/targetImage，本身即视为合法。
        break;
    default:
        // 未知匹配方式一律拒绝，不允许静默当成 NONE 处理。
        return STATUS_INVALID_PARAMETER;
    }

    // 三个方向的动作取值都必须合法，任一非法就整条规则拒绝。
    if (!NT_SUCCESS(KswordArkClipboardPolicyValidateAction(Rule->readAction)) ||
        !NT_SUCCESS(KswordArkClipboardPolicyValidateAction(Rule->writeAction)) ||
        !NT_SUCCESS(KswordArkClipboardPolicyValidateAction(Rule->enumAction))) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKClipboardPolicyIoctlSetConfig(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
/*++

Routine Description:

    校验并整体替换一份剪贴板策略配置。配置是一次性全量替换：请求里没写到的
    规则槽位一律清空，不做增量合并，避免 R3/R0 两侧规则表漂移。

--*/
{
    // state：全局单例状态，NULL 表示 Initialize 还没跑或已经被卸载。
    KSWORD_ARK_CLIPBOARD_POLICY_STATE* state = KswordArkClipboardPolicyGetState();
    // requestPacket：指向输入缓冲区的只读视图，实际内存仍属于 WDF 管理。
    const KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST* requestPacket = NULL;
    // inputBuffer/inputLength：WdfRequestRetrieveInputBuffer 的输出参数。
    PVOID inputBuffer = NULL;
    size_t inputLength = 0U;
    // nowUtc：本次 Set 成功时刻的系统时间，写入状态供 Query 回显。
    LARGE_INTEGER nowUtc = { 0 };
    // appliedConfigVersion：本次自增后的配置版本号，仅用于成功日志打印。
    ULONG64 appliedConfigVersion = 0ULL;
    // entryIndex：规则表遍历游标。
    ULONG entryIndex = 0UL;
    // status：贯穿整个函数的状态码，逐步覆盖。
    NTSTATUS status = STATUS_SUCCESS;

    // 出参先清零，任何一条早退路径都不会让调用方看到脏值。
    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    // 模块尚未初始化时直接拒绝，不尝试访问空指针。
    if (state == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    // 输入长度小于定长包体，说明调用方版本不对或参数拼错。
    if (InputBufferLength < sizeof(KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // 从 WDF 请求对象取出输入缓冲区，同时再校验一次实际可读长度。
    status = WdfRequestRetrieveInputBuffer(
        Request,
        sizeof(KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST),
        &inputBuffer,
        &inputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (inputLength < sizeof(KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // 把缓冲区视图转成协议结构体指针，校验包头自报的 size/version/ruleCount。
    requestPacket = (const KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST*)inputBuffer;
    if (requestPacket->size < sizeof(KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST) ||
        requestPacket->version != KSWORD_ARK_CLIPBOARD_POLICY_PROTOCOL_VERSION ||
        requestPacket->ruleCount > KSWORD_ARK_CLIPBOARD_POLICY_MAX_RULES) {
        return STATUS_INVALID_PARAMETER;
    }

    // 进入独占区：整块替换规则表期间不允许任何并发 Query 看到半张表。
    KswordARKAcquirePushLockExclusive(&state->ConfigLock);

    // 先整体清零，未出现在本次请求里的槽位自然变成"无规则"。
    RtlZeroMemory(state->Rules, sizeof(state->Rules));

    // 逐条拷贝、裁剪字符串终止符、校验，任一条失败就整体中止后续拷贝。
    for (entryIndex = 0UL; entryIndex < requestPacket->ruleCount; ++entryIndex) {
        // targetRule：状态表里本条规则的落点。
        KSWORD_ARK_CLIPBOARD_POLICY_RULE* targetRule = &state->Rules[entryIndex];

        // 整体值拷贝，两个结构体定义完全一致，可以直接赋值。
        *targetRule = requestPacket->rules[entryIndex];
        // R3 送来的字符串未必带终止符，先强制裁剪，后续比较全部按 NUL 结尾处理。
        targetRule->targetImage[KSWORD_ARK_CLIPBOARD_POLICY_IMAGE_CHARS - 1U] = L'\0';
        targetRule->ruleName[KSWORD_ARK_CLIPBOARD_POLICY_NAME_CHARS - 1U] = L'\0';
        // targetKind 不是 PID 时强制把 targetProcessId 清零，防止调用方传混两种匹配方式。
        if (targetRule->targetKind != KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_PID) {
            targetRule->targetProcessId = 0UL;
        }

        // 校验这一条规则的字段是否自洽，失败就跳出循环，交给下面统一回退处理。
        status = KswordArkClipboardPolicyValidateRule(targetRule);
        if (!NT_SUCCESS(status)) {
            break;
        }
    }

    if (!NT_SUCCESS(status)) {
        // 校验失败时不保留半张表：整体回退到"无规则"，比留下残缺配置更安全，
        // 语义与 ProcessProtect 完全一致。
        RtlZeroMemory(state->Rules, sizeof(state->Rules));
        state->RuleCount = 0UL;
        KswordARKReleasePushLockExclusive(&state->ConfigLock);
        // 拒绝原因写进日志，方便定位是哪条规则字段不对。
        KswordArkClipboardPolicyLogFormat(
            state,
            "Warn",
            "Clipboard policy config rejected, status=0x%08lX.",
            (unsigned long)status);
        return status;
    }

    // 全部校验通过，正式提交：记录时间戳、条数、全局标志，并自增版本号。
    KeQuerySystemTimePrecise(&nowUtc);
    state->GlobalFlags = requestPacket->globalFlags;
    state->RuleCount = requestPacket->ruleCount;
    state->ConfigVersion += 1ULL;
    state->AppliedAtUtc100ns = nowUtc;
    // 缓存自增后的版本号用于日志打印，避免日志格式化时还要重新持锁读取。
    appliedConfigVersion = state->ConfigVersion;

    // 提交完成，释放独占锁。
    KswordARKReleasePushLockExclusive(&state->ConfigLock);

    // 成功日志：记录总开关状态、规则条数与新版本号，便于排查"UI 点了没生效"。
    KswordArkClipboardPolicyLogFormat(
        state,
        "Info",
        "Clipboard policy config applied, enabled=%lu, rules=%lu, version=%I64u.",
        (unsigned long)((requestPacket->globalFlags & KSWORD_ARK_CLIPBOARD_POLICY_FLAG_ENABLED) != 0UL ? 1UL : 0UL),
        (unsigned long)requestPacket->ruleCount,
        (unsigned long long)appliedConfigVersion);

    // 按惯例把消费掉的输入字节数回填给调用方。
    *CompleteBytesOut = sizeof(KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKClipboardPolicyIoctlQueryState(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    )
/*++

Routine Description:

    只读回显当前剪贴板策略配置，供 R3 UI 重建规则表格或 CLI 核对。

--*/
{
    // state：全局单例状态。
    KSWORD_ARK_CLIPBOARD_POLICY_STATE* state = KswordArkClipboardPolicyGetState();
    // responsePacket：指向输出缓冲区的可写视图。
    KSWORD_ARK_CLIPBOARD_POLICY_STATE_RESPONSE* responsePacket = NULL;
    // outputLength：WdfRequestRetrieveOutputBuffer 实际给出的缓冲区长度。
    size_t outputLength = 0U;
    // status：贯穿函数的状态码。
    NTSTATUS status = STATUS_SUCCESS;

    // 出参先清零。
    if (CompleteBytesOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *CompleteBytesOut = 0U;

    // 模块尚未初始化时直接拒绝。
    if (state == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    // 输出缓冲区必须能装下整个定长响应包。
    if (OutputBufferLength < sizeof(KSWORD_ARK_CLIPBOARD_POLICY_STATE_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // 取出输出缓冲区指针。
    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(KSWORD_ARK_CLIPBOARD_POLICY_STATE_RESPONSE),
        (PVOID*)&responsePacket,
        &outputLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (outputLength < sizeof(KSWORD_ARK_CLIPBOARD_POLICY_STATE_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // 先整体清零，避免内核栈/池残留数据泄露给 R3。
    RtlZeroMemory(responsePacket, sizeof(*responsePacket));
    // 填包头：size/version 固定为当前协议自身的值，供 R3 做双重校验。
    responsePacket->size = sizeof(*responsePacket);
    responsePacket->version = KSWORD_ARK_CLIPBOARD_POLICY_PROTOCOL_VERSION;

    // 共享持锁读取规则表，读取期间可能有其它线程在排队等 Set 的独占锁，但读者互不阻塞。
    KswordARKAcquirePushLockShared(&state->ConfigLock);
    responsePacket->globalFlags = state->GlobalFlags;
    responsePacket->ruleCount = state->RuleCount;
    responsePacket->configVersion = (unsigned long long)state->ConfigVersion;
    responsePacket->appliedAtUtc100ns = (unsigned long long)state->AppliedAtUtc100ns.QuadPart;
    // 规则表整块拷贝，两侧数组长度完全一致。
    RtlCopyMemory(responsePacket->rules, state->Rules, sizeof(responsePacket->rules));
    KswordARKReleasePushLockShared(&state->ConfigLock);

    // 回填实际写入的字节数，等于整个响应包大小。
    *CompleteBytesOut = sizeof(*responsePacket);
    return STATUS_SUCCESS;
}
