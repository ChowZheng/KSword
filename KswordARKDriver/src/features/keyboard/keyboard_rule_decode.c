#include "keyboard_rule_ir.h"
#if defined(_MSC_VER)
// 第三方头使用匿名结构扩展，仅在其包含范围内关闭该扩展提示。
#pragma warning(push)
#pragma warning(disable: 4201)
#endif
#include "../../../../third_party/zydis/Zydis.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* PE模块的内部有界接口；公开调用者只需包含IR头。 */
int KswRuleCodeEnd(const KSW_RULE_IMAGE*, uint32_t, uint32_t*);
int KswRuleFunctionRanges(const KSW_RULE_IMAGE*, uint32_t, KSW_RULE_FUNCTION*, uint32_t*);

#define KSW_RULE_MAX_INSNS 4096U /* 代码复杂度预算与版本身份无关。 */
#define KSW_RULE_MAX_BYTES 65536U /* 每个函数所有片段合计不得超过64KiB。 */

/* 大型解码结构由宿主分配，不放在内核调用栈上。 */
typedef struct KSW_RULE_DECODE_WORK {
    ZydisDecoder Decoder;
    ZydisDecodedInstruction Instruction;
    ZydisDecodedOperand Operands[ZYDIS_MAX_OPERAND_COUNT];
} KSW_RULE_DECODE_WORK;

/* 不依赖用户态CRT的清零入口。 */
static void KswDecodeZero(void* pointer, size_t bytes)
{
    // 手写字节清零避免内核宿主依赖用户态CRT。
    unsigned char* p = (unsigned char*)pointer;
    while (bytes) { *p++ = 0; --bytes; }
}

/* 将全部宽度的GPR映射到统一family，位号与高层reaching-defs约定一致。 */
static uint8_t KswDecodeRegister(ZydisRegister reg)
{
    // 不同宽度的同一通用寄存器归并为一个抽象寄存器族。
    switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
    case ZYDIS_REGISTER_RAX: return KSW_REG_RAX;
    case ZYDIS_REGISTER_RCX: return KSW_REG_RCX;
    case ZYDIS_REGISTER_RDX: return KSW_REG_RDX;
    case ZYDIS_REGISTER_RBX: return KSW_REG_RBX;
    case ZYDIS_REGISTER_RSP: return KSW_REG_RSP;
    case ZYDIS_REGISTER_RBP: return KSW_REG_RBP;
    case ZYDIS_REGISTER_RSI: return KSW_REG_RSI;
    case ZYDIS_REGISTER_RDI: return KSW_REG_RDI;
    case ZYDIS_REGISTER_R8: return KSW_REG_R8;
    case ZYDIS_REGISTER_R9: return KSW_REG_R9;
    case ZYDIS_REGISTER_R10: return KSW_REG_R10;
    case ZYDIS_REGISTER_R11: return KSW_REG_R11;
    case ZYDIS_REGISTER_R12: return KSW_REG_R12;
    case ZYDIS_REGISTER_R13: return KSW_REG_R13;
    case ZYDIS_REGISTER_R14: return KSW_REG_R14;
    case ZYDIS_REGISTER_R15: return KSW_REG_R15;
    case ZYDIS_REGISTER_RIP: return KSW_REG_RIP;
    default: return KSW_REG_NONE; /* SIMD、段寄存器及标志不是GPR值来源。 */
    }
}

