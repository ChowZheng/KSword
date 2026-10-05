// 编辑日志（shared/evidence/memory_workbench/MemoryEditJournal.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：撤销日志属于**记错了不会报错**的那一类。
// 记错之后界面照样显示"可撤销"，Ctrl+Z 照样往目标里补写字节，只是补写的是错的内容：
//   * 合并规则放得太松 -> 一次 Ctrl+Z 把用户几十秒前、毫不相干的修改一并撤掉；
//   * 合并规则放得太紧 -> 连续键入四个字节要按四次 Ctrl+Z，等于没做合并；
//   * 合并时 before 取了"最晚一次"而不是"最早一次" -> 撤销后目标里留下中间态的脏数据；
//   * 回放前不核对 -> 目标已被别的程序改过，撤销把别人的修改悄悄覆盖掉；
//   * 容量记账不准 / 丢弃顺序不对 -> 要么吞掉最新的步骤，要么撑爆内存。
// 这些路径都不会抛异常，只有按调用顺序逐条断言才抓得住。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值全部手算写死（地址、字节、开销 2*长度+64 都是字面量），绝不从被测函数反算；
//   * 边界两侧都测（合并间隔 1500/1501 tick、步数 256/257、字节容量恰好/多一字节、
//     地址相接/差一字节、地址终点 2^64 的两侧）；
//   * 拒绝路径必须显式测，且被拒绝后日志状态原样不变；
//   * 最后用逐字节映射写成的朴素参考实现做随机对拍，并做"撤销到底回到初始镜像"的
//     模型无关校验。

#include "MemoryEditJournalTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace {

using namespace MemwbJournalTests;

using ksword::memwb::Channel;
using ksword::memwb::CheckReplay;
using ksword::memwb::kEditJournalMaxBytes;
using ksword::memwb::kEditJournalMaxSteps;
using ksword::memwb::kEditJournalMergeWindowTicks;
using ksword::memwb::kEditJournalStepOverheadBytes;
using ksword::memwb::ReplayCheck;
using ksword::memwb::Scope;

// ------------------------------------------------------------
// 一、常量、默认值、空日志上的行为。
// ------------------------------------------------------------
void TestConstantsAndDefaults(KswordTests::Suite& suite) {
    suite.expect(kEditJournalMergeWindowTicks == 1500ULL, L"journal: the merge window is 1500 ticks");
    suite.expect(kEditJournalMaxBytes == 33554432ULL, L"journal: the default byte capacity is 32 MiB");
    suite.expect(kEditJournalMaxSteps == 256U, L"journal: the default step capacity is 256");
    suite.expect(kEditJournalStepOverheadBytes == 64ULL, L"journal: the fixed per-step overhead is 64 bytes");

    // 默认值：失败路径上忘记赋值的调用方拿到的是"拒绝"。
    const JournalRecordResult blank;
    ExpectResult(suite, blank, JournalRecordStatus::InvalidInput, 0, 0, false,
        L"journal: a default record result is InvalidInput with all side effects zero");
    const ReplayCheck blankCheck;
    suite.expect(blankCheck.verdict == ReplayVerdict::MalformedReplay && blankCheck.firstMismatch == 0,
        L"journal: a default replay check refuses to write");

    // 单步开销 = 2 * 长度 + 64；回绕的长度返回最大值而不是回绕成小数。
    suite.expect(MemoryEditJournal::StepCost(0) == 64ULL && MemoryEditJournal::StepCost(1) == 66ULL
        && MemoryEditJournal::StepCost(100) == 264ULL, L"journal: StepCost is 2 * length + 64");
    suite.expect(MemoryEditJournal::StepCost(0x7FFFFFFFFFFFFFDFULL) == 0xFFFFFFFFFFFFFFFEULL,
        L"journal: the largest length whose cost still fits is costed exactly");
    suite.expect(MemoryEditJournal::StepCost(0x7FFFFFFFFFFFFFE0ULL) == kMax64
        && MemoryEditJournal::StepCost(kMax64) == kMax64,
        L"journal: a length whose cost would wrap is costed as the maximum");

    // 空日志。
    MemoryEditJournal journal;
    suite.expect(!journal.CanUndo() && !journal.CanRedo() && journal.UndoCount() == 0 && journal.RedoCount() == 0,
        L"journal: a new journal has nothing to undo or redo");
    suite.expect(journal.MemoryUsage() == 0 && journal.MaxBytes() == kEditJournalMaxBytes
        && journal.MaxSteps() == kEditJournalMaxSteps, L"journal: a new journal is empty with default limits");
    suite.expect(!journal.PeekUndo().has_value() && !journal.PeekRedo().has_value(),
        L"journal: an empty journal has no replay to offer");
    suite.expect(!journal.MarkUndone() && !journal.MarkRedone(),
        L"journal: marking undone or redone on an empty journal is refused");
    journal.Clear();
    suite.expect(journal.UndoCount() == 0 && journal.MemoryUsage() == 0, L"journal: Clear on an empty journal is harmless");
}

// ------------------------------------------------------------
// 二、记录与撤销/重做游标：LIFO 顺序、期望与待写字节方向。
// ------------------------------------------------------------
void TestRecordAndCursor(KswordTests::Suite& suite) {
    MemoryEditJournal journal;
    ExpectResult(suite, journal.Record(0x1000, B({ 0x00 }), B({ 0x41 }), 100), JournalRecordStatus::Recorded,
        0, 0, false, L"cursor: the first block is recorded");
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 0 && journal.MemoryUsage() == 66ULL,
        L"cursor: one 1-byte step costs 66 bytes");
    ExpectReplay(suite, journal.PeekUndo(), 0x1000, B({ 0x41 }), B({ 0x00 }),
        L"cursor: undo expects the new byte 41 and restores the old byte 00");
    suite.expect(!journal.PeekRedo().has_value(), L"cursor: nothing to redo yet");
    suite.expect(journal.UndoCount() == 1, L"cursor: peeking does not move the cursor");

    // 第二步在远处，不会合并（tick 也隔得很远）。
    ExpectResult(suite, journal.Record(0x2000, B({ 0xAA, 0xBB }), B({ 0xCC, 0xDD }), 100000),
        JournalRecordStatus::Recorded, 0, 0, false, L"cursor: a far block becomes its own step");
    suite.expect(journal.UndoCount() == 2 && journal.MemoryUsage() == 134ULL,
        L"cursor: costs add up: 66 + 68 = 134");
    ExpectReplay(suite, journal.PeekUndo(), 0x2000, B({ 0xCC, 0xDD }), B({ 0xAA, 0xBB }),
        L"cursor: undo takes the newest step first");

    // 撤销最新一步：游标后退，重做能看到它，方向相反。
    suite.expect(journal.MarkUndone(), L"cursor: the newest step is marked undone");
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 1 && journal.CanRedo(),
        L"cursor: one step moved to the redo side");
    ExpectReplay(suite, journal.PeekUndo(), 0x1000, B({ 0x41 }), B({ 0x00 }),
        L"cursor: undo now offers the older step");
    ExpectReplay(suite, journal.PeekRedo(), 0x2000, B({ 0xAA, 0xBB }), B({ 0xCC, 0xDD }),
        L"cursor: redo expects the old bytes and writes the new bytes");

    // 撤销到底再多撤一次被拒绝，状态不变。
    suite.expect(journal.MarkUndone() && !journal.CanUndo() && journal.RedoCount() == 2,
        L"cursor: the oldest step can be undone too");
    suite.expect(!journal.MarkUndone() && journal.RedoCount() == 2 && !journal.PeekUndo().has_value(),
        L"cursor: undoing past the bottom is refused and changes nothing");

    // 重做到顶，再多重做一次被拒绝；撤销/重做不改记账开销。
    suite.expect(journal.MarkRedone() && journal.MarkRedone() && journal.UndoCount() == 2 && journal.RedoCount() == 0,
        L"cursor: both steps can be redone");
    suite.expect(!journal.MarkRedone() && journal.UndoCount() == 2, L"cursor: redoing past the top is refused");
    suite.expect(journal.MemoryUsage() == 134ULL, L"cursor: undo and redo never change the accounted usage");
}

