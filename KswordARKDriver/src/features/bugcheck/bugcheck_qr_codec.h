#pragma once

#include "bugcheck_internal.h"
#include "bugcheck_bgp.h"
#include "bugcheck_evidence.h"

#define KSWORD_ARK_QR_SCHEMA_VERSION 2UL // KSQ2 独立二进制格式版本。
#define KSWORD_ARK_QR_BINARY_CAPACITY 2860UL // Base45 加五字符前缀后可容纳的最大完整二进制包。
#define KSWORD_ARK_QR_TEXT_CAPACITY 4297UL // QR 字母数字模式的 4296 字符及结尾 NUL。
#define KSWORD_ARK_QR_HEADER_BYTES 16UL // Magic、schema、flags、length、CRC32 四部分。
#define KSWORD_ARK_QR_FLAG_BGP 1UL // 包内存在完整 BGP 快照。
#define KSWORD_ARK_QR_FLAG_EVIDENCE 2UL // 包内存在完整扩展证据快照。

// 全部输出缓冲由调用方预先驻留；逐字段序列化，不复制结构 padding，不申请内存。
NTSTATUS KswordARKBugcheckQrEncode(
    _In_ const KSWORD_ARK_BUGCHECK_DIAGNOSTICS* Diagnostics,
    _In_ ULONG CallbackMask,
    _In_ ULONG ModuleCount,
    _In_opt_ const KSWORD_ARK_BGP_DUMP_STATE* Bgp,
    _In_opt_ const KSWORD_BUGCHECK_EVIDENCE* Evidence,
    _Out_writes_bytes_(BinaryCapacity) UCHAR* Binary,
    _In_ ULONG BinaryCapacity,
    _Out_writes_bytes_(TextCapacity) CHAR* Text,
    _In_ ULONG TextCapacity,
    _Out_ PULONG BinaryLength,
    _Out_ PULONG TextLength
    );

// 离线工具复用 CRC 算法验证边界；驱动编码时同样只执行有界整数操作。
ULONG KswordARKBugcheckQrCrc32(_In_reads_bytes_(Length) const UCHAR* Data, _In_ ULONG Length);
