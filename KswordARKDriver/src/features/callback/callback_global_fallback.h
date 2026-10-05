#pragma once // 私有全局回退仅供只读回调枚举使用。

#include "../../platform/runtime_signature_scan.h" // 复用安全读取、PE 节和函数边界扫描器。

typedef enum _KSW_CALLBACK_GLOBAL_FAMILY // 每个家族使用各自公开注册和移除导出。
{
    KswCallbackGlobalProcess = 1, // 进程通知 EX_FAST_REF 数组。
    KswCallbackGlobalThread = 2, // 线程通知 EX_FAST_REF 数组。
    KswCallbackGlobalImage = 3, // 映像通知 EX_FAST_REF 数组。
    KswCallbackGlobalRegistry = 4 // 经自注册校准的 Registry 双向链。
} KSW_CALLBACK_GLOBAL_FAMILY; // 只在驱动内部使用，不构成 R0/R3 协议。

typedef BOOLEAN (*KSW_CALLBACK_GLOBAL_EXECUTABLE_PROBE)( // 调用者验证回调所属模块的可执行节。
    _In_opt_ PVOID Context, _In_ ULONG_PTR Address); // 不把模块数据指针当成回调。
typedef BOOLEAN (*KSW_CALLBACK_GLOBAL_REGISTRY_LAYOUT_PROBE)( // 调用者提供当前自 Cookie 校准。
    _In_opt_ PVOID Context, _In_ ULONG_PTR Head); // 无当前布局证明时不能解释节点前缀。

NTSTATUS KswordArkCallbackGlobalFallbackResolve( // 不写内核、不创建注册、不改变删除信任。
    _In_ const KSW_RUNTIME_IMAGE_VIEW* View, // 当前内核的已验证 PE 视图。
    _In_ KSW_CALLBACK_GLOBAL_FAMILY Family, // 选择公开导出锚点。
    _In_ ULONG_PTR KnownCallback, // 本驱动当前注册函数；零表示没有自注册身份依据。
    _In_ KSW_CALLBACK_GLOBAL_EXECUTABLE_PROBE ExecutableProbe, // 验证任意已加载驱动的代码节。
    _In_opt_ KSW_CALLBACK_GLOBAL_REGISTRY_LAYOUT_PROBE RegistryLayoutProbe, // Registry 必须提供校准。
    _In_opt_ PVOID ProbeContext, // 传给两个验证器的只读上下文。
    _Out_ ULONG_PTR* AddressOut); // 唯一完整有效候选，否则输出零。
