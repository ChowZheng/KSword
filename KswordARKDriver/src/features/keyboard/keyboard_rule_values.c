#include "keyboard_rule_flow.h"

// 共用纯C值运算，不依赖宿主CRT或任何运行时内核对象。
void KswFlowZero(void* destination, size_t size)
{
    // 把目标内存视为字节序列，以有界循环清零。
    uint8_t* bytes = (uint8_t*)destination;
    size_t index;
    for (index = 0; index < size; ++index) bytes[index] = 0;
}

// 逐字节拷贝给定长度，避免引入宿主CRT依赖。
void KswFlowCopy(void* destination, const void* source, size_t size)
{
    uint8_t* output = (uint8_t*)destination;
    const uint8_t* input = (const uint8_t*)source;
    size_t index;
    for (index = 0; index < size; ++index) output[index] = input[index];
}

KSW_FLOW_VALUE KswFlowUnknown(void)
{
    // 全零结构即未知来源，不能作为任何已证明值。
    KSW_FLOW_VALUE value = {0};
    return value;
}

KSW_FLOW_VALUE KswFlowConst(int64_t number)
{
    // 常量值以完整八字节宽度保存，供后续精确条件计算。
    KSW_FLOW_VALUE value = {0};
    value.Kind = KSW_V_CONST;
    value.Width = 8;
    value.Value = number;
    return value;
}

int KswFlowIsConst(KSW_FLOW_VALUE value, int64_t number)
{
    // 只有类别和值同时匹配才承认指定常量。
    return value.Kind == KSW_V_CONST && value.Value == number;
}

static int KswValueSame(KSW_FLOW_VALUE left, KSW_FLOW_VALUE right)
{
    // 合流前比较类别、宽度、标签、辅助来源和数值全部字段。
    return left.Kind == right.Kind && left.Width == right.Width &&
        left.Tag == right.Tag && left.Aux == right.Aux && left.Source == right.Source && left.Value == right.Value;
}

// 零值只有在相应参数的空分支上，才能与其窗口句柄合为nullable来源。
static KSW_FLOW_VALUE KswValueJoin(KSW_FLOW_VALUE left, KSW_FLOW_VALUE right,
    uint16_t leftNull, uint16_t rightNull)
{
    // 合流先保留两条路径完全一致的抽象值。
    KSW_FLOW_VALUE result;
    if (KswValueSame(left, right)) return left;
    // 窗口对象与零合流必须由对应参数的空分支事实授权。
    if (KswFlowIsConst(left, 0) && (right.Kind == KSW_V_WNDOBJ || right.Kind == KSW_V_NULL_WNDOBJ) && right.Tag < 16 &&
        (leftNull & (1U << right.Tag)) != 0U) {result = right; result.Kind = KSW_V_NULL_WNDOBJ; return result;}
    // 对调左右路径时同样核验对应空分支位。
    if (KswFlowIsConst(right, 0) && (left.Kind == KSW_V_WNDOBJ || left.Kind == KSW_V_NULL_WNDOBJ) && left.Tag < 16 &&
        (rightNull & (1U << left.Tag)) != 0U) {result = left; result.Kind = KSW_V_NULL_WNDOBJ; return result;}
    // 同一入口窗口对象的两种可空表示可合成可空对象。
    if ((left.Kind == KSW_V_WNDOBJ || left.Kind == KSW_V_NULL_WNDOBJ) &&
        (right.Kind == KSW_V_WNDOBJ || right.Kind == KSW_V_NULL_WNDOBJ) && left.Tag == right.Tag)
    {result = left; result.Kind = KSW_V_NULL_WNDOBJ; return result;}
    // 窗口字段与零合流还必须保持相同参数标签。
    if (left.Kind == KSW_V_CONST && left.Value == 0 &&
        (right.Kind == KSW_V_WINDOW || right.Kind == KSW_V_NULL_WINDOW) &&
        right.Tag < 16 && (leftNull & (1U << right.Tag)) != 0U)
    {
        result = right;
        result.Kind = KSW_V_NULL_WINDOW;
        return result;
    }
    // 对调窗口字段的左右路径时复用相同空分支约束。
    if (right.Kind == KSW_V_CONST && right.Value == 0 &&
        (left.Kind == KSW_V_WINDOW || left.Kind == KSW_V_NULL_WINDOW) &&
        left.Tag < 16 && (rightNull & (1U << left.Tag)) != 0U)
    {
        result = left;
        result.Kind = KSW_V_NULL_WINDOW;
        return result;
    }
    // 两个同源同字段的窗口值合流仍保留可空来源。
    if ((left.Kind == KSW_V_WINDOW || left.Kind == KSW_V_NULL_WINDOW) &&
        (right.Kind == KSW_V_WINDOW || right.Kind == KSW_V_NULL_WINDOW) &&
        left.Tag == right.Tag && left.Aux == right.Aux)
    {
        result = left;
        result.Kind = KSW_V_NULL_WINDOW;
        return result;
    }
    // owner值合流只有来源、种类和解引用深度都一致才保留可空性。
    if ((left.Kind == KSW_V_OWNER || left.Kind == KSW_V_NULL_OWNER || left.Kind == KSW_V_CALL) &&
        (right.Kind == KSW_V_OWNER || right.Kind == KSW_V_NULL_OWNER || right.Kind == KSW_V_CALL) &&
        left.Tag == right.Tag && left.Aux == right.Aux && left.Value == right.Value &&
        (left.Kind == KSW_V_NULL_OWNER || right.Kind == KSW_V_NULL_OWNER))
    {result = left; result.Kind = KSW_V_NULL_OWNER; return result;}
    // 桶槽地址与相同节点next槽都只表示指向当前链元素的链接位置。
    if ((left.Kind == KSW_V_LINK || left.Kind == KSW_V_TABLE_SLOT) &&
        (right.Kind == KSW_V_LINK || right.Kind == KSW_V_TABLE_SLOT) && left.Tag == right.Tag && left.Aux == right.Aux && left.Source == right.Source &&
        (left.Kind != KSW_V_TABLE_SLOT || right.Kind != KSW_V_TABLE_SLOT || left.Value == right.Value))
    {
        result = left;
        result.Kind = KSW_V_LINK;
        result.Value = 0;
        return result;
    }
    // 其它来源不一致的路径退化为未知，不能产生虚假证明。
    return KswFlowUnknown();
}

