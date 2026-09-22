#pragma once

#include "KswordArkProcessIoctl.h"

// ============================================================
// KswordArkClipboardPolicyIoctl.h
// 作用：
// - 定义"剪贴板访问策略"唯一 R3/R0 协议；
// - 驱动只做策略的存储与查询，不在内核里挂任何回调，也不参与实际拦截判定——
//   真正的拦截发生在被注入目标进程的用户态 Agent DLL 里，Agent 通过本协议
//   查询"这个受保护进程当前的读/写/枚举策略是什么"；
// - 与 KswordArkProcessProtectIoctl.h 的"目标匹配+访问位掩码"结构同构，
//   但语义完全不同：ProcessProtect 削的是句柄访问权限，这里描述的是
//   "剪贴板读/写/枚举各自允许、拦截还是仅记录"。
// 说明：
// - 配置为一次性全量替换，不做增量下发，避免 R3/R0 两侧规则表漂移；
// - 目标进程当前是否命中某条规则、命中后要不要真的拦截，全部由 Agent DLL
//   自行判定，本协议只负责把规则表原样送到 R3 再原样取回。
// ============================================================

#define KSWORD_ARK_CLIPBOARD_POLICY_PROTOCOL_VERSION 1UL

#define KSWORD_ARK_IOCTL_FUNCTION_SET_CLIPBOARD_POLICY   0x91AUL
#define KSWORD_ARK_IOCTL_FUNCTION_QUERY_CLIPBOARD_POLICY 0x91BUL

#define IOCTL_KSWORD_ARK_SET_CLIPBOARD_POLICY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_SET_CLIPBOARD_POLICY, \
        METHOD_BUFFERED, \
        FILE_WRITE_ACCESS)

#define IOCTL_KSWORD_ARK_QUERY_CLIPBOARD_POLICY \
    CTL_CODE( \
        KSWORD_ARK_IOCTL_DEVICE_TYPE, \
        KSWORD_ARK_IOCTL_FUNCTION_QUERY_CLIPBOARD_POLICY, \
        METHOD_BUFFERED, \
        FILE_ANY_ACCESS)

// ------------------------------------------------------------
// 容量上限
// ------------------------------------------------------------
// 请求与响应都是定长 METHOD_BUFFERED 包，容量直接决定内核非分页缓冲区大小。
#define KSWORD_ARK_CLIPBOARD_POLICY_MAX_RULES 32UL
#define KSWORD_ARK_CLIPBOARD_POLICY_IMAGE_CHARS 260U
#define KSWORD_ARK_CLIPBOARD_POLICY_NAME_CHARS 64U

// ------------------------------------------------------------
// 目标匹配方式（与 ProcessProtect 同构）
// ------------------------------------------------------------
// PID       ：只匹配一个具体进程，进程退出后该规则自然失效。
// IMAGE_NAME：匹配映像文件名（不含目录），忽略大小写，例如 notepad.exe。
// IMAGE_PATH：匹配映像完整路径，去盘符后大小写无关后缀匹配。
#define KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_NONE 0UL
#define KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_PID 1UL
#define KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_IMAGE_NAME 2UL
#define KSWORD_ARK_CLIPBOARD_POLICY_TARGET_KIND_IMAGE_PATH 3UL

// ------------------------------------------------------------
// 动作：读/写/枚举各自独立取值，不是一个总开关
// ------------------------------------------------------------
// ALLOW   ：放行，仍然上报事件。
// BLOCK   ：不调用原始剪贴板 API，直接向调用方返回失败。
// LOG_ONLY：等价 ALLOW，但用于 UI 区分"用户显式选了仅记录"与"默认放行"。
#define KSWORD_ARK_CLIPBOARD_POLICY_ACTION_ALLOW 0UL
#define KSWORD_ARK_CLIPBOARD_POLICY_ACTION_BLOCK 1UL
#define KSWORD_ARK_CLIPBOARD_POLICY_ACTION_LOG_ONLY 2UL

// ------------------------------------------------------------
// 规则标志
// ------------------------------------------------------------
// ENABLED：未置位的规则会被 Agent 跳过，但仍占用一个槽位，便于 R3 保留草稿。
#define KSWORD_ARK_CLIPBOARD_POLICY_RULE_FLAG_ENABLED 0x00000001UL

// ------------------------------------------------------------
// 全局标志
// ------------------------------------------------------------
// ENABLED：总开关。关闭后驱动保留规则表，但 R3 侧应当视为"无策略"处理。
#define KSWORD_ARK_CLIPBOARD_POLICY_FLAG_ENABLED 0x00000001UL

typedef struct _KSWORD_ARK_CLIPBOARD_POLICY_RULE
{
    unsigned long ruleId;
    unsigned long flags;
    unsigned long targetKind;
    // targetKind 为 PID 时生效；其余匹配方式下必须为 0。
    unsigned long targetProcessId;
    // 三个方向各自独立取值，取 KSWORD_ARK_CLIPBOARD_POLICY_ACTION_*。
    unsigned long readAction;
    unsigned long writeAction;
    unsigned long enumAction;
    unsigned long reserved;
    wchar_t targetImage[KSWORD_ARK_CLIPBOARD_POLICY_IMAGE_CHARS];
    wchar_t ruleName[KSWORD_ARK_CLIPBOARD_POLICY_NAME_CHARS];
} KSWORD_ARK_CLIPBOARD_POLICY_RULE;

typedef struct _KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST
{
    unsigned long size;
    unsigned long version;
    unsigned long globalFlags;
    unsigned long ruleCount;
    KSWORD_ARK_CLIPBOARD_POLICY_RULE rules[KSWORD_ARK_CLIPBOARD_POLICY_MAX_RULES];
} KSWORD_ARK_CLIPBOARD_POLICY_CONFIG_REQUEST;

typedef struct _KSWORD_ARK_CLIPBOARD_POLICY_STATE_RESPONSE
{
    unsigned long size;
    unsigned long version;
    unsigned long globalFlags;
    unsigned long ruleCount;
    // 每次 SET 成功后递增，供 R3 判断规则表是否被别的客户端改过。
    unsigned long long configVersion;
    unsigned long long appliedAtUtc100ns;
    KSWORD_ARK_CLIPBOARD_POLICY_RULE rules[KSWORD_ARK_CLIPBOARD_POLICY_MAX_RULES];
} KSWORD_ARK_CLIPBOARD_POLICY_STATE_RESPONSE;
