#pragma once

#include <ntddk.h>

EXTERN_C_START

// 查询私有快照只保存已验证的记录地址和边界，不持有 DynData 全局状态。
typedef struct _KSW_MM_UNLOADED_LAYOUT
{
    PVOID Records; // 运行时记录数组地址；每次读取仍使用 MmCopyMemory。
    ULONG RecordSize; // 一条记录的已验证步长。
    ULONG NameOffset; // 有界 UNICODE_STRING 字段偏移。
    ULONG StartAddressOffset; // 有界映像起始地址偏移。
    ULONG EndAddressOffset; // 有界映像结束地址偏移。
    ULONG CurrentTimeOffset; // 有界卸载时间偏移。
} KSW_MM_UNLOADED_LAYOUT;

// PiDDB 快照仍要求表和资源分别通过地址及对象类型校验。
typedef struct _KSW_PIDDB_QUERY_LAYOUT
{
    PRTL_AVL_TABLE Table; // 当前内核映像中的已验证 AVL 表。
    PERESOURCE Lock; // 取锁前还必须证明其属于系统资源链。
    ULONG DriverNameOffset; // 有界驱动名偏移。
    ULONG TimeDateStampOffset; // 有界 PE 时间戳偏移。
    ULONG LoadStatusOffset; // 有界加载状态偏移。
    ULONG EntrySize; // 私有条目的已验证最小边界。
} KSW_PIDDB_QUERY_LAYOUT;

// 接受 PDB 或运行时验证的单项布局，不以整体 NtosActive 代替单项校验。
NTSTATUS KswordARKUnloadedResolveMmLayout(_Out_ KSW_MM_UNLOADED_LAYOUT* Layout);

// PiDDB 只使用已经验证的条目布局；缺失布局继续拒绝查询。
NTSTATUS KswordARKUnloadedResolvePiDdbLayout(_Out_ KSW_PIDDB_QUERY_LAYOUT* Layout);

EXTERN_C_END
