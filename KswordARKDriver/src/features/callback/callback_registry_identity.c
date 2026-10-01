/* Registry Cookie 布局通过本驱动活跃注册校准，不能把链节点当 Cookie。 */
#include "callback_internal.h" // 共享行协议与 runtime。

extern NTSTATUS KswordArkRegistryCallback(PVOID, PVOID, PVOID); // 本驱动已知注册函数，用于校准。

BOOLEAN KswordArkCallbackRegistryReadIdentity( // 读取固定 x64 前缀的三个标量。
    _In_ ULONG64 Node, _Out_ ULONG64* Cookie, _Out_ ULONG64* Context, _Out_ ULONG64* Callback
    ) // 结束参数。
{ // 开始有界读取。
    ULONG64 prefix[6]; // Link、两个 ULONG、Cookie、Context、Function 共 0x30 字节。
    if (!KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)Node, prefix, sizeof(prefix))) { return FALSE; } // 统一安全读取，不解引用 R3 地址。
    *Cookie = prefix[3]; *Context = prefix[4]; *Callback = prefix[5]; // 分别恢复 +0x18/+0x20/+0x28。
    return *Cookie != 0ULL && *Callback != 0ULL; // 零 Cookie 或零函数不具有注销语义。
} // 结束前缀读取。

BOOLEAN KswordArkCallbackRegistryLayoutValidated(_In_ ULONG64 Head) // 使用当前仍活跃的自注册校准整条链。
{ // 开始校准。
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime(); // 只使用已发布的当前 runtime。
    LIST_ENTRY link; // 保存每个节点的链路。
    ULONG64 node = 0ULL; // 当前注册节点。
    ULONG64 previous = Head; // 验证 reciprocal backward link。
    ULONG count = 0UL; // 有界遍历。
    ULONG matches = 0UL; // 要求自注册三元组唯一出现。
    if (runtime == NULL || runtime->RegistryCookie.QuadPart == 0LL ||
        (runtime->RegisteredCallbacksMask & KSWORD_ARK_CALLBACK_REGISTERED_REGISTRY) == 0UL ||
        !KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)Head, &link, sizeof(link))) { return FALSE; } // 旧 Cookie 不能独立证明布局。
    node = (ULONG64)(ULONG_PTR)link.Flink; // 从当前链头开始。
    while (node != Head && node != 0ULL && count < 512UL) { // 不接受超限或非终止链。
        ULONG64 cookie = 0ULL, context = 0ULL, callback = 0ULL; // 清空当前注册标量。
        if (!KswordArkCallbackEnumReadMemory((PVOID)(ULONG_PTR)node, &link, sizeof(link)) ||
            (ULONG64)(ULONG_PTR)link.Blink != previous ||
            !KswordArkCallbackRegistryReadIdentity(node, &cookie, &context, &callback)) { return FALSE; } // 任何节点损坏不发布校准。
        if (cookie == (ULONG64)runtime->RegistryCookie.QuadPart &&
            context == (ULONG64)(ULONG_PTR)runtime && callback == (ULONG64)(ULONG_PTR)KswordArkRegistryCallback) { ++matches; } // 自身 API 返回值、Context、函数三者共同证明布局。
        previous = node; node = (ULONG64)(ULONG_PTR)link.Flink; ++count; // 用本地链路前进。
    } // 结束遍历。
    return node == Head && matches == 1UL; // 只有完整链中的唯一自注册可校准。
} // 结束布局校准。

VOID KswordArkCallbackRegistryRemoved(_In_ ULONG64 Cookie) // 同步通过 ARK 注销的本驱动自注册状态。
{ // 开始自身状态同步。
    KSWORD_ARK_CALLBACK_RUNTIME* runtime = KswordArkCallbackGetRuntime(); // 不影响其它驱动的状态。
    if (runtime != NULL && (ULONG64)runtime->RegistryCookie.QuadPart == Cookie) { // 只匹配当前注册。
        runtime->RegistryCookie.QuadPart = 0LL; // 避免卸载阶段再次使用已经注销的 Cookie。
        (VOID)InterlockedAnd((volatile LONG*)&runtime->RegisteredCallbacksMask,
            (LONG)~KSWORD_ARK_CALLBACK_REGISTERED_REGISTRY); // 自身枚举同步显示未注册。
    } // 结束匹配。
} // 结束自身状态同步。
