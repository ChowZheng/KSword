// wpJ4_tests.Commit.cpp
// 作用：W1-W4 提交管线的关键判断——
//   - onEditCompleted 立即模式走一次完整 Commit；暂存模式只发
//     pendingPatchesChanged，端口零调用。
//   - canvasReadOnlyHook 提交期间恰好两次（true 后 false），确认接口抛异常时
//     也要走到 false（RAII，不吞异常）。
//   - 成功后只对 blocksWritten 个真正落地的块记账（"前两块成功、第三块
//     VerifyMismatch 只记两步"）、对应的局部重读聚合范围。
//   - scratchAreaDirtyReported / needsReread->requestReload 两个 W4 信号/副作用。

#include "wpJ4_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchWriteController.Internal.h"

#include <stdexcept>

namespace wpj4_test
{
    namespace
    {
        // M-C1：立即模式下 onEditCompleted 走一次完整 Commit，commitFinished 带
        // Committed 结果，pendingPatchesChanged 最终归零。
        void TestImmediateCommitSucceeds()
        {
            Harness h;
            h.AttachProcess(200);
            h.controller.setUiConfirmSuppressed(true); // 跳过确认，专注提交管线本身。
            h.EnsureWired();
            h.rawPort->script = {MemwbIoTests::MakeOk({0x01, 0x02})}; // 写前复核读。
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(2)};
            // 回读核对：再追加一条 Ok 结果。
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0xAA, 0xBB}));

            int finishedCount = 0;
            ksword::memwb::CommitReport lastReport;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport report)
                {
                    ++finishedCount;
                    lastReport = report;
                });
            quint64 lastPendingBytes = 999;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                [&](quint64 bytes, quint64 /*blocks*/) { lastPendingBytes = bytes; });

            h.LoadBaseline(0x4000, {0x01, 0x02});
            WPJ4_CHECK(h.Stage(0x4000, {0xAA, 0xBB}) == ksword::memwb::StageStatus::Ok);
            h.controller.onEditCompleted();

            WPJ4_CHECK(finishedCount == 1);
            WPJ4_CHECK(lastReport.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ4_CHECK(lastReport.blocksWritten == 1);
            WPJ4_CHECK(lastPendingBytes == 0);
            WPJ4_CHECK(h.readOnlyCalls.size() == 2);
            if (h.readOnlyCalls.size() == 2)
            {
                WPJ4_CHECK(h.readOnlyCalls[0] == true);
                WPJ4_CHECK(h.readOnlyCalls[1] == false);
            }
            // setCommitSuspendHook（COMMON 裁决第 3 项）：与 canvasReadOnlyHook_
            // 分开装配的另一路挂起通知，必须在同一次 Commit 里同步触发（true 后
            // false，各一次）。
            WPJ4_CHECK(h.commitSuspendCalls.size() == 2);
            if (h.commitSuspendCalls.size() == 2)
            {
                WPJ4_CHECK(h.commitSuspendCalls[0] == true);
                WPJ4_CHECK(h.commitSuspendCalls[1] == false);
            }
        }

        // M-C2：确认接口抛异常时，canvasReadOnlyHook 仍然走到 false（RAII），且
        // 异常原样向上传播（不被本类吞掉）。
        void TestExceptionDuringConfirmStillRestoresReadOnly()
        {
            Harness h;
            h.AttachProcess(201);
            h.controller.setUiConfirmSuppressed(false); // 强制走 ConfirmUi。
            h.prompter->throwOnUiConfirm = true;
            h.EnsureWired();

            h.LoadBaseline(0x4100, {0x00});
            WPJ4_CHECK(h.Stage(0x4100, {0x01}) == ksword::memwb::StageStatus::Ok);

            bool caught = false;
            try
            {
                h.controller.onEditCompleted();
            }
            catch (const std::runtime_error&)
            {
                caught = true;
            }
            WPJ4_CHECK_NOTE(caught, QStringLiteral("异常必须向上传播，不能被本类吞掉"));
            WPJ4_CHECK(h.readOnlyCalls.size() == 2);
            if (h.readOnlyCalls.size() == 2)
            {
                WPJ4_CHECK(h.readOnlyCalls[0] == true);
                WPJ4_CHECK_NOTE(
                    h.readOnlyCalls[1] == false,
                    QStringLiteral("确认接口抛异常也必须恢复只读状态为 false"));
            }
            // commitSuspendHook_ 走同一套 RAII，异常路径也必须恢复为 false。
            WPJ4_CHECK(h.commitSuspendCalls.size() == 2);
            if (h.commitSuspendCalls.size() == 2)
            {
                WPJ4_CHECK(h.commitSuspendCalls[0] == true);
                WPJ4_CHECK(h.commitSuspendCalls[1] == false);
            }
            // 既然确认阶段就抛了异常，端口不应该被写过。
            WPJ4_CHECK(h.rawPort->writeCalls.empty());
        }

        // M-C3：暂存模式下 onEditCompleted 只发 pendingPatchesChanged，端口零
        // 调用；之后 commitPendingNow() 才真正提交。
        void TestStagedModeDefersCommit()
        {
            Harness h;
            h.AttachProcess(202);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                == ksword::memwb::ModeSwitchStatus::Switched);

            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x05})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            h.LoadBaseline(0x4200, {0x00});
            WPJ4_CHECK(h.Stage(0x4200, {0x05}) == ksword::memwb::StageStatus::Ok);

            int pendingSignalCount = 0;
            quint64 pendingBytes = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                [&](quint64 bytes, quint64 /*blocks*/)
                {
                    ++pendingSignalCount;
                    pendingBytes = bytes;
                });

            h.controller.onEditCompleted();
            WPJ4_CHECK(pendingSignalCount == 1);
            WPJ4_CHECK(pendingBytes == 1);
            WPJ4_CHECK_NOTE(
                h.rawPort->writeCalls.empty(),
                QStringLiteral("暂存模式下 onEditCompleted 不应该触碰端口"));

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 1);
        }

        // M-C4：成功后只对 blocksWritten 个真正落地的块记账——构造"前两块成功、
        // 第三块 VerifyMismatch"的假端口场景，journal 只记两步，第三块的暂存
        // 补丁原样保留；needsReread 为真时 target_->requestReload() 被调用
        // （观察 sessionChanged 是否至少多发一次）。
        void TestOnlyLandedBlocksAreRecorded()
        {
            Harness h;
            h.AttachProcess(203);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();

            // 三个互不相邻的块，避免被 overlay 自动合并成一块。
            h.LoadBaseline(0x5000, std::vector<std::uint8_t>(0x210, 0));
            WPJ4_CHECK(h.Stage(0x5000, {0x11, 0x12, 0x13, 0x14}) == ksword::memwb::StageStatus::Ok);
            WPJ4_CHECK(h.Stage(0x5100, {0x21, 0x22, 0x23, 0x24}) == ksword::memwb::StageStatus::Ok);
            WPJ4_CHECK(h.Stage(0x5200, {0x31, 0x32, 0x33, 0x34}) == ksword::memwb::StageStatus::Ok);

            // 写前复核三次读（各自读到 before，即全 0）。
            h.rawPort->script = {
                MemwbIoTests::MakeOk({0, 0, 0, 0}),
                MemwbIoTests::MakeOk({0, 0, 0, 0}),
                MemwbIoTests::MakeOk({0, 0, 0, 0}),
                // 块1、块2 的回读核对均匹配 after。
                MemwbIoTests::MakeOk({0x11, 0x12, 0x13, 0x14}),
                MemwbIoTests::MakeOk({0x21, 0x22, 0x23, 0x24}),
                // 块3 回读核对不匹配 after（故意返回一份不同的数据）。
                MemwbIoTests::MakeOk({0xFF, 0xFF, 0xFF, 0xFF}),
            };
            h.rawPort->writeScript = {
                MemwbIoTests::MakeWriteOk(4),
                MemwbIoTests::MakeWriteOk(4),
                MemwbIoTests::MakeWriteOk(4),
            };

            int sessionChangedCount = 0;
            QObject::connect(&h.target, &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32) { ++sessionChangedCount; });

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            const ksword::memwb::CommitReport& report = attempt.report;
            WPJ4_CHECK(report.outcome == ksword::memwb::CommitOutcome::VerifyMismatch);
            WPJ4_CHECK(report.blocksWritten == 2);
            WPJ4_CHECK_NOTE(
                report.needsReread == true,
                QStringLiteral("VerifyMismatch 应当标记 needsReread"));
            WPJ4_CHECK_NOTE(
                sessionChangedCount >= 1,
                QStringLiteral("needsReread 应触发 target_->requestReload()，进而发出 sessionChanged"));

            // 局部重读应恰好一次，覆盖块1、块2的聚合范围 [0x5000, 0x5104)。
            WPJ4_CHECK(h.rereadCalls.size() == 1);
            if (h.rereadCalls.size() == 1)
            {
                WPJ4_CHECK(h.rereadCalls[0].first == 0x5000);
                WPJ4_CHECK(h.rereadCalls[0].second == (0x5104 - 0x5000));
            }

            // journal 只记两步：撤销两次都应该成功（Replayed），第三次没有更多
            // 历史（NothingToReplay，通过 canUndo() 间接验证）。
            WPJ4_CHECK(h.controller.canUndo() == true);

            // 第三块失败，暂存补丁原样保留在 overlay 里。
            const std::vector<ksword::memwb::DiffBlock> remaining = h.overlay.DiffBlocks();
            WPJ4_CHECK(remaining.size() == 1);
            if (!remaining.empty())
            {
                WPJ4_CHECK(remaining.front().address == 0x5200);
            }
        }

        // M-C5：scratchAreaDirty 的端口结果经由 byteStore/transaction 聚合进
        // CommitReport，handleCommitReport 据此发出 scratchAreaDirtyReported。
        void TestScratchAreaDirtyReported()
        {
            Harness h;
            h.AttachProcess(204);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();

            h.LoadBaseline(0x6000, {0x00});
            WPJ4_CHECK(h.Stage(0x6000, {0x09}) == ksword::memwb::StageStatus::Ok);

            ksword::memwb::IoReadResult precheck;
            precheck.status = ksword::memwb::IoReadStatus::Ok;
            precheck.data = {0x00};
            ksword::memwb::IoReadResult verify;
            verify.status = ksword::memwb::IoReadStatus::Ok;
            verify.data = {0x09};
            verify.scratchAreaDirty = true; // 回读阶段报告暂存区被弄脏。
            h.rawPort->script = {precheck, verify};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            int dirtySignalCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::scratchAreaDirtyReported,
                [&]() { ++dirtySignalCount; });

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ4_CHECK(attempt.report.scratchAreaDirty == true);
            WPJ4_CHECK(dirtySignalCount == 1);
        }

        // M-C6：commitFailed 是 commitFinished 的子集——只有非 NoChange/Committed
        // 的结果才额外发一次。用 TargetChanged（写前复核不匹配）构造。
        void TestCommitFailedOnlyOnRealFailure()
        {
            Harness h;
            h.AttachProcess(205);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();

            h.LoadBaseline(0x7000, {0x00});
            WPJ4_CHECK(h.Stage(0x7000, {0x09}) == ksword::memwb::StageStatus::Ok);
            // 写前复核故意返回与暂存时的 before 不一致的数据。
            h.rawPort->script = {MemwbIoTests::MakeOk({0xEE})};

            int finishedCount = 0;
            int failedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFailed,
                [&](ksword::memwb::CommitReport) { ++failedCount; });

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            const ksword::memwb::CommitReport& report = attempt.report;
            WPJ4_CHECK(report.outcome == ksword::memwb::CommitOutcome::TargetChanged);
            WPJ4_CHECK(finishedCount == 1);
            WPJ4_CHECK(failedCount == 1);
            WPJ4_CHECK(h.rawPort->writeCalls.empty()); // 写前复核失败，不该有任何写入。
        }

        // M-C7：没有暂存补丁时 commitPendingNow 返回 NoChange，不发
        // commitFailed，端口零调用。
        void TestNoChangeDoesNotFail()
        {
            Harness h;
            h.AttachProcess(206);
            h.EnsureWired();
            int failedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFailed,
                [&](ksword::memwb::CommitReport) { ++failedCount; });
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::NoChange);
            WPJ4_CHECK(failedCount == 0);
            WPJ4_CHECK(h.rawPort->calls.empty());
            WPJ4_CHECK(h.rawPort->writeCalls.empty());
        }

        // M-C8（D3 回归，原审核探针 reentrant-edit 场景；**第二轮审核 B-1/B-2
        // 裁决后重写**）：确认框弹出期间（PromptUiConfirm 的回调还没返回），别的
        // 入口在这个窗口里又触发一次 onEditCompleted()。修复前：嵌套会新建一个
        // 独立 Commit，它的 CommitReadOnlyGuard 析构时提前把画布解锁。D3 修复后：
        // 嵌套在 commitDepth_>0 时被直接拒绝，不提前解锁；但当时还会置
        // rerunRequested_ 待外层结束后自动补跑——第二轮审核发现这个补跑完全不看
        // 外层结果，已被裁决删掉（B-1/B-2）。本测试验证：嵌套被拒绝、画布不提前
        // 解锁，外层因内容代次被推进而被 Core 判 Stale（既有正确保护，非本次要修
        // 的缺陷），**绝不**自动补跑、**绝不**再弹第二次确认框，补丁原样留在
        // overlay 里。
        void TestNestedEditDuringConfirmIsRejectedNotNested()
        {
            Harness h;
            h.AttachProcess(207);
            h.controller.setUiConfirmSuppressed(false); // 强制走 ConfirmUi，制造"模态框开着"的窗口。
            h.EnsureWired();
            h.LoadBaseline(0x4300, {0x00});
            WPJ4_CHECK(h.Stage(0x4300, {0x01}) == ksword::memwb::StageStatus::Ok);
            // 这次提交会在 Core 的新鲜度复核（RecheckFreshness，步骤 (d)）就被
            // 拦下，根本不会走到写前复核读那一步（步骤 (e)），所以不需要编排
            // 任何端口脚本——下面直接断言端口零调用。

            int busyCount = 0;
            QString busySource;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy,
                [&](QString source) { ++busyCount; busySource = source; });
            int finishedCount = 0;
            int failedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFailed,
                [&](ksword::memwb::CommitReport) { ++failedCount; });

            bool triggered = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (triggered)
                {
                    return; // 防止万一出现递归时无限循环（正常情况下不会再触发）。
                }
                triggered = true;
                // 复现"别的入口在确认框开着期间又触发一次 onEditCompleted"：不需要
                // 真的 Stage 新字节，光调用这一个方法就足以推进内容代次、命中
                // D3 的重入保护。
                h.controller.onEditCompleted();
                // 外层确认框的回调还没返回，画布此刻必须仍然是只读（只应该看到
                // 一次 true，还没有任何 false）——这是 D3 修复要保证的核心不变式，
                // 第二轮裁决没有改变这一条。
                WPJ4_CHECK_NOTE(
                    h.readOnlyCalls.size() == 1 && h.readOnlyCalls[0] == true,
                    QStringLiteral("D3 修复：嵌套编辑被拒绝时画布不应该被提前解锁"));
            };

            h.controller.onEditCompleted();

            WPJ4_CHECK_NOTE(
                busyCount == 1, QStringLiteral("嵌套编辑应该被拒绝并发一次 commitRejectedBusy"));
            WPJ4_CHECK(busySource == QStringLiteral("onEditCompleted"));
            // 外层那次因为确认期间内容被推进而被 Core 判 Stale，commitFinished/
            // commitFailed 各恰好一次——**不再有合并补跑**，所以不会再多一轮。
            WPJ4_CHECK(finishedCount == 1);
            WPJ4_CHECK(failedCount == 1);
            // **B-1 裁决的核心断言**：只有外层这一次确认框，用户即使什么都没做
            // （这里脚本默认 uiConfirmAnswer=true），也绝不应该被系统自己再问
            // 一次。
            WPJ4_CHECK_NOTE(
                h.prompter->uiConfirmCallCount == 1,
                QStringLiteral("B-1 裁决：删掉合并补跑之后，绝不应该自动再弹一次确认框"));
            WPJ4_CHECK(h.readOnlyCalls.size() == 2);
            if (h.readOnlyCalls.size() == 2)
            {
                WPJ4_CHECK(h.readOnlyCalls[0] == true);
                WPJ4_CHECK(h.readOnlyCalls[1] == false);
            }
            // Stale 在新鲜度复核（d）就被拦下，连写前复核读（e）都没走到，端口
            // 完全没有被触碰。
            WPJ4_CHECK(h.rawPort->calls.empty());
            WPJ4_CHECK(h.rawPort->writeCalls.empty());
            // **B-1/B-2 裁决的另一半**：补丁继续以"待写入"状态原样留在 overlay
            // 里（AbortToStaged 不触碰 overlay），不会被静默丢弃，也不会被自动
            // 重跑掉——等用户下一次真正的编辑或点"应用"时才会被处理。
            WPJ4_CHECK(h.overlay.DiffBlocks().size() == 1);
        }

        // M-C11（review2-wpJ4.md B-1 回归，原审核探针 R2J4-B1）：用户在确认框里
        // 明确点了"否"（uiConfirmAnswer=false），且确认框开着期间发生过一次被
        // 拒绝的嵌套编辑——这次拒绝不应该让系统误以为"要重试"，用户已经给出的
        // 否定答案绝不能被系统自己的补跑机制无视、立刻又弹一次几乎相同的确认框
        // （已删除的 rerunRequested_ 机制曾经会这样做）。
        void TestRerunDoesNotRepromptAfterUserExplicitlyDenied()
        {
            Harness h;
            h.AttachProcess(216);
            h.controller.setUiConfirmSuppressed(false);
            h.EnsureWired();
            h.LoadBaseline(0x4D00, {0x00});
            WPJ4_CHECK(h.Stage(0x4D00, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.prompter->uiConfirmAnswer = false; // 用户将要点"否"。

            bool triggered = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (triggered)
                {
                    return;
                }
                triggered = true;
                // 确认框开着期间再编辑一次：命中 D3 的忙碌拒绝，不应该留下任何
                // 会影响这次确认结果的痕迹。
                h.controller.onEditCompleted();
            };

            h.controller.onEditCompleted();

            WPJ4_CHECK_NOTE(
                h.prompter->uiConfirmCallCount == 1,
                QStringLiteral("B-1 裁决：用户已经点过一次否，绝不应该被自动再问一次"));
            // 用户拒绝（UserCancelled）：确认阶段就中止，端口完全没被碰。
            WPJ4_CHECK(h.rawPort->calls.empty());
            WPJ4_CHECK(h.rawPort->writeCalls.empty());
            // UserCancelled 不丢补丁：仍然留在 overlay 里，等用户自己再决定。
            WPJ4_CHECK(h.overlay.DiffBlocks().size() == 1);
        }

        // M-C12（review2-wpJ4.md B-2/B-4 回归，原审核探针 R2J4-B4）：暂存模式下
        // 点"应用"（commitPendingNow）的确认框期间，嵌套触发一次被拒绝的
        // onEditCompleted；这次忙碌拒绝不应该留下任何残留标记——已删除的
        // rerunRequested_ 机制曾经只在 onEditCompleted 自己体内被消费，经由
        // commitPendingNow 触发的忙碌拒绝会让它永久卡在 true，污染下一次完全
        // 无关的正常编辑（多发一次意料之外的 Commit 尝试）。
        void TestBusyRejectionDoesNotPolluteNextIndependentEdit()
        {
            Harness h;
            h.AttachProcess(217);
            h.controller.setUiConfirmSuppressed(false);
            h.EnsureWired();
            h.LoadBaseline(0x4E00, std::vector<std::uint8_t>(0x200, 0x00));
            WPJ4_CHECK(h.Stage(0x4E00, {0x01}) == ksword::memwb::StageStatus::Ok);

            int busyCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy,
                [&](QString) { ++busyCount; });
            bool nestedTriggered = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (nestedTriggered)
                {
                    return;
                }
                nestedTriggered = true;
                // 模拟"点应用、确认框开着的时候又编辑了别的地址"：嵌套的
                // onEditCompleted() 必须被直接拒绝，不新建事务，不留下任何会
                // 影响"下一次独立编辑"的残留标记。
                h.controller.onEditCompleted();
            };

            // 这次 Apply 本身会因为上面嵌套编辑推进了内容代次，被 Core 的新鲜度
            // 复核判成 Stale（与 M-C8 同一现象，是既有的、完全正确的保护，不是
            // 本测试要验证的对象）。本测试不关心这次 Apply 自己的结果，只关心
            // 它结束之后系统状态是否干净，故意不编排任何端口脚本、不检查
            // outcome。
            h.controller.commitPendingNow();
            WPJ4_CHECK(nestedTriggered);
            WPJ4_CHECK(busyCount >= 1);

            // 之后做一次完全独立、不抑制重入的新编辑：B-2 的残留标记（已删除的
            // rerunRequested_）曾经会让这次编辑被污染出第二次 commitFinished。
            int finishedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            h.controller.setUiConfirmSuppressed(true);
            WPJ4_CHECK(h.Stage(0x4F00, {0x02}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
            h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x02}));
            h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
            h.controller.onEditCompleted();
            WPJ4_CHECK_NOTE(
                finishedCount == 1,
                QStringLiteral(
                    "B-2 裁决：忙碌拒绝不应该留下任何残留标记污染下一次毫不相关的正常编辑"));
        }

        // M-C14（review2-wpJ4.md C5 测试缺口补测）：canvasReadOnlyHook_/
        // commitSuspendHook_ 两个钩子都未设置时，CommitReadOnlyGuard 构造函数的
        // 判空防线（if (canvasHook_) {...}）必须真的生效——整个夹具里 Harness
        // 构造函数无条件设置了这两个钩子，这条防线此前从未被任何测试在"钩子真的
        // 没设置"的场景下驱动过一次 Commit。
        void TestCommitSucceedsWithoutReadOnlyHooks()
        {
            Harness h;
            h.AttachProcess(219);
            h.controller.setCanvasReadOnlyHook(nullptr);
            h.controller.setCommitSuspendHook(nullptr);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0x5F10, {0x00});
            WPJ4_CHECK(h.Stage(0x5F10, {0x0B}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x0B})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK_NOTE(
                attempt.report.outcome == ksword::memwb::CommitOutcome::Committed,
                QStringLiteral("两个钩子都未设置时不应该崩溃，提交仍应正常成功"));
            WPJ4_CHECK(h.readOnlyCalls.empty());
            WPJ4_CHECK(h.commitSuspendCalls.empty());
        }

        // M-C15（review2-wpJ4.md A10 白盒探针补测）：CommitReadOnlyGuard 的深度
        // 门禁（只在 depth 0→1/1→0 才触碰钩子）在当前公开 API 下永远不会被真正
        // 构造出深度>1 的场景（五个入口都在创建 Guard 之前自己先查过
        // commitDepth_），这条"重入感知"本身从未在深度>1 的真实场景下被验证过。
        // 直接白盒构造两层嵌套的 Guard，核对两个钩子确实只在最外层触发一次。
        void TestCommitReadOnlyGuardNestedDepthOnlyTriggersOnce()
        {
            int depth = 0;
            std::vector<bool> canvasCalls;
            std::vector<bool> suspendCalls;
            const ks::ui::WorkbenchWriteController::CanvasReadOnlyHook canvasHook =
                [&](bool v) { canvasCalls.push_back(v); };
            const ks::ui::WorkbenchWriteController::CommitSuspendHook suspendHook =
                [&](bool v) { suspendCalls.push_back(v); };
            {
                ks::ui::detail::CommitReadOnlyGuard outer(depth, canvasHook, suspendHook);
                WPJ4_CHECK(canvasCalls.size() == 1 && canvasCalls[0] == true);
                WPJ4_CHECK(suspendCalls.size() == 1 && suspendCalls[0] == true);
                {
                    // 真实构造第二层（当前公开 API 下永远不会发生）：验证深度
                    // 门禁本身在深度>1 时是否真的只触发一次。
                    ks::ui::detail::CommitReadOnlyGuard inner(depth, canvasHook, suspendHook);
                    WPJ4_CHECK_NOTE(
                        canvasCalls.size() == 1,
                        QStringLiteral("深度到 2 不应该再次触发 hook(true)"));
                }
                WPJ4_CHECK_NOTE(
                    canvasCalls.size() == 1,
                    QStringLiteral("内层退出到深度 1 不应该触发 hook(false)"));
            }
            WPJ4_CHECK(canvasCalls.size() == 2 && canvasCalls[1] == false);
            WPJ4_CHECK(suspendCalls.size() == 2 && suspendCalls[1] == false);
        }

        // M-C16（review2-wpJ4.md C9 测试缺口补测，同时验证"代次镶像→直接引用
        // target_->revisions()"这次主会话裁决的改动）：确认框弹出期间，发生一次
        // 纯重读（target_->requestReload()，只让来源代次 +1，会话身份七个字段
        // 原样不变，SameTarget 仍为真）——这是唯一能把"来源代次经由活引用被
        // transaction_ 感知到"这条链路和"身份变了"这条完全不同的判据分开验证的
        // 手段：此前所有 Stale 相关用例都走内容代次（BumpContent），没有一个走
        // 来源代次。
        void TestExternalReloadDuringConfirmCausesStaleViaLiveRevisions()
        {
            Harness h;
            h.AttachProcess(220);
            h.controller.setUiConfirmSuppressed(false);
            h.EnsureWired();
            h.LoadBaseline(0x5F20, {0x00});
            WPJ4_CHECK(h.Stage(0x5F20, {0x0C}) == ksword::memwb::StageStatus::Ok);

            int finishedCount = 0;
            ksword::memwb::CommitReport lastReport;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport report)
                {
                    ++finishedCount;
                    lastReport = report;
                });

            bool triggered = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (triggered)
                {
                    return;
                }
                triggered = true;
                h.target.requestReload();
            };

            h.controller.onEditCompleted();

            WPJ4_CHECK(finishedCount == 1);
            WPJ4_CHECK_NOTE(
                lastReport.outcome == ksword::memwb::CommitOutcome::Stale,
                QStringLiteral(
                    "target_->revisions() 必须是活引用：外部纯重读（不换身份）推进的来源"
                    "代次必须被 transaction_ 的新鲜度复核感知到，判 Stale"));
            WPJ4_CHECK(h.rawPort->calls.empty());
            WPJ4_CHECK(h.rawPort->writeCalls.empty());
            // Stale 不丢补丁：仍然留在 overlay 里。
            WPJ4_CHECK(h.overlay.DiffBlocks().size() == 1);
        }

        // M-C9（D3 修复新增）：commitPendingNow 在 commitDepth_ > 0 时直接返回
        // Busy，不嵌套执行、不触碰端口。
        void TestCommitPendingNowRejectedWhileBusy()
        {
            Harness h;
            h.AttachProcess(208);
            h.controller.setUiConfirmSuppressed(false);
            h.EnsureWired();
            h.LoadBaseline(0x4500, {0x00});
            WPJ4_CHECK(h.Stage(0x4500, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            int busyCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy,
                [&](QString) { ++busyCount; });

            ks::ui::CommitAttempt nestedAttempt;
            bool nestedAttemptCaptured = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (nestedAttemptCaptured)
                {
                    return;
                }
                nestedAttemptCaptured = true;
                nestedAttempt = h.controller.commitPendingNow();
            };

            h.controller.onEditCompleted();

            WPJ4_CHECK(nestedAttemptCaptured);
            WPJ4_CHECK_NOTE(
                nestedAttempt.status == ks::ui::CommitEntryStatus::Busy,
                QStringLiteral("commitPendingNow 在已有 Commit 进行时必须返回 Busy"));
            WPJ4_CHECK(busyCount >= 1);
        }

        // M-C10（review-wpJ4.md R6 测试缺口补测）：blocksWritten 大于快照块数时
        // 不应该越界访问/多记账——白盒直接调用 detail::HandleCommitReport，构造
        // 一个"虚报"更多 blocksWritten 的假报告（真实端口不会这样报，但这条防线
        // 本身必须被验证到，不能只是生产代码里"看起来对"）。
        void TestHandleCommitReportClampsBlocksWrittenToSnapshot()
        {
            Harness h;
            h.AttachProcess(209);
            h.EnsureWired();

            ksword::memwb::CommitReport fakeReport;
            fakeReport.outcome = ksword::memwb::CommitOutcome::Committed;
            fakeReport.blocksWritten = 5; // 故意报大于下面快照的数量。
            std::vector<ksword::memwb::DiffBlock> snapshot(2);
            snapshot[0].address = 0x1000;
            snapshot[0].before = {0x00};
            snapshot[0].after = {0x01};
            snapshot[1].address = 0x2000;
            snapshot[1].before = {0x00};
            snapshot[1].after = {0x02};

            std::vector<std::pair<std::uint64_t, std::uint64_t>> rereadCalls;
            const ks::ui::WorkbenchWriteController::RereadRangeFn callback =
                [&](std::uint64_t address, std::uint64_t length)
                {
                    rereadCalls.emplace_back(address, length);
                };

            ks::ui::detail::HandleCommitReport(
                &h.controller, &h.target, nullptr, callback, fakeReport, snapshot);

            // 不应该崩溃，且局部重读范围只覆盖这 2 块，不应该越界访问第 3~5 块。
            WPJ4_CHECK(rereadCalls.size() == 1);
            if (!rereadCalls.empty())
            {
                WPJ4_CHECK(rereadCalls[0].first == 0x1000);
                WPJ4_CHECK(rereadCalls[0].second == (0x2000 + 1 - 0x1000));
            }
        }

        // ---------------- S-wpJ4 补测（审核者追加，不在仓库夹具里） ----------------

        // S3：pendingPatchesChanged 的 (字节数, 块数) 两个参数顺序与取值都要对。杀 x21。
        void TestSuppPendingPatchesReportsBytesAndBlockCount()
        {
            Harness h;
            h.AttachProcess(641);
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                == ksword::memwb::ModeSwitchStatus::Switched);
            h.LoadBaseline(0xE400, std::vector<std::uint8_t>(0x110, 0x00));
            WPJ4_CHECK(h.Stage(0xE400, {0x01, 0x02, 0x03}) == ksword::memwb::StageStatus::Ok);
            WPJ4_CHECK(h.Stage(0xE500, {0x04}) == ksword::memwb::StageStatus::Ok);
            quint64 lastBytes = 0;
            quint64 lastBlocks = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                [&](quint64 bytes, quint64 blocks) { lastBytes = bytes; lastBlocks = blocks; });
            h.controller.onEditCompleted();
            WPJ4_CHECK_NOTE(
                lastBytes == 4 && lastBlocks == 2,
                QStringLiteral("pendingPatchesChanged 必须是 (4 字节, 2 块)"));
        }

        // S6：resolveModeSwitch(ApplyThenSwitch) 提交成功后要做完整 W4 收尾：
        // commitFinished 一次、局部重读、可撤销（journal 记账）、待写入清零通知。
        // 杀 x22/x23。
        void TestSuppApplyThenSwitchRunsW4Wrapup()
        {
            Harness h;
            h.AttachProcess(640);
            h.controller.setUiConfirmSuppressed(true);
            WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                == ksword::memwb::ModeSwitchStatus::Switched);
            h.LoadBaseline(0xE300, {0x00});
            WPJ4_CHECK(h.Stage(0xE300, {0x06}) == ksword::memwb::StageStatus::Ok);
            h.controller.onEditCompleted(); // 暂存模式：只暂存。
            WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                == ksword::memwb::ModeSwitchStatus::NeedsDecision);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x06})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            int finishedCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitFinished,
                [&](ksword::memwb::CommitReport) { ++finishedCount; });
            quint64 lastBytes = 99;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                [&](quint64 bytes, quint64 /*blocks*/) { lastBytes = bytes; });
            h.rereadCalls.clear();

            const ksword::memwb::ModeSwitchResult result =
                h.controller.resolveModeSwitch(ksword::memwb::ModeSwitchDecision::ApplyThenSwitch);
            WPJ4_CHECK(result.status == ksword::memwb::ModeSwitchStatus::Switched);
            WPJ4_CHECK_NOTE(
                finishedCount == 1,
                QStringLiteral("ApplyThenSwitch 提交成功后必须发一次 commitFinished"));
            WPJ4_CHECK_NOTE(
                h.rereadCalls.size() == 1,
                QStringLiteral("ApplyThenSwitch 提交成功后必须对落地块做一次局部重读"));
            WPJ4_CHECK_NOTE(
                h.controller.canUndo() == true,
                QStringLiteral("ApplyThenSwitch 落地的写入必须记进撤销历史"));
            WPJ4_CHECK_NOTE(
                lastBytes == 0,
                QStringLiteral("ApplyThenSwitch 提交后必须发 pendingPatchesChanged(0, 0)"));
        }
    }

    // 全部收尾 caller 都跑真实读/写/回读，再在结果或成员重读回调中同步销毁控制器。
    void TestCommitWrapupStopsAfterSynchronousDestruction()
    {
        for (int caller = 0; caller < 6; ++caller)
        {
            for (int phase = 0; phase < 2; ++phase)
            {
                Harness h;
                h.AttachProcess(330 + static_cast<std::uint32_t>(caller));
                auto owned = std::make_unique<ks::ui::WorkbenchWriteController>();
                auto* controller = owned.get();
                QPointer<ks::ui::WorkbenchWriteController> guarded(controller);
                controller->setTarget(&h.target);
                controller->setOverlay(&h.overlay);
                controller->setConfirmationSink(h.confirmations.get());
                controller->setAuditSink(&h.audit);
                controller->setUiConfirmSuppressed(true);
                controller->setIoPortFactory([&]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort>
                {
                    auto port = std::make_unique<MemwbIoTests::FakeMemoryIoPort>();
                    h.rawPort = port.get();
                    return port;
                });
                const auto mode = caller == 2 ? ksword::memwb::WriteMode::StagedThenApply
                                             : ksword::memwb::WriteMode::Immediate;
                WPJ4_CHECK(controller->requestModeSwitch(mode) == ksword::memwb::ModeSwitchStatus::Switched);
                h.LoadBaseline(0xA000, {0x00});
                if (caller != 5)
                {
                    WPJ4_CHECK(h.Stage(0xA000, {0x10}) == ksword::memwb::StageStatus::Ok);
                }
                h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x10})};
                h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

                // 撤销/重做先建立真实已落地历史，之后才挂删除结果槽。
                if (caller == 3 || caller == 4)
                {
                    WPJ4_CHECK(controller->commitPendingNow().report.outcome ==
                        ksword::memwb::CommitOutcome::Committed);
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x10}));
                    h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
                    h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
                    if (caller == 4)
                    {
                        WPJ4_CHECK(controller->undo() == ks::ui::CommitEntryStatus::Started);
                        WPJ4_CHECK(controller->canRedo());
                        h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x00}));
                        h.rawPort->script.push_back(MemwbIoTests::MakeOk({0x10}));
                        h.rawPort->writeScript.push_back(MemwbIoTests::MakeWriteOk(1));
                    }
                }
                int finished = 0;
                int pendingNotifications = 0;
                int availabilityNotifications = 0;
                int rereads = 0;
                int callbackAfterDelete = 0;
                bool historyReadyDuringSignal = false;
                QObject::connect(controller, &ks::ui::WorkbenchWriteController::commitFinished,
                    [&](ksword::memwb::CommitReport report)
                    {
                        ++finished;
                        WPJ4_CHECK(report.outcome == ksword::memwb::CommitOutcome::Committed);
                        historyReadyDuringSignal = caller == 3 ? controller->canRedo() : controller->canUndo();
                        if (phase == 0)
                        {
                            owned.reset();
                        }
                    });
                QObject::connect(controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                    [&](quint64, quint64) { ++pendingNotifications; });
                QObject::connect(controller, &ks::ui::WorkbenchWriteController::undoRedoAvailabilityChanged,
                    [&]() { ++availabilityNotifications; });
                controller->setRereadRangeCallback([&](std::uint64_t address, std::uint64_t length)
                {
                    ++rereads;
                    WPJ4_CHECK(address == 0xA000 && length == 1);
                    owned.reset();
                    // 函数对象本身来自被删除的成员；调用必须持有独立副本才能继续使用捕获。
                    WPJ4_CHECK(guarded.isNull());
                    ++callbackAfterDelete;
                });
                switch (caller)
                {
                case 0:
                    controller->onEditCompleted();
                    break;
                case 1:
                    WPJ4_CHECK(controller->commitPendingNow().report.outcome ==
                        ksword::memwb::CommitOutcome::Committed);
                    break;
                case 2:
                    WPJ4_CHECK(controller->requestModeSwitch(ksword::memwb::WriteMode::Immediate) ==
                        ksword::memwb::ModeSwitchStatus::NeedsDecision);
                    WPJ4_CHECK(controller->resolveModeSwitch(ksword::memwb::ModeSwitchDecision::ApplyThenSwitch)
                        .status == ksword::memwb::ModeSwitchStatus::Switched);
                    break;
                case 3:
                    WPJ4_CHECK(controller->undo() == ks::ui::CommitEntryStatus::Started);
                    break;
                case 4:
                    WPJ4_CHECK(controller->redo() == ks::ui::CommitEntryStatus::Started);
                    break;
                default:
                    WPJ4_CHECK(controller->beginPendingStage(0xA000, QByteArray(1, '\x10')) == 0);
                    break;
                }
                WPJ4_CHECK(guarded.isNull() && !owned);
                WPJ4_CHECK(finished == 1 && historyReadyDuringSignal);
                WPJ4_CHECK(pendingNotifications == 0 && availabilityNotifications == 0);
                WPJ4_CHECK(rereads == phase && callbackAfterDelete == phase);
                WPJ4_CHECK(!h.overlay.HasPendingPatches());
            }
        }
    }

    // 直接驱动真实报告分发器的失败/脏暂存/重读标记，逐个验证同步删除切断后续通知。
    void TestReportSignalsStopAfterSynchronousDestruction()
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            Harness h;
            h.AttachProcess(340);
            auto owned = std::make_unique<ks::ui::WorkbenchWriteController>();
            auto* controller = owned.get();
            QPointer<ks::ui::WorkbenchWriteController> guarded(controller);
            int failed = 0;
            int dirty = 0;
            int reload = 0;
            int reread = 0;
            QObject::connect(controller, &ks::ui::WorkbenchWriteController::commitFailed,
                [&](ksword::memwb::CommitReport)
                {
                    ++failed;
                    if (phase == 0)
                    {
                        owned.reset();
                    }
                });
            QObject::connect(controller, &ks::ui::WorkbenchWriteController::scratchAreaDirtyReported,
                [&]()
                {
                    ++dirty;
                    if (phase == 1)
                    {
                        owned.reset();
                    }
                });
            QObject::connect(&h.target, &ks::ui::WorkbenchTarget::sessionChanged,
                [&](quint32)
                {
                    ++reload;
                    if (phase == 2)
                    {
                        owned.reset();
                    }
                });
            ksword::memwb::CommitReport report;
            report.outcome = ksword::memwb::CommitOutcome::WriteFailed;
            report.blocksWritten = 1;
            report.scratchAreaDirty = true;
            report.needsReread = true;
            ksword::memwb::DiffBlock block;
            block.address = 0xA100;
            block.before = {0x00};
            block.after = {0x10};
            const ks::ui::WorkbenchWriteController::RereadRangeFn callback =
                [&](std::uint64_t, std::uint64_t) { ++reread; };
            ks::ui::detail::HandleCommitReport(controller, &h.target, nullptr, callback, report, {block});
            WPJ4_CHECK(guarded.isNull() && !owned);
            WPJ4_CHECK(failed == 1);
            WPJ4_CHECK(dirty == (phase == 0 ? 0 : 1));
            WPJ4_CHECK(reload == (phase == 2 ? 1 : 0));
            WPJ4_CHECK(reread == 0);
        }
    }

    void RunCommitTests()
    {
        TestCommitWrapupStopsAfterSynchronousDestruction();
        TestReportSignalsStopAfterSynchronousDestruction();
        TestSuppPendingPatchesReportsBytesAndBlockCount();
        TestSuppApplyThenSwitchRunsW4Wrapup();
        TestImmediateCommitSucceeds();
        TestExceptionDuringConfirmStillRestoresReadOnly();
        TestStagedModeDefersCommit();
        TestOnlyLandedBlocksAreRecorded();
        TestScratchAreaDirtyReported();
        TestCommitFailedOnlyOnRealFailure();
        TestNoChangeDoesNotFail();
        TestNestedEditDuringConfirmIsRejectedNotNested();
        TestCommitPendingNowRejectedWhileBusy();
        TestHandleCommitReportClampsBlocksWrittenToSnapshot();
        TestRerunDoesNotRepromptAfterUserExplicitlyDenied();
        TestBusyRejectionDoesNotPolluteNextIndependentEdit();
        TestCommitSucceedsWithoutReadOnlyHooks();
        TestCommitReadOnlyGuardNestedDepthOnlyTriggersOnce();
        TestExternalReloadDuringConfirmCausesStaleViaLiveRevisions();
    }
}
