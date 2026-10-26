#include "keyboard_rule_proof.h"

// 桶指针与循环计数器须为跨调用保存的通用寄存器，栈指针不能充当表步进。
static int KswLoopRegister(uint8_t reg)
{
    return reg == KSW_REG_RBX || reg == KSW_REG_RBP || reg == KSW_REG_RSI ||
        reg == KSW_REG_RDI || (reg >= KSW_REG_R12 && reg <= KSW_REG_R15);
}

// 所有候选指令先检查寄存器族，而不依赖实际版本的寄存器分配。
static int KswLoopRegOperand(const KSW_RULE_OPERAND* operand, uint8_t reg)
{
    return operand->Kind == KSW_RULE_OP_REG && operand->Reg == reg;
}

// 常量初始化只接受完整零化或直接赋值，不能把未经证明的旧寄存器值当计数。
static int KswLoopCounterInit(const KSW_RULE_INSN* instruction, uint8_t reg, uint32_t number)
{
    // 初始化只从指定寄存器的显式写入采样，不借用先前迭代的残留值。
    if (instruction->OperandCount != 2 || !KswLoopRegOperand(&instruction->Operands[0], reg)) return 0;
    if (instruction->Kind == KSW_RULE_MOV && instruction->Operands[1].Kind == KSW_RULE_OP_IMM &&
        instruction->Operands[1].Imm == (int64_t)number) return 1;
    return number == 0U && instruction->Kind == KSW_RULE_XOR &&
        KswLoopRegOperand(&instruction->Operands[1], reg);
}

// 每轮只允许一个一单位的计数变更，方向由循环类确定。
static int KswLoopCounterStep(const KSW_RULE_INSN* instruction, uint8_t reg, int upward)
{
    // INC/DEC与等价ADD/SUB 1均保留原寄存器族，别的算术不能充当桶计数。
    if (instruction->OperandCount == 0 || !KswLoopRegOperand(&instruction->Operands[0], reg)) return 0;
    if (instruction->Kind == (upward ? KSW_RULE_INC : KSW_RULE_DEC)) return 1;
    return instruction->OperandCount == 2 &&
        instruction->Kind == (upward ? KSW_RULE_ADD : KSW_RULE_SUB) &&
        instruction->Operands[1].Kind == KSW_RULE_OP_IMM && instruction->Operands[1].Imm == 1;
}

// 桶指针每轮必须按一枚指针宽度推进，排除RSP和其它算术伪步进。
static int KswLoopPointerStep(const KSW_RULE_INSN* instruction, uint8_t reg)
{
    // 每次回边恰好跨过一枚64位链头，避免把其它整数步进当桶遍历。
    return instruction->Kind == KSW_RULE_ADD && instruction->OperandCount == 2 &&
        KswLoopRegOperand(&instruction->Operands[0], reg) &&
        instruction->Operands[1].Kind == KSW_RULE_OP_IMM && instruction->Operands[1].Imm == 8;
}

// 比较值必须来自插入掩码交叉证明得到的桶数。
static int KswLoopCountCompare(const KSW_RULE_INSN* instruction, uint8_t reg, uint32_t buckets)
{
    // 比较常量来自注册插入掩码交叉证明，不能取样本中的固定128。
    return instruction->Kind == KSW_RULE_CMP && instruction->OperandCount == 2 &&
        KswLoopRegOperand(&instruction->Operands[0], reg) &&
        instruction->Operands[1].Kind == KSW_RULE_OP_IMM &&
        instruction->Operands[1].Imm == (int64_t)buckets;
}

// 会话来源必须是本映像识别的真实getter，并且落在插入规则证明的同一表偏移。
static int KswLoopSessionTable(KSW_FLOW_VALUE value, const KSW_FLOW_MODEL* model,
    const KSW_HOTKEY_RULE_RESULT* layout)
{
    // Source与位移都要对应同一实际会话getter和插入时证明的哈希表。
    return value.Kind == KSW_V_SESSION && value.Width >= 8 &&
        value.Source != 0U && value.Source == model->SessionSlot &&
        value.Value == (int64_t)layout->TableOffset;
}

// Mode2归纳后的桶槽继续保留同一会话来源与表偏移。
static int KswLoopTableSlot(KSW_FLOW_VALUE value, const KSW_FLOW_MODEL* model,
    const KSW_HOTKEY_RULE_RESULT* layout)
{
    // 指针归纳类型不能丢失原会话来源，重复同偏移的其它全局不满足。
    return value.Kind == KSW_V_TABLE_SLOT && value.Width >= 8 &&
        value.Source != 0U && value.Source == model->SessionSlot &&
        value.Aux == layout->TableOffset && value.Value == (int64_t)layout->TableOffset;
}

