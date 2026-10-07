// wpJ4_tests.Confirmations.cpp
// 作用：确认策略表与模式切换三选一——
//   - 进程范围 + 立即写入：suppressed=true 时 ConfirmUi 零调用，但审计仍然记一条
//     UiConfirmSuppressed（"跳过 UI 确认不跳过审计"）。
//   - 内核/物理范围 + 立即写入：suppressed=false 时 ConfirmUi 恰好一次。
//   - 强制同意（ConfirmApproval）永远弹，不受 setUiConfirmSuppressed(true) 影响。
//   - requestModeSwitch/resolveModeSwitch 三选一四分支（NeedsDecision ->
//     ApplyThenSwitch/DiscardThenSwitch/Cancel，以及无补丁时直接 Switched）。

#include "wpJ4_common.h"

#include <QPointer>

namespace wpj4_test
{
    namespace
    {
        // M-F1：进程范围 + 立即写入 + suppressed=true：ConfirmUi 零调用，审计仍
        // 记一条 UiConfirmSuppressed；CommitStarted/CommitFinished 至少各一条。
        void TestProcessImmediateSuppressedStillAudits()
        {
            Harness h;
            h.AttachProcess(500);
            const ksword::memwb::ConfirmDecision decision = h.policy.Decide(
                ksword::memwb::WriteMode::Immediate,
                ksword::memwb::Scope::ProcessVirtual,
                ksword::memwb::Channel::UserMode,
                false);
            WPJ4_CHECK_NOTE(
                decision.suppressed == true,
                QStringLiteral("进程范围+立即写入按确认策略表应当不弹"));
            h.controller.setUiConfirmSuppressed(decision.suppressed);
            h.EnsureWired();

            h.LoadBaseline(0xD000, {0x00});
            WPJ4_CHECK(h.Stage(0xD000, {0x01}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x01})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            h.controller.onEditCompleted();
            WPJ4_CHECK(h.prompter->uiConfirmCallCount == 0);
            WPJ4_CHECK(h.audit.CountOf(ksword::memwb::AuditEvent::UiConfirmSuppressed) >= 1);
            WPJ4_CHECK(h.audit.CountOf(ksword::memwb::AuditEvent::CommitStarted) >= 1);
            WPJ4_CHECK(h.audit.CountOf(ksword::memwb::AuditEvent::CommitFinished) >= 1);
        }

        // M-F2：内核范围 + 立即写入：确认策略表要求弹一次。
        void TestKernelImmediateAsksOnce()
        {
            Harness h;
            h.SwitchToKernel(ksword::memwb::Channel::StandardDriver);
            const ksword::memwb::ConfirmDecision decision = h.policy.Decide(
                ksword::memwb::WriteMode::Immediate,
                ksword::memwb::Scope::KernelVirtual,
                ksword::memwb::Channel::StandardDriver,
                false);
            WPJ4_CHECK_NOTE(
                decision.suppressed == false,
                QStringLiteral("内核范围+立即写入按确认策略表应当弹一次"));
            h.controller.setUiConfirmSuppressed(decision.suppressed);
            // SetCurrentContext 是装配层（WorkbenchHexPane/MemoryWorkbenchView）的
            // 职责："每次会话/模式变化后"刷新一次，供 ConfirmUi 拼"通道·范围"前缀；
            // WorkbenchWriteController 自己不调用它，这里手动模拟装配层已经做过
            // 这一步。
            h.confirmations->SetCurrentContext(
                ksword::memwb::WriteMode::Immediate,
                ksword::memwb::Scope::KernelVirtual,
                ksword::memwb::Channel::StandardDriver);
            h.EnsureWired();

            // 内核地址必须落在内核半区（kKernelSplit 之上）才会走
            // IsKernelVirtualAddress 判据；这里用一个明显落在用户半区的高位地址
            // 以外的普通内核态虚拟地址做普通端口写入（不经分步事务——那条分支
            // 由 wpJ5/其它用例覆盖，这里只关心确认策略）。实际上
            // MemoryIoByteStore 的内核分步事务分支只在 StandardDriver+
            // KernelVirtual+真正落在内核半区的地址才触发；这里故意用一个不在
            // 内核半区的地址，让它走普通端口分支，保持脚本简单。
            h.LoadBaseline(0x1000, {0x00});
            WPJ4_CHECK(h.Stage(0x1000, {0x02}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x02})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            h.controller.onEditCompleted();
            WPJ4_CHECK(h.prompter->uiConfirmCallCount == 1);
            WPJ4_CHECK(h.prompter->lastUiConfirmScope == ksword::memwb::Scope::KernelVirtual);
        }

