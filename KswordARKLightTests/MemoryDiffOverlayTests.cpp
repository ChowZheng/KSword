// 暂存编辑叠加层（shared/evidence/memory_workbench/MemoryDiffOverlay.h）的离线测试。
//
// 为什么值得一整套穷举断言：叠加层决定"用户看到的"和"最终写进目标的"是不是同一份
// 字节。它出错不会报错——合并时错用了中间值当原字节，写前复核就复核了个寂寞；
// 空操作编辑没消失，会多写一次不该写的字节；自己写入的字节被当成外部变化，
// 篡改检测就会天天误报，最终没人信它。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值全部手算写死，绝不从被测函数反算。基线统一取 地址 0x1000 + i 处
//     的字节 = i * 0x11（00 11 22 ... FF），暂存值一律取 0x01~0x0F 之类不会碰巧
//     等于基线的数，免得"等于基线则消失"的规则误伤断言；
//   * 边界两侧都测（窗口起止、地址溢出、暂存上限、空操作与非空操作）；
//   * 拒绝路径必须显式测，且被拒绝后状态必须原封不动；
//   * 同一条规则在 LoadBaseline 与 RefreshBaseline 下各测一遍，两者的区别
//     （补丁是否保留）正是这个类存在的理由。

#include "MemoryDiffOverlayTestSupport.h"

