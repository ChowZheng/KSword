#include "WorkbenchConfirmations.h"

// ============================================================
// WorkbenchConfirmations.cpp
// 作用：见头文件。QMessageBoxConfirmPrompter 是默认的生产实现，只用到 QtWidgets
// 自带的 QMessageBox/QCheckBox/QPushButton，没有牵连主程序其它重依赖类，因此
// 可以安全地作为本文件的默认值，不必像 WorkbenchStatusBar 的诊断抽屉那样拆接口
// 注入——这里的接口注入纯粹是为了让夹具不必真的弹出并等待模态框。
// ============================================================

#include "WorkbenchMessages.h"

#include "../../theme.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

namespace ks::ui
{
    using ksword::memwb::ApprovalAnswer;
    using ksword::memwb::ApprovalRequest;
    using ksword::memwb::Channel;
    using ksword::memwb::ModeSwitchDecision;
    using ksword::memwb::Scope;
    using ksword::memwb::UiConfirmRequest;
    using ksword::memwb::WriteMode;

    namespace
    {
        // QMessageBoxConfirmPrompter：基于真实 QMessageBox 的默认弹框执行器。
        class QMessageBoxConfirmPrompter final : public IConfirmPrompter
        {
        public:
            explicit QMessageBoxConfirmPrompter(QWidget* parent) : m_parent(parent) {}

            bool PromptUiConfirm(
                const UiConfirmRequest& request,
                const Scope scope,
                const Channel channel,
                const QString& targetDescription,
                const bool offerDontAskAgain,
                bool& dontAskAgainChecked) override
            {
                dontAskAgainChecked = false;
                // S9：m_parent 是 QPointer，父窗口销毁后这里自动是 nullptr，QMessageBox
                // 仍能无父窗口地弹出，不会悬空解引用。
                QMessageBox box(m_parent);
                // S-a（第二轮修复）：显式套 KswordTheme::OpaqueDialogStyle，与仓库
                // 其它 QDialog/QMessageBox（例如 KernelPlatformAuditTab.cpp）统一
                // 约定对齐——父容器用了透明/特殊样式时，弹框不该继承出黑底。
                box.setObjectName(QStringLiteral("WorkbenchUiConfirmMessageBox"));
                box.setStyleSheet(KswordTheme::OpaqueDialogStyle(box.objectName()));
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(workbench_messages::UiConfirmTitle());
                box.setText(workbench_messages::UiConfirmBody(request, scope, channel, targetDescription));
                QCheckBox* checkBox = nullptr;
                if (offerDontAskAgain)
                {
                    checkBox = new QCheckBox(workbench_messages::DontAskAgainThisRunText(), &box);
                    box.setCheckBox(checkBox);
                }
                QPushButton* acceptButton = box.addButton(
                    workbench_messages::UiConfirmAcceptButtonText(), QMessageBox::AcceptRole);
                QPushButton* rejectButton = box.addButton(
                    workbench_messages::UiConfirmRejectButtonText(), QMessageBox::RejectRole);
                box.setDefaultButton(rejectButton);
                box.exec();
                const bool agreed = box.clickedButton() == acceptButton;
                if (agreed && checkBox != nullptr)
                {
                    dontAskAgainChecked = checkBox->isChecked();
                }
                return agreed;
            }

