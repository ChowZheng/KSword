// 写事务测试套件的第二个文件：提交顺序 / 取消 / 抑制 / 会话无效 / 陈旧 / 写前复核 / 重入。
// 约定与断言原则见 MemoryWriteTransactionTests.cpp 与 MemoryWriteTransactionTestSupport.h。
//
// 本文件的核心是"写之前"的那一半：
//   (a) NoChange   (b) InvalidSession   (c) 界面确认   (d) 陈旧复核   (e) 写前复核
// 这五步里任何一步失败，store 的写调用次数都必须是 0，且暂存补丁原样保留。

#include "MemoryWriteTransactionTestSupport.h"

#include <stdexcept>

namespace {

using namespace MemwbTxnTests;

// 在"写之前"中止的统一断言：零写入、状态 Staged、补丁与中止前一模一样。
// 入参：suite 断言容器；tag 场景名；rig 夹具；report 提交报告；outcome 期望结果；
//       patchesBefore 提交前取的补丁副本。
void ExpectAbortedBeforeWrite(
    KswordTests::Suite& suite,
    const std::wstring& tag,
    const Rig& rig,
    const CommitReport& report,
    CommitOutcome outcome,
    const std::vector<DiffBlock>& patchesBefore) {
    suite.expect(report.outcome == outcome, (tag + L": the outcome is the expected abort reason").c_str());
    suite.expect(rig.store.writeCalls == 0, (tag + L": no byte was written").c_str());
    suite.expect(rig.txn.CurrentState() == State::Staged, (tag + L": the state goes back to Staged").c_str());
    suite.expect(rig.overlay.DiffBlocks() == patchesBefore,
        (tag + L": the staged patches are kept exactly as they were").c_str());
    suite.expect(report.blocksWritten == 0 && report.bytesWritten == 0,
        (tag + L": the report claims no written blocks or bytes").c_str());
    suite.expect(!report.failureText.empty(), (tag + L": the report explains the abort").c_str());
}

// ------------------------------------------------------------
// 一、(a) 没有差异块：NoChange，且排在 (b) 会话校验之前。
// ------------------------------------------------------------
void TestNoChange(KswordTests::Suite& suite) {
    Rig rig;
    const CommitReport report = rig.txn.Commit();
    suite.expect(report.outcome == CommitOutcome::NoChange && report.blocksTotal == 0,
        L"commit: nothing staged reports NoChange with zero blocks");
    suite.expect(rig.store.writeCalls == 0 && rig.store.readCalls == 0 && rig.sink.uiCalls == 0,
        L"commit: NoChange touches neither the store nor the user");
    suite.expect(rig.txn.CurrentState() == State::Idle && !report.needsReread && report.failureText.empty(),
        L"commit: NoChange leaves the state alone and asks for no re-read");
    const Log expected = {"AUDIT:CommitStarted", "AUDIT:CommitFinished"};
    suite.expect(rig.log == expected, L"commit: NoChange still writes exactly a start and a finish audit");
    suite.expect(rig.audit.records.back().outcome == CommitOutcome::NoChange,
        L"commit: the finish audit of a NoChange carries the outcome");

    // 顺序：没有东西可写时不去苛责会话，即便会话无效也是 NoChange 而不是 InvalidSession。
    Rig invalid;
    invalid.session.pid = 0;
    suite.expect(invalid.txn.Commit().outcome == CommitOutcome::NoChange,
        L"commit: NoChange is decided before the session is validated");

    // 对照：一旦有补丁，同一个无效会话立刻变成 InvalidSession。
    invalid.StageA();
    suite.expect(invalid.txn.Commit().outcome == CommitOutcome::InvalidSession,
        L"commit: the same invalid session is rejected once there is something to write");
}

// ------------------------------------------------------------
// 二、(b) 会话无效：拒绝，不问用户，不碰 store。
// ------------------------------------------------------------
void TestInvalidSession(KswordTests::Suite& suite) {
    struct Case {
        const wchar_t* tag;
        void (*mutate)(MemoryTargetSession&);
        ksword::memwb::SessionError expected;
    };
    const Case cases[] = {
        {L"missing pid", [](MemoryTargetSession& s) { s.pid = 0; }, ksword::memwb::SessionError::NeedsPid},
        {L"bad address width", [](MemoryTargetSession& s) { s.addressBits = 16; },
            ksword::memwb::SessionError::BadAddressBits},
        {L"kernel scope with a pid", [](MemoryTargetSession& s) { s.scope = Scope::KernelVirtual; },
            ksword::memwb::SessionError::PidMustBeZero},
        {L"out-of-range scope", [](MemoryTargetSession& s) { s.scope = static_cast<Scope>(9); },
            ksword::memwb::SessionError::BadScope},
        {L"out-of-range channel", [](MemoryTargetSession& s) { s.channel = static_cast<Channel>(9); },
            ksword::memwb::SessionError::BadChannel},
    };

    for (const Case& item : cases) {
        const std::wstring tag = std::wstring(L"invalid session (") + item.tag + L")";
        Rig rig;
        rig.StageA();
        rig.StageB();
        const std::vector<DiffBlock> before = rig.overlay.DiffBlocks();
        item.mutate(rig.session);
        const CommitReport report = rig.txn.Commit();
        suite.expect(report.outcome == CommitOutcome::InvalidSession && report.sessionError == item.expected,
            (tag + L": rejected with the specific session violation").c_str());
        suite.expect(rig.store.writeCalls == 0 && rig.store.readCalls == 0 && rig.sink.uiCalls == 0,
            (tag + L": neither the store nor the user is touched").c_str());
        suite.expect(rig.overlay.DiffBlocks() == before && rig.txn.CurrentState() == State::Staged,
            (tag + L": the staged patches and the Staged state survive").c_str());
        suite.expect(!report.failureText.empty() && !report.needsReread && report.blocksTotal == 2,
            (tag + L": the report explains the rejection and counts the blocks").c_str());
        const std::vector<AuditEvent> expectedEvents = {AuditEvent::CommitStarted, AuditEvent::CommitFinished};
        suite.expect(rig.audit.Events() == expectedEvents && rig.audit.records.back().outcome == CommitOutcome::InvalidSession,
            (tag + L": the audit is a start and a finish carrying InvalidSession").c_str());

        // 对照：把会话修好，同一批补丁照常提交。
        rig.session = MakeSession();
        suite.expect(rig.txn.Commit().outcome == CommitOutcome::Committed,
            (tag + L": the same patches commit once the session is repaired").c_str());
    }
}

// ------------------------------------------------------------
// 三、固定调用顺序：用有序日志整条断言。
// ------------------------------------------------------------
void TestCommitCallOrder(KswordTests::Suite& suite) {
    // 两块：全部写前复核读都在第一次写之前；之后每块"写、回读"依次进行。
    Rig rig;
    rig.StageA();
    rig.StageB();
    const CommitReport report = rig.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted",
        "UI",
        "AUDIT:UiConfirmAccepted",
        "R:1002+1",
        "R:1006+2",
        "W:1002+1:a0",
        "R:1002+1",
        "W:1006+2:a0",
        "R:1006+2",
        "AUDIT:CommitFinished",
    };
    suite.expect(rig.log == expected,
        L"commit: order is start audit, confirm, every pre-write read, then write+verify block by block");
    suite.expect(report.outcome == CommitOutcome::Committed && report.blocksTotal == 2
        && report.blocksWritten == 2 && report.bytesWritten == 3,
        L"commit: a full commit reports 2 of 2 blocks and 3 bytes");
    suite.expect(report.approvalsAsked == 0 && !report.needsReread && report.failureText.empty()
        && !report.scratchAreaDirty && !report.readModifyWriteWindow,
        L"commit: a clean commit asks for nothing, warns of nothing and needs no re-read");
    suite.expect(MemoryAt(rig, kA, 1) == Bytes{0x01} && MemoryAt(rig, kB, 2) == (Bytes{0x02, 0x03}),
        L"commit: the target now holds 01 at 0x1002 and 02 03 at 0x1006");
    suite.expect(rig.txn.CurrentState() == State::Committed && !rig.overlay.HasPendingPatches(),
        L"commit: the state is Committed and no patch is left");

