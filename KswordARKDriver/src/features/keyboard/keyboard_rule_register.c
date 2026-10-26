#include "keyboard_rule_proof.h"

// 入口参数必须保持原始编号与零附加变换，防止截取字段冒充构造输入。
static int KswParameter(KSW_FLOW_VALUE value, uint16_t parameter)
{
    return value.Kind == KSW_V_PARAM && value.Tag == parameter && value.Aux == 0;
}

// 每个角色都必须有一个完整、在本节点范围内、来源明确的最终字段。
static int KswCaptureConstructor(const KSW_FLOW_STATE* state, uint32_t size,
    const KSW_HOTKEY_RULE_RESULT* insertion, KSW_HOTKEY_RULE_RESULT* output)
{
    // 角色位图与标志/子链计数确保每种字段恰有一个完整来源。
    uint32_t index, roles = 0, flagsCount = 0, childCount = 0;
    // 插入证明先确定同表字段，构造器只补充节点布局。
    *output = *insertion;
    output->ObjectSize = size;
    for (index = 0; index < KSW_FLOW_NODE_CELLS; ++index)
    {
        const KSW_FLOW_CELL* cell = &state->Node[index];
        KSW_FLOW_VALUE value = cell->V;
        uint32_t bit = 0;
        // 未写入、负偏移及越过实际分配大小的单元不能发布为字段。
        if (!cell->Used || cell->Offset < 0 || (uint32_t)cell->Offset + cell->Width > size) continue;
        // 安装线程是完整指针，并记录其真实调用/IAT来源及解引用深度。
        if (cell->Width == 8 && value.Width >= 8 &&
            (value.Kind == KSW_V_CALL || value.Kind == KSW_V_OWNER) && value.Aux != 0)
        {
            bit = 1;
            output->OwnerOffset = (uint32_t)cell->Offset;
            output->OwnerGetterRva = value.Aux;
            output->OwnerGetterIndirect = value.Tag;
            output->OwnerGetterDepth = (uint32_t)value.Value;
        }
        // 回调参数来自构造器固定的第二入口参数。
        else if (cell->Width == 8 && value.Width >= 8 && KswParameter(value, 2))
        {
            bit = 2;
            output->CallbackOffset = (uint32_t)cell->Offset;
        }
        // HWND表示需同时记下窗口对象内的取值字段，零偏移也合法。
        else if (cell->Width == 8 && value.Width >= 8 &&
            (value.Kind == KSW_V_WINDOW || value.Kind == KSW_V_NULL_WINDOW) && value.Tag == 1)
        {
            bit = 4;
            output->WindowOffset = (uint32_t)cell->Offset;
            output->WindowObjectHandleOffset = value.Aux;
        }
        // 直接保存tagWND指针的形态由赋值锁提供，不能混作HWND值。
        else if (cell->Width == 8 && value.Width >= 8 &&
            (value.Kind == KSW_V_WNDOBJ || value.Kind == KSW_V_NULL_WNDOBJ) && value.Tag == 1)
        {bit = 4; output->WindowOffset = (uint32_t)cell->Offset; output->WindowRepresentation = 1;}
        // 可选目标参数保留自身字段宽度，不与owner偏移复用。
        else if (cell->Width == 8 && value.Width >= 8 && KswParameter(value, 6))
        {
            bit = 8;
            output->DestinationOffset = (uint32_t)cell->Offset;
            output->DestinationWidth = cell->Width;
        }
        // 修饰键允许构造器已证实的掩码变换，但来源仍须是第四参数。
        else if (cell->Width <= 4 && value.Kind == KSW_V_PARAM && value.Tag == 4 &&
            (value.Aux == 0 || value.Aux == 2))
        {
            bit = 16;
            output->ModifiersOffset = (uint32_t)cell->Offset;
            output->ModifiersWidth = cell->Width;
        }
        // VK字段必须落在插入哈希使用的同一偏移。
        else if (cell->Width <= 4 && KswParameter(value, 5))
        {
            bit = 32;
            if ((uint32_t)cell->Offset != insertion->VkOffset) return 0;
            output->VkWidth = cell->Width;
        }
        // 注册ID仅接受来自第三参数的完整DWORD。
        else if (cell->Width == 4 && KswParameter(value, 3))
        {
            bit = 64;
            output->IdOffset = (uint32_t)cell->Offset;
            output->IdWidth = cell->Width;
        }
        // 子链头由相邻两个指向同一头地址的完整指针共同证明。
        else if (cell->Width == 8 && value.Kind == KSW_V_NODE && value.Value == cell->Offset)
        {
            uint32_t other;
            for (other = 0; other < KSW_FLOW_NODE_CELLS; ++other)
            {
                const KSW_FLOW_CELL* link = &state->Node[other];
                // 第二个链指针必须紧邻且回指同一节点内头部。
                if (link->Used && link->Width == 8 && link->Offset == cell->Offset + 8 &&
                    link->V.Kind == KSW_V_NODE && link->V.Value == cell->Offset)
                {
                    output->ChildOffset = (uint32_t)cell->Offset;
                    ++childCount;
                }
            }
        }
        // 未知或常量短字段只作为内部flags候选，不冒充业务参数。
        else if (cell->Width <= 4 && (value.Kind == KSW_V_UNKNOWN || value.Kind == KSW_V_CONST))
        {
            output->FlagsOffset = (uint32_t)cell->Offset;
            output->FlagsWidth = cell->Width;
            ++flagsCount;
        }
        // 同一语义角色若出现第二个位置，就拒绝歧义布局。
        if (bit && (roles & bit) != 0U) return 0;
        roles |= bit;
    }
    // 允许有或无目标参数两种构造形态，最终还需全字段无重叠校验。
    return (roles == 127 || roles == 119) && flagsCount == 1 && childCount == 1 && KswHkLayoutValid(output);
}

