// ============================================================
// wpK1_tests.ProbeTracker.cpp
// 作用：WorkbenchServicesProbe 里 HvmProbeTracker（HVM 通道可用性探测的异步缓存状态机）的
//       逐分支断言，用手造的时间轴驱动，不读真实时钟。
// 重点：
//   - 未出结论前报告 NotProbed（"未知"，EvaluateChannel 不会据此置灰）；
//   - 驱动未加载不发探测并复位，复位后到达的旧探测结果必须被丢弃；
//   - 已有结论后按间隔重新探测，探测期间继续报告旧结论（不闪回 NotProbed）；
//   - 并发：任一时刻最多一个探测在途。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesProbe.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <set>
#include <thread>
#include <vector>

namespace svc = ksword::memwb_services_detail;
using ksword::memwb::ProbeState;

namespace
{
    // kInterval：测试用的重新探测间隔（毫秒）。
    constexpr std::uint64_t kInterval = 1000U;

    // TestFirstProbeLifecycle：从未探测 -> 启动 -> 在途 -> 完成 -> 结论。
    void TestFirstProbeLifecycle()
    {
        svc::HvmProbeTracker tracker(kInterval);
        WPK1_CHECK(tracker.State() == ProbeState::NotProbed);
        WPK1_CHECK(!tracker.InFlight());

        // 驱动未加载：不发探测，状态 NotProbed。
        const svc::HvmProbeTracker::Observation unloaded = tracker.Observe(false, 0U);
        WPK1_CHECK(unloaded.action == svc::HvmProbeTracker::Action::None);
        WPK1_CHECK(unloaded.state == ProbeState::NotProbed);
        WPK1_CHECK(unloaded.ticket == 0U);
        WPK1_CHECK(!tracker.InFlight());

        // 驱动已加载：发起第一次探测，领到非零票据；此刻对外仍是 NotProbed（未知）。
        const svc::HvmProbeTracker::Observation started = tracker.Observe(true, 10U);
        WPK1_CHECK(started.action == svc::HvmProbeTracker::Action::StartProbe);
        WPK1_CHECK(started.ticket != 0U);
        WPK1_CHECK(started.state == ProbeState::NotProbed);
        WPK1_CHECK(tracker.InFlight());

        // 在途期间重复观察：不重复启动，状态仍是 NotProbed。
        for (std::uint64_t now = 11U; now < 50U; now += 7U)
        {
            const svc::HvmProbeTracker::Observation pending = tracker.Observe(true, now);
            WPK1_CHECK(pending.action == svc::HvmProbeTracker::Action::None);
            WPK1_CHECK(pending.state == ProbeState::NotProbed);
            WPK1_CHECK(pending.ticket == 0U);
        }

        // 用错误票据完成：被忽略，仍在途、仍未知。
        tracker.Complete(started.ticket + 1000U, true, 60U);
        WPK1_CHECK(tracker.InFlight());
        WPK1_CHECK(tracker.State() == ProbeState::NotProbed);

        // 用正确票据完成：得出 Usable，不再在途。
        tracker.Complete(started.ticket, true, 100U);
        WPK1_CHECK(!tracker.InFlight());
        WPK1_CHECK(tracker.State() == ProbeState::Usable);

        // 重复完成同一票据：无效果（状态不被后到的结果覆盖）。
        tracker.Complete(started.ticket, false, 110U);
        WPK1_CHECK(tracker.State() == ProbeState::Usable);

        // 结论之后、间隔之内：不探测，报告 Usable。
        const svc::HvmProbeTracker::Observation cached = tracker.Observe(true, 100U + kInterval - 1U);
        WPK1_CHECK(cached.action == svc::HvmProbeTracker::Action::None);
        WPK1_CHECK(cached.state == ProbeState::Usable);
    }