    // 叠加层被回读字节吸收：基线更新，写入的字节显示成"自己写入"而不是外部变化。
    suite.expect(rig.overlay.BaselineByte(kA) == Val(0x01)
        && rig.overlay.ChangeKind(kA) == ksword::memwb::ByteChangeKind::SelfWritten,
        L"commit: the overlay baseline absorbed the verified byte and marks it as self-written");
    suite.expect(rig.overlay.ChangeKind(kB + 1) == ksword::memwb::ByteChangeKind::SelfWritten
        && rig.overlay.ChangeKind(0x1004ULL) == ksword::memwb::ByteChangeKind::Unchanged,
        L"commit: only the written bytes are self-written");

    // 确认请求带的是开始时的块摘要，目标身份是手写的字面量。
    suite.expect(rig.sink.lastUi.blocksTotal == 2 && rig.sink.lastUi.bytesTotal == 3
        && rig.sink.lastUi.blocks.size() == 2,
        L"commit: the confirmation request counts 2 blocks and 3 bytes");
    suite.expect(rig.sink.lastUi.blocks[0].address == 0x1002ULL && rig.sink.lastUi.blocks[0].length == 1
        && rig.sink.lastUi.blocks[1].address == 0x1006ULL && rig.sink.lastUi.blocks[1].length == 2,
        L"commit: the confirmation lists the blocks in ascending address order");
    suite.expect(rig.sink.lastUi.targetIdentity == kExpectedIdentity,
        L"commit: the confirmation names the target identity");
    suite.expect(rig.audit.records.front().targetIdentity == kExpectedIdentity
        && rig.audit.records.back().targetIdentity == kExpectedIdentity,
        L"commit: the audit records name the same target identity");