// 临时把某条必要指令截为RET，若目标仍可达就说明该指令并非必经。
static int KswLoopDominates(KSW_HK_RULE_CONTEXT* context, const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_MODEL* model, const KSW_FLOW_STATE* initial,
    uint32_t required, uint32_t target)
{
    KSW_RULE_INSN saved; // 局部IR指令在试探后恢复原字节语义。
    KSW_FLOW_ANALYSIS blocked; // 重新求解被截断的CFG可达性。
    int start, result = 0; // 不可达才表明该初始化或步进确实必经。
    if (required >= target || target >= function->InsnCount) return 0;
    start = KswFlowIndex(function, function->EntryRva); // 多pdata片段不假定入口是数组首项。
    if (start < 0) return 0;
    saved = function->Insns[required]; // 只修改临时已加载函数的该条IR。
    KswFlowZero(&blocked, sizeof(blocked));
    function->Insns[required].Kind = KSW_RULE_RET; // 仅修改当前已加载函数的临时IR。
    function->Insns[required].OperandCount = 0; // 被截断RET不再读取旧操作数。
    if (KswFlowRun(context->Full, function, model, (uint32_t)start, initial, &blocked))
        result = !blocked.States[target].Seen;
    else context->Error = blocked.Error ? blocked.Error : KSW_HK_RULE_BUDGET;
    function->Insns[required] = saved; // 任意结果都恢复原CFG后交还调用方。
    KswFlowFree(&blocked); // 不让多次支配性试探积累大状态。
    return result;
}

// 只允许规定的一次计数或指针写入出现在循环回边区间内。
static int KswLoopOnlyStepWrites(const KSW_RULE_FUNCTION* function,
    uint32_t begin, uint32_t end, uint8_t reg, uint32_t step)
{
    uint32_t index; // 遍历回边覆盖的实际指令范围。
    if (!KswLoopRegister(reg) || step < begin || step >= end || end >= function->InsnCount) return 0;
    for (index = begin; index <= end; ++index) // 除唯一canonical step外，寄存器不能重写。
        if (index != step && (function->Insns[index].RegWrites & (1U << reg)) != 0U) return 0;
    return (function->Insns[step].RegWrites & (1U << reg)) != 0U;
}

// 计数器在回边之前只允许有一个已到达的指定常量初始化。
static int KswLoopFindCounterInit(const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_ANALYSIS* basis, uint32_t before, uint8_t reg,
    uint32_t number, uint32_t* found)
{
    uint32_t index, count = 0; // 初始化若有两处可达定义则拒绝歧义。
    for (index = 0; index < before; ++index)
        if (basis->States[index].Seen && KswLoopCounterInit(&function->Insns[index], reg, number))
        {*found = index; ++count;} // 只发布唯一的0或N来源。
    return count == 1U;
}

// 指针初始化须直接取自真实会话基址加已证明的表偏移。
static int KswLoopFindPointerInit(const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_ANALYSIS* basis, const KSW_FLOW_MODEL* model,
    const KSW_HOTKEY_RULE_RESULT* layout, uint32_t before,
    uint8_t reg, uint32_t* found)
{
    uint32_t index, count = 0; // 表指针来源也要求唯一。
    for (index = 0; index < before; ++index)
    {
        const KSW_RULE_INSN* instruction = &function->Insns[index]; // 检查初始化实际操作数。
        KSW_FLOW_VALUE value; // 抽象地址必须是当前Session中的表。
        if (!basis->States[index].Seen || instruction->OperandCount != 2 ||
            !KswLoopRegOperand(&instruction->Operands[0], reg)) continue;
        if (instruction->Kind == KSW_RULE_LEA) // LEA保留偏移地址而不读取其内容。
            value = KswFlowAddress(&basis->States[index], &instruction->Operands[1], model);
        else if (instruction->Kind == KSW_RULE_MOV) // MOV只接收已经追到相同Session来源的值。
            value = KswFlowOperand(&basis->States[index], &instruction->Operands[1], model);
        else continue;
        if (!KswLoopSessionTable(value, model, layout)) continue;
        *found = index; // 作为后续CFG必经与最终Evidence锚点。
        ++count;
    }
    return count == 1U;
}

