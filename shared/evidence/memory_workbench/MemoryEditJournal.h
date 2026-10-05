#pragma once

// ============================================================
// MemoryEditJournal.h
// 作用：
// - "立即写入"模式下的撤销日志。立即写入时修改一经提交就已经落在目标内存里，暂存缓存
//   里什么都不剩，所以撤销不能再靠暂存缓存：日志记下每次 Committed 块的 before/after，
//   Ctrl+Z 就是"向目标补写旧值"，Ctrl+Y 是"补写新值"，明确是一次新的写入
//   （用户决策，设计文档第 6 节第 3 条）。
// - 为什么不复用旧的 MemoryEditHistory：它按"等长整快照"记账，窗口一移动（基线窗口
//   随视口跟随，长度会变）就判 SizeMismatch；本日志只存差异块，与窗口无关。
// - 纯 C++20 标准库实现，不依赖 Qt、不依赖 Win32、不读时钟（时间戳 tick 由调用方
//   注入）、不做任何 I/O。本类自己永远不写目标内存，只给出"该写什么、写之前
//   目标上应该是什么"，写与回读由调用方走同一条写事务/确认/审计链路。
//
// ============================================================
// 冻结接口摘要（后续 WP-J 的 WriteController 依赖，改动须同步通知）
// ============================================================
//   kEditJournalMergeWindowTicks  合并窗口 1500 个 tick（调用方用毫秒，即 1.5 s）
//   kEditJournalMaxBytes          容量上限 32 MiB
//   kEditJournalMaxSteps          步数上限 256
//   kEditJournalStepOverheadBytes 每步固定记账开销 64 字节
//   JournalRecordStatus           Recorded / Merged / Cancelled / Unchanged /
//                                 InvalidInput / TooLarge
//   JournalRecordResult           status + droppedOldest + redoDiscarded + historyCleared
//   JournalReplay{address, expectedCurrent, restore}
//                                 一次回放：写之前目标上应是 expectedCurrent，
//                                 然后把 restore 写到 address
//   ReplayVerdict / ReplayCheck   Match / LengthMismatch / ContentMismatch /
//                                 MalformedReplay；firstMismatch
//   CheckReplay(replay, current)  -> ReplayCheck   回放前核对当前字节（整步，不部分）
//   MemoryEditJournal(maxBytes = 32 MiB, maxSteps = 256)
//     BindTarget(session)         -> bool  换目标则清空；返回 true 表示清掉了非空历史
//     Record(address, before, after, tick) -> JournalRecordResult  记一个 Committed 块
//     PeekUndo() / PeekRedo()     -> optional<JournalReplay>  只读取，不移动游标
//     MarkUndone() / MarkRedone() -> bool  回放已成功写入目标后移动游标
//     Clear()                     清空全部步骤（不改已绑定目标）
//     CanUndo/CanRedo/UndoCount/RedoCount/MemoryUsage/MaxBytes/MaxSteps  查询
//     StepCost(length)            -> uint64  一步的记账开销，测试与界面估算用
//
// ============================================================
// 调用协议（Undo / Redo 一轮）
// ============================================================
//   1. replay = journal.PeekUndo()；没有值就是没东西可撤销。
//   2. 调用方读取目标 [replay.address, +长度) 的当前字节，CheckReplay 核对；
//      不是 Match 就**整步拒绝**（提示"目标已被其他程序改动，无法自动撤销"），
//      不写任何字节、不调用 MarkUndone。日志不做部分回放。
//   3. 核对通过后经写事务把 replay.restore 写到 replay.address。
//   4. 写成功才调用 MarkUndone()。写失败则不调用，步骤原样留在原位，可以重试。
//   5. 这次补写**不要**再 Record：它是撤销本身，记进去会切断重做分支并多出一步。
//   重做对称：PeekRedo / CheckReplay / 写 / MarkRedone。
//
// ============================================================
// 合并规则（Record 内部）
// ============================================================
// "同一插入点连续键入"在日志层的判据（四条同时满足才合并，否则另起一步）：
//   a. 上一步存在，且上一次操作不是撤销/重做（撤销/重做后第一次记录一律另起一步，
//      免得新写入悄悄并进用户刚刚回放过的步骤）；
//   b. 新块与上一步的地址范围相接（首尾相邻）或重叠，前向键入、回退键入、原地改写
//      同一字节都算；
//   c. 时间间隔 tick - 上一步 tick 在 [0, 1500]。间隔是相邻两次记录之间的滑动间隔，
//      合并后上一步的 tick 更新为新块的 tick，所以持续键入可以连成一步；
//      tick 倒退（新块比上一步还早）一律不合并；
//   d. 重叠部分一致：新块的 before 必须等于上一步的 after（说明中间没人动过），
//      不一致说明目标在两次写入之间被别处改过，不合并。
// 合并结果是一个连续块：before 取每个字节"最早一次"的原值，after 取"最晚一次"的新值。
// 合并后整块 before 与 after 完全相同（键入又改回原值）时，这一步已无净效果，
// 直接移除并返回 Cancelled。
//
// ============================================================
// 容量与丢弃
// ============================================================
// - 一步的记账开销 = 2 * 块长度 + 64 字节固定开销（只存差异块，不存整快照）。
//   总开销不超过 32 MiB、步数不超过 256；新记录放不下时从最旧一步开始丢弃，
//   丢弃数经 JournalRecordResult::droppedOldest 报告，界面据此提示一次"更早的
//   撤销历史已被丢弃"。
// - 单个块自己就放不下（开销超过总容量）：无法记录，也不能假装之前的历史还连着，
//   清空全部历史并返回 TooLarge（与旧历史"不可记录的编辑清空陈旧历史"同语义）。
// - 新记录会切断重做分支（被切的步数在 redoDiscarded 里）。
// - before 与 after 完全相同（空操作）不记录，返回 Unchanged，且不切断重做分支。
// - 输入不合法（长度为 0、before/after 不等长、address + 长度回绕）返回
//   InvalidInput，日志状态完全不变。终点（不含）必须能用 uint64 表示，
//   与叠加层的统一溢出规则一致。
//
// ============================================================
// 换目标
// ============================================================
// BindTarget(session)：与已绑定目标 SameTarget 则什么都不做；否则清空历史并绑定新目标。
// 清掉的是非空历史时返回 true，界面据此提示一次"换了目标，撤销历史已清空"。
// 尚未绑定过目标时的第一次绑定也按"换目标"处理：绑定前记下的步骤来路不明，同样清掉。
// SameTarget 比较七个会话字段（含 attachGeneration、ddmaGeneration）。
//
// 线程模型：非线程安全，调用方在 UI 线程使用。
// ============================================================