// ------------------------------------------------------------
// 三、不合法输入与空操作。
// ------------------------------------------------------------
void TestInvalidAndUnchanged(KswordTests::Suite& suite) {
    MemoryEditJournal journal;
    journal.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    journal.Record(0x2000, B({ 0 }), B({ 1 }), 100000);
    journal.MarkUndone();
    // 状态基线：1 个可撤销、1 个可重做、开销 132。

    // 不合法输入：状态原样不变，包括不切断重做分支。
    ExpectResult(suite, journal.Record(0x3000, JournalBytes{}, JournalBytes{}, 200000),
        JournalRecordStatus::InvalidInput, 0, 0, false, L"input: an empty block is invalid");
    ExpectResult(suite, journal.Record(0x3000, B({ 1, 2 }), B({ 3 }), 200000),
        JournalRecordStatus::InvalidInput, 0, 0, false, L"input: before and after of different length are invalid");
    ExpectResult(suite, journal.Record(0xFFFFFFFFFFFFFFFFULL, B({ 0 }), B({ 1 }), 200000),
        JournalRecordStatus::InvalidInput, 0, 0, false, L"input: a block whose end would be 2^64 is invalid (1 byte at the maximum)");
    ExpectResult(suite, journal.Record(0xFFFFFFFFFFFFFFFEULL, B({ 0, 0 }), B({ 1, 1 }), 200000),
        JournalRecordStatus::InvalidInput, 0, 0, false, L"input: a 2-byte block ending exactly at 2^64 is invalid");
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 1 && journal.MemoryUsage() == 132ULL,
        L"input: rejected blocks leave counts, redo branch and usage untouched");

    // 空操作：不记录，也不切断重做分支。
    ExpectResult(suite, journal.Record(0x1000, B({ 5, 6 }), B({ 5, 6 }), 200000),
        JournalRecordStatus::Unchanged, 0, 0, false, L"input: a block with identical before and after is Unchanged");
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 1 && journal.MemoryUsage() == 132ULL,
        L"input: an Unchanged block keeps the redo branch");

    // 终点恰为 uint64 最大值（不含）是合法边界：最末字节本身不可写，但前一个可以。
    MemoryEditJournal edge;
    ExpectResult(suite, edge.Record(0xFFFFFFFFFFFFFFFEULL, B({ 0 }), B({ 1 }), 0), JournalRecordStatus::Recorded,
        0, 0, false, L"input: a 1-byte block ending exactly at the maximum is valid");
    ExpectReplay(suite, edge.PeekUndo(), 0xFFFFFFFFFFFFFFFEULL, B({ 1 }), B({ 0 }),
        L"input: the edge block replays correctly");

    // 部分字节相同的块按原样记录整块（范围就是实际写过的范围）。
    MemoryEditJournal partial;
    partial.Record(0x1000, B({ 1, 2, 3 }), B({ 1, 9, 3 }), 0);
    ExpectReplay(suite, partial.PeekUndo(), 0x1000, B({ 1, 9, 3 }), B({ 1, 2, 3 }),
        L"input: a partly unchanged block is recorded whole");
    suite.expect(partial.MemoryUsage() == 70ULL, L"input: a 3-byte step costs 70 bytes");
}

