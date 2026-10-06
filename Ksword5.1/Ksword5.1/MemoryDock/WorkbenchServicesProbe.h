#pragma once

// ============================================================
// WorkbenchServicesProbe.h
// 作用：
// - 内存工作台 Phase 3 WP-K1 里三个"带时间/带状态"的纯决策：
//   1) HvmProbeTracker：HVM 通道可用性探测的异步缓存状态机。探测本身是一次驱动
//      IOCTL，不能在 UI 线程里每次都发；这里只决定"此刻要不要启动一次后台探测"
//      与"当前对外报告哪个 ProbeState"，真正的后台探测由宿主侧
//      （MemoryDock.WorkbenchServices.Gate.cpp）执行，完成后回调 Complete。
//   2) DecideHandleCache / CreateTimesMatch：状态条"保护"段查询用的进程句柄小缓存
//      的复用/复核/重开判据（句柄按 pid + 进程创建时间缓存，进程重用 pid 时失效）。
//   3) DecideCandidateRefresh：候选进程列表进程级缓存的"同步填充/后台刷新/直接用缓存"
//      判据（宿主侧 MemoryDock.WorkbenchServices.Impl.cpp 使用）。
// - 本文件及其 .cpp 是纯逻辑：只用标准库与 Core 值类型，时间由调用方以毫秒数传入
//   （不自己读时钟），因此夹具可以用手造的时间轴逐步断言。
//
// ============================================================
// HvmProbeTracker 语义（对应 EvaluateChannel 对 HVM 的口径，MemoryChannelGate.h）
// ============================================================
// - 探测是异步的：未得出结论前对外报告 NotProbed（"可用性未知"，EvaluateChannel
//   会判成"可用但未知"，不得置灰）；得出结论后报告 Usable/Unusable。
// - 驱动未加载时不发探测，并把状态复位为 NotProbed（EvaluateChannel 对驱动类通道
//   先判 DriverNotLoaded，探测状态此刻没有意义）；驱动之后加载了要重新探测。
// - 已有结论后每隔 reprobeIntervalMs 重新探测一次；重新探测进行期间继续报告旧结论，
//   不回退成 NotProbed（窗口标定等状态可能在运行期变化，但不能因此闪烁）。
// - 每次启动探测都领一个递增的 ticket；Complete 只接受"当前在途探测"的 ticket，
//   驱动卸载/复位之后到达的旧结果一律丢弃，不会把过期结论写回状态。
// - 线程安全：Observe 在 UI 线程调用，Complete 在探测工作线程调用，内部用互斥锁。
// ============================================================

#include "../../../shared/evidence/memory_workbench/MemoryChannelGate.h"

#include <cstdint>
#include <mutex>

namespace ksword::memwb_services_detail
{
    // HvmProbeTracker：见文件头。
    class HvmProbeTracker
    {
    public:
        // Action：Observe 的结论——是否需要调用方启动一次后台探测。
        enum class Action
        {
            // None：不需要启动探测。
            None,
            // StartProbe：调用方必须启动一次后台探测，并在完成时用 ticket 调用 Complete。
            StartProbe
        };

        // Observation：一次 Observe 的完整结果。
        struct Observation
        {
            // action：是否启动探测。
            Action action = Action::None;
            // ticket：action 为 StartProbe 时有效的探测票据，否则为 0。
            std::uint64_t ticket = 0;
            // state：此刻应当对外报告的 ProbeState。
            ksword::memwb::ProbeState state = ksword::memwb::ProbeState::NotProbed;
        };

        // 构造：reprobeIntervalMs 已有结论后的重新探测间隔（毫秒）。
        explicit HvmProbeTracker(std::uint64_t reprobeIntervalMs) noexcept;

        // Observe：UI 线程每次取 GateInputs 时调用一次。
        // 传入：driverLoaded 驱动此刻是否已加载；nowMs 调用方的单调时钟毫秒读数。
        // 传出：见 Observation。返回 StartProbe 时内部已经把"在途"标记置位，调用方
        //       不得重复启动，必须保证最终调用 Complete（否则探测永远在途）。
        Observation Observe(bool driverLoaded, std::uint64_t nowMs);

        // Complete：探测工作线程在一次探测完成后调用。
        // 传入：ticket 启动时领到的票据；usable 探测结论；nowMs 完成时刻。
        // 行为：ticket 与当前在途探测不符（过期/复位后到达/重复完成）时什么都不做。
        void Complete(std::uint64_t ticket, bool usable, std::uint64_t nowMs);

