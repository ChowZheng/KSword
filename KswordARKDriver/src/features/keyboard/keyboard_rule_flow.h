#pragma once

#include "keyboard_rule_match.h"

// 有界抽象值不读取运行时对象，只记录代码中的参数、指针及常量来源。
enum KSW_FLOW_VALUE_KIND
{
    // 值类别覆盖未知、常量、参数和栈帧来源。
    KSW_V_UNKNOWN = 0, KSW_V_CONST, KSW_V_PARAM, KSW_V_FRAME,
    // 窗口句柄、已验证对象及其可空形态分别建模。
    KSW_V_CALL, KSW_V_WNDOBJ, KSW_V_WINDOW, KSW_V_NULL_WINDOW, KSW_V_NULL_WNDOBJ,
    // owner指针按地址、结果和可空结果区分。
    KSW_V_OWNER_ADDRESS, KSW_V_OWNER, KSW_V_NULL_OWNER,
    // 会话和节点分别保留基址及字段来源。
    KSW_V_SESSION, KSW_V_SESSION_FIELD, KSW_V_NODE, KSW_V_NODE_FIELD,
    // 桶头、表槽与链槽必须保持来源身份。
    KSW_V_BUCKET, KSW_V_TABLE_HEAD, KSW_V_TABLE_SLOT, KSW_V_LINK,
    // 全局地址、循环索引和已检查布尔值供专项证明使用。
    KSW_V_GLOBAL, KSW_V_LOOP_INDEX, KSW_V_CHECKED_BOOL
};

typedef struct KSW_FLOW_VALUE
{
    uint8_t Kind; // 值的结构来源，不是x64寄存器编号。
    uint8_t Width; // 来源有效字节数，截断指针不能重新变回完整指针。
    uint16_t Tag; // 参数号或派生值类别。
    uint32_t Aux; // 调用RVA、节点字段或掩码。
    uint32_t Source; // 同一节点/槽来源，字段位移不能覆盖此身份。
    int64_t Value; // 常量、帧偏移或指针内部偏移。
} KSW_FLOW_VALUE;

typedef struct KSW_FLOW_CELL
{
    int32_t Offset; // 帧相对地址或节点内偏移。
    uint8_t Width; // 本单元完整写入的宽度。
    uint8_t Used; // 零偏移同样有效，不能用Offset==0表示缺失。
    KSW_FLOW_VALUE V; // 最后完整写入的来源。
} KSW_FLOW_CELL;

// 栈槽容量有界，超过预算不得制造部分证明。
#define KSW_FLOW_STACK_CELLS 40U
// 节点字段槽也有独立上限。
#define KSW_FLOW_NODE_CELLS 32U
// 固定点分析只接收有限长度的解码函数。
#define KSW_FLOW_MAX_INSNS 2048U

typedef struct KSW_FLOW_STATE
{
    uint8_t Seen; // 只有从入口可达的状态才参与证明。
    uint8_t Crit; // 当前路径是否已进入公开USER临界区。
    uint8_t RemoveSucceeded; // 私有卸载调用已在本路径返回真。
    uint8_t LastZeroKnown; // 最后条件码是否可由常量求出。
    uint8_t LastZero; // 最后条件码的ZF值。
    uint8_t LastTestReg; // 可在零分支上约束的寄存器族。
    uint8_t LastGuard; // 节点字段比较产生的守卫位。
    uint8_t Guards; // 同一当前节点上已经证明的比较条件。
    uint8_t NodeAlive; // 已分配且未交给未知可变调用的新节点。
    uint8_t PointerBelowLimit; // 当前条件仅把合法对象与NULL分开。
    uint8_t CarryKnown, Carry, SignKnown, Sign, LessKnown, Less; // 常量比较支持等价的位/有符号分支。
    uint16_t NullArgs; // 本路径明确为空的入口参数。
    uint16_t NonNullArgs; // 本路径明确非空的入口参数。
    uint32_t ComparedCache; // 已比较的同一会话缓存位，零表示尚未证明。
    uint32_t NullOwnerSource; // 空分支实际测试的owner getter。
    uint16_t NullOwnerKind, NullOwnerDepth; // 不把无关常量零当owner来源。
    KSW_FLOW_VALUE LastValue; // 供test/cmp后分支建立来源条件。
    KSW_FLOW_VALUE Regs[18]; // NONE、16个GPR族及RIP。
    KSW_FLOW_CELL Stack[KSW_FLOW_STACK_CELLS]; // 仅保存显式栈槽，不猜未知内存。
    KSW_FLOW_CELL Node[KSW_FLOW_NODE_CELLS]; // 仅保存本次分配节点的完整字段写入。
} KSW_FLOW_STATE;

typedef struct KSW_FLOW_OWNER_ALIAS
{
    // 别名入口与规范RVA关联实际owner调用链。
    uint32_t EntryRva, CanonicalRva;
    // 类别及解引用深度用于识别相同owner来源。
    uint16_t Kind, Depth;
} KSW_FLOW_OWNER_ALIAS;

