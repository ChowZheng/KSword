#include "keyboard_rule_proof.h"

// 证据容器不进入嵌套内核栈，由有界宿主分配器提供临时空间。
KSW_HOTKEY_RULE_RESULT* KswHkNewResult(KSW_HK_RULE_CONTEXT* context)
{
    // 结果由宿主分配且清零，不能把大型证据数组压入内核栈。
    KSW_HOTKEY_RULE_RESULT* result = (KSW_HOTKEY_RULE_RESULT*)context->Full->Alloc(
        context->Full->Context, sizeof(KSW_HOTKEY_RULE_RESULT));
    // 分配失败记录资源错误，调用者据此区分真实失败与无匹配。
    if (result) KswFlowZero(result, sizeof(*result));
    else context->Error = KSW_HK_RULE_RESOURCE;
    return result;
}

int KswHkCallDirect(const KSW_RULE_INSN* instruction)
{
    // 只有CALL且首操作数是立即数时才属于直接调用。
    return instruction->Kind == KSW_RULE_CALL && instruction->OperandCount != 0 &&
        instruction->Operands[0].Kind == KSW_RULE_OP_IMM;
}

int KswHkLoad(KSW_HK_RULE_CONTEXT* context, uint32_t entry, KSW_RULE_FUNCTION* function)
{
    // 已分析函数必须先确认在代码节且可完整加载。
    uint32_t index;
    if (!KswRuleIsCode(context->Full, entry, 1) || !KswRuleLoadFunction(context->Full, entry, function)) return 0;
    // 重复引用同一函数沿用已有记录，不占用新预算。
    for (index = 0; index < context->SeenCount; ++index) if (context->SeenFunctions[index] == entry) return 1;
    // 超过分析函数数量上限时释放IR并报告预算失败。
    if (context->SeenCount == KSW_HOTKEY_RULE_MAX_FUNCTIONS)
    {
        context->Error = KSW_HK_RULE_BUDGET;
        KswRuleFreeFunction(context->Full, function);
        return 0;
    }
    // 新函数入口登记为后续运行时缓存的完整代码依赖。
    context->SeenFunctions[context->SeenCount++] = entry;
    return 1;
}

KSW_FLOW_VALUE KswHkCallArgument(const KSW_FLOW_STATE* state, uint32_t argument)
{
    // x64前四个实参位于RCX、RDX、R8、R9。
    const uint8_t registers[4] = {KSW_REG_RCX, KSW_REG_RDX, KSW_REG_R8, KSW_REG_R9};
    uint32_t index;
    int64_t offset;
    // 第五和第六参数只有在栈帧来源明确时才可读取。
    if (argument > 0 && argument <= 4) return state->Regs[registers[argument - 1]];
    if (argument < 5 || argument > 6 || state->Regs[KSW_REG_RSP].Kind != KSW_V_FRAME) return KswFlowUnknown();
    // 调用点栈指针加影子空间和参数序号得到实际栈槽偏移。
    offset = state->Regs[KSW_REG_RSP].Value + 0x20 + (int64_t)(argument - 5U) * 8;
    // 仅从已完整写入的栈单元恢复参数来源。
    for (index = 0; index < KSW_FLOW_STACK_CELLS; ++index)
        if (state->Stack[index].Used && state->Stack[index].Offset == offset)
            return state->Stack[index].V;
    return KswFlowUnknown();
}

int KswHkEvidence(KSW_HK_RULE_CONTEXT* context, KSW_HOTKEY_RULE_RESULT* result,
    const KSW_RULE_INSN* instruction)
{
    uint32_t index;
    KSW_HOTKEY_RULE_EVIDENCE* evidence;
    // 同一指令RVA只需记录一次原始字节证据。
    for (index = 0; index < result->EvidenceCount; ++index)
        if (result->Evidence[index].Rva == instruction->Rva) return 1;
    // 证据数组及指令长度均须符合架构上限。
    if (result->EvidenceCount >= KSW_HOTKEY_RULE_MAX_EVIDENCE ||
        instruction->Length == 0 || instruction->Length > 15) return 0;
    // 先完整读取指令字节，成功后才递增证据计数。
    evidence = &result->Evidence[result->EvidenceCount];
    evidence->Rva = instruction->Rva;
    evidence->Length = (uint8_t)instruction->Length;
    if (!KswRuleRead(context->Full, evidence->Rva, evidence->Bytes, evidence->Length)) return 0;
    ++result->EvidenceCount;
    return 1;
}

