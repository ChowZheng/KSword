#include "WorkbenchServicesProbe.h"

// ============================================================
// WorkbenchServicesProbe.cpp
// 作用：实现 WorkbenchServicesProbe.h 的 HVM 探测状态机与句柄缓存判据。
// ============================================================

namespace ksword::memwb_services_detail
{
    HvmProbeTracker::HvmProbeTracker(const std::uint64_t reprobeIntervalMs) noexcept
        : reprobeIntervalMs_(reprobeIntervalMs)
    {
    }

    HvmProbeTracker::Observation HvmProbeTracker::Observe(
        const bool driverLoaded,
        const std::uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        Observation observation;

        // 驱动未加载：不发探测。曾经有结论或有在途探测就整体复位（驱动之后再加载时必须
        // 重新探测，旧结论不可信）。在途探测的迟到结果由 inFlight_ 清零直接挡住
        // （Complete 要求 inFlight_ 为真）；这里再递增代次是第二道保险，保证复位之后
        // 新启动的探测领到的票据与任何旧票据都不同。
        if (!driverLoaded)
        {
            if (state_ != ksword::memwb::ProbeState::NotProbed || inFlight_)
            {
                ++generation_;
                state_ = ksword::memwb::ProbeState::NotProbed;
                inFlight_ = false;
            }
            observation.state = ksword::memwb::ProbeState::NotProbed;
            return observation;
        }

        observation.state = state_;

        // 已经有探测在途：继续报告旧状态，不重复启动。
        if (inFlight_)
        {
            return observation;
        }

        // 是否到了该探测的时间：从未得出结论（NotProbed）必须探测；已有结论则要等够
        // 间隔。时钟倒退（nowMs 小于上次完成时刻）按"到期"处理，避免被一个倒退的
        // 时钟永远压住不再探测。
        bool due = false;
        if (state_ == ksword::memwb::ProbeState::NotProbed)
        {
            due = true;
        }
        else if (nowMs < lastCompleteMs_ || (nowMs - lastCompleteMs_) >= reprobeIntervalMs_)
        {
            due = true;
        }

        if (due)
        {
            inFlight_ = true;
            ++generation_;
            observation.action = Action::StartProbe;
            observation.ticket = generation_;
        }
        return observation;
    }

    void HvmProbeTracker::Complete(
        const std::uint64_t ticket,
        const bool usable,
        const std::uint64_t nowMs)
    {
        std::lock_guard<std::mutex> guard(mutex_);

        // 只接受"当前在途探测"的结果：没有在途、或票据已被复位/新探测取代，一律丢弃。
        if (!inFlight_ || ticket != generation_)
        {
            return;
        }
        state_ = usable ? ksword::memwb::ProbeState::Usable : ksword::memwb::ProbeState::Unusable;
        inFlight_ = false;
        lastCompleteMs_ = nowMs;
    }

    ksword::memwb::ProbeState HvmProbeTracker::State() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return state_;
    }

    bool HvmProbeTracker::InFlight() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        return inFlight_;
    }

    HandleCacheDecision DecideHandleCache(
        const bool hasEntry,
        const std::uint32_t entryPid,
        const std::uint32_t wantedPid,
        const std::uint64_t verifiedAtMs,
        const std::uint64_t nowMs,
        const std::uint64_t revalidateIntervalMs) noexcept
    {
        // 没有条目，或条目属于别的进程：必须重新打开。
        if (!hasEntry || entryPid != wantedPid)
        {
            return HandleCacheDecision::Reopen;
        }
        // 时钟倒退：无法判断距上次核验过了多久，保守地要求复核。
        if (nowMs < verifiedAtMs)
        {
            return HandleCacheDecision::Revalidate;
        }
        if ((nowMs - verifiedAtMs) >= revalidateIntervalMs)
        {
            return HandleCacheDecision::Revalidate;
        }
        return HandleCacheDecision::UseCached;
    }

    bool CreateTimesMatch(
        const std::uint64_t cachedCreateTime,
        const std::uint64_t freshCreateTime) noexcept
    {
        // 0 表示"没读到"：不知道不能当作相等，否则读取失败会被误判成"身份未变"。
        if (cachedCreateTime == 0U || freshCreateTime == 0U)
        {
            return false;
        }
        return cachedCreateTime == freshCreateTime;
    }

    CandidateRefreshDecision DecideCandidateRefresh(
        const bool everFilled,
        const bool refreshing,
        const std::uint64_t filledAtMs,
        const std::uint64_t nowMs,
        const std::uint64_t staleMs) noexcept
    {
        // 从未填充过：没有在途刷新才允许同步阻塞一次；已有在途刷新就返回空缓存，
        // 不重复启动（后台刷新完成后下一次查询自然拿到数据）。
        if (!everFilled)
        {
            return refreshing ? CandidateRefreshDecision::ReturnCache
                              : CandidateRefreshDecision::RefreshSync;
        }

        // 已有缓存：刷新在途就不重复启动。
        if (refreshing)
        {
            return CandidateRefreshDecision::ReturnCache;
        }

        // 时钟倒退无法判断缓存年龄，保守地按陈旧处理；否则达到阈值才刷新。
        if (nowMs < filledAtMs || (nowMs - filledAtMs) >= staleMs)
        {
            return CandidateRefreshDecision::RefreshAsync;
        }
        return CandidateRefreshDecision::ReturnCache;
    }
}