static int KswProveAllocation(KSW_HK_RULE_CONTEXT* context, const KSW_RULE_FUNCTION* function,
    const KSW_FLOW_ANALYSIS* basis, uint32_t allocation, uint32_t insert,
    const KSW_HOTKEY_RULE_RESULT* insertion, KSW_HOTKEY_RULE_RESULT* output)
{
    // 焦点分析只从实际分配调用开始，排除函数中的既有节点分支。
    KSW_FLOW_ANALYSIS focus;
    KSW_FLOW_MODEL model = context->Model;
    uint32_t index, matches = 0, caches = 0;
    KSW_FLOW_VALUE bytes = basis->States[allocation].Regs[KSW_REG_RCX];
    KswFlowZero(&focus, sizeof(focus));
    // 节点大小须是有界且按指针对齐的实际分配实参。
    if (bytes.Kind != KSW_V_CONST || bytes.Value < 32 ||
        bytes.Value > KSW_HOTKEY_RULE_MAX_NODE || (bytes.Value & 7) != 0) return 0;
    model.AllocRva = function->Insns[allocation].Rva;
    model.InsertRva = insert;
    model.Mode = 0;
    // 仅把这一分配返回值视作可追踪的新节点。
    if (!KswFlowRun(context->Full, function, &model, allocation, &basis->States[allocation], &focus)) goto done;
    // 插入调用必须可达，且当前节点仍活着并作为完整RCX实参。
    for (index = 0; index < function->InsnCount; ++index)
    {
        const KSW_RULE_INSN* call = &function->Insns[index];
        const KSW_FLOW_STATE* state = &focus.States[index];
        KSW_FLOW_VALUE node;
        if (!state->Seen || !state->NodeAlive || call->Kind != KSW_RULE_CALL ||
            !KswHkCallDirect(call) || call->Target != insert) continue;
        node = state->Regs[KSW_REG_RCX];
        if (node.Kind != KSW_V_NODE || node.Value != 0 || node.Width < 8) continue;
        // 字段必须由这条新节点路径的最终写入快照取出。
        if (!KswCaptureConstructor(state, (uint32_t)bytes.Value, insertion, output)) continue;
        {
            // 透明owner包装器按当前映像全部返回路径归一，不按调用RVA白名单猜测。
            KSW_FLOW_VALUE owner = KswFlowConst((int64_t)output->OwnerGetterDepth), canonical;
            owner.Kind = output->OwnerGetterDepth || output->OwnerGetterIndirect == 2 ? KSW_V_OWNER : KSW_V_CALL;
            owner.Aux = output->OwnerGetterRva;
            owner.Tag = (uint16_t)output->OwnerGetterIndirect;
            if (!KswHkCanonicalOwner(context, owner, &canonical)) continue;
            // 归一后的底层来源将用于卸载路径的同源比较。
            output->OwnerGetterRva = canonical.Aux;
            output->OwnerGetterIndirect = canonical.Tag;
            output->OwnerGetterDepth = (uint32_t)canonical.Value;
        }
        // 超过一个完整候选时拒绝，不能按代码顺序取第一项。
        if (matches++) goto done;
        output->RegisterRva = function->EntryRva;
        // 分配与插入调用的字节必须进入运行时失效证据。
        if (!KswHkEvidence(context, output, &function->Insns[allocation]) ||
            !KswHkEvidence(context, output, call)) {matches = 0; goto done;}
    }
done:
    // 缓存位只能从已成功初始化新节点的焦点路径继续推导。
    if (matches == 1 && focus.States)
    {
        for (index = 0; index < function->InsnCount; ++index)
        {
            const KSW_RULE_INSN* store = &function->Insns[index];
            const KSW_FLOW_STATE* state = &focus.States[index];
            KSW_FLOW_VALUE address;
            // 表已经插入且写入值抽象为零；寄存器零与立即数零等价。
            if (!state->Seen || (state->Guards & 32U) == 0 || store->OperandCount != 2 ||
                store->Operands[0].Kind != KSW_RULE_OP_MEM || store->Operands[0].Width != 8 ||
                !((store->Kind == KSW_RULE_AND || store->Kind == KSW_RULE_MOV) &&
                    KswFlowIsConst(KswFlowOperand(state, &store->Operands[1], &model), 0))) continue;
            address = KswFlowAddress(state, &store->Operands[0], &model);
            // 缓存必须位于已证表末后的有界区域，不复用表内槽位。
            if (address.Kind != KSW_V_SESSION || address.Value < (int64_t)output->TableOffset + output->BucketCount * 8U ||
                address.Value > (int64_t)output->TableOffset + output->BucketCount * 8U + 0x1000) continue;
            // 多次清零若指向不同位置则不能归为同一个缓存字段。
            if (caches && output->CacheOffset != (uint32_t)address.Value) {matches = 0; break;}
            output->CacheOffset = (uint32_t)address.Value;
            ++caches;
            // 真实零写入是后续卸载交叉验证和运行时缓存的指令证据。
            if (!KswHkEvidence(context, output, store)) {matches = 0; break;}
        }
    }
    // 释放焦点分析后，只有唯一节点及至少一个同源缓存清零才通过。
    KswFlowFree(&focus);
    return matches == 1 && caches != 0;
}

