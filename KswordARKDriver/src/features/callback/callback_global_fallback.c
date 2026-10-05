/* 有界公开导出调用链 + 完整容器校验；只读特征回退从不提升 PDB 或注销信任。 */
#include "callback_global_fallback.h" // 内部验证契约和统一安全读取器。
#include "../../platform/pool_compat.h" // 兼容最低 16299 的非分页池分配器。

#define KSW_CALLBACK_GLOBAL_TAG 'gCbK' // 每次解析独立分配，避免内核栈承载候选集。
#define KSW_CALLBACK_GLOBAL_REFERENCE_LIMIT 256UL // 满容量视为截断，不能声称候选唯一。
#define KSW_CALLBACK_GLOBAL_SLOT_COUNT 64UL // 当前通知数组 ABI，完整读取后才发布。
#define KSW_CALLBACK_GLOBAL_LIST_LIMIT 512UL // 环形链必须在预算内完整闭合。
#define KSW_CALLBACK_GLOBAL_FAST_REF_MASK (~(ULONG_PTR)0x0FU) // x64 EX_FAST_REF 四位引用计数。

static PCSTR const g_ProcessAnchors[] = { // 覆盖公开包装器和不同进程注册子类型。
    "PsSetCreateProcessNotifyRoutine", "PsSetCreateProcessNotifyRoutineEx",
    "PsSetCreateProcessNotifyRoutineEx2" }; // 三者可能经同一个内部函数到达数组。
static PCSTR const g_ThreadAnchors[] = { // 移除和注册都可作为独立调用图入口。
    "PsRemoveCreateThreadNotifyRoutine", "PsSetCreateThreadNotifyRoutine",
    "PsSetCreateThreadNotifyRoutineEx" }; // 缺失新导出时保留旧导出。
static PCSTR const g_ImageAnchors[] = { // 支持 Ex 包装器或基本版本。
    "PsRemoveLoadImageNotifyRoutine", "PsSetLoadImageNotifyRoutine",
    "PsSetLoadImageNotifyRoutineEx" }; // 不通过单个短 LEA 模式猜测全局。
static PCSTR const g_RegistryAnchors[] = { // 使用返回 Cookie 的公开注册/注销链。
    "CmUnRegisterCallback", "CmRegisterCallback", "CmRegisterCallbackEx" }; // 私有布局另行校准。

static BOOLEAN KswCallbackGlobalKernelPointer(_In_ ULONG_PTR Address) // 拒绝用户地址与非对齐地址。
{
    return Address >= (ULONG_PTR)MmSystemRangeStart && // 候选只允许系统虚拟地址。
        (Address & (sizeof(ULONG_PTR) - 1U)) == 0U; // 容器/块地址必须自然对齐。
}

