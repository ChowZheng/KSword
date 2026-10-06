// ============================================================
// MemoryEditJournal.cpp
// 作用：实现立即写入模式的撤销日志。规则（合并、容量、丢弃、换目标）全部写在
//       MemoryEditJournal.h 文件头，这里只放实现与实现层面的不变式：
//   * usage_ 恒等于各步 StepCost(长度) 之和，且恒不超过 maxBytes_；
//   * steps_.size() 恒不超过 maxSteps_；
//   * 游标 cursor_ 恒不超过 steps_.size()；Record 结束时 cursor_ 等于 steps_.size()；
//   * 每一步的 before 与 after 等长、非空，且 address + 长度不回绕。
// ============================================================

#include "MemoryEditJournal.h"

#include <algorithm>
#include <utility>

namespace ksword::memwb
{
    namespace
    {
        // uint64 地址空间的最大值，区间算术的回绕判据。
        constexpr std::uint64_t kAddressMax = 0xFFFFFFFFFFFFFFFFULL;
    }

    ReplayCheck CheckReplay(const JournalReplay& replay, const JournalBytes& current)
    {
        // 默认就是 MalformedReplay，所以每一条早退路径只要不改 verdict 就是"不许写"。
        ReplayCheck check;

        // 第一步：回放本身必须自洽。空回放没有任何内容可核对，不能让它空洞地通过。
        if (replay.expectedCurrent.empty() || replay.expectedCurrent.size() != replay.restore.size())
        {
            return check;
        }

        // 第二步：长度必须一致。回读没取全时绝不能拿前缀去比。
        if (current.size() != replay.expectedCurrent.size())
        {
            check.verdict = ReplayVerdict::LengthMismatch;
            return check;
        }

        // 第三步：逐字节核对，记下第一个不同的偏移。整步判定，不做部分匹配。
        for (std::size_t offset = 0; offset < current.size(); ++offset)
        {
            if (current[offset] != replay.expectedCurrent[offset])
            {
                check.verdict = ReplayVerdict::ContentMismatch;
                check.firstMismatch = offset;
                return check;
            }
        }
        check.verdict = ReplayVerdict::Match;
        return check;
    }

    MemoryEditJournal::MemoryEditJournal(const std::uint64_t maxBytes, const std::size_t maxSteps)
        : maxBytes_(maxBytes)
        , maxSteps_(maxSteps)
    {
    }

    std::uint64_t MemoryEditJournal::StepCost(const std::uint64_t length)
    {
        // 2 * length + 固定开销会回绕的长度，直接给最大值：它必然超过任何容量，
        // 调用方因此一定走"放不下"分支，而不是被回绕出的小数骗过。
        if (length > (kAddressMax - kEditJournalStepOverheadBytes) / 2)
        {
            return kAddressMax;
        }
        return 2 * length + kEditJournalStepOverheadBytes;
    }

    bool MemoryEditJournal::FitsAlone(const std::uint64_t length) const
    {
        return StepCost(length) <= maxBytes_;
    }

    bool MemoryEditJournal::BindTarget(const MemoryTargetSession& session)
    {
        // 同一目标：什么都不做。
        if (hasTarget_ && SameTarget(target_, session))
        {
            return false;
        }

        // 换了目标，或第一次绑定：绑定前的步骤来路不明，一律清掉。
        // 只有清掉的是非空历史才需要界面提示。
        const bool hadHistory = !steps_.empty();
        Clear();
        target_ = session;
        hasTarget_ = true;
        return hadHistory;
    }

    void MemoryEditJournal::Clear()
    {
        steps_.clear();
        cursor_ = 0;
        usage_ = 0;
        mergeBarrier_ = false;
    }

    std::size_t MemoryEditJournal::CutRedoBranch()
    {
        // 从尾部逐步弹出游标之后的步骤，并同步扣减记账开销。
        std::size_t discarded = 0;
        while (steps_.size() > cursor_)
        {
            usage_ -= StepCost(steps_.back().after.size());
            steps_.pop_back();
            ++discarded;
        }
        return discarded;
    }

