#pragma once

#include <ntddk.h>

typedef struct _KSW_KEYBOARD_HOTKEY_RUNTIME
{
    PVOID Win32kfullBase;
    ULONG Win32kfullSize;
    PVOID Win32kbaseBase;
    ULONG Win32kbaseSize;
    ULONG_PTR IsHotKeyAddress;
    ULONG_PTR SessionGlobals; // 实际USER namespace基址，必须与请求快照精确一致。
    ULONG_PTR UserSessionState; // 仅SG相对表/缓存使用的实际W32用户会话状态指针。
    ULONG SessionId; // 与namespace分开保存的实际进程会话身份。
    ULONG_PTR TableBase;
    ULONG TableOffset;
    ULONG NextOffset;
    ULONG ModifiersOffset;
    ULONG VkOffset;
    ULONG IdOffset;
    ULONG ObjectSize; // 规则证明的完整主节点大小，最大256字节。
    ULONG BucketCount; // 同一分桶读写关系证明的实际桶数。
    ULONG OwnerOffset; // 主节点内的安装线程字段。
    ULONG CallbackOffset; // 主节点内的回调字段。
    ULONG WindowOffset; // 主节点内的原始windowValue字段，不在快照中归一化。
    ULONG WindowRepresentation; // 0为HWND，1为有引用的tagWND指针；由两端数据流证明。
    ULONG DestinationOffset; // 主节点内的目标参数字段。
    ULONG DestinationWidth; // 零表示规则证明该节点没有Destination字段。
    ULONG FlagsOffset; // 主节点内部标志字段。
    ULONG ChildOffset; // 主节点双向子链表头字段。
    ULONG ModifiersWidth; // 已证明的修饰键字段宽度。
    ULONG FlagsWidth; // 已证明的内部标志字段宽度。
    ULONG VkWidth; // 已证明的虚拟键字段宽度。
    ULONG IdWidth; // 已证明的标识字段宽度。
    ULONG WindowObjectHandleOffset; // tagWND内对应HWND的只读字段。
    ULONG CacheOffset; // 用户会话内的派生缓存位置。
    ULONG CacheRva; // 映像内缓存位置，零表示使用会话相对位置。
    ULONG_PTR CacheAddress; // 每次请求重新绑定的缓存地址。
    ULONG RemoveRva; // 当前已加载Full内规则证明的删除函数。
    ULONG EnterCritRva; // 当前已加载Base内的临界区入口。
    ULONG LeaveCritRva; // 当前已加载Base内的临界区退出入口。
    ULONG ValidateHwndRva; // 当前已加载Base内的窗口校验入口。
    ULONG SessionGetterSlotRva; // Full实际使用的用户会话getter槽。
    ULONG SessionGetterProvider; // 0为W32，1为SGD；按实际导入语义选择，不按版本选择。
    ULONG_PTR SessionGetterAddress; // 当前WIN32K导出与Full IAT同时确认的getter。
    ULONG RuleProven; // 仅完整通用规则及代码指纹验证后置一。
    ULONG RuleGeneration; // 缓存代次只用于拒绝已过时的scalar副本。
    ULONG Win32kfullTimeDateStamp; // 仅为缓存身份和诊断，不是支持门槛。
} KSW_KEYBOARD_HOTKEY_RUNTIME, *PKSW_KEYBOARD_HOTKEY_RUNTIME;

struct _KSW_HOOK_SYSTEM_MODULE_ENTRY; // 模块视图由既有快照拥有。
struct _KSW_HOOK_SYSTEM_MODULE_INFORMATION; // 冷态语义绑定复用同一次已加载模块快照。

NTSTATUS KswordARKKeyboardResolveRuleSchema(
    _In_ const struct _KSW_HOOK_SYSTEM_MODULE_ENTRY* Full,
    _In_ const struct _KSW_HOOK_SYSTEM_MODULE_ENTRY* Base,
    _In_ const struct _KSW_HOOK_SYSTEM_MODULE_INFORMATION* Modules,
    _Out_ KSW_KEYBOARD_HOTKEY_RUNTIME* RuntimeOut); // 只复制scalar，不发布全局缓存指针。

NTSTATUS KswordARKKeyboardValidateRuleRuntime(
    _In_ const KSW_KEYBOARD_HOTKEY_RUNTIME* Runtime); // 不重新匹配CFG，只复核当前代码与缓存代次。

NTSTATUS KswordARKKeyboardGetSessionId(_Out_ ULONG* SessionIdOut); // 从当前或已附加进程取得实际SID，不混入指针或散列。

NTSTATUS KswordARKKeyboardBindHotkeyRuntimeContext(_Inout_ KSW_KEYBOARD_HOTKEY_RUNTIME* Runtime); // 只绑定本次namespace，不进行CFG证明或分页池分配。

NTSTATUS
KswordARKKeyboardResolveHotkeyRuntime(
    _Out_ KSW_KEYBOARD_HOTKEY_RUNTIME* RuntimeOut
    );

ULONG64
KswordARKKeyboardHashHotkeyObject(
    _In_reads_bytes_(ObjectBytes) const UCHAR* Object,
    _In_ SIZE_T ObjectBytes
    );