// 回边必须使用真实指令边界，且初始化留在循环之前。
static int KswLoopBackEdge(const KSW_RULE_FUNCTION* function, uint32_t branch,
    uint32_t* head)
{
    int index; // 实际目标必须映射到完整解码指令边界。
    const KSW_RULE_INSN* instruction = &function->Insns[branch]; // 只处理真实后向条件回边。
    if (instruction->Kind != KSW_RULE_JCC || instruction->Target >= instruction->Rva) return 0;
    index = KswFlowIndex(function, instruction->Target);
    if (index < 0 || (uint32_t)index >= branch) return 0;
    *head = (uint32_t)index; // 初始化必须在该回边之外。
    return 1;
}

// 找到同一个计数器的唯一一步，禁止在循环中有别的计数写入。
static int KswLoopFindCounterStep(const KSW_RULE_FUNCTION* function,
    uint32_t head, uint32_t before, uint8_t reg, int upward, uint32_t* found)
{
    uint32_t index, count = 0; // 每轮计数变化只允许一条。
    for (index = head; index < before; ++index)
        if (KswLoopCounterStep(&function->Insns[index], reg, upward))
        {*found = index; ++count;} // 同寄存器多步进不能取得证明。
    return count == 1U;
}

// 找到桶地址的唯一八字节步进，不能用栈调整或不相干寄存器代替。
static int KswLoopFindPointerStep(const KSW_RULE_FUNCTION* function,
    uint32_t head, uint32_t before, uint8_t reg, uint32_t* found)
{
    uint32_t index, count = 0; // 指针步进同样要求唯一。
    for (index = head; index < before; ++index)
        if (KswLoopPointerStep(&function->Insns[index], reg))
        {*found = index; ++count;} // 第二条加八或不同方向均形成歧义。
    return count == 1U;
}

