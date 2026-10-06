// 暂存编辑叠加层（shared/evidence/memory_workbench/MemoryDiffOverlay.h）离线测试的第二部分：
// 重读语义、上次读取的保留条件、变化种类优先级、写入确认、视图与暂存上限。
// 第一部分与入口在 MemoryDiffOverlayTests.cpp，共用夹具在 MemoryDiffOverlayTestSupport.h。
// 断言原则见第一部分文件头：期望值手算写死、边界两侧都测、拒绝路径必测且拒绝后状态不变。

#include "MemoryDiffOverlayTestSupport.h"

namespace MemwbOverlayTests {

namespace {

// ------------------------------------------------------------
// 八、LoadBaseline 与 RefreshBaseline 对补丁的不同处理。
// ------------------------------------------------------------
void TestLoadVersusRefreshPatches(KswordTests::Suite& suite) {
    // 同一目标重读：补丁保留，视图 = 新基线叠加旧补丁。
    MemoryDiffOverlay overlay = MakeOverlay();
    overlay.Stage(0x1002, {0x01});
    Bytes reread = StandardBaseline();
    reread[5] = 0x56;
    suite.expect(overlay.RefreshBaseline("target-A", kBase, reread, Bytes(16, 1)) == BaselineLoadStatus::Ok,
        L"refresh: a same-target refresh is accepted");
    suite.expect(overlay.DiffBlocks() == Blocks{ { 0x1002, {0x22}, {0x01} } } && overlay.PendingByteCount() == 1,
        L"refresh: staged patches survive a re-read");
    suite.expect(overlay.EffectiveByte(0x1002) == Val(0x01) && overlay.EffectiveByte(0x1005) == Val(0x56),
        L"refresh: the view shows the old patch over the new baseline");

    // 新目标 / 新快照：同样的参数换成 LoadBaseline，补丁被清空。
    suite.expect(overlay.LoadBaseline("target-A", kBase, reread, Bytes(16, 1)) == BaselineLoadStatus::Ok,
        L"load: a load with identical arguments is accepted");
    suite.expect(!overlay.HasPendingPatches() && overlay.PendingByteCount() == 0 && overlay.EffectiveByte(0x1002) == Val(0x22),
        L"load: a load clears every staged patch");

    // 翻页：窗口挪到别处，补丁按绝对地址保留，窗口外时仍显示，且此时不能再往窗口外暂存。
    MemoryDiffOverlay paged = MakeOverlay();
    paged.Stage(0x1002, {0x01});
    paged.RefreshBaseline("target-A", 0x1008, Bytes(16, 0x7E), Bytes(16, 1));
    suite.expect(paged.DiffBlocks() == Blocks{ { 0x1002, {0x22}, {0x01} } },
        L"refresh: a patch outside the new window is kept");
    const auto outside = paged.Materialize(0x1002, 1);
    suite.expect(outside.ok && outside.bytes == Bytes{0x01} && outside.validMask == Bytes{1},
        L"refresh: a patch outside the window still shows in the view and counts as valid");
    suite.expect(paged.EffectiveByte(0x1002) == Val(0x01) && paged.BaselineByte(0x1002) == std::nullopt,
        L"refresh: the effective byte comes from the patch while the baseline has nothing there");
    suite.expect(paged.ChangeKind(0x1002) == ByteChangeKind::Pending, L"refresh: an off-window patch is still pending");
    suite.expect(paged.Stage(0x1002, {0x03}) == StageStatus::OutOfWindow,
        L"refresh: staging into the old, now-unread range is rejected");
    suite.expect(paged.EffectiveByte(0x1008) == Val(0x7E), L"refresh: the new window is readable");

    // 翻回去与窗口变长 / 变短：补丁都还在。
    paged.RefreshBaseline("target-A", kBase, StandardBaseline(), Bytes(16, 1));
    suite.expect(paged.EffectiveByte(0x1002) == Val(0x01) && paged.BaselineByte(0x1002) == Val(0x22),
        L"refresh: paging back shows the patch over its own baseline again");
    paged.RefreshBaseline("target-A", kBase, Bytes(8, 0x7E), Bytes(8, 1));
    suite.expect(paged.PendingByteCount() == 1 && paged.EffectiveByte(0x1002) == Val(0x01),
        L"refresh: shrinking the window keeps the patch");

    // 安全例外：身份串变了就不是同一目标，补丁必须清空，否则会被写进另一个目标。
    MemoryDiffOverlay other = MakeOverlay();
    other.Stage(0x1002, {0x01});
    suite.expect(other.RefreshBaseline("target-B", kBase, StandardBaseline(), Bytes(16, 1)) == BaselineLoadStatus::Ok,
        L"refresh: a refresh under a new identity is accepted");
    suite.expect(!other.HasPendingPatches() && other.IdentityKey() == "target-B",
        L"refresh: a different identity drops the patches instead of carrying them to another target");
}

// ------------------------------------------------------------
// 九、"上次读取"的保留条件：身份、基址、长度三者全等。
// ------------------------------------------------------------
void TestPreviousReadRetention(KswordTests::Suite& suite) {
    // 第一次读取：窗口 0x2000，4 字节 10 20 30 40，没有上次读取。
    const auto firstRead = []() {
        MemoryDiffOverlay overlay;
        overlay.LoadBaseline("K", 0x2000, {0x10, 0x20, 0x30, 0x40}, {1, 1, 1, 1});
        return overlay;
    };
    MemoryDiffOverlay first = firstRead();
    suite.expect(!first.HasPreviousRead() && first.ChangeKind(0x2001) == ByteChangeKind::Unchanged,
        L"previous: a first read has no previous read and reports unchanged");

    // 三者全等：上一份基线成为上次读取，0x2001 由 0x20 变 0x21 显示为外部变化。
    MemoryDiffOverlay same = firstRead();
    same.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(same.HasPreviousRead() && same.PreviousByte(0x2001) == Val(0x20) && same.BaselineByte(0x2001) == Val(0x21),
        L"previous: an identical target keeps the old baseline as the previous read");
    suite.expect(same.ChangeKind(0x2001) == ByteChangeKind::ExternalChange && same.ChangeKind(0x2000) == ByteChangeKind::Unchanged
        && same.ChangeKind(0x2002) == ByteChangeKind::Unchanged && same.ChangeKind(0x2003) == ByteChangeKind::Unchanged,
        L"previous: only the byte that differs from the previous read is an external change");

    // 第三次读取内容不变：上次读取被顶替成第二次，外部变化标记随之消失。
    same.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(same.HasPreviousRead() && same.ChangeKind(0x2001) == ByteChangeKind::Unchanged,
        L"previous: the previous read always tracks the read before the latest one");

    // 身份、基址、长度任一不同：上次读取作废，不得产生外部变化。
    MemoryDiffOverlay identity = firstRead();
    identity.RefreshBaseline("K2", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(!identity.HasPreviousRead() && identity.ChangeKind(0x2001) == ByteChangeKind::Unchanged,
        L"previous: a different identity discards the previous read");
    MemoryDiffOverlay rebased = firstRead();
    rebased.RefreshBaseline("K", 0x2001, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(!rebased.HasPreviousRead() && rebased.ChangeKind(0x2002) == ByteChangeKind::Unchanged,
        L"previous: a different base address discards the previous read");
    MemoryDiffOverlay resized = firstRead();
    resized.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40, 0x50}, {1, 1, 1, 1, 1});
    suite.expect(!resized.HasPreviousRead() && resized.ChangeKind(0x2001) == ByteChangeKind::Unchanged,
        L"previous: a different length discards the previous read");

    // LoadBaseline 的条件相同：全等则保留（只清补丁），不全等则作废。
    MemoryDiffOverlay loadedSame = firstRead();
    loadedSame.LoadBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(loadedSame.HasPreviousRead() && loadedSame.ChangeKind(0x2001) == ByteChangeKind::ExternalChange,
        L"previous: LoadBaseline applies the same retention rule when the target is identical");
    MemoryDiffOverlay loadedOther = firstRead();
    loadedOther.LoadBaseline("K", 0x2004, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(!loadedOther.HasPreviousRead(), L"previous: LoadBaseline discards the previous read when the base differs");

    // 被拒绝的重读不得顶替上次读取。
    MemoryDiffOverlay refused = firstRead();
    refused.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    refused.RefreshBaseline("K", 0x2000, {0x99, 0x99, 0x99, 0x99}, {1, 1});
    suite.expect(refused.PreviousByte(0x2001) == Val(0x20) && refused.BaselineByte(0x2001) == Val(0x21),
        L"previous: a rejected re-read changes neither the baseline nor the previous read");

    // 上次没读到的字节没有比对依据，不能判成外部变化；这次没读到则是不可读。
    MemoryDiffOverlay unreadBefore;
    unreadBefore.LoadBaseline("K", 0x2000, {0x10, 0x00, 0x30, 0x40}, {1, 0, 1, 1});
    unreadBefore.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(unreadBefore.ChangeKind(0x2001) == ByteChangeKind::Unchanged && unreadBefore.PreviousByte(0x2001) == std::nullopt,
        L"previous: a byte that was unread last time cannot be called an external change");
    MemoryDiffOverlay unreadNow = firstRead();
    unreadNow.RefreshBaseline("K", 0x2000, {0x10, 0x00, 0x30, 0x40}, {1, 0, 1, 1});
    suite.expect(unreadNow.ChangeKind(0x2001) == ByteChangeKind::Unreadable,
        L"previous: a byte unread this time is unreadable, not changed");
}

// ------------------------------------------------------------
// 十、ChangeKind 五种取值与优先级 Pending > SelfWritten > ExternalChange > Unchanged。
// ------------------------------------------------------------
void TestChangeKindPriority(KswordTests::Suite& suite) {
    // 窗口 0x2000，6 字节；重读后 0x2001~0x2003 被外部改了，0x2005 这次没读到。
    MemoryDiffOverlay overlay;
    overlay.LoadBaseline("K", 0x2000, {0x10, 0x20, 0x30, 0x40, 0x50, 0x60}, {1, 1, 1, 1, 1, 1});
    overlay.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x31, 0x41, 0x50, 0x00}, {1, 1, 1, 1, 1, 0});
    suite.expect(overlay.ChangeKind(0x2000) == ByteChangeKind::Unchanged && overlay.ChangeKind(0x2004) == ByteChangeKind::Unchanged,
        L"kind: bytes equal to the previous read are unchanged");
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::ExternalChange && overlay.ChangeKind(0x2003) == ByteChangeKind::ExternalChange,
        L"kind: bytes that differ from the previous read are external changes");
    suite.expect(overlay.ChangeKind(0x2005) == ByteChangeKind::Unreadable && overlay.ChangeKind(0x1FFF) == ByteChangeKind::Unreadable
        && overlay.ChangeKind(0x2006) == ByteChangeKind::Unreadable,
        L"kind: an unread byte and both addresses just outside the window are unreadable");
    suite.expect(MemoryDiffOverlay().ChangeKind(0x2000) == ByteChangeKind::Unreadable,
        L"kind: with no baseline everything is unreadable");

