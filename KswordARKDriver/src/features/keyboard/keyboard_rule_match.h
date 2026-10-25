#pragma once

#include "keyboard_rule_ir.h"

// 规则只描述结构与调用关系；这些值不是Windows版本号或PE身份名单。
// 规则版本只使缓存失效，不代表Windows版本支持表。
#define KSW_HOTKEY_RULE_VERSION 1U
// 节点及证据上限确保内核分配和检查有界。
#define KSW_HOTKEY_RULE_MAX_NODE 256U
#define KSW_HOTKEY_RULE_MAX_EVIDENCE 96U
// 分析函数数目有独立上限，超限作为预算错误。
#define KSW_HOTKEY_RULE_MAX_FUNCTIONS 128U

typedef enum KSW_HOTKEY_RULE_ERROR
{
    // 成功与映像读取、资源不足分别回报。
    KSW_HK_RULE_OK = 0,
    KSW_HK_RULE_INVALID_IMAGE,
    KSW_HK_RULE_RESOURCE,
    // 卸载、单槽释放、注册与插入的失败阶段分开标记。
    KSW_HK_RULE_UNREGISTER,
    KSW_HK_RULE_REMOVE,
    KSW_HK_RULE_REGISTER,
    KSW_HK_RULE_INSERT,
    // 布局冲突、候选歧义或分析预算耗尽都不发布schema。
    KSW_HK_RULE_LAYOUT,
    KSW_HK_RULE_AMBIGUOUS,
    KSW_HK_RULE_BUDGET
} KSW_HOTKEY_RULE_ERROR;

typedef struct KSW_HOTKEY_RULE_EVIDENCE
{
    uint32_t Rva; // Full映像内的实际指令位置。
    uint8_t Length; // 指令最多15字节。
    uint8_t Bytes[15]; // 运行时缓存再次使用前的原始证据。
} KSW_HOTKEY_RULE_EVIDENCE;

typedef struct KSW_HOTKEY_RULE_RESULT
{
    uint32_t RuleVersion; // 规则实现版本，只用于缓存失效。
    uint32_t Error; // 未证明时不提供可调用结果。
    uint32_t TableOffset; // 用户会话状态内的表偏移。
    uint32_t TableRva; // 非会话相对表的映像RVA，零表示使用TableOffset。
    uint32_t CacheOffset; // 同一会话状态内的缓存位。
    uint32_t CacheRva; // 映像内缓存位，零表示使用CacheOffset。
    uint32_t BucketCount; // 插入掩码与删除循环交叉证明的桶数。
    uint32_t ObjectSize; // 实际分配且字段均在范围内的节点大小。
    uint32_t OwnerOffset; // 安装线程来源字段。
    uint32_t CallbackOffset; // 注册回调参数字段。
    uint32_t WindowOffset; // 窗口句柄字段。
    uint32_t WindowRepresentation; // 0为HWND，1为经赋值锁持有的tagWND指针。
    uint32_t DestinationOffset; // 目标窗口参数字段。
    uint32_t DestinationWidth; // 零表示经构造器证明没有此字段，不能读取OwnerOffset。
    uint32_t ModifiersOffset; // 修饰键字段。
    uint32_t FlagsOffset; // 内部标志字段。
    uint32_t VkOffset; // 虚拟键字段。
    uint32_t IdOffset; // 注册标识字段。
    uint32_t NextOffset; // 相同节点来源的哈希链字段。
    uint32_t ChildOffset; // 两个自指针共同证明的子项链表头。
    uint32_t ModifiersWidth; // 实际完整字段写入宽度。
    uint32_t FlagsWidth; // 内部标志的实际宽度。
    uint32_t VkWidth; // 虚拟键的实际宽度。
    uint32_t IdWidth; // 注册标识的实际宽度。
    uint32_t WindowObjectHandleOffset; // 注册/删除对窗口对象中同一字段的读取。
    uint32_t RemoveRva; // 接收节点链槽地址的一参摘链/释放helper；归属筛选由ScanRva证明。
    uint32_t ScanRva; // 包含完整归属筛选及单节点调用的扫描函数。
    uint32_t UnregisterRva; // 公开卸载入口的内部包装函数。
    uint32_t RegisterRva; // 实际分配和初始化节点的注册函数。
    uint32_t InsertRva; // 同一表内读写链头的插入函数。
    uint32_t OwnerGetterRva; // 注册和卸载共同使用的安装线程来源。
    uint32_t OwnerGetterIndirect; // 0为Full的callee RVA，1为函数IAT槽，2为数据IAT槽。
    uint32_t OwnerGetterDepth; // 安装线程来源的实际解引用深度。
    uint32_t EnterCritRva; // Base实际导出的临界区入口。
    uint32_t LeaveCritRva; // Base实际导出的退出入口。
    uint32_t ValidateHwndRva; // Base实际导出的窗口验证入口。
    uint32_t AssignmentRva; // 经全路径验证的Base栈描述符赋值锁，可为零。
    uint32_t SessionGetterSlotRva; // Full实际调用的W32或SGD用户会话getter IAT槽。
    uint32_t SessionGetterProvider; // 0为W32，1为SGD；由实际导入及同基址读写关系决定。
    uint32_t AnalyzedFunctionCount; // 连同排除其它候选所使用的代码范围一并校验缓存。
    uint32_t AnalyzedFunctions[KSW_HOTKEY_RULE_MAX_FUNCTIONS]; // Full的已解码函数入口，不是版本地址表。
    uint32_t EvidenceCount; // 只发布完整规则所需的有界证据。
    KSW_HOTKEY_RULE_EVIDENCE Evidence[KSW_HOTKEY_RULE_MAX_EVIDENCE]; // 不含主机版本授权信息。
} KSW_HOTKEY_RULE_RESULT;

// 对同一实际映像应用通用规则；成功与否不依赖版本、时间戳、GUID或SHA白名单。
int KswHotkeyMatchRules(const KSW_RULE_IMAGE* Full, const KSW_RULE_IMAGE* Base,
    KSW_HOTKEY_RULE_RESULT* Result);
