// 写事务（shared/evidence/memory_workbench/MemoryWriteTransaction.h）的离线测试。
//
// 为什么值得一整套穷举断言：写事务决定"用户确认过的"和"最终写进目标的"是不是同一件事。
// 它出错不会报错——写前复核漏了一块，就会把别人刚改过的字节悄悄覆盖掉；确认窗口里
// （旧代码是嵌套事件循环）用户切了目标而没有重新核对，字节就写进了另一个进程；
// 一次同意被循环外复用，下一块没被问就写了；失败时把还没写的暂存补丁清掉，
// 用户的输入就无声无息地丢了。这些路径都不会抛异常，只有按调用顺序逐条断言才抓得住。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值全部手算写死（地址、字节、调用日志都是字面量），绝不从被测函数反算；
//   * 边界两侧都测（NoChange 与有差异、会话有效与无效、同意给与不给）；
//   * 拒绝 / 失败路径必须显式测，且失败后 overlay 里的暂存补丁必须原样保留；
//   * 每条不变式至少一正一反：既测"该写时写了"，也测"不该写时零写入"。
//
// 本文件：入口 + 暂存与 Idle/Staged 零写入 + 写入模式 + 模式切换三选一。

#include "MemoryWriteTransactionTestSupport.h"

namespace {

using namespace MemwbTxnTests;

// ------------------------------------------------------------
// 一、不变式 1：Idle / Staged 状态下对 store 的写调用次数恒为 0。
// ------------------------------------------------------------
void TestIdleAndStagedNeverWrite(KswordTests::Suite& suite) {
    Rig rig;
    suite.expect(rig.txn.CurrentState() == State::Idle, L"txn: a new transaction starts Idle");
    suite.expect(rig.txn.Mode() == WriteMode::Immediate, L"txn: the default write mode is Immediate");
    suite.expect(!rig.txn.IsBusy(), L"txn: a new transaction is not busy");
    suite.expect(!rig.txn.UiConfirmSuppressed(), L"txn: ui confirmation is not suppressed by default");
    suite.expect(!rig.txn.PendingModeSwitch().has_value(), L"txn: a new transaction has no pending mode switch");
    suite.expect(rig.store.writeCalls == 0 && rig.store.readCalls == 0,
        L"txn: construction does not touch the store");
    suite.expect(rig.audit.records.empty() && rig.sink.uiCalls == 0,
        L"txn: construction writes no audit and asks nobody");

    // 暂存一块：状态进入 Staged，叠加层有补丁，store 一次都没碰。
    suite.expect(rig.txn.Stage(kA, Bytes{0x01}) == StageStatus::Ok, L"txn: staging one byte is accepted");
    suite.expect(rig.txn.CurrentState() == State::Staged, L"txn: a successful stage from Idle enters Staged");
    suite.expect(rig.overlay.HasPendingPatches() && rig.overlay.PendingByteCount() == 1,
        L"txn: the stage is forwarded to the overlay");
    const std::vector<DiffBlock> blocks = rig.overlay.DiffBlocks();
    suite.expect(blocks.size() == 1 && blocks[0].address == 0x1002ULL
        && blocks[0].before == Bytes{0x22} && blocks[0].after == Bytes{0x01},
        L"txn: the staged block records original 22 and new 01 at 0x1002");
    suite.expect(rig.store.writeCalls == 0 && rig.store.readCalls == 0,
        L"txn: staging never reaches the store, in either direction");
    suite.expect(rig.sink.uiCalls == 0 && rig.audit.records.empty(),
        L"txn: staging asks nobody and writes no audit");

    // 再暂存一块：仍是 Staged，仍然零写入，目标内存原封不动。
    suite.expect(rig.txn.Stage(kB, Bytes{0x02, 0x03}) == StageStatus::Ok, L"txn: a second stage is accepted");
    suite.expect(rig.txn.CurrentState() == State::Staged && rig.overlay.PendingByteCount() == 3,
        L"txn: two staged blocks hold 3 pending bytes in Staged");
    suite.expect(rig.store.writeCalls == 0, L"txn: still no store write after two stages");
    suite.expect(MemoryAt(rig, kA, 1) == Bytes{0x22} && MemoryAt(rig, kB, 2) == (Bytes{0x66, 0x77}),
        L"txn: the target memory is untouched by staging");
}

// ------------------------------------------------------------
// 二、Stage 的转发语义：被拒绝的不改状态；空操作不算 Staged。
// ------------------------------------------------------------
void TestStageForwardingAndStateRules(KswordTests::Suite& suite) {
    // 被拒绝的暂存：原因原样返回，状态仍是 Idle，没有补丁。
    Rig rig;
    suite.expect(rig.txn.Stage(0x2000ULL, Bytes{0x01}) == StageStatus::OutOfWindow,
        L"txn: staging outside the window is rejected with OutOfWindow");
    suite.expect(rig.txn.Stage(kA, Bytes{}) == StageStatus::Empty, L"txn: staging nothing is rejected as Empty");
    suite.expect(rig.txn.CurrentState() == State::Idle && !rig.overlay.HasPendingPatches(),
        L"txn: rejected stages leave the state Idle and the overlay empty");

    // 没读到的字节不能编辑。
    Rig unreadRig;
    suite.expect(unreadRig.overlay.LoadBaseline("target-A", kBase, StandardBaseline(),
        Bytes{1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}) == ksword::memwb::BaselineLoadStatus::Ok,
        L"txn: a baseline with one unread byte loads");
    suite.expect(unreadRig.txn.Stage(0x1004ULL, Bytes{0x01}) == StageStatus::UnreadBytes
        && unreadRig.txn.CurrentState() == State::Idle,
        L"txn: staging an unread byte is rejected and the state stays Idle");

    // 被拒绝的暂存不动已有的 Staged 与补丁。
    rig.StageA();
    const std::vector<DiffBlock> before = rig.overlay.DiffBlocks();
    suite.expect(rig.txn.Stage(0x2000ULL, Bytes{0x01}) == StageStatus::OutOfWindow
        && rig.txn.CurrentState() == State::Staged && rig.overlay.DiffBlocks() == before,
        L"txn: a rejected stage keeps Staged and the existing patches");

    // 空操作编辑：写成与基线相同的字节，补丁消失，Staged 回到 Idle。
    suite.expect(rig.txn.Stage(kA, Bytes{0x22}) == StageStatus::Ok, L"txn: staging the baseline value is accepted");
    suite.expect(!rig.overlay.HasPendingPatches() && rig.txn.CurrentState() == State::Idle,
        L"txn: staging back to the baseline value drops the patch and returns to Idle");

    // 从 Idle 起的空操作编辑不会假装有东西可提交。
    Rig noop;
    suite.expect(noop.txn.Stage(kA, Bytes{0x22}) == StageStatus::Ok && noop.txn.CurrentState() == State::Idle,
        L"txn: a no-op edit from Idle stays Idle");
}

// ------------------------------------------------------------
// 三、OnEditCompleted 与两种写入模式共用同一条管线。
// ------------------------------------------------------------
void TestImmediateAndStagedModesShareOnePipeline(KswordTests::Suite& suite) {
    // 立即写入：编辑完成即走完整提交。
    Rig immediate;
    immediate.StageA();
    suite.expect(immediate.store.writeCalls == 0, L"txn: Immediate mode still stages without writing");
    const auto fired = immediate.txn.OnEditCompleted();
    suite.expect(fired.has_value() && fired->outcome == CommitOutcome::Committed,
        L"txn: OnEditCompleted in Immediate mode commits automatically");
    suite.expect(immediate.store.writeCalls == 1 && MemoryAt(immediate, kA, 1) == Bytes{0x01},
        L"txn: the automatic commit wrote 01 at 0x1002 with exactly one store write");
    suite.expect(immediate.txn.CurrentState() == State::Committed && !immediate.overlay.HasPendingPatches(),
        L"txn: the automatic commit ends Committed with nothing pending");

    // 暂存后统一应用：编辑完成什么都不发生，显式 Commit 才写。
    Rig staged(WriteMode::StagedThenApply);
    staged.StageA();
    const auto skipped = staged.txn.OnEditCompleted();
    suite.expect(!skipped.has_value(), L"txn: OnEditCompleted in StagedThenApply mode does nothing");
    suite.expect(staged.store.writeCalls == 0 && staged.store.readCalls == 0 && staged.sink.uiCalls == 0,
        L"txn: StagedThenApply mode touches neither the store nor the user on edit completion");
    suite.expect(staged.audit.records.empty() && staged.txn.CurrentState() == State::Staged,
        L"txn: StagedThenApply mode writes no audit and stays Staged on edit completion");
    const CommitReport applied = staged.txn.Commit();
    suite.expect(applied.outcome == CommitOutcome::Committed && staged.store.writeCalls == 1
        && MemoryAt(staged, kA, 1) == Bytes{0x01},
        L"txn: an explicit Commit in StagedThenApply mode writes the staged byte");

    // 两种触发时机走的是同一条管线：完整的调用序列逐项相同。
    suite.expect(immediate.log == staged.log,
        L"txn: Immediate and StagedThenApply produce the identical call sequence");
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1", "W:1002+1:a0", "R:1002+1", "AUDIT:CommitFinished",
    };
    suite.expect(immediate.log == expected,
        L"txn: the shared pipeline is audit-start, confirm, re-read, write, verify read, audit-finish");