namespace {

using namespace MemwbOverlayTests;

// ------------------------------------------------------------
// 一、基线载入：校验、拒绝不改状态、掩码规整。
// ------------------------------------------------------------
void TestLoadBaselineRules(KswordTests::Suite& suite) {
    // 掩码长度必须与字节长度相同，短一个、长一个都拒绝，且首次载入失败不留基线。
    MemoryDiffOverlay rejected;
    suite.expect(rejected.LoadBaseline("k", 0x1000, {1, 2, 3}, {1, 1}) == BaselineLoadStatus::MaskLengthMismatch,
        L"load: a mask one byte short is rejected");
    suite.expect(rejected.LoadBaseline("k", 0x1000, {1, 2, 3}, {1, 1, 1, 1}) == BaselineLoadStatus::MaskLengthMismatch,
        L"load: a mask one byte long is rejected");
    suite.expect(!rejected.HasBaseline(), L"load: a rejected first load leaves no baseline behind");

    // 窗口终点溢出：终点 = 基址 + 长度，恰为 uint64 最大值合法，多 1 就拒绝。
    suite.expect(rejected.LoadBaseline("k", kTop - 3, {1, 2, 3, 4}, {1, 1, 1, 1}) == BaselineLoadStatus::AddressOverflow,
        L"load: a window whose end passes the top of the address space is rejected");
    suite.expect(rejected.LoadBaseline("k", kTop - 4, {1, 2, 3, 4}, {1, 1, 1, 1}) == BaselineLoadStatus::Ok,
        L"load: a window ending exactly at the top of the address space is accepted");
    suite.expect(rejected.BaseAddress() == kTop - 4 && rejected.BaselineSize() == 4 && rejected.IdentityKey() == "k",
        L"load: an accepted load records base address, size and identity");

    // 空窗口合法（整段读取失败时调用方可以这样重置）。
    MemoryDiffOverlay empty;
    suite.expect(empty.LoadBaseline("k", 0x10, {}, {}) == BaselineLoadStatus::Ok && empty.HasBaseline()
        && empty.BaselineSize() == 0,
        L"load: an empty window is accepted");

    // 被拒绝的载入不改任何状态：基线、身份、补丁都原样。
    MemoryDiffOverlay keep = MakeOverlay();
    keep.Stage(0x1002, {0x01});
    const Blocks pending = keep.DiffBlocks();
    suite.expect(keep.LoadBaseline("target-B", 0x9000, {1, 2}, {1}) == BaselineLoadStatus::MaskLengthMismatch,
        L"load: the mismatching load is rejected");
    suite.expect(keep.RefreshBaseline("target-B", 0x9000, {1, 2}, {1}) == BaselineLoadStatus::MaskLengthMismatch,
        L"refresh: the mismatching refresh is rejected");
    suite.expect(keep.DiffBlocks() == pending && keep.PendingByteCount() == 1,
        L"load: rejected loads and refreshes keep the staged patches");
    suite.expect(keep.IdentityKey() == "target-A" && keep.BaseAddress() == kBase && keep.BaselineSize() == 16,
        L"load: rejected loads and refreshes keep the previous baseline");

    // 掩码里任何非零值都按"已读到"处理；没读到的字节在视图里固定为 0，不泄漏缓冲里的垃圾值。
    MemoryDiffOverlay normalised;
    normalised.LoadBaseline("k", 0x1000, {0xAA, 0xEE, 0xCC}, {1, 0, 2});
    const auto view = normalised.Materialize(0x1000, 3);
    suite.expect(view.ok && view.bytes == Bytes{0xAA, 0x00, 0xCC},
        L"load: an unread byte is reported as 0 and the garbage under it is not leaked");
    suite.expect(view.validMask == Bytes{1, 0, 1}, L"load: a non-zero mask value is normalised to 1");
}

// ------------------------------------------------------------
// 二、暂存的基本形态：before/after、升序、基线不被改动。
// ------------------------------------------------------------
void TestStageBasics(KswordTests::Suite& suite) {
    MemoryDiffOverlay overlay = MakeOverlay();
    suite.expect(!overlay.HasPendingPatches() && overlay.PendingByteCount() == 0,
        L"stage: a fresh overlay has no pending patches");

    // 单字节：before 取基线原值 0x22，after 取写入值。
    suite.expect(overlay.Stage(0x1002, {0x01}) == StageStatus::Ok, L"stage: a single byte is accepted");
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1002, {0x22}, {0x01} } },
        L"stage: the block records the baseline byte as before and the new byte as after");
    suite.expect(overlay.HasPendingPatches() && overlay.PendingByteCount() == 1,
        L"stage: the pending count follows the staged bytes");

    // 乱序暂存的补丁按地址升序列出。
    overlay.Stage(0x1008, {0x0A, 0x0B});
    overlay.Stage(0x1000, {0x09});
    suite.expect(overlay.DiffBlocks() == Blocks{
            { 0x1000, {0x00}, {0x09} },
            { 0x1002, {0x22}, {0x01} },
            { 0x1008, {0x88, 0x99}, {0x0A, 0x0B} } },
        L"stage: blocks are listed in ascending address order whatever the staging order");
    suite.expect(overlay.PendingByteCount() == 4, L"stage: pending count adds up across blocks");

    // 视图 = 基线叠加补丁；基线本身不被改动。
    const auto view = overlay.Materialize(0x1000, 5);
    suite.expect(view.ok && view.bytes == Bytes{0x09, 0x11, 0x01, 0x33, 0x44} && view.validMask == Bytes{1, 1, 1, 1, 1},
        L"stage: the materialised view shows the patch over the baseline");
    suite.expect(overlay.EffectiveByte(0x1002) == Val(0x01) && overlay.EffectiveByte(0x1003) == Val(0x33),
        L"stage: the effective byte is the patch where one exists and the baseline elsewhere");
    suite.expect(overlay.EffectiveByte(0x0FFF) == std::nullopt, L"stage: a byte outside the window has no value");
    suite.expect(overlay.BaselineByte(0x1002) == Val(0x22), L"stage: staging does not modify the baseline");
}