// ------------------------------------------------------------
// 四、合并的时间条件：1500 / 1501 两侧、滑动窗口、tick 倒退、tick 近最大值。
// ------------------------------------------------------------
void TestMergeTimeBoundary(KswordTests::Suite& suite) {
    // 间隔恰好 1500：合并。
    MemoryEditJournal at1500;
    at1500.Record(0x1000, B({ 0 }), B({ 1 }), 10000);
    ExpectResult(suite, at1500.Record(0x1001, B({ 0 }), B({ 2 }), 11500), JournalRecordStatus::Merged, 0, 0, false,
        L"time: a gap of exactly 1500 ticks merges");
    suite.expect(at1500.UndoCount() == 1 && at1500.MemoryUsage() == 68ULL, L"time: the merged step costs 2*2+64 = 68");
    ExpectReplay(suite, at1500.PeekUndo(), 0x1000, B({ 1, 2 }), B({ 0, 0 }),
        L"time: the merged step undoes both bytes at once");

    // 间隔 1501：另起一步。
    MemoryEditJournal at1501;
    at1501.Record(0x1000, B({ 0 }), B({ 1 }), 10000);
    ExpectResult(suite, at1501.Record(0x1001, B({ 0 }), B({ 2 }), 11501), JournalRecordStatus::Recorded, 0, 0, false,
        L"time: a gap of 1501 ticks does not merge");
    suite.expect(at1501.UndoCount() == 2 && at1501.MemoryUsage() == 132ULL, L"time: two separate steps cost 132");
    ExpectReplay(suite, at1501.PeekUndo(), 0x1001, B({ 2 }), B({ 0 }), L"time: the newest byte is its own step");

    // 间隔 0、1、1499：都合并。
    for (const std::uint64_t gap : { 0ULL, 1ULL, 1499ULL }) {
        MemoryEditJournal small;
        small.Record(0x1000, B({ 0 }), B({ 1 }), 777);
        suite.expect(small.Record(0x1001, B({ 0 }), B({ 2 }), 777 + gap).status == JournalRecordStatus::Merged,
            L"time: gaps of 0, 1 and 1499 ticks all merge");
    }

    // 滑动窗口：间隔按相邻两次记录算，合并后上一步的 tick 更新，持续键入可连成一步。
    MemoryEditJournal sliding;
    const std::uint64_t ticks[] = { 0, 1500, 3000, 4500 };
    JournalRecordStatus statuses[4] = {};
    for (std::size_t index = 0; index < 4; ++index) {
        statuses[index] = sliding.Record(0x1000 + index, B({ 0 }), B({ static_cast<std::uint8_t>(index + 1) }),
            ticks[index]).status;
    }
    suite.expect(statuses[0] == JournalRecordStatus::Recorded && statuses[1] == JournalRecordStatus::Merged
        && statuses[2] == JournalRecordStatus::Merged && statuses[3] == JournalRecordStatus::Merged,
        L"time: four bytes typed 1500 ticks apart chain into one step");
    ExpectReplay(suite, sliding.PeekUndo(), 0x1000, B({ 1, 2, 3, 4 }), B({ 0, 0, 0, 0 }),
        L"time: the chained step covers all four bytes");
    suite.expect(sliding.MemoryUsage() == 72ULL, L"time: the chained 4-byte step costs 72");
    // 与"上一次并入"相隔 1501 才断开（距第一次早已超过 1500）。
    ExpectResult(suite, sliding.Record(0x1004, B({ 0 }), B({ 5 }), 6001), JournalRecordStatus::Recorded, 0, 0, false,
        L"time: 1501 ticks after the last merge starts a new step");

    // tick 倒退：不合并；与上一步 tick 相同才算 0 间隔。
    MemoryEditJournal backward;
    backward.Record(0x1000, B({ 0 }), B({ 1 }), 5000);
    ExpectResult(suite, backward.Record(0x1001, B({ 0 }), B({ 2 }), 4999), JournalRecordStatus::Recorded, 0, 0, false,
        L"time: a tick that went backwards never merges");
    ExpectResult(suite, backward.Record(0x1002, B({ 0 }), B({ 3 }), 4999), JournalRecordStatus::Merged, 0, 0, false,
        L"time: an equal tick merges (gap 0) into the step whose tick went backwards");

    // tick 接近 uint64 最大值：差值不能回绕。
    MemoryEditJournal high1500;
    high1500.Record(0x1000, B({ 0 }), B({ 1 }), kMax64 - 1500);
    suite.expect(high1500.Record(0x1001, B({ 0 }), B({ 2 }), kMax64).status == JournalRecordStatus::Merged,
        L"time: a gap of 1500 ending at the maximum tick still merges");
    MemoryEditJournal high1501;
    high1501.Record(0x1000, B({ 0 }), B({ 1 }), kMax64 - 1501);
    suite.expect(high1501.Record(0x1001, B({ 0 }), B({ 2 }), kMax64).status == JournalRecordStatus::Recorded,
        L"time: a gap of 1501 ending at the maximum tick does not merge");
}

// ------------------------------------------------------------
// 五、合并的地址条件与重叠一致性。
// ------------------------------------------------------------
void TestMergeAddressRules(KswordTests::Suite& suite) {
    // 前向相接合并，差一个字节的空洞不合并。
    MemoryEditJournal hole;
    hole.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    ExpectResult(suite, hole.Record(0x1002, B({ 0 }), B({ 2 }), 10), JournalRecordStatus::Recorded, 0, 0, false,
        L"address: a one-byte hole after the step does not merge");

    // 回退键入：新块紧挨在上一步之前。
    MemoryEditJournal backward;
    backward.Record(0x1004, B({ 5 }), B({ 6 }), 0);
    ExpectResult(suite, backward.Record(0x1003, B({ 4 }), B({ 7 }), 10), JournalRecordStatus::Merged, 0, 0, false,
        L"address: a block right before the step merges (backspace-style typing)");
    ExpectReplay(suite, backward.PeekUndo(), 0x1003, B({ 7, 6 }), B({ 4, 5 }),
        L"address: the backward merge starts at the lower address");
    MemoryEditJournal backwardHole;
    backwardHole.Record(0x1004, B({ 5 }), B({ 6 }), 0);
    ExpectResult(suite, backwardHole.Record(0x1002, B({ 4 }), B({ 7 }), 10), JournalRecordStatus::Recorded, 0, 0, false,
        L"address: a one-byte hole before the step does not merge");

    // 部分重叠：重叠字节新块的 before 与上一步的 after 一致才合并；before 取最早、after 取最晚。
    MemoryEditJournal overlap;
    overlap.Record(0x1000, B({ 0, 0, 0 }), B({ 1, 2, 3 }), 0);
    ExpectResult(suite, overlap.Record(0x1001, B({ 2, 3 }), B({ 9, 8 }), 10), JournalRecordStatus::Merged, 0, 0, false,
        L"address: an overlapping block with matching bytes merges");
    ExpectReplay(suite, overlap.PeekUndo(), 0x1000, B({ 1, 9, 8 }), B({ 0, 0, 0 }),
        L"address: merged before keeps the earliest bytes, merged after the latest");
    suite.expect(overlap.MemoryUsage() == 70ULL, L"address: an overlap merge does not double count the shared bytes");

    // 重叠并向后延伸。
    MemoryEditJournal extend;
    extend.Record(0x1000, B({ 0, 0 }), B({ 1, 2 }), 0);
    extend.Record(0x1001, B({ 2, 0 }), B({ 5, 6 }), 10);
    ExpectReplay(suite, extend.PeekUndo(), 0x1000, B({ 1, 5, 6 }), B({ 0, 0, 0 }),
        L"address: an overlapping block extending past the end merges into three bytes");

    // 新块完全覆盖上一步并向两侧延伸：上一步那个字节的 before 仍取最早的 5，而不是新块读到的 9。
    MemoryEditJournal superset;
    superset.Record(0x1001, B({ 5 }), B({ 9 }), 0);
    superset.Record(0x1000, B({ 7, 9, 7 }), B({ 1, 2, 3 }), 10);
    ExpectReplay(suite, superset.PeekUndo(), 0x1000, B({ 1, 2, 3 }), B({ 7, 5, 7 }),
        L"address: a covering block keeps the earliest original byte of the covered position");

    // 新块落在上一步内部。
    MemoryEditJournal inside;
    inside.Record(0x1000, B({ 0, 0, 0, 0 }), B({ 1, 2, 3, 4 }), 0);
    inside.Record(0x1001, B({ 2 }), B({ 9 }), 10);
    ExpectReplay(suite, inside.PeekUndo(), 0x1000, B({ 1, 9, 3, 4 }), B({ 0, 0, 0, 0 }),
        L"address: a block inside the step rewrites only its own byte");
    suite.expect(inside.UndoCount() == 1 && inside.MemoryUsage() == 72ULL, L"address: it stays one 4-byte step");

    // 原地改写同一字节：before 仍是最早的 0。
    MemoryEditJournal retype;
    retype.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    ExpectResult(suite, retype.Record(0x1000, B({ 1 }), B({ 2 }), 100), JournalRecordStatus::Merged, 0, 0, false,
        L"address: retyping the same byte merges");
    ExpectReplay(suite, retype.PeekUndo(), 0x1000, B({ 2 }), B({ 0 }), L"address: the retyped step restores the very first byte");

    // 重叠不一致：目标在两次写入之间被别处改过（9 不等于上一步写下的 1），不合并。
    MemoryEditJournal stale;
    stale.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    ExpectResult(suite, stale.Record(0x1000, B({ 9 }), B({ 2 }), 100), JournalRecordStatus::Recorded, 0, 0, false,
        L"address: an overlap that disagrees with the previous after does not merge");
    ExpectReplay(suite, stale.PeekUndo(), 0x1000, B({ 2 }), B({ 9 }),
        L"address: the unmerged step restores the externally changed byte, not the first one");

    // 只有第二个重叠字节不一致也不能合并（不能只核对第一个字节）。
    MemoryEditJournal secondByte;
    secondByte.Record(0x1000, B({ 0, 0 }), B({ 1, 2 }), 0);
    ExpectResult(suite, secondByte.Record(0x1000, B({ 1, 7 }), B({ 3, 4 }), 100), JournalRecordStatus::Recorded,
        0, 0, false, L"address: a disagreement in the second overlapping byte also blocks the merge");
}

