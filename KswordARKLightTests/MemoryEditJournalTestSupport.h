#pragma once

// 编辑日志测试的共用支撑：构造小工具、确定性随机数、朴素参考实现与随机对拍驱动。
//
// 朴素参考实现（NaiveJournal）的写法刻意与被测实现完全不同：
//   * 被测实现：每步存连续的 before/after 向量，合并时按偏移拼接，开销用增量 usage_ 记账；
//   * 参考实现：每步存"地址 -> (前值, 后值)"的逐字节映射，合并就是映射求并集，
//     开销每次从所有步骤重新求和。
// 两边只共享规格（合并四条件、容量规则、丢弃顺序），不共享任何算法，所以随机对拍
// 才有证明力。参考实现本身也是"一次一步"地照规格抄写，没有任何优化。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryEditJournal.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace MemwbJournalTests {

using ksword::memwb::JournalBytes;
using ksword::memwb::JournalRecordResult;
using ksword::memwb::JournalRecordStatus;
using ksword::memwb::JournalReplay;
using ksword::memwb::MemoryEditJournal;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::ReplayVerdict;

// uint64 的最大值，测试里用来检验"没有被截断/回绕"。
inline constexpr std::uint64_t kMax64 = 0xFFFFFFFFFFFFFFFFULL;

// Rng：确定性伪随机数（splitmix64），跨编译器结果一致，失败可复现。
class Rng {
public:
    explicit Rng(const std::uint64_t seed) : state_(seed) {}

    // 返回 [0, bound) 内的数；bound 必须非零。
    std::uint64_t Below(const std::uint64_t bound) {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        value ^= value >> 31;
        return value % bound;
    }

private:
    std::uint64_t state_;
};

// NaiveStep：参考实现里的一步。逐字节映射，键是绝对地址。
struct NaiveByte {
    std::uint8_t before = 0;
    std::uint8_t after = 0;
};

struct NaiveStep {
    std::map<std::uint64_t, NaiveByte> bytes;
    std::uint64_t tick = 0;
};

// NaiveJournal：朴素参考实现，只对外提供与被测类同名的观察接口。
class NaiveJournal {
public:
    NaiveJournal(const std::uint64_t maxBytes, const std::size_t maxSteps)
        : maxBytes_(maxBytes), maxSteps_(maxSteps) {}

    // 总开销：每次从全部步骤重新求和，与被测类的增量记账互相印证。
    std::uint64_t Total() const {
        std::uint64_t total = 0;
        for (const NaiveStep& step : steps_) {
            total += 2 * step.bytes.size() + 64;
        }
        return total;
    }

    JournalRecordResult Record(
        const std::uint64_t address,
        const JournalBytes& before,
        const JournalBytes& after,
        const std::uint64_t tick) {
        JournalRecordResult result;
        if (before.empty() || before.size() != after.size()) {
            return result;
        }
        const std::uint64_t length = before.size();
        if (address > kMax64 - length) {
            return result;
        }
        if (before == after) {
            result.status = JournalRecordStatus::Unchanged;
            return result;
        }

        // 单块放不下：清空全部历史。
        const std::uint64_t cost = 2 * length + 64;
        if (maxSteps_ == 0 || cost > maxBytes_) {
            result.status = JournalRecordStatus::TooLarge;
            result.historyCleared = !steps_.empty();
            steps_.clear();
            cursor_ = 0;
            barrier_ = false;
            return result;
        }

        // 切断重做分支。
        result.redoDiscarded = steps_.size() - cursor_;
        steps_.resize(cursor_);

        // 尝试合并：时间、相接、重叠一致三个条件，然后并集不能超容量。
        if (!barrier_ && !steps_.empty() && TryMerge(address, before, after, tick, result)) {
            return result;
        }

        // 另起一步：从最旧的开始腾名额。
        while (!steps_.empty() && (steps_.size() + 1 > maxSteps_ || Total() + cost > maxBytes_)) {
            steps_.erase(steps_.begin());
            ++result.droppedOldest;
        }
        NaiveStep step;
        for (std::uint64_t index = 0; index < length; ++index) {
            step.bytes[address + index] = NaiveByte{ before[index], after[index] };
        }
        step.tick = tick;
        steps_.push_back(step);
        cursor_ = steps_.size();
        barrier_ = false;
        result.status = JournalRecordStatus::Recorded;
        return result;
    }

    std::optional<JournalReplay> PeekUndo() const {
        if (cursor_ == 0) {
            return std::nullopt;
        }
        return Flatten(steps_[cursor_ - 1], true);
    }