// ------------------------------------------------------------
// 三、暂存的拒绝路径：原因枚举、检查顺序、拒绝后状态不变。
// ------------------------------------------------------------
void TestStageRejections(KswordTests::Suite& suite) {
    MemoryDiffOverlay none;
    suite.expect(none.Stage(0x1000, {0x01}) == StageStatus::OutOfWindow,
        L"stage: staging before any baseline is loaded is rejected as out of window");

    MemoryDiffOverlay overlay = MakeOverlay();
    suite.expect(overlay.Stage(0x1000, {}) == StageStatus::Empty, L"stage: an empty edit is rejected");
    suite.expect(overlay.Stage(kTop, {}) == StageStatus::Empty,
        L"stage: emptiness is reported even when the address is also unusable");

    // 窗口为 [0x1000, 0x1010)：两侧各偏一个字节、跨起点、跨终点、过长、很远，全部拒绝。
    suite.expect(overlay.Stage(0x0FFF, {0x01}) == StageStatus::OutOfWindow, L"stage: one byte below the window is rejected");
    suite.expect(overlay.Stage(0x1010, {0x01}) == StageStatus::OutOfWindow, L"stage: one byte past the window is rejected");
    suite.expect(overlay.Stage(0x0FFF, {0x01, 0x02}) == StageStatus::OutOfWindow,
        L"stage: a range straddling the window start is rejected");
    suite.expect(overlay.Stage(0x100F, {0x01, 0x02}) == StageStatus::OutOfWindow,
        L"stage: a range straddling the window end is rejected");
    suite.expect(overlay.Stage(0x1000, Bytes(17, 0x01)) == StageStatus::OutOfWindow,
        L"stage: a range one byte longer than the window is rejected");
    suite.expect(overlay.Stage(0x5000, {0x01}) == StageStatus::OutOfWindow, L"stage: a distant address is rejected");
    suite.expect(!overlay.HasPendingPatches(), L"stage: rejected edits leave nothing staged");

    // 窗口首尾两个字节都合法。
    suite.expect(overlay.Stage(0x1000, {0x01}) == StageStatus::Ok && overlay.Stage(0x100F, {0x02}) == StageStatus::Ok,
        L"stage: the first and the last byte of the window are both editable");
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1000, {0x00}, {0x01} }, { 0x100F, {0xFF}, {0x02} } },
        L"stage: boundary edits are recorded exactly");
    MemoryDiffOverlay whole = MakeOverlay();
    suite.expect(whole.Stage(0x1000, Bytes(16, 0x01)) == StageStatus::Ok && whole.PendingByteCount() == 16,
        L"stage: the whole window can be staged in one edit");

    // 没读到的字节不能编辑；第 2 个字节（0x2002）没读到。
    MemoryDiffOverlay partial;
    partial.LoadBaseline("k", 0x2000, {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17}, {1, 1, 0, 1, 1, 1, 1, 1});
    suite.expect(partial.Stage(0x2001, {0x01, 0x02}) == StageStatus::UnreadBytes,
        L"stage: a range ending on an unread byte is rejected");
    suite.expect(partial.Stage(0x2002, {0x01}) == StageStatus::UnreadBytes, L"stage: the unread byte itself is rejected");
    suite.expect(partial.Stage(0x2000, {0x01, 0x02, 0x03}) == StageStatus::UnreadBytes,
        L"stage: a range starting before an unread byte and covering it is rejected");
    suite.expect(!partial.HasPendingPatches(), L"stage: unread rejections leave nothing staged");
    suite.expect(partial.Stage(0x2003, {0x01, 0x02}) == StageStatus::Ok && partial.Stage(0x2000, {0x01, 0x02}) == StageStatus::Ok,
        L"stage: ranges on either side of the unread byte are accepted");
    suite.expect(partial.DiffBlocks() == Blocks{ { 0x2000, {0x10, 0x11}, {0x01, 0x02} }, { 0x2003, {0x13, 0x14}, {0x01, 0x02} } },
        L"stage: the accepted ranges stay separate across the unread byte");

    // 检查顺序：越界先于"没读到"；溢出先于越界。
    MemoryDiffOverlay edge;
    edge.LoadBaseline("k", 0x2000, {0x10, 0x11}, {0, 1});
    suite.expect(edge.Stage(0x1FFF, {0x01, 0x02}) == StageStatus::OutOfWindow,
        L"stage: out of window is reported before unread bytes");
    suite.expect(overlay.Stage(kTop, {0x01, 0x02}) == StageStatus::AddressOverflow,
        L"stage: overflow is reported before out of window");
}

