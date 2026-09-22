#pragma once

// clipboard_policy_internal.h：本模块（剪贴板策略）内部共用的状态结构与声明，
// 只在 clipboard_policy_runtime.c / clipboard_policy_ioctl.c 之间共享，
// 不对外暴露——外部只能通过 ark/ark_clipboard_policy.h 声明的四个函数访问。
#include <ntifs.h>
#include <ntstrsafe.h>
#include <wdf.h>

#include "ark/ark_clipboard_policy.h"
#include "ark/ark_log.h"
#include "ark/ark_push_lock.h"

// 非分页内存分配打的池标签，倒过来读是 'KbPc'，用于 !poolused 时定位泄漏来源。
#define KSWORD_ARK_CLIPBOARD_POLICY_TAG_STATE 'cPbK'

// KswordArkAllocateNonPaged：驱动内共用的非分页分配器。
// 实际定义在 src/features/callback/callback_runtime.c，它会在可用时走
// ExAllocatePool2，否则回退到 ExAllocatePoolWithTag；这里只是复用声明，
// 不重复实现，避免出现两套内存分配策略。
PVOID
KswordArkAllocateNonPaged(
    _In_ SIZE_T bytes,
    _In_ ULONG poolTag
    );

// KSWORD_ARK_CLIPBOARD_POLICY_STATE：本模块运行时状态的全部内容。
// 与 ProcessProtect 的状态结构相比刻意精简：这里驱动只做"存一张表、原样
// 回显"，不在内核里执行任何判定，因此不需要台账、统计计数器或"最近一次
// 拦截"快照——那些字段只有驱动自己消费规则表时才有意义。
typedef struct _KSWORD_ARK_CLIPBOARD_POLICY_STATE
{
    // Device：控制设备句柄，仅用于把 Set/Query 的异常情况写进 R3 日志通道。
    WDFDEVICE Device;

    // ConfigLock：保护下面这一整块规则表；Set 时独占，Query 时共享。
    EX_PUSH_LOCK ConfigLock;
    // GlobalFlags：KSWORD_ARK_CLIPBOARD_POLICY_FLAG_* 的组合，目前只有总开关。
    ULONG GlobalFlags;
    // RuleCount：当前生效的规则条数，取值范围 [0, MAX_RULES]。
    ULONG RuleCount;
    // ConfigVersion：每次 Set 成功后自增，供 R3 判断规则表是否被别的客户端改过。
    ULONG64 ConfigVersion;
    // AppliedAtUtc100ns：最近一次 Set 成功时的系统时间。
    LARGE_INTEGER AppliedAtUtc100ns;
    // Rules：定长内嵌数组，不是链表——32 条上限决定了整块 memcpy 比链表遍历更简单。
    KSWORD_ARK_CLIPBOARD_POLICY_RULE Rules[KSWORD_ARK_CLIPBOARD_POLICY_MAX_RULES];
} KSWORD_ARK_CLIPBOARD_POLICY_STATE;

// KswordArkClipboardPolicyGetState：返回当前全局单例状态指针；
// 驱动尚未完成 Initialize 或已经 Uninitialize 时返回 NULL。
KSWORD_ARK_CLIPBOARD_POLICY_STATE*
KswordArkClipboardPolicyGetState(
    VOID
    );

// KswordArkClipboardPolicyLogFormat：向 R3 日志通道追加一条格式化文本；
// State 为 NULL 或 Device 未就绪时静默跳过，不影响调用方主流程。
VOID
KswordArkClipboardPolicyLogFormat(
    _In_ KSWORD_ARK_CLIPBOARD_POLICY_STATE* State,
    _In_z_ PCSTR LevelText,
    _In_z_ _Printf_format_string_ PCSTR FormatText,
    ...
    );