    // 成功之后再 Commit：没有东西了，NoChange，状态保持 Committed。
    const CommitReport again = rig.txn.Commit();
    suite.expect(again.outcome == CommitOutcome::NoChange && rig.txn.CurrentState() == State::Committed
        && rig.store.writeCalls == 2,
        L"commit: a second commit with nothing staged is NoChange and writes nothing more");
}

// ------------------------------------------------------------
// 四、(c) 界面确认：拒绝回到 Staged；抑制只跳 UI，不跳审计。
// ------------------------------------------------------------
void TestUiConfirmCancelAndSuppress(KswordTests::Suite& suite) {
    // 用户拒绝：回到 Staged，零写入，三块补丁原样保留。
    Rig cancel;
    cancel.StageThree();
    const std::vector<DiffBlock> before = cancel.overlay.DiffBlocks();
    cancel.sink.uiAnswer = false;
    const CommitReport cancelled = cancel.txn.Commit();
    ExpectAbortedBeforeWrite(suite, L"cancel", cancel, cancelled, CommitOutcome::UserCancelled, before);
    suite.expect(cancel.store.readCalls == 0 && !cancelled.needsReread && cancelled.blocksTotal == 3,
        L"cancel: a declined commit does not even re-read the target and asks for no re-read");
    const std::vector<AuditEvent> cancelEvents = {
        AuditEvent::CommitStarted, AuditEvent::UiConfirmDenied, AuditEvent::CommitFinished};
    suite.expect(cancel.audit.Events() == cancelEvents && cancel.audit.records.back().outcome == CommitOutcome::UserCancelled,
        L"cancel: the audit records the denial and the UserCancelled finish");

    // 取消之后还能再提交（用户改了主意）。
    cancel.sink.uiAnswer = true;
    suite.expect(cancel.txn.Commit().outcome == CommitOutcome::Committed && cancel.store.writeCalls == 3,
        L"cancel: the same patches commit after the user changes their mind");

    // 抑制：不调用 ConfirmUi，仍然写一条审计，其余照常。
    Rig quiet;
    quiet.txn.SetUiConfirmSuppressed(true);
    suite.expect(quiet.txn.UiConfirmSuppressed(), L"suppress: the setting reads back");
    quiet.StageA();
    const CommitReport report = quiet.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted",
        "AUDIT:UiConfirmSuppressed",
        "R:1002+1",
        "W:1002+1:a0",
        "R:1002+1",
        "AUDIT:CommitFinished",
    };
    suite.expect(quiet.log == expected,
        L"suppress: ConfirmUi is skipped but the suppression is still audited, the rest is unchanged");
    suite.expect(quiet.sink.uiCalls == 0 && report.outcome == CommitOutcome::Committed,
        L"suppress: the user was never asked and the commit still succeeded");
    suite.expect(quiet.audit.Count(AuditEvent::UiConfirmSuppressed) == 1
        && quiet.audit.records[1].text.find("suppressed") != std::string::npos,
        L"suppress: the audit note says the confirmation was suppressed by the setting");

    // 抑制时的审计条数不得少于未抑制时。
    Rig loud;
    loud.StageA();
    (void)loud.txn.Commit();
    suite.expect(quiet.audit.records.size() >= loud.audit.records.size(),
        L"suppress: a suppressed commit never audits fewer records than a confirmed one");
    suite.expect(quiet.audit.records.size() == 3 && loud.audit.records.size() == 3,
        L"suppress: both paths audit exactly start, confirmation and finish");
    suite.expect(loud.audit.Count(AuditEvent::UiConfirmAccepted) == 1 && loud.audit.Count(AuditEvent::UiConfirmSuppressed) == 0,
        L"suppress: the confirmed path audits the acceptance, not the suppression");

    // 设置在每次 Commit 时读取：关掉抑制后又会问。
    // 第一次提交已把 0x1002 写成 01，所以这次要暂存一个不同的值，否则会被当成空操作。
    quiet.txn.SetUiConfirmSuppressed(false);
    quiet.txn.Stage(kA, Bytes{0x07});
    (void)quiet.txn.Commit();
    suite.expect(quiet.sink.uiCalls == 1, L"suppress: turning suppression off makes the next commit ask again");
}

