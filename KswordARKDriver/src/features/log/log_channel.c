/*++

Module Name:

    log_channel.c

Abstract:

    This file contains log ring buffer helper functions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "ark/ark_driver.h"
#include "log_channel.tmh"

#include <ntstrsafe.h>

#include "KswordArkLogProtocol.h"
#include "../bugcheck/bugcheck_evidence.h" // 独立镜像有严重度的原始日志，不从崩溃路径读取 WDF 队列。

// Increment ring index and wrap around at queue capacity.
static ULONG
KswordARKDriverAdvanceRingIndex(
    _In_ ULONG currentIndex
    )
{
    return (currentIndex + 1U) % KSWORD_ARK_LOG_RING_CAPACITY;
}

NTSTATUS
KswordARKDriverInitializeLogChannel(
    _In_ WDFDEVICE Device
    )
{
    PDEVICE_CONTEXT deviceContext = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES spinLockAttributes;

    if (Device == WDF_NO_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = DeviceGetContext(Device);
    if (deviceContext == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (deviceContext->LogQueueLock != WDF_NO_HANDLE) {
        return STATUS_SUCCESS;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&spinLockAttributes);
    spinLockAttributes.ParentObject = Device;
    status = WdfSpinLockCreate(&spinLockAttributes, &deviceContext->LogQueueLock);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfSpinLockCreate failed %!STATUS!", status);
        return status;
    }

    deviceContext->LogQueueHeadIndex = 0U;
    deviceContext->LogQueueTailIndex = 0U;
    deviceContext->LogQueueCount = 0U;
    RtlZeroMemory(deviceContext->LogEntryLength, sizeof(deviceContext->LogEntryLength));
    RtlZeroMemory(deviceContext->LogEntryText, sizeof(deviceContext->LogEntryText));
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnqueueLogLine(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR FormattedLogLine
    )
{
    PDEVICE_CONTEXT deviceContext = NULL;
    size_t lineLengthBytes = 0;
    ULONG slotIndex = 0;
    ULONG traceSeverity = 0UL; // 文字日志严重度：1=Warn、2=Error、3=Fatal，零不镜像。

    if (Device == WDF_NO_HANDLE || FormattedLogLine == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = DeviceGetContext(Device);
    if (deviceContext == NULL || deviceContext->LogQueueLock == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (!NT_SUCCESS(RtlStringCbLengthA(
        FormattedLogLine,
        KSWORD_ARK_LOG_ENTRY_MAX_BYTES,
        &lineLengthBytes))) {
        lineLengthBytes = KSWORD_ARK_LOG_ENTRY_MAX_BYTES - 1U;
    }

    // 只镜像有真实日志级别前缀的 Warn/Error/Fatal；文字不提供原操作 NTSTATUS。
    if (lineLengthBytes >= 6U && RtlCompareMemory(FormattedLogLine, "[Warn]", 6U) == 6U) { // 固定前缀比较不读取声明范围之外。
        traceSeverity = 1UL; // 警告日志由类别和 Code 表达，不推断驱动故障。
    } else if (lineLengthBytes >= 7U && RtlCompareMemory(FormattedLogLine, "[Error]", 7U) == 7U) { // 只识别项目实际 Error 文本格式。
        traceSeverity = 2UL; // 错误文字日志单独保留，其原操作状态仍缺失。
    } else if (lineLengthBytes >= 7U && RtlCompareMemory(FormattedLogLine, "[Fatal]", 7U) == 7U) { // 严重日志同样保存原文来源。
        traceSeverity = 3UL; // 不将 Fatal 等同当前 BugCheck 根因。
    } // 普通 Info/Debug 不挤占最近六条诊断事件。
    if (traceSeverity != 0UL) { // 仅对上述三个实际日志级别执行镜像。
        KswordARKBugcheckTraceRecord( // 在 WDF 队列锁之外调用，trace 自身只有一次 CAS 尝试。
            KSWORD_BUGCHECK_TRACE_KIND_DRIVER_LOG, // 明确为文字日志，不冒充 IOCTL 返回或寄存器现场。
            traceSeverity, // 保存原始文本级别的稳定映射。
            STATUS_NOT_SUPPORTED, // 文本日志无法提供原操作 NTSTATUS，事件标志会说明缺失。
            FormattedLogLine, // 使用既有长度校验通过的真实日志文本。
            (ULONG)lineLengthBytes); // 原字节上限由 log ring 常量限定，不做额外全长格式化。
    } // 镜像完成后仍执行原有日志入队与所有权流程。
    WdfSpinLockAcquire(deviceContext->LogQueueLock);

    slotIndex = deviceContext->LogQueueTailIndex;
    RtlZeroMemory(deviceContext->LogEntryText[slotIndex], KSWORD_ARK_LOG_ENTRY_MAX_BYTES);
    if (lineLengthBytes > 0U) {
        RtlCopyMemory(deviceContext->LogEntryText[slotIndex], FormattedLogLine, lineLengthBytes);
    }
    deviceContext->LogEntryText[slotIndex][lineLengthBytes] = '\0';
    deviceContext->LogEntryLength[slotIndex] = (ULONG)lineLengthBytes;

    if (deviceContext->LogQueueCount == KSWORD_ARK_LOG_RING_CAPACITY) {
        deviceContext->LogQueueHeadIndex =
            KswordARKDriverAdvanceRingIndex(deviceContext->LogQueueHeadIndex);
    }
    else {
        deviceContext->LogQueueCount += 1U;
    }

    deviceContext->LogQueueTailIndex =
        KswordARKDriverAdvanceRingIndex(deviceContext->LogQueueTailIndex);

    WdfSpinLockRelease(deviceContext->LogQueueLock);
    return STATUS_SUCCESS;
}

NTSTATUS
KswordARKDriverEnqueueLogFrame(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR LevelText,
    _In_z_ PCSTR MessageText
    )
{
    CHAR frameBuffer[KSWORD_ARK_LOG_ENTRY_MAX_BYTES] = { 0 };
    PCSTR safeLevelText = (LevelText != NULL) ? LevelText : "Info";
    PCSTR safeMessageText = (MessageText != NULL) ? MessageText : "";
    NTSTATUS status = STATUS_SUCCESS;

    status = RtlStringCbPrintfA(
        frameBuffer,
        sizeof(frameBuffer),
        "[%s]%s%s",
        safeLevelText,
        safeMessageText,
        KSWORD_ARK_LOG_END_MARKER);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "RtlStringCbPrintfA failed %!STATUS!", status);
        return status;
    }

    return KswordARKDriverEnqueueLogLine(Device, frameBuffer);
}

NTSTATUS
KswordARKDriverReadNextLogLine(
    _In_ WDFDEVICE Device,
    _Out_writes_bytes_(OutputBufferLength) PVOID OutputBuffer,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* BytesWrittenOut
    )
{
    PDEVICE_CONTEXT deviceContext = NULL;
    ULONG slotIndex = 0;
    ULONG lineLength = 0;

    if (BytesWrittenOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *BytesWrittenOut = 0U;

    if (Device == WDF_NO_HANDLE || OutputBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    deviceContext = DeviceGetContext(Device);
    if (deviceContext == NULL || deviceContext->LogQueueLock == WDF_NO_HANDLE) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    WdfSpinLockAcquire(deviceContext->LogQueueLock);

    if (deviceContext->LogQueueCount == 0U) {
        WdfSpinLockRelease(deviceContext->LogQueueLock);
        return STATUS_NO_MORE_ENTRIES;
    }

    slotIndex = deviceContext->LogQueueHeadIndex;
    lineLength = deviceContext->LogEntryLength[slotIndex];
    if (OutputBufferLength < lineLength) {
        WdfSpinLockRelease(deviceContext->LogQueueLock);
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (lineLength > 0U) {
        RtlCopyMemory(OutputBuffer, deviceContext->LogEntryText[slotIndex], lineLength);
    }
    *BytesWrittenOut = lineLength;

    deviceContext->LogEntryLength[slotIndex] = 0U;
    deviceContext->LogEntryText[slotIndex][0] = '\0';
    deviceContext->LogQueueHeadIndex =
        KswordARKDriverAdvanceRingIndex(deviceContext->LogQueueHeadIndex);
    deviceContext->LogQueueCount -= 1U;

    WdfSpinLockRelease(deviceContext->LogQueueLock);
    return STATUS_SUCCESS;
}
