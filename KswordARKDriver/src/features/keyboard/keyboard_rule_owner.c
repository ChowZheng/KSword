#include "keyboard_rule_proof.h"

// 透明包装器只可写自己的栈帧；对象或全局写入会破坏来源证明。
static int KswOwnerSafeInstruction(const KSW_FLOW_STATE* state,
    const KSW_RULE_INSN* instruction, const KSW_FLOW_MODEL* model)
{
    uint32_t index; // 遍历完整可见操作数，防止把隐含的外部写入归为透明包装。
    // 未识别指令可能有隐式内存写，不能凭缺少显式操作数认定透明。
    if (instruction->Kind == KSW_RULE_OTHER) return 0;
    // 控制流与只读比较可继续验证；其它指令还需逐项排除对对象/全局的写入。
    if (instruction->Kind == KSW_RULE_CALL || instruction->Kind == KSW_RULE_JMP ||
        instruction->Kind == KSW_RULE_JCC || instruction->Kind == KSW_RULE_CMP ||
        instruction->Kind == KSW_RULE_TEST || instruction->Kind == KSW_RULE_PUSH ||
        instruction->Kind == KSW_RULE_LEA || instruction->Kind == KSW_RULE_NOP ||
        instruction->Kind == KSW_RULE_RET) return 1;
    for (index = 0; index < instruction->OperandCount; ++index) // 所有可能写入的内存操作数都要检查地址来源。
    {
        const KSW_RULE_OPERAND* operand = &instruction->Operands[index]; // 保留当前解码操作数的写入角色。
        KSW_FLOW_VALUE address; // 只读计算目标地址的抽象来源。
        if (operand->Kind != KSW_RULE_OP_MEM ||
            (index != 0 && instruction->Kind != KSW_RULE_OTHER)) continue;
        address = KswFlowAddress(state, operand, model); // 栈帧之外的写入会破坏透明性。
        if (address.Kind != KSW_V_FRAME) return 0;
    }
    return 1;
}

static int KswOwnerValue(KSW_FLOW_VALUE value)
{
    // 只接收完整指针、真实来源及有界解引用层数；整数零不能冒充owner。
    return value.Width >= 8 && value.Aux != 0 && value.Value >= 0 && value.Value <= 4 &&
        (value.Kind == KSW_V_CALL || value.Kind == KSW_V_OWNER ||
            value.Kind == KSW_V_NULL_OWNER) && value.Tag <= 2;
}

static int KswOwnerAddAlias(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    KSW_FLOW_VALUE canonical)
{
    uint32_t index; // 同一包装入口只允许一个确定的规范来源。
    for (index = 0; index < context->Model.OwnerAliasCount; ++index)
    {
        const KSW_FLOW_OWNER_ALIAS* alias = &context->Model.OwnerAliases[index]; // 比对已记录的入口映射。
        if (alias->EntryRva != entry) continue;
        return alias->CanonicalRva == canonical.Aux && alias->Kind == canonical.Tag &&
            alias->Depth == (uint16_t)canonical.Value;
    }
    if (context->Model.OwnerAliasCount >= 8)
    {context->Error = KSW_HK_RULE_BUDGET; return 0;}
    // 四元组一次写入模型，后续构造器和卸载都使用该规范化来源。
    context->Model.OwnerAliases[context->Model.OwnerAliasCount].EntryRva = entry;
    context->Model.OwnerAliases[context->Model.OwnerAliasCount].CanonicalRva = canonical.Aux;
    context->Model.OwnerAliases[context->Model.OwnerAliasCount].Kind = canonical.Tag;
    context->Model.OwnerAliases[context->Model.OwnerAliasCount].Depth = (uint16_t)canonical.Value;
    ++context->Model.OwnerAliasCount; // 只有完整映射写入后才发布计数。
    return 1;
}

