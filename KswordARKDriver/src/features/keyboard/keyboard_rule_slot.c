#include "keyboard_rule_proof.h"

// 槽位与节点用同一来源标记，不能把另一个节点的next或释放调用混作当前目标。
enum {KSW_SLOT_SOURCE = 1U, KSW_SLOT_UNLINK = 8U, KSW_SLOT_FREE = 16U,
    KSW_SLOT_COMPLETE = KSW_SLOT_UNLINK | KSW_SLOT_FREE};

// 入口RCX是指向当前哈希链指针的槽地址，而不是节点本身。
static KSW_FLOW_VALUE KswSlotInput(void)
{
    KSW_FLOW_VALUE value = KswFlowConst(0); // 零值仅用来初始化抽象槽，不读取真实对象。
    // Tag、Aux和Source同时锁定唯一入口链槽来源，防止同位移别名混入。
    value.Kind = KSW_V_LINK;
    value.Tag = KSW_SLOT_SOURCE;
    value.Aux = KSW_SLOT_SOURCE;
    value.Source = KSW_SLOT_SOURCE;
    value.Width = 8;
    return value;
}

// 第二层释放包装器的入口RCX必须保持第一层从该槽取出的节点身份。
static KSW_FLOW_VALUE KswSlotNode(void)
{
    KSW_FLOW_VALUE value = KswFlowConst(0); // 第二层从原槽节点起步，延续同一来源标签。
    value.Kind = KSW_V_NODE;
    value.Tag = KSW_SLOT_SOURCE;
    value.Aux = KSW_SLOT_SOURCE;
    value.Source = KSW_SLOT_SOURCE;
    value.Width = 8;
    return value;
}

// 只有完整指针且来源、节点基址都一致时才允许充当释放实参。
static int KswSlotSameNode(KSW_FLOW_VALUE value)
{
    return value.Kind == KSW_V_NODE && value.Width >= 8 &&
        value.Tag == KSW_SLOT_SOURCE && value.Source == KSW_SLOT_SOURCE &&
        value.Value == 0;
}

// 链槽必须仍属于入口RCX，不能拿仅有相同位移的其它链槽冒充。
static int KswSlotSameLink(KSW_FLOW_VALUE value)
{
    return (value.Kind == KSW_V_LINK || value.Kind == KSW_V_TABLE_SLOT) &&
        value.Width >= 8 && value.Tag == KSW_SLOT_SOURCE &&
        value.Source == KSW_SLOT_SOURCE;
}

// 摘链写入同时绑定入口槽和从同一节点读取的next字段。
static int KswSlotUnlinkWrite(const KSW_FLOW_STATE* state,
    const KSW_RULE_INSN* instruction, const KSW_FLOW_MODEL* model)
{
    KSW_FLOW_VALUE address, next; // 同时比较写入目标链槽与写入值的节点字段来源。
    if (instruction->Kind != KSW_RULE_MOV || instruction->OperandCount != 2 ||
        instruction->Operands[0].Kind != KSW_RULE_OP_MEM ||
        instruction->Operands[0].Width != 8 || instruction->Operands[1].Width != 8) return 0;
    address = KswFlowAddress(state, &instruction->Operands[0], model); // 地址必须回到入口槽。
    next = KswFlowOperand(state, &instruction->Operands[1], model); // 源值必须是同节点next。
    return KswSlotSameLink(address) && next.Kind == KSW_V_NODE_FIELD &&
        next.Width >= 8 && next.Source == address.Source &&
        next.Aux == model->NextOffset;
}

// 释放包装器只能以真实Full导入槽或已解码直接函数为终点。
static int KswSlotFreeImport(const KSW_RULE_INSN* instruction, uint32_t slot)
{
    return (instruction->Kind == KSW_RULE_CALL || instruction->Kind == KSW_RULE_JMP) &&
        instruction->OperandCount == 1 && instruction->Operands[0].Kind == KSW_RULE_OP_MEM &&
        instruction->Target == slot;
}