        // State：当前对外报告的状态（不触发任何探测，供诊断与测试读取）。
        ksword::memwb::ProbeState State() const;

        // InFlight：此刻是否有探测在途（诊断与测试读取）。
        bool InFlight() const;

    private:
        // reprobeIntervalMs_：重新探测间隔（构造后不变）。
        const std::uint64_t reprobeIntervalMs_;
        // mutex_：保护下面全部可变状态。
        mutable std::mutex mutex_;
        // state_：当前对外报告的探测状态。
        ksword::memwb::ProbeState state_ = ksword::memwb::ProbeState::NotProbed;
        // inFlight_：是否有探测在途。
        bool inFlight_ = false;
        // generation_：票据发放计数；复位也会递增，使在途探测的结果失效。
        std::uint64_t generation_ = 0;
        // lastCompleteMs_：上一次完成探测的时刻，用于判断是否到了重新探测的时间。
        std::uint64_t lastCompleteMs_ = 0;
    };

    // HandleCacheDecision：句柄缓存对一次查询的处置。
    enum class HandleCacheDecision
    {
        // UseCached：直接复用缓存句柄。
        UseCached,
        // Revalidate：缓存句柄属于该 pid，但已有一段时间没复核身份；调用方应重新读
        // 当前 pid 的创建时间并用 CreateTimesMatch 与缓存值比对，不符则丢弃重开。
        Revalidate,
        // Reopen：没有可用缓存（无条目或条目属于别的 pid），必须重新打开。
        Reopen
    };

    // DecideHandleCache：句柄缓存复用判据。
    // 传入：hasEntry 缓存里有没有条目；entryPid 条目的 pid；wantedPid 本次要查的 pid；
    //       verifiedAtMs 条目上次核验身份的时刻；nowMs 当前时刻；
    //       revalidateIntervalMs 复核间隔（毫秒）。
    // 规则：无条目或 pid 不符 -> Reopen；距上次核验已达间隔、或时钟倒退 -> Revalidate；
    //       其余 -> UseCached。
    HandleCacheDecision DecideHandleCache(
        bool hasEntry,
        std::uint32_t entryPid,
        std::uint32_t wantedPid,
        std::uint64_t verifiedAtMs,
        std::uint64_t nowMs,
        std::uint64_t revalidateIntervalMs) noexcept;

    // CreateTimesMatch：缓存条目的创建时间与当前读到的创建时间是否表示同一个进程实例。
    // 规则：两者都必须非零（0 表示没读到，"不知道"不能当作"相等"），且数值相等。
    bool CreateTimesMatch(std::uint64_t cachedCreateTime, std::uint64_t freshCreateTime) noexcept;

    // CandidateRefreshDecision：候选进程缓存对一次 processCandidates() 查询的处置。
    // 背景：ks::process::EnumerateProcesses 每次都会做一遍系统句柄快照，耗时可达上百
    //       毫秒，而 processCandidates() 在 UI 线程被调用（目标进程输入框每次输入都可能
    //       触发补全），所以必须有进程级缓存：只有"从未取到过"才允许同步阻塞一次，其余时
    //       候要么直接用缓存，要么后台刷新。
    enum class CandidateRefreshDecision
    {
        // ReturnCache：直接返回缓存（可能是空列表：首次填充正在后台进行）。
        ReturnCache,
        // RefreshSync：从未取到过且没有后台刷新在途，同步枚举一次再返回。
        RefreshSync,
        // RefreshAsync：缓存已陈旧且没有刷新在途，先返回旧缓存并启动一次后台刷新。
        RefreshAsync
    };

    // DecideCandidateRefresh：候选进程缓存的刷新判据。
    // 传入：everFilled 缓存是否曾经成功填充过；refreshing 是否已有刷新在途；
    //       filledAtMs 上次填充完成的时刻；nowMs 当前时刻；staleMs 陈旧阈值。
    // 规则：从未填充过——没有刷新在途就 RefreshSync，有则 ReturnCache（空列表，稍后即有）；
    //       已填充——有刷新在途则 ReturnCache；距上次填充达到阈值、或时钟倒退（无法判断
    //       过了多久）则 RefreshAsync；其余 ReturnCache。
    CandidateRefreshDecision DecideCandidateRefresh(
        bool everFilled,
        bool refreshing,
        std::uint64_t filledAtMs,
        std::uint64_t nowMs,
        std::uint64_t staleMs) noexcept;
}