// ------------------------------------------------------------
// 六、合并后净效果为零：整步移除。
// ------------------------------------------------------------
void TestCancelledMerge(KswordTests::Suite& suite) {
    MemoryEditJournal journal;
    journal.Record(0x5000, B({ 3 }), B({ 4 }), 100000);
    journal.Record(0x1000, B({ 0 }), B({ 1 }), 100010);
    ExpectResult(suite, journal.Record(0x1000, B({ 1 }), B({ 0 }), 100020), JournalRecordStatus::Cancelled, 0, 0, false,
        L"cancel: typing back the original byte cancels the step");
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 0 && journal.MemoryUsage() == 66ULL,
        L"cancel: only the unrelated older step is left, and its cost is accounted");
    ExpectReplay(suite, journal.PeekUndo(), 0x5000, B({ 4 }), B({ 3 }), L"cancel: the older step is on top again");

    // 被取消后，上一步重新成为"最近一步"：它的时间窗口还在，相接的新块可以并入。
    ExpectResult(suite, journal.Record(0x5001, B({ 0 }), B({ 8 }), 100030), JournalRecordStatus::Merged, 0, 0, false,
        L"cancel: the step below becomes the merge target again");

    // 只有部分字节净效果为零：不取消。
    MemoryEditJournal partial;
    partial.Record(0x1000, B({ 0, 0 }), B({ 1, 1 }), 0);
    ExpectResult(suite, partial.Record(0x1001, B({ 1 }), B({ 0 }), 10), JournalRecordStatus::Merged, 0, 0, false,
        L"cancel: a merge that still changes one byte is not cancelled");
    ExpectReplay(suite, partial.PeekUndo(), 0x1000, B({ 1, 0 }), B({ 0, 0 }),
        L"cancel: the surviving step keeps the byte that is still different");

    // 单独一步的往返（改了又改回）也是取消，日志回到空。
    MemoryEditJournal alone;
    alone.Record(0x1000, B({ 7, 7 }), B({ 8, 9 }), 0);
    ExpectResult(suite, alone.Record(0x1000, B({ 8, 9 }), B({ 7, 7 }), 5), JournalRecordStatus::Cancelled, 0, 0, false,
        L"cancel: reverting a whole 2-byte step cancels it");
    suite.expect(alone.UndoCount() == 0 && alone.MemoryUsage() == 0 && !alone.PeekUndo().has_value(),
        L"cancel: the journal is empty again");
}

// ------------------------------------------------------------
// 七、合并屏障：撤销/重做之后的第一次记录一律另起一步。
// ------------------------------------------------------------
void TestMergeBarrier(KswordTests::Suite& suite) {
    // 撤销之后：与上一步相接且时间足够近，也不合并；新记录切断重做分支；之后恢复合并。
    MemoryEditJournal afterUndo;
    afterUndo.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    afterUndo.Record(0x5000, B({ 0 }), B({ 1 }), 10);
    afterUndo.MarkUndone();
    ExpectResult(suite, afterUndo.Record(0x1001, B({ 0 }), B({ 2 }), 20), JournalRecordStatus::Recorded, 0, 1, false,
        L"barrier: a block right after an undo starts a new step and cuts the redo branch");
    suite.expect(afterUndo.UndoCount() == 2 && afterUndo.RedoCount() == 0,
        L"barrier: the step below was not merged into");
    ExpectResult(suite, afterUndo.Record(0x1002, B({ 0 }), B({ 3 }), 30), JournalRecordStatus::Merged, 0, 0, false,
        L"barrier: the next block merges again, the barrier was cleared by the record");
    ExpectReplay(suite, afterUndo.PeekUndo(), 0x1001, B({ 2, 3 }), B({ 0, 0 }), L"barrier: the new step absorbed the next block");

    // 重做之后同理。
    MemoryEditJournal afterRedo;
    afterRedo.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    afterRedo.Record(0x5000, B({ 0 }), B({ 1 }), 10);
    afterRedo.MarkUndone();
    afterRedo.MarkRedone();
    ExpectResult(suite, afterRedo.Record(0x5001, B({ 0 }), B({ 2 }), 20), JournalRecordStatus::Recorded, 0, 0, false,
        L"barrier: a block right after a redo does not merge either");
    suite.expect(afterRedo.UndoCount() == 3, L"barrier: three separate steps after the redo");

    // 被拒绝的 Mark 不置位屏障。
    MemoryEditJournal refused;
    suite.expect(!refused.MarkUndone() && !refused.MarkRedone(), L"barrier: marks on an empty journal are refused");
    refused.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    ExpectResult(suite, refused.Record(0x1001, B({ 0 }), B({ 2 }), 10), JournalRecordStatus::Merged, 0, 0, false,
        L"barrier: a refused mark did not set the barrier");

    // 空操作与不合法输入不清屏障：撤销之后它们不算"第一次记录"。
    MemoryEditJournal sticky;
    sticky.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    sticky.Record(0x5000, B({ 0 }), B({ 1 }), 10);
    sticky.MarkUndone();
    sticky.Record(0x1001, B({ 0 }), B({ 0 }), 20);
    sticky.Record(0x1001, JournalBytes{}, JournalBytes{}, 20);
    ExpectResult(suite, sticky.Record(0x1001, B({ 0 }), B({ 2 }), 30), JournalRecordStatus::Recorded, 0, 1, false,
        L"barrier: no-op and invalid records do not clear the barrier");
}