        // M-F3：强制同意（ConfirmApproval）永远弹，即使 setUiConfirmSuppressed(true)。
        void TestApprovalAlwaysAsksRegardlessOfSuppression()
        {
            Harness h;
            h.AttachProcess(501);
            h.controller.setUiConfirmSuppressed(true); // 普通确认全部跳过。
            h.EnsureWired();

            h.LoadBaseline(0xD100, {0x00});
            WPJ4_CHECK(h.Stage(0xD100, {0x03}) == ksword::memwb::StageStatus::Ok);
            // 批准框返回后必须再次读取 before，随后才是写入后的回读。
            h.rawPort->script = {
                MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x03})};
            // 第一次写入被后端要求显式同意；同意之后重试同一次写入。
            h.rawPort->writeScript = {
                MemwbIoTests::MakeWriteNeedsApproval(), MemwbIoTests::MakeWriteOk(1)};
            h.prompter->approvalAnswer = ksword::memwb::ApprovalAnswer::ThisBlockOnly;

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ4_CHECK(h.prompter->uiConfirmCallCount == 0);
            WPJ4_CHECK_NOTE(
                h.prompter->approvalCallCount == 1,
                QStringLiteral("强制同意不受 setUiConfirmSuppressed(true) 影响"));
            WPJ4_CHECK(h.rawPort->writeCalls.size() == 2);
            if (h.rawPort->writeCalls.size() == 2)
            {
                WPJ4_CHECK(h.rawPort->writeCalls[0].approved == false);
                WPJ4_CHECK(h.rawPort->writeCalls[1].approved == true);
            }
        }

        // M-F4：requestModeSwitch/resolveModeSwitch 三选一四分支。
        void TestModeSwitchThreeWayBranches()
        {
            // 分支 1：没有未提交补丁时直接 Switched。
            {
                Harness h;
                h.AttachProcess(502);
                h.EnsureWired();
                WPJ4_CHECK(
                    h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                    == ksword::memwb::ModeSwitchStatus::Switched);
                WPJ4_CHECK(h.controller.mode() == ksword::memwb::WriteMode::StagedThenApply);
            }

            // 分支 2：有未提交补丁时 NeedsDecision，分别验证 Cancel 与
            // DiscardThenSwitch 两条路径（先切到 Staged 模式让补丁真的"待提交"，
            // 不会被立即模式顺手消耗掉）。
            {
                Harness h;
                h.AttachProcess(504);
                h.EnsureWired();
                WPJ4_CHECK(
                    h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                    == ksword::memwb::ModeSwitchStatus::Switched);
                h.LoadBaseline(0xD300, {0x00});
                WPJ4_CHECK(h.Stage(0xD300, {0x05}) == ksword::memwb::StageStatus::Ok);
                h.controller.onEditCompleted(); // Staged 模式：只暂存，补丁仍待提交。

                WPJ4_CHECK(
                    h.controller.requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                    == ksword::memwb::ModeSwitchStatus::NeedsDecision);

                // 选 Cancel：模式与补丁都不变。
                const std::uint64_t contentBefore = h.target.capture().rev.content;
                int pendingNotifications = 0;
                quint64 pendingBytes = 1;
                quint64 pendingBlocks = 1;
                QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                    [&](quint64 bytes, quint64 blocks) {
                        ++pendingNotifications;
                        pendingBytes = bytes;
                        pendingBlocks = blocks;
                    });
                ksword::memwb::ModeSwitchResult cancelResult =
                    h.controller.resolveModeSwitch(ksword::memwb::ModeSwitchDecision::Cancel);
                WPJ4_CHECK(cancelResult.status == ksword::memwb::ModeSwitchStatus::Cancelled);
                WPJ4_CHECK(h.controller.mode() == ksword::memwb::WriteMode::StagedThenApply);
                WPJ4_CHECK(h.overlay.HasPendingPatches() == true);
                WPJ4_CHECK(h.target.capture().rev.content == contentBefore);
                WPJ4_CHECK(pendingNotifications == 0);

                // 选 DiscardThenSwitch：补丁被丢弃，模式切换成功。
                WPJ4_CHECK(
                    h.controller.requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                    == ksword::memwb::ModeSwitchStatus::NeedsDecision);
                ksword::memwb::ModeSwitchResult discardResult = h.controller.resolveModeSwitch(
                    ksword::memwb::ModeSwitchDecision::DiscardThenSwitch);
                WPJ4_CHECK(discardResult.status == ksword::memwb::ModeSwitchStatus::Switched);
                WPJ4_CHECK(h.controller.mode() == ksword::memwb::WriteMode::Immediate);
                WPJ4_CHECK(h.overlay.HasPendingPatches() == false);
                WPJ4_CHECK(h.target.capture().rev.content == contentBefore + 1);
                WPJ4_CHECK(pendingNotifications == 1 && pendingBytes == 0 && pendingBlocks == 0);
                WPJ4_CHECK(h.rawPort->writeCalls.empty());

                // 迟到的相同决定没有待决请求，不能再次推进内容代次或通知。
                WPJ4_CHECK(h.controller.resolveModeSwitch(ksword::memwb::ModeSwitchDecision::DiscardThenSwitch)
                    .status == ksword::memwb::ModeSwitchStatus::NoPendingSwitch);
                WPJ4_CHECK(h.target.capture().rev.content == contentBefore + 1 && pendingNotifications == 1);

                // 确认前补丁已被其它入口清空：仍可切换模式，但没有新内容变化可通知。
                WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                    == ksword::memwb::ModeSwitchStatus::Switched);
                WPJ4_CHECK(h.Stage(0xD300, {0x07}) == ksword::memwb::StageStatus::Ok);
                WPJ4_CHECK(h.controller.requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                    == ksword::memwb::ModeSwitchStatus::NeedsDecision);
                h.overlay.DiscardAll();
                WPJ4_CHECK(h.controller.resolveModeSwitch(ksword::memwb::ModeSwitchDecision::DiscardThenSwitch)
                    .status == ksword::memwb::ModeSwitchStatus::Switched);
                WPJ4_CHECK(h.target.capture().rev.content == contentBefore + 1 && pendingNotifications == 1);
            }

            // 分支 3：ApplyThenSwitch——先提交补丁成功后再切换，提交期间同样要
            // 经过 canvasReadOnlyHook 的只读守卫。
            {
                Harness h;
                h.AttachProcess(505);
                h.controller.setUiConfirmSuppressed(true);
                WPJ4_CHECK(
                    h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                    == ksword::memwb::ModeSwitchStatus::Switched);
                h.LoadBaseline(0xD400, {0x00});
                WPJ4_CHECK(h.Stage(0xD400, {0x06}) == ksword::memwb::StageStatus::Ok);
                h.controller.onEditCompleted();

                WPJ4_CHECK(
                    h.controller.requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                    == ksword::memwb::ModeSwitchStatus::NeedsDecision);
                h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x06})};
                h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

                ksword::memwb::ModeSwitchResult applyResult =
                    h.controller.resolveModeSwitch(ksword::memwb::ModeSwitchDecision::ApplyThenSwitch);
                WPJ4_CHECK(applyResult.status == ksword::memwb::ModeSwitchStatus::Switched);
                WPJ4_CHECK(applyResult.commitReport.has_value());
                if (applyResult.commitReport.has_value())
                {
                    WPJ4_CHECK(
                        applyResult.commitReport->outcome == ksword::memwb::CommitOutcome::Committed);
                }
                WPJ4_CHECK(h.controller.mode() == ksword::memwb::WriteMode::Immediate);
                WPJ4_CHECK(h.readOnlyCalls.size() == 2);
                if (h.readOnlyCalls.size() == 2)
                {
                    WPJ4_CHECK(h.readOnlyCalls[0] == true);
                    WPJ4_CHECK(h.readOnlyCalls[1] == false);
                }
            }
        }
        // 丢弃通知同步删除控制器后，仅返回栈上结果，不再访问宿主成员。
        void TestModeDiscardSignalCanDestroyController()
        {
            Harness h;
            h.AttachProcess(506);
            auto owned = std::make_unique<ks::ui::WorkbenchWriteController>();
            auto* controller = owned.get();
            const QPointer<ks::ui::WorkbenchWriteController> guard(controller);
            controller->setTarget(&h.target);
            controller->setOverlay(&h.overlay);
            controller->setConfirmationSink(h.confirmations.get());
            controller->setAuditSink(&h.audit);
            controller->setIoPortFactory([]() -> std::unique_ptr<ksword::memwb::IMemoryIoPort> {
                return std::make_unique<MemwbIoTests::FakeMemoryIoPort>();
            });
            WPJ4_CHECK(controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                == ksword::memwb::ModeSwitchStatus::Switched);
            h.LoadBaseline(0xD500, {0x00});
            WPJ4_CHECK(h.Stage(0xD500, {0x08}) == ksword::memwb::StageStatus::Ok);
            WPJ4_CHECK(controller->requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                == ksword::memwb::ModeSwitchStatus::NeedsDecision);
            const std::uint64_t contentBefore = h.target.capture().rev.content;
            int notifications = 0;
            QObject::connect(controller, &ks::ui::WorkbenchWriteController::pendingPatchesChanged,
                [&](quint64 bytes, quint64 blocks) {
                    ++notifications;
                    WPJ4_CHECK(bytes == 0 && blocks == 0);
                    owned.reset();
                });
            WPJ4_CHECK(controller->resolveModeSwitch(ksword::memwb::ModeSwitchDecision::DiscardThenSwitch)
                .status == ksword::memwb::ModeSwitchStatus::Switched);
            WPJ4_CHECK(!guard && !owned && notifications == 1);
            WPJ4_CHECK(!h.overlay.HasPendingPatches() && h.target.capture().rev.content == contentBefore + 1);
        }

        // M-F5（D3 修复新增）：resolveModeSwitch 的 ApplyThenSwitch 分支在
        // commitDepth_ > 0 时必须直接返回 Busy，不嵌套执行自己的那次 Commit。
        // 复用 onEditCompleted 自己的确认框作为"正在进行中的 Commit"窗口——忙
        // 检查在本类内部排在"是否真的有待决切换请求"之前，所以不需要先摆出一个
        // 合法的 NeedsDecision 状态，直接在确认框开着的时候调用它即可验证。
        void TestResolveModeSwitchApplyRejectedWhileBusy()
        {
            Harness h;
            h.AttachProcess(506);
            h.controller.setUiConfirmSuppressed(false);
            h.EnsureWired();
            h.LoadBaseline(0xD500, {0x00});
            WPJ4_CHECK(h.Stage(0xD500, {0x07}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x07})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            int busyCount = 0;
            QObject::connect(&h.controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy,
                [&](QString) { ++busyCount; });
            ksword::memwb::ModeSwitchResult nestedResult;
            bool nestedCalled = false;
            h.prompter->duringUiConfirm = [&]()
            {
                if (nestedCalled)
                {
                    return;
                }
                nestedCalled = true;
                nestedResult = h.controller.resolveModeSwitch(
                    ksword::memwb::ModeSwitchDecision::ApplyThenSwitch);
            };

            h.controller.onEditCompleted();

            WPJ4_CHECK(nestedCalled);
            WPJ4_CHECK_NOTE(
                nestedResult.status == ksword::memwb::ModeSwitchStatus::Busy,
                QStringLiteral("resolveModeSwitch(ApplyThenSwitch) 在已有 Commit 进行时必须返回 Busy"));
            WPJ4_CHECK(busyCount >= 1);
        }

        // M-F6（review-wpJ4.md R17 测试缺口补测）：resolveModeSwitch 的
        // ApplyThenSwitch 分支必须在提交前刷新一次会话（DDMA 代次）——用
        // FakeWorkbenchServices::DdmaGenerationCallCount() 断言 target_->
        // session() 确实被多调用了一次，不是被跳过。
        void TestApplyThenSwitchRefreshesDdmaGeneration()
        {
            Harness h;
            h.AttachProcess(508);
            WPJ4_CHECK(h.target.requestChannel(ksword::memwb::Channel::Ddma));
            h.controller.setUiConfirmSuppressed(true);
            WPJ4_CHECK(
                h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply)
                == ksword::memwb::ModeSwitchStatus::Switched);
            h.LoadBaseline(0xD700, {0x00});
            WPJ4_CHECK(h.Stage(0xD700, {0x09}) == ksword::memwb::StageStatus::Ok);
            h.controller.onEditCompleted(); // 暂存模式：只暂存，不提交。

            WPJ4_CHECK(
                h.controller.requestModeSwitch(ksword::memwb::WriteMode::Immediate)
                == ksword::memwb::ModeSwitchStatus::NeedsDecision);

            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x09})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            const int before = h.servicesRaw->DdmaGenerationCallCount();
            const ksword::memwb::ModeSwitchResult result =
                h.controller.resolveModeSwitch(ksword::memwb::ModeSwitchDecision::ApplyThenSwitch);
            WPJ4_CHECK(result.status == ksword::memwb::ModeSwitchStatus::Switched);
            WPJ4_CHECK_NOTE(
                h.servicesRaw->DdmaGenerationCallCount() > before,
                QStringLiteral("ApplyThenSwitch 必须在提交前刷新一次 DDMA 代次（target_->session()）"));
        }
        // M-F7（review2-wpJ4.md C10 测试缺口补测）：commitPendingNow 必须在提交
        // 前刷新一次会话（DDMA 代次）——与上面的 M-F6（resolveModeSwitch 的
        // ApplyThenSwitch 分支）同一手法，但 M-F6 只覆盖了那一个分支，
        // commitPendingNow 自己的 target_->session() 这一行完全没有对应测试。
        void TestCommitPendingNowRefreshesDdmaGeneration()
        {
            Harness h;
            h.AttachProcess(509);
            WPJ4_CHECK(h.target.requestChannel(ksword::memwb::Channel::Ddma));
            h.controller.setUiConfirmSuppressed(true);
            h.EnsureWired();
            h.LoadBaseline(0xD800, {0x00});
            WPJ4_CHECK(h.Stage(0xD800, {0x0A}) == ksword::memwb::StageStatus::Ok);
            h.rawPort->script = {MemwbIoTests::MakeOk({0x00}), MemwbIoTests::MakeOk({0x0A})};
            h.rawPort->writeScript = {MemwbIoTests::MakeWriteOk(1)};

            const int before = h.servicesRaw->DdmaGenerationCallCount();
            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ4_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ4_CHECK_NOTE(
                h.servicesRaw->DdmaGenerationCallCount() > before,
                QStringLiteral("commitPendingNow 必须在提交前刷新一次 DDMA 代次（target_->session()）"));
        }
    }

    void RunConfirmationTests()
    {
        TestProcessImmediateSuppressedStillAudits();
        TestKernelImmediateAsksOnce();
        TestApprovalAlwaysAsksRegardlessOfSuppression();
        TestModeSwitchThreeWayBranches();
        TestModeDiscardSignalCanDestroyController();
        TestResolveModeSwitchApplyRejectedWhileBusy();
        TestApplyThenSwitchRefreshesDdmaGeneration();
        TestCommitPendingNowRefreshesDdmaGeneration();
    }
}
