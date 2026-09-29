#pragma once

// ============================================================
// hook/ClipboardGuardVTableHook.h
// 作用：
// 1) 对 OleGetClipboard 返回的 IDataObject 做虚表补丁，覆盖 GetData/
//    QueryGetData/EnumFormatEtc 三个槽位——这是 OLE 剪贴板路径唯一真正
//    搬运数据的入口，OleGetClipboard 本身只是拿到接口指针；
// 2) 本仓库首次做 COM 虚表级 hook，不复用 HookEngine 的 inline hook 机制
//    （那套是为"覆盖导出函数入口 + 建 trampoline"设计的，虚表槽位替换只是
//    单个对齐指针的原子写入，更轻量，也不需要 HookEngine 那套反汇编前导
//    长度计算）；
// 3) 按虚表指针去重：同一 COM 类的多个对象实例通常共享同一份虚表，只需
//    补一次；补丁信息（原函数指针）按虚表指针存表，供三个 Hooked 函数
//    反查"这份虚表对应的真实实现在哪"。
// ============================================================

#include "pch.h"

#include <objidl.h>

namespace apimon
{
    // InstallClipboardDataObjectVTableHooksIfNeeded 作用：
    // - 输入：dataObject 是 OleGetClipboard 成功返回的接口指针；
    // - 处理：读取其虚表指针，若从未见过，则把 GetData/QueryGetData/
    //   EnumFormatEtc 三个槽位替换成本模块的 Hooked* 函数，原指针存表备查；
    // - 返回：无返回值，dataObject 为空或虚表已打过都安全地什么都不做。
    void InstallClipboardDataObjectVTableHooksIfNeeded(IDataObject* dataObject);

    // UninstallAllClipboardDataObjectVTableHooks 作用：
    // - 处理：把所有已打补丁的虚表槽位复原成原始函数指针，清空记录表；
    // - 调用：由 HookTargets.cpp 的 UninstallConfiguredHooks() 在会话结束时调用，
    //   必须早于 DLL 可能被卸载的时间点，避免虚表里残留指向已释放模块的指针。
    void UninstallAllClipboardDataObjectVTableHooks();
}