// ------------------------------------------------------------
// 八、新写入切断重做分支。
// ------------------------------------------------------------
void TestRedoBranchCut(KswordTests::Suite& suite) {
    MemoryEditJournal journal;
    journal.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    journal.Record(0x2000, B({ 0 }), B({ 1 }), 10000);
    journal.Record(0x3000, B({ 0 }), B({ 1 }), 20000);
    journal.MarkUndone();
    journal.MarkUndone();
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 2 && journal.MemoryUsage() == 198ULL,
        L"redo cut: two steps are on the redo side, all three still accounted");

    // 空操作与不合法输入都不切。
    journal.Record(0x4000, B({ 1 }), B({ 1 }), 30000);
    journal.Record(0x4000, B({ 1 }), B({ 1, 2 }), 30000);
    suite.expect(journal.RedoCount() == 2, L"redo cut: no-op and invalid blocks keep the redo branch");

    // 真正的新写入切断：被切的两步释放开销。
    ExpectResult(suite, journal.Record(0x4000, B({ 0 }), B({ 1 }), 30000), JournalRecordStatus::Recorded, 0, 2, false,
        L"redo cut: a new write discards the two redo steps");
    suite.expect(journal.UndoCount() == 2 && journal.RedoCount() == 0 && journal.MemoryUsage() == 132ULL,
        L"redo cut: the discarded steps no longer count towards usage");
    suite.expect(!journal.PeekRedo().has_value() && !journal.MarkRedone(), L"redo cut: nothing can be redone any more");
    ExpectReplay(suite, journal.PeekUndo(), 0x4000, B({ 1 }), B({ 0 }), L"redo cut: the new step is on top");
}

// ------------------------------------------------------------
// 九、CheckReplay：整步核对，没有部分匹配。
// ------------------------------------------------------------
void TestCheckReplay(KswordTests::Suite& suite) {
    JournalReplay replay;
    replay.address = 0x1000;
    replay.expectedCurrent = B({ 1, 2, 3 });
    replay.restore = B({ 4, 5, 6 });

    ReplayCheck check = CheckReplay(replay, B({ 1, 2, 3 }));
    suite.expect(check.verdict == ReplayVerdict::Match && check.firstMismatch == 0,
        L"check: identical bytes match");

    // 长度不同：短、长、空。
    suite.expect(CheckReplay(replay, B({ 1, 2 })).verdict == ReplayVerdict::LengthMismatch
        && CheckReplay(replay, B({ 1, 2, 3, 4 })).verdict == ReplayVerdict::LengthMismatch
        && CheckReplay(replay, JournalBytes{}).verdict == ReplayVerdict::LengthMismatch,
        L"check: a shorter, longer or empty read-back is a length mismatch");

    // 内容不同：首、中、末字节各一次，多处不同时报第一个。
    check = CheckReplay(replay, B({ 9, 2, 3 }));
    suite.expect(check.verdict == ReplayVerdict::ContentMismatch && check.firstMismatch == 0,
        L"check: a different first byte is reported at offset 0");
    check = CheckReplay(replay, B({ 1, 9, 3 }));
    suite.expect(check.verdict == ReplayVerdict::ContentMismatch && check.firstMismatch == 1,
        L"check: a different middle byte is reported at offset 1");
    check = CheckReplay(replay, B({ 1, 2, 9 }));
    suite.expect(check.verdict == ReplayVerdict::ContentMismatch && check.firstMismatch == 2,
        L"check: a different last byte alone still rejects the whole step");
    check = CheckReplay(replay, B({ 1, 8, 9 }));
    suite.expect(check.verdict == ReplayVerdict::ContentMismatch && check.firstMismatch == 1,
        L"check: with several differences the first one is reported");
    // 写后值（restore）不能冒充期望值。
    suite.expect(CheckReplay(replay, B({ 4, 5, 6 })).verdict == ReplayVerdict::ContentMismatch,
        L"check: the bytes to be written are not accepted as the current bytes");

    // 回放本身不自洽：不得空洞通过。
    JournalReplay empty;
    suite.expect(CheckReplay(empty, JournalBytes{}).verdict == ReplayVerdict::MalformedReplay,
        L"check: an empty replay never matches, not even an empty read-back");
    JournalReplay unequal;
    unequal.expectedCurrent = B({ 1, 2 });
    unequal.restore = B({ 1 });
    suite.expect(CheckReplay(unequal, B({ 1, 2 })).verdict == ReplayVerdict::MalformedReplay,
        L"check: an expected/restore length disagreement is malformed");
    JournalReplay noExpected;
    noExpected.restore = B({ 1 });
    suite.expect(CheckReplay(noExpected, JournalBytes{}).verdict == ReplayVerdict::MalformedReplay,
        L"check: a replay without expected bytes is malformed");

    // 与日志配合：撤销期望的是 after，重做期望的是 before。
    MemoryEditJournal journal;
    journal.Record(0x1000, B({ 0x10, 0x11 }), B({ 0xAA, 0xBB }), 0);
    suite.expect(CheckReplay(*journal.PeekUndo(), B({ 0xAA, 0xBB })).verdict == ReplayVerdict::Match
        && CheckReplay(*journal.PeekUndo(), B({ 0x10, 0x11 })).verdict == ReplayVerdict::ContentMismatch,
        L"check: undo matches the written bytes and rejects the original bytes");
    journal.MarkUndone();
    suite.expect(CheckReplay(*journal.PeekRedo(), B({ 0x10, 0x11 })).verdict == ReplayVerdict::Match
        && CheckReplay(*journal.PeekRedo(), B({ 0xAA, 0xBB })).verdict == ReplayVerdict::ContentMismatch,
        L"check: redo matches the original bytes and rejects the written bytes");
}