static BOOLEAN KswCallbackGlobalValidateArray( // 完整校验每个活动块，空数组没有布局证据。
    _In_ ULONG_PTR Address, _In_ ULONG_PTR KnownCallback,
    _In_ KSW_CALLBACK_GLOBAL_EXECUTABLE_PROBE ExecutableProbe,
    _In_opt_ PVOID Context)
{
    ULONG_PTR slots[KSW_CALLBACK_GLOBAL_SLOT_COUNT]; // 本地保存第一次数组快照。
    ULONG_PTR reread[KSW_CALLBACK_GLOBAL_SLOT_COUNT]; // 第二次读取检测注册/注销并发。
    ULONG index = 0UL; // 有界遍历全部 64 个槽。
    ULONG active = 0UL; // 全零 writable 数据不能证明这是通知数组。
    ULONG knownMatches = 0UL; // 当前自注册回调必须唯一出现在家族容器中。

    if (!KswCallbackGlobalKernelPointer(Address) || // 地址形状不合法时不访问。
        !KswordARKRuntimeReadMemory((const VOID*)Address, slots, sizeof(slots))) {
        return FALSE; // 短读和失败均不能形成容器证据。
    }
    for (index = 0UL; index < KSW_CALLBACK_GLOBAL_SLOT_COUNT; ++index) { // 所有非空项都必须有效。
        ULONG_PTR blockAddress = slots[index] & KSW_CALLBACK_GLOBAL_FAST_REF_MASK; // 剥离引用计数位。
        ULONG_PTR block[3]; // Rundown、Function、Context 的已知通知块前缀。
        ULONG_PTR blockAgain[3]; // 比对可影响行身份的两个字段。
        if (slots[index] == 0U) { // 真正空槽不用解释块。
            continue; // 当前项不参与有效容器候选。
        }
        if (!KswCallbackGlobalKernelPointer(blockAddress) || // 非零引用计数不能形成零块指针。
            !KswordARKRuntimeReadMemory((const VOID*)blockAddress, block, sizeof(block)) ||
            block[1] == 0U || !ExecutableProbe(Context, block[1]) || // 回调必须位于已加载模块可执行节。
            !KswordARKRuntimeReadMemory((const VOID*)blockAddress, blockAgain, sizeof(blockAgain)) ||
            blockAgain[1] != block[1] || blockAgain[2] != block[2]) {
            return FALSE; // 任一坏块、字段漂移或读取失败都拒绝整个候选。
        }
        active += 1UL; // 至少一个真实活动块提供结构依据。
        if (KnownCallback != 0U && block[1] == KnownCallback) { // 绑定家族身份。
            knownMatches += 1UL; // 记录精确自注册函数匹配数。
        }
    }
    if (active == 0UL || (KnownCallback != 0U && knownMatches != 1UL) || // 空/缺失/重复自注册不适配。
        !KswordARKRuntimeReadMemory((const VOID*)Address, reread, sizeof(reread))) {
        return FALSE; // 不能用局部已读槽证明完整容器。
    }
    for (index = 0UL; index < KSW_CALLBACK_GLOBAL_SLOT_COUNT; ++index) { // 全槽二次快照的块身份必须一致。
        ULONG_PTR rereadBlock = reread[index] & KSW_CALLBACK_GLOBAL_FAST_REF_MASK; // 引用计数变化不属于注册身份变化。
        if ((reread[index] != 0U && !KswCallbackGlobalKernelPointer(rereadBlock)) || // 非零低位不能单独形成合法空槽。
            (slots[index] & KSW_CALLBACK_GLOBAL_FAST_REF_MASK) != rereadBlock) { // 块地址变化留待下次重新枚举。
            return FALSE; // 当前校验失败，保留不可用状态。
        }
    }
    return TRUE; // 结构和本次完整快照均通过，仍是非 PDB 证据。
}

static BOOLEAN KswCallbackGlobalRegistryPass( // 每跳安全读取并检查前后链互指及已校准前缀。
    _In_ ULONG_PTR Address, _In_ ULONG_PTR KnownCallback,
    _In_ KSW_CALLBACK_GLOBAL_EXECUTABLE_PROBE ExecutableProbe,
    _In_opt_ PVOID Context, _Out_ ULONG64* IdentityOut, _Out_ ULONG* CountOut)
{
    LIST_ENTRY head; // 当前链头快照。
    LIST_ENTRY headAgain; // 完成后验证链头未改变。
    ULONG_PTR previous = Address; // 每个节点的 Blink 必须指向真实前驱。
    ULONG_PTR current = 0U; // 每次只从本地快照读取下一跳。
    ULONG count = 0UL; // 链表最大遍历预算。
    ULONG matches = 0UL; // 当前自回调只能出现一次。
    ULONG64 identity = 1469598103934665603ULL; // 两遍身份摘要只用于检测变化，不赋予信任。

    if (!KswordARKRuntimeReadMemory((const VOID*)Address, &head, sizeof(head))) { // 整头读取。
        return FALSE; // 当前校验失败，保留不可用状态。
    }
    current = (ULONG_PTR)head.Flink; // 开始遍历。
    if (current == Address || !KswCallbackGlobalKernelPointer((ULONG_PTR)head.Blink)) { // 空链不可校准。
        return FALSE; // 当前校验失败，保留不可用状态。
    }
    while (current != Address && count < KSW_CALLBACK_GLOBAL_LIST_LIMIT) { // 只接受预算内闭合环。
        ULONG_PTR prefix[6]; // +0/+8 Links、+0x18 Cookie、+0x20 Context、+0x28 Function。
        LIST_ENTRY next; // 当前节点的后继必须反向指回当前节点。
        ULONG index = 0UL; // 摘要仅包含链路和稳定身份字段。
        if (!KswCallbackGlobalKernelPointer(current) || // 每跳重新验证而非复用旧有效性。
            !KswordARKRuntimeReadMemory((const VOID*)current, prefix, sizeof(prefix)) ||
            prefix[1] != previous || !KswCallbackGlobalKernelPointer(prefix[0]) ||
            !KswordARKRuntimeReadMemory((const VOID*)prefix[0], &next, sizeof(next)) ||
            (ULONG_PTR)next.Blink != current || prefix[3] == 0U || // Cookie 为值且非零。
            prefix[5] == 0U || !ExecutableProbe(Context, prefix[5])) {
            return FALSE; // 断链、损坏前缀或数据节函数指针全部拒绝。
        }
        for (index = 0UL; index < RTL_NUMBER_OF(prefix); ++index) { // 跳过内部可变化的计数字段。
            if (index == 2UL) { // 非身份计数字段不参与快照一致性。
                continue; // 当前项不参与有效容器候选。
            }
            identity = (identity ^ (ULONG64)prefix[index]) * 1099511628211ULL; // 有界本地运算。
        }
        if (KnownCallback != 0U && prefix[5] == KnownCallback) { // 记录家族自注册依据。
            matches += 1UL; // 记录当前链中的精确自注册函数匹配数。
        }
        previous = current; // 保存本跳作为下一节点的前驱。
        current = prefix[0]; // 只使用本地读取出的前向链接。
        count += 1UL; // 使用安全读取出的本地链接推进。
    }
    if (current != Address || count == 0UL || // 未闭合、超限或空链均不具有完整性证据。
        (KnownCallback != 0U && matches != 1UL) || previous != (ULONG_PTR)head.Blink ||
        !KswordARKRuntimeReadMemory((const VOID*)Address, &headAgain, sizeof(headAgain)) ||
        headAgain.Flink != head.Flink || headAgain.Blink != head.Blink) {
        return FALSE; // 并发链头变化留待下一次重新定位。
    }
    *IdentityOut = identity; // 输出完整身份摘要。
    *CountOut = count; // 输出完整链本地摘要和数量。
    return TRUE; // 当前一遍全部通过。
}