typedef struct KSW_FLOW_MODEL
{
    uint32_t AllocRva; // 只将这个实际分配调用的返回值标为新节点。
    uint32_t InsertRva; // 已证明的插入函数，调用前是节点初始化证明终点。
    uint32_t RemoveRva; // 返回值用于缓存失效成功分支。
    uint32_t OwnerGetterRva; // 注册/卸载共同的安装线程来源。
    uint32_t TableOffset; // 已独立识别的同一会话哈希表。
    uint32_t NextOffset; // 插入和删除共同的链字段。
    uint32_t OwnerOffset; // 删除守卫所使用的节点归属字段。
    uint32_t WindowOffset; // 删除守卫所使用的窗口字段。
    uint32_t IdOffset; // 删除守卫所使用的注册标识字段。
    uint32_t LoopReg; // 已证明按指针宽度推进的桶地址寄存器。
    uint32_t Mode; // 0普通参数流、1插入、2删除。
    uint32_t EnterSlot; // Full的EnterCrit IAT槽位。
    uint32_t LeaveSlot; // Full的LeaveCrit IAT槽位。
    uint32_t ValidateSlot; // Full的ValidateHwnd IAT槽位。
    uint32_t SessionSlot; // Full的W32GetUserSessionState IAT槽位。
    uint32_t AllocSlot; // Full的零初始化分配器IAT槽位。
    uint32_t BugcheckSlot; // 不返回的已知系统调用，不构造虚假的后续路径。
    uint32_t OwnerDataSlot; // 实际导入的gptiCurrent指针槽。
    uint32_t RemoveSignature; // 0四参数扫描器，1内联的窗口/ID包装器。
    uint32_t OwnerSourceRva; // 构造器实际安装线程来源。
    uint32_t OwnerSourceKind; // 调用/IAT/导入数据来源类别。
    uint32_t OwnerSourceDepth; // 同一getter结果的实际解引用深度。
    uint32_t LoopIndexReg; // 索引式桶循环的有界计数寄存器。
    uint32_t FlagsOffset, FlagsWidth, ChildOffset; // 普通节点路径的实际字段。
    uint32_t FreeSlot, FreeRva; // 经生命周期证明的释放入口。
    uint32_t AssignmentSlot; // 实际HMAssignmentLock导入，写入相同节点的对象槽。
    uint32_t WindowRepresentation; // 删除比较的窗口值表示。
    // 透明owner别名数量由八项数组限制。
    uint32_t OwnerAliasCount;
    KSW_FLOW_OWNER_ALIAS OwnerAliases[8]; // 只来自当前映像透明返回证明，不是版本表。
} KSW_FLOW_MODEL;

typedef struct KSW_FLOW_ANALYSIS
{
    const KSW_RULE_IMAGE* Image; // 所有临时内存由同一个映像宿主释放。
    const KSW_RULE_FUNCTION* Function; // 指令边界在此分析期间不变。
    KSW_FLOW_STATE* States; // 每条可达指令入口的合并后状态。
    uint32_t Count; // 与Function的指令数一致。
    uint32_t Error; // 分支越界、资源或预算失败不得返回部分证明。
} KSW_FLOW_ANALYSIS;

// 入口状态可用于从某个分配返回点单独证明新节点路径，排除既有节点分支。
int KswFlowRun(const KSW_RULE_IMAGE* Image, const KSW_RULE_FUNCTION* Function,
    const KSW_FLOW_MODEL* Model, uint32_t StartIndex,
    const KSW_FLOW_STATE* Initial, KSW_FLOW_ANALYSIS* Analysis);
// 释放固定点状态与运行分析使用同一个映像宿主。
void KswFlowFree(KSW_FLOW_ANALYSIS* Analysis);
// 初态按x64调用约定标记参数寄存器和栈槽。
void KswFlowInit(KSW_FLOW_STATE* State);
// 操作数读取遵守已证明的来源与有效宽度。
KSW_FLOW_VALUE KswFlowOperand(const KSW_FLOW_STATE* State,
    const KSW_RULE_OPERAND* Operand, const KSW_FLOW_MODEL* Model);
// 未知值和常量值由共用构造器创建。
KSW_FLOW_VALUE KswFlowUnknown(void);
KSW_FLOW_VALUE KswFlowConst(int64_t Value);
// 常量判断要求来源类别和值同时一致。
int KswFlowIsConst(KSW_FLOW_VALUE Value, int64_t Constant);
// 指令索引只接收精确解码边界。
int KswFlowIndex(const KSW_RULE_FUNCTION* Function, uint32_t Rva);
// 地址推导与状态单元存储均有独立来源约束。
KSW_FLOW_VALUE KswFlowAddress(const KSW_FLOW_STATE* State,
    const KSW_RULE_OPERAND* Operand, const KSW_FLOW_MODEL* Model);
int KswFlowStore(KSW_FLOW_CELL* Cells, uint32_t Count, int32_t Offset,
    uint8_t Width, KSW_FLOW_VALUE Value);
// 寄存器写入必须处理窄写截断。
void KswFlowSetReg(KSW_FLOW_STATE* State, uint8_t Reg, uint8_t Width, KSW_FLOW_VALUE Value);
// 状态合流只保留每条可达路径共同成立的事实。
int KswFlowJoin(KSW_FLOW_STATE* Destination, const KSW_FLOW_STATE* Source);
// 清零和复制使用纯C字节循环以复用内核与离线宿主。
void KswFlowZero(void* Destination, size_t Size);
void KswFlowCopy(void* Destination, const void* Source, size_t Size);