// ------------------------------------------------------------
// 四、地址溢出边界：终点恰为 uint64 最大值合法，再多一个字节就拒绝，绝不回绕。
// ------------------------------------------------------------
void TestAddressOverflowBoundaries(KswordTests::Suite& suite) {
    // 窗口为 [kTop-16, kTop)，最后 4 个字节（0xCC 0xDD 0xEE 0xFF）紧贴地址空间顶端。
    MemoryDiffOverlay top;
    top.LoadBaseline("k", kTop - 16, StandardBaseline(), Bytes(16, 1));
    suite.expect(top.Stage(kTop - 3, {1, 2, 3, 4}) == StageStatus::AddressOverflow,
        L"overflow: an edit whose end is one past the largest address is rejected");
    suite.expect(top.Stage(kTop, {1}) == StageStatus::AddressOverflow, L"overflow: the very last address cannot be edited");
    suite.expect(top.Stage(kTop - 1, {1, 2, 3}) == StageStatus::AddressOverflow, L"overflow: a longer overshoot is rejected");
    suite.expect(!top.HasPendingPatches(), L"overflow: rejected edits leave nothing staged");
    suite.expect(top.Stage(kTop - 4, {1, 2, 3, 4}) == StageStatus::Ok,
        L"overflow: an edit ending exactly at the largest address is accepted");
    suite.expect(top.DiffBlocks() == Blocks{ { kTop - 4, {0xCC, 0xDD, 0xEE, 0xFF}, {1, 2, 3, 4} } },
        L"overflow: the accepted edit next to the top is recorded exactly");

    // 回绕陷阱：address + length 回绕成一个落在窗口内的小数，朴素的 "end <= windowEnd" 会放行。
    MemoryDiffOverlay wrap;
    wrap.LoadBaseline("k", 0, StandardBaseline(), Bytes(16, 1));
    suite.expect(wrap.Stage(kTop - 1, {1, 2, 3, 4}) == StageStatus::AddressOverflow,
        L"overflow: an end that wraps into the window is still an overflow");
    suite.expect(!wrap.HasPendingPatches(), L"overflow: the wrapped edit left nothing staged");

    // 视图、写入确认、丢弃都用同一条溢出判据。
    suite.expect(top.Materialize(kTop - 4, 4).ok && !top.Materialize(kTop - 3, 4).ok && !top.Materialize(kTop, 1).ok,
        L"overflow: materialise accepts an end at the top and rejects one past it");
    suite.expect(top.Materialize(kTop - 4, 4).bytes == Bytes{1, 2, 3, 4},
        L"overflow: the view next to the top shows the patch");
    DiffBlock overshoot{ kTop - 3, {1, 2, 3, 4}, {1, 2, 3, 4} };
    suite.expect(top.AcceptWrite(overshoot, {1, 2, 3, 4}) == AcceptWriteStatus::AddressOverflow,
        L"overflow: accepting a write that overshoots the top is rejected");
    suite.expect(top.PendingByteCount() == 4, L"overflow: the rejected accept left the patch in place");
    // 再暂存一个起点更靠前的块，让溢出的丢弃范围能"压"到它（回绕后的终点是个很小的数）。
    top.Stage(kTop - 8, {5, 6, 7});
    suite.expect(top.Discard(kTop - 7, 10) == 0 && top.PendingByteCount() == 7,
        L"overflow: a discard range that overflows does nothing");
    suite.expect(top.Discard(kTop - 5, 5) == 4 && top.DiffBlocks() == Blocks{ { kTop - 8, {0x88, 0x99, 0xAA}, {5, 6, 7} } },
        L"overflow: a discard range ending exactly at the top still works");
}