// Mode2把指针归纳变量标成TABLE_SLOT，再实际验证循环内读取同一桶槽。
static int KswLoopPointerRead(KSW_HK_RULE_CONTEXT* context,
    const KSW_RULE_FUNCTION* function, const KSW_FLOW_STATE* initial,
    const KSW_FLOW_MODEL* inputModel, const KSW_HOTKEY_RULE_RESULT* layout,
    uint32_t head, uint32_t step, uint32_t* readIndex)
{
    KSW_FLOW_ANALYSIS analysis; // Mode2重新求得稳定桶指针的每条可达状态。
    KSW_FLOW_MODEL model = *inputModel; // 仅为当前候选启用指针归纳，不污染其它候选。
    uint32_t index, count = 0; // 要求在回边前确有同表节点读取。
    int start, result = 0; // 来源不明或解码预算失败都返回拒绝。
    KswFlowZero(&analysis, sizeof(analysis));
    model.Mode = 2U; // 同一TABLE_SLOT在每轮+8后保持稳定抽象身份。
    model.LoopIndexReg = 0U;
    start = KswFlowIndex(function, function->EntryRva); // 冷片段不改变实际函数入口。
    if (start < 0 || !KswFlowRun(context->Full, function, &model, (uint32_t)start,
            initial, &analysis))
    {context->Error = analysis.Error ? analysis.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (index = head; index < step; ++index)
    {
        const KSW_RULE_INSN* instruction = &function->Insns[index]; // 寻找当前桶的节点读取。
        KSW_FLOW_VALUE slot, node; // 槽地址与其节点值必须同源。
        const KSW_RULE_OPERAND* operand; // 只检查桶指针寄存器的零位移读取。
        if (!analysis.States[index].Seen || instruction->Kind != KSW_RULE_MOV ||
            instruction->OperandCount != 2 || instruction->Operands[0].Kind != KSW_RULE_OP_REG) continue;
        operand = &instruction->Operands[1]; // MOV源操作数才是表槽读取。
        if (operand->Kind != KSW_RULE_OP_MEM || operand->Base != model.LoopReg ||
            operand->Index != 0 || operand->Disp != 0 || operand->Width != 8) continue;
        slot = KswFlowAddress(&analysis.States[index], operand, &model); // 槽属于已识别的会话表。
        node = KswFlowOperand(&analysis.States[index], operand, &model); // 节点延续该槽Source。
        if (!KswLoopTableSlot(slot, &model, layout) || node.Kind != KSW_V_NODE ||
            node.Source != slot.Source) continue;
        *readIndex = index; // 后续CFG必经性围绕真实读表指令建立。
        ++count;
    }
    result = count != 0U; // 无实际桶读取的算术循环不能通过。
done: // 大型固定点状态在每个候选后立即释放。
    KswFlowFree(&analysis);
    return result;
}

// 指针式循环共有初始化与同表读取，只在计数方向及分支条件上变化。
static int KswLoopPointerCandidate(KSW_HK_RULE_CONTEXT* context,
    const KSW_RULE_FUNCTION* function, const KSW_FLOW_ANALYSIS* basis,
    const KSW_FLOW_STATE* initial, const KSW_FLOW_MODEL* inputModel,
    const KSW_HOTKEY_RULE_RESULT* layout, uint32_t branch,
    uint32_t head, int upward, KSW_FLOW_MODEL* resultModel, uint32_t* anchor)
{
    KSW_FLOW_MODEL model = *inputModel; // 每个指针候选从相同已验证导入模型开始。
    const KSW_RULE_INSN* condition = &function->Insns[branch]; // 后向条件跳转决定计数方向。
    uint32_t countInit = 0, pointerInit = 0, countStep = 0, pointerStep = 0; // 四个结构节点都需必经。
    uint32_t compare = branch - 1U, pointerMatches = 0, selectedAnchor = 0; // 仅发布唯一表指针。
    KSW_FLOW_MODEL selected = *inputModel; // 暂存完整成功候选后再复制给调用方。
    uint8_t counter, pointer; // 与编译器分配的实际非易失寄存器绑定。
    if (branch == 0 || !basis->States[branch].Seen) return 0;
    if (upward) // 0到N必须先CMP N，再由JB/JNE形成有界回边。
    {
        if ((condition->Condition != KSW_RULE_COND_C && condition->Condition != KSW_RULE_COND_NZ) ||
            function->Insns[compare].Kind != KSW_RULE_CMP ||
            function->Insns[compare].OperandCount != 2 ||
            function->Insns[compare].Operands[0].Kind != KSW_RULE_OP_REG) return 0;
        counter = function->Insns[compare].Operands[0].Reg; // 比较对象即本循环计数器。
        if (!KswLoopCountCompare(&function->Insns[compare], counter, layout->BucketCount)) return 0;
    }
    else // N到0由SUB/DEC 1的标志位直接控制JNZ。
    {
        if (condition->Condition != KSW_RULE_COND_NZ ||
            function->Insns[compare].OperandCount == 0 ||
            function->Insns[compare].Operands[0].Kind != KSW_RULE_OP_REG) return 0;
        counter = function->Insns[compare].Operands[0].Reg; // 最后一条算术同时承担计数与分支条件。
        if (!KswLoopCounterStep(&function->Insns[compare], counter, 0)) return 0;
        countStep = compare; // 降序步进的位置由紧邻JNZ的算术决定。
    }
    if (!KswLoopRegister(counter) || !KswLoopFindCounterInit(function, basis, head,
            counter, upward ? 0U : layout->BucketCount, &countInit)) return 0;
    if (upward && !KswLoopFindCounterStep(function, head, compare, counter, 1, &countStep)) return 0;
    if (!KswLoopOnlyStepWrites(function, head, branch, counter, countStep)) return 0;
    for (pointer = KSW_REG_RBX; pointer <= KSW_REG_R15; ++pointer) // 不预设某个Windows版本的寄存器。
    {
        uint32_t read = 0; // 本指针候选须找到同表桶头读取。
        model = *inputModel; // 前一个候选不得污染来源或计数语义。
        if (!KswLoopRegister(pointer) || pointer == counter ||
            !KswLoopFindPointerInit(function, basis, &model, layout, head,
                pointer, &pointerInit) ||
            !KswLoopFindPointerStep(function, head, upward ? compare : countStep,
                pointer, &pointerStep) ||
            !KswLoopOnlyStepWrites(function, head, branch, pointer, pointerStep)) continue;
        model.LoopReg = pointer; // 特定寄存器仅在通过初始化/步进结构后进入Mode2。
        model.LoopIndexReg = 0U;
        if (!KswLoopPointerRead(context, function, initial, &model, layout,
                head, pointerStep, &read)) continue;
        if (!KswLoopDominates(context, function, &model, initial, countInit, read) ||
            !KswLoopDominates(context, function, &model, initial, pointerInit, read) ||
            !KswLoopDominates(context, function, &model, initial, read, countStep) ||
            !KswLoopDominates(context, function, &model, initial, pointerStep, branch) ||
            !KswLoopDominates(context, function, &model, initial, countStep, branch)) continue;
        if (++pointerMatches > 1U) return 0; // 同一回边只能有一个真正完成读表与步进的指针。
        selected = model; // 缓存唯一已证明的指针式循环模型。
        selectedAnchor = pointerInit; // 输出会话表初始化证据位置。
    }
    if (pointerMatches != 1U) return 0;
    *resultModel = selected; // 全部候选审查完毕才提交寄存器身份。
    *anchor = selectedAnchor; // 与寄存器身份一同提交表来源锚点。
    return 1;
}

// 索引式循环要求每轮getter、同一base/index/offset的读节点与取槽地址。
static int KswLoopIndexedCandidate(KSW_HK_RULE_CONTEXT* context,
    const KSW_RULE_FUNCTION* function, const KSW_FLOW_ANALYSIS* basis,
    const KSW_FLOW_STATE* initial, const KSW_FLOW_MODEL* inputModel,
    const KSW_HOTKEY_RULE_RESULT* layout, uint32_t branch,
    uint32_t head, KSW_FLOW_MODEL* resultModel, uint32_t* anchor)
{
    KSW_FLOW_MODEL model = *inputModel; // 标记索引归纳时仍使用当前映像的真实getter。
    KSW_FLOW_ANALYSIS analysis; // 重新计算每轮索引与会话表访问来源。
    const KSW_RULE_INSN* condition = &function->Insns[branch]; // 回边必须受N边界控制。
    uint32_t compare = branch - 1U, counterInit = 0, counterStep = 0; // 唯一零初始化与加一位置。
    uint32_t getter = 0, read = 0, slot = 0, index, getterCount = 0, readCount = 0, slotCount = 0; // getter/读取/取槽三证独立计数。
    uint8_t counter; // 同一个非易失寄存器用作三处访问的桶索引。
    int start, result = 0; // 任何一项证据歧义都拒绝发布模型。
    if (branch == 0 || !basis->States[branch].Seen ||
        (condition->Condition != KSW_RULE_COND_C && condition->Condition != KSW_RULE_COND_NZ) ||
        function->Insns[compare].Kind != KSW_RULE_CMP ||
        function->Insns[compare].OperandCount != 2 ||
        function->Insns[compare].Operands[0].Kind != KSW_RULE_OP_REG) return 0;
    counter = function->Insns[compare].Operands[0].Reg;
    if (!KswLoopRegister(counter) ||
        !KswLoopCountCompare(&function->Insns[compare], counter, layout->BucketCount) ||
        !KswLoopFindCounterInit(function, basis, head, counter, 0U, &counterInit) ||
        !KswLoopFindCounterStep(function, head, compare, counter, 1, &counterStep) ||
        !KswLoopOnlyStepWrites(function, head, branch, counter, counterStep)) return 0;
    KswFlowZero(&analysis, sizeof(analysis)); // 后续失败可安全释放固定点结果。
    model.Mode = 2U; // 零初始化与INC在抽象值中合成稳定LOOP_INDEX。
    model.LoopReg = 0U;
    model.LoopIndexReg = counter;
    start = KswFlowIndex(function, function->EntryRva); // 不假设冷代码片段的排序位置。
    if (start < 0 || !KswFlowRun(context->Full, function, &model, (uint32_t)start,
            initial, &analysis))
    {context->Error = analysis.Error ? analysis.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (index = head; index < counterStep; ++index)
    {
        const KSW_RULE_INSN* instruction = &function->Insns[index]; // 只查看本次回边范围内的实际指令。
        KSW_FLOW_VALUE address, node; // 槽地址和节点内容均要来自同一getter。
        const KSW_RULE_OPERAND* memory; // 与计数寄存器相同的scale8索引操作数。
        if (!analysis.States[index].Seen) continue;
        if (instruction->Kind == KSW_RULE_CALL && instruction->OperandCount == 1 &&
            instruction->Operands[0].Kind == KSW_RULE_OP_MEM &&
            instruction->Target == model.SessionSlot)
        {getter = index; ++getterCount; continue;} // 每轮恰有一处真实session getter。
        if (instruction->OperandCount != 2 || instruction->Operands[0].Kind != KSW_RULE_OP_REG) continue;
        memory = &instruction->Operands[1]; // MOV/LEA都须使用同一索引形式。
        if (memory->Kind != KSW_RULE_OP_MEM || memory->Index != counter ||
            memory->Scale != 8 || memory->Width != 8) continue;
        address = KswFlowAddress(&analysis.States[index], memory, &model); // 确认地址在layout表内。
        if (!KswLoopTableSlot(address, &model, layout)) continue;
        if (instruction->Kind == KSW_RULE_MOV)
        {
            node = KswFlowOperand(&analysis.States[index], memory, &model); // 真实读表产出的节点保留Source。
            if (node.Kind != KSW_V_NODE || node.Source != address.Source) continue;
            read = index; ++readCount; // 节点读取不能有第二个可替代位置。
        }
        else if (instruction->Kind == KSW_RULE_LEA)
        {slot = index; ++slotCount;} // LEA得到指向同一桶头的链接槽。
    }
    if (getterCount != 1U || readCount != 1U || slotCount != 1U ||
        function->Insns[read].Operands[1].Base != function->Insns[slot].Operands[1].Base ||
        function->Insns[read].Operands[1].Disp != function->Insns[slot].Operands[1].Disp ||
        !KswLoopDominates(context, function, &model, initial, counterInit, getter) ||
        !KswLoopDominates(context, function, &model, initial, getter, read) ||
        !KswLoopDominates(context, function, &model, initial, getter, slot) ||
        !KswLoopDominates(context, function, &model, initial, read, counterStep) ||
        !KswLoopDominates(context, function, &model, initial, slot, counterStep) ||
        !KswLoopDominates(context, function, &model, initial, counterStep, branch)) goto done;
    *resultModel = model; // 所有CFG必经性验证后才提交索引寄存器。
    *anchor = slot; // 同表LEA作为最终可回查证据锚点。
    result = 1;
done: // 失败不留下任何候选固定点内存。
    KswFlowFree(&analysis);
    return result;
}

// 三种等价桶循环形态只发布唯一的结构证明，不按PE版本或RVA选型。
int KswHkProofLoop(KSW_HK_RULE_CONTEXT* context, const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_ANALYSIS* basis, const KSW_FLOW_STATE* initial,
    KSW_FLOW_MODEL* model, const KSW_HOTKEY_RULE_RESULT* layout,
    uint32_t* anchorIndex)
{
    KSW_FLOW_MODEL candidate, selected; // 只有一个候选时才发布完整循环模型。
    uint32_t branch, head, anchor = 0, selectedAnchor = 0, matches = 0, backEdges = 0; // 限制CFG搜索预算。
    KswFlowZero(&selected, sizeof(selected)); // 歧义失败不泄漏旧候选字段。
    if (!context || !context->Full || !function || !basis || !basis->States ||
        basis->Count != function->InsnCount || !initial || !model || !layout || !anchorIndex ||
        model->SessionSlot == 0U || layout->BucketCount < 2U ||
        layout->BucketCount > 256U || layout->TableOffset == 0U ||
        (layout->BucketCount & (layout->BucketCount - 1U)) != 0U) return 0;
    for (branch = 0; branch < function->InsnCount; ++branch) // 扫描真实可达的后向条件边。
    {
        if (!basis->States[branch].Seen || !KswLoopBackEdge(function, branch, &head)) continue;
        if (++backEdges > 64U) {context->Error = KSW_HK_RULE_BUDGET; return 0;} // 异常复杂函数不降级为猜测。
        if (KswLoopPointerCandidate(context, function, basis, initial, model,
                layout, branch, head, 0, &candidate, &anchor))
        {if (matches++) return 0; selected = candidate; selectedAnchor = anchor;} // N降至零的已证明回边。
        if (KswLoopPointerCandidate(context, function, basis, initial, model,
                layout, branch, head, 1, &candidate, &anchor))
        {if (matches++) return 0; selected = candidate; selectedAnchor = anchor;} // 零升至N的指针回边。
        if (KswLoopIndexedCandidate(context, function, basis, initial, model,
                layout, branch, head, &candidate, &anchor))
        {if (matches++) return 0; selected = candidate; selectedAnchor = anchor;} // getter加索引读取的回边。
        if (context->Error != 0U) return 0;
    }
    if (matches != 1U) return 0;
    *model = selected; // 唯一循环的寄存器身份回传给删除扫描。
    *anchorIndex = selectedAnchor; // 证据指令随模型一起提交。
    return 1;
}
