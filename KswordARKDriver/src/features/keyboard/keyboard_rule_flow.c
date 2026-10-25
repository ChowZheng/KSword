#include "keyboard_rule_flow.h"

// 指令RVA必须是已解码边界，不允许跳进立即数或跨未载入代码块。
int KswFlowIndex(const KSW_RULE_FUNCTION* function, uint32_t rva)
{
    // 二分查找只在已解码指令序列内进行，左右边界分别限定候选区间。
    uint32_t low = 0, high = function->InsnCount;
    while (low < high)
    {
        // 取当前区间中点并按RVA有序性收缩查找范围。
        uint32_t middle = low + (high - low) / 2U;
        if (function->Insns[middle].Rva < rva) low = middle + 1U;
        else high = middle;
    }
    // 只有RVA精确落在指令起点时才返回索引，否则拒绝该跳转边。
    return low < function->InsnCount && function->Insns[low].Rva == rva ? (int)low : -1;
}

static int KswDirect(const KSW_RULE_INSN* instruction)
{
    // 立即数首操作数表示可直接确定调用目标。
    return instruction->OperandCount > 0 && instruction->Operands[0].Kind == KSW_RULE_OP_IMM;
}

static KSW_FLOW_VALUE KswAddValue(KSW_FLOW_VALUE value, int64_t amount,
    const KSW_FLOW_MODEL* model, uint8_t destination)
{
    // 常量、栈帧、会话和全局地址的加减可保留原有来源。
    if (value.Kind == KSW_V_CONST || value.Kind == KSW_V_FRAME ||
        value.Kind == KSW_V_SESSION || value.Kind == KSW_V_GLOBAL)
    {
        // 用无符号运算承载位宽内的地址偏移，再写回抽象值。
        value.Value = (int64_t)((uint64_t)value.Value + (uint64_t)amount);
        return value;
    }
    // 节点指针偏移仍指向同一节点；删除模式下next字段成为链槽。
    if (value.Kind == KSW_V_NODE)
    {
        value.Value += amount;
        // 只有已知next偏移可提升为链槽来源。
        if (model->Mode == 2U && value.Value == (int64_t)model->NextOffset) value.Kind = KSW_V_LINK;
        return value;
    }
    // 已证明桶循环对槽地址加一个指针宽度，不改变其表身份。
    if (value.Kind == KSW_V_TABLE_SLOT && amount == 8 && destination == model->LoopReg)
        return value; // 已独立证明的桶循环归纳变量，不把每轮地址当作另一个表。
    // 索引循环每次加一时保留归纳变量来源。
    if (value.Kind == KSW_V_LOOP_INDEX && amount == 1 && destination == model->LoopIndexReg) return value;
    // 参数加零不改变原始参数身份。
    if (value.Kind == KSW_V_PARAM && amount == 0) return value;
    // 其余算术可能改变指针含义，按未知值处理。
    return KswFlowUnknown();
}

static int KswWriteOperand(KSW_FLOW_STATE* state, const KSW_RULE_OPERAND* operand,
    const KSW_FLOW_MODEL* model, KSW_FLOW_VALUE value)
{
    // 写回目标分为寄存器、已知栈槽、已分配节点和不可证明的外部内存。
    KSW_FLOW_VALUE address;
    // 寄存器写先识别已证明循环计数器的零初值。
    if (operand->Kind == KSW_RULE_OP_REG)
    {
        // 删除模式的循环索引从零开始，赋予专用来源标签。
        if (model->Mode == 2U && operand->Reg == model->LoopIndexReg && model->LoopIndexReg != 0 && KswFlowIsConst(value, 0))
        {value.Kind = KSW_V_LOOP_INDEX; value.Tag = 1;}
        // 寄存器有效宽度和截断规则由共用写入函数统一处理。
        KswFlowSetReg(state, operand->Reg, operand->Width, value);
        return 1;
    }
    // 内存目标先换算成保留来源的抽象地址。
    address = KswFlowAddress(state, operand, model);
    // 单槽证明只接受实际链位置上的指针写入。
    if (model->Mode == 3U && (address.Kind == KSW_V_LINK || address.Kind == KSW_V_TABLE_SLOT))
    {
        // 同源节点next指针完整覆盖链槽才建立摘链守卫8。
        if (operand->Width == 8 && value.Kind == KSW_V_NODE_FIELD && value.Aux == model->NextOffset &&
            value.Source != 0 && value.Source == address.Source) state->Guards |= 8U;
        // 不符合同源完整写入时清除摘链及释放相关守卫。
        else state->Guards &= 0xe7U;
        return 1;
    }
    // 帧内存写入可纳入有界栈槽状态。
    if (address.Kind == KSW_V_FRAME)
        return KswFlowStore(state->Stack, KSW_FLOW_STACK_CELLS, (int32_t)address.Value, operand->Width, value);
    // 普通模式仅跟踪本次分配节点的字段写入。
    if (address.Kind == KSW_V_NODE && model->Mode == 0U)
    {
        // 拒绝超出最大节点边界的字段写入。
        if (address.Value < 0 || address.Value + operand->Width > KSW_HOTKEY_RULE_MAX_NODE) return 0;
        return KswFlowStore(state->Node, KSW_FLOW_NODE_CELLS, (int32_t)address.Value, operand->Width, value);
    }
    return 1; // 未知外部内存不变成任何已证明字段。
}