/* 只给语义明确的指令命名，其余指令仍保留全部GPR写集合。 */
static uint16_t KswDecodeKind(const ZydisDecodedInstruction* instruction)
{
    // 仅映射规则引擎能解释的指令，余类保留为OTHER。
    switch (instruction->mnemonic) {
    case ZYDIS_MNEMONIC_MOV: return KSW_RULE_MOV;
    case ZYDIS_MNEMONIC_MOVZX: return KSW_RULE_MOVZX;
    case ZYDIS_MNEMONIC_MOVSX: case ZYDIS_MNEMONIC_MOVSXD: return KSW_RULE_MOVSX;
    case ZYDIS_MNEMONIC_LEA: return KSW_RULE_LEA;
    case ZYDIS_MNEMONIC_XOR: return KSW_RULE_XOR;
    case ZYDIS_MNEMONIC_AND: return KSW_RULE_AND;
    case ZYDIS_MNEMONIC_OR: return KSW_RULE_OR;
    case ZYDIS_MNEMONIC_ADD: return KSW_RULE_ADD;
    case ZYDIS_MNEMONIC_SUB: return KSW_RULE_SUB;
    case ZYDIS_MNEMONIC_DEC: return KSW_RULE_DEC;
    case ZYDIS_MNEMONIC_INC: return KSW_RULE_INC;
    case ZYDIS_MNEMONIC_CMP: return KSW_RULE_CMP;
    case ZYDIS_MNEMONIC_TEST: return KSW_RULE_TEST;
    case ZYDIS_MNEMONIC_BT: return KSW_RULE_BT;
    case ZYDIS_MNEMONIC_CALL: return KSW_RULE_CALL;
    case ZYDIS_MNEMONIC_JMP: return KSW_RULE_JMP;
    case ZYDIS_MNEMONIC_RET: return KSW_RULE_RET;
    case ZYDIS_MNEMONIC_PUSH: return KSW_RULE_PUSH;
    case ZYDIS_MNEMONIC_POP: return KSW_RULE_POP;
    case ZYDIS_MNEMONIC_UD2: return KSW_RULE_TRAP;
    case ZYDIS_MNEMONIC_NOP: case ZYDIS_MNEMONIC_ENDBR64: return KSW_RULE_NOP;
    // 未知条件跳转仍保留CFG类别，条件细节降为ANY。
    default: return instruction->meta.category == ZYDIS_CATEGORY_COND_BR ? KSW_RULE_JCC : KSW_RULE_OTHER;
    }
}