NTSTATUS KswordArkCallbackGlobalFallbackResolve( // 公共枚举回退入口，PDB 优先由调用者保持。
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View, _In_ KSW_CALLBACK_GLOBAL_FAMILY Family,
    _In_ ULONG_PTR KnownCallback, _In_ KSW_CALLBACK_GLOBAL_EXECUTABLE_PROBE ExecutableProbe,
    _In_opt_ KSW_CALLBACK_GLOBAL_REGISTRY_LAYOUT_PROBE RegistryLayoutProbe,
    _In_opt_ PVOID ProbeContext, _Out_ ULONG_PTR* AddressOut)
{
    PCSTR const* anchors = NULL; // 选择当前回调家族锚点。
    ULONG anchorCount = 0UL; // 所有导出名称均为固定小数组。
    KSW_RUNTIME_DATA_REFERENCE* references = NULL; // 候选缓冲区放入非分页池。
    ULONG count = 0UL; // 扫描器返回的有界地址/函数对数量。
    ULONG index = 0UL; // 候选遍历下标。
    ULONG_PTR selected = 0U; // 始终只允许唯一有效容器。
    NTSTATUS status = STATUS_NOT_FOUND; // 缺证据时保持 unavailable。

    if (AddressOut == NULL) { // 不向空输出写入。
        return STATUS_INVALID_PARAMETER; // 参数不足不能开始解析。
    }
    *AddressOut = 0U; // 所有失败路径都不泄漏猜测地址。
    if (View == NULL || ExecutableProbe == NULL) { // 必需验证器完整。
        return STATUS_INVALID_PARAMETER; // 参数不足不能开始解析。
    }
    if (KeGetCurrentIrql() > APC_LEVEL) { // 安全读取器不支持高 IRQL。
        return STATUS_INVALID_DEVICE_STATE; // 高 IRQL 下不触碰候选地址。
    }
    switch (Family) { // 无 build 上限假设，按当前容器证据验证。
    case KswCallbackGlobalProcess: // 进程家族。
        anchors = g_ProcessAnchors; // 选择固定公开导出名称。
        anchorCount = RTL_NUMBER_OF(g_ProcessAnchors); // 固定导出数量。
        break; // 当前家族选择完成。
    case KswCallbackGlobalThread: // 线程家族。
        anchors = g_ThreadAnchors; // 选择固定公开导出名称。
        anchorCount = RTL_NUMBER_OF(g_ThreadAnchors); // 固定导出数量。
        break; // 当前家族选择完成。
    case KswCallbackGlobalImage: // 映像家族。
        anchors = g_ImageAnchors; // 选择固定公开导出名称。
        anchorCount = RTL_NUMBER_OF(g_ImageAnchors); // 固定导出数量。
        break; // 当前家族选择完成。
    case KswCallbackGlobalRegistry: // 注册表家族。
        anchors = g_RegistryAnchors; // 选择固定公开导出名称。
        anchorCount = RTL_NUMBER_OF(g_RegistryAnchors); // 固定导出数量。
        break; // 当前家族选择完成。
    default: // 不接受未知家族。
        return STATUS_INVALID_PARAMETER; // 不扫描未知公开入口。
    }
    if (Family == KswCallbackGlobalRegistry && RegistryLayoutProbe == NULL) { // 不猜 CM 前缀。
        return STATUS_NOT_SUPPORTED; // 缺少当前 CM 布局证据。
    }
    references = (KSW_RUNTIME_DATA_REFERENCE*)KswordARKAllocateNonPagedPool(
        sizeof(*references) * KSW_CALLBACK_GLOBAL_REFERENCE_LIMIT, KSW_CALLBACK_GLOBAL_TAG); // 固定小预算。
    if (references == NULL) { // 不降级为栈上大数组。
        return STATUS_INSUFFICIENT_RESOURCES; // 分配失败直接停止。
    }
    count = KswordARKRuntimeCollectFunctionDataReferences(View, anchors, anchorCount,
        2UL, 0x800UL, references, KSW_CALLBACK_GLOBAL_REFERENCE_LIMIT); // 只扫描 PE 函数边界内的两级调用图。
    if (count >= KSW_CALLBACK_GLOBAL_REFERENCE_LIMIT) { // 满缓冲不能证明其它候选不存在。
        ExFreePoolWithTag(references, KSW_CALLBACK_GLOBAL_TAG); // 释放本次资源。
        return STATUS_BUFFER_OVERFLOW; // 明确拒绝截断后的假唯一。
    }
    for (index = 0UL; index < count; ++index) { // 所有收集到的 writable 候选都要验证。
        ULONG_PTR address = references[index].Address; // 只接受直接引用地址，不猜邻近偏移。
        ULONG prior = 0UL; // 地址可能由不同函数重复引用。
        BOOLEAN duplicate = FALSE; // 同址引用不构成歧义。
        BOOLEAN valid = FALSE; // 每个候选独立获取完整结构证据。
        SIZE_T bytes = Family == KswCallbackGlobalRegistry ? sizeof(LIST_ENTRY) :
            sizeof(ULONG_PTR) * KSW_CALLBACK_GLOBAL_SLOT_COUNT; // 整个容器必须处于 writable 非代码节。
        for (prior = 0UL; prior < index; ++prior) { // 全部同址候选只处理一次。
            if (references[prior].Address == address) { // 去重地址。
                duplicate = TRUE; // 相同地址由多个函数引用只算一个候选。
                break; // 无需继续检查同址重复。
            }
        }
        if (duplicate || !KswCallbackGlobalKernelPointer(address) ||
            !KswordARKRuntimeAddressIsWritableData(View, address, bytes)) { // 非 writable 地址拒绝。
            continue; // 不读取已确认越界或非数据节的候选。
        }
        if (Family == KswCallbackGlobalRegistry) { // 前缀解释以当前自注册 Cookie 校准为前提。
            ULONG64 first = 0ULL, second = 0ULL; // 两遍完整链身份摘要。
            ULONG firstCount = 0UL, secondCount = 0UL; // 两遍数量也必须相等。
            valid = RegistryLayoutProbe(ProbeContext, address) && // 旧 Cookie 或未注册状态不能提供校准。
                KswCallbackGlobalRegistryPass(address, KnownCallback, ExecutableProbe, ProbeContext, &first, &firstCount) &&
                KswCallbackGlobalRegistryPass(address, KnownCallback, ExecutableProbe, ProbeContext, &second, &secondCount) &&
                first == second && firstCount == secondCount && RegistryLayoutProbe(ProbeContext, address); // 校准仍活跃。
        }
        else { // 通知数组沿用 EX_FAST_REF/RoutineBlock ABI，并完整验证活动项。
            valid = KswCallbackGlobalValidateArray(address, KnownCallback, ExecutableProbe, ProbeContext); // 双快照。
        }
        if (!valid) { // 部分可读或仅一个函数匹配不够发布。
            continue; // 当前项不参与有效容器候选。
        }
        if (selected != 0U) { // 多候选明确不可用。
            status = STATUS_OBJECT_NAME_COLLISION; // 不猜哪个结构正确。
            selected = 0U; // 失败输出不保留上一个猜测。
            break; // 已发现歧义，无需继续读取。
        }
        selected = address; // 暂存唯一有效地址。
        status = STATUS_SUCCESS; // 暂存唯一完整有效候选。
    }
    ExFreePoolWithTag(references, KSW_CALLBACK_GLOBAL_TAG); // 任何状态都释放本次候选集。
    *AddressOut = selected; // 仅唯一有效容器返回非零地址。
    return status; // 来源和删除信任由原有枚举行逻辑保持。
}