// 直接调用同源节点的包装器需要先单独证明其所有终止路径均释放该节点。
static int KswSlotProofFreeWrapper(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    uint32_t freeSlot, uint32_t depth, KSW_HOTKEY_RULE_RESULT* result)
{
    KSW_RULE_FUNCTION function; // 当前实际包装器的完整解码区间。
    KSW_FLOW_ANALYSIS basis, final; // 首轮找释放入口，次轮核对全部终止路径。
    KSW_FLOW_STATE* initial = 0; // 抽象入口节点放宿主池，避免内核栈膨胀。
    KSW_FLOW_MODEL model = context->Model; // 沿用已验证PE导入与节点字段模型。
    uint32_t index, freeCount = 0, terminalCount = 0, nested = 0, freeIndex = 0; // 释放入口与终止路径必须有界且唯一。
    int start, success = 0; // 任一来源或路径不确定时保持失败。
    KswFlowZero(&function, sizeof(function)); // 失败清理路径不会误释放未加载代码。
    KswFlowZero(&basis, sizeof(basis));
    KswFlowZero(&final, sizeof(final));
    if (!KswHkLoad(context, entry, &function)) return 0; // 只在真实代码边界建立包装证明。
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial)); // 大状态不放调用栈。
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial); // 清除其它参数携带的旧证明事实。
    initial->Regs[KSW_REG_RCX] = KswSlotNode();
    model.Mode = 4U; // 第二层只证明同源节点到真实释放导入的生命周期。
    model.FreeSlot = freeSlot;
    model.FreeRva = 0U;
    start = KswFlowIndex(&function, function.EntryRva); // 冷代码片段可能改变指令数组的起点。
    if (start < 0 || !KswFlowRun(context->Full, &function, &model, (uint32_t)start,
            initial, &basis))
    {context->Error = basis.Error ? basis.Error : KSW_HK_RULE_BUDGET; goto done;}
    // 只从已可达且以同源节点作RCX的调用挑选释放终点。
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* instruction = &function.Insns[index]; // 当前候选实际调用指令。
        const KSW_FLOW_STATE* state = &basis.States[index]; // 仅分析入口可达且未失去节点来源的路径。
        if (!state->Seen || !KswSlotSameNode(state->Regs[KSW_REG_RCX])) continue;
        if (KswSlotFreeImport(instruction, freeSlot))
        {
            if (++freeCount != 1U || nested != 0U ||
                (instruction->Kind == KSW_RULE_JMP &&
                    (state->Regs[KSW_REG_RSP].Kind != KSW_V_FRAME ||
                        state->Regs[KSW_REG_RSP].Value != 0))) goto done;
            freeIndex = index; // 记住唯一真实导入释放位置供第二轮核验。
        }
        else if (KswHkCallDirect(instruction))
        {
            if (depth >= 2U || nested != 0U || freeCount != 0U ||
                instruction->Target == entry ||
                !KswSlotProofFreeWrapper(context, instruction->Target, freeSlot,
                    depth + 1U, result)) goto done;
            nested = instruction->Target; // 同一节点最多再经一层已证明包装器。
            freeIndex = index; // 外层调用也须由最终状态证明可达。
        }
    }
    if (freeCount + (nested != 0U) != 1U) goto done;
    model.FreeRva = nested; // 只把已独立证明的直接包装器登记为释放终点。
    if (!KswFlowRun(context->Full, &function, &model, (uint32_t)start,
            initial, &final))
    {context->Error = final.Error ? final.Error : KSW_HK_RULE_BUDGET; goto done;}
    // 每个可达返回都必须已经通过真实导入释放节点；尾跳只允许恢复入口栈后直达该导入。
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* instruction = &function.Insns[index]; // 审核每一个可能结束函数的指令。
        const KSW_FLOW_STATE* state = &final.States[index]; // 全路径合并后的释放守卫必须仍成立。
        if (!state->Seen) continue;
        if (instruction->Kind == KSW_RULE_RET)
        {
            if ((state->Guards & KSW_SLOT_FREE) != KSW_SLOT_FREE ||
                state->Regs[KSW_REG_RSP].Kind != KSW_V_FRAME ||
                state->Regs[KSW_REG_RSP].Value != 0) goto done;
            ++terminalCount; // 该RET仅在已释放同一节点且栈平衡时计入。
            if (!KswHkEvidence(context, result, instruction)) goto done;
        }
        else if (instruction->Kind == KSW_RULE_JMP &&
            KswFlowIndex(&function, instruction->Target) < 0)
        {
            if (!KswSlotFreeImport(instruction, freeSlot) ||
                !KswSlotSameNode(state->Regs[KSW_REG_RCX]) ||
                state->Regs[KSW_REG_RSP].Kind != KSW_V_FRAME ||
                state->Regs[KSW_REG_RSP].Value != 0) goto done;
            ++terminalCount; // 尾跳仅允许恢复入口栈后直达真实FreePool导入。
            if (!KswHkEvidence(context, result, instruction)) goto done;
        }
    }
    if (final.States[freeIndex].Seen && terminalCount != 0U &&
        KswHkEvidence(context, result, &function.Insns[freeIndex])) success = 1;