/* 把完整解码转换为紧凑IR；不截断额外可见操作数，也不把高八位寄存器当低八位。 */
static int KswDecodeBytes(KSW_RULE_DECODE_WORK* work, const unsigned char* bytes,
    size_t available, uint32_t rva, KSW_RULE_INSN* output)
{
    uint32_t i; /* 显式与隐含操作数都参加写集合计算。 */
    // 输出先清零，Zydis必须完整解码且可见操作数不超IR容量。
    KswDecodeZero(output, sizeof(*output));
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&work->Decoder, bytes, available, &work->Instruction, work->Operands)) ||
        !work->Instruction.length || work->Instruction.length > available || work->Instruction.operand_count_visible > 3U) return 0;
    // 指令RVA、长度和语义类别来自当前实际映像字节。
    output->Rva = rva; output->Length = work->Instruction.length;
    output->Kind = KswDecodeKind(&work->Instruction);
    if (work->Instruction.mnemonic == ZYDIS_MNEMONIC_INT && work->Instruction.operand_count_visible == 1 &&
        work->Operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && work->Operands[0].imm.value.u == 0x29U)
        output->Kind = KSW_RULE_TRAP; /* fastfail没有正常返回边，不把冷块终点当隐含fallthrough。 */
    // 可见操作数数量及条件码种类明确记录到IR。
    output->OperandCount = work->Instruction.operand_count_visible;
    output->Condition = work->Instruction.mnemonic == ZYDIS_MNEMONIC_JZ ? KSW_RULE_COND_Z :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JNZ ? KSW_RULE_COND_NZ :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JB ? KSW_RULE_COND_C :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JNB ? KSW_RULE_COND_NC :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JS ? KSW_RULE_COND_S :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JNS ? KSW_RULE_COND_NS :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JL ? KSW_RULE_COND_L :
        work->Instruction.mnemonic == ZYDIS_MNEMONIC_JNL ? KSW_RULE_COND_GE : KSW_RULE_COND_ANY;
    // Zydis报告的标志写集合决定旧条件事实是否失效。
    if (work->Instruction.cpu_flags) {
        const ZydisAccessedFlags* flags = work->Instruction.cpu_flags; /* 未定义的标志同样使旧条件来源失效。 */
        output->FlagWrites = (uint8_t)((flags->modified | flags->set_0 | flags->set_1 | flags->undefined) != 0);
    }
    // 所有显式和隐式操作数都参加寄存器写集合统计。
    for (i = 0; i < work->Instruction.operand_count; ++i) {
        const ZydisDecodedOperand* source = &work->Operands[i]; /* 由Zydis发布的完整操作数。 */
        uint8_t reg = source->type == ZYDIS_OPERAND_TYPE_REGISTER ? KswDecodeRegister(source->reg.value) : KSW_REG_NONE;
        // 被写通用寄存器按族设置写位，未知指令也不能保留旧值。
        if (reg && (source->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) output->RegWrites |= (uint32_t)1U << reg;
        // 前三个可见操作数保留原顺序及字节宽度。
        if (i < output->OperandCount) {
            KSW_RULE_OPERAND* op = &output->Operands[i]; /* 可见操作数保留原始顺序。 */
            // 非整字节或超过IR宽度的操作数拒绝截断。
            if ((source->size & 7U) || source->size / 8U > UINT8_MAX) return 0;
            op->Width = (uint8_t)(source->size / 8U);
            // 高八位寄存器不能冒充相应低八位寄存器。
            if (source->type == ZYDIS_OPERAND_TYPE_REGISTER) {
                op->Kind = KSW_RULE_OP_REG; op->Reg = reg;
                if (source->reg.value == ZYDIS_REGISTER_AH || source->reg.value == ZYDIS_REGISTER_BH ||
                    source->reg.value == ZYDIS_REGISTER_CH || source->reg.value == ZYDIS_REGISTER_DH) output->Kind = KSW_RULE_OTHER;
            // 内存操作数分别保存基址、索引、比例和位移。
            } else if (source->type == ZYDIS_OPERAND_TYPE_MEMORY) {
                op->Kind = KSW_RULE_OP_MEM; op->Base = KswDecodeRegister(source->mem.base);
                op->Index = KswDecodeRegister(source->mem.index); op->Scale = source->mem.scale;
                op->Disp = source->mem.disp.has_displacement ? source->mem.disp.value : 0;
                // RIP相对内存在IR里归一为实际RVA。
                if (op->Base == KSW_REG_RIP) op->Disp += (int64_t)rva + work->Instruction.length; /* IR内RIP内存统一为RVA，不让语义层反复猜相对基址。 */
                // 无法表示的向量索引地址不能伪装成普通基址索引。
                if (source->mem.index != ZYDIS_REGISTER_NONE && !op->Index) return 0; /* IR不伪装支持VSIB地址。 */
                // FS/GS线程段读取不能被当作普通对象字段。
                if (output->Kind != KSW_RULE_LEA && (source->mem.segment == ZYDIS_REGISTER_FS || source->mem.segment == ZYDIS_REGISTER_GS))
                    output->Kind = KSW_RULE_OTHER; /* 无段字段的IR不能把线程段读取误认为普通对象字段。 */
            // 立即数保留原始64位有符号位模式；其它类型拒绝。
            } else if (source->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                op->Kind = KSW_RULE_OP_IMM; op->Imm = source->imm.value.s; /* 位模式保留为有符号64位值。 */
            } else return 0; /* far pointer等未承载形式不能被截成空操作数。 */
        }
    }
    // 直接分支与RIP间接槽都只记录可计算的32位目标RVA。
    if (output->Kind == KSW_RULE_CALL || output->Kind == KSW_RULE_JMP || output->Kind == KSW_RULE_JCC) {
        if (output->OperandCount) {
            const ZydisDecodedOperand* operand = &work->Operands[0]; /* 直接分支或RIP间接槽均由解码器计算地址。 */
            ZyanU64 target = 0; /* 不接受溢出或超出32位RVA的目标。 */
            if ((operand->type == ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                    (operand->type == ZYDIS_OPERAND_TYPE_MEMORY && operand->mem.base == ZYDIS_REGISTER_RIP)) &&
                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&work->Instruction, operand, rva, &target)) && target <= UINT32_MAX)
                output->Target = (uint32_t)target;
        }
    }
    return 1;
}