    std::size_t MemoryEditJournal::EvictWhileOverBytes()
    {
        // 从最旧一步开始丢，直到总开销回到上限内。至少保留最后一步：
        // 调用方保证最后一步自己放得进上限，所以循环一定能收敛。
        std::size_t dropped = 0;
        while (usage_ > maxBytes_ && steps_.size() > 1)
        {
            usage_ -= StepCost(steps_.front().after.size());
            steps_.pop_front();
            if (cursor_ > 0)
            {
                --cursor_;
            }
            ++dropped;
        }
        return dropped;
    }

    bool MemoryEditJournal::TryMerge(
        const std::uint64_t address,
        const JournalBytes& before,
        const JournalBytes& after,
        const std::uint64_t tick,
        JournalRecordResult& result)
    {
        // 条件 a：有上一步，且上一次操作不是撤销/重做。
        if (mergeBarrier_ || steps_.empty())
        {
            return false;
        }
        Step& last = steps_.back();

        // 条件 c：时间间隔在 [0, 1500]。tick 倒退不合并；先判倒退，减法才不会回绕。
        if (tick < last.tick || tick - last.tick > kEditJournalMergeWindowTicks)
        {
            return false;
        }

        // 条件 b：范围相接或重叠。终点（不含）在 Record 里已校验可表示，加法不回绕。
        const std::uint64_t lastEnd = last.address + last.after.size();
        const std::uint64_t newEnd = address + after.size();
        if (address > lastEnd || last.address > newEnd)
        {
            return false;
        }

        // 条件 d：重叠部分的 before 必须等于上一步的 after，否则说明目标在两次写入
        // 之间被别处改过，把它们并成一步会让撤销越过别人的修改。
        const std::uint64_t overlapStart = std::max(address, last.address);
        const std::uint64_t overlapEnd = std::min(newEnd, lastEnd);
        for (std::uint64_t cursor = overlapStart; cursor < overlapEnd; ++cursor)
        {
            if (before[cursor - address] != last.after[cursor - last.address])
            {
                return false;
            }
        }

        // 并集范围必须自己放得进总容量，否则放弃合并，让新块另起一步（它单独肯定放得下）。
        const std::uint64_t mergedStart = std::min(address, last.address);
        const std::uint64_t mergedEnd = std::max(newEnd, lastEnd);
        const std::uint64_t mergedLength = mergedEnd - mergedStart;
        if (!FitsAlone(mergedLength))
        {
            return false;
        }

        // 合并：先铺上一步，再铺新块。重叠字节的 before 保留上一步的（最早一次），
        // after 取新块的（最晚一次）；只在新块里的字节，before/after 都取新块的。
        JournalBytes mergedBefore(static_cast<std::size_t>(mergedLength), 0);
        JournalBytes mergedAfter(static_cast<std::size_t>(mergedLength), 0);
        const std::size_t lastOffset = static_cast<std::size_t>(last.address - mergedStart);
        for (std::size_t index = 0; index < last.after.size(); ++index)
        {
            mergedBefore[lastOffset + index] = last.before[index];
            mergedAfter[lastOffset + index] = last.after[index];
        }
        const std::size_t newOffset = static_cast<std::size_t>(address - mergedStart);
        for (std::size_t index = 0; index < after.size(); ++index)
        {
            const std::uint64_t absolute = address + index;
            const bool coveredByLast = absolute >= last.address && absolute < lastEnd;
            if (!coveredByLast)
            {
                mergedBefore[newOffset + index] = before[index];
            }
            mergedAfter[newOffset + index] = after[index];
        }

        // 用并集替换上一步，并更新记账与时间戳（滑动窗口）。
        usage_ -= StepCost(last.after.size());
        last.address = mergedStart;
        last.before = std::move(mergedBefore);
        last.after = std::move(mergedAfter);
        last.tick = tick;
        usage_ += StepCost(last.after.size());

        // 净效果为零（键入又改回原值）：这一步撤销起来什么也不会发生，直接移除。
        if (last.before == last.after)
        {
            usage_ -= StepCost(last.after.size());
            steps_.pop_back();
            cursor_ = steps_.size();
            result.status = JournalRecordStatus::Cancelled;
            return true;
        }

        // 合并让这一步变大了，总开销可能越界：丢最旧的步骤腾地方。
        result.droppedOldest += EvictWhileOverBytes();
        result.status = JournalRecordStatus::Merged;
        return true;
    }