// ------------------------------------------------------------
// 十、完整流程：键入合并、整步撤销、外部改动整步拒绝、写失败可重试。
// ------------------------------------------------------------
void TestWorldFlow(KswordTests::Suite& suite) {
    // 目标内存：地址 0x1000 起 8 字节。
    std::vector<std::uint8_t> memory = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 };
    MemoryEditJournal journal;

    // 连续键入四个字节（每次 300 tick），合并成一步。
    const std::uint8_t typed[] = { 0xAA, 0xBB, 0xCC, 0xDD };
    for (std::size_t index = 0; index < 4; ++index) {
        const JournalBytes before = B({ memory[index] });
        memory[index] = typed[index];
        journal.Record(0x1000 + index, before, B({ typed[index] }), index * 300);
    }
    suite.expect(journal.UndoCount() == 1, L"flow: four typed bytes are one undo step");

    // 外部程序改了第二个字节：整步拒绝，日志与目标都不动。
    memory[1] = 0x77;
    const std::optional<JournalReplay> offered = journal.PeekUndo();
    const ReplayCheck rejected = CheckReplay(*offered, JournalBytes(memory.begin(), memory.begin() + 4));
    suite.expect(rejected.verdict == ReplayVerdict::ContentMismatch && rejected.firstMismatch == 1,
        L"flow: an external change to byte 1 rejects the whole step");
    suite.expect(journal.UndoCount() == 1 && journal.RedoCount() == 0 && memory[0] == 0xAA && memory[3] == 0xDD,
        L"flow: a rejected undo changes neither the journal nor the other bytes");

    // 外部改动被还原后核对通过；写入失败时不标记，步骤原样保留，可重试。
    memory[1] = 0xBB;
    const ReplayCheck accepted = CheckReplay(*journal.PeekUndo(), JournalBytes(memory.begin(), memory.begin() + 4));
    suite.expect(accepted.verdict == ReplayVerdict::Match, L"flow: the step can be undone once the bytes match again");
    suite.expect(journal.UndoCount() == 1 && journal.CanUndo(), L"flow: a failed write (no MarkUndone) keeps the step in place");
    ExpectReplay(suite, journal.PeekUndo(), 0x1000, B({ 0xAA, 0xBB, 0xCC, 0xDD }), B({ 0x10, 0x11, 0x12, 0x13 }),
        L"flow: the retry offers exactly the same replay");

    // 写成功后标记：目标回到原样；重做回到键入后的样子。
    for (std::size_t index = 0; index < 4; ++index) {
        memory[index] = journal.PeekUndo()->restore[index];
    }
    journal.MarkUndone();
    suite.expect(memory[0] == 0x10 && memory[1] == 0x11 && memory[2] == 0x12 && memory[3] == 0x13
        && journal.UndoCount() == 0 && journal.RedoCount() == 1, L"flow: one undo restores all four bytes");
    suite.expect(CheckReplay(*journal.PeekRedo(), JournalBytes(memory.begin(), memory.begin() + 4)).verdict
        == ReplayVerdict::Match, L"flow: the redo precondition holds after the undo");
}

// ------------------------------------------------------------
// 十一、步数容量：256 / 257 两侧、最旧先丢、切断先于丢弃。
// ------------------------------------------------------------
void TestStepCapacity(KswordTests::Suite& suite) {
    MemoryEditJournal journal;
    int droppedEarly = 0;
    for (std::uint64_t index = 0; index < 256; ++index) {
        // 地址相隔 0x100、tick 相隔 10000：互不相接，不会合并。
        const JournalRecordResult result = journal.Record(0x10000 + index * 0x100, B({ 0 }), B({ 1 }), index * 10000);
        droppedEarly += result.droppedOldest > 0 ? 1 : 0;
    }
    suite.expect(droppedEarly == 0 && journal.UndoCount() == 256 && journal.MemoryUsage() == 16896ULL,
        L"steps: exactly 256 steps fit without dropping (256 * 66 = 16896)");

    // 第 257 步：丢弃最旧一步并报告。
    ExpectResult(suite, journal.Record(0x10000 + 256 * 0x100, B({ 0 }), B({ 1 }), 256 * 10000),
        JournalRecordStatus::Recorded, 1, 0, false, L"steps: the 257th step drops exactly one oldest step");
    suite.expect(journal.UndoCount() == 256 && journal.MemoryUsage() == 16896ULL,
        L"steps: the count and usage stay at the limit");
    std::uint64_t lastUndone = 0;
    int undone = 0;
    while (journal.PeekUndo().has_value()) {
        lastUndone = journal.PeekUndo()->address;
        journal.MarkUndone();
        ++undone;
    }
    suite.expect(undone == 256 && lastUndone == 0x10100ULL,
        L"steps: the oldest step (0x10000) is gone and 0x10100 is the oldest left");

    // 小上限：3 步。
    MemoryEditJournal three(kEditJournalMaxBytes, 3);
    three.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    three.Record(0x2000, B({ 0 }), B({ 1 }), 10000);
    three.Record(0x3000, B({ 0 }), B({ 1 }), 20000);
    ExpectResult(suite, three.Record(0x4000, B({ 0 }), B({ 1 }), 30000), JournalRecordStatus::Recorded, 1, 0, false,
        L"steps: with a limit of 3 the 4th step drops one");
    suite.expect(three.UndoCount() == 3, L"steps: three steps are kept");

    // 切断先于丢弃：撤销一步后写入新块，被切掉的步数腾出了名额，不该再丢最旧的。
    three.MarkUndone();
    ExpectResult(suite, three.Record(0x5000, B({ 0 }), B({ 1 }), 40000), JournalRecordStatus::Recorded, 0, 1, false,
        L"steps: the redo cut frees a slot first, so nothing else is dropped");
    suite.expect(three.UndoCount() == 3, L"steps: still three steps after the cut");

    // 上限 1 与 0。
    MemoryEditJournal one(kEditJournalMaxBytes, 1);
    one.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    ExpectResult(suite, one.Record(0x2000, B({ 0 }), B({ 1 }), 10000), JournalRecordStatus::Recorded, 1, 0, false,
        L"steps: with a limit of 1 every new step replaces the previous one");
    ExpectReplay(suite, one.PeekUndo(), 0x2000, B({ 1 }), B({ 0 }), L"steps: the survivor is the newest");
    MemoryEditJournal none(kEditJournalMaxBytes, 0);
    ExpectResult(suite, none.Record(0x1000, B({ 0 }), B({ 1 }), 0), JournalRecordStatus::TooLarge, 0, 0, false,
        L"steps: a journal limited to 0 steps records nothing");
    suite.expect(none.UndoCount() == 0 && none.MemoryUsage() == 0, L"steps: and holds nothing");
}