static int KswProveRegister(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    KSW_HOTKEY_RULE_RESULT* output)
{
    // 两份Result候选使用宿主堆内存，避免大型证据结构占据内核栈。
    KSW_RULE_FUNCTION function;
    KSW_FLOW_ANALYSIS basis;
    KSW_FLOW_STATE* initial;
    KSW_FLOW_MODEL model = context->Model;
    KSW_HOTKEY_RULE_RESULT* insertion = 0;
    KSW_HOTKEY_RULE_RESULT* candidate = 0;
    uint32_t callIndex, allocation, matches = 0;
    int success = 0;
    // 函数与抽象状态在任何失败分支都保持可安全释放。
    KswFlowZero(&function, sizeof(function));
    KswFlowZero(&basis, sizeof(basis));
    if (!KswHkLoad(context, entry, &function)) return 0;
    // 入口状态单独分配；不足时明确区分资源失败与无匹配。
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial));
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    insertion = KswHkNewResult(context);
    candidate = KswHkNewResult(context);
    if (!insertion || !candidate) goto release;
    // 构造器接收已验证窗口对象，后续窗口字段沿此身份传播。
    KswFlowInit(initial);
    initial->Regs[KSW_REG_RCX].Kind = KSW_V_WNDOBJ;
    model.Mode = 0;
    // 先分析全部可达分支，才枚举可能的插入和分配调用。
    if (!KswFlowRun(context->Full, &function, &model, (uint32_t)KswFlowIndex(&function, function.EntryRva), initial, &basis)) goto release;
    for (callIndex = 0; callIndex < function.InsnCount; ++callIndex)
    {
        const KSW_RULE_INSN* call = &function.Insns[callIndex];
        // 只有真实直接调用且可达的callee可能是该构造器的插入函数。
        if (call->Kind != KSW_RULE_CALL || !KswHkCallDirect(call) || !basis.States[callIndex].Seen) continue;
        KswFlowZero(insertion, sizeof(*insertion));
        // 每个候选都先独立证明节点next与表头的同桶插入。
        if (!KswHkProofInsert(context, call->Target, insertion)) continue;
        for (allocation = 0; allocation < function.InsnCount; ++allocation)
        {
            const KSW_RULE_INSN* allocate = &function.Insns[allocation];
            // 分配调用须指向实际零初始化分配器的导入槽，而非任意direct callee。
            if (!basis.States[allocation].Seen || allocate->Kind != KSW_RULE_CALL ||
                KswHkCallDirect(allocate) || allocate->Target != model.AllocSlot) continue;
            // 聚焦该分配返回值，证明字段布局、归一owner与同会话缓存清零。
            if (!KswProveAllocation(context, &function, &basis, allocation,
                    call->Target, insertion, candidate)) continue;
            // 仅唯一的分配与插入组合可以发布到调用者结果。
            if (matches++) goto release;
            *output = *candidate;
            success = 1;
        }
    }