    JournalRecordResult MemoryEditJournal::Record(
        const std::uint64_t address,
        const JournalBytes& before,
        const JournalBytes& after,
        const std::uint64_t tick)
    {
        // 默认结果就是 InvalidInput + 全零，下面的早退路径只改需要改的字段。
        JournalRecordResult result;

        // 第一步：输入合法性。空块、不等长、终点回绕都是调用方 bug，日志状态不变。
        if (before.empty() || before.size() != after.size())
        {
            return result;
        }
        const std::uint64_t length = before.size();
        if (length > kAddressMax - address)
        {
            return result;
        }

        // 第二步：空操作不记录，也不切断重做分支。
        if (before == after)
        {
            result.status = JournalRecordStatus::Unchanged;
            return result;
        }

        // 第三步：单块自己就放不下（或日志被配置为不记录）：清空陈旧历史并报告。
        if (maxSteps_ == 0 || !FitsAlone(length))
        {
            result.status = JournalRecordStatus::TooLarge;
            result.historyCleared = !steps_.empty();
            Clear();
            return result;
        }

        // 第四步：新写入切断重做分支，随后先尝试并入上一步。
        result.redoDiscarded = CutRedoBranch();
        if (TryMerge(address, before, after, tick, result))
        {
            mergeBarrier_ = false;
            return result;
        }

        // 第五步：另起一步。先从最旧一步开始腾出步数与字节名额，再追加。
        // 比较写成 cost > maxBytes_ - usage_ 而不是 usage_ + cost，避免回绕。
        const std::uint64_t cost = StepCost(length);
        while (!steps_.empty() && (steps_.size() + 1 > maxSteps_ || cost > maxBytes_ - usage_))
        {
            usage_ -= StepCost(steps_.front().after.size());
            steps_.pop_front();
            ++result.droppedOldest;
        }

        // 第六步：追加新步骤，游标移到末尾，清除合并屏障。
        Step step;
        step.address = address;
        step.before = before;
        step.after = after;
        step.tick = tick;
        steps_.push_back(std::move(step));
        usage_ += cost;
        cursor_ = steps_.size();
        mergeBarrier_ = false;
        result.status = JournalRecordStatus::Recorded;
        return result;
    }

    std::optional<JournalReplay> MemoryEditJournal::PeekUndo() const
    {
        // 游标前一步就是下一次要撤销的步骤。
        if (cursor_ == 0)
        {
            return std::nullopt;
        }
        const Step& step = steps_[cursor_ - 1];
        JournalReplay replay;
        replay.address = step.address;
        replay.expectedCurrent = step.after;
        replay.restore = step.before;
        return replay;
    }

    std::optional<JournalReplay> MemoryEditJournal::PeekRedo() const
    {
        // 游标处的一步就是下一次要重做的步骤。
        if (cursor_ >= steps_.size())
        {
            return std::nullopt;
        }
        const Step& step = steps_[cursor_];
        JournalReplay replay;
        replay.address = step.address;
        replay.expectedCurrent = step.before;
        replay.restore = step.after;
        return replay;
    }

    bool MemoryEditJournal::MarkUndone()
    {
        if (cursor_ == 0)
        {
            return false;
        }
        --cursor_;
        mergeBarrier_ = true;
        return true;
    }

    bool MemoryEditJournal::MarkRedone()
    {
        if (cursor_ >= steps_.size())
        {
            return false;
        }
        ++cursor_;
        mergeBarrier_ = true;
        return true;
    }

    bool MemoryEditJournal::CanUndo() const
    {
        return cursor_ > 0;
    }

    bool MemoryEditJournal::CanRedo() const
    {
        return cursor_ < steps_.size();
    }

    std::size_t MemoryEditJournal::UndoCount() const
    {
        return cursor_;
    }

    std::size_t MemoryEditJournal::RedoCount() const
    {
        return steps_.size() - cursor_;
    }

    std::uint64_t MemoryEditJournal::MemoryUsage() const
    {
        return usage_;
    }

    std::uint64_t MemoryEditJournal::MaxBytes() const
    {
        return maxBytes_;
    }

    std::size_t MemoryEditJournal::MaxSteps() const
    {
        return maxSteps_;
    }
}