// ------------------------------------------------------------
// 五、合并：相邻、重叠、包含、桥接、包住多个块。
// ------------------------------------------------------------
void TestMergeBehaviour(KswordTests::Suite& suite) {
    // 相邻与桥接。先放 0x1004 与 0x1008 两个互不相邻的块。
    MemoryDiffOverlay overlay = MakeOverlay();
    overlay.Stage(0x1004, {0x01});
    overlay.Stage(0x1008, {0x02});
    suite.expect(overlay.DiffBlocks().size() == 2, L"merge: blocks with a gap between them stay separate");

    // 紧贴右侧：0x1005 接在 0x1004 后面并入同一块。
    overlay.Stage(0x1005, {0x03});
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1004, {0x44, 0x55}, {0x01, 0x03} }, { 0x1008, {0x88}, {0x02} } },
        L"merge: a byte adjacent on the right joins the earlier block");

    // 紧贴左侧：0x1007 排在 0x1008 前面并入它，与 0x1004 块之间仍隔着 0x1006。
    overlay.Stage(0x1007, {0x04});
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1004, {0x44, 0x55}, {0x01, 0x03} }, { 0x1007, {0x77, 0x88}, {0x04, 0x02} } },
        L"merge: a byte adjacent on the left joins the later block");

    // 桥接：0x1006 把两块连成一块。
    overlay.Stage(0x1006, {0x05});
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1004, {0x44, 0x55, 0x66, 0x77, 0x88}, {0x01, 0x03, 0x05, 0x04, 0x02} } },
        L"merge: a byte filling the gap bridges two blocks into one");
    suite.expect(overlay.PendingByteCount() == 5, L"merge: the pending count after bridging is the sum of the parts");

    // 重叠：新范围压住旧块的后两个字节，重叠处 before 仍是基线原值而不是中间值 3、4。
    MemoryDiffOverlay overlap = MakeOverlay();
    overlap.Stage(0x1002, {1, 2, 3, 4});
    overlap.Stage(0x1004, {5, 6, 7, 8});
    suite.expect(overlap.DiffBlocks() == Blocks{ { 0x1002, {0x22, 0x33, 0x44, 0x55, 0x66, 0x77}, {1, 2, 5, 6, 7, 8} } },
        L"merge: overlapping edits merge and the overlap keeps the original byte as before");
    suite.expect(overlap.PendingByteCount() == 6, L"merge: overlapping bytes are counted once");

    // 包含：新范围完全落在旧块里，只改 after，长度不变。
    MemoryDiffOverlay inside = MakeOverlay();
    inside.Stage(0x1000, {1, 2, 3, 4, 5, 6});
    inside.Stage(0x1002, {9, 9});
    suite.expect(inside.DiffBlocks() == Blocks{ { 0x1000, {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, {1, 2, 9, 9, 5, 6} } },
        L"merge: an edit inside an existing block only changes its after bytes");
    suite.expect(inside.PendingByteCount() == 6, L"merge: rewriting staged bytes does not grow the count");

    // 包住多个旧块：三个单字节块被一次 10 字节的编辑吞掉。
    MemoryDiffOverlay envelope = MakeOverlay();
    envelope.Stage(0x1002, {1});
    envelope.Stage(0x1006, {2});
    envelope.Stage(0x100A, {3});
    suite.expect(envelope.DiffBlocks().size() == 3, L"merge: three separate blocks are staged first");
    envelope.Stage(0x1001, {4, 5, 6, 7, 8, 9, 0x0A, 0x0B, 0x0C, 0x0D});
    suite.expect(envelope.DiffBlocks() == Blocks{ { 0x1001, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA},
            {4, 5, 6, 7, 8, 9, 0x0A, 0x0B, 0x0C, 0x0D} } },
        L"merge: one edit covering several blocks absorbs them all");
    suite.expect(envelope.PendingByteCount() == 10, L"merge: the absorbed blocks are counted once");
}