int KswHkLayoutValid(const KSW_HOTKEY_RULE_RESULT* result)
{
    // 节点十个字段逐一组成偏移/宽度对，含可选目标字段。
    uint32_t offsets[10], widths[10], index, other;
    offsets[0] = result->OwnerOffset; widths[0] = 8;
    offsets[1] = result->CallbackOffset; widths[1] = 8;
    offsets[2] = result->WindowOffset; widths[2] = 8;
    offsets[3] = result->DestinationOffset; widths[3] = result->DestinationWidth;
    offsets[4] = result->ModifiersOffset; widths[4] = result->ModifiersWidth;
    offsets[5] = result->FlagsOffset; widths[5] = result->FlagsWidth;
    offsets[6] = result->VkOffset; widths[6] = result->VkWidth;
    offsets[7] = result->IdOffset; widths[7] = result->IdWidth;
    offsets[8] = result->NextOffset; widths[8] = 8;
    offsets[9] = result->ChildOffset; widths[9] = 16;
    // 对象大小必须合理且按八字节对齐，桶数须为二的幂。
    if (result->ObjectSize < 32 || result->ObjectSize > KSW_HOTKEY_RULE_MAX_NODE ||
        (result->ObjectSize & 7U) != 0 || result->BucketCount < 2 || result->BucketCount > 256 ||
        (result->BucketCount & (result->BucketCount - 1U)) != 0U) return 0;
    // 每个字段须有合法对齐、在对象内且不与已有字段重叠。
    for (index = 0; index < 10; ++index)
    {
        uint32_t alignment = widths[index] > 8 ? 8 : widths[index];
        // 构造器已证明缺失的目标字段允许零宽度跳过。
        if (index == 3 && widths[index] == 0) continue;
        if (!(alignment == 1 || alignment == 2 || alignment == 4 || alignment == 8) ||
            offsets[index] % alignment != 0 || offsets[index] > result->ObjectSize ||
            widths[index] > result->ObjectSize - offsets[index]) return 0;
        for (other = 0; other < index; ++other)
            if (offsets[index] < offsets[other] + widths[other] &&
                offsets[other] < offsets[index] + widths[index]) return 0;
    }
    return 1;
}

