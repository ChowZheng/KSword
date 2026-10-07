#include "wpG_common.h"

// ============================================================
// wpG_tests.Gaps.cpp
// 作用：审核报告 review-wpG.md 第 5 节"建议补的测试"落地版——补上原来零覆盖的
// WorkbenchActions / WorkbenchSettings / WorkbenchStringWriteDialog /
// WriteModeSwitch，并把 B1-B14、S1/S2/S4/S5/S6/S7/S9/S10 的修复钉成回归断言。
// 本文件里的断言反映"修好之后"的期望行为（不是像审核报告草稿那样先写成
// DEFECT 再等修复——修复已经落地，这里直接断言正确行为，修复被撤回时这些
// 断言会变红）。
// ============================================================

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchActions.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchMessages.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSessionBar.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSettings.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchStringWriteDialog.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WriteModeSwitch.h"
#include "../../../Ksword5.1/Ksword5.1/UI/ThemeStatusRole.h"
#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include "../../../shared/evidence/memory_workbench/MemoryWritePolicy.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <cstddef>
#include <vector>

namespace wpg_test
{
    using ksword::memwb::ApprovalAnswer;
    using ksword::memwb::ApprovalRequest;
    using ksword::memwb::Channel;
    using ksword::memwb::CommitOutcome;
    using ksword::memwb::CommitReport;
    using ksword::memwb::GateReason;
    using ksword::memwb::GateVerdict;
    using ksword::memwb::IoReadStatus;
    using ksword::memwb::MemoryWritePolicy;
    using ksword::memwb::ModeSwitchDecision;
    using ksword::memwb::ModeSwitchStatus;
    using ksword::memwb::Scope;
    using ksword::memwb::SessionError;
    using ksword::memwb::StageStatus;
    using ksword::memwb::UiConfirmRequest;
    using ksword::memwb::WriteMode;

    namespace
    {
        // ---- T1（M01/M02）：通道记忆要"每范围一份、非默认值也记住" ----
        void RunChannelMemoryTest()
        {
            ks::ui::WorkbenchSessionBar bar;
            bar.setChannel(Channel::Hvm);                        // 进程范围显式选 HVM（非默认）
            WPG_CHECK(bar.currentChannel() == Channel::Hvm);      // 立即生效，不是悄悄落回 R3
            bar.setScope(Scope::KernelVirtual);
            WPG_CHECK(bar.currentChannel() == Channel::StandardDriver);
            bar.setChannel(Channel::Ddma);                       // 内核范围显式选 DDMA（非默认）
            bar.setScope(Scope::Physical);
            WPG_CHECK(bar.currentChannel() == Channel::StandardDriver);   // 物理范围没选过 -> 默认
            bar.setScope(Scope::ProcessVirtual);
            WPG_CHECK(bar.currentChannel() == Channel::Hvm);
            bar.setScope(Scope::KernelVirtual);
            WPG_CHECK(bar.currentChannel() == Channel::Ddma);
        }

        // ---- T2（M04/M05/M28）："可用但未知"的 HVM 不得置灰但必须提示 ----
        void RunUnknownVerdictTest()
        {
            ks::ui::WorkbenchSessionBar bar;
            bar.show();
            std::array<GateVerdict, 4> verdicts{};
            verdicts[0] = GateVerdict{ true, GateReason::None };
            verdicts[1] = GateVerdict{ true, GateReason::None };
            verdicts[2] = GateVerdict{ true, GateReason::ProbeNotDone };
            verdicts[3] = GateVerdict{ false, GateReason::SessionNotReady };
            bar.setChannelVerdicts(verdicts);
            bar.setChannel(Channel::Hvm);
            WPG_CHECK(bar.channelSegmented()->isSegmentEnabled(2));
            WPG_CHECK(!bar.channelWarningText().isEmpty());
            WPG_CHECK(!bar.channelSegmented()->isSegmentEnabled(3));
            WPG_CHECK(bar.channelSegmented()->segmentToolTip(3).contains(QStringLiteral("会话")));
            verdicts[2] = GateVerdict{ true, GateReason::None };
            bar.setChannelVerdicts(verdicts);
            WPG_CHECK(bar.channelWarningText().isEmpty());
        }