static uint8_t KswCompareGuard(KSW_FLOW_VALUE left, KSW_FLOW_VALUE right,
    const KSW_FLOW_MODEL* model, const KSW_FLOW_STATE* state)
{
    // 比较字段可能位于任一侧，先把节点字段归一到左侧。
    if (left.Kind != KSW_V_NODE_FIELD)
    {
        KSW_FLOW_VALUE swap = left;
        left = right;
        right = swap;
    }
    // 只有节点字段比较才可能证明删除目标身份。
    if (left.Kind != KSW_V_NODE_FIELD) return 0;
    // 归属字段必须来自准确的调用参数或同源owner表达式，建立守卫1。
    if (left.Aux == model->OwnerOffset &&
        ((!model->RemoveSignature && right.Kind == KSW_V_PARAM && right.Tag == 1 && right.Aux == 0) ||
            (model->RemoveSignature && (right.Kind == KSW_V_CALL || right.Kind == KSW_V_OWNER || right.Kind == KSW_V_NULL_OWNER) &&
                right.Aux == model->OwnerSourceRva && right.Tag == model->OwnerSourceKind &&
                right.Value == model->OwnerSourceDepth))) return 1;
    // 注册ID字段必须对应当前扫描签名的原始参数，建立守卫2。
    if (left.Aux == model->IdOffset && right.Kind == KSW_V_PARAM &&
        right.Tag == (model->RemoveSignature ? 2 : 3) && right.Aux == 0) return 2;
    // 窗口字段允许已证明的窗口值、对象表示或明确空参数，建立守卫4。
    if (left.Aux == model->WindowOffset &&
        (((right.Kind == KSW_V_WINDOW || right.Kind == KSW_V_NULL_WINDOW) && right.Tag == (model->RemoveSignature ? 1 : 2)) ||
            (model->WindowRepresentation && (right.Kind == KSW_V_WNDOBJ || right.Kind == KSW_V_NULL_WNDOBJ) &&
                right.Tag == (model->RemoveSignature ? 1 : 2)) ||
            (KswFlowIsConst(right, 0) && (state->NullArgs & (1U << (model->RemoveSignature ? 1 : 2))) != 0U))) return 4;
    // 其它比较不提供任何节点身份守卫。
    return 0;
}