    // Pending 压过 ExternalChange。
    overlay.Stage(0x2001, {0x99});
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::Pending && overlay.ChangeKind(0x2003) == ByteChangeKind::ExternalChange,
        L"kind: a staged byte is pending even where the baseline changed externally");

    // SelfWritten 压过 ExternalChange：0x2001 写成 0x99、0x2002 写成 0x77 后，基线与上次读取不同，但那是我们写的。
    overlay.Stage(0x2002, {0x77});
    const Blocks written = overlay.DiffBlocks();
    suite.expect(written == Blocks{ { 0x2001, {0x21, 0x31}, {0x99, 0x77} } }, L"kind: the two staged bytes form one block");
    suite.expect(overlay.AcceptWrite(written.front(), {0x99, 0x77}) == AcceptWriteStatus::Ok, L"kind: the write is confirmed");
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::SelfWritten && overlay.ChangeKind(0x2002) == ByteChangeKind::SelfWritten,
        L"kind: confirmed writes show as self-written, not as an external change");
    suite.expect(overlay.ChangeKind(0x2003) == ByteChangeKind::ExternalChange, L"kind: a neighbouring external change is unaffected");

    // Pending 压过 SelfWritten；丢弃之后回到 SelfWritten。
    overlay.Stage(0x2001, {0x55});
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::Pending && overlay.ChangeKind(0x2002) == ByteChangeKind::SelfWritten,
        L"kind: a new edit on a self-written byte is pending");
    overlay.Discard(0x2001, 1);
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::SelfWritten, L"kind: discarding the edit restores the self-written mark");

    // 补丁值恰等于新基线（外部把它改成了我们想写的值）：不再是 Pending，按外部变化显示。
    MemoryDiffOverlay equal;
    equal.LoadBaseline("K", 0x2000, {0x10, 0x20, 0x30, 0x40}, {1, 1, 1, 1});
    equal.Stage(0x2001, {0x01});
    suite.expect(equal.ChangeKind(0x2001) == ByteChangeKind::Pending, L"kind: a staged byte differing from the baseline is pending");
    equal.RefreshBaseline("K", 0x2000, {0x10, 0x01, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(equal.ChangeKind(0x2001) == ByteChangeKind::ExternalChange && equal.HasPendingPatches(),
        L"kind: a patch equal to the new baseline is no longer pending but is kept");

    // 补丁压在重读后没读到的字节上：基线未知视为不同，仍是 Pending，且视图里有效。
    MemoryDiffOverlay lost;
    lost.LoadBaseline("K", 0x2000, {0x10, 0x20, 0x30, 0x40}, {1, 1, 1, 1});
    lost.Stage(0x2002, {0x05});
    lost.RefreshBaseline("K", 0x2000, {0x10, 0x20, 0x00, 0x40}, {1, 1, 0, 1});
    suite.expect(lost.ChangeKind(0x2002) == ByteChangeKind::Pending && lost.EffectiveByte(0x2002) == Val(0x05),
        L"kind: a patch over a byte that became unreadable is pending and still has a value");
    const auto over = lost.Materialize(0x2001, 3);
    suite.expect(over.bytes == Bytes{0x20, 0x05, 0x40} && over.validMask == Bytes{1, 1, 1},
        L"kind: the patch makes the unreadable byte valid in the view");
}

// ------------------------------------------------------------
// 十一、AcceptWrite：更新基线、移除补丁、标记自己写入、拒绝不改状态。
// ------------------------------------------------------------
void TestAcceptWrite(KswordTests::Suite& suite) {
    // 窗口 0x2000，重读后 0x2001 由 0x20 变 0x21（有上次读取）。暂存 0x77 并确认写入。
    MemoryDiffOverlay overlay;
    overlay.LoadBaseline("K", 0x2000, {0x10, 0x20, 0x30, 0x40}, {1, 1, 1, 1});
    overlay.RefreshBaseline("K", 0x2000, {0x10, 0x21, 0x30, 0x40}, {1, 1, 1, 1});
    overlay.Stage(0x2001, {0x77});
    const Blocks block = overlay.DiffBlocks();
    suite.expect(overlay.AcceptWrite(block.front(), {0x77}) == AcceptWriteStatus::Ok, L"accept: a matching read-back is accepted");
    suite.expect(!overlay.HasPendingPatches() && overlay.PendingByteCount() == 0, L"accept: the confirmed patch is removed");
    suite.expect(overlay.BaselineByte(0x2001) == Val(0x77), L"accept: the baseline takes the read-back byte");
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::SelfWritten && overlay.ChangeKind(0x2000) == ByteChangeKind::Unchanged,
        L"accept: our own write is not displayed as an external change");

    // 下一次读取：目标就是我们写的值 -> 不再有任何标记；之后再变才是真正的外部变化。
    overlay.RefreshBaseline("K", 0x2000, {0x10, 0x77, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::Unchanged && overlay.PreviousByte(0x2001) == Val(0x77),
        L"accept: the next read clears the self-written mark and compares against what we wrote");
    overlay.RefreshBaseline("K", 0x2000, {0x10, 0x78, 0x30, 0x40}, {1, 1, 1, 1});
    suite.expect(overlay.ChangeKind(0x2001) == ByteChangeKind::ExternalChange,
        L"accept: a later change after our write is a genuine external change");

    // 没有上次读取时同样显示 SelfWritten；LoadBaseline 之后清除。
    MemoryDiffOverlay lone = MakeOverlay();
    lone.Stage(0x1002, {0x01});
    suite.expect(lone.AcceptWrite(lone.DiffBlocks().front(), {0x01}) == AcceptWriteStatus::Ok
        && lone.ChangeKind(0x1002) == ByteChangeKind::SelfWritten && !lone.HasPreviousRead(),
        L"accept: a self-written byte is shown even when there is no previous read");
    lone.LoadBaseline("target-B", kBase, StandardBaseline(), Bytes(16, 1));
    suite.expect(lone.ChangeKind(0x1002) == ByteChangeKind::Unchanged, L"accept: a fresh load clears the self-written marks");

    // 回读与暂存值不同（硬件吞了写入）：基线取回读值，补丁照样移除。
    MemoryDiffOverlay differs = MakeOverlay();
    differs.Stage(0x1002, {0x01});
    differs.AcceptWrite(differs.DiffBlocks().front(), {0x02});
    suite.expect(differs.BaselineByte(0x1002) == Val(0x02) && !differs.HasPendingPatches(),
        L"accept: the baseline reflects what was read back, not what was staged");

    // 回读长度不符：拒绝且所有状态原封不动。
    MemoryDiffOverlay refuse = MakeOverlay();
    refuse.Stage(0x1002, {0x01, 0x02});
    const Blocks staged = refuse.DiffBlocks();
    suite.expect(refuse.AcceptWrite(staged.front(), {0x01, 0x02, 0x03}) == AcceptWriteStatus::ReadBackLengthMismatch,
        L"accept: a read-back one byte too long is rejected");
    suite.expect(refuse.AcceptWrite(staged.front(), {0x01}) == AcceptWriteStatus::ReadBackLengthMismatch,
        L"accept: a read-back one byte too short is rejected");
    suite.expect(refuse.AcceptWrite(staged.front(), {}) == AcceptWriteStatus::ReadBackLengthMismatch,
        L"accept: an empty read-back is rejected");
    suite.expect(refuse.DiffBlocks() == staged && refuse.PendingByteCount() == 2,
        L"accept: a rejected accept keeps the patch");
    suite.expect(refuse.BaselineByte(0x1002) == Val(0x22) && refuse.BaselineByte(0x1003) == Val(0x33),
        L"accept: a rejected accept leaves the baseline alone");
    suite.expect(refuse.ChangeKind(0x1002) == ByteChangeKind::Pending, L"accept: a rejected accept marks nothing as self-written");
    suite.expect(MemoryDiffOverlay().AcceptWrite(DiffBlock{}, {}) == AcceptWriteStatus::Empty,
        L"accept: a block with no bytes is rejected");

    // 只确认一个合并块的中间一段：补丁拆成两块，其余仍待写。
    MemoryDiffOverlay part = MakeOverlay();
    part.Stage(0x1000, {1, 2, 3, 4, 5});
    suite.expect(part.AcceptWrite(DiffBlock{ 0x1001, {0x11, 0x22}, {2, 3} }, {2, 3}) == AcceptWriteStatus::Ok,
        L"accept: a sub-range of a staged block can be confirmed");
    suite.expect(part.DiffBlocks() == Blocks{ { 0x1000, {0x00}, {1} }, { 0x1003, {0x33, 0x44}, {4, 5} } } && part.PendingByteCount() == 3,
        L"accept: confirming the middle leaves the two ends staged");
    suite.expect(part.BaselineByte(0x1001) == Val(2) && part.BaselineByte(0x1002) == Val(3),
        L"accept: only the confirmed range is written into the baseline");

    // 写入期间用户又改了同一处：新的暂存保留，其 before 变成目标上现在的值（回读值）。
    MemoryDiffOverlay newer = MakeOverlay();
    newer.Stage(0x1000, {1, 2, 3, 4});
    const DiffBlock inFlight = newer.DiffBlocks().front();
    newer.Stage(0x1001, {9});
    suite.expect(newer.AcceptWrite(inFlight, {1, 2, 3, 4}) == AcceptWriteStatus::Ok, L"accept: the in-flight block is confirmed");
    suite.expect(newer.DiffBlocks() == Blocks{ { 0x1001, {2}, {9} } },
        L"accept: an edit made during the write survives with the written byte as its original");
    suite.expect(newer.ChangeKind(0x1001) == ByteChangeKind::Pending && newer.ChangeKind(0x1000) == ByteChangeKind::SelfWritten,
        L"accept: the surviving edit is pending while the written bytes are self-written");
    MemoryDiffOverlay same = MakeOverlay();
    same.Stage(0x1000, {1, 2, 3, 4});
    const DiffBlock sent = same.DiffBlocks().front();
    same.Stage(0x1001, {0x0B});
    same.AcceptWrite(sent, {1, 0x0B, 3, 4});
    suite.expect(!same.HasPendingPatches(), L"accept: an edit made during the write that equals the read-back is a no-op");

    // 窗口已经挪走：补丁照样移除，窗口里的基线不被误改。
    MemoryDiffOverlay moved = MakeOverlay();
    moved.Stage(0x1002, {1});
    moved.RefreshBaseline("target-A", 0x1008, Bytes(16, 0x7E), Bytes(16, 1));
    suite.expect(moved.AcceptWrite(DiffBlock{ 0x1002, {0x22}, {1} }, {1}) == AcceptWriteStatus::Ok && !moved.HasPendingPatches(),
        L"accept: a write outside the current window still removes its patch");
    suite.expect(moved.BaselineByte(0x1002) == std::nullopt && moved.BaselineByte(0x1008) == Val(0x7E)
        && moved.ChangeKind(0x1008) == ByteChangeKind::Unchanged,
        L"accept: nothing in the current window is touched by an off-window write");

    // 回读证明这些字节读得到：原先没读到的字节被确认写入后变成有效。
    MemoryDiffOverlay gap;
    gap.LoadBaseline("K", 0x2000, {0x10, 0x00, 0x30}, {1, 0, 1});
    suite.expect(gap.BaselineByte(0x2001) == std::nullopt, L"accept: the byte starts out unread");
    gap.AcceptWrite(DiffBlock{ 0x2001, {}, {0x05} }, {0x05});
    suite.expect(gap.BaselineByte(0x2001) == Val(0x05) && gap.ChangeKind(0x2001) == ByteChangeKind::SelfWritten,
        L"accept: a confirmed write makes a previously unread byte valid");

    // 只与窗口部分重叠：只有重叠的两个字节进基线并标记（0x1006、0x1007 在窗口之外）。
    moved.AcceptWrite(DiffBlock{ 0x1006, {}, {1, 2, 3, 4} }, {0x0A, 0x0B, 0x0C, 0x0D});
    suite.expect(moved.BaselineByte(0x1008) == Val(0x0C) && moved.BaselineByte(0x1009) == Val(0x0D) && moved.BaselineByte(0x1007) == std::nullopt,
        L"accept: a partially overlapping write updates only the part inside the window");
    suite.expect(moved.ChangeKind(0x1008) == ByteChangeKind::SelfWritten && moved.ChangeKind(0x1009) == ByteChangeKind::SelfWritten
        && moved.ChangeKind(0x100A) == ByteChangeKind::Unchanged,
        L"accept: only the overlapping bytes are marked self-written");
}

// ------------------------------------------------------------
// 十二、Materialize 与 EffectiveByte 的区间处理。
// ------------------------------------------------------------
void TestMaterialize(KswordTests::Suite& suite) {
    // 起点在窗口之前：前两个字节无效，0x1000 处真实的 0x00 有效——掩码把"真的是 0"与"没有"分开。
    MemoryDiffOverlay overlay = MakeOverlay();
    const auto head = overlay.Materialize(0x0FFE, 6);
    suite.expect(head.ok && head.bytes == Bytes{0x00, 0x00, 0x00, 0x11, 0x22, 0x33} && head.validMask == Bytes{0, 0, 1, 1, 1, 1},
        L"materialize: a range starting below the window marks those bytes invalid");
    const auto tail = overlay.Materialize(0x100E, 4);
    suite.expect(tail.ok && tail.bytes == Bytes{0xEE, 0xFF, 0x00, 0x00} && tail.validMask == Bytes{1, 1, 0, 0},
        L"materialize: a range running past the window end marks the excess invalid");

    // 零长度合法，返回空视图；没有基线时全部无效；超过防御上限拒绝。
    const auto zero = overlay.Materialize(0x1000, 0);
    suite.expect(zero.ok && zero.bytes.empty() && zero.validMask.empty(), L"materialize: a zero length is an empty valid view");
    const auto nothing = MemoryDiffOverlay().Materialize(0x1000, 4);
    suite.expect(nothing.ok && nothing.validMask == Bytes{0, 0, 0, 0}, L"materialize: without a baseline every byte is invalid");
    const std::uint64_t cap = ksword::memwb::kMemoryDiffOverlayMaxMaterializeBytes;
    suite.expect(cap == 64ULL * 1024ULL * 1024ULL, L"materialize: the defensive cap is 64 MiB");
    const auto huge = overlay.Materialize(0, cap + 1);
    suite.expect(!huge.ok && huge.bytes.empty() && huge.validMask.empty(),
        L"materialize: a length one above the defensive cap is rejected without allocating");
    const auto atCap = MemoryDiffOverlay().Materialize(0, cap);
    suite.expect(atCap.ok && atCap.bytes.size() == cap && atCap.validMask.size() == cap,
        L"materialize: a length exactly at the defensive cap is still served");

    // 窗口 [0x1000, 0x1010) 的两端：首尾字节在窗口内，紧邻的两个地址在窗口外。
    suite.expect(overlay.BaselineByte(0x1000) == Val(0x00) && overlay.BaselineByte(0x100F) == Val(0xFF),
        L"materialize: the first and last byte of the window have baseline values");
    suite.expect(overlay.BaselineByte(0x0FFF) == std::nullopt && overlay.BaselineByte(0x1010) == std::nullopt
        && overlay.EffectiveByte(0x1010) == std::nullopt,
        L"materialize: the addresses just outside the window have no value");
    suite.expect(overlay.ChangeKind(0x1010) == ByteChangeKind::Unreadable && overlay.ChangeKind(0x100F) == ByteChangeKind::Unchanged,
        L"materialize: the address one past the window is unreadable and the last byte is not");

    // 补丁部分压在请求起点之前、完全包含、跨多个块。
    overlay.Stage(0x1002, {1, 2, 3, 4});
    overlay.Stage(0x1008, {5});
    suite.expect(overlay.Materialize(0x1004, 4).bytes == Bytes{3, 4, 0x66, 0x77}, L"materialize: a patch starting before the range still shows");
    suite.expect(overlay.Materialize(0x1000, 4).bytes == Bytes{0x00, 0x11, 1, 2}, L"materialize: a patch cut by the range end shows its head");
    suite.expect(overlay.Materialize(0x1003, 2).bytes == Bytes{2, 3}, L"materialize: a range inside one patch shows only patch bytes");
    suite.expect(overlay.Materialize(0x1004, 6).bytes == Bytes{3, 4, 0x66, 0x77, 5, 0x99},
        L"materialize: a range crossing several patches stitches them over the baseline");
    suite.expect(overlay.Materialize(0x1004, 6).validMask == Bytes{1, 1, 1, 1, 1, 1}, L"materialize: stitched bytes are all valid");
}

// ------------------------------------------------------------
// 十三、暂存总量上限。
// ------------------------------------------------------------
void TestPendingLimit(KswordTests::Suite& suite) {
    // 常量就是 64 MiB，默认构造的对象用它。
    suite.expect(ksword::memwb::kMemoryDiffOverlayMaxPendingBytes == 67108864ULL, L"limit: the constant is 64 MiB");
    suite.expect(MemoryDiffOverlay().MaxPendingBytes() == 67108864ULL, L"limit: a default overlay uses the 64 MiB constant");

    // 用小上限（4）验证边界两侧。
    MemoryDiffOverlay small(4);
    small.LoadBaseline("k", kBase, StandardBaseline(), Bytes(16, 1));
    suite.expect(small.MaxPendingBytes() == 4, L"limit: the limit can be lowered for testing");
    suite.expect(small.Stage(0x1000, {1, 2, 3, 4, 5}) == StageStatus::TooLarge && !small.HasPendingPatches(),
        L"limit: a single edit one byte over the limit is rejected");
    suite.expect(small.Stage(0x1000, {1, 2, 3, 4}) == StageStatus::Ok && small.PendingByteCount() == 4,
        L"limit: an edit exactly at the limit is accepted");
    suite.expect(small.Stage(0x1008, {1}) == StageStatus::TooLarge, L"limit: one more byte elsewhere is rejected");
    suite.expect(small.Stage(0x1004, {1}) == StageStatus::TooLarge, L"limit: growing a block by one byte is rejected");
    suite.expect(small.DiffBlocks() == Blocks{ { 0x1000, {0x00, 0x11, 0x22, 0x33}, {1, 2, 3, 4} } } && small.PendingByteCount() == 4,
        L"limit: a rejected edit leaves the staged state untouched");

    // 在上限处改写已暂存的字节不增长，允许；空操作也不增长。
    suite.expect(small.Stage(0x1000, {5, 6, 7, 8}) == StageStatus::Ok && small.PendingByteCount() == 4,
        L"limit: rewriting staged bytes at the limit is allowed");
    suite.expect(small.Stage(0x1004, {0x44}) == StageStatus::Ok && small.PendingByteCount() == 4,
        L"limit: an edit equal to the baseline at the limit is allowed");
    // 一次编辑同时加一个、减一个：净增长 0，允许。0x1003 还原（-1），0x1004 新增（+1）。
    suite.expect(small.Stage(0x1003, {0x33, 0x01}) == StageStatus::Ok && small.PendingByteCount() == 4,
        L"limit: an edit that adds one byte and removes one is judged by the net count");
    suite.expect(small.DiffBlocks() == Blocks{ { 0x1000, {0x00, 0x11, 0x22}, {5, 6, 7} }, { 0x1004, {0x44}, {1} } },
        L"limit: the net-zero edit produced the expected blocks");

    // 丢弃释放额度。
    small.Discard(0x1000, 3);
    suite.expect(small.PendingByteCount() == 1 && small.Stage(0x1008, {1, 2, 3}) == StageStatus::Ok && small.PendingByteCount() == 4,
        L"limit: discarding frees room for new edits");

    // 没读到的字节先于容量被报告；上限为 0 时任何编辑都超限。
    MemoryDiffOverlay unread(4);
    unread.LoadBaseline("k", kBase, StandardBaseline(), Bytes{0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1});
    suite.expect(unread.Stage(0x1000, {1, 2, 3, 4, 5, 6}) == StageStatus::UnreadBytes,
        L"limit: unread bytes are reported before the size limit");
    MemoryDiffOverlay zero(0);
    zero.LoadBaseline("k", kBase, StandardBaseline(), Bytes(16, 1));
    suite.expect(zero.Stage(0x1000, {1}) == StageStatus::TooLarge, L"limit: a zero limit rejects every edit");

    // 真实的 64 MiB 上限：窗口 64 MiB + 1，一次暂存 64 MiB + 1 字节必须被拒绝。
    const std::size_t windowSize = 64U * 1024U * 1024U + 1U;
    MemoryDiffOverlay real;
    real.LoadBaseline("big", 0, Bytes(windowSize, 0x00), Bytes(windowSize, 1));
    suite.expect(real.Stage(0, Bytes(windowSize, 0x01)) == StageStatus::TooLarge && !real.HasPendingPatches(),
        L"limit: one byte over the real 64 MiB limit is rejected");
}

} // namespace

void RunOverlayBehaviorGroups(KswordTests::Suite& suite) {
    TestLoadVersusRefreshPatches(suite);
    TestPreviousReadRetention(suite);
    TestChangeKindPriority(suite);
    TestAcceptWrite(suite);
    TestMaterialize(suite);
    TestPendingLimit(suite);
}

} // namespace MemwbOverlayTests
