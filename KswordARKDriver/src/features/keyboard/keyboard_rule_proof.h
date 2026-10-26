#pragma once
#include "keyboard_rule_flow.h"

// 规则上下文仅包含当前映像、抽象模型、错误和被分析函数入口。
typedef struct KSW_HK_RULE_CONTEXT {
    const KSW_RULE_IMAGE* Full; // 当前实际映像，不包含版本授权状态。
    // Base与Full分别使用自己的读取回调和映像大小。
    const KSW_RULE_IMAGE* Base;
    KSW_FLOW_MODEL Model; // 本映像解析出的导入槽。
    uint32_t Error; // 资源与解码失败不能伪装为无候选。
    uint32_t SeenCount; // 本次规则实际分析的Full函数。
    uint32_t SeenFunctions[KSW_HOTKEY_RULE_MAX_FUNCTIONS]; // 只用于当前映像证明与缓存完整性。
} KSW_HK_RULE_CONTEXT;

// 注册证明捕获分配、字段必经初始化及同表插入。
int KswHkProofRegister(KSW_HK_RULE_CONTEXT* Context, KSW_HOTKEY_RULE_RESULT* Result);
// 卸载证明交叉验证扫描、缓存失效和单槽释放。
int KswHkProofUnregister(KSW_HK_RULE_CONTEXT* Context, KSW_HOTKEY_RULE_RESULT* Result);
// 插入证明要求桶头读写与节点next字段同源。
int KswHkProofInsert(KSW_HK_RULE_CONTEXT* Context, uint32_t Entry,
    KSW_HOTKEY_RULE_RESULT* Result);
// 单槽证明要求摘链、释放及返回值生命周期完整。
int KswHkProofSlot(KSW_HK_RULE_CONTEXT* Context, uint32_t Entry,
    KSW_HOTKEY_RULE_RESULT* Result);
// 桶循环证明从实际CFG取得索引或指针归纳过程。
int KswHkProofLoop(KSW_HK_RULE_CONTEXT* Context,
    const KSW_RULE_FUNCTION* Function, const KSW_FLOW_ANALYSIS* Basis,
    const KSW_FLOW_STATE* Initial, KSW_FLOW_MODEL* Model,
    const KSW_HOTKEY_RULE_RESULT* Layout, uint32_t* AnchorIndex);
// 赋值锁证明只接受Base函数每条正常返回路径的对象槽写入。
int KswHkProofAssignment(KSW_HK_RULE_CONTEXT* Context, uint32_t* BaseRva);
// owner包装器只在同源返回且无外部副作用时规范化。
int KswHkCanonicalOwner(KSW_HK_RULE_CONTEXT* Context, KSW_FLOW_VALUE Input,
    KSW_FLOW_VALUE* Output);
// 证据函数保存实际指令字节与RVA供运行时复核。
int KswHkEvidence(KSW_HK_RULE_CONTEXT* Context, KSW_HOTKEY_RULE_RESULT* Result,
    const KSW_RULE_INSN* Instruction);
// 加载函数同时登记本次分析过的完整代码范围。
int KswHkLoad(KSW_HK_RULE_CONTEXT* Context, uint32_t Entry, KSW_RULE_FUNCTION* Function);
// 实参读取遵守当前调用点寄存器与栈帧位置。
KSW_FLOW_VALUE KswHkCallArgument(const KSW_FLOW_STATE* State, uint32_t Argument);
// 直接调用判断不根据符号名或版本猜测。
int KswHkCallDirect(const KSW_RULE_INSN* Instruction);
// 布局校验要求字段完整、对齐且互不重叠。
int KswHkLayoutValid(const KSW_HOTKEY_RULE_RESULT* Result);
// 大型结果从宿主分配，避免嵌套内核栈负担。
KSW_HOTKEY_RULE_RESULT* KswHkNewResult(KSW_HK_RULE_CONTEXT* Context);