    std::optional<JournalReplay> PeekRedo() const {
        if (cursor_ >= steps_.size()) {
            return std::nullopt;
        }
        return Flatten(steps_[cursor_], false);
    }

    bool MarkUndone() {
        if (cursor_ == 0) {
            return false;
        }
        --cursor_;
        barrier_ = true;
        return true;
    }

    bool MarkRedone() {
        if (cursor_ >= steps_.size()) {
            return false;
        }
        ++cursor_;
        barrier_ = true;
        return true;
    }

    std::size_t UndoCount() const { return cursor_; }
    std::size_t RedoCount() const { return steps_.size() - cursor_; }

private:
    // Flatten：把一步展开成回放。撤销是"期望 after、写 before"，重做相反。
    static JournalReplay Flatten(const NaiveStep& step, const bool forUndo) {
        JournalReplay replay;
        replay.address = step.bytes.begin()->first;
        for (const auto& entry : step.bytes) {
            replay.expectedCurrent.push_back(forUndo ? entry.second.after : entry.second.before);
            replay.restore.push_back(forUndo ? entry.second.before : entry.second.after);
        }
        return replay;
    }

    bool TryMerge(
        const std::uint64_t address,
        const JournalBytes& before,
        const JournalBytes& after,
        const std::uint64_t tick,
        JournalRecordResult& result) {
        NaiveStep& last = steps_.back();
        const std::uint64_t length = before.size();
        const std::uint64_t lastStart = last.bytes.begin()->first;
        const std::uint64_t lastEnd = lastStart + last.bytes.size();

        // 条件 c：间隔 [0, 1500]，倒退不合并。
        if (tick < last.tick || tick - last.tick > 1500) {
            return false;
        }
        // 条件 b：范围相接或重叠。
        if (address > lastEnd || lastStart > address + length) {
            return false;
        }
        // 条件 d：重叠字节的新 before 必须等于上一步的 after。
        for (std::uint64_t index = 0; index < length; ++index) {
            const auto found = last.bytes.find(address + index);
            if (found != last.bytes.end() && found->second.after != before[index]) {
                return false;
            }
        }

        // 并集：已有字节只更新 after（before 保留最早的），新字节整个加入。
        NaiveStep merged = last;
        for (std::uint64_t index = 0; index < length; ++index) {
            const auto found = merged.bytes.find(address + index);
            if (found == merged.bytes.end()) {
                merged.bytes[address + index] = NaiveByte{ before[index], after[index] };
            } else {
                found->second.after = after[index];
            }
        }
        merged.tick = tick;
        if (2 * merged.bytes.size() + 64 > maxBytes_) {
            return false;
        }

        // 净效果为零：整步移除。
        bool allSame = true;
        for (const auto& entry : merged.bytes) {
            allSame = allSame && entry.second.before == entry.second.after;
        }
        if (allSame) {
            steps_.pop_back();
            cursor_ = steps_.size();
            barrier_ = false;
            result.status = JournalRecordStatus::Cancelled;
            return true;
        }

        // 替换上一步；总开销越界就从最旧的开始丢，至少留最后一步。
        last = merged;
        while (Total() > maxBytes_ && steps_.size() > 1) {
            steps_.erase(steps_.begin());
            ++result.droppedOldest;
        }
        cursor_ = steps_.size();
        barrier_ = false;
        result.status = JournalRecordStatus::Merged;
        return true;
    }

    std::uint64_t maxBytes_;
    std::size_t maxSteps_;
    std::vector<NaiveStep> steps_;
    std::size_t cursor_ = 0;
    bool barrier_ = false;
};

// SamePeek：两个回放是否逐字段相同（含"都没有"）。
inline bool SamePeek(const std::optional<JournalReplay>& left, const std::optional<JournalReplay>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left.has_value()) {
        return true;
    }
    return left->address == right->address && left->expectedCurrent == right->expectedCurrent
        && left->restore == right->restore;
}

// SameResult：两个 Record 结果是否逐字段相同。
inline bool SameResult(const JournalRecordResult& left, const JournalRecordResult& right) {
    return left.status == right.status && left.droppedOldest == right.droppedOldest
        && left.redoDiscarded == right.redoDiscarded && left.historyCleared == right.historyCleared;
}

// RandomConfig：随机对拍一轮的参数。
struct RandomConfig {
    // 日志容量。
    std::uint64_t maxBytes = ksword::memwb::kEditJournalMaxBytes;
    std::size_t maxSteps = ksword::memwb::kEditJournalMaxSteps;
    // 迭代次数与种子。
    int iterations = 3000;
    std::uint64_t seed = 1;
    // 是否混入"目标被外部改动"的操作。
    bool externalWrites = false;
    // 是否在结束后做整体展开校验（撤销到底应回到初始镜像，再重做到底回到末态）。
    // 只在不会因容量丢弃步骤、也没有外部改动时才成立。
    bool unwind = false;
};