// ------------------------------------------------------------
// 五、(d) 确认返回之后重新核对：身份、来源代次、内容代次分别触发 Stale。
// ------------------------------------------------------------
void TestStaleDetection(KswordTests::Suite& suite) {
    struct Case {
        const wchar_t* tag;
        void (*mutate)(Rig&);
    };
    const Case cases[] = {
        // 只有来源代次变（换目标 / 重读）。
        {L"only the source revision changed", [](Rig& r) { r.revisions.BumpSource(); }},
        // 只有内容代次变（编辑 / 撤销）。
        {L"only the content revision changed", [](Rig& r) { r.revisions.BumpContent(); }},
        {L"both revisions changed", [](Rig& r) {
            r.revisions.BumpSource();
            r.revisions.BumpContent();
        }},
        // 目标身份变：七个字段逐个换。
        {L"attach generation changed", [](Rig& r) { r.session.attachGeneration = 8; }},
        {L"channel changed", [](Rig& r) { r.session.channel = Channel::Hvm; }},
        {L"pid changed", [](Rig& r) { r.session.pid = 4321; }},
        {L"process create time changed", [](Rig& r) { r.session.processCreateTime100ns = 5001; }},
        {L"ddma generation changed", [](Rig& r) { r.session.ddmaGeneration = 4; }},
        {L"address width changed to a still-valid 32", [](Rig& r) { r.session.addressBits = 32; }},
        {L"scope changed to a still-valid kernel scope",
            [](Rig& r) {
                r.session.scope = Scope::KernelVirtual;
                r.session.pid = 0;
            }},
    };

    for (const Case& item : cases) {
        const std::wstring tag = std::wstring(L"stale (") + item.tag + L")";
        Rig rig;
        rig.StageThree();
        const std::vector<DiffBlock> before = rig.overlay.DiffBlocks();
        void (*const mutate)(Rig&) = item.mutate;
        rig.sink.onUi = [&rig, mutate]() { mutate(rig); };
        const CommitReport report = rig.txn.Commit();
        ExpectAbortedBeforeWrite(suite, tag, rig, report, CommitOutcome::Stale, before);
        suite.expect(rig.store.readCalls == 0,
            (tag + L": the check happens before the pre-write reads, so the target is not even read").c_str());
        suite.expect(rig.sink.uiCalls == 1 && report.needsReread,
            (tag + L": the user was asked once and the report asks for a re-read").c_str());
        ExpectAuditBracketed(suite, tag, rig);
        suite.expect(rig.audit.records.back().outcome == CommitOutcome::Stale
            && rig.audit.Count(AuditEvent::UiConfirmAccepted) == 1,
            (tag + L": the audit shows the user accepted but the commit finished Stale").c_str());
    }

    // 失败原因区分来源与内容。
    Rig source;
    source.StageA();
    source.sink.onUi = [&source]() { source.revisions.BumpSource(); };
    suite.expect(source.txn.Commit().failureText.find("source") != std::string::npos,
        L"stale: a source-only change is reported as a source revision change");
    Rig content;
    content.StageA();
    content.sink.onUi = [&content]() { content.revisions.BumpContent(); };
    suite.expect(content.txn.Commit().failureText.find("content") != std::string::npos,
        L"stale: a content-only change is reported as a content revision change");

    // 对照一：确认窗口里什么都没变，正常提交。
    Rig calm;
    calm.StageA();
    calm.sink.onUi = []() {};
    suite.expect(calm.txn.Commit().outcome == CommitOutcome::Committed,
        L"stale: an uneventful confirmation window commits normally");

    // 对照二：变化发生在 Commit 开始之前，快照在 (a) 才捕获，所以不算陈旧。
    Rig early;
    early.StageA();
    early.revisions.BumpSource();
    early.revisions.BumpContent();
    early.session.attachGeneration = 99;
    suite.expect(early.txn.Commit().outcome == CommitOutcome::Committed,
        L"stale: changes before Commit starts are part of the snapshot and are not stale");

    // 抑制了界面确认，(d) 仍然要做：在"确认被抑制"的审计落下那一刻改代次。
    Rig quiet;
    quiet.txn.SetUiConfirmSuppressed(true);
    quiet.StageA();
    quiet.audit.onRecord = [&quiet](const AuditRecord& record) {
        if (record.event == AuditEvent::UiConfirmSuppressed) {
            quiet.revisions.BumpSource();
        }
    };
    const CommitReport quietReport = quiet.txn.Commit();
    suite.expect(quietReport.outcome == CommitOutcome::Stale && quiet.store.writeCalls == 0
        && quiet.store.readCalls == 0,
        L"stale: the freshness re-check still runs when the ui confirmation is suppressed");
    suite.expect(quiet.sink.uiCalls == 0, L"stale: and the suppressed ui confirmation was indeed never asked");
}

