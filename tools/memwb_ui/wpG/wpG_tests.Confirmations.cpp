#include "wpG_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchMessages.h"

// ============================================================
// wpG_tests.Confirmations.cpp
// 作用：验证 WorkbenchConfirmations 四条硬判据：
//   A) ConfirmApproval 永远弹，且不受 SetUiConfirmSuppressed 影响；
//   B) "本次其余块也强制"只在 blocksTotal-blockIndex>1 时出现；
//   C) 写入模式三选一的四个落点（应用成功切换/应用失败不切换/丢弃切换/取消不切换）；
//   D) ConfirmUi 的"本次运行不再询问"只在用户同意且勾选时才记进 MemoryWritePolicy。
// 用真实的 MemoryWriteTransaction + FakeByteStore 驱动，而不是只摸 WorkbenchConfirmations
// 自己的接口方法——这样才是 WP-G 实际会被调用的方式，不是自说自话的单元测试。
// ============================================================

namespace wpg_test
{
    using ksword::memwb::ApprovalAnswer;
    using ksword::memwb::ApprovalRequest;
    using ksword::memwb::CommitOutcome;
    using ksword::memwb::Channel;
    using ksword::memwb::MemoryDiffOverlay;
    using ksword::memwb::MemoryTargetSession;
    using ksword::memwb::MemoryWritePolicy;
    using ksword::memwb::MemoryWriteTransaction;
    using ksword::memwb::ModeSwitchDecision;
    using ksword::memwb::ModeSwitchStatus;
    using ksword::memwb::Scope;
    using ksword::memwb::SessionRevisions;
    using ksword::memwb::UiConfirmRequest;
    using ksword::memwb::WriteMode;

    namespace
    {
        // MakeSession：拼一个自洽的进程范围会话，供多个测试复用。
        MemoryTargetSession MakeSession()
        {
            MemoryTargetSession session;
            session.scope = Scope::ProcessVirtual;
            session.pid = 4242;
            session.processCreateTime100ns = 1;
            session.attachGeneration = 1;
            session.channel = Channel::UserMode;
            session.addressBits = 64;
            return session;
        }

        // LoadMatchingBaseline：把 overlay 的基线灌成与 store.readBytes 完全一致，
        // 这样 Stage 产生的差异块"原值"才等于 Commit 写前复核 (e) 步读到的真实字节，
        // 否则每次 Commit 都会在复核阶段被误判成 TargetChanged。
        void LoadMatchingBaseline(MemoryDiffOverlay& overlay, const FakeByteStore& store)
        {
            const std::vector<std::uint8_t> validMask(store.readBytes.size(), 1);
            overlay.LoadBaseline("wpg-confirm-test", store.baseAddress, store.readBytes, validMask);
        }
    }

    namespace
    {
        // RunApprovalAlwaysAsksTest：A) 不受抑制影响。
        void RunApprovalAlwaysAsksTest()
        {
            MemoryDiffOverlay overlay;
            const MemoryTargetSession session = MakeSession();
            SessionRevisions revisions;
            FakeByteStore store;
            store.baseAddress = 0x1000;
            store.readBytes.assign(16, 0xAA);
            FakeConfirmPrompter* prompterRaw = nullptr;
            auto prompter = std::make_unique<FakeConfirmPrompter>();
            prompterRaw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
            confirmations.SetCurrentContext(WriteMode::Immediate, Scope::ProcessVirtual, Channel::UserMode);
            NullAuditSink audit;

            MemoryWriteTransaction tx(overlay, session, revisions, store, confirmations, audit, WriteMode::Immediate);
            tx.SetUiConfirmSuppressed(true); // 显式打开抑制：ConfirmUi 不该被问，ConfirmApproval 必须照样问。

            LoadMatchingBaseline(overlay, store);
            tx.Stage(0x1000, {0x11, 0x22});
            revisions.BumpContent();

            store.nextWriteNeedsApproval = true;
            prompterRaw->approvalAnswers.push_back(ApprovalAnswer::ThisBlockOnly);

            const auto report = tx.Commit();
            WPG_CHECK_NOTE(report.outcome == CommitOutcome::Committed,
                QStringLiteral("outcome=%1 text=%2").arg(static_cast<int>(report.outcome)).arg(
                    QString::fromStdString(report.failureText)));
            WPG_CHECK(prompterRaw->approvalCalls == 1);
            WPG_CHECK(prompterRaw->uiConfirmCalls == 0); // 抑制生效：普通确认没被问过。
        }