// RandomStats：一轮对拍里各种事件真的发生过多少次，用来断言对拍没有空转。
struct RandomStats {
    int mismatches = 0;
    int unwindFailures = 0;
    int merged = 0;
    int cancelled = 0;
    int recorded = 0;
    int unchanged = 0;
    int tooLarge = 0;
    int dropped = 0;
    int redoCuts = 0;
    int undone = 0;
    int redone = 0;
    int replayRejected = 0;
};

// RunRandomJournal：模拟一块 64 字节的目标内存，随机交替"写入/撤销/重做/外部改动"，
// 每一步都把被测日志与朴素参考实现逐项对照（结果、计数、开销、两个 Peek）。
inline RandomStats RunRandomJournal(const RandomConfig& config) {
    constexpr std::uint64_t kBase = 0x1000;
    constexpr std::uint64_t kSize = 64;
    Rng rng(config.seed);
    MemoryEditJournal real(config.maxBytes, config.maxSteps);
    NaiveJournal naive(config.maxBytes, config.maxSteps);
    RandomStats stats;

    // 目标内存：小字母表的随机初值，便于出现"改成同值 / 改回原值 / 重叠一致"的情形。
    std::vector<std::uint8_t> memory(kSize);
    for (std::uint8_t& value : memory) {
        value = static_cast<std::uint8_t>(rng.Below(4));
    }
    const std::vector<std::uint8_t> initial = memory;
    std::uint64_t tick = 100000;
    bool firstMismatchReported = false;
    // 上一次写入的位置与写前字节，供定向写入使用。
    bool hasLast = false;
    std::uint64_t lastAddress = 0;
    JournalBytes lastBefore;

    // 记一次不一致，只把第一次的现场打印出来，方便复现。
    const auto noteMismatch = [&](const int iteration, const wchar_t* what) {
        ++stats.mismatches;
        if (!firstMismatchReported) {
            firstMismatchReported = true;
            std::wcerr << L"random journal mismatch: seed=" << config.seed << L" iteration=" << iteration
                       << L" " << what << L'\n';
        }
    };

    // 读取目标内存里 [address, address+length) 的当前字节。
    const auto readMemory = [&](const std::uint64_t address, const std::size_t length) {
        return std::vector<std::uint8_t>(
            memory.begin() + static_cast<std::ptrdiff_t>(address - kBase),
            memory.begin() + static_cast<std::ptrdiff_t>(address - kBase + length));
    };

    for (int iteration = 0; iteration < config.iterations; ++iteration) {
        const std::uint64_t choice = rng.Below(100);
        if (choice < 62) {
            // 写入：多数是随机位置/长度/内容；一部分是"针对上一次写入"的定向写入，
            // 专门制造合并、重叠、改回原值（取消）这些随机很难撞上的情形。
            // 模式 0 随机；1 改回上一次写入前的字节；2 紧接上一次写入之后；3 落在上一次写入范围内。
            std::uint64_t mode = hasLast ? rng.Below(100) : 100;
            std::size_t length = static_cast<std::size_t>(1 + rng.Below(rng.Below(10) == 0 ? 10 : 4));
            std::uint64_t address = kBase + rng.Below(kSize - length + 1);
            JournalBytes after(length);
            for (std::uint8_t& value : after) {
                value = static_cast<std::uint8_t>(rng.Below(4));
            }
            if (mode < 22) {
                // 改回上一次写入前的字节：与合并后恰好净效果为零。
                address = lastAddress;
                after = lastBefore;
                length = after.size();
            } else if (mode < 47 && lastAddress + lastBefore.size() + length <= kBase + kSize) {
                // 紧接上一次写入之后。
                address = lastAddress + lastBefore.size();
            } else if (mode < 60) {
                // 起点落在上一次写入范围内，长度保证不越过区域末尾。
                address = lastAddress + rng.Below(lastBefore.size());
                if (address + length > kBase + kSize) {
                    address = kBase + kSize - length;
                }
            } else {
                mode = 100;
            }
            const JournalBytes before = readMemory(address, length);
            for (std::size_t index = 0; index < length; ++index) {
                memory[static_cast<std::size_t>(address - kBase) + index] = after[index];
            }
            hasLast = true;
            lastAddress = address;
            lastBefore = before;

            // tick：定向写入多用小间隔；随机写入用全部边界值；偶尔倒退一点。
            static const std::uint64_t steps[] = { 0, 0, 1, 10, 1499, 1500, 1501, 4000 };
            if (rng.Below(20) == 0 && tick >= 7) {
                tick -= 7;
            } else {
                tick += steps[rng.Below(mode < 100 ? 6 : 8)];
            }
            const JournalRecordResult realResult = real.Record(address, before, after, tick);
            const JournalRecordResult naiveResult = naive.Record(address, before, after, tick);
            if (!SameResult(realResult, naiveResult)) {
                noteMismatch(iteration, L"record result");
            }
            stats.merged += realResult.status == JournalRecordStatus::Merged ? 1 : 0;
            stats.cancelled += realResult.status == JournalRecordStatus::Cancelled ? 1 : 0;
            stats.recorded += realResult.status == JournalRecordStatus::Recorded ? 1 : 0;
            stats.unchanged += realResult.status == JournalRecordStatus::Unchanged ? 1 : 0;
            stats.tooLarge += realResult.status == JournalRecordStatus::TooLarge ? 1 : 0;
            stats.dropped += realResult.droppedOldest > 0 ? 1 : 0;
            stats.redoCuts += realResult.redoDiscarded > 0 ? 1 : 0;
        } else if (choice < 94) {
            // 撤销（choice 62..81）或重做（choice 82..93）：先核对再回放再标记。
            const bool forUndo = choice < 82;
            const std::optional<JournalReplay> replay = forUndo ? real.PeekUndo() : real.PeekRedo();
            const std::optional<JournalReplay> reference = forUndo ? naive.PeekUndo() : naive.PeekRedo();
            if (!SamePeek(replay, reference)) {
                noteMismatch(iteration, L"peek before replay");
            }
            if (replay.has_value()) {
                const ksword::memwb::ReplayCheck check = ksword::memwb::CheckReplay(
                    *replay, readMemory(replay->address, replay->expectedCurrent.size()));
                if (check.verdict == ReplayVerdict::Match) {
                    for (std::size_t index = 0; index < replay->restore.size(); ++index) {
                        memory[static_cast<std::size_t>(replay->address - kBase) + index] = replay->restore[index];
                    }
                    const bool realMarked = forUndo ? real.MarkUndone() : real.MarkRedone();
                    const bool naiveMarked = forUndo ? naive.MarkUndone() : naive.MarkRedone();
                    if (!realMarked || !naiveMarked) {
                        noteMismatch(iteration, L"mark after replay");
                    }
                    stats.undone += forUndo ? 1 : 0;
                    stats.redone += forUndo ? 0 : 1;
                } else {
                    ++stats.replayRejected;
                    // 没有外部改动时核对不可能失败：失败就是日志记错了。
                    if (!config.externalWrites) {
                        noteMismatch(iteration, L"replay rejected without external writes");
                    }
                }
            }
        } else if (config.externalWrites) {
            // 外部改动：直接改目标内存，不通知日志。
            memory[static_cast<std::size_t>(rng.Below(kSize))] = static_cast<std::uint8_t>(rng.Below(4));
        }

        // 每一步之后：计数、开销、两个 Peek 都必须与参考实现一致，容量不变式必须成立。
        if (real.UndoCount() != naive.UndoCount() || real.RedoCount() != naive.RedoCount()) {
            noteMismatch(iteration, L"counts");
        }
        if (real.MemoryUsage() != naive.Total()) {
            noteMismatch(iteration, L"memory usage");
        }
        if (!SamePeek(real.PeekUndo(), naive.PeekUndo()) || !SamePeek(real.PeekRedo(), naive.PeekRedo())) {
            noteMismatch(iteration, L"peeks");
        }
        if (real.MemoryUsage() > config.maxBytes || real.UndoCount() + real.RedoCount() > config.maxSteps) {
            noteMismatch(iteration, L"capacity invariant");
        }
    }

    // 整体展开校验：先重做到顶端得到末态镜像；撤销到底必须回到初始镜像；
    // 再重做到顶端必须回到末态镜像。replayAll 逐步核对、回放、标记，核对失败返回 false。
    if (config.unwind) {
        const auto replayAll = [&](const bool forUndo) {
            for (;;) {
                const std::optional<JournalReplay> replay = forUndo ? real.PeekUndo() : real.PeekRedo();
                if (!replay.has_value()) {
                    return true;
                }
                const ksword::memwb::ReplayCheck check = ksword::memwb::CheckReplay(
                    *replay, readMemory(replay->address, replay->restore.size()));
                if (check.verdict != ReplayVerdict::Match) {
                    return false;
                }
                for (std::size_t index = 0; index < replay->restore.size(); ++index) {
                    memory[static_cast<std::size_t>(replay->address - kBase) + index] = replay->restore[index];
                }
                if (forUndo) {
                    real.MarkUndone();
                } else {
                    real.MarkRedone();
                }
            }
        };
        stats.unwindFailures += replayAll(false) ? 0 : 1;
        const std::vector<std::uint8_t> finalImage = memory;
        stats.unwindFailures += replayAll(true) ? 0 : 1;
        stats.unwindFailures += memory == initial ? 0 : 1;
        stats.unwindFailures += replayAll(false) ? 0 : 1;
        stats.unwindFailures += memory == finalImage ? 0 : 1;
    }
    return stats;
}

