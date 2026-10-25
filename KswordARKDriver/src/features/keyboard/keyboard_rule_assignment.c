#include "keyboard_rule_proof.h"

// 独立证明Base的赋值锁实际把描述符中的窗口对象写入目标槽。
// 输入描述符和槽位仅为抽象值；不读取、更不调用PE中的代码。
int KswHkProofAssignment(KSW_HK_RULE_CONTEXT* context, uint32_t* baseRva)
{
    // 解码函数与抽象状态只在本次证明期间存在，返回后只发布Base内的入口RVA。
    KSW_RULE_FUNCTION function;
    KSW_FLOW_ANALYSIS analysis;
    KSW_FLOW_MODEL model;
    KSW_FLOW_STATE* initial = 0;
    KSW_FLOW_VALUE slot, object;
    uint32_t entry = 0, index, returns = 0;
    int success = 0;

    // 失败时输出保持零；缺失Base映像或分配/读取回调不能构成任何证明。
    if (baseRva) *baseRva = 0;
    if (!context || !context->Base || !baseRva || !context->Base->Alloc ||
        !context->Base->Free || !context->Base->Read) return 0;
    KswFlowZero(&function, sizeof(function));
    KswFlowZero(&analysis, sizeof(analysis));
    KswFlowZero(&model, sizeof(model));
    // 仅以Base的真实导出为候选，并要求完整解码为可执行函数。
    if (!KswRuleFindExport(context->Base, "HMAssignmentLock", &entry) ||
        !KswRuleIsCode(context->Base, entry, 1) ||
        !KswRuleLoadFunction(context->Base, entry, &function)) return 0;
    (void)KswRuleFindImportSlot(context->Base, "ntoskrnl.exe", "KeBugCheckEx", &model.BugcheckSlot);

    // 入口描述符位于抽象栈帧，不向实际内核地址写入任何字节。
    initial = (KSW_FLOW_STATE*)context->Base->Alloc(context->Base->Context, sizeof(*initial));
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial);
    initial->Regs[KSW_REG_RCX] = KswFlowConst(0x100);
    initial->Regs[KSW_REG_RCX].Kind = KSW_V_FRAME;
    initial->Regs[KSW_REG_RDX] = KswFlowConst(0);
    initial->NodeAlive = 0;

    // 描述符的槽和值分别带节点与窗口对象来源，防止普通常量伪装为赋值。
    slot = KswFlowConst(0);
    slot.Kind = KSW_V_NODE;
    slot.Source = 1;
    object = KswFlowConst(0);
    object.Kind = KSW_V_WNDOBJ;
    object.Tag = 2;
    // 两个相邻八字节槽必须完整写入；资源预算不足直接拒绝。
    if (!KswFlowStore(initial->Stack, KSW_FLOW_STACK_CELLS, 0x100, 8, slot) ||
        !KswFlowStore(initial->Stack, KSW_FLOW_STACK_CELLS, 0x108, 8, object))
    {context->Error = KSW_HK_RULE_RESOURCE; goto done;}

    // 遍历所有可达控制流；分析错误不能当作不存在的赋值路径。
    if (!KswFlowRun(context->Base, &function, &model, 0, initial, &analysis))
    {
        if (analysis.Error) context->Error = analysis.Error;
        goto done;
    }
    // 每个可达RET都须看到目标槽由同一描述符窗口对象完整覆盖。
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_FLOW_STATE* state = &analysis.States[index];
        uint32_t cell;
        int written = 0;
        if (!state->Seen || function.Insns[index].Kind != KSW_RULE_RET) continue;
        ++returns;
        // 任一返回缺少该赋值，就不能把导出视为可靠赋值锁。
        for (cell = 0; cell < KSW_FLOW_NODE_CELLS; ++cell)
        {
            const KSW_FLOW_CELL* value = &state->Node[cell];
            if (value->Used && value->Offset == 0 && value->Width == 8 &&
                value->V.Kind == KSW_V_WNDOBJ && value->V.Tag == 2 &&
                value->V.Width >= 8)
            {written = 1; break;}
        }
        if (!written) goto done;
    }
    // 至少存在一个正常返回，才可发布该Base导出位置。
    if (!returns) goto done;
    *baseRva = entry;
    success = 1;
done:
    // 同源宿主回收分析状态、解码指令与入口抽象帧。
    KswFlowFree(&analysis);
    KswRuleFreeFunction(context->Base, &function);
    if (initial) context->Base->Free(context->Base->Context, initial);
    return success;
}