        // RunRestOfBatchThresholdTest：B) blocksTotal-blockIndex>1 才出现"本次其余"。
        void RunRestOfBatchThresholdTest()
        {
            auto prompter = std::make_unique<FakeConfirmPrompter>();
            FakeConfirmPrompter* raw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));

            ApprovalRequest lastOfOne;
            lastOfOne.blockIndex = 0;
            lastOfOne.blocksTotal = 1; // 1-0=1，不大于 1 -> 不提供
            raw->approvalAnswers.push_back(ApprovalAnswer::ThisBlockOnly);
            confirmations.ConfirmApproval(lastOfOne);
            WPG_CHECK(!raw->lastOfferRestOfBatch);

            ApprovalRequest middleOfThree;
            middleOfThree.blockIndex = 0;
            middleOfThree.blocksTotal = 3; // 3-0=3>1 -> 提供
            raw->approvalAnswers.push_back(ApprovalAnswer::RestOfBatch);
            confirmations.ConfirmApproval(middleOfThree);
            WPG_CHECK(raw->lastOfferRestOfBatch);

            ApprovalRequest lastOfTwo;
            lastOfTwo.blockIndex = 1;
            lastOfTwo.blocksTotal = 2; // 2-1=1，不大于 1 -> 不提供（即使不是唯一一块）
            raw->approvalAnswers.push_back(ApprovalAnswer::ThisBlockOnly);
            confirmations.ConfirmApproval(lastOfTwo);
            WPG_CHECK(!raw->lastOfferRestOfBatch);
        }

        // RunModeSwitchFourBranchesTest：C) 四个落点。
        void RunModeSwitchFourBranchesTest()
        {
            auto makeTx = [](
                MemoryDiffOverlay& overlay, const MemoryTargetSession& session, SessionRevisions& revisions,
                FakeByteStore& store, ks::ui::WorkbenchConfirmations& confirmations, NullAuditSink& audit) {
                return std::make_unique<MemoryWriteTransaction>(
                    overlay, session, revisions, store, confirmations, audit, WriteMode::StagedThenApply);
            };

            // (a) 应用并切换，提交成功 -> Switched。
            {
                MemoryDiffOverlay overlay;
                const MemoryTargetSession session = MakeSession();
                SessionRevisions revisions;
                FakeByteStore store;
                store.baseAddress = 0x2000;
                store.readBytes.assign(16, 0x00);
                auto prompter = std::make_unique<FakeConfirmPrompter>();
                FakeConfirmPrompter* raw = prompter.get();
                ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
                confirmations.SetCurrentContext(WriteMode::StagedThenApply, Scope::ProcessVirtual, Channel::UserMode);
                NullAuditSink audit;
                auto tx = makeTx(overlay, session, revisions, store, confirmations, audit);

                LoadMatchingBaseline(overlay, store);
                tx->Stage(0x2000, {0x01});
                revisions.BumpContent();
                const auto setStatus = tx->SetMode(WriteMode::Immediate);
                WPG_CHECK(setStatus == ModeSwitchStatus::NeedsDecision);

                // Resolve 内部会走一次真正的 Commit：当前模式仍是 StagedThenApply，
                // 按确认策略表"暂存→点应用"每次都弹 ConfirmUi，这里安排用户同意。
                raw->uiConfirmAnswers.push_back(true);
                raw->modeSwitchAnswers.push_back(ModeSwitchDecision::ApplyThenSwitch);
                const auto decision = confirmations.PromptModeSwitch(
                    tx->Mode(), WriteMode::Immediate, overlay.PendingByteCount(), 1);
                const auto result = tx->ResolveModeSwitch(decision);
                WPG_CHECK_NOTE(result.status == ModeSwitchStatus::Switched,
                    QStringLiteral("status=%1").arg(static_cast<int>(result.status)));
                WPG_CHECK(tx->Mode() == WriteMode::Immediate);
            }

            // (b) 应用并切换，提交失败 -> ApplyFailed，模式不变。
            {
                MemoryDiffOverlay overlay;
                const MemoryTargetSession session = MakeSession();
                SessionRevisions revisions;
                FakeByteStore store;
                store.baseAddress = 0x2000;
                store.readBytes.assign(16, 0x00);
                auto prompter = std::make_unique<FakeConfirmPrompter>();
                FakeConfirmPrompter* raw = prompter.get();
                ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
                NullAuditSink audit;
                auto tx = makeTx(overlay, session, revisions, store, confirmations, audit);

                LoadMatchingBaseline(overlay, store);
                tx->Stage(0x2000, {0x01});
                revisions.BumpContent();
                store.failNextWrite = true; // 让这次 Commit 在写入阶段失败
                const auto setStatusB = tx->SetMode(WriteMode::Immediate);
                WPG_CHECK(setStatusB == ModeSwitchStatus::NeedsDecision);

                raw->uiConfirmAnswers.push_back(true); // 先过普通确认，才能真正走到写入阶段的失败
                raw->modeSwitchAnswers.push_back(ModeSwitchDecision::ApplyThenSwitch);
                const auto decision = confirmations.PromptModeSwitch(
                    tx->Mode(), WriteMode::Immediate, overlay.PendingByteCount(), 1);
                const auto result = tx->ResolveModeSwitch(decision);
                WPG_CHECK_NOTE(result.status == ModeSwitchStatus::ApplyFailed,
                    QStringLiteral("status=%1").arg(static_cast<int>(result.status)));
                WPG_CHECK(tx->Mode() == WriteMode::StagedThenApply); // 失败不切换
                WPG_CHECK(result.commitReport.has_value());
            }

            // (c) 丢弃并切换 -> Switched，目标内存不受影响。
            {
                MemoryDiffOverlay overlay;
                const MemoryTargetSession session = MakeSession();
                SessionRevisions revisions;
                FakeByteStore store;
                store.baseAddress = 0x2000;
                store.readBytes.assign(16, 0x00);
                auto prompter = std::make_unique<FakeConfirmPrompter>();
                FakeConfirmPrompter* raw = prompter.get();
                ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
                NullAuditSink audit;
                auto tx = makeTx(overlay, session, revisions, store, confirmations, audit);

                LoadMatchingBaseline(overlay, store);
                tx->Stage(0x2000, {0x01});
                revisions.BumpContent();
                const auto setStatusC = tx->SetMode(WriteMode::Immediate);
                WPG_CHECK(setStatusC == ModeSwitchStatus::NeedsDecision);

                raw->modeSwitchAnswers.push_back(ModeSwitchDecision::DiscardThenSwitch);
                const auto decision = confirmations.PromptModeSwitch(
                    tx->Mode(), WriteMode::Immediate, overlay.PendingByteCount(), 1);
                const auto result = tx->ResolveModeSwitch(decision);
                WPG_CHECK(result.status == ModeSwitchStatus::Switched);
                WPG_CHECK(tx->Mode() == WriteMode::Immediate);
                WPG_CHECK(store.writeCalls == 0); // 丢弃：从未真正写过目标
            }

            // (d) 取消 -> Cancelled，模式不变。
            {
                MemoryDiffOverlay overlay;
                const MemoryTargetSession session = MakeSession();
                SessionRevisions revisions;
                FakeByteStore store;
                store.baseAddress = 0x2000;
                store.readBytes.assign(16, 0x00);
                auto prompter = std::make_unique<FakeConfirmPrompter>();
                FakeConfirmPrompter* raw = prompter.get();
                ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
                NullAuditSink audit;
                auto tx = makeTx(overlay, session, revisions, store, confirmations, audit);

                LoadMatchingBaseline(overlay, store);
                tx->Stage(0x2000, {0x01});
                revisions.BumpContent();
                const auto setStatusD = tx->SetMode(WriteMode::Immediate);
                WPG_CHECK(setStatusD == ModeSwitchStatus::NeedsDecision);

                raw->modeSwitchAnswers.push_back(ModeSwitchDecision::Cancel);
                const auto decision = confirmations.PromptModeSwitch(
                    tx->Mode(), WriteMode::Immediate, overlay.PendingByteCount(), 1);
                const auto result = tx->ResolveModeSwitch(decision);
                WPG_CHECK(result.status == ModeSwitchStatus::Cancelled);
                WPG_CHECK(tx->Mode() == WriteMode::StagedThenApply);
            }
        }

        // RunLeaveWithPendingTest（WP-J6 Wave 3 新增，修复缺陷 3）：
        // WorkbenchConfirmations::PromptLeaveWithPending 必须原样转发给注入的
        // IConfirmPrompter（薄转发，不加逻辑），且与 PromptModeSwitch 是两条
        // 完全独立的调用——同一次调用不应该让 modeSwitchCalls 也增加（反过来
        // 也一样），证明这不是误用 PromptModeSwitch 伪装出来的。另外核对
        // WorkbenchMessages 新增的三个文案函数产出的正文真的带上了传入的
        // 字节数/块数/原因短句，不是写死的占位字符串。
        void RunLeaveWithPendingTest()
        {
            auto prompter = std::make_unique<FakeConfirmPrompter>();
            FakeConfirmPrompter* raw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));

            raw->leaveWithPendingAnswers.push_back(ModeSwitchDecision::ApplyThenSwitch);
            const auto decision = confirmations.PromptLeaveWithPending(128ULL, 3ULL, QStringLiteral("切换范围"));
            WPG_CHECK(decision == ModeSwitchDecision::ApplyThenSwitch);
            WPG_CHECK(raw->leaveWithPendingCalls == 1);
            WPG_CHECK(raw->modeSwitchCalls == 0); // 两条独立调用，不是同一回事。
            WPG_CHECK(raw->lastLeaveWithPendingBytes == 128ULL);
            WPG_CHECK(raw->lastLeaveWithPendingBlocks == 3ULL);
            WPG_CHECK(raw->lastLeaveWithPendingReasonText == QStringLiteral("切换范围"));

            // 队列空时的安全默认值：Cancel（不隐式应用也不隐式丢弃）。
            const auto fallback = confirmations.PromptLeaveWithPending(1ULL, 1ULL, QStringLiteral("切换通道"));
            WPG_CHECK(fallback == ModeSwitchDecision::Cancel);

            // 文案函数：正文必须真的带上传入的参数，不是写死的占位字符串；且与
            // ModeSwitchDialogTitle/Body 的文案明显不同（修复缺陷 3 的核心——
            // 不能再用"切换写入模式"这套文案）。
            const QString title = ks::ui::workbench_messages::LeaveWithPendingDialogTitle();
            WPG_CHECK(title != ks::ui::workbench_messages::ModeSwitchDialogTitle());
            const QString body = ks::ui::workbench_messages::LeaveWithPendingDialogBody(
                256ULL, 4ULL, QStringLiteral("切换通道"));
            WPG_CHECK(body.contains(QStringLiteral("256")));
            WPG_CHECK(body.contains(QStringLiteral("4")));
            WPG_CHECK(body.contains(QStringLiteral("切换通道")));
            WPG_CHECK(!ks::ui::workbench_messages::LeaveWithPendingApplyButtonText().isEmpty());
            WPG_CHECK(!ks::ui::workbench_messages::LeaveWithPendingDiscardButtonText().isEmpty());
            WPG_CHECK(!ks::ui::workbench_messages::LeaveWithPendingCancelButtonText().isEmpty());
        }

        // RunDontAskAgainTest：D) 只在用户同意且勾选时才记入策略。
        void RunDontAskAgainTest()
        {
            MemoryWritePolicy policy;
            auto prompter = std::make_unique<FakeConfirmPrompter>();
            FakeConfirmPrompter* raw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, &policy, std::move(prompter));
            confirmations.SetCurrentContext(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver);

            UiConfirmRequest request;
            request.targetIdentity = "kernel";
            request.blocksTotal = 1;
            request.bytesTotal = 4;

            // 第一次：同意但不勾选 -> 不记忆，下次仍应提供勾选框。
            raw->uiConfirmAnswers.push_back(true);
            raw->uiConfirmDontAskAgain.push_back(false);
            confirmations.ConfirmUi(request);
            WPG_CHECK(raw->lastOfferDontAskAgain); // 内核范围+立即写入属于"首次危险立即"，应提供勾选框
            WPG_CHECK(!policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver));

            // 第二次：同意且勾选 -> 记忆生效。
            raw->uiConfirmAnswers.push_back(true);
            raw->uiConfirmDontAskAgain.push_back(true);
            confirmations.ConfirmUi(request);
            WPG_CHECK(policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver));
        }

        // RejectButCheckedPrompter：最坏情况的弹框执行器——用户拒绝，却（错误地）同时报告
        // 勾了"本次运行不再询问"；并记录收到的 offerDontAskAgain。FakeConfirmPrompter 只在
        // 同意时才报告勾选（与真实 QMessageBox 实现一致），所以抓不到适配层自己的
        // "只在同意时才记忆"判断——复核时的变异重放证明了这一点。
        class RejectButCheckedPrompter final : public ks::ui::IConfirmPrompter
        {
        public:
            bool lastOffer = false;
            bool PromptUiConfirm(
                const UiConfirmRequest&, Scope, Channel, const QString&, bool offerDontAskAgain,
                bool& dontAskAgainChecked) override
            {
                lastOffer = offerDontAskAgain;
                dontAskAgainChecked = true;
                return false;
            }
            ApprovalAnswer PromptApproval(const ApprovalRequest&, Scope, Channel, const QString&, bool) override
            {
                return ApprovalAnswer::Deny;
            }
            ModeSwitchDecision PromptModeSwitch(WriteMode, WriteMode, std::uint64_t, std::uint64_t) override
            {
                return ModeSwitchDecision::Cancel;
            }
        };

        // RunDontAskAgainRejectedAndOfferTest：D2) ①用户拒绝时即使执行器报告勾了框也不得记忆；
        // ②勾选框是否提供完全由策略（Decide().offerDontAskAgain）决定——内核范围+立即写入提供，
        // 进程范围+立即写入不提供，适配层必须原样透传，不得自己改写。
        void RunDontAskAgainRejectedAndOfferTest()
        {
            UiConfirmRequest request;
            request.targetIdentity = "kernel";
            request.blocksTotal = 1;
            request.bytesTotal = 4;
            {
                MemoryWritePolicy policy;
                auto prompter = std::make_unique<RejectButCheckedPrompter>();
                RejectButCheckedPrompter* raw = prompter.get();
                ks::ui::WorkbenchConfirmations confirmations(nullptr, &policy, std::move(prompter));
                confirmations.SetCurrentContext(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver);
                WPG_CHECK(!confirmations.ConfirmUi(request));
                WPG_CHECK(raw->lastOffer);
                WPG_CHECK_NOTE(
                    !policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver),
                    QStringLiteral("用户拒绝时，即使执行器报告勾了框，也不得记入策略"));
            }
            {
                MemoryWritePolicy policy;
                auto prompter = std::make_unique<RejectButCheckedPrompter>();
                RejectButCheckedPrompter* raw = prompter.get();
                ks::ui::WorkbenchConfirmations confirmations(nullptr, &policy, std::move(prompter));
                confirmations.SetCurrentContext(WriteMode::Immediate, Scope::ProcessVirtual, Channel::UserMode);
                confirmations.ConfirmUi(request);
                WPG_CHECK_NOTE(
                    !raw->lastOffer,
                    QStringLiteral("进程范围+立即写入：策略不提供勾选框，适配层不得自己改成提供"));
            }
        }
    }

    void RunConfirmationsTests()
    {
        RunApprovalAlwaysAsksTest();
        RunRestOfBatchThresholdTest();
        RunModeSwitchFourBranchesTest();
        RunLeaveWithPendingTest();
        RunDontAskAgainTest();
        RunDontAskAgainRejectedAndOfferTest();
    }
}