    // 立即写入模式下没有补丁时的编辑完成：照走管线，结果是 NoChange，不碰 store。
    Rig empty;
    const auto nothing = empty.txn.OnEditCompleted();
    suite.expect(nothing.has_value() && nothing->outcome == CommitOutcome::NoChange,
        L"txn: Immediate mode with no patches reports NoChange");
    suite.expect(empty.store.writeCalls == 0 && empty.store.readCalls == 0 && empty.sink.uiCalls == 0,
        L"txn: NoChange touches neither the store nor the user");

    // 切到暂存模式之后，编辑完成不再自动提交。
    Rig toggled;
    suite.expect(toggled.txn.SetMode(WriteMode::StagedThenApply) == ModeSwitchStatus::Switched,
        L"txn: switching mode with no patches is immediate");
    toggled.StageA();
    suite.expect(!toggled.txn.OnEditCompleted().has_value() && toggled.store.writeCalls == 0,
        L"txn: after switching to StagedThenApply edit completion no longer commits");
}

// ------------------------------------------------------------
// 四、模式切换：没有补丁直接切；有补丁必须先三选一。
// ------------------------------------------------------------
void TestModeSwitchWithoutPatches(KswordTests::Suite& suite) {
    Rig rig;
    const ModeSwitchStatus toStaged = rig.txn.SetMode(WriteMode::StagedThenApply);
    suite.expect(toStaged == ModeSwitchStatus::Switched && rig.txn.Mode() == WriteMode::StagedThenApply,
        L"txn: with no patches the mode switches directly");
    suite.expect(!rig.txn.PendingModeSwitch().has_value() && rig.store.writeCalls == 0,
        L"txn: a direct switch leaves no pending request and touches no store");

    // 目标等于当前模式：什么都没变，也不需要决定，哪怕有补丁。
    suite.expect(rig.txn.SetMode(WriteMode::StagedThenApply) == ModeSwitchStatus::Switched,
        L"txn: setting the current mode again is a no-op switch");
    rig.StageA();
    suite.expect(rig.txn.SetMode(WriteMode::StagedThenApply) == ModeSwitchStatus::Switched,
        L"txn: setting the current mode needs no decision even with patches pending");
    suite.expect(rig.overlay.HasPendingPatches() && !rig.txn.PendingModeSwitch().has_value(),
        L"txn: setting the current mode leaves the patches and creates no pending request");

    // 守卫真值表。
    suite.expect(rig.txn.PendingOverlayGuard(WriteMode::StagedThenApply, WriteMode::Immediate),
        L"txn: the guard blocks a real mode change while patches are pending");
    suite.expect(rig.txn.PendingOverlayGuard(WriteMode::Immediate, WriteMode::StagedThenApply),
        L"txn: the guard blocks the opposite direction too");
    suite.expect(!rig.txn.PendingOverlayGuard(WriteMode::Immediate, WriteMode::Immediate),
        L"txn: the guard never blocks a switch to the same mode");
    rig.overlay.DiscardAll();
    suite.expect(!rig.txn.PendingOverlayGuard(WriteMode::StagedThenApply, WriteMode::Immediate),
        L"txn: the guard lets a real mode change through when nothing is pending");
}