// ------------------------------------------------------------
// 十二、字节容量：恰好 / 多一字节、单块过大清空、多步连丢。
// ------------------------------------------------------------
void TestByteCapacity(KswordTests::Suite& suite) {
    // 100 字节的块开销 264。容量 1056 = 4 * 264：四步恰好放下。
    MemoryEditJournal exact(1056, 256);
    for (std::uint64_t index = 0; index < 4; ++index) {
        suite.expect(exact.Record(0x10000 + index * 0x1000, Filled(100, 0), Filled(100, 1), index * 10000)
            .droppedOldest == 0, L"bytes: four 100-byte steps fit a capacity of exactly 1056");
    }
    suite.expect(exact.MemoryUsage() == 1056ULL && exact.UndoCount() == 4, L"bytes: usage equals the capacity");
    ExpectResult(suite, exact.Record(0x20000, Filled(100, 0), Filled(100, 1), 50000), JournalRecordStatus::Recorded,
        1, 0, false, L"bytes: the fifth step drops the oldest one");
    suite.expect(exact.MemoryUsage() == 1056ULL && exact.UndoCount() == 4, L"bytes: usage stays at the capacity");

    // 容量 1055：少一字节，第四步就放不下。
    MemoryEditJournal shy(1055, 256);
    shy.Record(0x10000, Filled(100, 0), Filled(100, 1), 0);
    shy.Record(0x11000, Filled(100, 0), Filled(100, 1), 10000);
    shy.Record(0x12000, Filled(100, 0), Filled(100, 1), 20000);
    ExpectResult(suite, shy.Record(0x13000, Filled(100, 0), Filled(100, 1), 30000), JournalRecordStatus::Recorded,
        1, 0, false, L"bytes: one byte short of 1056 the fourth step drops one");
    suite.expect(shy.UndoCount() == 3 && shy.MemoryUsage() == 792ULL, L"bytes: three steps = 792 remain");

    // 单块边界：容量 264 恰好放下 100 字节，101 字节（266）放不下。
    MemoryEditJournal single(264, 256);
    ExpectResult(suite, single.Record(0x1000, Filled(100, 0), Filled(100, 1), 0), JournalRecordStatus::Recorded,
        0, 0, false, L"bytes: a block whose cost equals the capacity is recorded");
    ExpectResult(suite, single.Record(0x9000, Filled(101, 0), Filled(101, 1), 100000), JournalRecordStatus::TooLarge,
        0, 0, true, L"bytes: a block one byte too large is refused and clears the history");
    suite.expect(single.UndoCount() == 0 && single.RedoCount() == 0 && single.MemoryUsage() == 0,
        L"bytes: the cleared journal is empty");
    ExpectResult(suite, single.Record(0x9000, Filled(101, 0), Filled(101, 1), 200000), JournalRecordStatus::TooLarge,
        0, 0, false, L"bytes: refusing on an empty journal reports no cleared history");

    // 清空也包括重做分支。
    MemoryEditJournal withRedo(264, 256);
    withRedo.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    withRedo.Record(0x5000, B({ 0 }), B({ 1 }), 10000);
    withRedo.MarkUndone();
    ExpectResult(suite, withRedo.Record(0x9000, Filled(101, 0), Filled(101, 1), 20000), JournalRecordStatus::TooLarge,
        0, 0, true, L"bytes: a too-large block also clears the redo branch");
    suite.expect(withRedo.UndoCount() == 0 && withRedo.RedoCount() == 0, L"bytes: nothing left on either side");

    // 66 与 63：一字节块的开销恰为 66。
    MemoryEditJournal tiny(66, 256);
    tiny.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    ExpectResult(suite, tiny.Record(0x9000, B({ 0 }), B({ 1 }), 100000), JournalRecordStatus::Recorded, 1, 0, false,
        L"bytes: with capacity 66 a second 1-byte step replaces the first");
    ExpectResult(suite, tiny.Record(0x2000, B({ 0, 0 }), B({ 1, 1 }), 200000), JournalRecordStatus::TooLarge, 0, 0, true,
        L"bytes: a 2-byte block (cost 68) exceeds capacity 66");
    MemoryEditJournal below(63, 256);
    ExpectResult(suite, below.Record(0x1000, B({ 0 }), B({ 1 }), 0), JournalRecordStatus::TooLarge, 0, 0, false,
        L"bytes: a capacity below the fixed overhead records nothing");

    // 一次丢多步：容量 1000，三步 264 再加 464 与 464。
    MemoryEditJournal multi(1000, 256);
    multi.Record(0x10000, Filled(100, 0), Filled(100, 1), 0);
    multi.Record(0x11000, Filled(100, 0), Filled(100, 1), 10000);
    multi.Record(0x12000, Filled(100, 0), Filled(100, 1), 20000);
    ExpectResult(suite, multi.Record(0x13000, Filled(200, 0), Filled(200, 1), 30000), JournalRecordStatus::Recorded,
        1, 0, false, L"bytes: a 464-byte-cost step drops one 264 step (792 + 464 > 1000, 528 + 464 = 992)");
    suite.expect(multi.MemoryUsage() == 992ULL && multi.UndoCount() == 3, L"bytes: usage 992 after the first big step");
    ExpectResult(suite, multi.Record(0x14000, Filled(200, 0), Filled(200, 1), 40000), JournalRecordStatus::Recorded,
        2, 0, false, L"bytes: the next big step drops two steps (992 -> 728 -> 464, then 464 + 464 = 928)");
    suite.expect(multi.MemoryUsage() == 928ULL && multi.UndoCount() == 2, L"bytes: the two big steps remain, 928");
}

// ------------------------------------------------------------
// 十三、默认 32 MiB 容量的精确边界（单块 16777184 字节 = 恰好 33554432）。
// ------------------------------------------------------------
void TestDefaultByteCapacityBoundary(KswordTests::Suite& suite) {
    constexpr std::size_t kFits = 16777184;
    MemoryEditJournal journal;
    JournalBytes before(kFits, 0);
    JournalBytes after(kFits, 0);
    after[0] = 1;

    ExpectResult(suite, journal.Record(0x100000, before, after, 0), JournalRecordStatus::Recorded, 0, 0, false,
        L"default capacity: a 16777184-byte block (cost exactly 33554432) is recorded");
    suite.expect(journal.MemoryUsage() == 33554432ULL && journal.MemoryUsage() == journal.MaxBytes(),
        L"default capacity: usage equals the 32 MiB capacity");

    // 再记一个 1 字节块：旧的大块被丢弃为它腾地方。
    ExpectResult(suite, journal.Record(0x9000000, B({ 0 }), B({ 1 }), 100000), JournalRecordStatus::Recorded, 1, 0, false,
        L"default capacity: the huge step is dropped to make room for a small one");
    suite.expect(journal.UndoCount() == 1 && journal.MemoryUsage() == 66ULL, L"default capacity: only the small step remains");

    // 多一字节：单块放不下，清空历史。
    MemoryEditJournal over;
    over.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    before.push_back(0);
    after.push_back(0);
    ExpectResult(suite, over.Record(0x100000, before, after, 100000), JournalRecordStatus::TooLarge, 0, 0, true,
        L"default capacity: a 16777185-byte block is refused and clears the history");
    suite.expect(over.UndoCount() == 0 && over.MemoryUsage() == 0, L"default capacity: the journal is empty afterwards");
}

