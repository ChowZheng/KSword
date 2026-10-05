#pragma once

// ============================================================
// MemoryDock.WorkbenchServices.Internal.h
// 作用：
// - MemoryDock.WorkbenchServices.*.cpp 几个实现文件之间共享的内部函数声明。
//   对外接口见 MemoryDock.WorkbenchServices.h；本头文件**不对外**，接线步骤不应包含它。
// - 为什么需要它：生产 IWorkbenchServices、审计接收器、候选进程缓存分别放在不同的
//   .cpp 里（单文件不超过 800 行），ConfigureShared 所在文件需要拿到它们的工厂函数，
//   QueryAttachedProcessInfo 需要拿到候选进程缓存的查名函数。
// - 本头文件只用标准库、Core 值类型与 IWorkbenchServices / IAuditSink 两个抽象接口，
//   不包含 Framework.h，不暴露任何具体类。
// ============================================================

#include "../UI/MemoryWorkbench/WorkbenchServices.h"
#include "../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ks::ui::workbench_dock::detail
{
    // CreateProductionServices：创建一份生产 IWorkbenchServices。
    // 调用方法：WorkbenchShared::ServicesFactory 的实现体（每个 Dock 实例的
    //           WorkbenchTarget 构造时各调用一次）。
    // 传出：新对象，永不为空。对象不持有任何 QObject/Dock 裸指针，因此在工作线程上
    //       作为"最后一个持有者"被销毁是安全的。
    std::unique_ptr<IWorkbenchServices> CreateProductionServices();

    // CreateAuditLogSink：创建写项目日志的生产审计接收器。
    // 调用方法：WorkbenchShared::AuditSinkFactory 的实现体（Configure 时只调用一次）。
    // 传出：新对象，永不为空；线程安全。
    std::unique_ptr<ksword::memwb::IAuditSink> CreateAuditLogSink();

    // LookupProcessName：按 pid 取进程名（UTF-8）。
    // 规则：先用 ks::process::QueryProcessPathByPid 取映像路径并截出文件名（便宜且是
    //       当前事实）；取不到再查候选进程缓存；都取不到返回空串。**不会**触发完整的
    //       进程枚举（GetProcessNameByPID 的回退路径会做，UI 线程上不可接受）。
    // 线程：UI 线程调用。
    std::string LookupProcessName(std::uint32_t pid);
}