// ------------------------------------------------------------
// 六、(e) 写前复核：任何一块读不出、读一半、或与 before 不一致，整体零写入。
// ------------------------------------------------------------
void TestPrecheckTargetChanged(KswordTests::Suite& suite) {
    struct Case {
        const wchar_t* tag;
        void (*prepare)(Rig&);
        int expectedReads;
    };
    const Case cases[] = {
        // 第一块的原字节被改了：读到第一块就停。
        {L"first block differs", [](Rig& r) { r.store.memory[kA] = 0x23; }, 1},
        // 中间块第一个字节被改了。
        {L"middle block differs in its first byte", [](Rig& r) { r.store.memory[kB] = 0x00; }, 2},
        // 中间块只有最后一个字节不同：必须整段比较。
        {L"middle block differs only in its last byte", [](Rig& r) { r.store.memory[kB + 1] = 0x78; }, 2},
        // 最后一块被改了：前两块都复核通过，仍然一个字节都不写。
        {L"last block differs after two clean blocks", [](Rig& r) { r.store.memory[kC] = 0xCD; }, 3},
        // 目标上已经是我们要写的值：与 before 不同就是"已变"。
        {L"target already holds the new value", [](Rig& r) { r.store.memory[kA] = 0x01; }, 1},
        // 读失败：无法确认没变，同样中止。数据故意给对（与 before 一致），
        // 这样只有"读失败"这一个标志能让它中止，不会被"字节不一致"顺带挡住。
        {L"re-read fails although the bytes handed back match", [](Rig& r) {
            r.store.readHook = [](std::uint64_t address, std::uint64_t, AccessResult& out) {
                if (address != kB) {
                    return false;
                }
                out.ok = false;
                out.data = Bytes{0x66, 0x77};
                out.bytesDone = 2;
                out.failureText = "device gone";
                return true;
            };
        }, 2},
        // 读一半：缺的那部分没法核对。这一例读到的字节比 before 短。
        {L"re-read is partial and short", [](Rig& r) {
            r.store.readHook = [](std::uint64_t address, std::uint64_t, AccessResult& out) {
                if (address != kB) {
                    return false;
                }
                out.ok = true;
                out.partial = true;
                out.data = Bytes{0x66};
                out.bytesDone = 1;
                return true;
            };
        }, 2},
        // 读一半：这一例读到的字节与 before 完全一致，只有 partial 标志在说"没读全"。
        {L"re-read is flagged partial although the bytes match", [](Rig& r) {
            r.store.readHook = [](std::uint64_t address, std::uint64_t, AccessResult& out) {
                if (address != kB) {
                    return false;
                }
                out.ok = true;
                out.partial = true;
                out.data = Bytes{0x66, 0x77};
                out.bytesDone = 2;
                return true;
            };
        }, 2},
        // 声称成功、未 partial，但长度不对：不一致。
        {L"re-read returns too many bytes", [](Rig& r) {
            r.store.readHook = [](std::uint64_t address, std::uint64_t, AccessResult& out) {
                if (address != kA) {
                    return false;
                }
                out.ok = true;
                out.data = Bytes{0x22, 0x00};
                out.bytesDone = 2;
                return true;
            };
        }, 1},
    };

    for (const Case& item : cases) {
        const std::wstring tag = std::wstring(L"precheck (") + item.tag + L")";
        Rig rig;
        rig.StageThree();
        const std::vector<DiffBlock> before = rig.overlay.DiffBlocks();
        item.prepare(rig);
        const CommitReport report = rig.txn.Commit();
        ExpectAbortedBeforeWrite(suite, tag, rig, report, CommitOutcome::TargetChanged, before);
        suite.expect(rig.store.readCalls == item.expectedReads,
            (tag + L": the pre-write reads stop at the first block that cannot be confirmed").c_str());
        suite.expect(report.needsReread && rig.sink.uiCalls == 1,
            (tag + L": the user had confirmed and the report asks for a re-read").c_str());
        ExpectAuditBracketed(suite, tag, rig);
        suite.expect(rig.audit.records.back().outcome == CommitOutcome::TargetChanged,
            (tag + L": the finish audit carries TargetChanged").c_str());
    }

    // 精确的调用序列：复核在第二块停下，一次写都没有，也没有回读。
    Rig rig;
    rig.StageThree();
    rig.store.memory[kB] = 0x00;
    (void)rig.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1", "R:1006+2",
        "AUDIT:CommitFinished",
    };
    suite.expect(rig.log == expected, L"precheck: the call log shows reads only and no write at all");

    // 读失败的原因要原样带出。
    Rig failing;
    failing.StageA();
    failing.store.readHook = [](std::uint64_t, std::uint64_t, AccessResult& out) {
        out.ok = false;
        out.failureText = "device gone";
        return true;
    };
    suite.expect(failing.txn.Commit().failureText.find("device gone") != std::string::npos,
        L"precheck: the store's failure text reaches the report");

    // 写前复核读结果里的告警位也要带出，哪怕这次提交被中止。
    Rig dirty;
    dirty.StageA();
    dirty.store.dirtyOnRead.insert(kA);
    dirty.store.rmwOnRead.insert(kA);
    dirty.store.memory[kA] = 0x23;
    const CommitReport dirtyReport = dirty.txn.Commit();
    suite.expect(dirtyReport.outcome == CommitOutcome::TargetChanged && dirtyReport.scratchAreaDirty
        && dirtyReport.readModifyWriteWindow,
        L"precheck: warning bits from the pre-write read survive an aborted commit");
}