#include "MemoryTargetSession.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace ksword::memwb
{
    // 字节序列的别名，before/after/回放内容都用它。
    using JournalBytes = std::vector<std::uint8_t>;

    // 合并窗口：1500 个 tick。tick 的单位由调用方决定，约定为毫秒，即 1.5 秒。
    inline constexpr std::uint64_t kEditJournalMergeWindowTicks = 1500;

    // 容量上限：32 MiB，与旧编辑历史一致。
    inline constexpr std::uint64_t kEditJournalMaxBytes = 32ULL * 1024ULL * 1024ULL;

    // 步数上限：256，与旧编辑历史一致。
    inline constexpr std::size_t kEditJournalMaxSteps = 256;

    // 每步固定记账开销（字节）：步骤对象、地址、时间戳与两个向量头的粗略估计。
    inline constexpr std::uint64_t kEditJournalStepOverheadBytes = 64;

    // JournalRecordStatus：一次 Record 的结果。
    enum class JournalRecordStatus
    {
        // 记成了新的一步。
        Recorded,
        // 并入了上一步。
        Merged,
        // 并入上一步后净效果为零（改了又改回原值），该步已被移除。
        Cancelled,
        // before 与 after 完全相同，没有记录，日志状态不变。
        Unchanged,
        // 输入不合法，日志状态不变。这是调用方 bug，不是用户可见的失败。
        InvalidInput,
        // 单块自己就超过总容量，无法记录；全部历史已清空。
        TooLarge
    };

    // JournalRecordResult：Record 的结果与需要界面提示的副作用。
    // 默认值是 InvalidInput + 全零，失败路径上不会残留上一次的数字。
    struct JournalRecordResult
    {
        // 结果种类。
        JournalRecordStatus status = JournalRecordStatus::InvalidInput;
        // 因容量（步数或字节）而被丢弃的最旧步骤数；非零时界面应提示一次。
        std::size_t droppedOldest = 0;
        // 因新记录而被切断的重做步数。
        std::size_t redoDiscarded = 0;
        // 全部历史是否被清空（仅 TooLarge 且清空前非空时为 true）。
        bool historyCleared = false;
    };

    // JournalReplay：一次回放的内容，撤销与重做共用。
    struct JournalReplay
    {
        // 回放写入的起始绝对地址。
        std::uint64_t address = 0;
        // 写之前目标上应有的字节（撤销时是 after，重做时是 before），
        // 调用方用 CheckReplay 核对，不符则整步拒绝。
        JournalBytes expectedCurrent;
        // 要写入的字节（撤销时是 before，重做时是 after），与 expectedCurrent 等长。
        JournalBytes restore;
    };

    // ReplayVerdict：CheckReplay 的结论。只有 Match 才允许写。
    enum class ReplayVerdict
    {
        // 当前字节与 expectedCurrent 逐字节一致，可以回放。
        Match,
        // 当前字节长度与 expectedCurrent 不同（例如回读没取全），整步拒绝。
        LengthMismatch,
        // 长度相同但内容不同：目标已被别处改过，整步拒绝。
        ContentMismatch,
        // 回放本身不自洽（expectedCurrent 为空，或与 restore 不等长）。
        MalformedReplay
    };

    // ReplayCheck：CheckReplay 的结果。默认值是 MalformedReplay，即"不许写"。
    struct ReplayCheck
    {
        // 结论。
        ReplayVerdict verdict = ReplayVerdict::MalformedReplay;
        // 仅 ContentMismatch 时有意义：第一个不同的字节在回放范围内的偏移。
        std::size_t firstMismatch = 0;
    };

    // CheckReplay：回放前核对当前字节。
    // 调用方法：先读目标 [replay.address, replay.address + replay.expectedCurrent.size())，
    //           把读到的字节作为 current 传入；只有返回 Match 才可以写 replay.restore。
    // 传入：replay 来自 PeekUndo/PeekRedo；current 目标上读到的当前字节。
    // 传出：ReplayCheck；整步判定，没有"部分匹配可部分回放"这种结果。
    ReplayCheck CheckReplay(const JournalReplay& replay, const JournalBytes& current);

    // MemoryEditJournal：立即写入模式的撤销日志，规则见文件头。
    class MemoryEditJournal
    {
    public:
        // 构造一个空日志。maxBytes / maxSteps 默认是 32 MiB / 256 步的常量；
        // 参数只为让测试能用小容量验证边界。maxSteps 为 0 表示不记录任何步骤。
        explicit MemoryEditJournal(
            std::uint64_t maxBytes = kEditJournalMaxBytes,
            std::size_t maxSteps = kEditJournalMaxSteps);

        // 绑定当前目标。
        // 传入：session 当前目标会话。
        // 传出：true 表示因目标变化清空了非空历史（界面提示一次）；其余情况 false。
        bool BindTarget(const MemoryTargetSession& session);

        // 记录一次已成功写入并回读确认的块。
        // 传入：address 块起始绝对地址；before 写入前目标上的字节；after 写入后的字节
        //       （与 before 等长）；tick 调用方注入的单调时间戳。
        // 传出：JournalRecordResult，含状态与需要提示的副作用。
        JournalRecordResult Record(
            std::uint64_t address,
            const JournalBytes& before,
            const JournalBytes& after,
            std::uint64_t tick);

        // 取下一次撤销的回放内容（游标前一步），不移动游标。没有可撤销步骤返回 nullopt。
        // expectedCurrent = 该步的 after，restore = 该步的 before。
        std::optional<JournalReplay> PeekUndo() const;

        // 取下一次重做的回放内容（游标处的一步），不移动游标。没有可重做步骤返回 nullopt。
        // expectedCurrent = 该步的 before，restore = 该步的 after。
        std::optional<JournalReplay> PeekRedo() const;

        // 撤销已成功写入目标后调用：游标后退一步，并禁止下一次 Record 并入任何步骤。
        // 传出：false 表示没有可撤销步骤（状态不变）。
        bool MarkUndone();

        // 重做已成功写入目标后调用：游标前进一步，并禁止下一次 Record 并入任何步骤。
        // 传出：false 表示没有可重做步骤（状态不变）。
        bool MarkRedone();

        // 清空全部步骤。不改已绑定的目标。
        void Clear();

        // 是否有可撤销步骤。
        bool CanUndo() const;

        // 是否有可重做步骤。
        bool CanRedo() const;

        // 可撤销的步数。
        std::size_t UndoCount() const;

        // 可重做的步数。
        std::size_t RedoCount() const;

        // 当前全部步骤的记账开销合计（字节），恒不超过 MaxBytes。
        std::uint64_t MemoryUsage() const;

        // 本实例使用的容量上限（字节）。
        std::uint64_t MaxBytes() const;

        // 本实例使用的步数上限。
        std::size_t MaxSteps() const;

        // 长度为 length 的块的记账开销：2 * length + 固定开销。length 过大导致回绕时
        // 返回 uint64 最大值（必然超过任何容量），不会回绕成小数。
        static std::uint64_t StepCost(std::uint64_t length);

    private:
        // Step：一步。记录一个连续块的前后字节与最近一次并入的时间戳。
        struct Step
        {
            // 块起始绝对地址。
            std::uint64_t address = 0;
            // 每个字节最早一次写入前的原值。
            JournalBytes before;
            // 每个字节最晚一次写入后的值，与 before 等长。
            JournalBytes after;
            // 最近一次记录或并入的 tick，供合并窗口判断。
            std::uint64_t tick = 0;
        };

        // 长度为 length 的单个块自己是否放得进总容量。
        bool FitsAlone(std::uint64_t length) const;

        // 切断重做分支，返回被切掉的步数。
        std::size_t CutRedoBranch();

        // 尝试把新块并入上一步。成功时改写 result 并返回 true。
        bool TryMerge(
            std::uint64_t address,
            const JournalBytes& before,
            const JournalBytes& after,
            std::uint64_t tick,
            JournalRecordResult& result);

        // 从最旧一步开始丢弃，直到总开销不超过上限（至少保留最后一步），返回丢弃数。
        std::size_t EvictWhileOverBytes();

        // 容量上限（字节）。
        std::uint64_t maxBytes_;
        // 步数上限。
        std::size_t maxSteps_;
        // 全部步骤，最旧在前。
        std::deque<Step> steps_;
        // 游标：steps_[0, cursor_) 可撤销，steps_[cursor_, size) 可重做。
        std::size_t cursor_ = 0;
        // 全部步骤的记账开销合计。
        std::uint64_t usage_ = 0;
        // 合并屏障：撤销/重做之后置位，下一次 Record 清除；置位时禁止并入。
        bool mergeBarrier_ = false;
        // 是否已绑定过目标。
        bool hasTarget_ = false;
        // 已绑定的目标。
        MemoryTargetSession target_;
    };
}