// 调用返回值的来源使用callee/IAT槽，不从版本或符号地址名单授予权限。
static int KswCallTransfer(KSW_FLOW_STATE* state, const KSW_RULE_INSN* instruction,
    const KSW_FLOW_MODEL* model)
{
    // 调用结果初始未知；列出x64调用约定中的易失寄存器并记录目标类型。
    KSW_FLOW_VALUE result = KswFlowUnknown();
    const uint8_t volatileRegisters[7] = {KSW_REG_RAX, KSW_REG_RCX, KSW_REG_RDX,
        KSW_REG_R8, KSW_REG_R9, KSW_REG_R10, KSW_REG_R11};
    uint32_t index;
    int direct = KswDirect(instruction);
    int known = 0;
    // 已知BugCheck导入不返回，不能构造后续可达状态。
    if (!direct && instruction->Target != 0 && instruction->Target == model->BugcheckSlot) return 0;
    // EnterCrit只有两参数均为零时才证明进入所需USER临界区。
    if (!direct && instruction->Target == model->EnterSlot && model->EnterSlot != 0)
    {
        state->Crit = (uint8_t)(KswFlowIsConst(state->Regs[KSW_REG_RCX], 0) &&
            KswFlowIsConst(state->Regs[KSW_REG_RDX], 0));
        known = 1;
    }
    // LeaveCrit立即撤销当前路径的临界区事实。
    if (!direct && instruction->Target == model->LeaveSlot && model->LeaveSlot != 0)
    {
        state->Crit = 0;
        known = 1;
    }
    // 窗口验证器返回值沿用传入句柄的来源，完整参数才可提升为对象。
    if (!direct && instruction->Target == model->ValidateSlot && model->ValidateSlot != 0)
    {
        result = state->Regs[KSW_REG_RCX];
        if (result.Kind == KSW_V_PARAM && result.Width >= 8) result.Kind = KSW_V_WNDOBJ;
        else if (!KswFlowIsConst(result, 0)) result = KswFlowUnknown();
        known = 1;
    }
    // 会话getter返回抽象会话基址，并以实际IAT槽记录来源。
    if (!direct && instruction->Target == model->SessionSlot && model->SessionSlot != 0)
    {
        result = KswFlowConst(0);
        result.Kind = KSW_V_SESSION;
        result.Source = model->SessionSlot;
        known = 1;
    }
    // HMAssignmentLock仅按已证明的帧描述符模拟目标字段写入。
    if (!direct && instruction->Target == model->AssignmentSlot && model->AssignmentSlot != 0)
    {
        KSW_RULE_OPERAND descriptor;
        KSW_FLOW_VALUE address, object;
        // 描述符首槽为节点地址、第二槽为窗口对象，均从当前栈帧读取。
        KswFlowZero(&descriptor, sizeof(descriptor));
        descriptor.Kind = KSW_RULE_OP_MEM; descriptor.Width = 8; descriptor.Base = KSW_REG_RCX;
        address = KswFlowOperand(state, &descriptor, model);
        descriptor.Disp = 8;
        object = KswFlowOperand(state, &descriptor, model);
        // 仅当节点地址、零参数和对象来源均满足约束时记入对象字段。
        if (model->Mode == 0U && state->Regs[KSW_REG_RCX].Kind == KSW_V_FRAME &&
            KswFlowIsConst(state->Regs[KSW_REG_RDX], 0) && address.Kind == KSW_V_NODE && address.Value >= 0 &&
            address.Value + 8 <= KSW_HOTKEY_RULE_MAX_NODE &&
            (object.Kind == KSW_V_WNDOBJ || object.Kind == KSW_V_NULL_WNDOBJ || KswFlowIsConst(object, 0)))
            if (!KswFlowStore(state->Node, KSW_FLOW_NODE_CELLS, (int32_t)address.Value, 8, object)) return -1;
        known = 1;
    }
    // 单槽/释放包装证明只承认已匹配释放入口及当前节点指针。
    if ((model->Mode == 3U || model->Mode == 4U) &&
        ((!direct && instruction->Target == model->FreeSlot && model->FreeSlot != 0) ||
            (direct && instruction->Target == model->FreeRva && model->FreeRva != 0)))
    {
        // 释放实参须是零偏移、完整宽度且来源为当前单槽节点。
        KSW_FLOW_VALUE node = state->Regs[KSW_REG_RCX];
        if (node.Kind == KSW_V_NODE && node.Value == 0 && node.Width >= 8 && node.Source == 1 &&
            (model->Mode == 4U || (state->Guards & 8U) != 0U))
        {
            // 同一路径第二次释放不能作为有效生命周期证明。
            if ((state->Guards & 16U) != 0U) return -1;
            state->Guards |= 16U;
        }
        known = 1;
    }
    // 只有已识别分配点调用已识别分配器才产生新节点。
    if (instruction->Rva == model->AllocRva && model->AllocRva != 0 &&
        !direct && instruction->Target == model->AllocSlot)
    {
        // 新节点以分配指令RVA标识来源，并清空旧节点字段状态。
        result = KswFlowConst(0);
        result.Kind = KSW_V_NODE;
        result.Source = model->AllocRva;
        state->NodeAlive = 1;
        KswFlowZero(state->Node, sizeof(state->Node));
        known = 1;
    }
    // 其余调用先保留目标RVA和直接或间接调用类别。
    if (!known)
    {
        result.Kind = KSW_V_CALL;
        result.Width = 8;
        result.Aux = instruction->Target;
        result.Tag = (uint16_t)(direct ? 0 : 1);
        // 透明owner包装仅按已证明的别名表恢复规范来源与解引用深度。
        if (direct)
            for (index = 0; index < model->OwnerAliasCount && index < 8; ++index)
                if (model->OwnerAliases[index].EntryRva == instruction->Target)
                {
                    result.Aux = model->OwnerAliases[index].CanonicalRva;
                    result.Tag = model->OwnerAliases[index].Kind;
                    result.Value = model->OwnerAliases[index].Depth;
                    result.Kind = result.Value ? KSW_V_OWNER : KSW_V_CALL;
                    break;
                }
    }
    // 新的删除调用先清除旧的成功事实，等待本次返回分支确认。
    if (direct && instruction->Target == model->RemoveRva && model->RemoveRva != 0)
        state->RemoveSucceeded = 0;
    // 插入调用的真实到达给当前分配路径标记提交守卫32。
    if (direct && instruction->Target == model->InsertRva && model->InsertRva != 0)
        state->Guards |= 32U; // 本分配路径已经真实提交到已证明的同一哈希表。
    // 未知调用若接收新节点参数，就不能继续假定节点未逃逸。
    if (state->NodeAlive && !known && instruction->Target != model->InsertRva &&
        instruction->Target != model->OwnerGetterRva)
    {
        // 逐个检查参数寄存器是否携带本次新节点。
        for (index = 1; index < 5; ++index)
        {
            uint8_t reg = volatileRegisters[index];
            if (state->Regs[reg].Kind == KSW_V_NODE) state->NodeAlive = 0;
        }
    }
    // 调用会改写所有易失寄存器，仅RAX保留归类后的返回来源。
    for (index = 0; index < 7; ++index) state->Regs[volatileRegisters[index]] = KswFlowUnknown();
    state->Regs[KSW_REG_RAX] = result;
    // 调用破坏旧条件码、指针界限和比较守卫，避免跨调用复用。
    state->LastZeroKnown = 0;
    state->CarryKnown = state->SignKnown = state->LessKnown = 0;
    state->PointerBelowLimit = 0;
    state->LastGuard = 0;
    state->LastTestReg = 0;
    state->LastValue = KswFlowUnknown();
    return 1;
}

