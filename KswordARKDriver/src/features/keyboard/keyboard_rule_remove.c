#include "keyboard_rule_proof.h"

// 公开入口与私有包装器都允许已验证的窗口对象或明确的空窗口。
static int KswWindowArg(KSW_FLOW_VALUE v, const KSW_FLOW_STATE* s, uint16_t tag)
{
    if ((v.Kind == KSW_V_WNDOBJ || v.Kind == KSW_V_NULL_WNDOBJ) &&
        v.Tag == tag && v.Width >= 8) return 1;
    return KswFlowIsConst(v, 0) && tag < 16U && (s->NullArgs & (1U << tag)) != 0U;
}

static int KswParam(KSW_FLOW_VALUE v, uint16_t tag)
{
    // 参数号和未变形的Aux共同限制来源，避免显示值恰好相同造成错配。
    return v.Kind == KSW_V_PARAM && v.Tag == tag && v.Aux == 0U && v.Width >= 4;
}

// 安装线程必须与构造器记录的来源、间接层数完全一致。
static int KswOwnerArg(KSW_FLOW_VALUE v, const KSW_HOTKEY_RULE_RESULT* layout)
{
    return (v.Kind == KSW_V_CALL || v.Kind == KSW_V_OWNER ||
        v.Kind == KSW_V_NULL_OWNER) && v.Width >= 8 &&
        v.Aux == layout->OwnerGetterRva && v.Tag == layout->OwnerGetterIndirect &&
        v.Value == (int64_t)layout->OwnerGetterDepth;
}

// 输入槽必须属于同一会话哈希表，且当前节点已通过三项身份比较。
static int KswNodeSlot(const KSW_FLOW_STATE* s, const KSW_FLOW_MODEL* m,
    const KSW_HOTKEY_RULE_RESULT* layout)
{
    KSW_FLOW_VALUE v = s->Regs[KSW_REG_RCX]; // 一参helper接收的是当前链槽而非节点地址。
    if ((s->Guards & 7U) != 7U || v.Width < 8 ||
        v.Source == 0U || v.Source != m->SessionSlot) return 0;
    if (v.Kind == KSW_V_LINK) return 1; // next槽继承表节点的同一Session来源。
    return v.Kind == KSW_V_TABLE_SLOT && v.Aux == layout->TableOffset;
}

// 临时截断单条IR，证明表初始化和cmp0对最终缓存清零路径必经。
static int KswCacheDominates(KSW_HK_RULE_CONTEXT* context,
    const KSW_RULE_FUNCTION* fn, const KSW_FLOW_MODEL* model,
    const KSW_FLOW_STATE* initial, uint32_t required, uint32_t target)
{
    KSW_RULE_INSN saved; // 修改临时IR前保存原指令以便无条件恢复。
    KSW_FLOW_ANALYSIS blocked; // 截断路径的独立可达性求解结果。
    int start, yes = 0; // 只在目标确实不可达时认定为必经。
    if (required >= target || target >= fn->InsnCount) return 0;
    start = KswFlowIndex(fn, fn->EntryRva); // pdata冷片段可能排在主入口前后。
    if (start < 0) return 0;
    saved = fn->Insns[required]; // 任何失败都需恢复该证明指令。
    KswFlowZero(&blocked, sizeof(blocked));
    fn->Insns[required].Kind = KSW_RULE_RET; // 只改当前有界解码数组，不改输入PE。
    fn->Insns[required].OperandCount = 0; // 临时RET不再读取原操作数。
    if (KswFlowRun(context->Full, fn, model, (uint32_t)start, initial, &blocked))
        yes = !blocked.States[target].Seen;
    else context->Error = blocked.Error ? blocked.Error : KSW_HK_RULE_BUDGET;
    fn->Insns[required] = saved; // 调用方继续使用原始完整CFG。
    KswFlowFree(&blocked); // 释放每次必经性试探的固定点状态。
    return yes;
}