// 公共导出和导入名只是语义锚点，不把样本身份当作功能开关。
int KswHotkeyMatchRules(const KSW_RULE_IMAGE* full, const KSW_RULE_IMAGE* base,
    KSW_HOTKEY_RULE_RESULT* output)
{
    // 入口只使用当前两张映像，不通过版本号或哈希放行。
    KSW_HK_RULE_CONTEXT context;
    uint32_t enter, leaveRva, validate, assignment = 0, provider = 0;
    // 输出先清空并预设图像错误，失败路径不会泄漏半份schema。
    if (!output) return 0;
    KswFlowZero(output, sizeof(*output));
    output->RuleVersion = KSW_HOTKEY_RULE_VERSION;
    output->Error = KSW_HK_RULE_INVALID_IMAGE;
    KswFlowZero(&context, sizeof(context));
    // 宿主读取和分配回调不完整时不能运行规则。
    if (!full || !base || !full->Read || !full->Alloc || !full->Free || !base->Read) return 0;
    context.Full = full; context.Base = base;
    // 从Full实际INT解析临界区、窗口验证和分配器槽。
    if (!KswRuleFindImportSlot(full, "win32kbase.sys", "EnterCrit", &context.Model.EnterSlot) ||
        !KswRuleFindImportSlot(full, "win32kbase.sys", "ValidateHwnd", &context.Model.ValidateSlot) ||
        !KswRuleFindImportSlot(full, "win32kbase.sys", "Win32AllocPoolZInit", &context.Model.AllocSlot)) return 0;
    // 会话getter按当前导入在W32与SGD提供者之间选择。
    if (!KswRuleFindImportSlot(full, "win32k.sys", "W32GetUserSessionState", &context.Model.SessionSlot))
    {
        if (!KswRuleFindImportSlot(full, "win32ksgd.sys", "SGDGetUserSessionState", &context.Model.SessionSlot)) return 0;
        provider = 1;
    }
    // owner导入数据与赋值锁证明是当前映像的可选能力。
    (void)KswRuleFindImportSlot(full, "win32kbase.sys", "gptiCurrent", &context.Model.OwnerDataSlot);
    if (KswHkProofAssignment(&context, &assignment))
        (void)KswRuleFindImportSlot(full, "win32kbase.sys", "HMAssignmentLock", &context.Model.AssignmentSlot);
    // LeaveCrit可由两种当前导出名提供，须确有真实槽位。
    if (!KswRuleFindImportSlot(full, "win32kbase.sys", "UserSessionSwitchLeaveCrit", &context.Model.LeaveSlot) &&
        !KswRuleFindImportSlot(full, "win32kbase.sys", "LeaveCrit", &context.Model.LeaveSlot)) return 0;
    // 已知BugCheck槽用于证明不返回路径，缺失时保持保守。
    (void)KswRuleFindImportSlot(full, "ntoskrnl.exe", "KeBugCheckEx", &context.Model.BugcheckSlot);
    // Base公开导出还需处于可执行代码节。
    if (!KswRuleFindExport(base, "EnterCrit", &enter) || !KswRuleFindExport(base, "ValidateHwnd", &validate) ||
        (!KswRuleFindExport(base, "UserSessionSwitchLeaveCrit", &leaveRva) && !KswRuleFindExport(base, "LeaveCrit", &leaveRva)) ||
        !KswRuleIsCode(base, enter, 1) || !KswRuleIsCode(base, leaveRva, 1) || !KswRuleIsCode(base, validate, 1)) return 0;
    // 注册、卸载和布局分阶段证明，各失败返回对应阶段错误。
    output->Error = KSW_HK_RULE_REGISTER;
    if (!KswHkProofRegister(&context, output)) {output->Error = KSW_HK_RULE_REGISTER; goto failure;}
    output->Error = KSW_HK_RULE_UNREGISTER;
    if (!KswHkProofUnregister(&context, output)) {output->Error = KSW_HK_RULE_UNREGISTER; goto failure;}
    if (!KswHkLayoutValid(output)) {output->Error = KSW_HK_RULE_LAYOUT; goto failure;}
    // 规则结果记录Base实际函数入口、会话getter和所有分析函数。
    output->EnterCritRva = enter;
    output->LeaveCritRva = leaveRva;
    output->ValidateHwndRva = validate;
    output->AssignmentRva = assignment;
    output->SessionGetterSlotRva = context.Model.SessionSlot;
    output->AnalyzedFunctionCount = context.SeenCount;
    KswFlowCopy(output->AnalyzedFunctions, context.SeenFunctions, context.SeenCount * sizeof(uint32_t));
    // 完整证明成功后才发布OK、提供者和匹配标志。
    output->RuleVersion = KSW_HOTKEY_RULE_VERSION;
    output->Error = KSW_HK_RULE_OK;
    output->SessionGetterProvider = provider;
    return 1;
failure:
    {
        // 失败时只保留错误码与规则版本，不提供半证明偏移。
        uint32_t error = context.Error ? context.Error : output->Error;
        KswFlowZero(output, sizeof(*output));
        output->RuleVersion = KSW_HOTKEY_RULE_VERSION;
        output->Error = error;
    }
    return 0;
}
