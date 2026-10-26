#include "keyboard_rule_proof.h"

// 插入规则只接受同一输入节点、相同虚拟键索引、同一表的读-链接-写。
int KswHkProofInsert(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    KSW_HOTKEY_RULE_RESULT* result)
{
    // 函数IR、数据流和入口抽象节点均属于本次临时证明。
    KSW_RULE_FUNCTION function;
    KSW_FLOW_ANALYSIS analysis;
    KSW_FLOW_STATE* initial;
    KSW_FLOW_MODEL model = context->Model;
    uint32_t index, nextIndex = 0, tableIndex = 0, matches = 0;
    int success = 0;
    // 未完整读取目标函数时不得使用部分初始化的字段或证据。
    KswFlowZero(&function, sizeof(function));
    KswFlowZero(&analysis, sizeof(analysis));
    if (!KswHkLoad(context, entry, &function)) return 0;
    // 从宿主分配独立入口状态，避免把节点结构压入内核栈。
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial));
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial);
    // 参数一明确是尚存活的完整节点指针，不能由普通常量推导。
    initial->Regs[KSW_REG_RCX] = KswFlowConst(0);
    initial->Regs[KSW_REG_RCX].Kind = KSW_V_NODE;
    initial->NodeAlive = 1;
    model.Mode = 1;
    // Mode1只追同一新节点与会话表的插入关系。
    if (!KswFlowRun(context->Full, &function, &model, (uint32_t)KswFlowIndex(&function, function.EntryRva), initial, &analysis)) goto release;
    // 候选的第一次写入必须把实际表头放入当前节点的next字段。
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* instruction = &function.Insns[index];
        const KSW_FLOW_STATE* state = &analysis.States[index];
        KSW_FLOW_VALUE address, source;
        uint32_t other;
        // 仅接受可达的完整八字节内存写，部分指针写不能保留链身份。
        if (!state->Seen || instruction->Kind != KSW_RULE_MOV || instruction->OperandCount != 2 ||
            instruction->Operands[0].Kind != KSW_RULE_OP_MEM || instruction->Operands[0].Width != 8) continue;
        address = KswFlowAddress(state, &instruction->Operands[0], &model);
        source = KswFlowOperand(state, &instruction->Operands[1], &model);
        // 表头值须来自同一会话的哈希桶，next地址须在当前节点内。
        if (address.Kind != KSW_V_NODE || address.Value < 0 || source.Kind != KSW_V_TABLE_HEAD) continue;
        // 掩码必须构成不超过256桶的低位连续位集，不能从任意常量授权。
        if (source.Value <= 0 || source.Value > 0x100000 || source.Tag == 0 ||
            source.Tag > 255 || ((uint32_t)source.Tag & ((uint32_t)source.Tag + 1U)) != 0U) continue;
        for (other = index + 1; other < function.InsnCount; ++other)
        {
            const KSW_RULE_INSN* store = &function.Insns[other];
            const KSW_FLOW_STATE* after = &analysis.States[other];
            KSW_FLOW_VALUE destination, node;
            // 第二次写入需可达且是完整表头指针更新。
            if (!after->Seen || store->Kind != KSW_RULE_MOV || store->OperandCount != 2 ||
                store->Operands[0].Kind != KSW_RULE_OP_MEM || store->Operands[0].Width != 8) continue;
            destination = KswFlowAddress(after, &store->Operands[0], &model);
            node = KswFlowOperand(after, &store->Operands[1], &model);
            // 写回表头的节点必须与先前接入next的节点及桶索引完全相同。
            if (destination.Kind != KSW_V_TABLE_HEAD || node.Kind != KSW_V_NODE || node.Value != 0 ||
                destination.Value != source.Value || destination.Tag != source.Tag || destination.Aux != source.Aux) continue;
            // 多个候选只能给出完全一致的布局，最终仍由唯一匹配数控制。
            if (matches && ((uint32_t)source.Value != result->TableOffset ||
                (uint32_t)source.Tag + 1U != result->BucketCount ||
                (uint32_t)address.Value != result->NextOffset || source.Aux != result->VkOffset)) goto release;
            result->TableOffset = (uint32_t)source.Value;
            result->BucketCount = (uint32_t)source.Tag + 1U;
            result->VkOffset = source.Aux;
            result->NextOffset = (uint32_t)address.Value;
            result->InsertRva = entry;
            // 两条写入的IR索引用于后续必经检查与原始字节证据。
            nextIndex = index;
            tableIndex = other;
            ++matches;
        }
    }
    // 无候选或歧义候选都不能公开插入函数身份。
    if (matches != 1) goto release;
    // 排除next写入后必须无法抵达表头写入，验证控制流必经性而非地址先后。
    {
        KSW_RULE_FUNCTION blocked = function;
        KSW_RULE_INSN saved = function.Insns[nextIndex];
        KSW_FLOW_ANALYSIS bypass;
        // 临时阻断next写入后重跑CFG；若表头仍可写入，二者没有必经关系。
        KswFlowZero(&bypass, sizeof(bypass));
        blocked.Insns[nextIndex].Kind = KSW_RULE_RET;
        if (!KswFlowRun(context->Full, &blocked, &model, (uint32_t)KswFlowIndex(&function, function.EntryRva), initial, &bypass))
        {
            // 任一分析失败均恢复原IR并拒绝部分证明。
            blocked.Insns[nextIndex] = saved;
            KswFlowFree(&bypass);
            goto release;
        }
        success = !bypass.States[tableIndex].Seen;
        // 恢复临时指令，原映像及后续证据保持不变。
        blocked.Insns[nextIndex] = saved;
        KswFlowFree(&bypass);
    }
    // 同时保存两次真实写入的指令字节，供运行时指纹复核。
    if (success)
        success = KswHkEvidence(context, result, &function.Insns[nextIndex]) &&
            KswHkEvidence(context, result, &function.Insns[tableIndex]);
release:
    // 分析结果和入口状态由各自宿主释放，不遗留中间节点。
    KswFlowFree(&analysis);
    context->Full->Free(context->Full->Context, initial);
done:
    KswRuleFreeFunction(context->Full, &function);
    return success;
}
