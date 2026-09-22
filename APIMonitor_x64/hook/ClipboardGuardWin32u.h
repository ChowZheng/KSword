#pragma once

// ============================================================
// hook/ClipboardGuardWin32u.h
// 作用：
// 1) tier2：在 win32u.dll（Windows 10 起 user32 内部真正触发系统调用的那一层）
//    对剪贴板相关导出做拦截，防止只挂 user32 层被绕过；
// 2) 只在策略判定为"拦截"的方向才安装对应 hook——不拦截时压根不装，
//    因为不拦截的调用已经在 tier1（user32/ole32）层被完整记录，tier2 再装一份
//    "仅记录、原样放行"的 hook 只会重复上报同一次调用，没有额外信号；
// 3) 复用已公开的 InstallInlineHook/UninstallInlineHook，但入口 stub 是本模块
//    自己生成的最小 x64 桩——不读入参、不调用原函数、直接返回失败，因此完全
//    不需要知道 win32u 这些内部函数的真实签名（公开文档本来就没有稳定描述），
//    这一点和 HookTargets.cpp 里现有的 Fake-Success 桩是同一种安全原语。
// ============================================================

#include "pch.h"

namespace apimon
{
    // SyncClipboardWin32uHooks 作用：
    // - 处理：按当前剪贴板策略（ResolveClipboardAction）逐个检查
    //   Get/Set/EmptyClipboard 在 win32u.dll 里的对应导出，策略是 BLOCK 且还没装
    //   就装上，不是 BLOCK 且已经装了就卸掉；找不到对应导出（老系统/未来改名）
    //   直接跳过，不影响 tier1；
    // - 调用：会话建立时（InstallConfiguredHooks）与热更新轮询（MonitorAgent 的
    //   250ms 周期）各调用一次，函数本身幂等、可重复调用。
    void SyncClipboardWin32uHooks();

    // UninstallAllClipboardWin32uHooks 作用：卸载所有已安装的 tier2 hook，
    // 会话结束（UninstallConfiguredHooks）时调用。
    void UninstallAllClipboardWin32uHooks();
}
