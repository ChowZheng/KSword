#pragma once

// ============================================================
// MemoryDock.WorkbenchServices.h
// 作用：
// - 内存工作台（UI/MemoryWorkbench/ 的 MemoryWorkbenchView 等装配层）在 MemoryDock 侧的
//   "生产实现"对外接口。装配层本身不依赖任何驱动/Win32/日志头文件，一切重依赖都由
//   MemoryDock 通过依赖注入交给它；本头文件只暴露接线步骤需要的七个自由函数，具体
//   实现分布在同名的 MemoryDock.WorkbenchServices.*.cpp 里。
// - 本头文件的形状（命名空间、函数名、签名）是后续接线步骤依赖的冻结接口，不要改签名。
//
// ============================================================
// 接线步骤速查（MemoryDock 构造 MemoryWorkbenchView 时）
// ============================================================
//   1) 创建第一个 MemoryWorkbenchView 之前调用一次 ConfigureShared()（幂等，UI 线程）；
//   2) view->setDisasmBackends(MakeDecodeBackend(), MakeAssembleBackend());
//   3) view->setGlobalSkipDangerousConfirmProvider(GlobalSkipDangerousConfirm);
//   4) view->setAttachedProcessInfoProvider(QueryAttachedProcessInfo);
//   5) view->setProtectionProvider：视图的回调只带地址，pid 由接线步骤绑定——
//        [view](std::uint64_t address) {
//            return workbench_dock::QueryProtection(view->target().session().pid, address);
//        }
//      （内核/物理范围下 session().pid 恒为 0，QueryProtection 对 pid 为 0 返回空，
//      状态条"保护"段随之不显示）。
//   6) view->setGateInputsProvider：**必须由接线步骤补上 hasProcessTarget**——
//        [view]() {
//            ksword::memwb::GateInputs inputs = workbench_dock::QueryGateInputs();
//            inputs.hasProcessTarget = view->target().session().pid != 0;
//            return inputs;
//        }
//      原因：QueryGateInputs 没有会话参数，不知道当前目标，hasProcessTarget 恒为 false；
//      而当前 MemoryWorkbenchView / WorkbenchPageProvider 都把回调结果原样交给
//      EvaluateChannel，不会自己填这个字段，漏填的后果是进程范围下**所有通道都被判
//      NeedsPid 不可用**（页读取全部被取消）。
//
// ============================================================
// 线程规则
// ============================================================
// - 七个函数全部只允许在 UI 线程调用（QueryGateInputs 内部的 HVM 探测自己会切到工作线程，
//   但对调用方不可见）。
// - 生产审计接收器、生产 IWorkbenchServices、int3 字节存储工厂等对象通过
//   ConfigureShared() 注入 WorkbenchShared，本头文件不暴露它们的类型。
// ============================================================

#include "../UI/MemoryWorkbench/MemoryWorkbenchView.h"
#include "../UI/MemoryWorkbench/WorkbenchDisasmView.h"
#include "../../../shared/evidence/memory_workbench/MemoryChannelGate.h"

#include <cstdint>
#include <optional>

namespace ks::ui::workbench_dock
{
    // ConfigureShared：构造 WorkbenchShared::WorkbenchBackends 并调用
    // WorkbenchShared::Instance().Configure(...) 一次。
    // 调用方法：MemoryDock 在创建第一个 MemoryWorkbenchView 之前调用；多次调用安全，
    //           只有第一次真正配置（后续调用立即返回，不重复建对象、不重复写日志）。
    // 注入内容：地址簿文件路径（用户配置目录，不存在则创建）、int3 字节存储工厂、
    //           生产 IWorkbenchServices 工厂、真实读写端口工厂、内核分步事务端口工厂、
    //           写项目日志的审计接收器。
    // 失败可见性：Configure 被拒绝（例如装配顺序错误）时写一条 err 日志并带上拒绝原因，
    //             不抛异常、不阻止后续流程。
    void ConfigureShared();

    // QueryGateInputs：通道可用性判定（EvaluateChannel）所需的运行期输入。
    // 填充：driverLoaded（静默打开驱动设备，结果缓存 1 秒）、hvmProbe（后台异步探测
    //       私有页表窗口，探测未出结论时报告 NotProbed 即"未知"，不会置灰）、
    //       ddmaSessionReady（既有 isDdmaUsable 判据，纯内存读取）。
    // 不填：hasProcessTarget 恒为 false，由接线步骤按视图当前会话补上（见文件头第 6 条）。
    // 耗时：缓存命中时只读几个内存变量；缓存过期的那一次多一次 CreateFile，约数十微秒。
    ksword::memwb::GateInputs QueryGateInputs();

    // QueryProtection：状态条"保护"段数据——某进程某地址所在页的保护属性与区域类型。
    // 传入：pid 目标进程号（0 表示没有进程目标）；address 要查询的虚拟地址。
    // 传出：徽章文字与配色（RWX 红、X 橙、RW 绿，详见 FormatProtectionBadge）；
    //       pid 为 0、地址在内核半区、打不开进程、VirtualQueryEx 失败时返回 nullopt。
    // 句柄：按 pid + 进程创建时间做小缓存，进程重用 pid 时失效。
    std::optional<ks::ui::WorkbenchProtectionInfo> QueryProtection(
        std::uint32_t pid,
        std::uint64_t address);

    // QueryAttachedProcessInfo：目标 chip 的展示字段（进程名 + 是否可读写）。
    // 传入：pid 目标进程号。
    // 传出：processName 取候选进程缓存或 GetProcessNameByPID；canReadWrite 与旧 Dock
    //       附加时同口径（能以 VM_READ|VM_WRITE|VM_OPERATION|QUERY_INFORMATION 打开）。
    //       pid 为 0，或进程已不存在且取不到名字时返回 nullopt。
    std::optional<ks::ui::AttachedProcessDisplayInfo> QueryAttachedProcessInfo(std::uint32_t pid);

    // GlobalSkipDangerousConfirm：全局"跳过危险操作重复确认"开关当前值。
    // 复用既有设置项（ks::settings::dangerousActionConfirmationsSuppressed，设置页的
    // suppress_dangerous_action_confirmations 键），不新增第二处键名；每次调用都
    // 读取最新持久值，不做缓存（用户刚关掉开关后下一次写入必须立刻重新确认）。
    bool GlobalSkipDangerousConfirm();

    // MakeDecodeBackend：反汇编单条解码后端，包装既有 InstructionDecoder::decode。
    // 规则：只接受 Zydis 解出的、长度有界（1..15 且不超过可用字节）的指令；解不出就
    //       返回 nullopt，由反汇编视图把这一个字节标成 db 并从下一字节重新同步。
    ks::ui::DecodeOneFn MakeDecodeBackend();

    // MakeAssembleBackend：汇编后端，包装既有 InstructionAssembler::assemble。
    // 规则（汇编核心的边界规则原样保留，本包装只做类型转换，不绕过、不放宽）：
    //   - 源码、地址与架构原样交给既有汇编核心；
    //   - 失败（含"报告成功却没有机器码""机器码超过 64 KiB"）一律不返回任何机器码；
    //   - 失败原因与出错行号（errorLine）原样透传；
    //   - "新机器码必须覆盖完整旧指令、不得超出原指令长度"由反汇编视图在拿到结果后判定
    //     （超长拒绝、过短用 NOP 补齐），不在这里重复也不在这里放宽。
    ks::ui::AssembleOneFn MakeAssembleBackend();
}