        // ---- T3（M07/M25/M29）：点 × 后"干净"报告不得让红 chip 复活；⚠ 标记 ----
        void RunScratchAckTest()
        {
            ks::ui::WorkbenchStatusBar statusBar(std::make_unique<FakeDiagnosticsHost>());
            statusBar.show();
            int acks = 0;
            QObject::connect(&statusBar, &ks::ui::WorkbenchStatusBar::scratchDirtyAcknowledged, [&acks]() { ++acks; });
            statusBar.reportScratchAreaDirty(true);
            statusBar.acknowledgeScratchAreaDirty();
            WPG_CHECK(acks == 1);
            statusBar.reportScratchAreaDirty(false);
            WPG_CHECK(!statusBar.isScratchAreaDirtyChipVisible());
            statusBar.setReadResultText(QStringLiteral("已读 8/16 字节"), true);
            WPG_CHECK(statusBar.summaryText().contains(QStringLiteral("⚠")));
            statusBar.setReadResultText(QStringLiteral("已读 16/16 字节"), false);
            WPG_CHECK(!statusBar.summaryText().contains(QStringLiteral("⚠")));
            WPG_CHECK(statusBar.summaryText().contains(QStringLiteral("16/16")));
        }

        // ---- T3b（B8，已修）：从未显示过的状态条，读回仍应如实 ----
        void RunHiddenGetterTest()
        {
            ks::ui::WorkbenchStatusBar statusBar(std::make_unique<FakeDiagnosticsHost>());
            statusBar.setDrawerExpanded(true);
            statusBar.reportScratchAreaDirty(true);
            WPG_CHECK(statusBar.isDrawerExpanded());
            WPG_CHECK(statusBar.isScratchAreaDirtyChipVisible());
            statusBar.setReadModifyWriteWindow(true);
            WPG_CHECK(statusBar.isReadModifyWriteWindowChipVisible());

            ks::ui::WorkbenchSessionBar bar;   // 同一类 B8：从未 show() 过的会话条
            std::array<GateVerdict, 4> allBad{};
            for (GateVerdict& v : allBad) { v = GateVerdict{ false, GateReason::DriverNotLoaded }; }
            bar.setChannelVerdicts(allBad);
            WPG_CHECK(!bar.channelWarningText().isEmpty());
        }

        // ---- T4（M09/M10，及 B9）：CommitReportSummary / Translate 的内容语义 ----
        void RunCommitSummaryTest()
        {
            CommitReport partial;
            partial.outcome = CommitOutcome::WriteFailed;
            partial.blocksTotal = 3;
            partial.blocksWritten = 2;
            partial.bytesWritten = 8;
            partial.failureText = "boom";
            const QString text = ks::ui::workbench_messages::CommitReportSummary(partial);
            WPG_CHECK(text.contains(QStringLiteral("8")) && text.contains(QStringLiteral("2")));
            WPG_CHECK(text.contains(QStringLiteral("boom")));
            // 非 Committed 的部分写入必须走"已写入 N 字节"这句措辞（与 Committed 分支
            // 专用的"共 N 字节"分开），否则两个分支的条件被合并后这里测不出来。
            WPG_CHECK_NOTE(text.contains(QStringLiteral("已写入")), text);
            WPG_CHECK(!ks::ui::workbench_messages::Translate(CommitOutcome::Committed).contains(QStringLiteral("失败")));
            WPG_CHECK(ks::ui::workbench_messages::Translate(CommitOutcome::WriteFailed).contains(QStringLiteral("失败")));
            WPG_CHECK(ks::ui::workbench_messages::Translate(CommitOutcome::UserCancelled).contains(QStringLiteral("取消")));

            CommitReport committed;                                  // Committed 分支不得把"已写入"重复两次
            committed.outcome = CommitOutcome::Committed;
            committed.blocksWritten = 3;
            committed.bytesWritten = 96;
            const QString committedText = ks::ui::workbench_messages::CommitReportSummary(committed);
            WPG_CHECK(committedText.contains(QStringLiteral("96")) && committedText.contains(QStringLiteral("3")));
            const int firstOccurrence = committedText.indexOf(QStringLiteral("已写入"));
            const int secondOccurrence = firstOccurrence < 0
                ? -1
                : committedText.indexOf(QStringLiteral("已写入"), firstOccurrence + 1);
            WPG_CHECK_NOTE(firstOccurrence >= 0 && secondOccurrence < 0, committedText);

            CommitReport mismatch;                                   // B9：目标上留着 4 字节必须告知
            mismatch.outcome = CommitOutcome::VerifyMismatch;
            mismatch.blocksTotal = 3;
            mismatch.bytesWritten = 4;
            WPG_CHECK(ks::ui::workbench_messages::CommitReportSummary(mismatch).contains(QStringLiteral("4")));
        }