void TestModeSwitchNeedsDecision(KswordTests::Suite& suite) {
    // 有补丁：不切换，记下待决请求，不碰 store。两个方向都一样。
    Rig rig;
    rig.StageA();
    const ModeSwitchStatus asked = rig.txn.SetMode(WriteMode::StagedThenApply);
    suite.expect(asked == ModeSwitchStatus::NeedsDecision, L"txn: pending patches force a decision");
    suite.expect(rig.txn.Mode() == WriteMode::Immediate, L"txn: the mode does not change before the decision");
    suite.expect(rig.txn.PendingModeSwitch() == std::optional<WriteMode>(WriteMode::StagedThenApply),
        L"txn: the requested mode is remembered as pending");
    suite.expect(rig.store.writeCalls == 0 && rig.store.readCalls == 0 && rig.overlay.HasPendingPatches(),
        L"txn: asking for a decision neither writes nor drops the patches");

    Rig reverse(WriteMode::StagedThenApply);
    reverse.StageA();
    suite.expect(reverse.txn.SetMode(WriteMode::Immediate) == ModeSwitchStatus::NeedsDecision
        && reverse.txn.Mode() == WriteMode::StagedThenApply,
        L"txn: switching from StagedThenApply back to Immediate also needs a decision");

    // Cancel：什么都不改，待决请求清掉。
    const auto cancelled = rig.txn.ResolveModeSwitch(ModeSwitchDecision::Cancel);
    suite.expect(cancelled.status == ModeSwitchStatus::Cancelled && rig.txn.Mode() == WriteMode::Immediate,
        L"txn: Cancel keeps the mode");
    suite.expect(rig.overlay.HasPendingPatches() && rig.txn.CurrentState() == State::Staged
        && !rig.txn.PendingModeSwitch().has_value(),
        L"txn: Cancel keeps the patches and the Staged state and clears the pending request");
    suite.expect(rig.store.writeCalls == 0 && rig.store.readCalls == 0 && rig.sink.uiCalls == 0,
        L"txn: Cancel touches neither the store nor the user");

    // 迟到的第二次决定：没有待决请求就什么都不做——尤其 Discard 不得丢补丁。
    suite.expect(rig.txn.ResolveModeSwitch(ModeSwitchDecision::DiscardThenSwitch).status
        == ModeSwitchStatus::NoPendingSwitch,
        L"txn: a decision without a pending request is refused");
    suite.expect(rig.overlay.HasPendingPatches() && rig.txn.Mode() == WriteMode::Immediate,
        L"txn: a refused Discard decision does not drop the patches or change the mode");
    suite.expect(rig.txn.ResolveModeSwitch(ModeSwitchDecision::ApplyThenSwitch).status
        == ModeSwitchStatus::NoPendingSwitch && rig.store.writeCalls == 0,
        L"txn: a refused Apply decision does not write");

    // 未识别的决定值按 Cancel 处理。
    Rig garbage;
    garbage.StageA();
    (void)garbage.txn.SetMode(WriteMode::StagedThenApply);
    const auto unknown = garbage.txn.ResolveModeSwitch(static_cast<ModeSwitchDecision>(42));
    suite.expect(unknown.status == ModeSwitchStatus::Cancelled && garbage.txn.Mode() == WriteMode::Immediate
        && garbage.overlay.HasPendingPatches() && garbage.store.writeCalls == 0,
        L"txn: an unrecognised decision value behaves like Cancel");

    // 待决期间再 SetMode 成当前模式：待决请求作废，之后迟到的决定无效。
    Rig stale;
    stale.StageA();
    (void)stale.txn.SetMode(WriteMode::StagedThenApply);
    suite.expect(stale.txn.SetMode(WriteMode::Immediate) == ModeSwitchStatus::Switched
        && !stale.txn.PendingModeSwitch().has_value(),
        L"txn: setting the current mode cancels the pending request");
    suite.expect(stale.txn.ResolveModeSwitch(ModeSwitchDecision::DiscardThenSwitch).status
        == ModeSwitchStatus::NoPendingSwitch && stale.overlay.HasPendingPatches(),
        L"txn: a decision after the request was cancelled does nothing");
}

