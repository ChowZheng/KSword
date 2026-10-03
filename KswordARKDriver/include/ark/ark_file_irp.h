#pragma once

#include <ntddk.h>
#include "driver/KswordArkFileIrpIoctl.h"

EXTERN_C_START

// 删除内部接口：不新增 R0/R3 协议，不接受用户态提供的对象地址。
BOOLEAN KswordARKDriverShouldRetryDeleteByIrp(_In_ NTSTATUS Status);

// 使用 MM 清理可释放的映像 section；不修改文件系统共享的 section 指针。
NTSTATUS KswordARKDriverFlushFileObjectForDelete(_In_ PFILE_OBJECT FileObject);

// 显式强制删除后端专用：清空共享 section 三成员，调用方必须持有对象引用。
NTSTATUS KswordARKDriverClearFileObjectSectionsForDelete(_In_ PFILE_OBJECT FileObject);

// 在同一已引用对象上执行 Ex/传统类兼容和强制 section 清空后的单次重试。
NTSTATUS KswordARKDriverDeleteFileObjectByIrp(
    _In_ PFILE_OBJECT FileObject,
    _In_ BOOLEAN IgnoreReadOnly,
    _Out_ PBOOLEAN CancelledOut);

// 在已引用对象上直发基础文件系统 SET_INFORMATION；调用方保有对象并负责关闭。
NTSTATUS KswordARKDriverSetDispositionByIrp(
    _In_ PFILE_OBJECT FileObject,
    _In_ ULONG InformationClass,
    _In_reads_bytes_(InputBytes) const void* InputData,
    _In_ ULONG InputBytes,
    _Out_ PBOOLEAN CancelledOut);

// 忽略共享模式的删除阶段重试：复用打开句柄，Ex 不支持时兼容传统删除类。
NTSTATUS KswordARKDriverDeleteHandleByIrp(
    _In_ HANDLE FileHandle,
    _In_ BOOLEAN IgnoreReadOnly);

/*
 * KswordARKDriverSubmitFileIrp
 * Inputs:
 * - Request 是 IOCTL handler 已复制并完成边界校验的固定请求快照，紧随其后的
 *   内联输入数据由 InputData/InputBytes 单独传入，避免后端再次触碰
 *   METHOD_BUFFERED 系统缓冲。
 * Processing:
 * - 按 targetLayer 解析目标设备栈，自行分配 IRP、填写 IO_STACK_LOCATION，
 *   通过 IoCallDriver/PoCallDriver 直发目标驱动，并以带超时的完成事件同步等待。
 * Return behavior:
 * - 缓冲本身可用时返回 STATUS_SUCCESS，具体语义写入响应的 status/各阶段
 *   NTSTATUS；只有缓冲或参数根本不可用时才返回失败 NTSTATUS。
 */
NTSTATUS
KswordARKDriverSubmitFileIrp(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST* Request,
    _In_reads_bytes_opt_(InputBytes) const void* InputData,
    _In_ ULONG InputBytes,
    _Out_ size_t* BytesWrittenOut
    );

// FileDock IRP 删除预设的内部入口；通用 R3 IRP 请求不会启用强制 section 清空。
NTSTATUS KswordARKDriverSubmitFileDeleteIrp(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_FILE_IRP_SUBMIT_REQUEST* Request,
    _In_reads_bytes_(InputBytes) const void* InputData,
    _In_ ULONG InputBytes,
    _Out_ size_t* BytesWrittenOut);

/*
 * KswordARKDriverEnumerateDirectoryByIrp
 * Inputs:
 * - Request 指定目录 NT 路径、分页窗口以及要绕过的栈层。
 * Processing:
 * - 用自建 IRP_MJ_DIRECTORY_CONTROL/IRP_MN_QUERY_DIRECTORY 直发目标层，
 *   把 FILE_ID_BOTH_DIR_INFORMATION 链转换为与 ZwQueryDirectoryFile 路径
 *   完全相同的固定协议行，便于 R3 做逐行差集。
 * Return behavior:
 * - 与 KswordARKDriverEnumerateDirectory 一致：缓冲有效即返回 STATUS_SUCCESS，
 *   目录语义结果保存在 queryStatus/lastStatus。
 */
NTSTATUS
KswordARKDriverEnumerateDirectoryByIrp(
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _In_ const KSWORD_ARK_FILE_IRP_ENUM_DIRECTORY_REQUEST* Request,
    _Out_ size_t* BytesWrittenOut
    );

EXTERN_C_END