static int KswTransfer(KSW_FLOW_STATE* state, const KSW_RULE_INSN* instruction,
    const KSW_FLOW_MODEL* model)
{
    // 取目标和两个操作数的抽象值作为当前指令转移输入。
    const KSW_RULE_OPERAND* destination = &instruction->Operands[0];
    KSW_FLOW_VALUE left = KswFlowUnknown(), right = KswFlowUnknown(), value;
    uint32_t index;
    if (instruction->OperandCount > 0) left = KswFlowOperand(state, destination, model);
    if (instruction->OperandCount > 1) right = KswFlowOperand(state, &instruction->Operands[1], model);
    // 调用交给专门的调用约定处理；返回或陷阱终止本路径。
    if (instruction->Kind == KSW_RULE_CALL) return KswCallTransfer(state, instruction, model);
    if (instruction->Kind == KSW_RULE_RET || instruction->Kind == KSW_RULE_TRAP) return 0;
    // 压栈先移动抽象栈指针，再把完整源值写入新栈槽。
    if (instruction->Kind == KSW_RULE_PUSH)
    {
        state->Regs[KSW_REG_RSP] = KswAddValue(state->Regs[KSW_REG_RSP], -8, model, KSW_REG_RSP);
        // 栈指针失去帧身份时不得继续证明栈参数。
        if (state->Regs[KSW_REG_RSP].Kind != KSW_V_FRAME) return -1;
        return KswFlowStore(state->Stack, KSW_FLOW_STACK_CELLS,
            (int32_t)state->Regs[KSW_REG_RSP].Value, 8, left) ? 1 : -1;
    }
    // 弹栈先读取旧栈顶，再移动栈指针并写回目标。
    if (instruction->Kind == KSW_RULE_POP)
    {
        KSW_RULE_OPERAND memory;
        KswFlowZero(&memory, sizeof(memory));
        memory.Kind = KSW_RULE_OP_MEM; memory.Base = KSW_REG_RSP; memory.Width = 8;
        value = KswFlowOperand(state, &memory, model);
        state->Regs[KSW_REG_RSP] = KswAddValue(state->Regs[KSW_REG_RSP], 8, model, KSW_REG_RSP);
        return KswWriteOperand(state, destination, model, value) ? 1 : -1;
    }
    // 普通移动与扩展移动都按目标宽度写入抽象来源。
    if (instruction->Kind == KSW_RULE_MOV || instruction->Kind == KSW_RULE_MOVZX ||
        instruction->Kind == KSW_RULE_MOVSX)
    {
        // 删除链向下读到新节点时旧元素的身份守卫必须失效。
        if (destination->Kind == KSW_RULE_OP_REG && destination->Reg != KSW_REG_RSP &&
            right.Kind == KSW_V_NODE && instruction->Operands[1].Kind == KSW_RULE_OP_MEM && model->Mode == 2U)
            state->Guards = 0; // 新链元素必须重新经过所有者、窗口和ID三个守卫。
        // 从会话表起点读入循环寄存器时标记真实表槽。
        if (right.Kind == KSW_V_SESSION && destination->Kind == KSW_RULE_OP_REG &&
            destination->Reg == model->LoopReg && right.Value == model->TableOffset && model->Mode == 2U)
        {right.Kind = KSW_V_TABLE_SLOT; right.Aux = model->TableOffset;}
        return KswWriteOperand(state, destination, model, right) ? 1 : -1;
    }
    // LEA只计算抽象地址；已知常量基址的偏移折叠成常量。
    if (instruction->Kind == KSW_RULE_LEA)
    {
        value = KswFlowAddress(state, &instruction->Operands[1], model);
        if (instruction->Operands[1].Base != 0 && instruction->Operands[1].Index == 0 &&
            state->Regs[instruction->Operands[1].Base].Kind == KSW_V_CONST)
            value = KswFlowConst(state->Regs[instruction->Operands[1].Base].Value + instruction->Operands[1].Disp);
        // 删除循环的表起点地址保留桶槽来源。
        if (value.Kind == KSW_V_SESSION && destination->Reg == model->LoopReg &&
            value.Value == (int64_t)model->TableOffset && model->Mode == 2U)
        {value.Kind = KSW_V_TABLE_SLOT; value.Aux = model->TableOffset;}
        return KswWriteOperand(state, destination, model, value) ? 1 : -1;
    }
    // CMP/TEST重设条件事实并从本次操作数重新建立证明。
    if (instruction->Kind == KSW_RULE_CMP || instruction->Kind == KSW_RULE_TEST)
    {
        // 旧ZF、符号、进位和比较对象都不能跨新比较沿用。
        state->LastZeroKnown = 0;
        state->CarryKnown = state->SignKnown = state->LessKnown = 0;
        state->LastTestReg = 0;
        state->LastValue = KswFlowUnknown();
        state->PointerBelowLimit = 0;
        // 节点字段与目标身份的比较结果另存为待分支确认守卫。
        state->LastGuard = KswCompareGuard(left, right, model, state);
        // 会话字段与零比较时记录缓存字段位置供失效证明。
        if (instruction->Kind == KSW_RULE_CMP && left.Kind == KSW_V_SESSION_FIELD && KswFlowIsConst(right, 0))
            state->ComparedCache = left.Aux;
        // 两个常量操作数可精确求出零、符号、进位及有符号小于条件。
        if (left.Kind == KSW_V_CONST && right.Kind == KSW_V_CONST)
        {
            state->LastZeroKnown = 1;
            state->LastZero = (uint8_t)(instruction->Kind == KSW_RULE_CMP ?
                left.Value == right.Value : (left.Value & right.Value) == 0);
            {
                // 先按操作数宽度形成位掩码，再模拟相同宽度的CMP或TEST结果。
                uint32_t bits = (uint32_t)destination->Width * 8U;
                uint64_t mask = bits && bits < 64 ? (UINT64_C(1) << bits) - 1U : UINT64_MAX;
                uint64_t a = (uint64_t)left.Value & mask, b = (uint64_t)right.Value & mask;
                uint64_t number = instruction->Kind == KSW_RULE_CMP ? (a - b) & mask : a & b;
                // 按截断结果的最高位计算符号条件。
                state->SignKnown = 1; state->Sign = bits ? (uint8_t)((number >> (bits - 1U)) & 1U) : 0;
                // CMP另外计算无符号借位与有符号小于；TEST的进位恒为零。
                if (instruction->Kind == KSW_RULE_CMP)
                {state->CarryKnown = 1; state->Carry = (uint8_t)(a < b); state->LessKnown = 1;
                 state->Less = (uint8_t)((int64_t)(a << (64U - bits)) < (int64_t)(b << (64U - bits)));}
                else {state->CarryKnown = 1; state->Carry = 0;}
            }
        }
        // 合法窗口对象与保留哨兵值不相等，条件码可直接确定。
        if (instruction->Kind == KSW_RULE_CMP &&
            (left.Kind == KSW_V_WNDOBJ || left.Kind == KSW_V_NULL_WNDOBJ) &&
            right.Kind == KSW_V_CONST && (right.Value == 1 || right.Value == 2))
        {
            state->LastZeroKnown = 1; state->LastZero = 0;
            // 与界限2比较时保留空对象分支的专用条件事实。
            if (right.Value == 2)
            {state->PointerBelowLimit = 1; state->LastValue = left;
             state->LastTestReg = destination->Kind == KSW_RULE_OP_REG ? destination->Reg : 0;}
        }
        // 自测或与零比较记录被测试值，并推导已约束参数的零条件。
        if ((instruction->Kind == KSW_RULE_TEST && destination->Kind == KSW_RULE_OP_REG &&
                instruction->Operands[1].Kind == KSW_RULE_OP_REG &&
                destination->Reg == instruction->Operands[1].Reg) ||
            (instruction->Kind == KSW_RULE_CMP && KswFlowIsConst(right, 0)))
        {
            state->LastValue = left;
            if (destination->Kind == KSW_RULE_OP_REG) state->LastTestReg = destination->Reg;
            // 先前路径已证明参数为空或非空时直接确定本次ZF。
            if ((left.Kind == KSW_V_PARAM || left.Kind == KSW_V_WNDOBJ || left.Kind == KSW_V_NULL_WNDOBJ) && left.Tag < 16)
            {
                uint16_t mask = (uint16_t)(1U << left.Tag);
                if ((state->NullArgs & mask) != 0U) {state->LastZeroKnown = 1; state->LastZero = 1;}
                if ((state->NonNullArgs & mask) != 0U) {state->LastZeroKnown = 1; state->LastZero = 0;}
            }
        }
        return 1;
    }
    // 支持的算术/位运算仅在来源可证明时传播抽象值。
    if (instruction->Kind == KSW_RULE_ADD || instruction->Kind == KSW_RULE_SUB ||
        instruction->Kind == KSW_RULE_INC || instruction->Kind == KSW_RULE_DEC ||
        instruction->Kind == KSW_RULE_AND || instruction->Kind == KSW_RULE_OR || instruction->Kind == KSW_RULE_XOR)
    {
        value = KswFlowUnknown();
        // INC/DEC的隐含第二操作数恒为一，自异或或与零可归零。
        if (instruction->Kind == KSW_RULE_INC || instruction->Kind == KSW_RULE_DEC) right = KswFlowConst(1);
        if (instruction->Kind == KSW_RULE_XOR && destination->Kind == KSW_RULE_OP_REG &&
            instruction->Operands[1].Kind == KSW_RULE_OP_REG && destination->Reg == instruction->Operands[1].Reg)
            value = KswFlowConst(0);
        else if (instruction->Kind == KSW_RULE_AND && KswFlowIsConst(right, 0)) value = KswFlowConst(0);
        // 双常量运算按实际指令类别求值，保持宽度内的位模式。
        else if (left.Kind == KSW_V_CONST && right.Kind == KSW_V_CONST)
        {
            int64_t number = left.Value;
            if (instruction->Kind == KSW_RULE_AND) number &= right.Value;
            else if (instruction->Kind == KSW_RULE_OR) number |= right.Value;
            else if (instruction->Kind == KSW_RULE_XOR) number ^= right.Value;
            else if (instruction->Kind == KSW_RULE_SUB || instruction->Kind == KSW_RULE_DEC) number = (int64_t)((uint64_t)number - (uint64_t)right.Value);
            else number = (int64_t)((uint64_t)number + (uint64_t)right.Value);
            value = KswFlowConst(number);
        }
        // 常量位移下的地址算术交给来源保持规则。
        else if ((instruction->Kind == KSW_RULE_ADD || instruction->Kind == KSW_RULE_SUB ||
                instruction->Kind == KSW_RULE_INC || instruction->Kind == KSW_RULE_DEC) && right.Kind == KSW_V_CONST)
            value = KswAddValue(left, (instruction->Kind == KSW_RULE_SUB || instruction->Kind == KSW_RULE_DEC) ?
                -right.Value : right.Value, model, destination->Reg);
        // 节点字段与掩码相与形成桶索引来源，掩码作为桶数证据。
        else if (instruction->Kind == KSW_RULE_AND && left.Kind == KSW_V_NODE_FIELD && right.Kind == KSW_V_CONST)
        {
            value = left;
            value.Kind = KSW_V_BUCKET;
            value.Tag = (uint16_t)right.Value;
        }
        // 参数与常量相与只保留其来源并标记已掩码。
        else if (left.Kind == KSW_V_PARAM && right.Kind == KSW_V_CONST && instruction->Kind == KSW_RULE_AND)
        {value = left; value.Aux |= 2U;}
        // 不可表示的写入触发预算失败，不返回局部证明。
        if (!KswWriteOperand(state, destination, model, value)) return -1;
        // 算术结果只有确为常量时才有可信零条件。
        state->LastZeroKnown = (uint8_t)(value.Kind == KSW_V_CONST);
        state->LastZero = (uint8_t)(value.Value == 0);
        state->LastValue = value;
        // 算术改写其它条件码和旧窗口界限/字段比较事实。
        state->CarryKnown = state->SignKnown = state->LessKnown = 0;
        state->PointerBelowLimit = 0;
        state->LastGuard = 0;
        state->LastTestReg = destination->Kind == KSW_RULE_OP_REG ? destination->Reg : 0;
        return 1;
    }
    // 跳转与NOP在本阶段不改寄存器；CFG边由固定点循环处理。
    if (instruction->Kind == KSW_RULE_JMP || instruction->Kind == KSW_RULE_JCC || instruction->Kind == KSW_RULE_NOP) return 1;
    // 未知指令按解码器写集合清空寄存器，并在写标志时清空条件事实。
    for (index = 1; index < 18; ++index)
        if ((instruction->RegWrites & (1U << index)) != 0U) state->Regs[index] = KswFlowUnknown();
    if (instruction->FlagWrites) {state->LastZeroKnown = 0; state->CarryKnown = state->SignKnown = state->LessKnown = 0;
        state->LastGuard = 0; state->LastTestReg = 0; state->LastValue = KswFlowUnknown(); state->PointerBelowLimit = 0;}
    return 1;
}