        // ---- T5（M27）："本次其余块"边界对：(index,total) ----
        void RunRestOfBatchPairsTest()
        {
            auto prompter = std::make_unique<FakeConfirmPrompter>();
            FakeConfirmPrompter* raw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
            struct Pair { std::uint64_t index; std::uint64_t total; bool expectOffer; };
            const Pair pairs[] = { {0, 1, false}, {0, 2, true}, {1, 2, false}, {0, 3, true}, {1, 3, true}, {2, 3, false} };
            for (const Pair& pair : pairs)
            {
                ApprovalRequest request;
                request.blockIndex = pair.index;
                request.blocksTotal = pair.total;
                confirmations.ConfirmApproval(request);
                WPG_CHECK(raw->lastOfferRestOfBatch == pair.expectOffer);
            }
        }

        // ---- T6（M12/M14/M26/M41，及 B2）：真实 QMessageBox；探针自带定时器，随
        // 作用域销毁，避免残留定时器误关下一个框 ----
        struct ModalProbe
        {
            QString defaultText;
            QString escapeText;
            QString bodyText;
            QStringList buttonTexts;
            bool hasCheckBox = false;
            bool checkBoxChecked = false;
            bool seen = false;
            QTimer inspectTimer;
            QTimer guardTimer;
        };

        void ArmModalProbe(ModalProbe* probe, int key)
        {
            probe->inspectTimer.setSingleShot(true);
            probe->guardTimer.setSingleShot(true);
            QObject::connect(&probe->inspectTimer, &QTimer::timeout, [probe, key]() {
                QMessageBox* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (box == nullptr) { return; }
                probe->seen = true;
                probe->defaultText = box->defaultButton() != nullptr ? box->defaultButton()->text() : QString();
                probe->escapeText = box->escapeButton() != nullptr ? box->escapeButton()->text() : QString();
                probe->bodyText = box->text();
                for (QAbstractButton* button : box->buttons()) { probe->buttonTexts << button->text(); }
                probe->hasCheckBox = box->checkBox() != nullptr;
                probe->checkBoxChecked = box->checkBox() != nullptr && box->checkBox()->isChecked();
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
                QApplication::sendEvent(box, &press);
            });
            QObject::connect(&probe->guardTimer, &QTimer::timeout, []() {   // 兜底：没弹出时别挂死
                for (QWidget* widget : QApplication::topLevelWidgets())
                {
                    if (qobject_cast<QMessageBox*>(widget) != nullptr && widget->isVisible()) { widget->close(); }
                }
            });
            probe->inspectTimer.start(250);
            probe->guardTimer.start(1500);
        }