static const KSW_FLOW_CELL* KswFindCell(const KSW_FLOW_CELL* cells,
    uint32_t count, int32_t offset, uint8_t width)
{
    // 仅匹配正在使用、偏移相同且覆盖读取宽度的完整单元。
    uint32_t index;
    for (index = 0; index < count; ++index)
        if (cells[index].Used && cells[index].Offset == offset &&
            cells[index].Width >= width) return &cells[index];
    return 0;
}

// 部分重写必须使相交旧槽失效，不能拿旧的完整指针覆盖新字节。
int KswFlowStore(KSW_FLOW_CELL* cells, uint32_t count, int32_t offset,
    uint8_t width, KSW_FLOW_VALUE value)
{
    // 预留空槽索引；遍历所有旧单元以处理写入重叠。
    uint32_t index, freeIndex = count;
    for (index = 0; index < count; ++index)
    {
        // 新写入与旧单元相交时使旧完整值失效。
        if (cells[index].Used && (int64_t)cells[index].Offset < (int64_t)offset + width &&
            (int64_t)offset < (int64_t)cells[index].Offset + cells[index].Width)
            cells[index].Used = 0;
        // 记录首个可复用槽，容量不足则拒绝这次写入。
        if (!cells[index].Used && freeIndex == count) freeIndex = index;
    }
    if (freeIndex == count) return 0;
    // 新单元同时保存占用标志、偏移、宽度和完整来源。
    cells[freeIndex].Used = 1;
    cells[freeIndex].Offset = offset;
    cells[freeIndex].Width = width;
    cells[freeIndex].V = value;
    return 1;
}