    // TestReprobe：结论之后的重新探测——间隔到期才发；探测期间继续报告旧结论。
    void TestReprobe()
    {
        svc::HvmProbeTracker tracker(kInterval);
        const svc::HvmProbeTracker::Observation first = tracker.Observe(true, 0U);
        tracker.Complete(first.ticket, true, 50U);
        WPK1_CHECK(tracker.State() == ProbeState::Usable);

        // 恰好到间隔：重新探测，且此刻仍报告旧结论 Usable（不回退成 NotProbed）。
        const svc::HvmProbeTracker::Observation again = tracker.Observe(true, 50U + kInterval);
        WPK1_CHECK(again.action == svc::HvmProbeTracker::Action::StartProbe);
        WPK1_CHECK(again.state == ProbeState::Usable);
        WPK1_CHECK(again.ticket != first.ticket);
        WPK1_CHECK(tracker.InFlight());

        // 重新探测期间再观察：不重复启动，仍报告旧结论。
        const svc::HvmProbeTracker::Observation during = tracker.Observe(true, 50U + kInterval + 5U);
        WPK1_CHECK(during.action == svc::HvmProbeTracker::Action::None);
        WPK1_CHECK(during.state == ProbeState::Usable);

        // 重新探测的结论覆盖旧结论：Usable -> Unusable。
        tracker.Complete(again.ticket, false, 2000U);
        WPK1_CHECK(tracker.State() == ProbeState::Unusable);
        const svc::HvmProbeTracker::Observation unusable = tracker.Observe(true, 2001U);
        WPK1_CHECK(unusable.action == svc::HvmProbeTracker::Action::None);
        WPK1_CHECK(unusable.state == ProbeState::Unusable);

        // Unusable 同样会按间隔重新探测（窗口标定状态可能变化）。
        const svc::HvmProbeTracker::Observation retry = tracker.Observe(true, 2000U + kInterval);
        WPK1_CHECK(retry.action == svc::HvmProbeTracker::Action::StartProbe);
        WPK1_CHECK(retry.state == ProbeState::Unusable);
        tracker.Complete(retry.ticket, true, 3100U);
        WPK1_CHECK(tracker.State() == ProbeState::Usable);
    }

    // TestDriverUnloadResets：驱动卸载复位，复位后到达的旧结果被丢弃。
    void TestDriverUnloadResets()
    {
        svc::HvmProbeTracker tracker(kInterval);

        // 在途探测期间驱动被卸载。
        const svc::HvmProbeTracker::Observation started = tracker.Observe(true, 0U);
        WPK1_CHECK(started.action == svc::HvmProbeTracker::Action::StartProbe);
        const svc::HvmProbeTracker::Observation unloaded = tracker.Observe(false, 5U);
        WPK1_CHECK(unloaded.action == svc::HvmProbeTracker::Action::None);
        WPK1_CHECK(unloaded.state == ProbeState::NotProbed);
        WPK1_CHECK(!tracker.InFlight());

        // 旧探测此时才完成：结果必须被丢弃，状态保持 NotProbed。
        tracker.Complete(started.ticket, true, 10U);
        WPK1_CHECK(tracker.State() == ProbeState::NotProbed);
        WPK1_CHECK(!tracker.InFlight());

        // 驱动重新加载：重新探测，票据与旧票据不同。
        const svc::HvmProbeTracker::Observation reloaded = tracker.Observe(true, 20U);
        WPK1_CHECK(reloaded.action == svc::HvmProbeTracker::Action::StartProbe);
        WPK1_CHECK(reloaded.ticket != started.ticket);
        // 旧票据再来一次（迟到的重复）仍然无效，新探测不受影响。
        tracker.Complete(started.ticket, false, 25U);
        WPK1_CHECK(tracker.InFlight());
        WPK1_CHECK(tracker.State() == ProbeState::NotProbed);
        tracker.Complete(reloaded.ticket, true, 30U);
        WPK1_CHECK(tracker.State() == ProbeState::Usable);

        // 已有结论后驱动卸载：结论作废，复位成 NotProbed（驱动之后再加载必须重新探测）。
        const svc::HvmProbeTracker::Observation gone = tracker.Observe(false, 40U);
        WPK1_CHECK(gone.state == ProbeState::NotProbed);
        WPK1_CHECK(tracker.State() == ProbeState::NotProbed);
        const svc::HvmProbeTracker::Observation back = tracker.Observe(true, 41U);
        WPK1_CHECK(back.action == svc::HvmProbeTracker::Action::StartProbe);
        WPK1_CHECK(back.state == ProbeState::NotProbed);

        // 驱动一直未加载时反复观察：不产生任何动作，也不无故递增票据。
        svc::HvmProbeTracker idle(kInterval);
        for (std::uint64_t now = 0U; now < 100U; now += 10U)
        {
            const svc::HvmProbeTracker::Observation none = idle.Observe(false, now);
            WPK1_CHECK(none.action == svc::HvmProbeTracker::Action::None);
        }
        const svc::HvmProbeTracker::Observation firstReal = idle.Observe(true, 200U);
        WPK1_CHECK(firstReal.action == svc::HvmProbeTracker::Action::StartProbe);
    }