// 内联卸载的缓存写入必须出现在同一函数已证明表初始化与回边之后。
static int KswAfterLoop(const KSW_RULE_FUNCTION* fn,
    const KSW_FLOW_ANALYSIS* analysis, uint32_t anchor, uint32_t store)
{
    uint32_t i; // 确认缓存清零确实排列在已验证表循环的后向边之后。
    if (anchor >= store) return 0;
    for (i = anchor + 1U; i < store; ++i)
        if (analysis->States[i].Seen && fn->Insns[i].Kind == KSW_RULE_JCC &&
            fn->Insns[i].Target < fn->Insns[i].Rva) return 1;
    return 0;
}

// 缓存偏移由注册插入后路径先确定；卸载只交叉验证同getter的cmp0与zero-store。
static int KswCache(KSW_HK_RULE_CONTEXT* context, const KSW_RULE_FUNCTION* fn,
    const KSW_FLOW_ANALYSIS* analysis, const KSW_FLOW_STATE* initial,
    const KSW_FLOW_MODEL* model, KSW_HOTKEY_RULE_RESULT* layout,
    uint32_t anchor, int outlined)
{
    uint32_t i, j, stores = 0; // 缓存清零须有唯一cmp0与零写入配对。
    if (layout->CacheOffset == 0U || model->SessionSlot == 0U) return 0;
    for (i = 0; i < fn->InsnCount; ++i)
    {
        const KSW_RULE_INSN* insn = &fn->Insns[i]; // 仅检查当前可达的真实写入指令。
        const KSW_FLOW_STATE* state = &analysis->States[i]; // 同一扫描图的比较事实必须传到此处。
        KSW_FLOW_VALUE address, zero; // 地址与零值分别通过抽象数据流核验。
        uint32_t cmp = fn->InsnCount; // 哨兵表示尚无同源比较。
        if (!state->Seen || insn->OperandCount != 2 ||
            (insn->Kind != KSW_RULE_MOV && insn->Kind != KSW_RULE_AND) ||
            insn->Operands[0].Kind != KSW_RULE_OP_MEM ||
            insn->Operands[0].Width != 8 ||
            state->ComparedCache != layout->CacheOffset ||
            (outlined && !state->RemoveSucceeded)) continue;
        address = KswFlowAddress(state, &insn->Operands[0], model); // 固定偏移来自注册路径已证明的CacheOffset。
        zero = KswFlowOperand(state, &insn->Operands[1], model); // 零寄存器和立即数零都可证明，不猜原始编码。
        if (address.Kind != KSW_V_SESSION || address.Source != model->SessionSlot ||
            address.Value != (int64_t)layout->CacheOffset || !KswFlowIsConst(zero, 0)) continue;
        if (!outlined && (!KswAfterLoop(fn, analysis, anchor, i) ||
            !KswCacheDominates(context, fn, model, initial, anchor, i))) continue;
        for (j = 0; j < i; ++j) // 同源cmp0必须支配本次清零。
        {
            const KSW_RULE_INSN* test = &fn->Insns[j]; // 候选实际比较指令。
            const KSW_FLOW_STATE* before = &analysis->States[j]; // 保留比较发生时的会话来源。
            KSW_FLOW_VALUE observed, expected; // 左侧缓存字段与右侧抽象零值。
            if (!before->Seen || test->Kind != KSW_RULE_CMP ||
                test->OperandCount != 2 || test->Operands[0].Kind != KSW_RULE_OP_MEM ||
                test->Operands[0].Width != 8) continue;
            observed = KswFlowOperand(before, &test->Operands[0], model); // 字段来源与offset均须匹配。
            expected = KswFlowOperand(before, &test->Operands[1], model); // CMP使用零寄存器同样有效。
            if (observed.Kind == KSW_V_SESSION_FIELD &&
                observed.Source == model->SessionSlot &&
                observed.Aux == layout->CacheOffset && KswFlowIsConst(expected, 0) &&
                KswCacheDominates(context, fn, model, initial, j, i))
            {cmp = j; break;}
        }
        if (cmp == fn->InsnCount) continue; // 缺少必经cmp0的零写入不授权。
        if (++stores != 1U ||
            !KswHkEvidence(context, layout, &fn->Insns[cmp]) ||
            !KswHkEvidence(context, layout, insn)) return 0;
    }
    return stores == 1U; // 重复或缺失缓存写入均保守拒绝。
}

