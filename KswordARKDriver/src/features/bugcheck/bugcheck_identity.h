#pragma once // 映像身份只在正常运行期解析。
#include "bugcheck_evidence.h" // 使用唯一固定证据结构。

// 仅通过 RuntimeReadMemory 读取有界映像，崩溃路径不调用本函数。
VOID KswordARKBugcheckIdentityRead(_In_ ULONG_PTR Base, _In_ ULONG Size,
    _In_opt_ PCUNICODE_STRING Path, _Out_ KSWORD_BUGCHECK_IMAGE_ID* Identity);