static void KswConstrainBranch(KSW_FLOW_STATE* state, int zero, const KSW_FLOW_MODEL* model)
{
    // 分支约束使用最近一次测试值，而非任意早期比较。
    KSW_FLOW_VALUE tested = state->LastValue;
    // 只有走到字段相等的零分支才累计节点身份守卫。
    if (state->LastGuard != 0U && zero) state->Guards |= state->LastGuard;
    // 卸载调用的非零返回才证明删除成功。
    if (tested.Kind == KSW_V_CALL && tested.Tag == 0 && tested.Aux == model->RemoveRva && !zero)
        state->RemoveSucceeded = 1;
    // 参数测试的两条边分别记录明确空值与明确非空值。
    if ((tested.Kind == KSW_V_PARAM || tested.Kind == KSW_V_WNDOBJ || tested.Kind == KSW_V_NULL_WNDOBJ) && tested.Tag < 16)
    {
        if (zero) state->NullArgs |= (uint16_t)(1U << tested.Tag);
        else state->NonNullArgs |= (uint16_t)(1U << tested.Tag);
    }
    // owner零分支保留实际getter来源与深度，供合流识别nullable owner。
    if (zero && (tested.Kind == KSW_V_CALL || tested.Kind == KSW_V_OWNER || tested.Kind == KSW_V_NULL_OWNER))
    {
        state->NullOwnerSource = tested.Aux;
        state->NullOwnerKind = tested.Tag;
        state->NullOwnerDepth = (uint16_t)tested.Value;
    }
    // 零分支可把刚测试的寄存器约束为常量零。
    if (zero && state->LastTestReg > 0 && state->LastTestReg < 18)
        state->Regs[state->LastTestReg] = KswFlowConst(0);
}

