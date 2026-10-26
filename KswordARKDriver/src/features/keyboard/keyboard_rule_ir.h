#pragma once
// 内核编译使用WDK类型，并在本头内提供规则引擎所需固定宽度别名。
#if defined(_KERNEL_MODE)
#include <ntddk.h>
// WDK使用km CRT，不能从VC用户态stdint引入另一套crtdefs。
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef INT32 int32_t;
typedef INT64 int64_t;
// 旧WDK缺少标准常量宏时提供等价64位常量。
#ifndef UINT64_C
#define UINT64_C(value) value##ui64
#endif
// 无标准最大值宏时显式给出完整64位掩码。
#ifndef UINT64_MAX
#define UINT64_MAX ((uint64_t)~UINT64_C(0))
#endif
// 32位RVA上界沿用无符号32位最大值。
#ifndef UINT32_MAX
#define UINT32_MAX ((uint32_t)0xffffffffU)
#endif
// 解码宽度检查所需的八位上限由本头补齐。
#ifndef UINT8_MAX
#define UINT8_MAX 0xffU
#endif
// 离线宿主直接使用标准C的大小及整数类型。
#else
#include <stddef.h>
#include <stdint.h>
#endif

// 宿主仅提供有界映像读取与临时分配；内核和离线程序共用后续全部规则。
typedef struct KSW_RULE_IMAGE {
    // 上下文由宿主独占，规则引擎不直接解引用模块基址。
    void* Context;
    // 映像大小给全部RVA读操作设置统一边界。
    uint32_t SizeOfImage;
    // 读取回调必须返回完整长度成功与否。
    int (*Read)(void*, uint32_t, void*, size_t);
    // 临时分配和释放回调共同管理解码及证明工作区。
    void* (*Alloc)(void*, size_t);
    void (*Free)(void*, void*);
} KSW_RULE_IMAGE;

// 操作数类型分别表示缺失、寄存器、内存和立即数。
enum {KSW_RULE_OP_NONE, KSW_RULE_OP_REG, KSW_RULE_OP_MEM, KSW_RULE_OP_IMM};
// 通用寄存器族跨八位至六十四位统一编号。
enum {KSW_REG_NONE, KSW_REG_RAX, KSW_REG_RCX, KSW_REG_RDX, KSW_REG_RBX,
    KSW_REG_RSP, KSW_REG_RBP, KSW_REG_RSI, KSW_REG_RDI, KSW_REG_R8, KSW_REG_R9,
    KSW_REG_R10, KSW_REG_R11, KSW_REG_R12, KSW_REG_R13, KSW_REG_R14, KSW_REG_R15, KSW_REG_RIP};
// 只给引擎可解释的指令分配专用语义类别。
enum {KSW_RULE_OTHER, KSW_RULE_MOV, KSW_RULE_MOVZX, KSW_RULE_MOVSX, KSW_RULE_LEA,
    KSW_RULE_XOR, KSW_RULE_AND, KSW_RULE_OR, KSW_RULE_ADD, KSW_RULE_SUB,
    KSW_RULE_DEC, KSW_RULE_INC, KSW_RULE_CMP, KSW_RULE_TEST, KSW_RULE_CALL,
    KSW_RULE_JMP, KSW_RULE_JCC, KSW_RULE_RET, KSW_RULE_PUSH, KSW_RULE_POP,
    KSW_RULE_NOP, KSW_RULE_BT, KSW_RULE_TRAP};
// 条件类别区分零、进位、符号及有符号大小关系。
enum {KSW_RULE_COND_ANY, KSW_RULE_COND_Z, KSW_RULE_COND_NZ, KSW_RULE_COND_C,
    KSW_RULE_COND_NC, KSW_RULE_COND_S, KSW_RULE_COND_NS, KSW_RULE_COND_L, KSW_RULE_COND_GE};

typedef struct KSW_RULE_OPERAND {
    // 一个操作数保留类别、有效宽度及寄存器寻址字段。
    uint8_t Kind, Width, Reg, Base, Index, Scale;
    // 位移和立即数用64位位模式承载。
    int64_t Disp, Imm;
} KSW_RULE_OPERAND;
typedef struct KSW_RULE_INSN {
    // 指令身份、控制流目标及寄存器写集合统一由解码器提供。
    uint32_t Rva, Target, RegWrites;
    // 指令长度和语义类别决定单步转移及边界。
    uint16_t Length, Kind;
    // 条件码、可见操作数数目和标志写入共同决定分支事实。
    uint8_t Condition, OperandCount, FlagWrites;
    // 前三个可见操作数保留原顺序，额外可见项拒绝。
    KSW_RULE_OPERAND Operands[3];
} KSW_RULE_INSN;
// 函数片段由半开RVA区间表示。
typedef struct KSW_RULE_RANGE {uint32_t Begin, End;} KSW_RULE_RANGE;
typedef struct KSW_RULE_FUNCTION {
    // 函数入口、指令数及非连续片段数均为有界值。
    uint32_t EntryRva, InsnCount, RangeCount;
    // 异常表链式冷块最多记录十六个范围。
    KSW_RULE_RANGE Ranges[16];
    // 指令数组归属宿主分配器，须显式释放。
    KSW_RULE_INSN* Insns;
} KSW_RULE_FUNCTION;

// 所有PE读取先经统一减法范围检查。
int KswRuleRead(const KSW_RULE_IMAGE* Image, uint32_t Rva, void* Output, size_t Size);
// 导出查找须返回唯一、可执行且非转发目标。
int KswRuleFindExport(const KSW_RULE_IMAGE* Image, const char* Name, uint32_t* Rva);
// 导入查找返回当前INT名称对应的实际IAT槽位。
int KswRuleFindImportSlot(const KSW_RULE_IMAGE* Image, const char* Dll, const char* Name, uint32_t* Rva);
// 代码校验要求整个请求范围在可执行节内。
int KswRuleIsCode(const KSW_RULE_IMAGE* Image, uint32_t Rva, uint32_t Size);
// 单条指令解码输出紧凑且来源明确的IR。
int KswRuleDecode(const KSW_RULE_IMAGE* Image, uint32_t Rva, KSW_RULE_INSN* Instruction);
// 函数加载结合异常表范围和有界叶函数规则。
int KswRuleLoadFunction(const KSW_RULE_IMAGE* Image, uint32_t Entry, KSW_RULE_FUNCTION* Function);
// 函数释放回到创建该指令数组的宿主。
void KswRuleFreeFunction(const KSW_RULE_IMAGE* Image, KSW_RULE_FUNCTION* Function);