release:
    // 所有候选和数据流均由对应映像宿主回收，不能留下部分Result引用。
    KswFlowFree(&basis);
    if (insertion) context->Full->Free(context->Full->Context, insertion);
    if (candidate) context->Full->Free(context->Full->Context, candidate);
    context->Full->Free(context->Full->Context, initial);
done:
    // 解码函数最后释放；歧义候选即使曾写出完整字段也返回失败。
    KswRuleFreeFunction(context->Full, &function);
    return success && matches == 1;
}

// 公开RegisterHotKey的稳定参数语义绑定到内部构造器，寄存器分配不作条件。
int KswHkProofRegister(KSW_HK_RULE_CONTEXT* context, KSW_HOTKEY_RULE_RESULT* output)
{
    // 公开入口只负责把稳定参数语义绑定到唯一已证明的私有构造器。
    KSW_RULE_FUNCTION function;
    KSW_FLOW_ANALYSIS analysis;
    KSW_FLOW_STATE* initial;
    KSW_HOTKEY_RULE_RESULT* candidate = 0;
    uint32_t entry, index, matches = 0;
    int success = 0;
    KswFlowZero(&function, sizeof(function));
    KswFlowZero(&analysis, sizeof(analysis));
    // 导出缺失、解码不完整时不尝试猜测私有入口。
    if (!KswRuleFindExport(context->Full, "NtUserRegisterHotKey", &entry) ||
        !KswHkLoad(context, entry, &function)) return 0;
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial));
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    // 较大的候选结果由宿主分配，完成唯一性证明前不污染输出。
    candidate = KswHkNewResult(context);
    if (!candidate) goto release;
    KswFlowInit(initial);
    // 从真实公开入口遍历控制流，保留USER临界区状态约束。
    if (!KswFlowRun(context->Full, &function, &context->Model, (uint32_t)KswFlowIndex(&function, function.EntryRva), initial, &analysis)) goto release;
    for (index = 0; index < function.InsnCount; ++index)
    {
        const KSW_RULE_INSN* call = &function.Insns[index];
        const KSW_FLOW_STATE* state = &analysis.States[index];
        KSW_FLOW_VALUE window;
        // 只有进入临界区后的直接调用可作为私有构造器候选。
        if (!state->Seen || !state->Crit || call->Kind != KSW_RULE_CALL || !KswHkCallDirect(call)) continue;
        window = KswHkCallArgument(state, 1);
        // 窗口、零保留参数、回调、ID及修饰键均须来自公开入口的既定位置。
        if (!(((window.Kind == KSW_V_WNDOBJ || window.Kind == KSW_V_NULL_WNDOBJ) && window.Tag == 1 && window.Width >= 8) ||
                (KswFlowIsConst(window, 0) && (state->NullArgs & (1U << 1)) != 0U)) ||
            !KswFlowIsConst(KswHkCallArgument(state, 2), 0) ||
            !KswParameter(KswHkCallArgument(state, 3), 2) ||
            !KswParameter(KswHkCallArgument(state, 4), 3) ||
            !KswParameter(KswHkCallArgument(state, 5), 4)) continue;
        KswFlowZero(candidate, sizeof(*candidate));
        // 私有构造器须独立证明完整布局，不能只据公开调用位置授权。
        if (!KswProveRegister(context, call->Target, candidate)) continue;
        // 多个有效callee是歧义，即使字段相同也拒绝取首个。
        if (matches++) goto release;
        *output = *candidate;
        success = KswHkEvidence(context, output, call);
    }
release:
    // 对所有失败和成功路径统一释放分析与候选内存。
    KswFlowFree(&analysis);
    if (candidate) context->Full->Free(context->Full->Context, candidate);
    context->Full->Free(context->Full->Context, initial);
done:
    KswRuleFreeFunction(context->Full, &function);
    // 返回值同时要求已保存证据和唯一候选。
    return success && matches == 1;
}