void TestModeSwitchDiscardAndApply(KswordTests::Suite& suite) {
    // DiscardThenSwitch：补丁丢弃、状态 Idle、模式切换，且一个字节都没写。
    Rig discard;
    discard.StageA();
    discard.StageB();
    (void)discard.txn.SetMode(WriteMode::StagedThenApply);
    const auto dropped = discard.txn.ResolveModeSwitch(ModeSwitchDecision::DiscardThenSwitch);
    suite.expect(dropped.status == ModeSwitchStatus::Switched && discard.txn.Mode() == WriteMode::StagedThenApply,
        L"txn: Discard then switch changes the mode");
    suite.expect(!discard.overlay.HasPendingPatches() && discard.txn.CurrentState() == State::Idle,
        L"txn: Discard drops every patch and returns to Idle");
    suite.expect(discard.store.writeCalls == 0 && discard.store.readCalls == 0
        && MemoryAt(discard, kA, 1) == Bytes{0x22},
        L"txn: Discard never writes the dropped edits to the target");
    suite.expect(!dropped.commitReport.has_value() && !discard.txn.PendingModeSwitch().has_value(),
        L"txn: Discard runs no commit and clears the pending request");

    // ApplyThenSwitch 成功：先走完整提交，再切换。
    Rig apply;
    apply.StageA();
    (void)apply.txn.SetMode(WriteMode::StagedThenApply);
    const auto applied = apply.txn.ResolveModeSwitch(ModeSwitchDecision::ApplyThenSwitch);
    suite.expect(applied.status == ModeSwitchStatus::Switched && apply.txn.Mode() == WriteMode::StagedThenApply,
        L"txn: Apply then switch changes the mode after a successful commit");
    suite.expect(applied.commitReport.has_value() && applied.commitReport->outcome == CommitOutcome::Committed
        && applied.commitReport->blocksWritten == 1,
        L"txn: Apply carries the commit report of the commit it ran");
    suite.expect(MemoryAt(apply, kA, 1) == Bytes{0x01} && !apply.overlay.HasPendingPatches(),
        L"txn: Apply wrote the staged byte before switching");
    suite.expect(apply.sink.uiCalls == 1, L"txn: Apply goes through the normal user confirmation");

    // ApplyThenSwitch 提交失败：模式不变，补丁保留，待决请求清掉。
    Rig failing;
    failing.StageA();
    failing.store.writeFault[kA].fail = true;
    (void)failing.txn.SetMode(WriteMode::StagedThenApply);
    const auto failed = failing.txn.ResolveModeSwitch(ModeSwitchDecision::ApplyThenSwitch);
    suite.expect(failed.status == ModeSwitchStatus::ApplyFailed && failing.txn.Mode() == WriteMode::Immediate,
        L"txn: a failed Apply does not switch the mode");
    suite.expect(failed.commitReport.has_value() && failed.commitReport->outcome == CommitOutcome::WriteFailed,
        L"txn: a failed Apply reports why the commit failed");
    suite.expect(failing.overlay.HasPendingPatches() && !failing.txn.PendingModeSwitch().has_value(),
        L"txn: a failed Apply keeps the unwritten patch and clears the pending request");

    // 用户在写入确认里拒绝：同样不切换。
    Rig declined;
    declined.StageA();
    declined.sink.uiAnswer = false;
    (void)declined.txn.SetMode(WriteMode::StagedThenApply);
    const auto refused = declined.txn.ResolveModeSwitch(ModeSwitchDecision::ApplyThenSwitch);
    suite.expect(refused.status == ModeSwitchStatus::ApplyFailed && declined.txn.Mode() == WriteMode::Immediate
        && refused.commitReport.has_value() && refused.commitReport->outcome == CommitOutcome::UserCancelled,
        L"txn: declining the write confirmation during Apply keeps the mode");
    suite.expect(declined.store.writeCalls == 0 && declined.overlay.HasPendingPatches(),
        L"txn: a declined Apply wrote nothing and kept the patches");

    // 待决期间补丁已被外部清空：Apply 得到 NoChange，没有东西可写，切换放行。
    Rig drained;
    drained.StageA();
    (void)drained.txn.SetMode(WriteMode::StagedThenApply);
    drained.overlay.DiscardAll();
    const auto nothing = drained.txn.ResolveModeSwitch(ModeSwitchDecision::ApplyThenSwitch);
    suite.expect(nothing.status == ModeSwitchStatus::Switched && nothing.commitReport.has_value()
        && nothing.commitReport->outcome == CommitOutcome::NoChange && drained.store.writeCalls == 0,
        L"txn: Apply with nothing left to write switches without touching the store");
}