void KswFlowSetReg(KSW_FLOW_STATE* state, uint8_t reg, uint8_t width, KSW_FLOW_VALUE value)
{
    // 非法寄存器编号不能污染抽象寄存器状态。
    if (reg == 0 || reg >= 18) return;
    // 部分寄存器写不授权拼接的指针；32位写只保留明确常量或数值参数来源。
    if (width < 8 && value.Kind != KSW_V_CONST && value.Kind != KSW_V_PARAM &&
        value.Kind != KSW_V_NODE_FIELD && value.Kind != KSW_V_BUCKET && value.Kind != KSW_V_LOOP_INDEX && value.Kind != KSW_V_CHECKED_BOOL)
        value = KswFlowUnknown();
    // 窄常量写入按实际位宽截断高位。
    if (value.Kind == KSW_V_CONST && width > 0 && width < 8)
        value.Value = (int64_t)((uint64_t)value.Value & ((UINT64_C(1) << (width * 8U)) - 1U));
    // 只记录本次写入的有效低位，读取更宽寄存器时不能猜测未写的高位。
    if (value.Kind != KSW_V_UNKNOWN)
    {
        // 全宽MOV不能洗白一个已被32位写截断的指针参数。
        if (value.Kind == KSW_V_PARAM && value.Width != 0 && width > value.Width) width = value.Width;
        value.Width = width;
    }
    // 把已限定有效宽度的值写入对应寄存器族。
    state->Regs[reg] = value;
}

void KswFlowInit(KSW_FLOW_STATE* state)
{
    // 初态保留四个寄存器参数、帧指针和两个栈上传入参数。
    KSW_FLOW_VALUE value;
    uint32_t index;
    const uint8_t registers[4] = {KSW_REG_RCX, KSW_REG_RDX, KSW_REG_R8, KSW_REG_R9};
    // 先清空全部状态，再标记入口可达。
    KswFlowZero(state, sizeof(*state));
    state->Seen = 1;
    // RCX、RDX、R8、R9依次标记为完整八字节参数1至4。
    for (index = 0; index < 4; ++index)
    {
        value = KswFlowUnknown();
        value.Kind = KSW_V_PARAM;
        value.Width = 8;
        value.Tag = (uint16_t)(index + 1U);
        state->Regs[registers[index]] = value;
    }
    // RSP从相对帧偏移零开始，便于追踪压栈和局部变量。
    value = KswFlowConst(0);
    value.Kind = KSW_V_FRAME;
    state->Regs[KSW_REG_RSP] = value;
    // 第五、第六参数存放在x64调用约定的栈槽0x28和0x30。
    for (index = 5; index <= 6; ++index)
    {
        value.Kind = KSW_V_PARAM;
        value.Tag = (uint16_t)index;
        value.Value = 0;
        (void)KswFlowStore(state->Stack, KSW_FLOW_STACK_CELLS,
            (int32_t)(0x28U + (index - 5U) * 8U), 8, value);
    }
}