// 每层独立解码并验证全部正常返回；递归仅沿实际返回来源的direct callee前进。
static int KswOwnerResolve(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    uint32_t level, uint32_t path[4], KSW_FLOW_VALUE* output)
{
    KSW_RULE_FUNCTION function; // 当前包装器的实际pdata及指令边界。
    KSW_FLOW_ANALYSIS analysis; // 全可达路径的抽象状态。
    KSW_FLOW_MODEL model; // 清除旧别名后的独立函数模型。
    KSW_FLOW_STATE* initial = 0; // 有界宿主分配的入口状态，避免大栈对象。
    KSW_FLOW_VALUE returned = KswFlowUnknown(), callee; // 汇总本层返回与下一层真实来源。
    uint32_t index, calls = 0, returns = 0, callTarget = 0; // 调用和返回均要求唯一、完整。
    uint8_t callTag = 0, nullable = 0; // 区分直接callee和IAT，并保留空分支。
    int success = 0; // 仅在全部证明和别名登记完成后返回成功。
    if (level >= 4 || entry == 0 || !output)
    {context->Error = KSW_HK_RULE_BUDGET; return 0;}
    for (index = 0; index < level; ++index) if (path[index] == entry) return 0; // 拒绝递归环。
    for (index = 0; index < context->Model.OwnerAliasCount; ++index)
    {
        const KSW_FLOW_OWNER_ALIAS* alias = &context->Model.OwnerAliases[index]; // 已证明别名可直接复用。
        if (alias->EntryRva != entry) continue;
        *output = KswFlowConst(alias->Depth); // 用保存的间接层数重建抽象返回值。
        output->Kind = alias->Depth ? KSW_V_OWNER : KSW_V_CALL;
        output->Aux = alias->CanonicalRva;
        output->Tag = alias->Kind;
        return 1;
    }
    path[level] = entry; // 当前入口加入递归路径，供下一层检测环。
    KswFlowZero(&function, sizeof(function)); // 失败清理时不会误释放未加载指令。
    KswFlowZero(&analysis, sizeof(analysis));
    if (!KswHkLoad(context, entry, &function)) return 0;
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial)); // 大入口状态放宿主池。
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial); // 包装器不借用调用方的旧寄存器事实。
    model = context->Model; // 复制实际PE导入槽作为本层语义锚点。
    model.Mode = 0;
    model.OwnerAliasCount = 0; // 先看真实callee，再递归归一；不能让旧alias掩盖包装器中的额外调用。
    if (!KswFlowRun(context->Full, &function, &model, 0, initial, &analysis)) // 分析失败不得发布半份别名。
    {if (analysis.Error) context->Error = analysis.Error; goto done;}
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* instruction = &function.Insns[index]; // 当前已解码的真实指令。
        const KSW_FLOW_STATE* state = &analysis.States[index]; // 只采信入口可达路径。
        KSW_FLOW_VALUE value; // 记录返回时的安装线程来源。
        if (!state->Seen) continue;
        if (!KswOwnerSafeInstruction(state, instruction, &model)) goto done; // 发现外部写入即拒绝透明性。
        if (instruction->Kind == KSW_RULE_CALL &&
            !(model.BugcheckSlot != 0 && instruction->Target == model.BugcheckSlot))
        {
            if (instruction->Target == 0 || ++calls != 1) goto done; // 多次正常调用无法归结到唯一来源。
            callTarget = instruction->Target; // 精确绑定当前直接callee或导入槽。
            callTag = (uint8_t)(KswHkCallDirect(instruction) ? 0 : 1);
        }
        if (instruction->Kind != KSW_RULE_RET) continue;
        value = state->Regs[KSW_REG_RAX]; // 每条RET都检查同一规范owner。
        if (!KswOwnerValue(value)) goto done;
        if (returns++ == 0) returned = value; // 首个返回建立本层候选来源。
        else if (returned.Aux != value.Aux || returned.Tag != value.Tag ||
            returned.Value != value.Value) goto done;
        if (value.Kind == KSW_V_NULL_OWNER) nullable = 1; // 空分支信息不可在下一层归一时丢失。
    }
    if (returns == 0 || calls > 1) goto done;
    if (calls == 0)
    {
        if (returned.Tag != 2) goto done; // 只接受实际gpti数据读作零调用叶子。
        callee = returned; // gpti数据导入本身就是零调用的叶来源。
    }
    else
    {
        if (returned.Aux != callTarget || returned.Tag != callTag) goto done;
        if (callTag == 0) // 只有实际direct callee可继续解码一层。
        {
            if (!KswOwnerResolve(context, callTarget, level + 1, path, &callee)) goto done;
        }
        else
        {
            callee = KswFlowConst(0); // IAT槽是递归终点，不跳到未映射外部模块。
            callee.Kind = KSW_V_CALL;
            callee.Width = 8;
            callee.Aux = callTarget;
            callee.Tag = 1;
        }
    }
    if (!KswOwnerValue(callee)) goto done;
    if (calls != 0)
    {
        if (returned.Value > 4 - callee.Value) goto done; // 解引用深度受统一预算约束。
        callee.Value += returned.Value;
    }
    callee.Kind = nullable || callee.Kind == KSW_V_NULL_OWNER ? KSW_V_NULL_OWNER : // 包装器保留真实空返回路径。
        (callee.Value ? KSW_V_OWNER : KSW_V_CALL);
    if (!KswOwnerAddAlias(context, entry, callee)) goto done;
    *output = callee; // 证明完成后才对调用方发布规范值。
    success = 1;
done: // 成功、歧义与资源失败走同一释放路径。
    KswFlowFree(&analysis);
    KswRuleFreeFunction(context->Full, &function);
    if (initial) context->Full->Free(context->Full->Context, initial);
    return success;
}

int KswHkCanonicalOwner(KSW_HK_RULE_CONTEXT* context, KSW_FLOW_VALUE input,
    KSW_FLOW_VALUE* output)
{
    KSW_FLOW_VALUE canonical; // 输出的共同底层getter与解引用层数。
    uint32_t path[4] = {0}; // 限长递归路径用于识别自环。
    uint32_t savedAliases; // 失败时撤销本次探索的别名登记。
    if (output) *output = KswFlowUnknown(); // 失败路径不残留旧owner来源。
    if (!context || !context->Full || !context->Full->Alloc || !context->Full->Free ||
        !output || !KswOwnerValue(input)) return 0;
    if (input.Tag == 1 || input.Tag == 2) // 已是导入槽或数据源时无需递归包装。
    {*output = input; return 1;}
    if (input.Tag != 0) return 0;
    savedAliases = context->Model.OwnerAliasCount; // 递归失败不污染后续候选。
    if (!KswOwnerResolve(context, input.Aux, 0, path, &canonical) ||
        input.Value > 4 - canonical.Value)
    {context->Model.OwnerAliasCount = savedAliases; return 0;}
    canonical.Value += input.Value; // 合并外层表达式新增的真实解引用次数。
    canonical.Kind = input.Kind == KSW_V_NULL_OWNER || canonical.Kind == KSW_V_NULL_OWNER ?
        KSW_V_NULL_OWNER : (canonical.Value ? KSW_V_OWNER : KSW_V_CALL);
    *output = canonical; // 只有完全同源的包装器才形成可复用身份。
    return 1;
}