    // TestClockBackwards：时钟倒退不能让探测永远被压住。
    void TestClockBackwards()
    {
        svc::HvmProbeTracker tracker(kInterval);
        const svc::HvmProbeTracker::Observation first = tracker.Observe(true, 5000U);
        tracker.Complete(first.ticket, true, 5100U);

        // 当前时刻早于上次完成时刻：按到期处理，重新探测。
        const svc::HvmProbeTracker::Observation backwards = tracker.Observe(true, 100U);
        WPK1_CHECK(backwards.action == svc::HvmProbeTracker::Action::StartProbe);
        WPK1_CHECK(backwards.state == ProbeState::Usable);
    }

    // TestTicketsStrictlyIncrease：票据严格递增、从不为 0。
    void TestTicketsStrictlyIncrease()
    {
        svc::HvmProbeTracker tracker(0U);
        std::uint64_t previous = 0U;
        for (int round = 0; round < 20; ++round)
        {
            const svc::HvmProbeTracker::Observation observation =
                tracker.Observe(true, static_cast<std::uint64_t>(round) * 10U);
            WPK1_CHECK(observation.action == svc::HvmProbeTracker::Action::StartProbe);
            WPK1_CHECK(observation.ticket > previous);
            previous = observation.ticket;
            tracker.Complete(observation.ticket, round % 2 == 0, static_cast<std::uint64_t>(round) * 10U + 1U);
        }
    }

    // TestConcurrentSingleFlight：多线程同时观察并完成，任一时刻最多一个探测在途，且没有数据竞争
    // 造成的状态错乱（最终状态必须是 Usable 或 Unusable 之一，票据不重复发放）。
    void TestConcurrentSingleFlight()
    {
        constexpr int kThreadCount = 4;
        constexpr int kIterations = 2000;

        svc::HvmProbeTracker tracker(0U);
        // outstanding / maxOutstanding：当前在途（已领票据尚未 Complete）的探测数与历史峰值。
        std::atomic<int> outstanding{0};
        std::atomic<int> maxOutstanding{0};
        // clock：全线程共享的单调递增"时钟"，保证每次调用的 nowMs 都不同。
        std::atomic<std::uint64_t> clock{0U};
        // ticketsPerThread：每个线程各自记录领到的票据，汇合后统一检查唯一性（避免在线程里加锁）。
        std::vector<std::vector<std::uint64_t>> ticketsPerThread(kThreadCount);

        std::vector<std::thread> threads;
        for (int threadIndex = 0; threadIndex < kThreadCount; ++threadIndex)
        {
            threads.emplace_back([&, threadIndex]() {
                for (int iteration = 0; iteration < kIterations; ++iteration)
                {
                    const std::uint64_t now = clock.fetch_add(1U);
                    const svc::HvmProbeTracker::Observation observation = tracker.Observe(true, now);
                    if (observation.action != svc::HvmProbeTracker::Action::StartProbe)
                    {
                        continue;
                    }
                    const int current = outstanding.fetch_add(1) + 1;
                    int observedMax = maxOutstanding.load();
                    while (current > observedMax && !maxOutstanding.compare_exchange_weak(observedMax, current))
                    {
                    }
                    ticketsPerThread[static_cast<std::size_t>(threadIndex)].push_back(observation.ticket);
                    // 先把在途计数减回去，再 Complete：Complete 之后别的线程才可能启动新探测，
                    // 所以计数的峰值只可能是 1。
                    outstanding.fetch_sub(1);
                    tracker.Complete(observation.ticket, iteration % 2 == 0, clock.fetch_add(1U));
                }
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        // 汇合后检查：票据全局唯一、峰值为 1、至少发起过探测。
        std::set<std::uint64_t> allTickets;
        std::size_t totalStarted = 0U;
        for (const std::vector<std::uint64_t>& tickets : ticketsPerThread)
        {
            totalStarted += tickets.size();
            allTickets.insert(tickets.begin(), tickets.end());
        }
        WPK1_CHECK(maxOutstanding.load() == 1);
        WPK1_CHECK(totalStarted > 0U);
        WPK1_CHECK(allTickets.size() == totalStarted);
        WPK1_CHECK(!tracker.InFlight());
        const ProbeState finalState = tracker.State();
        WPK1_CHECK(finalState == ProbeState::Usable || finalState == ProbeState::Unusable);
    }
}

namespace wpK1_test
{
    void RunProbeTrackerTests()
    {
        TestFirstProbeLifecycle();
        TestReprobe();
        TestDriverUnloadResets();
        TestClockBackwards();
        TestTicketsStrictlyIncrease();
        TestConcurrentSingleFlight();
    }
}