// ------------------------------------------------------------
// 六、最早的 before、空操作消失、拆分。
// ------------------------------------------------------------
void TestEarliestBeforeAndNoOp(KswordTests::Suite& suite) {
    // 连续三次改同一个字节：before 始终是基线原值 0x44。
    MemoryDiffOverlay overlay = MakeOverlay();
    overlay.Stage(0x1004, {0x01});
    overlay.Stage(0x1004, {0x02});
    overlay.Stage(0x1004, {0x03});
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1004, {0x44}, {0x03} } },
        L"earliest: repeated edits keep the first original byte and the latest value");
    // 改回基线原值：补丁消失，不留痕。
    suite.expect(overlay.Stage(0x1004, {0x44}) == StageStatus::Ok && !overlay.HasPendingPatches()
        && overlay.PendingByteCount() == 0,
        L"no-op: writing the baseline value back removes the patch");

    // 对没有补丁的位置写基线原值：成功但什么都不留；紧邻已有补丁时也不会扩展它。
    MemoryDiffOverlay fresh = MakeOverlay();
    suite.expect(fresh.Stage(0x1006, {0x66}) == StageStatus::Ok && !fresh.HasPendingPatches(),
        L"no-op: an edit equal to the baseline leaves no patch");
    fresh.Stage(0x1000, {0x01});
    fresh.Stage(0x1001, {0x11});
    suite.expect(fresh.DiffBlocks() == Blocks{ { 0x1000, {0x00}, {0x01} } },
        L"no-op: an unchanged byte next to a patch does not extend it");

    // 单次编辑内部的空操作字节把它拆成两块：中间的 0x11 恰等于基线。
    MemoryDiffOverlay split = MakeOverlay();
    split.Stage(0x1000, {1, 0x11, 3});
    suite.expect(split.DiffBlocks() == Blocks{ { 0x1000, {0x00}, {1} }, { 0x1002, {0x22}, {3} } },
        L"split: an unchanged byte in the middle of one edit splits it into two blocks");

    // 在已有块中间写基线原值：一块拆两块；再从两端把它们削干净。
    MemoryDiffOverlay carve = MakeOverlay();
    carve.Stage(0x1000, {1, 2, 3, 4});
    carve.Stage(0x1001, {0x11});
    suite.expect(carve.DiffBlocks() == Blocks{ { 0x1000, {0x00}, {1} }, { 0x1002, {0x22, 0x33}, {3, 4} } },
        L"split: reverting the second byte of a block splits it in two");
    suite.expect(carve.PendingByteCount() == 3, L"split: the reverted byte leaves the pending count");
    carve.Stage(0x1000, {0x00});
    suite.expect(carve.DiffBlocks() == Blocks{ { 0x1002, {0x22, 0x33}, {3, 4} } },
        L"split: reverting the first byte of a block removes just that byte");
    carve.Stage(0x1002, {0x22, 0x33});
    suite.expect(carve.DiffBlocks().empty() && carve.PendingByteCount() == 0,
        L"split: reverting every remaining byte removes the whole patch");

    // 从右端削一个字节：块缩短，不拆。
    MemoryDiffOverlay trim = MakeOverlay();
    trim.Stage(0x1000, {1, 2, 3, 4});
    trim.Stage(0x1003, {0x33});
    suite.expect(trim.DiffBlocks() == Blocks{ { 0x1000, {0x00, 0x11, 0x22}, {1, 2, 3} } },
        L"split: reverting the last byte of a block shortens it");
    trim.Stage(0x1000, {0x00, 0x11});
    suite.expect(trim.DiffBlocks() == Blocks{ { 0x1002, {0x22}, {3} } },
        L"split: reverting a prefix leaves the rest of the block alone");

    // "最早"指暂存时刻的原字节，而不是重读之后的基线：外部把 0x44 改成 0x45 之后，
    // 再暂存同一处，before 仍是用户当初看到的 0x44，写前复核才能发现目标被动过。
    MemoryDiffOverlay moved = MakeOverlay();
    moved.Stage(0x1004, {0x01});
    Bytes reread = StandardBaseline();
    reread[4] = 0x45;
    moved.RefreshBaseline("target-A", kBase, reread, Bytes(16, 1));
    suite.expect(moved.DiffBlocks() == Blocks{ { 0x1004, {0x44}, {0x01} } },
        L"earliest: a refresh does not rewrite the recorded original byte");
    moved.Stage(0x1004, {0x02});
    suite.expect(moved.DiffBlocks() == Blocks{ { 0x1004, {0x44}, {0x02} } },
        L"earliest: restaging after the target changed keeps the original byte the user saw");
    // 写成"重读后的基线值 0x45"才是空操作，补丁随之消失。
    moved.Stage(0x1004, {0x45});
    suite.expect(!moved.HasPendingPatches(), L"no-op: equality is judged against the current baseline");
}

