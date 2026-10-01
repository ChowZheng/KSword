/* 扩展回调公开 API 注销。注册参数来自唯一匹配的当前枚举行，不解引用 R3 裸地址。 */
#include "callback_internal.h" // 引入共享协议与枚举验证接口。
#include "callback_extended_internal.h" // 使用有界导出解析封装。
#include "callback_external_minifilter.h" // 复用 Filter Manager 整体卸载后端。

typedef VOID (NTAPI* KSW_COALESCING_UNREGISTER)(PVOID); // 未在 WDK 声明的导出 ABI：参数为注册句柄。
typedef VOID (NTAPI* KSW_PRIORITY_UNREGISTER)(PDRIVER_OBJECT); // 未在 WDK 声明的导出 ABI：参数为驱动对象。
typedef NTSTATUS (NTAPI* KSW_PROCESS_NOTIFY_EX2)(PSCREATEPROCESSNOTIFYTYPE, PVOID, BOOLEAN); // 正规 Ex2 API 的动态导出签名。

NTSTATUS KswordArkCallbackRemoveProcessNotify( // 按当前注册子类型选择配对 API，旧地址入口允许有限回退。
    _In_ ULONG64 CallbackAddress, // 已验证的回调函数地址。
    _In_ ULONG RegistrationType, // 当前枚举子类型；UNKNOWN 用于旧地址入口。
    _Out_ PCWSTR* ApiOut // 返回实际尝试的 API 名称。
    ) // 结束参数声明。
{ // 开始进程注销分发。
    NTSTATUS status = STATUS_NOT_SUPPORTED; // 未知能力默认不支持。
    KSW_PROCESS_NOTIFY_EX2 routine = NULL; // Ex2 在旧系统可能不存在。
    if (RegistrationType == KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_LEGACY) { // 传统注册只走配对 API。
        *ApiOut = L"PsSetCreateProcessNotifyRoutine"; // 保留实际 API 名称。
        return PsSetCreateProcessNotifyRoutine((PCREATE_PROCESS_NOTIFY_ROUTINE)(ULONG_PTR)CallbackAddress, TRUE); // 返回原始状态。
    } // 结束传统分支。
    if (RegistrationType == KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_EX ||
        RegistrationType == KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_UNKNOWN) { // Ex 或旧地址入口先尝试 Ex。
        *ApiOut = L"PsSetCreateProcessNotifyRoutineEx"; // 记录当前 API。
        status = PsSetCreateProcessNotifyRoutineEx((PCREATE_PROCESS_NOTIFY_ROUTINE_EX)(ULONG_PTR)CallbackAddress, TRUE); // 注销指定函数。
        if (RegistrationType != KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_UNKNOWN ||
            (status != STATUS_INVALID_PARAMETER && status != STATUS_PROCEDURE_NOT_FOUND)) { // 明确子类型或实际 API 错误不继续猜测。
            return status; // 保留配对 API 原始结果。
        } // 结束回退条件。
        *ApiOut = L"PsSetCreateProcessNotifyRoutine"; // 旧地址入口尝试传统注册。
        status = PsSetCreateProcessNotifyRoutine((PCREATE_PROCESS_NOTIFY_ROUTINE)(ULONG_PTR)CallbackAddress, TRUE); // 沿用旧兼容顺序。
        if (status != STATUS_INVALID_PARAMETER && status != STATUS_PROCEDURE_NOT_FOUND) { // 仅未匹配允许继续 Ex2。
            return status; // 成功或其它错误立即返回。
        } // 结束传统回退结果。
    } // 结束 Ex/未知分支。
    if (RegistrationType != KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_PROCESS_EX2 &&
        RegistrationType != KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_UNKNOWN) { // 不猜测未识别的新子类型。
        return STATUS_NOT_SUPPORTED; // 拒绝错误签名。
    } // 结束子类型检查。
    *ApiOut = L"PsSetCreateProcessNotifyRoutineEx2"; // 记录明确缺失或实际调用的 API。
    routine = (KSW_PROCESS_NOTIFY_EX2)KswordArkCallbackExtendedGetSystemRoutine(*ApiOut); // 兼容不导出 Ex2 的旧内核。
    return routine == NULL ? STATUS_PROCEDURE_NOT_FOUND :
        routine(PsCreateProcessNotifySubsystems, (PVOID)(ULONG_PTR)CallbackAddress, TRUE); // 正确 NotifyType 与 Remove=TRUE。
} // 结束进程注销分发。

