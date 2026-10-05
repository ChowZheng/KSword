#pragma once

// ============================================================
// WorkbenchServices.h
// 作用：
// - 定义内存工作台"目标与导航"子系统对外部环境的唯一依赖面 IWorkbenchServices。
//   WorkbenchTarget（见 WorkbenchTarget.h）只通过这一个接口去枚举模块、读指针、
//   取候选进程列表、取 DDMA 代次，自己不直接碰 Win32 API、不直接碰 MemoryDock 的成员。
// - 真实实现（把这些调用接到 ks::process::Enumerate*、MemoryAccessBackend 等）是
//   MemoryDock/WorkbenchServices.cpp（后续工作包 K）的职责，本文件只定义契约。
// - 离屏夹具用一份完全可脚本化的假实现（FakeWorkbenchServices，定义在
//   tools/memwb_ui/wpI/ 下），不链接任何 Win32 调用，可以模拟延迟、失败与并发完成顺序。
//
// 线程规则（务必遵守，见 target.md 2.5）：
// - enumerateProcessModules / enumerateKernelModules 两个函数会被 WorkbenchTarget 从
//   QThreadPool 的工作线程调用，实现必须自身线程安全、绝不触碰任何 QWidget/QObject 的
//   UI 状态；可以在工作线程里做阻塞式枚举。
// - processCandidates / readPointer / ddmaGeneration 三个函数恒在 UI 线程被调用。
// - 本接口不拥有任何对象的生命周期：WorkbenchTarget 以 std::shared_ptr 持有实现，
//   确保纵使 WorkbenchTarget 本身被销毁，仍在途的工作线程调用也不会访问已释放的对象。
//
// 本文件只用标准库类型加 Core（ksword::memwb）的值类型，不包含 Windows.h、
// 不包含 Framework.h；需要时才在真实实现的 .cpp 里引入。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryModuleDirectory.h"
#include "../../../../shared/evidence/memory_workbench/MemoryProcessMatch.h"
#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ks::ui
{
    // ModuleEnumResult：一次模块枚举的结果。
    struct ModuleEnumResult
    {
        // ok：true 表示枚举本身成功（即便 records 为空——空进程也可能确实没有模块）。
        bool ok = false;
        // records：枚举到的模块记录；ok 为 false 时恒为空。
        std::vector<ksword::memwb::ModuleRecord> records;
        // failure：ok 为 false 时的失败说明（错误码/细节串，不含面向用户的句子）；
        // ok 为 true 时恒为空。
        std::string failure;
    };

    // PointerReadResult：一次指针读取的结果，供 SessionAddressResolver 的
    // IPointerReader 适配器转发（见 WorkbenchTarget.cpp 的 ServicesPointerReader）。
    struct PointerReadResult
    {
        // ok：true 表示读到了 value；false 时 value 恒为 0。
        bool ok = false;
        // value：零扩展后的指针值，仅 ok 为 true 时有意义。
        std::uint64_t value = 0;
        // failure：ok 为 false 时的失败说明，不含面向用户的句子；ok 为 true 时恒为空。
        std::string failure;
    };

    // IWorkbenchServices：WorkbenchTarget 依赖的外部环境的唯一接口。
    // 每个方法的调用线程见文件顶部的"线程规则"，实现者必须遵守。
    class IWorkbenchServices
    {
    public:
        // 虚析构：允许经基类指针销毁实现。
        // 析构发生的线程不是固定的（可疑点 #4 修复：旧注释"析构发生在 UI 线程"
        // 不准确）：WorkbenchTarget 本身确实只在 UI 线程被销毁，但
        // ModuleEnumTask（见 WorkbenchTarget.Modules.cpp）在工作线程上也持有一份
        // 独立的 shared_ptr<IWorkbenchServices>；如果 WorkbenchTarget 先在 UI 线程
        // 销毁、而某个仍在途的枚举任务恰好是最后一个持有者，真正触发析构（引用计数
        // 归零）的那一刻就落在工作线程上。真实实现如果内部持有 QObject/Dock 的裸
        // 指针，析构函数与 enumerateProcessModules/enumerateKernelModules 两个
        // 工作线程方法都必须自己对这种跨线程析构安全——不能假设析构一定发生在
        // UI 线程才去碰只应该在 UI 线程访问的对象。
        virtual ~IWorkbenchServices() = default;

        // enumerateProcessModules：枚举某个进程实例的模块列表。工作线程调用。
        // 传入：pid 目标进程号；expectCreateTime 期望的进程创建时间（100ns），用于实现
        //       自行校验"枚举到的是不是同一个进程实例"（例如先按身份核对再枚举）；
        //       0 表示调用方不知道/不要求校验。
        // 传出：ModuleEnumResult。实现不得抛异常（枚举失败请以 ok=false 返回）。
        virtual ModuleEnumResult enumerateProcessModules(
            std::uint32_t pid,
            std::uint64_t expectCreateTime) = 0;

        // enumerateKernelModules：枚举内核模块列表。工作线程调用，无参数（内核只有一份）。
        virtual ModuleEnumResult enumerateKernelModules() = 0;

        // processCandidates：取当前可见的候选进程列表，供"目标进程"输入框的文本匹配
        // （ksword::memwb::MatchProcessText）与自动补全使用。UI 线程调用。
        // 传出：候选进程（可能含重复或 pid=0 的项，匹配函数自己会过滤，这里不必预先去重）。
        virtual std::vector<ksword::memwb::ProcessCandidate> processCandidates() = 0;

        // readPointer：按会话描述的目标，在给定地址读取一个指针宽度的值。UI 线程调用，
        // 由 SessionAddressResolver 的解引用门控保证调用前已经排除了物理范围与 DDMA 通道，
        // 实现不需要重复这些检查，但也不应该自行"回退到别的通道"——读不到就如实报失败。
        // 传入：session 当前会话；address 读取地址；width 宽度（字节，4 或 8）。
        virtual PointerReadResult readPointer(
            const ksword::memwb::MemoryTargetSession& session,
            std::uint64_t address,
            std::uint32_t width) = 0;

        // ddmaGeneration：取 DDMA 暂存扇区当前代次（包装既有的 ddmaSessionGeneration()）。
        // UI 线程调用；WorkbenchTarget 每次取会话（session()/capture()）且当前通道为
        // Ddma 时都会调用一次，用来喂 MemoryTargetTracker::ObserveDdma。
        virtual std::uint64_t ddmaGeneration() = 0;
    };
}