// 扫描签名0是四参owner/window/id/mode3；签名1是私有两参窗口/id内联扫描。
static int KswScan(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    uint32_t signature, KSW_HOTKEY_RULE_RESULT* layout)
{
    KSW_RULE_FUNCTION fn; // 当前四参扫描或两参内联函数的完整指令集合。
    KSW_FLOW_ANALYSIS basis, scan; // 先证明循环，再检查每个节点的身份守卫。
    KSW_FLOW_STATE* initial = 0; // 大入口状态由宿主分配，避免内核栈占用。
    KSW_FLOW_MODEL model = context->Model; // 沿用当前映像实际导入槽及规范owner别名。
    uint32_t i, anchor = 0, helper = 0, calls = 0, nexts = 0; // 单槽目标、调用和next来源都需证成。
    int start, result = 0; // 任何歧义均不发布扫描入口。
    KswFlowZero(&fn, sizeof(fn)); // 未加载函数时仍可安全走清理路径。
    KswFlowZero(&basis, sizeof(basis));
    KswFlowZero(&scan, sizeof(scan));
    if (!KswHkLoad(context, entry, &fn)) return 0;
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial)); // 不把多字段状态放栈上。
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial); // 两种签名从相同未知参数基线开始。
    if (signature == 0U)
    {
        initial->Regs[KSW_REG_RDX].Kind = KSW_V_WNDOBJ; // 四参入口第二参数是窗口对象。
        initial->Regs[KSW_REG_R9] = KswFlowConst(3); // 类型3来自调用图实参，不是猜测默认值。
    }
    else initial->Regs[KSW_REG_RCX].Kind = KSW_V_WNDOBJ; // 内联入口第一参数承载窗口对象。
    model.Mode = 0U; // 先以一般数据流找真实桶循环，不预设节点。
    model.RemoveSignature = signature; // owner/window/ID参数号由当前入口形态决定。
    model.TableOffset = layout->TableOffset;
    model.NextOffset = layout->NextOffset;
    model.OwnerOffset = layout->OwnerOffset;
    model.WindowOffset = layout->WindowOffset;
    model.IdOffset = layout->IdOffset;
    model.OwnerSourceRva = layout->OwnerGetterRva; // 与构造器规范化的安装线程来源绑定。
    model.OwnerSourceKind = layout->OwnerGetterIndirect;
    model.OwnerSourceDepth = layout->OwnerGetterDepth;
    model.WindowRepresentation = layout->WindowRepresentation; // HWND值与tagWND*不可互相冒充。
    start = KswFlowIndex(&fn, fn.EntryRva); // 冷代码也必须从真实入口可达。
    if (start < 0 || !KswFlowRun(context->Full, &fn, &model, (uint32_t)start,
            initial, &basis))
    {context->Error = basis.Error ? basis.Error : KSW_HK_RULE_BUDGET; goto done;}
    if (!KswHkProofLoop(context, &fn, &basis, initial, &model, layout, &anchor)) goto done; // 桶数与表源交叉证明。
    model.Mode = 2U; // 同一当前节点上累计owner/window/ID三项守卫。
    if (!KswFlowRun(context->Full, &fn, &model, (uint32_t)start,
            initial, &scan))
    {context->Error = scan.Error ? scan.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (i = 0; i < fn.InsnCount; ++i)
    {
        const KSW_RULE_INSN* insn = &fn.Insns[i]; // 当前可达的链遍历或删除调用。
        const KSW_FLOW_STATE* state = &scan.States[i]; // 重新载入节点会清除旧身份守卫。
        KSW_FLOW_VALUE link; // next槽必须来自本表当前节点。
        if (!state->Seen) continue;
        if ((insn->Kind == KSW_RULE_LEA || insn->Kind == KSW_RULE_ADD) &&
            insn->OperandCount == 2 && insn->Operands[0].Kind == KSW_RULE_OP_REG)
        {
            if (insn->Kind == KSW_RULE_LEA)
            {
                link = KswFlowAddress(state, &insn->Operands[1], &model); // LEA只认可当前节点next字段。
                if (link.Kind == KSW_V_LINK && link.Source == model.SessionSlot) ++nexts;
            }
            else if (state->Regs[insn->Operands[0].Reg].Kind == KSW_V_NODE &&
                state->Regs[insn->Operands[0].Reg].Source == model.SessionSlot &&
                state->Regs[insn->Operands[0].Reg].Value == 0 &&
                insn->Operands[1].Kind == KSW_RULE_OP_IMM &&
                insn->Operands[1].Imm == layout->NextOffset) ++nexts;
        }
        if (!KswHkCallDirect(insn) || !KswNodeSlot(state, &model, layout)) continue; // 三守卫缺一不可。
        if (helper == 0U)
        {
            if (!KswHkProofSlot(context, insn->Target, layout))
            {if (context->Error != 0U) goto done; continue;}
            helper = insn->Target; // 只有生命周期证明过的一参目标可发布。
        }
        else if (insn->Target != helper) goto done;
        ++calls; // 同语义同目标的重复调用可计入同一候选。
        if (!KswHkEvidence(context, layout, insn)) goto done;
    }
    if (helper == 0U || calls == 0U || nexts == 0U) goto done;
    for (i = 0; i < fn.InsnCount; ++i)
        if (scan.States[i].Seen && KswHkCallDirect(&fn.Insns[i]) &&
            fn.Insns[i].Target == helper && !KswNodeSlot(&scan.States[i], &model, layout)) goto done;
    if (!KswHkEvidence(context, layout, &fn.Insns[anchor])) goto done;
    layout->ScanRva = entry; // 保留身份扫描函数，绝不覆盖单槽RemoveRva。
    if (signature != 0U &&
        !KswCache(context, &fn, &scan, initial, &model, layout, anchor, 0)) goto done;
    result = 1;
done: // 所有候选失败也释放完整分析状态与临时函数。
    KswFlowFree(&basis);
    KswFlowFree(&scan);
    if (initial) context->Full->Free(context->Full->Context, initial);
    KswRuleFreeFunction(context->Full, &fn);
    return result;
}