static VOID KswordArkExtendedRemoveMessage( // 写入 API 名称、阶段和状态码，保留具体失败原因。
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE* Response, // 输出语义回执。
    _In_z_ PCWSTR Api, // 实际注销 API 或检查阶段。
    _In_z_ PCWSTR Reason, // 具体阶段说明。
    _In_ NTSTATUS Status // 阶段状态码。
    ) // 结束诊断参数。
{ // 开始诊断写入。
    (VOID)RtlStringCbPrintfW(Response->message, sizeof(Response->message), // 有界写入共享消息。
        L"%ws: %ws (NTSTATUS=0x%08lX).", Api, Reason, (ULONG)Status); // 保留 API 与精确状态。
} // 结束诊断写入。

NTSTATUS KswordArkCallbackRemoveExtendedPublic( // 注销已重新验证的扩展类别。
    _In_ const KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_REQUEST* RequestPacket, // 输入完整行身份。
    _Inout_ KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_EX_RESPONSE* ResponsePacket // 输出注销与复核结果。
    ) // 结束注销参数。
{ // 开始扩展注销。
    NTSTATUS status = STATUS_SUCCESS; // 保存 API 或重枚举状态。
    BOOLEAN present = FALSE; // 保存精确目标行是否存在。
    ULONG fields = 0UL; // 保存重新枚举的字段能力。
    ULONG subtype = 0UL; // 保存重新枚举的注册子类型。
    ULONG64 registration = 0ULL; // 保存当前注册记录或句柄。
    ULONG64 context = 0ULL; // 保存文件系统注册所需的驱动对象。
    PCWSTR api = L"Callback row revalidation"; // 默认阶段名称。

    if (RequestPacket == NULL || ResponsePacket == NULL) { // 检查内部调用参数。
        return STATUS_INVALID_PARAMETER; // 拒绝空包。
    } // 结束空包检查。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || RequestPacket->identityHash == 0ULL ||
        (RequestPacket->flags & KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_FLAG_REQUIRE_REVALIDATION) == 0UL) {
        status = STATUS_INVALID_PARAMETER; // 扩展注销必须由 PASSIVE_LEVEL 完整行请求触发。
        KswordArkExtendedRemoveMessage(ResponsePacket, api, L"Complete row identity and PASSIVE_LEVEL are required", status); // 说明拒绝原因。
        return status; // 返回参数错误。
    } // 结束请求检查。
    status = KswordArkCallbackEnumRevalidateRemoveRequest(RequestPacket, FALSE, TRUE, // 只复核目标身份，不要求无关行代次一致。
        &present, &fields, &registration, NULL, &context, &subtype); // 从当前枚举获得 API 参数。
    ResponsePacket->revalidationStatus = status; // 记录前置验证结果。
    if (!NT_SUCCESS(status) || !present) { // 不使用已经消失、歧义或无法枚举的记录。
        status = NT_SUCCESS(status) ? STATUS_NOT_FOUND : status; // 保留枚举错误或目标已消失状态。
        ResponsePacket->revalidationStatus = status; // 更新目标缺失状态。
        KswordArkExtendedRemoveMessage(ResponsePacket, api,
            status == STATUS_NOT_FOUND ? L"Selected registration is no longer present; no unregister API was called"
                : status == STATUS_RETRY ? L"Registration snapshot changed during validation; refresh and retry"
                    : L"Current registration enumeration failed or yielded ambiguous matches; no unregister API was called", status); // 区分目标消失与查询失败。
        return status; // 返回验证失败。
    } // 结束目标验证。
    if ((fields & KSWORD_ARK_CALLBACK_ENUM_FIELD_REMOVABLE_CANDIDATE) == 0UL) { // 禁止把纯展示节点当成 API 句柄。
        status = STATUS_NOT_SUPPORTED; // 当前行未具备参数语义。
        KswordArkExtendedRemoveMessage(ResponsePacket, api, // 明确缺失的 API 参数。
            RequestPacket->callbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_POWER_SETTING
                ? L"PoUnregisterPowerSettingCallback requires the handle returned by PoRegisterPowerSettingCallback; the list node is unverified"
                : RequestPacket->callbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY
                    ? L"CmUnRegisterCallback requires a Cookie from a list calibrated against a live Ksword registration"
                : RequestPacket->callbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PLUG_PLAY
                    ? L"IoUnregisterPlugPlayNotificationEx requires NotificationEntry returned by IoRegisterPlugPlayNotification; the list node is unverified"
                    : L"Enumeration has no reliable unregister arguments for this record", status); // 不把节点误当成注册句柄。
        return status; // 返回不可注销状态。
    } // 结束参数语义检查。

    ResponsePacket->mappingFlags |= KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_ENUMERATED; // 标记目标重枚举成功。
    switch (RequestPacket->callbackClass) { // 按共享扩展类别调用匹配的公开 API。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PROCESS: // 子类型来自当前唯一匹配行，不能信任 R3 猜测。
        if (subtype == KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_UNKNOWN) { // 完整行入口必须恢复准确子类型。
            status = STATUS_NOT_SUPPORTED; // 未识别注册不盲试 API。
            break; // 不调用注销。
        } // 结束子类型检查。
        status = KswordArkCallbackRemoveProcessNotify(RequestPacket->callbackAddress, subtype, &api); // 配对 Ex2/Ex/传统 API。
        break; // 后置确认函数注册消失。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER: { // Pre/Post 行只允许整体卸载所属过滤器。
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_REQUEST filterRequest = { 0 }; // 转换到既有公开过滤器对象卸载入口。
        KSWORD_ARK_REMOVE_EXTERNAL_CALLBACK_RESPONSE filterResponse = { 0 }; // 保存过滤器名称等公开映射。
        ULONG64 filterObject = context != 0ULL ? context : registration; // 子行用所属对象，父行用注册对象。
        api = L"FltEnumerateFilters / FltUnloadFilter"; // 明确整体卸载 API。
        if (filterObject == 0ULL || filterObject != RequestPacket->registrationAddress) { // 必须匹配 EX 请求中的所属对象身份。
            status = STATUS_INVALID_PARAMETER; // 不把函数或操作表当成 FilterObject。
            break; // 拒绝错误对象映射。
        } // 结束对象检查。
        filterRequest.callbackClass = KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_MINIFILTER; // 指定公开过滤器卸载类别。
        filterRequest.callbackAddress = filterObject; // 后端再次从 FltEnumerateFilters 查找当前对象。
        status = KswordArkCallbackExternalMinifilterRemove(&filterRequest, &filterResponse); // 使用 FltUnloadFilter，保留卸载拒绝状态。
        KswordArkCallbackEnumCopyWide(ResponsePacket->serviceName, RTL_NUMBER_OF(ResponsePacket->serviceName), filterResponse.serviceName); // 返回真实过滤器名称。
        ResponsePacket->mappingFlags |= filterResponse.mappingFlags; // 保留公开对象映射结果。
        break; // 后置通过公开父行检查整个 FilterObject 消失。
    } // 结束 Minifilter 分支。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_REGISTRY: { // Cookie 是值，不是地址。
        LARGE_INTEGER cookie; // 与链节点分开的 API 参数。
        api = L"CmUnRegisterCallback"; // 保存实际调用名称。
        if ((fields & KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE) == 0UL || registration == 0ULL) { // 必须经过自身注册校准。
            status = STATUS_NOT_SUPPORTED; // 禁止把链节点当 Cookie。
            break; // 停止无 Cookie 的调用。
        } // 结束 Cookie 证据检查。
        cookie.QuadPart = (LONGLONG)registration; // 允许非指针、非对齐 Cookie。
        status = CmUnRegisterCallback(cookie); // 保留 API 原始状态。
        if (NT_SUCCESS(status)) { KswordArkCallbackRegistryRemoved(registration); } // 同步自身状态。
        break; // 后置确认原始节点消失。
    } // 结束 Registry 分支。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_BUGCHECK: // 经典 BugCheck 使用公开注册记录。
        api = L"KeDeregisterBugCheckCallback"; // 保存实际调用名称。
        status = KeDeregisterBugCheckCallback((PKBUGCHECK_CALLBACK_RECORD)(ULONG_PTR)registration)
            ? STATUS_SUCCESS : STATUS_NOT_FOUND; // BOOLEAN FALSE 表示记录未成功注销。
        break; // 完成经典 BugCheck 分发。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_BUGCHECK_REASON: // Reason 使用独立公开注册记录。
        api = L"KeDeregisterBugCheckReasonCallback"; // 保存实际调用名称。
        status = KeDeregisterBugCheckReasonCallback((PKBUGCHECK_REASON_CALLBACK_RECORD)(ULONG_PTR)registration)
            ? STATUS_SUCCESS : STATUS_NOT_FOUND; // 保留注销失败结果。
        break; // 完成 Reason 分发。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_FILE_SYSTEM: // FS 注销需要 DriverObject 与函数两个参数。
        api = L"IoUnregisterFsRegistrationChange"; // 保存实际调用名称。
        IoUnregisterFsRegistrationChange((PDRIVER_OBJECT)(ULONG_PTR)context,
            (PDRIVER_FS_NOTIFICATION)(ULONG_PTR)RequestPacket->callbackAddress); // 使用当前行重新获得的驱动对象。
        break; // VOID API 必须依赖后置枚举确认结果。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_LOGON_SESSION: // 普通登录会话终止通过函数注销。
        api = L"SeUnregisterLogonSessionTerminatedRoutine"; // 保存实际调用名称。
        if (subtype == KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_EX) { // Ex 注销同时匹配函数和原始 Context，Context 可以为空。
            api = L"SeUnregisterLogonSessionTerminatedRoutineEx"; // 保留实际调用名称。
            status = SeUnregisterLogonSessionTerminatedRoutineEx(
                (PSE_LOGON_SESSION_TERMINATED_ROUTINE_EX)(ULONG_PTR)RequestPacket->callbackAddress,
                (PVOID)(ULONG_PTR)context); // 使用当前枚举恢复的 Context。
            break; // 完成 Ex 分发。
        } // 结束 Ex 子类型。
        if (subtype != KSWORD_ARK_CALLBACK_REGISTRATION_TYPE_LOGON_LEGACY) { // 未知子类型不猜测签名。
            status = STATUS_NOT_SUPPORTED; // 缺少 Ex 注销参数语义。
            break; // 停止错误签名调用。
        } // 结束子类型检查。
        status = SeUnregisterLogonSessionTerminatedRoutine(
            (PSE_LOGON_SESSION_TERMINATED_ROUTINE)(ULONG_PTR)RequestPacket->callbackAddress); // 返回 API 原始状态。
        break; // 完成登录会话分发。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_SHUTDOWN: // Shutdown 使用当前 DeviceObject。
        api = L"IoUnregisterShutdownNotification"; // 保存实际调用名称。
        IoUnregisterShutdownNotification((PDEVICE_OBJECT)(ULONG_PTR)registration); // 取消设备的关机通知注册。
        break; // VOID API 通过后置枚举确认。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_NMI: // NMI 枚举已检查节点自句柄等于注册句柄。
        api = L"KeDeregisterNmiCallback"; // 保存实际调用名称。
        status = KeDeregisterNmiCallback((PVOID)(ULONG_PTR)registration); // 使用当前自句柄并保留 API 状态。
        break; // 完成 NMI 分发。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_IMAGE_VERIFICATION: // 镜像验证包装器维护自身计数，不能跳过。
        api = L"SeUnregisterImageVerificationCallback"; // 使用与注册 API 配对的导出。
        SeUnregisterImageVerificationCallback((PVOID)(ULONG_PTR)registration); // 包装器维护计数并调用 ExUnregisterCallback。
        break; // 后置复核对应 CallbackObject 注册块。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_GENERIC_KERNEL: // CallbackObject 注册块是 ExRegisterCallback 返回值。
        api = L"ExUnregisterCallback"; // 保存实际调用名称。
        ExUnregisterCallback((PVOID)(ULONG_PTR)registration); // 注销当前匹配的注册块。
        break; // VOID API 通过后置枚举确认。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_DEBUG_PRINT: // Debug Print 以函数和 Enable=FALSE 注销。
        api = L"DbgSetDebugPrintCallback"; // 保存实际 API 名称。
        status = DbgSetDebugPrintCallback((PDEBUG_PRINT_CALLBACK)(ULONG_PTR)RequestPacket->callbackAddress, FALSE); // 返回原始状态。
        break; // 完成 Debug Print 注销。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_COALESCING: // 合并通知以注册块自句柄注销。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PRIORITY: // Priority 以 DriverObject 注销。
        if (RequestPacket->callbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_COALESCING) { // 查询句柄类导出。
            KSW_COALESCING_UNREGISTER routine = (KSW_COALESCING_UNREGISTER)KswordArkCallbackExtendedGetSystemRoutine(L"PoUnregisterCoalescingCallback"); // 不引入不存在的静态导入。
            api = L"PoUnregisterCoalescingCallback"; // 保存实际名称。
            if (routine == NULL || (fields & KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE) == 0UL) { // 句柄证据与导出均必需。
                status = routine == NULL ? STATUS_PROCEDURE_NOT_FOUND : STATUS_NOT_SUPPORTED; // 指出缺失导出或句柄。
                break; // 不调用错误 ABI。
            } // 结束句柄检查。
            routine((PVOID)(ULONG_PTR)registration); // 使用重枚举得到的注册块基址。
        } else { // Priority 签名仅接收 DriverObject。
            KSW_PRIORITY_UNREGISTER routine = (KSW_PRIORITY_UNREGISTER)KswordArkCallbackExtendedGetSystemRoutine(L"IoUnregisterPriorityCallback"); // 按名称解析稳定导出。
            api = L"IoUnregisterPriorityCallback"; // 保存实际名称。
            if (routine == NULL || context == 0ULL) { // 防止把 EX 回调块或 slot 当成 DriverObject。
                status = routine == NULL ? STATUS_PROCEDURE_NOT_FOUND : STATUS_INVALID_PARAMETER; // 保留实际拒绝原因。
                break; // 结束错误请求。
            } // 结束 DriverObject 检查。
            routine((PDRIVER_OBJECT)(ULONG_PTR)context); // 使用已验证的 DriverObject。
        } // 结束动态 ABI 分发。
        break; // VOID API 通过后置枚举确认。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_POWER_SETTING: // 电源设置必须有真实注册句柄证据。
    case KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_PLUG_PLAY: // PnP 必须有真实 NotificationEntry 证据。
        if ((fields & KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE) == 0UL || registration == 0ULL) { // 不接受纯存储节点。
            status = STATUS_NOT_SUPPORTED; // 句柄证据不足。
            KswordArkExtendedRemoveMessage(ResponsePacket, api, L"A verified registration handle is required; storage nodes are not handles", status); // 指出缺失参数。
            return status; // 不执行错误句柄调用。
        } // 结束句柄检查。
        if (RequestPacket->callbackClass == KSWORD_ARK_EXTERNAL_CALLBACK_REMOVE_TYPE_POWER_SETTING) { // 分发电源设置注销。
            api = L"PoUnregisterPowerSettingCallback"; // 记录实际 API。
            status = PoUnregisterPowerSettingCallback((PVOID)(ULONG_PTR)registration); // 使用真实句柄并返回原始状态。
        } else { // 分发 PnP 注销。
            api = L"IoUnregisterPlugPlayNotificationEx"; // 记录有等待保证的 API。
            status = IoUnregisterPlugPlayNotificationEx((PVOID)(ULONG_PTR)registration); // 使用真实 NotificationEntry。
        } // 结束句柄类分发。
        break; // 完成句柄注销路径。
    default: // 不猜测未知类别的注销 ABI。
        status = STATUS_NOT_SUPPORTED; // 返回明确不支持。
        KswordArkExtendedRemoveMessage(ResponsePacket, api, L"No public unregister backend for this class", status); // 说明没有后端。
        return status; // 结束未知类别。
    } // 结束 API 分发。
    ResponsePacket->mappingFlags |= KSWORD_ARK_EXTERNAL_CALLBACK_MAPPING_FLAG_PUBLIC_API; // 标记确已进入公开 API 路径。
    if (!NT_SUCCESS(status)) { // API 失败时保留原始状态，不执行其它注销。
        KswordArkExtendedRemoveMessage(ResponsePacket, api,
            status == STATUS_PROCEDURE_NOT_FOUND ? L"Required kernel unregister export is unavailable"
                : status == STATUS_NOT_SUPPORTED ? L"Unregister arguments are not validated for this registration"
                    : L"Unregister API rejected the registration", status); // 区分未调用与 API 原始失败。
        return status; // 返回 API 失败。
    } // 结束 API 失败分支。
    present = FALSE; // 后置确认重新设置目标存在状态。
    status = KswordArkCallbackEnumRevalidateRemoveRequest(RequestPacket, FALSE, FALSE, // 后置检查注册仍存在，不依赖易变化的显示身份。
        &present, NULL, NULL, NULL, NULL, NULL); // 确认精确目标注册已消失。
    ResponsePacket->revalidationStatus = status; // 保存后置枚举状态。
    if (!NT_SUCCESS(status)) { // 注销之后枚举失败不能报告确认成功。
        KswordArkExtendedRemoveMessage(ResponsePacket, api, L"Unregister API completed, but confirmation enumeration failed", status); // VOID API 也不能假称已经成功。
        return status; // 返回复核错误。
    } // 结束后置枚举失败。
    if (present) { // VOID API 可能没有真正移除记录。
        ResponsePacket->revalidationStatus = STATUS_UNSUCCESSFUL; // 保留仍存在的证据。
        KswordArkExtendedRemoveMessage(ResponsePacket, api, L"API returned, but the selected registration is still present", STATUS_UNSUCCESSFUL); // 明确未确认移除。
        return STATUS_UNSUCCESSFUL; // 禁止假成功。
    } // 结束仍存在分支。
    KswordArkExtendedRemoveMessage(ResponsePacket, api, L"Unregistered and confirmed absent", STATUS_SUCCESS); // 给出 API 与确认结果。
    return STATUS_SUCCESS; // 返回已确认的注销成功。
} // 结束扩展注销。