/* 独立指令入口也完整限制在代码节内，工作区由宿主分配。 */
int KswRuleDecode(const KSW_RULE_IMAGE* image, uint32_t rva, KSW_RULE_INSN* instruction)
{
    // 大型Zydis工作结构交由宿主分配，栈上只留15字节指令缓冲。
    KSW_RULE_DECODE_WORK* work; /* 不将Zydis操作数数组放入内核栈。 */
    unsigned char bytes[15]; /* x64完整指令的架构上限。 */
    uint32_t end; size_t available; int result = 0;
    // 无输出指针或非代码RVA不能开始解码。
    if (!instruction) return 0;
    KswDecodeZero(instruction, sizeof(*instruction));
    if (!image || !image->Alloc || !image->Free || !KswRuleCodeEnd(image, rva, &end)) return 0;
    // 一次最多读取当前代码节余量内的15个字节。
    available = end - rva < sizeof(bytes) ? (size_t)(end - rva) : sizeof(bytes);
    if (!KswRuleRead(image, rva, bytes, available)) return 0;
    work = (KSW_RULE_DECODE_WORK*)image->Alloc(image->Context, sizeof(*work));
    if (!work) return 0;
    // 初始化64位解码器成功后才解释输入字节。
    if (ZYAN_SUCCESS(ZydisDecoderInit(&work->Decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        result = KswDecodeBytes(work, bytes, available, rva, instruction);
    // 工作区始终释放，失败时把输出清为完整未知状态。
    image->Free(image->Context, work);
    if (!result) KswDecodeZero(instruction, sizeof(*instruction)); /* 解码失败不保留半份IR。 */
    return result;
}

/* 将片段按RVA排序并归并，让最终指令数组天然有序且不重复。 */
static void KswDecodeSortRanges(KSW_RULE_FUNCTION* fn)
{
    // 片段排序和合并预算只有十六项，可使用插入排序。
    uint32_t i, count = 0;
    // 排序时移动比当前片段起点更大的既有范围。
    for (i = 1; i < fn->RangeCount; ++i) {
        KSW_RULE_RANGE range = fn->Ranges[i]; uint32_t j = i; /* 最多16项，插入排序无需CRT。 */
        while (j && fn->Ranges[j - 1U].Begin > range.Begin) { fn->Ranges[j] = fn->Ranges[j - 1U]; --j; }
        fn->Ranges[j] = range;
    }
    // 相邻或重叠片段合成单个RVA区间。
    for (i = 0; i < fn->RangeCount; ++i) {
        if (count && fn->Ranges[i].Begin <= fn->Ranges[count - 1U].End) {
            if (fn->Ranges[i].End > fn->Ranges[count - 1U].End) fn->Ranges[count - 1U].End = fn->Ranges[i].End;
        } else fn->Ranges[count++] = fn->Ranges[i];
    }
    fn->RangeCount = count;
}

/* 确认目标是已完整解码的指令边界，不能跳入另一条指令中部。 */
static int KswDecodeHasInstruction(const KSW_RULE_FUNCTION* fn, uint32_t rva)
{
    // 二分搜索要求跳转目标严格落在已解码指令起点。
    uint32_t low = 0, high = fn->InsnCount;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2U;
        if (fn->Insns[middle].Rva < rva) low = middle + 1U; else high = middle;
    }
    return low < fn->InsnCount && fn->Insns[low].Rva == rva;
}

/* 释放唯一指令数组并清空对象，失败路径与调用者清理共用。 */
void KswRuleFreeFunction(const KSW_RULE_IMAGE* image, KSW_RULE_FUNCTION* fn)
{
    // 释放指令数组后清空函数对象，避免重复释放旧指针。
    if (!fn) return;
    if (fn->Insns && image && image->Free) image->Free(image->Context, fn->Insns);
    KswDecodeZero(fn, sizeof(*fn));
}

/* 完整解码pdata及链式冷块；无pdata只接受有界、无调用且以RET收口的叶候选。 */
int KswRuleLoadFunction(const KSW_RULE_IMAGE* image, uint32_t entry, KSW_RULE_FUNCTION* fn)
{
    KSW_RULE_DECODE_WORK* work = NULL; /* 解码器及代码缓冲全部通过回调取得。 */
    unsigned char* code = NULL;
    uint32_t leafEnd = 0, mode, i, total = 0, maximum = 0, required = entry;
    int success = 0; /* 只有完整范围与分支边界均确认后才发布结果。 */
    // 无输出指针或分配回调时不创建半份函数IR。
    if (!fn) return 0;
    KswDecodeZero(fn, sizeof(*fn));
    if (!image || !image->Alloc || !image->Free) return 0;
    // 先取得真实pdata片段，缺少pdata时转入有界叶函数模式。
    fn->EntryRva = entry;
    mode = (uint32_t)KswRuleFunctionRanges(image, entry, fn, &leafEnd);
    if (!mode) { KswDecodeZero(fn, sizeof(*fn)); return 0; } /* 元数据失败同样不能留下部分函数范围。 */
    // 叶函数只在当前节内读取最多4KiB。
    if (mode == 2U) {
        fn->RangeCount = 1; fn->Ranges[0].Begin = entry;
        fn->Ranges[0].End = leafEnd - entry > 4096U ? entry + 4096U : leafEnd; /* 无函数表时最多读取4KiB。 */
    }
    KswDecodeSortRanges(fn);
    // 累计所有片段大小，拒绝空片段或超出64KiB预算。
    for (i = 0; i < fn->RangeCount; ++i) {
        uint32_t bytes = fn->Ranges[i].End - fn->Ranges[i].Begin;
        if (!bytes || bytes > KSW_RULE_MAX_BYTES - total) goto done;
        total += bytes; if (bytes > maximum) maximum = bytes;
    }
    // 解码器、代码片段和指令数组均由宿主独立分配。
    work = (KSW_RULE_DECODE_WORK*)image->Alloc(image->Context, sizeof(*work));
    code = (unsigned char*)image->Alloc(image->Context, maximum);
    fn->Insns = (KSW_RULE_INSN*)image->Alloc(image->Context, (size_t)KSW_RULE_MAX_INSNS * sizeof(*fn->Insns));
    if (!work || !code || !fn->Insns || !ZYAN_SUCCESS(ZydisDecoderInit(&work->Decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) goto done;
    // 逐段完整读入字节，并且每次只从指令边界推进。
    for (i = 0; i < fn->RangeCount; ++i) {
        uint32_t rva = fn->Ranges[i].Begin, bytes = fn->Ranges[i].End - rva;
        if (!KswRuleRead(image, rva, code, bytes)) goto done;
        // 单条指令最多十五字节，指令总数另有4096项上限。
        while (rva < fn->Ranges[i].End) {
            KSW_RULE_INSN* insn; size_t available = fn->Ranges[i].End - rva; /* 每次只交给解码器一条指令上限。 */
            if (fn->InsnCount == KSW_RULE_MAX_INSNS) goto done;
            if (available > 15U) available = 15U;
            insn = &fn->Insns[fn->InsnCount];
            if (!KswDecodeBytes(work, code + rva - fn->Ranges[i].Begin, available, rva, insn)) goto done;
            ++fn->InsnCount;
            // 无pdata候选不能含调用、未收口的栈写或范围外分支。
            if (mode == 2U) {
                if (insn->Kind == KSW_RULE_CALL || (insn->Kind != KSW_RULE_RET && (insn->RegWrites & ((uint32_t)1U << KSW_REG_RSP)))) goto done;
                if (insn->Kind == KSW_RULE_JMP || insn->Kind == KSW_RULE_JCC) {
                    if (insn->Operands[0].Kind != KSW_RULE_OP_IMM || insn->Target < entry || insn->Target >= fn->Ranges[i].End) goto done;
                    if (insn->Target > required) required = insn->Target;
                }
            }
            // 下一位置由完整指令长度确定，RET后才可缩短叶函数尾部。
            rva += insn->Length; /* 唯一推进单位是完整指令长度。 */
            if (mode == 2U && insn->Kind == KSW_RULE_RET && rva > required) { fn->Ranges[i].End = rva; break; }
        }
        // 叶函数最终必须以真实RET结束。
        if (mode == 2U && fn->Insns[fn->InsnCount - 1U].Kind != KSW_RULE_RET) goto done;
    }
    if (!KswDecodeHasInstruction(fn, entry)) goto done;
    // 所有分支目标都按完整指令边界复查。
    for (i = 0; i < fn->InsnCount; ++i) {
        const KSW_RULE_INSN* insn = &fn->Insns[i]; uint32_t range; int inside = 0;
        if (insn->Kind != KSW_RULE_JMP && insn->Kind != KSW_RULE_JCC) continue;
        for (range = 0; range < fn->RangeCount; ++range)
            if (insn->Target >= fn->Ranges[range].Begin && insn->Target < fn->Ranges[range].End) inside = 1;
        if (inside && !KswDecodeHasInstruction(fn, insn->Target)) goto done;
        // 有条件跳转到未知冷块不构造缺失的CFG边。
        if (insn->Kind == KSW_RULE_JCC && !inside) goto done; /* 未由unwind归属证明的外部冷块不能补猜。 */
        // 无条件外跳只能在有pdata且目标仍属代码节时保留。
        if (insn->Kind == KSW_RULE_JMP && !inside) {
            if (!insn->Target || mode == 2U) goto done; /* 无pdata叶函数不能通过未知尾跳获得范围。 */
            if (insn->Operands[0].Kind == KSW_RULE_OP_IMM && !KswRuleIsCode(image, insn->Target, 1)) goto done;
        }
    }
    // 只有所有范围和分支均验证完成才标记成功。
    success = 1;
done:
    // 任一退出路径释放局部工作区，失败时释放并清空函数IR。
    if (code) image->Free(image->Context, code);
    if (work) image->Free(image->Context, work);
    if (!success) KswRuleFreeFunction(image, fn);
    return success;
}