// ------------------------------------------------------------
// 七、状态机迁移：用探针记录每次访问时的状态。
// ------------------------------------------------------------
void TestStateProgression(KswordTests::Suite& suite) {
    Rig rig;
    rig.StageA();
    std::vector<State> seen;
    std::vector<bool> busySeen;
    rig.sink.onUi = [&rig, &seen, &busySeen]() {
        seen.push_back(rig.txn.CurrentState());
        busySeen.push_back(rig.txn.IsBusy());
    };
    rig.store.probe = [&rig, &seen, &busySeen]() {
        seen.push_back(rig.txn.CurrentState());
        busySeen.push_back(rig.txn.IsBusy());
    };
    const CommitReport report = rig.txn.Commit();
    // 依次是：确认时、写前复核读时、写时、回读时。
    const std::vector<State> expected = {
        State::ConfirmPending, State::ConfirmPending, State::Writing, State::Verifying};
    suite.expect(seen == expected,
        L"state: ConfirmPending during confirmation and pre-write read, Writing during write, Verifying during read-back");
    suite.expect(busySeen == std::vector<bool>(4, true), L"state: the transaction is busy at every one of those moments");
    suite.expect(report.outcome == CommitOutcome::Committed && rig.txn.CurrentState() == State::Committed
        && !rig.txn.IsBusy(),
        L"state: a successful commit ends Committed and not busy");

    // 抑制确认时同样先进入 ConfirmPending。
    Rig quiet;
    quiet.txn.SetUiConfirmSuppressed(true);
    quiet.StageA();
    State atSuppressAudit = State::Idle;
    quiet.audit.onRecord = [&quiet, &atSuppressAudit](const AuditRecord& record) {
        if (record.event == AuditEvent::UiConfirmSuppressed) {
            atSuppressAudit = quiet.txn.CurrentState();
        }
    };
    (void)quiet.txn.Commit();
    suite.expect(atSuppressAudit == State::ConfirmPending,
        L"state: the suppressed-confirmation audit is written while ConfirmPending");

    // Stage 之后从各个终态都回到 Staged。
    suite.expect(rig.txn.Stage(kB, Bytes{0x02, 0x03}) == StageStatus::Ok && rig.txn.CurrentState() == State::Staged,
        L"state: staging after Committed returns to Staged");
    Rig failed;
    failed.StageA();
    failed.store.writeFault[kA].fail = true;
    (void)failed.txn.Commit();
    suite.expect(failed.txn.CurrentState() == State::Failed, L"state: a failed write ends Failed");
    failed.StageB();
    suite.expect(failed.txn.CurrentState() == State::Staged, L"state: staging after Failed returns to Staged");
}