KSW_FLOW_VALUE KswFlowAddress(const KSW_FLOW_STATE* state,
    const KSW_RULE_OPERAND* operand, const KSW_FLOW_MODEL* model)
{
    // 地址计算只接受内存操作数，RIP相对地址保留全局来源。
    KSW_FLOW_VALUE base, index;
    if (operand->Kind != KSW_RULE_OP_MEM) return KswFlowUnknown();
    if (operand->Base == KSW_REG_RIP)
    {
        base = KswFlowConst(operand->Disp);
        base.Kind = KSW_V_GLOBAL;
        return base;
    }
    // 基址和索引都从当前寄存器状态读取，避免猜测动态地址。
    base = state->Regs[operand->Base < 18 ? operand->Base : 0];
    // 带索引的内存地址仅识别已证明的桶索引或桶循环。
    if (operand->Index != 0)
    {
        index = state->Regs[operand->Index < 18 ? operand->Index : 0];
        // 会话基址加桶索引乘八形成具体桶头地址。
        if (base.Kind == KSW_V_SESSION && index.Kind == KSW_V_BUCKET && operand->Scale == 8)
        {
            base.Kind = KSW_V_TABLE_HEAD;
            base.Aux = index.Aux;
            base.Tag = index.Tag;
            base.Value += operand->Disp;
            return base;
        }
        // 会话基址加循环索引乘八且偏移匹配表时形成表槽。
        if (base.Kind == KSW_V_SESSION && index.Kind == KSW_V_LOOP_INDEX && operand->Scale == 8 &&
            base.Value + operand->Disp == model->TableOffset)
        {base.Kind = KSW_V_TABLE_SLOT; base.Aux = model->TableOffset; base.Value += operand->Disp; return base;}
        // 其它索引形式退化为未知地址。
        return KswFlowUnknown();
    }
    // 零指针基址不授予可读取地址身份。
    if (base.Kind == KSW_V_CONST && base.Value == 0) return KswFlowUnknown();
    // 帧、会话、节点和全局地址可在来源不变的前提下加位移。
    if (base.Kind == KSW_V_FRAME || base.Kind == KSW_V_SESSION ||
        base.Kind == KSW_V_NODE || base.Kind == KSW_V_GLOBAL)
    {
        base.Value += operand->Disp;
        // 删除模式下节点next偏移改记为链接位置。
        if (base.Kind == KSW_V_NODE && model->Mode == 2U &&
            base.Value == (int64_t)model->NextOffset) base.Kind = KSW_V_LINK;
        return base;
    }
    // 链接位置只能用零位移继续引用。
    if (base.Kind == KSW_V_TABLE_SLOT || base.Kind == KSW_V_LINK)
        return operand->Disp == 0 ? base : KswFlowUnknown();
    // 窗口对象地址仅推导字段来源，不访问真实对象内存。
    // 窗口对象内只记录字段来源，绝不实际读取这些假想地址。
    if (base.Kind == KSW_V_PARAM || base.Kind == KSW_V_WNDOBJ || base.Kind == KSW_V_NULL_WNDOBJ)
    {
        // 截断对象或未证明非空的可空对象不能读取字段。
        if (base.Width < 8 || (base.Kind == KSW_V_NULL_WNDOBJ &&
            (base.Tag >= 16 || (state->NonNullArgs & (1U << base.Tag)) == 0U))) return KswFlowUnknown();
        // 把合法对象字段记录为窗口来源及字段偏移。
        base.Kind = KSW_V_WINDOW;
        base.Aux = (uint32_t)operand->Disp;
        base.Value = 0;
        return base;
    }
    // 实际owner导入数据地址支持一层解引用。
    if (base.Kind == KSW_V_OWNER_ADDRESS && operand->Disp == 0) return base;
    // owner getter返回的值每解引用一次递增来源深度。
    if ((base.Kind == KSW_V_CALL || base.Kind == KSW_V_OWNER) && operand->Disp == 0)
    {base.Kind = KSW_V_OWNER; ++base.Value; return base;}
    // 其它基址类别没有可证明的内存地址。
    return KswFlowUnknown();
}

KSW_FLOW_VALUE KswFlowOperand(const KSW_FLOW_STATE* state,
    const KSW_RULE_OPERAND* operand, const KSW_FLOW_MODEL* model)
{
    // 先按操作数类型区分寄存器、立即数和内存读取。
    KSW_FLOW_VALUE value;
    const KSW_FLOW_CELL* cell;
    // 寄存器读取必须满足所请求的有效宽度。
    if (operand->Kind == KSW_RULE_OP_REG)
    {
        value = operand->Reg < 18 ? state->Regs[operand->Reg] : KswFlowUnknown();
        if (value.Width < operand->Width && value.Width < 4) return KswFlowUnknown();
        return value;
    }
    // 立即数形成精确常量；内存则先推导抽象地址。
    if (operand->Kind == KSW_RULE_OP_IMM) return KswFlowConst(operand->Imm);
    value = KswFlowAddress(state, operand, model);
    // 实际导入的gptiCurrent槽读取得到owner指针地址。
    if (value.Kind == KSW_V_GLOBAL && value.Value == model->OwnerDataSlot && model->OwnerDataSlot != 0)
    {value.Kind = KSW_V_OWNER_ADDRESS; value.Aux = model->OwnerDataSlot; value.Tag = 2; value.Value = 0; return value;}
    // owner地址解引用返回完整owner值。
    if (value.Kind == KSW_V_OWNER_ADDRESS)
    {value.Kind = KSW_V_OWNER; value.Width = 8; return value;}
    if (value.Kind == KSW_V_OWNER) return value;
    // 帧槽和普通模式节点字段只从此前完整写入的单元读取。
    if (value.Kind == KSW_V_FRAME || (value.Kind == KSW_V_NODE && model->Mode == 0U))
    {
        cell = KswFindCell(value.Kind == KSW_V_FRAME ? state->Stack : state->Node,
            value.Kind == KSW_V_FRAME ? KSW_FLOW_STACK_CELLS : KSW_FLOW_NODE_CELLS,
            (int32_t)value.Value, operand->Width);
        return cell ? cell->V : KswFlowUnknown();
    }
    // 其它节点读取转成带偏移与读取宽度的字段来源。
    if (value.Kind == KSW_V_NODE)
    {
        value.Kind = KSW_V_NODE_FIELD;
        value.Aux = (uint32_t)value.Value;
        value.Width = operand->Width;
    }
    // 会话字段读取保留会话内偏移，供表和缓存核验。
    else if (value.Kind == KSW_V_SESSION)
    {
        value.Kind = KSW_V_SESSION_FIELD;
        value.Aux = (uint32_t)value.Value;
    }
    // 表槽或链槽读取产生当前链节点来源。
    else if (value.Kind == KSW_V_TABLE_SLOT || value.Kind == KSW_V_LINK)
    {
        value.Kind = KSW_V_NODE;
        value.Value = 0;
    }
    // 返回可证明的字段来源或未知值，不读取输入映像外数据。
    return value;
}