// ------------------------------------------------------------
// 七、丢弃：整块、部分（拆分）、跨块、窗口外、巨大范围。
// ------------------------------------------------------------
void TestDiscard(KswordTests::Suite& suite) {
    // 两个块：A = 0x1000..0x1003 {1,2,3,4}，B = 0x1008..0x1009 {5,6}。
    const auto twoBlocks = []() {
        MemoryDiffOverlay overlay = MakeOverlay();
        overlay.Stage(0x1000, {1, 2, 3, 4});
        overlay.Stage(0x1008, {5, 6});
        return overlay;
    };

    // 丢弃块中间两个字节：A 拆成两块，B 不受影响。
    MemoryDiffOverlay middle = twoBlocks();
    suite.expect(middle.Discard(0x1001, 2) == 2, L"discard: discarding two inner bytes reports two");
    suite.expect(middle.DiffBlocks() == Blocks{ { 0x1000, {0x00}, {1} }, { 0x1003, {0x33}, {4} }, { 0x1008, {0x88, 0x99}, {5, 6} } },
        L"discard: the middle of a block can be cut out, leaving two blocks");
    suite.expect(middle.PendingByteCount() == 4, L"discard: the pending count drops by the discarded bytes");

    // 丢弃块的首字节与末字节。
    MemoryDiffOverlay head = twoBlocks();
    suite.expect(head.Discard(0x1000, 1) == 1, L"discard: discarding the first byte reports one");
    suite.expect(head.DiffBlocks() == Blocks{ { 0x1001, {0x11, 0x22, 0x33}, {2, 3, 4} }, { 0x1008, {0x88, 0x99}, {5, 6} } },
        L"discard: the head of a block can be trimmed");
    MemoryDiffOverlay tail = twoBlocks();
    suite.expect(tail.Discard(0x1009, 100) == 1, L"discard: a range running past the end counts only the staged byte");
    suite.expect(tail.DiffBlocks() == Blocks{ { 0x1000, {0x00, 0x11, 0x22, 0x33}, {1, 2, 3, 4} }, { 0x1008, {0x88}, {5} } },
        L"discard: the tail of a block can be trimmed");

    // 跨越间隙：削掉 A 的后两个字节，并整块吞掉 B。
    MemoryDiffOverlay across = twoBlocks();
    suite.expect(across.Discard(0x1002, 8) == 4, L"discard: a range spanning a gap counts every staged byte inside it");
    suite.expect(across.DiffBlocks() == Blocks{ { 0x1000, {0x00, 0x11}, {1, 2} } },
        L"discard: a spanning range trims one block and removes the other");

    // 精确丢弃整块；范围仅与块相邻（不重叠）则什么都不做；长度 0 什么都不做。
    MemoryDiffOverlay exact = twoBlocks();
    suite.expect(exact.Discard(0x1008, 2) == 2 && exact.DiffBlocks().size() == 1, L"discard: an exact block is removed whole");
    MemoryDiffOverlay adjacent = twoBlocks();
    suite.expect(adjacent.Discard(0x1004, 4) == 0 && adjacent.PendingByteCount() == 6,
        L"discard: a range that only touches a block discards nothing");
    suite.expect(adjacent.Discard(0x1000, 0) == 0 && adjacent.PendingByteCount() == 6, L"discard: a zero length discards nothing");

    // 整个地址空间一次丢弃必须立即返回（逐字节扫描会跑到天荒地老）。
    MemoryDiffOverlay everything = twoBlocks();
    suite.expect(everything.Discard(0, kTop) == 6 && !everything.HasPendingPatches(),
        L"discard: a range covering the whole address space returns promptly with every byte");

    // DiscardAll 不碰基线。
    MemoryDiffOverlay all = twoBlocks();
    all.DiscardAll();
    suite.expect(!all.HasPendingPatches() && all.PendingByteCount() == 0 && all.DiffBlocks().empty(),
        L"discard: DiscardAll clears every patch");
    suite.expect(all.BaselineByte(0x1001) == Val(0x11), L"discard: discarding never touches the baseline");

    // 丢弃不要求范围在窗口内：窗口挪走之后窗口外的补丁仍可丢弃。
    MemoryDiffOverlay moved = MakeOverlay();
    moved.Stage(0x1002, {0x01});
    moved.RefreshBaseline("target-A", 0x1008, Bytes(16, 0x7E), Bytes(16, 1));
    suite.expect(moved.Discard(0x1002, 1) == 1 && !moved.HasPendingPatches(),
        L"discard: a patch outside the current window can still be discarded");
}

} // namespace

int RunMemwbOverlayTests() {
    KswordTests::Suite suite(L"MEMWB overlay");
    TestLoadBaselineRules(suite);
    TestStageBasics(suite);
    TestStageRejections(suite);
    TestAddressOverflowBoundaries(suite);
    TestMergeBehaviour(suite);
    TestEarliestBeforeAndNoOp(suite);
    TestDiscard(suite);
    MemwbOverlayTests::RunOverlayBehaviorGroups(suite);
    suite.report();
    return suite.failures();
}