static int KswOutlinedArgs(const KSW_FLOW_STATE* state,
    const KSW_RULE_INSN* call, const KSW_HOTKEY_RULE_RESULT* layout)
{
    KSW_FLOW_VALUE owner, window, id; // 从真实调用点而非显示串取得三类业务参数。
    if (!state->Seen || !KswHkCallDirect(call)) return 0;
    owner = KswHkCallArgument(state, 1); // 安装线程须与构造器同源同层数。
    window = KswHkCallArgument(state, 2); // nullable窗口仍绑定原入口参数。
    id = KswHkCallArgument(state, 3); // 注册ID来自两参包装器的第二参数。
    return KswOwnerArg(owner, layout) && KswWindowArg(window, state, 1) &&
        KswParam(id, 2) && KswFlowIsConst(KswHkCallArgument(state, 4), 3);
}

// 先让_Private自身证明两参内联扫描，再枚举真正的四参目标。
static int KswPrivate(KSW_HK_RULE_CONTEXT* context, uint32_t entry,
    KSW_HOTKEY_RULE_RESULT* output)
{
    KSW_RULE_FUNCTION fn; // 私有包装器可能自身内联扫描，也可能调用独立扫描函数。
    KSW_FLOW_ANALYSIS basis, final; // 第二轮仅为outlined回执到缓存清零的约束。
    KSW_FLOW_STATE* initial = 0; // 大入口状态放宿主池。
    KSW_HOTKEY_RULE_RESULT* scratch = 0; // 每个候选重置为已证实的注册布局。
    KSW_HOTKEY_RULE_RESULT* chosenResult = 0; // 唯一完整候选才发布给上层。
    KSW_FLOW_MODEL model = context->Model; // 继续使用真实PE getter与owner别名。
    uint32_t i, chosen = 0, matches = 0; // 相同目标可去重，两个不同目标拒绝。
    int start, result = 0; // 资源或预算失败不降级为猜测。
    KswFlowZero(&fn, sizeof(fn));
    KswFlowZero(&basis, sizeof(basis));
    KswFlowZero(&final, sizeof(final));
    scratch = KswHkNewResult(context); // 完整Result不放内核栈。
    chosenResult = KswHkNewResult(context);
    if (!scratch || !chosenResult) goto done;
    *scratch = *output; // 先尝试入口自身的两参内联语义。
    if (KswScan(context, entry, 1U, scratch))
    {
        scratch->UnregisterRva = entry; // 内联扫描和包装器可共用同一真实入口。
        *output = *scratch;
        result = 1;
        goto done;
    }
    if (context->Error != 0U || !KswHkLoad(context, entry, &fn)) goto done;
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial)); // 非内联才加载包装函数。
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    KswFlowInit(initial); // 原窗口入口建立为nullable对象来源。
    initial->Regs[KSW_REG_RCX].Kind = KSW_V_WNDOBJ;
    model.Mode = 0U; // 四参callee识别前不授予任何删除成功状态。
    start = KswFlowIndex(&fn, fn.EntryRva);
    if (start < 0 || !KswFlowRun(context->Full, &fn, &model, (uint32_t)start,
            initial, &basis))
    {context->Error = basis.Error ? basis.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (i = 0; i < fn.InsnCount; ++i)
    {
        const KSW_RULE_INSN* call = &fn.Insns[i]; // 只读审查每个真实direct调用实参。
        if (!KswOutlinedArgs(&basis.States[i], call, output)) continue;
        if (chosen != 0U)
        {
            if (call->Target != chosen || !KswHkEvidence(context, chosenResult, call)) goto done;
            continue;
        }
        *scratch = *output; // 前一候选的证据和字段不得污染下一候选。
        if (!KswScan(context, call->Target, 0U, scratch))
        {if (context->Error != 0U) goto done; continue;}
        model.RemoveRva = call->Target; // 仅包装器用扫描器布尔回执约束缓存失效。
        if (!KswFlowRun(context->Full, &fn, &model, (uint32_t)start,
                initial, &final))
        {context->Error = final.Error ? final.Error : KSW_HK_RULE_BUDGET; goto done;}
        if (!KswCache(context, &fn, &final, initial, &model, scratch, 0U, 1))
        {
            KswFlowFree(&final);
            if (context->Error != 0U) goto done;
            continue; // 失败候选不阻止后续真正四参调用接受同一套证明。
        }
        if (!KswHkEvidence(context, scratch, call)) goto done;
        chosen = call->Target; // 记录唯一完成扫描与缓存交集的目标。
        *chosenResult = *scratch; // Result在堆上暂存，等待全函数消歧。
        ++matches;
        KswFlowFree(&final);
    }
    if (matches != 1U) goto done;
    for (i = 0; i < fn.InsnCount; ++i)
        if (basis.States[i].Seen && KswHkCallDirect(&fn.Insns[i]) &&
            fn.Insns[i].Target == chosen &&
            !KswOutlinedArgs(&basis.States[i], &fn.Insns[i], output)) goto done;
    chosenResult->UnregisterRva = entry; // 上层保留真实私有包装入口身份。
    *output = *chosenResult;
    result = 1;
done: // 任何候选失败都释放分析与堆上Result，不发布半份布局。
    KswFlowFree(&basis);
    KswFlowFree(&final);
    if (initial) context->Full->Free(context->Full->Context, initial);
    if (scratch) context->Full->Free(context->Full->Context, scratch);
    if (chosenResult) context->Full->Free(context->Full->Context, chosenResult);
    KswRuleFreeFunction(context->Full, &fn);
    return result;
}