// ------------------------------------------------------------
// 五、报告与状态的初值：默认值必须是"什么都没发生"。
// ------------------------------------------------------------
void TestDefaultsAreSafe(KswordTests::Suite& suite) {
    const CommitReport report;
    suite.expect(report.outcome == CommitOutcome::NoChange && report.blocksTotal == 0 && report.blocksWritten == 0
        && report.bytesWritten == 0 && report.approvalsAsked == 0,
        L"txn: a default report claims no outcome and no work");
    suite.expect(!report.scratchAreaDirty && !report.readModifyWriteWindow && !report.needsReread
        && report.failureText.empty() && report.sessionError == ksword::memwb::SessionError::None,
        L"txn: a default report carries no warnings and no failure text");

    const AccessResult access;
    suite.expect(!access.ok && !access.partial && access.data.empty() && access.bytesDone == 0
        && !access.needsExplicitApproval && !access.rolledBack,
        L"txn: a default access result is a failure that read and wrote nothing");
    suite.expect(!access.scratchAreaDirty && !access.readModifyWriteWindow && access.failureText.empty(),
        L"txn: a default access result carries no warnings");
}

} // namespace

namespace MemwbTxnTests {

// 本文件的测试组：暂存 / 模式 / 模式切换 / 初值。
void RunStagingAndModeGroups(KswordTests::Suite& suite) {
    TestIdleAndStagedNeverWrite(suite);
    TestStageForwardingAndStateRules(suite);
    TestImmediateAndStagedModesShareOnePipeline(suite);
    TestModeSwitchWithoutPatches(suite);
    TestModeSwitchNeedsDecision(suite);
    TestModeSwitchDiscardAndApply(suite);
    TestDefaultsAreSafe(suite);
}

} // namespace MemwbTxnTests

// 套件入口：三个文件的测试组共用同一个 Suite，只报一次汇总。
int RunMemwbWriteTransactionTests() {
    KswordTests::Suite suite(L"MEMWB write txn");
    MemwbTxnTests::RunStagingAndModeGroups(suite);
    MemwbTxnTests::RunCommitFlowGroups(suite);
    MemwbTxnTests::RunWriteOutcomeGroups(suite);
    suite.report();
    return suite.failures();
}