done: // 成功、歧义、预算失败共用临时分析与函数释放路径。
    KswFlowFree(&basis);
    KswFlowFree(&final);
    if (initial) context->Full->Free(context->Full->Context, initial);
    KswRuleFreeFunction(context->Full, &function);
    return success;
}

// 先找出唯一的实际摘链写和释放入口，再由Mode3证明全部返回路径。
int KswHkProofSlot(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    KSW_HOTKEY_RULE_RESULT* layout)
{
    KSW_RULE_FUNCTION function; // 一参helper的真实可达指令集合。
    KSW_FLOW_ANALYSIS basis, final; // 分别寻找结构写入和完成布尔回执证明。
    KSW_FLOW_STATE* initial = 0; // 指向链槽的入口状态由宿主分配。
    KSW_HOTKEY_RULE_RESULT* candidate = 0; // 新证据确认前不覆盖调用方布局。
    KSW_FLOW_MODEL model; // 从完整布局复制next、flags与子链语义。
    uint32_t freeSlot = 0, wrapper = 0, unlinkIndex = 0, freeIndex = 0; // 记录唯一摘链与释放位置。
    uint32_t index, unlinkCount = 0, freeCount = 0, successReturns = 0, returns = 0; // 预算内统计可达结果。
    int start, success = 0; // 只在全部成功路径已验证时发布入口。
    if (!context || !context->Full || !layout || layout->ObjectSize < 8U ||
        layout->ObjectSize > KSW_HOTKEY_RULE_MAX_NODE ||
        layout->NextOffset > layout->ObjectSize - 8U ||
        !KswRuleFindImportSlot(context->Full, "win32kbase.sys", "Win32FreePool", &freeSlot)) return 0;
    KswFlowZero(&function, sizeof(function)); // 失败清理路径安全处理未加载函数。
    KswFlowZero(&basis, sizeof(basis));
    KswFlowZero(&final, sizeof(final));
    model = context->Model; // 保留当前映像的实际导入槽和函数来源。
    model.Mode = 3U; // 入口是LINK槽，只有同源next写与释放才累计守卫。
    model.NextOffset = layout->NextOffset;
    model.FlagsOffset = layout->FlagsOffset;
    model.FlagsWidth = layout->FlagsWidth;
    model.ChildOffset = layout->ChildOffset;
    model.FreeSlot = freeSlot;
    model.FreeRva = 0U;
    if (!KswHkLoad(context, entry, &function)) return 0;
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial)); // 内核栈不承载大状态。
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    candidate = KswHkNewResult(context);
    if (!candidate) goto done;
    *candidate = *layout; // 原有字段与证据保留在堆上的暂存结果。
    KswFlowInit(initial);
    initial->Regs[KSW_REG_RCX] = KswSlotInput(); // 标记唯一输入链槽来源。
    start = KswFlowIndex(&function, function.EntryRva);
    if (start < 0 || !KswFlowRun(context->Full, &function, &model, (uint32_t)start,
            initial, &basis))
    {context->Error = basis.Error ? basis.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* instruction = &function.Insns[index]; // 当前可达候选结构指令。
        const KSW_FLOW_STATE* state = &basis.States[index]; // 只有同节点来源与摘链守卫可授权释放。
        if (!state->Seen) continue;
        if (KswSlotUnlinkWrite(state, instruction, &model))
        {
            if (++unlinkCount != 1U) goto done;
            unlinkIndex = index; // 摘链写入必须唯一且写出同节点next。
        }
        if ((state->Guards & KSW_SLOT_UNLINK) != KSW_SLOT_UNLINK ||
            !KswSlotSameNode(state->Regs[KSW_REG_RCX]) ||
            instruction->Kind != KSW_RULE_CALL) continue;
        if (KswSlotFreeImport(instruction, freeSlot))
        {
            if (++freeCount != 1U || wrapper != 0U) goto done;
            freeIndex = index; // 直接导入释放也须验证返回路径。
        }
        else if (KswHkCallDirect(instruction))
        {
            if (wrapper != 0U || freeCount != 0U ||
                !KswSlotProofFreeWrapper(context, instruction->Target, freeSlot,
                    1U, candidate)) goto done;
            wrapper = instruction->Target; // 只接纳递归证明过的包装入口。
            freeIndex = index; // 保存原helper中的释放调用证据。
        }
    }
    if (unlinkCount != 1U || freeCount + (wrapper != 0U) != 1U) goto done;
    model.FreeRva = wrapper; // 第二轮仅将已通过独立证明的wrapper算作释放。
    if (!KswFlowRun(context->Full, &function, &model, (uint32_t)start,
            initial, &final))
    {context->Error = final.Error ? final.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* instruction = &function.Insns[index]; // 检查每条实际可达RET。
        const KSW_FLOW_STATE* state = &final.States[index]; // 聚合守卫可能因失败分支而被清除。
        KSW_FLOW_VALUE value; // 仅解析AL的零、已验证真或其安全Phi。
        if (!state->Seen || instruction->Kind != KSW_RULE_RET) continue;
        ++returns; // 至少要存在一个可达完成路径。
        value = state->Regs[KSW_REG_RAX]; // 布尔零可有副作用，不能据此授权成功。
        if (KswFlowIsConst(value, 0)) continue;
        if (KswFlowIsConst(value, 1) &&
            (state->Guards & KSW_SLOT_COMPLETE) == KSW_SLOT_COMPLETE) ++successReturns;
        else if (value.Kind == KSW_V_CHECKED_BOOL && value.Tag == KSW_SLOT_COMPLETE)
            ++successReturns;
        else goto done;
        if (!KswHkEvidence(context, candidate, instruction)) goto done;
    }
    if (!returns || !successReturns || !final.States[unlinkIndex].Seen ||
        !final.States[freeIndex].Seen ||
        !KswHkEvidence(context, candidate, &function.Insns[unlinkIndex]) ||
        !KswHkEvidence(context, candidate, &function.Insns[freeIndex])) goto done;
    candidate->RemoveRva = entry; // 公开的是一参链槽helper，不是外层扫描器。
    *layout = *candidate;
    success = 1;
done: // 所有失败结果都释放暂存并保持调用方布局不变。
    KswFlowFree(&basis);
    KswFlowFree(&final);
    if (candidate) context->Full->Free(context->Full->Context, candidate);
    if (initial) context->Full->Free(context->Full->Context, initial);
    KswRuleFreeFunction(context->Full, &function);
    return success;
}