        void RunRealDialogsTest()
        {
            for (const int key : { static_cast<int>(Qt::Key_Escape), static_cast<int>(Qt::Key_Return) })
            {
                ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr);
                ModalProbe modeProbe;                                 // 三选一：默认与 Esc 都是"取消"
                ArmModalProbe(&modeProbe, key);
                const ModeSwitchDecision decision =
                    confirmations.PromptModeSwitch(WriteMode::StagedThenApply, WriteMode::Immediate, 12, 3);
                WPG_CHECK(modeProbe.seen);
                WPG_CHECK(modeProbe.defaultText == QStringLiteral("取消"));
                WPG_CHECK(modeProbe.escapeText == QStringLiteral("取消"));
                WPG_CHECK(modeProbe.buttonTexts.size() == 3);
                WPG_CHECK(decision == ModeSwitchDecision::Cancel);

                ApprovalRequest approval;                             // 强制同意：默认取消，Esc/Enter 都 Deny
                approval.targetIdentity = "memwb-target/1|scope=0|pid=4242|base=0x1000|len=16";
                approval.blockIndex = 0;
                approval.blocksTotal = 3;
                approval.address = 0x1000;
                approval.length = 4;
                confirmations.SetCurrentContext(WriteMode::Immediate, Scope::ProcessVirtual, Channel::UserMode);
                confirmations.SetTargetDescription(QStringLiteral("chrome.exe · PID 4242 · x64"));
                ModalProbe approvalProbe;
                ArmModalProbe(&approvalProbe, key);
                const ApprovalAnswer answer = confirmations.ConfirmApproval(approval);
                WPG_CHECK(approvalProbe.seen);
                WPG_CHECK(approvalProbe.defaultText == QStringLiteral("取消"));
                WPG_CHECK(approvalProbe.buttonTexts.size() == 3);     // offerRestOfBatch 时共三个按钮
                WPG_CHECK(answer == ApprovalAnswer::Deny);
                // B2：正文必须是人话（通道/范围/目标描述），绝不出现原始 IdentityKey 的
                // "|scope="/"|pid=" 这种机器键分隔片段。
                WPG_CHECK_NOTE(approvalProbe.bodyText.contains(QStringLiteral("R3")), approvalProbe.bodyText);
                WPG_CHECK_NOTE(approvalProbe.bodyText.contains(QStringLiteral("进程")), approvalProbe.bodyText);
                WPG_CHECK_NOTE(approvalProbe.bodyText.contains(QStringLiteral("chrome.exe")), approvalProbe.bodyText);
                WPG_CHECK_NOTE(!approvalProbe.bodyText.contains(QStringLiteral("|pid=")), approvalProbe.bodyText);
                WPG_CHECK_NOTE(!approvalProbe.bodyText.contains(QStringLiteral("|scope=")), approvalProbe.bodyText);

                MemoryWritePolicy policy;                             // 普通确认：默认取消；内核+立即带复选框、默认不勾
                ks::ui::WorkbenchConfirmations withPolicy(nullptr, &policy);
                withPolicy.SetCurrentContext(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver);
                withPolicy.SetTargetDescription(QString());          // 空描述必须退回"当前目标"，不是空字符串
                UiConfirmRequest request;
                request.targetIdentity = "kernel";
                request.blocksTotal = 1;
                request.bytesTotal = 4;
                ModalProbe uiProbe;
                ArmModalProbe(&uiProbe, key);
                const bool agreed = withPolicy.ConfirmUi(request);
                WPG_CHECK(uiProbe.seen);
                WPG_CHECK(uiProbe.defaultText == QStringLiteral("取消"));
                WPG_CHECK(uiProbe.hasCheckBox && !uiProbe.checkBoxChecked);
                WPG_CHECK(!agreed);
                WPG_CHECK(!policy.IsRemembered(Scope::KernelVirtual, Channel::StandardDriver));
                WPG_CHECK_NOTE(uiProbe.bodyText.contains(QStringLiteral("R0")), uiProbe.bodyText);
                WPG_CHECK_NOTE(uiProbe.bodyText.contains(QStringLiteral("内核")), uiProbe.bodyText);
                WPG_CHECK_NOTE(uiProbe.bodyText.contains(QStringLiteral("当前目标")), uiProbe.bodyText);
                WPG_CHECK_NOTE(!uiProbe.bodyText.contains(QStringLiteral("kernel")), uiProbe.bodyText);
            }
        }