// 公开NtUser入口须在已验证的USER临界区调用唯一_private目标。
int KswHkProofUnregister(KSW_HK_RULE_CONTEXT* context, KSW_HOTKEY_RULE_RESULT* output)
{
    KSW_RULE_FUNCTION fn; // 公共NtUser卸载导出的实际指令集合。
    KSW_FLOW_ANALYSIS analysis; // EnterCrit与窗口/ID实参必须在同一路径成立。
    KSW_FLOW_STATE* initial = 0; // 参数抽象状态由宿主池承载。
    KSW_HOTKEY_RULE_RESULT* scratch = 0; // 每个私有callee单独尝试完整证明。
    KSW_HOTKEY_RULE_RESULT* chosenResult = 0; // 唯一可用结果在全调用点核对后发布。
    uint32_t entry, i, chosen = 0, matches = 0; // 目标重复可去重、不同目标拒绝。
    int start, result = 0; // 失败默认不授予可调用删除入口。
    KswFlowZero(&fn, sizeof(fn));
    KswFlowZero(&analysis, sizeof(analysis));
    if (!KswRuleFindExport(context->Full, "NtUserUnregisterHotKey", &entry) ||
        !KswHkLoad(context, entry, &fn)) return 0;
    initial = (KSW_FLOW_STATE*)context->Full->Alloc(context->Full->Context, sizeof(*initial)); // 避免完整状态入栈。
    if (!initial) {context->Error = KSW_HK_RULE_RESOURCE; goto done;}
    scratch = KswHkNewResult(context);
    chosenResult = KswHkNewResult(context);
    if (!scratch || !chosenResult) goto done;
    KswFlowInit(initial); // 两个公开参数仍保持原始来源Tag。
    start = KswFlowIndex(&fn, fn.EntryRva);
    if (start < 0 || !KswFlowRun(context->Full, &fn, &context->Model,
            (uint32_t)start, initial, &analysis))
    {context->Error = analysis.Error ? analysis.Error : KSW_HK_RULE_BUDGET; goto done;}
    for (i = 0; i < fn.InsnCount; ++i)
    {
        const KSW_RULE_INSN* call = &fn.Insns[i]; // NtUser调用点必须为实际direct callee。
        const KSW_FLOW_STATE* state = &analysis.States[i]; // 只接纳USER临界区内已验证的参数路径。
        if (!state->Seen || !state->Crit || !KswHkCallDirect(call) ||
            !KswWindowArg(KswHkCallArgument(state, 1), state, 1) ||
            !KswParam(KswHkCallArgument(state, 2), 2)) continue;
        if (chosen != 0U)
        {
            if (call->Target != chosen || !KswHkEvidence(context, chosenResult, call)) goto done;
            continue;
        }
        *scratch = *output; // 旧候选不会将半份Evidence带入下一私有callee。
        if (!KswPrivate(context, call->Target, scratch))
        {if (context->Error != 0U) goto done; continue;}
        chosen = call->Target; // 全部实际调用点需共同指向此唯一目标。
        *chosenResult = *scratch;
        if (!KswHkEvidence(context, chosenResult, call)) goto done;
        ++matches; // 同目标重复调用不等于第二个可用ABI。
    }
    if (matches != 1U) goto done;
    for (i = 0; i < fn.InsnCount; ++i)
        if (analysis.States[i].Seen && KswHkCallDirect(&fn.Insns[i]) &&
            fn.Insns[i].Target == chosen &&
            (!analysis.States[i].Crit ||
                !KswWindowArg(KswHkCallArgument(&analysis.States[i], 1),
                    &analysis.States[i], 1) ||
                !KswParam(KswHkCallArgument(&analysis.States[i], 2), 2))) goto done;
    *output = *chosenResult; // 公开结果仅在完整三层调用链通过后发布。
    result = 1;
done: // 清理后端指令与堆状态，不保留失败的候选结果。
    KswFlowFree(&analysis);
    if (initial) context->Full->Free(context->Full->Context, initial);
    if (scratch) context->Full->Free(context->Full->Context, scratch);
    if (chosenResult) context->Full->Free(context->Full->Context, chosenResult);
    KswRuleFreeFunction(context->Full, &fn);
    return result;
}
