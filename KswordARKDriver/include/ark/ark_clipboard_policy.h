#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "driver/KswordArkClipboardPolicyIoctl.h"

EXTERN_C_START

/*
 * KswordARKClipboardPolicyInitialize
 * Inputs:
 * - Device 是控制设备，仅用于把配置变更写进 R3 日志通道。
 * Processing:
 * - 分配并发布剪贴板策略表的宿主状态。本模块不挂任何内核回调，
 *   Initialize 失败只影响这一张表能不能被查询/下发，不影响驱动其余功能。
 * Return behavior:
 * - 分配失败返回 STATUS_INSUFFICIENT_RESOURCES。
 */
NTSTATUS
KswordARKClipboardPolicyInitialize(
    _In_ WDFDEVICE Device
    );

/*
 * KswordARKClipboardPolicyUninitialize
 * Processing:
 * - 释放策略表状态。本模块没有注册任何回调，卸载顺序上不依赖其它模块先注销。
 */
VOID
KswordARKClipboardPolicyUninitialize(
    VOID
    );

/*
 * KswordARKClipboardPolicyIoctlSetConfig
 * Processing:
 * - 校验并整体替换剪贴板策略规则表。配置是一次性全量替换，不做增量合并。
 * - 驱动本身不消费这张表——它只是被注入到受管进程里的用户态 Agent DLL
 *   查询"这个进程当前允许还是拦截剪贴板读/写/枚举"的权威来源。
 */
NTSTATUS
KswordARKClipboardPolicyIoctlSetConfig(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

/*
 * KswordARKClipboardPolicyIoctlQueryState
 * Processing:
 * - 回读当前策略规则表，供 R3 UI 重新填充规则表格或供 CLI 核对。
 */
NTSTATUS
KswordARKClipboardPolicyIoctlQueryState(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _Out_ size_t* CompleteBytesOut
    );

EXTERN_C_END