// ------------------------------------------------------------
// 十四、合并让步骤变大时的容量处理。
// ------------------------------------------------------------
void TestMergeGrowthCapacity(KswordTests::Suite& suite) {
    // 合并后总开销越界：丢最旧的步骤，被并入的那一步保留。
    MemoryEditJournal grow(300, 256);
    grow.Record(0x1000, Filled(10, 0), Filled(10, 1), 0);
    grow.Record(0x9000, Filled(10, 0), Filled(10, 1), 100000);
    ExpectResult(suite, grow.Record(0x900A, Filled(100, 0), Filled(100, 1), 100010), JournalRecordStatus::Merged,
        1, 0, false, L"growth: merging into a step grows it past the total and drops the oldest step");
    suite.expect(grow.UndoCount() == 1 && grow.MemoryUsage() == 284ULL,
        L"growth: the merged 110-byte step (284) is the only one left");
    suite.expect(grow.PeekUndo()->address == 0x9000ULL && grow.PeekUndo()->restore.size() == 110,
        L"growth: it is the merged step at 0x9000, 110 bytes long");

    // 并集自己就放不下：不合并，新块（单独放得下）另起一步并挤掉旧步骤。
    MemoryEditJournal refuse(500, 256);
    refuse.Record(0x9000, Filled(150, 0), Filled(150, 1), 0);
    ExpectResult(suite, refuse.Record(0x9096, Filled(100, 0), Filled(100, 1), 10), JournalRecordStatus::Recorded,
        1, 0, false, L"growth: a union that cannot fit is not merged; the block becomes its own step");
    suite.expect(refuse.UndoCount() == 1 && refuse.PeekUndo()->address == 0x9096ULL && refuse.MemoryUsage() == 264ULL,
        L"growth: only the new 100-byte step is left");
}

// ------------------------------------------------------------
// 十五、换目标：七个字段各自触发清空，提示只给一次。
// ------------------------------------------------------------
void TestBindTarget(KswordTests::Suite& suite) {
    const MemoryTargetSession base = MakeSession();

    // 第一次绑定：绑定前记下的步骤来路不明，清掉并提示；之后同一目标不再动。
    MemoryEditJournal journal;
    journal.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    suite.expect(journal.BindTarget(base) && journal.UndoCount() == 0,
        L"bind: the first bind clears history recorded before any target was known");
    suite.expect(!journal.BindTarget(base), L"bind: binding the same target again does nothing and reports nothing");
    journal.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    suite.expect(!journal.BindTarget(base) && journal.UndoCount() == 1,
        L"bind: the same target keeps its history");

    // 空历史换目标：不提示（没有东西被清掉），但目标已更新。
    MemoryEditJournal empty;
    suite.expect(!empty.BindTarget(base), L"bind: the first bind of an empty journal reports nothing");
    MemoryTargetSession other = base;
    other.pid = 4321;
    suite.expect(!empty.BindTarget(other), L"bind: changing the target of an empty journal reports nothing");
    empty.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    suite.expect(empty.BindTarget(base) && empty.UndoCount() == 0,
        L"bind: the target was updated silently, so going back to the old one clears the new history");

    // 七个字段逐个变一次：每个都必须触发清空（含重做分支），且只提示一次。
    for (int field = 0; field < 7; ++field) {
        MemoryEditJournal each;
        each.BindTarget(base);
        each.Record(0x1000, B({ 0 }), B({ 1 }), 0);
        each.Record(0x5000, B({ 0 }), B({ 1 }), 10000);
        each.MarkUndone();
        MemoryTargetSession changed = base;
        switch (field) {
        case 0: changed.scope = Scope::KernelVirtual; break;
        case 1: changed.pid = base.pid + 1; break;
        case 2: changed.processCreateTime100ns = base.processCreateTime100ns + 1; break;
        case 3: changed.attachGeneration = base.attachGeneration + 1; break;
        case 4: changed.channel = Channel::StandardDriver; break;
        case 5: changed.ddmaGeneration = base.ddmaGeneration + 1; break;
        default: changed.addressBits = 32; break;
        }
        suite.expect(each.BindTarget(changed) && each.UndoCount() == 0 && each.RedoCount() == 0
            && each.MemoryUsage() == 0, L"bind: a change in any one of the seven session fields clears the journal");
        suite.expect(!each.BindTarget(changed), L"bind: the clear is reported only once");
    }

    // Clear 不改已绑定的目标。
    MemoryEditJournal cleared;
    cleared.BindTarget(base);
    cleared.Record(0x1000, B({ 0 }), B({ 1 }), 0);
    cleared.Clear();
    suite.expect(cleared.UndoCount() == 0 && !cleared.BindTarget(base),
        L"bind: Clear keeps the bound target, so rebinding it reports nothing");
}

// ------------------------------------------------------------
// 十六、随机对拍：被测日志 vs 逐字节映射的朴素参考实现。
// ------------------------------------------------------------
void TestRandomDifferential(KswordTests::Suite& suite) {
    const RandomStats total = RunStandardRandomSuite();
    suite.expect(total.mismatches == 0,
        L"random: the journal agrees with the naive per-byte reference on every operation");
    suite.expect(total.unwindFailures == 0,
        L"random: undoing everything restores the initial image and redoing everything restores the final one");
    // 对拍不能空转：每一类事件都必须真的发生过足够多次。
    suite.expect(total.merged > 200 && total.cancelled > 20 && total.recorded > 500 && total.unchanged > 50,
        L"random: merges, cancellations, plain records and no-ops were all exercised");
    suite.expect(total.dropped > 50 && total.tooLarge > 5 && total.redoCuts > 50,
        L"random: capacity drops, oversize clears and redo cuts were all exercised");
    suite.expect(total.undone > 200 && total.redone > 50 && total.replayRejected > 5,
        L"random: undo, redo and rejected replays were all exercised");
}

} // namespace

int RunMemwbEditJournalTests() {
    KswordTests::Suite suite(L"MEMWB edit journal");
    TestConstantsAndDefaults(suite);
    TestRecordAndCursor(suite);
    TestInvalidAndUnchanged(suite);
    TestMergeTimeBoundary(suite);
    TestMergeAddressRules(suite);
    TestCancelledMerge(suite);
    TestMergeBarrier(suite);
    TestRedoBranchCut(suite);
    TestCheckReplay(suite);
    TestWorldFlow(suite);
    TestStepCapacity(suite);
    TestByteCapacity(suite);
    TestDefaultByteCapacityBoundary(suite);
    TestMergeGrowthCapacity(suite);
    TestBindTarget(suite);
    TestRandomDifferential(suite);
    suite.report();
    return suite.failures();
}
