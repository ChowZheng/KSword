/* 已定位特殊注册节点的参数语义验证；只读快照，所有私有地址由统一读取器访问。 */
#pragma once // 每个编译单元只引入一次。

static BOOLEAN KswordArkSpecialDecodeIdentity( // 返回真实 API 参数与能力位。
    _In_ ULONG CallbackClass, _In_ ULONG64 Node, // 输入类别与已校验拓扑的节点。
    _Out_ ULONG64* Callback, _Out_ ULONG64* Context, // 输出函数与原始上下文或 DriverObject。
    _Out_ ULONG64* Registration, _Out_ ULONG* Fields // 输出真实注册句柄与有效性。
    ) // 结束参数。
{ // 开始语义解码。
    ULONG64 words[14]; // 有界读取最大 0x70 字节前缀。
    ULONG64 base = Node; // 默认链节点位于记录起点。
    SIZE_T bytes = sizeof(words); // 默认读取电源记录前缀。
    DRIVER_OBJECT driver; // 用公开结构验证 DriverObject。
    *Fields = 0UL; // 不满足结构证据时不发布能力。
    if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_COALESCING) { // Coalescing 的 Link 位于记录 +0x30。
        if (Node < 0x30ULL) { return FALSE; } // 避免减法回绕。
        base -= 0x30ULL; bytes = 0x48U; // 只读包含自句柄和 ExCallback 的前缀。
    } else if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_DEBUG_PRINT) { // Debug Print 的 Link 位于记录 +0x18。
        if (Node < 0x18ULL) { return FALSE; } // 避免减法回绕。
        base -= 0x18ULL; bytes = 0x28U; // 读取 Flags、Rundown、函数和 Link。
    } else if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_PRIORITY) { // Priority 使用 EX 回调块。
        bytes = 0x28U; // 覆盖自指针、实际函数和 DriverObject。
    } else if (CallbackClass != KSWORD_ARK_CALLBACK_ENUM_CLASS_POWER_SETTING &&
               CallbackClass != KSWORD_ARK_CALLBACK_ENUM_CLASS_PLUG_PLAY) { // 未知 ABI 保持只读。
        return FALSE; // 不猜测其它类别。
    } // 结束类别与前缀选择。
    RtlZeroMemory(words, sizeof(words)); // 防止短读使用残留。
    if (!KswordARKRuntimeReadMemory((PVOID)(ULONG_PTR)base, words, bytes)) { // 候选内核地址只用安全读取器。
        return FALSE; // 失败不发布注销参数。
    } // 结束前缀读取。
    *Registration = base; // API 句柄默认是注册记录起点。
    if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_POWER_SETTING) { // 验证 PSet 标签，不能只将链节点当句柄。
        if ((ULONG)words[2] != 0x74655350UL) { return FALSE; } // PoUnregisterPowerSettingCallback 同样要求该标签。
        *Callback = words[10]; *Context = words[11]; // V2 函数 +0x50、上下文 +0x58。
        *Fields = KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE; // 标签和函数验证后发布真实句柄。
    } else if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_PLUG_PLAY) { // 只接受公共 PnP 通知前缀。
        const ULONG category = (ULONG)words[2]; // EventCategory 位于 +0x10。
        if (category < 1UL || category > 3UL || ((words[7] >> 16U) & 0xFFULL) != 0ULL) { return FALSE; } // 排除非法类别和已经注销的记录。
        if (!KswordARKRuntimeReadMemory((PVOID)(ULONG_PTR)words[6], &driver, sizeof(driver)) ||
            driver.Type != IO_TYPE_DRIVER || driver.Size < sizeof(driver)) { return FALSE; } // DriverObject +0x30 必须具有公开对象形态。
        *Callback = words[4]; *Context = words[5]; // Callback +0x20、Context +0x28。
        *Fields = KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE; // Link 在起点，返回的 NotificationEntry 就是 base。
    } else if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_COALESCING) { // Link 不是注销句柄。
        if (words[2] != base || words[8] == 0ULL) { return FALSE; } // 自句柄 +0x10 和 ExCallback +0x40 同时验证。
        *Callback = words[3]; *Context = words[5]; // 实际用户函数 +0x18，跳过内部 dispatcher。
        *Fields = KSWORD_ARK_CALLBACK_ENUM_FIELD_HANDLE; // 注销函数要求 base。
    } else if (CallbackClass == KSWORD_ARK_CALLBACK_ENUM_CLASS_PRIORITY) { // Priority 注销要求 DriverObject。
        if (words[2] != base || !KswordARKRuntimeReadMemory((PVOID)(ULONG_PTR)words[4], &driver, sizeof(driver)) ||
            driver.Type != IO_TYPE_DRIVER || driver.Size < sizeof(driver)) { return FALSE; } // 自指针和公开 DriverObject 双重验证。
        *Callback = words[3]; *Context = words[4]; // 实际函数 +0x18 和 DriverObject +0x20。
        *Fields = KSWORD_ARK_CALLBACK_ENUM_FIELD_CONTEXT_ADDRESS; // Context 对该类别明确表示 DriverObject。
    } else { // Debug Print 只按函数调用公开导出，不传私有记录。
        *Callback = words[2]; *Context = 0ULL; *Registration = Node; // 函数在 Link 前一个指针槽。
        *Fields = KSWORD_ARK_CALLBACK_ENUM_FIELD_CALLBACK_ADDRESS; // 后续继续验证函数的可执行归属。
    } // 结束各类参数解码。
    return *Callback != 0ULL; // 非空函数还必须经调用方的可执行区验证。
} // 结束语义解码。