// AccumulateStats：把一轮对拍的统计累加进总计。
inline void AccumulateStats(RandomStats& total, const RandomStats& stats) {
    total.mismatches += stats.mismatches;
    total.unwindFailures += stats.unwindFailures;
    total.merged += stats.merged;
    total.cancelled += stats.cancelled;
    total.recorded += stats.recorded;
    total.unchanged += stats.unchanged;
    total.tooLarge += stats.tooLarge;
    total.dropped += stats.dropped;
    total.redoCuts += stats.redoCuts;
    total.undone += stats.undone;
    total.redone += stats.redone;
    total.replayRejected += stats.replayRejected;
}

// RunStandardRandomSuite：标准的一组对拍配置，返回累计统计。
//   * 默认容量、无外部改动：每轮结束做"撤销到底回到初始镜像"的模型无关校验；
//   * 默认容量、混入外部改动：核对失败会真的发生，日志必须仍与参考实现一致；
//   * 小容量（步数与字节两个维度）：丢弃、单块过大清空、合并后越界都被压到。
inline RandomStats RunStandardRandomSuite() {
    RandomStats total;
    for (std::uint64_t seed = 101; seed < 109; ++seed) {
        RandomConfig config;
        config.iterations = 200;
        config.seed = seed;
        config.unwind = true;
        AccumulateStats(total, RunRandomJournal(config));
    }
    for (std::uint64_t seed = 201; seed < 205; ++seed) {
        RandomConfig config;
        config.iterations = 3000;
        config.seed = seed;
        config.externalWrites = true;
        AccumulateStats(total, RunRandomJournal(config));
    }
    const std::pair<std::uint64_t, std::size_t> limits[] = {
        { 400, 5 }, { 1000, 256 }, { 80, 4 }, { 250, 3 }, { 100000, 2 }, { 140, 1 },
    };
    std::uint64_t seed = 301;
    for (const auto& limit : limits) {
        for (int round = 0; round < 2; ++round) {
            RandomConfig config;
            config.maxBytes = limit.first;
            config.maxSteps = limit.second;
            config.iterations = 4000;
            config.seed = seed++;
            config.externalWrites = round == 1;
            AccumulateStats(total, RunRandomJournal(config));
        }
    }
    return total;
}