// ------------------------------------------------------------
// 八、重入与异常：确认窗口里再点一次"应用"必须被挡住；异常不能让对象永远忙。
// ------------------------------------------------------------
void TestReentrancyAndExceptions(KswordTests::Suite& suite) {
    // 在确认窗口（旧代码的嵌套事件循环）里重入各个入口。
    Rig rig;
    rig.StageA();
    CommitReport nested;
    ModeSwitchStatus nestedSetMode = ModeSwitchStatus::Switched;
    ModeSwitchStatus nestedResolve = ModeSwitchStatus::Switched;
    StageStatus nestedStage = StageStatus::Empty;
    State stateAfterNestedStage = State::Idle;
    rig.sink.onUi = [&]() {
        nested = rig.txn.Commit();
        nestedSetMode = rig.txn.SetMode(WriteMode::StagedThenApply);
        nestedResolve = rig.txn.ResolveModeSwitch(ModeSwitchDecision::DiscardThenSwitch).status;
        nestedStage = rig.txn.Stage(0x1004ULL, Bytes{0x05});
        stateAfterNestedStage = rig.txn.CurrentState();
    };
    const CommitReport outer = rig.txn.Commit();
    suite.expect(nested.outcome == CommitOutcome::Busy && !nested.failureText.empty(),
        L"reentrancy: a commit started inside the confirmation window is refused as Busy");
    suite.expect(nestedSetMode == ModeSwitchStatus::Busy && nestedResolve == ModeSwitchStatus::Busy
        && rig.txn.Mode() == WriteMode::Immediate,
        L"reentrancy: mode switching inside the confirmation window is refused as Busy");
    suite.expect(outer.outcome == CommitOutcome::Committed && rig.store.writeCalls == 1,
        L"reentrancy: the outer commit still completes and the block is written exactly once");
    suite.expect(nestedStage == StageStatus::Ok && stateAfterNestedStage == State::ConfirmPending,
        L"reentrancy: staging inside the window is accepted but does not move the state machine");
    suite.expect(rig.overlay.HasPendingPatches() && MemoryAt(rig, 0x1004ULL, 1) == Bytes{0x44},
        L"reentrancy: an edit staged inside the window is not swept into the running commit");
    suite.expect(rig.audit.Count(AuditEvent::CommitStarted) == 2 && rig.audit.Count(AuditEvent::CommitFinished) == 2
        && rig.audit.records[1].event == AuditEvent::CommitStarted
        && rig.audit.records[2].event == AuditEvent::CommitFinished
        && rig.audit.records[2].outcome == CommitOutcome::Busy,
        L"reentrancy: the refused attempt is audited as its own start and a Busy finish");
    suite.expect(!rig.txn.IsBusy() && rig.txn.CurrentState() == State::Committed,
        L"reentrancy: afterwards the transaction is idle again");

    // store 抛异常：写阶段，忙标志清掉、状态 Failed、补丁还在，之后可以重试。
    Rig writeThrow;
    writeThrow.StageA();
    writeThrow.store.onWrite = []() { throw std::runtime_error("boom"); };
    bool thrown = false;
    try {
        (void)writeThrow.txn.Commit();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    suite.expect(thrown, L"exception: an exception from the store propagates to the caller");
    suite.expect(!writeThrow.txn.IsBusy() && writeThrow.txn.CurrentState() == State::Failed,
        L"exception: the busy flag is cleared and the in-flight state becomes Failed");
    suite.expect(writeThrow.overlay.HasPendingPatches(), L"exception: the staged patch survives the exception");
    writeThrow.store.onWrite = nullptr;
    suite.expect(writeThrow.txn.Commit().outcome == CommitOutcome::Committed,
        L"exception: the transaction can be retried after the exception");

    // 确认接口抛异常：ConfirmPending 变 Failed。
    Rig uiThrow;
    uiThrow.StageA();
    uiThrow.sink.onUi = []() { throw std::runtime_error("dialog crashed"); };
    thrown = false;
    try {
        (void)uiThrow.txn.Commit();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    suite.expect(thrown && !uiThrow.txn.IsBusy() && uiThrow.txn.CurrentState() == State::Failed
        && uiThrow.store.writeCalls == 0,
        L"exception: an exception from the confirmation ends Failed with nothing written");

    // 审计接口在开始时抛异常：还没有任何进行中的状态，状态保持 Staged。
    Rig auditThrow;
    auditThrow.StageA();
    auditThrow.audit.onRecord = [](const AuditRecord&) { throw std::runtime_error("audit sink down"); };
    thrown = false;
    try {
        (void)auditThrow.txn.Commit();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    suite.expect(thrown && !auditThrow.txn.IsBusy() && auditThrow.txn.CurrentState() == State::Staged
        && auditThrow.store.writeCalls == 0 && auditThrow.store.readCalls == 0,
        L"exception: an audit failure at start leaves the state Staged and nothing written");
}

} // namespace

namespace MemwbTxnTests {

// 本文件的测试组：写之前的各步骤与重入。
void RunCommitFlowGroups(KswordTests::Suite& suite) {
    TestNoChange(suite);
    TestInvalidSession(suite);
    TestCommitCallOrder(suite);
    TestUiConfirmCancelAndSuppress(suite);
    TestStaleDetection(suite);
    TestPrecheckTargetChanged(suite);
    TestStateProgression(suite);
    TestReentrancyAndExceptions(suite);
}

} // namespace MemwbTxnTests