static int KswJoinCells(KSW_FLOW_CELL* left, const KSW_FLOW_CELL* right,
    uint32_t count, uint16_t leftNull, uint16_t rightNull)
{
    // 同一槽只有两条路径均完整写入相同宽度才可在合流后保留。
    uint32_t index;
    int changed = 0;
    for (index = 0; index < count; ++index)
    {
        // 逐槽检查左状态中已有的有效单元。
        const KSW_FLOW_CELL* other;
        KSW_FLOW_VALUE joined;
        if (!left[index].Used) continue;
        // 右路径缺失或宽度不同则移除该槽。
        other = KswFindCell(right, count, left[index].Offset, left[index].Width);
        if (other == 0 || other->Width != left[index].Width)
        {
            left[index].Used = 0;
            changed = 1;
            continue;
        }
        // 来源按可空规则合流，变化时标记需要继续迭代。
        joined = KswValueJoin(left[index].V, other->V, leftNull, rightNull);
        if (!KswValueSame(joined, left[index].V)) {left[index].V = joined; changed = 1;}
    }
    return changed;
}

// 仅保留所有路径共有的值和守卫；第一次到达的路径不是最终证明。
int KswFlowJoin(KSW_FLOW_STATE* destination, const KSW_FLOW_STATE* source)
{
    // 合流前保存左路径空参事实，并计算零条件分支的共同守卫。
    uint32_t index;
    uint16_t leftNull = destination->NullArgs;
    // 已知ZF恒假的路径对ZF真蕴含守卫不施加限制。
    uint8_t zeroGuard = (uint8_t)((destination->LastZeroKnown && !destination->LastZero ? 7U : destination->LastGuard) &
        (source->LastZeroKnown && !source->LastZero ? 7U : source->LastGuard));
    int changed = 0;
    // 目标第一次可达时直接复制源状态。
    if (!destination->Seen) {KswFlowCopy(destination, source, sizeof(*source)); return 1;}
    // 逐个寄存器合流；所有分支都认同的来源才可保留。
    for (index = 0; index < 18; ++index)
    {
        KSW_FLOW_VALUE joined = KswValueJoin(destination->Regs[index], source->Regs[index], leftNull, source->NullArgs);
        KSW_FLOW_VALUE a = destination->Regs[index], b = source->Regs[index];
        // 零与owner来源合流需匹配实际getter种类、地址及解引用深度。
        if (KswFlowIsConst(a, 0) && (b.Kind == KSW_V_OWNER || b.Kind == KSW_V_CALL || b.Kind == KSW_V_NULL_OWNER) &&
            destination->NullOwnerSource == b.Aux && destination->NullOwnerKind == b.Tag &&
            destination->NullOwnerDepth <= b.Value)
        {joined = b; joined.Kind = KSW_V_NULL_OWNER;}
        if (KswFlowIsConst(b, 0) && (a.Kind == KSW_V_OWNER || a.Kind == KSW_V_CALL || a.Kind == KSW_V_NULL_OWNER) &&
            source->NullOwnerSource == a.Aux && source->NullOwnerKind == a.Tag && source->NullOwnerDepth <= a.Value)
        {joined = a; joined.Kind = KSW_V_NULL_OWNER;}
        // RAX可把已核验成功值与零返回合成为已检查布尔来源。
        if (index == KSW_REG_RAX)
        {
            // 成功路径须同时具有释放和摘链守卫8与16，即掩码24。
            int safeA = a.Kind == KSW_V_CHECKED_BOOL || (KswFlowIsConst(a, 1) && (destination->Guards & 24U) == 24U);
            int safeB = b.Kind == KSW_V_CHECKED_BOOL || (KswFlowIsConst(b, 1) && (source->Guards & 24U) == 24U);
            if ((safeA && (safeB || KswFlowIsConst(b, 0))) || (safeB && KswFlowIsConst(a, 0)))
            {joined = KswFlowConst(1); joined.Kind = KSW_V_CHECKED_BOOL; joined.Width = 1; joined.Tag = 24;}
        }
        // 更新寄存器后继续合并路径共有的事实。
        if (!KswValueSame(joined, destination->Regs[index])) {destination->Regs[index] = joined; changed = 1;}
    }
// 各路径的临界区、成功、存活和守卫位按位与保留。
#define KSW_JOIN_AND(field) do {uint32_t merged = destination->field & source->field; if (merged != destination->field) {destination->field = (uint8_t)merged; changed = 1;}} while (0)
    KSW_JOIN_AND(Crit); KSW_JOIN_AND(RemoveSucceeded); KSW_JOIN_AND(NodeAlive); KSW_JOIN_AND(Guards); KSW_JOIN_AND(PointerBelowLimit);
#undef KSW_JOIN_AND
    // 空参及非空参约束也只保留所有路径共同拥有的位。
    if ((destination->NullArgs & source->NullArgs) != destination->NullArgs) {destination->NullArgs &= source->NullArgs; changed = 1;}
    if ((destination->NonNullArgs & source->NonNullArgs) != destination->NonNullArgs) {destination->NonNullArgs &= source->NonNullArgs; changed = 1;}
    // ZF不一致或来源未知时撤销确定性；其它条件码同理。
    if (!source->LastZeroKnown || destination->LastZero != source->LastZero)
        if (destination->LastZeroKnown) {destination->LastZeroKnown = 0; changed = 1;}
#define KSW_JOIN_FLAG(known, value) do {if ((!source->known || destination->value != source->value) && destination->known) {destination->known = 0; changed = 1;}} while (0)
    KSW_JOIN_FLAG(CarryKnown, Carry); KSW_JOIN_FLAG(SignKnown, Sign); KSW_JOIN_FLAG(LessKnown, Less);
#undef KSW_JOIN_FLAG
    // 比较的目标寄存器不一致时不可沿用旧约束。
    if (destination->LastTestReg != source->LastTestReg && destination->LastTestReg != 0) {destination->LastTestReg = 0; changed = 1;}
    // 只会走非零分支的输入不约束零分支：保留“ZF为真则字段相等”的蕴含事实。
    // 字段相等守卫只保留零条件路径共同保证的事实。
    if (destination->LastGuard != zeroGuard) {destination->LastGuard = zeroGuard; changed = 1;}
    // 缓存比较位置不一致时清除缓存证明。
    if (destination->ComparedCache != source->ComparedCache && destination->ComparedCache != 0) {destination->ComparedCache = 0; changed = 1;}
    // 空owner的getter来源不一致时清除其归一身份。
    if ((destination->NullOwnerSource != source->NullOwnerSource || destination->NullOwnerKind != source->NullOwnerKind ||
        destination->NullOwnerDepth != source->NullOwnerDepth) && destination->NullOwnerSource != 0)
    {destination->NullOwnerSource = 0; changed = 1;}
    // 最近一次被测试值以及栈槽、节点槽都必须逐路径合流。
    {
        KSW_FLOW_VALUE joined = KswValueJoin(destination->LastValue, source->LastValue, leftNull, source->NullArgs);
        if (!KswValueSame(joined, destination->LastValue)) {destination->LastValue = joined; changed = 1;}
    }
    changed |= KswJoinCells(destination->Stack, source->Stack, KSW_FLOW_STACK_CELLS, leftNull, source->NullArgs);
    changed |= KswJoinCells(destination->Node, source->Node, KSW_FLOW_NODE_CELLS, leftNull, source->NullArgs);
    // 返回状态是否有变化，供固定点算法继续迭代。
    return changed;
}