        // ---- B2 补充：WorkbenchConfirmations 把 scope/channel/目标描述原样转发给
        // 弹框实现（用假执行器核对转发参数，不依赖真实 QMessageBox）----
        void RunTargetDescriptionForwardingTest()
        {
            auto prompter = std::make_unique<FakeConfirmPrompter>();
            FakeConfirmPrompter* raw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
            confirmations.SetCurrentContext(WriteMode::Immediate, Scope::Physical, Channel::Ddma);
            confirmations.SetTargetDescription(QStringLiteral("物理内存窗口"));

            UiConfirmRequest uiRequest;
            confirmations.ConfirmUi(uiRequest);
            WPG_CHECK(raw->lastUiConfirmScope == Scope::Physical);
            WPG_CHECK(raw->lastUiConfirmChannel == Channel::Ddma);
            WPG_CHECK(raw->lastUiConfirmTargetDescription == QStringLiteral("物理内存窗口"));

            ApprovalRequest approvalRequest;
            confirmations.ConfirmApproval(approvalRequest);
            WPG_CHECK(raw->lastApprovalScope == Scope::Physical);
            WPG_CHECK(raw->lastApprovalChannel == Channel::Ddma);
            WPG_CHECK(raw->lastApprovalTargetDescription == QStringLiteral("物理内存窗口"));

            confirmations.SetCurrentContext(WriteMode::Immediate, Scope::KernelVirtual, Channel::StandardDriver);
            confirmations.ConfirmUi(uiRequest);
            WPG_CHECK(raw->lastUiConfirmScope == Scope::KernelVirtual);   // 换了范围/通道之后立刻跟着变
            WPG_CHECK(raw->lastUiConfirmChannel == Channel::StandardDriver);
        }

        // ---- B5：ChannelMemory 的读出/灌入接口 ----
        void RunChannelMemoryAccessorsTest()
        {
            ks::ui::WorkbenchSessionBar bar;
            WPG_CHECK(bar.rememberedChannel(Scope::ProcessVirtual) == Channel::UserMode);   // 默认值
            WPG_CHECK(bar.rememberedChannel(Scope::KernelVirtual) == Channel::StandardDriver);
            bar.setScope(Scope::KernelVirtual);
            bar.setChannel(Channel::Hvm);
            WPG_CHECK(bar.rememberedChannel(Scope::KernelVirtual) == Channel::Hvm);

            // restoreChannelMemory：灌入装配层从持久化读回的值，DDMA（3）必须被
            // Core 的 Restore 清洗成默认值，不是原样接受。
            bar.restoreChannelMemory(Scope::Physical, 3U);            // 3=Ddma，必须被清洗
            WPG_CHECK(bar.rememberedChannel(Scope::Physical) != Channel::Ddma);
            bar.restoreChannelMemory(Scope::Physical, 2U);            // 2=Hvm，合法值原样采用
            WPG_CHECK(bar.rememberedChannel(Scope::Physical) == Channel::Hvm);
            bar.setScope(Scope::Physical);                            // 灌完记忆后真正切过去，显示要跟上
            WPG_CHECK(bar.currentChannel() == Channel::Hvm);
        }

        // ---- S5：setSession 组合入口——rememberAsUserChoice 控制是否写记忆，且
        // 记忆写的是"新范围"而不是调用前的旧范围 ----
        void RunSetSessionTest()
        {
            ks::ui::WorkbenchSessionBar bar;
            bar.setSession(Scope::KernelVirtual, Channel::Hvm, true);   // 用户显式选择：记忆生效
            WPG_CHECK(bar.currentScope() == Scope::KernelVirtual);
            WPG_CHECK(bar.currentChannel() == Channel::Hvm);
            WPG_CHECK(bar.rememberedChannel(Scope::KernelVirtual) == Channel::Hvm);

            bar.setSession(Scope::Physical, Channel::Ddma, false);      // 程序化强制通道：不记忆
            WPG_CHECK(bar.currentChannel() == Channel::Ddma);
            WPG_CHECK(bar.rememberedChannel(Scope::Physical) != Channel::Ddma);   // 没被当成"上次使用"记住

            bar.setScope(Scope::ProcessVirtual);                       // 其余范围的记忆不受影响
            WPG_CHECK(bar.rememberedChannel(Scope::KernelVirtual) == Channel::Hvm);
        }

        // ---- T14（M37）：三选一转发的参数（记录式执行器；FakeConfirmPrompter 忽略这些参数） ----
        class RecordingPrompter final : public ks::ui::IConfirmPrompter
        {
        public:
            WriteMode lastFrom = WriteMode::Immediate;
            WriteMode lastTo = WriteMode::Immediate;
            std::uint64_t lastBytes = 0;
            std::uint64_t lastBlocks = 0;
            bool PromptUiConfirm(
                const UiConfirmRequest&, Scope, Channel, const QString&, bool, bool& dontAskAgainChecked) override
            {
                dontAskAgainChecked = false;
                return false;
            }
            ApprovalAnswer PromptApproval(const ApprovalRequest&, Scope, Channel, const QString&, bool) override
            {
                return ApprovalAnswer::Deny;
            }
            ModeSwitchDecision PromptModeSwitch(WriteMode from, WriteMode to, std::uint64_t bytes, std::uint64_t blocks) override
            {
                lastFrom = from; lastTo = to; lastBytes = bytes; lastBlocks = blocks;
                return ModeSwitchDecision::Cancel;
            }
        };