// 固定点合并每条真实边的入口；超过预算或跳到未解码块会整体失败。
int KswFlowRun(const KSW_RULE_IMAGE* image, const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_MODEL* model, uint32_t startIndex,
    const KSW_FLOW_STATE* initial, KSW_FLOW_ANALYSIS* analysis)
{
    // 为每条指令分配入口状态；固定点只传播真实可达边。
    KSW_FLOW_STATE* temporary;
    uint32_t round, index;
    int changed = 1;
    // 初始化分析元信息，拒绝空函数、超预算函数和非法起点。
    KswFlowZero(analysis, sizeof(*analysis));
    analysis->Image = image; analysis->Function = function; analysis->Count = function->InsnCount;
    if (function->InsnCount == 0 || function->InsnCount > KSW_FLOW_MAX_INSNS || startIndex >= function->InsnCount) return 0;
    // 状态数组与临时状态都由映像宿主分配，任一失败即完整清理。
    analysis->States = (KSW_FLOW_STATE*)image->Alloc(image->Context, sizeof(KSW_FLOW_STATE) * function->InsnCount);
    temporary = (KSW_FLOW_STATE*)image->Alloc(image->Context, sizeof(KSW_FLOW_STATE));
    if (analysis->States == 0 || temporary == 0)
    {
        if (temporary) image->Free(image->Context, temporary);
        KswFlowFree(analysis);
        return 0;
    }
    // 从指定起点复制初态并标记可达。
    KswFlowZero(analysis->States, sizeof(KSW_FLOW_STATE) * function->InsnCount);
    KswFlowCopy(&analysis->States[startIndex], initial, sizeof(*initial));
    analysis->States[startIndex].Seen = 1;
    // 最多迭代64轮直到状态不再变化，超限视为预算失败。
    for (round = 0; round < 64 && changed; ++round)
    {
        changed = 0;
        for (index = 0; index < function->InsnCount; ++index)
        {
            // 只处理已可达指令，并以拷贝执行单条转移。
            const KSW_RULE_INSN* instruction = &function->Insns[index];
            int keep, destination, fallthrough;
            if (!analysis->States[index].Seen) continue;
            KswFlowCopy(temporary, &analysis->States[index], sizeof(*temporary));
            keep = KswTransfer(temporary, instruction, model);
            // 不可表示指令或资源超限立即标记整次分析失败。
            if (keep < 0) {analysis->Error = KSW_HK_RULE_BUDGET; goto done;}
            if (!keep) continue;
            // 顺序后继必须对应一条已解码指令。
            fallthrough = KswFlowIndex(function, instruction->Rva + instruction->Length);
            if (instruction->Kind == KSW_RULE_JMP || instruction->Kind == KSW_RULE_JCC)
            {
                // 跳转目标也必须落在已解码边界。
                destination = KswFlowIndex(function, instruction->Target);
                // 完整退栈后的尾跳可离开当前函数，不构造虚假块内后继。
                if (destination < 0 && instruction->Kind == KSW_RULE_JMP &&
                    temporary->Regs[KSW_REG_RSP].Kind == KSW_V_FRAME &&
                    temporary->Regs[KSW_REG_RSP].Value == 0 && instruction->Target != 0)
                    continue;
                // 其它越界跳转使整个证明失败。
                if (destination < 0) {analysis->Error = KSW_HK_RULE_BUDGET; goto done;}
                // 无条件跳转只合并目标入口状态。
                if (instruction->Kind == KSW_RULE_JMP)
                {
                    changed |= KswFlowJoin(&analysis->States[destination], temporary);
                    continue;
                }
                // 零条件分支按已知ZF剪枝，并对两条可行边分别施加约束。
                if (instruction->Condition == KSW_RULE_COND_Z || instruction->Condition == KSW_RULE_COND_NZ)
                {
                    int takenZero = instruction->Condition == KSW_RULE_COND_Z;
                    if (!temporary->LastZeroKnown || temporary->LastZero == takenZero)
                    {
                        KswConstrainBranch(temporary, takenZero, model);
                        changed |= KswFlowJoin(&analysis->States[destination], temporary);
                    }
                    KswFlowCopy(temporary, &analysis->States[index], sizeof(*temporary));
                    if (!temporary->LastZeroKnown || temporary->LastZero != takenZero)
                    {
                        KswConstrainBranch(temporary, !takenZero, model);
                        if (fallthrough < 0) {analysis->Error = KSW_HK_RULE_BUDGET; goto done;}
                        changed |= KswFlowJoin(&analysis->States[fallthrough], temporary);
                    }
                    continue;
                }
                // 进位、符号和有符号比较仅在条件事实已知时剪枝。
                if (instruction->Condition >= KSW_RULE_COND_C && instruction->Condition <= KSW_RULE_COND_GE)
                {
                    int known = 0, taken = 0;
                    // 对象界限比较的进位分支区分空值与有效对象。
                    if (temporary->PointerBelowLimit &&
                        (instruction->Condition == KSW_RULE_COND_C || instruction->Condition == KSW_RULE_COND_NC))
                    {
                        int takenNull = instruction->Condition == KSW_RULE_COND_C;
                        KswConstrainBranch(temporary, takenNull, model);
                        changed |= KswFlowJoin(&analysis->States[destination], temporary);
                        KswFlowCopy(temporary, &analysis->States[index], sizeof(*temporary));
                        KswConstrainBranch(temporary, !takenNull, model);
                        if (fallthrough < 0) {analysis->Error = KSW_HK_RULE_BUDGET; goto done;}
                        changed |= KswFlowJoin(&analysis->States[fallthrough], temporary);
                        continue;
                    }
                    // 分别把已知进位、符号或有符号小于映射到跳转条件。
                    if (instruction->Condition == KSW_RULE_COND_C || instruction->Condition == KSW_RULE_COND_NC)
                    {known = temporary->CarryKnown; taken = temporary->Carry == (instruction->Condition == KSW_RULE_COND_C);}
                    if (instruction->Condition == KSW_RULE_COND_S || instruction->Condition == KSW_RULE_COND_NS)
                    {known = temporary->SignKnown; taken = temporary->Sign == (instruction->Condition == KSW_RULE_COND_S);}
                    if (instruction->Condition == KSW_RULE_COND_L || instruction->Condition == KSW_RULE_COND_GE)
                    {known = temporary->LessKnown; taken = temporary->Less == (instruction->Condition == KSW_RULE_COND_L);}
                    // 条件已确定时只合并实际可达的一条边。
                    if (known)
                    {
                        int target = taken ? destination : fallthrough;
                        if (target < 0) {analysis->Error = KSW_HK_RULE_BUDGET; goto done;}
                        changed |= KswFlowJoin(&analysis->States[target], temporary);
                        continue;
                    }
                }
                // 条件未知时两条边都必须进入固定点合流。
                changed |= KswFlowJoin(&analysis->States[destination], temporary);
            }
            // 不存在有效顺序后继时不得接受当前路径。
            if (fallthrough < 0) {analysis->Error = KSW_HK_RULE_BUDGET; goto done;}
            changed |= KswFlowJoin(&analysis->States[fallthrough], temporary);
        }
    }
    // 尚未收敛即属于预算失败；结束时释放临时状态。
    if (changed) analysis->Error = KSW_HK_RULE_BUDGET;
done:
    image->Free(image->Context, temporary);
    return analysis->Error == 0;
}

void KswFlowFree(KSW_FLOW_ANALYSIS* analysis)
{
    // 状态数组由创建它的宿主释放，防止跨映像释放。
    if (analysis->States && analysis->Image) analysis->Image->Free(analysis->Image->Context, analysis->States);
    analysis->States = 0;
}