            ApprovalAnswer PromptApproval(
                const ApprovalRequest& request,
                const Scope scope,
                const Channel channel,
                const QString& targetDescription,
                const bool offerRestOfBatch) override
            {
                QMessageBox box(m_parent);
                box.setObjectName(QStringLiteral("WorkbenchApprovalMessageBox"));   // S-a
                box.setStyleSheet(KswordTheme::OpaqueDialogStyle(box.objectName()));
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(workbench_messages::ApprovalDialogTitle());
                box.setText(workbench_messages::ApprovalDialogBody(request, scope, channel, targetDescription));
                QPushButton* thisBlockButton = box.addButton(
                    workbench_messages::ApprovalThisBlockOnlyButtonText(), QMessageBox::DestructiveRole);
                QPushButton* restOfBatchButton = offerRestOfBatch
                    ? box.addButton(workbench_messages::ApprovalRestOfBatchButtonText(), QMessageBox::DestructiveRole)
                    : nullptr;
                QPushButton* cancelButton = box.addButton(
                    workbench_messages::ApprovalCancelButtonText(), QMessageBox::RejectRole);
                box.setDefaultButton(cancelButton);
                box.exec();
                QAbstractButton* clicked = box.clickedButton();
                if (clicked == thisBlockButton)
                {
                    return ApprovalAnswer::ThisBlockOnly;
                }
                if (restOfBatchButton != nullptr && clicked == restOfBatchButton)
                {
                    return ApprovalAnswer::RestOfBatch;
                }
                return ApprovalAnswer::Deny;
            }

            ModeSwitchDecision PromptModeSwitch(
                const WriteMode /*fromMode*/,
                const WriteMode toMode,
                const std::uint64_t pendingBytes,
                const std::uint64_t pendingBlocks) override
            {
                QMessageBox box(m_parent);
                box.setObjectName(QStringLiteral("WorkbenchModeSwitchMessageBox"));   // S-a
                box.setStyleSheet(KswordTheme::OpaqueDialogStyle(box.objectName()));
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(workbench_messages::ModeSwitchDialogTitle());
                box.setText(workbench_messages::ModeSwitchDialogBody(pendingBytes, pendingBlocks, toMode));
                QPushButton* applyButton = box.addButton(
                    workbench_messages::ApplyThenSwitchButtonText(), QMessageBox::AcceptRole);
                QPushButton* discardButton = box.addButton(
                    workbench_messages::DiscardThenSwitchButtonText(), QMessageBox::DestructiveRole);
                QPushButton* cancelButton = box.addButton(
                    workbench_messages::ModeSwitchCancelButtonText(), QMessageBox::RejectRole);
                box.setDefaultButton(cancelButton);
                box.setEscapeButton(cancelButton);
                box.exec();
                QAbstractButton* clicked = box.clickedButton();
                if (clicked == applyButton)
                {
                    return ModeSwitchDecision::ApplyThenSwitch;
                }
                if (clicked == discardButton)
                {
                    return ModeSwitchDecision::DiscardThenSwitch;
                }
                return ModeSwitchDecision::Cancel;
            }

            // PromptLeaveWithPending（修复缺陷 3）：覆写基类的保守默认实现，
            // 真正弹一个"有未提交的修改"三选一框——文案与 PromptModeSwitch 完全
            // 分开（见 WorkbenchMessages.h 对应三个函数的注释），不再把"离开
            // 视图"说成"切换写入模式"。
            ModeSwitchDecision PromptLeaveWithPending(
                const std::uint64_t pendingBytes,
                const std::uint64_t pendingBlocks,
                const QString& reasonText) override
            {
                QMessageBox box(m_parent);
                box.setObjectName(QStringLiteral("WorkbenchLeaveWithPendingMessageBox"));
                box.setStyleSheet(KswordTheme::OpaqueDialogStyle(box.objectName()));
                box.setIcon(QMessageBox::Warning);
                box.setWindowTitle(workbench_messages::LeaveWithPendingDialogTitle());
                box.setText(workbench_messages::LeaveWithPendingDialogBody(pendingBytes, pendingBlocks, reasonText));
                QPushButton* applyButton = box.addButton(
                    workbench_messages::LeaveWithPendingApplyButtonText(), QMessageBox::AcceptRole);
                QPushButton* discardButton = box.addButton(
                    workbench_messages::LeaveWithPendingDiscardButtonText(), QMessageBox::DestructiveRole);
                QPushButton* cancelButton = box.addButton(
                    workbench_messages::LeaveWithPendingCancelButtonText(), QMessageBox::RejectRole);
                // 默认按钮与 Esc 都是"取消"：有未提交修改时离开不该是误触一下
                // Enter/Esc 就发生的事（与 PromptModeSwitch 同一处理方式）。
                box.setDefaultButton(cancelButton);
                box.setEscapeButton(cancelButton);
                box.exec();
                QAbstractButton* clicked = box.clickedButton();
                if (clicked == applyButton)
                {
                    return ModeSwitchDecision::ApplyThenSwitch;
                }
                if (clicked == discardButton)
                {
                    return ModeSwitchDecision::DiscardThenSwitch;
                }
                return ModeSwitchDecision::Cancel;
            }