        void RunModeSwitchForwardTest()
        {
            auto prompter = std::make_unique<RecordingPrompter>();
            RecordingPrompter* raw = prompter.get();
            ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, std::move(prompter));
            confirmations.PromptModeSwitch(WriteMode::StagedThenApply, WriteMode::Immediate, 12, 3);
            WPG_CHECK(raw->lastFrom == WriteMode::StagedThenApply && raw->lastTo == WriteMode::Immediate);
            WPG_CHECK(raw->lastBytes == 12 && raw->lastBlocks == 3);
        }

        // ---- T15（M38/M39）：回写绝不能再发请求信号 ----
        void RunWriteBackSilentTest()
        {
            ks::ui::WorkbenchSessionBar bar;
            bar.show();
            int scopeSignals = 0;
            int channelSignals = 0;
            int modeSignals = 0;
            QObject::connect(&bar, &ks::ui::WorkbenchSessionBar::scopeRequested, [&scopeSignals](Scope) { ++scopeSignals; });
            QObject::connect(&bar, &ks::ui::WorkbenchSessionBar::channelRequested, [&channelSignals](Channel) { ++channelSignals; });
            QObject::connect(&bar, &ks::ui::WorkbenchSessionBar::modeRequested, [&modeSignals](WriteMode) { ++modeSignals; });
            bar.setScope(Scope::KernelVirtual);
            bar.setChannel(Channel::Hvm);
            bar.setChannel(Channel::Ddma);
            bar.setScope(Scope::Physical);
            bar.setScope(Scope::ProcessVirtual);
            bar.setWriteMode(WriteMode::StagedThenApply);
            bar.setSession(Scope::KernelVirtual, Channel::StandardDriver, true);   // S5 新接口同样不得发信号
            WPG_CHECK(scopeSignals == 0 && channelSignals == 0 && modeSignals == 0);
        }

        // ---- T16（M32）：目标 chip 文字色（palette 断言，主题色现取） ----
        void RunChipColorTest()
        {
            ks::ui::WorkbenchSessionBar bar;
            bar.show();
            QToolButton* chip = nullptr;
            for (QToolButton* button : bar.findChildren<QToolButton*>())
            {
                // 目标 chip = 带文字的 QToolButton，但写入模式胶囊的两个半边现在也带文字（即时/暂存），要排除。
                if (!button->text().isEmpty() && qobject_cast<ks::ui::WriteModeSwitch*>(button->parentWidget()) == nullptr) { chip = button; }
            }
            WPG_CHECK(chip != nullptr);
            if (chip == nullptr) { return; }
            bar.setTargetInfo(true, QStringLiteral("a.exe"), 7, 64, true);
            WPG_CHECK(chip->palette().color(QPalette::ButtonText) == KswordTheme::TextPrimaryColor());
            bar.setTargetInfo(false, QString(), 0, 64, false);
            WPG_CHECK(chip->palette().color(QPalette::ButtonText) == KswordTheme::ErrorColor());
            bar.setScope(Scope::KernelVirtual);                       // B3：内核范围是中性色，不是错误色
            WPG_CHECK(chip->palette().color(QPalette::ButtonText) == KswordTheme::TextSecondaryColor());
        }
    }

    void RunGapTests()
    {
        RunChannelMemoryTest();
        RunUnknownVerdictTest();
        RunScratchAckTest();
        RunHiddenGetterTest();
        RunCommitSummaryTest();
        RunRestOfBatchPairsTest();
        RunRealDialogsTest();
        RunTargetDescriptionForwardingTest();
        RunChannelMemoryAccessorsTest();
        RunSetSessionTest();
        RunModeSwitchForwardTest();
        RunWriteBackSilentTest();
        RunChipColorTest();
    }
}