// B：字节列表简写。
inline JournalBytes B(const std::initializer_list<std::uint8_t> values) {
    return JournalBytes(values);
}

// Filled：count 个同值字节。
inline JournalBytes Filled(const std::size_t count, const std::uint8_t value) {
    return JournalBytes(count, value);
}

// MakeSession：七个字段取互不相同的小数值，作为换目标测试的基准会话。
inline MemoryTargetSession MakeSession() {
    MemoryTargetSession session;
    session.scope = ksword::memwb::Scope::ProcessVirtual;
    session.pid = 1234;
    session.processCreateTime100ns = 5;
    session.attachGeneration = 7;
    session.channel = ksword::memwb::Channel::Hvm;
    session.ddmaGeneration = 9;
    session.addressBits = 64;
    return session;
}

// ExpectReplay：断言一个回放的地址、期望当前字节与待写字节三者同时符合手算值。
inline void ExpectReplay(
    KswordTests::Suite& suite,
    const std::optional<JournalReplay>& replay,
    const std::uint64_t address,
    const JournalBytes& expectedCurrent,
    const JournalBytes& restore,
    const wchar_t* label) {
    suite.expect(replay.has_value() && replay->address == address
        && replay->expectedCurrent == expectedCurrent && replay->restore == restore, label);
}

// ExpectResult：断言 Record 结果的状态与三个副作用字段。
inline void ExpectResult(
    KswordTests::Suite& suite,
    const JournalRecordResult& result,
    const JournalRecordStatus status,
    const std::size_t dropped,
    const std::size_t redoCut,
    const bool cleared,
    const wchar_t* label) {
    suite.expect(result.status == status && result.droppedOldest == dropped
        && result.redoDiscarded == redoCut && result.historyCleared == cleared, label);
}

} // namespace MemwbJournalTests