        private:
            // S9：同头文件 m_dialogParent 的理由——父窗口可能先于本执行器销毁。
            QPointer<QWidget> m_parent;
        };
    }

    WorkbenchConfirmations::WorkbenchConfirmations(
        QWidget* dialogParent,
        ksword::memwb::MemoryWritePolicy* policy,
        std::unique_ptr<IConfirmPrompter> prompter)
        // S-d：dialogParent 只需要转给 m_prompter 的默认实现（弹框真正的父窗口），
        // 不必在本类里再保存一份从未被读过的 QPointer。
        : m_policy(policy)
        , m_prompter(prompter ? std::move(prompter) : std::make_unique<QMessageBoxConfirmPrompter>(dialogParent))
    {
    }

    WorkbenchConfirmations::~WorkbenchConfirmations() = default;

    void WorkbenchConfirmations::SetCurrentContext(
        const WriteMode mode, const Scope scope, const Channel channel)
    {
        m_mode = mode;
        m_scope = scope;
        m_channel = channel;
    }

    void WorkbenchConfirmations::SetTargetDescription(const QString& description)
    {
        m_targetDescription = description;
    }

    bool WorkbenchConfirmations::ConfirmUi(const UiConfirmRequest& request)
    {
        // offerDontAskAgain 只在策略判定为"首次危险立即写入"时为真；暂存应用等场景
        // 不提供勾选框（policy 为空时也一律不提供，保守地每次都问）。
        bool offerDontAskAgain = false;
        if (m_policy != nullptr)
        {
            offerDontAskAgain = m_policy->Decide(m_mode, m_scope, m_channel, false).offerDontAskAgain;
        }
        bool dontAskAgainChecked = false;
        const bool agreed = m_prompter->PromptUiConfirm(
            request, m_scope, m_channel, m_targetDescription, offerDontAskAgain, dontAskAgainChecked);
        if (agreed && dontAskAgainChecked && m_policy != nullptr)
        {
            // 只在用户同意写入、且确实勾了"本次运行不再询问"时才记忆；用户拒绝时
            // 即使勾了框也不记（拒绝意味着这次写入本身都没发生，不该省略下次确认）。
            m_policy->NoteConfirmed(m_scope, m_channel, true);
        }
        return agreed;
    }

    ApprovalAnswer WorkbenchConfirmations::ConfirmApproval(const ApprovalRequest& request)
    {
        // 这一步永远弹，不读任何抑制开关——MemoryWriteTransaction 本身也是这样调用的。
        // "本次其余块也强制"只在 blocksTotal - blockIndex > 1 时才提供。
        const bool offerRestOfBatch = request.blocksTotal > request.blockIndex + 1;
        return m_prompter->PromptApproval(request, m_scope, m_channel, m_targetDescription, offerRestOfBatch);
    }

    ModeSwitchDecision WorkbenchConfirmations::PromptModeSwitch(
        const WriteMode fromMode,
        const WriteMode toMode,
        const std::uint64_t pendingBytes,
        const std::uint64_t pendingBlocks)
    {
        return m_prompter->PromptModeSwitch(fromMode, toMode, pendingBytes, pendingBlocks);
    }

    ModeSwitchDecision WorkbenchConfirmations::PromptLeaveWithPending(
        const std::uint64_t pendingBytes,
        const std::uint64_t pendingBlocks,
        const QString& reasonText)
    {
        return m_prompter->PromptLeaveWithPending(pendingBytes, pendingBlocks, reasonText);
    }

    IConfirmPrompter& WorkbenchConfirmations::prompterForTest()
    {
        return *m_prompter;
    }
}
