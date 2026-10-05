#pragma once

// ============================================================
// WorkbenchConfirmations.h
// 作用：
// - 实现 MemoryWriteTransaction.h 的 IConfirmationSink：ConfirmUi（可被设置抑制
//   的普通确认）与 ConfirmApproval（永远弹、不受抑制影响的强制同意）。
// - 额外提供"写入模式三选一"的封装函数 PromptModeSwitch，供上层在
//   MemoryWriteTransaction::SetMode 返回 NeedsDecision 时调用，取得
//   ModeSwitchDecision 后自行调 ResolveModeSwitch（那一步仍会按需回调本类的
//   ConfirmUi/ConfirmApproval，三选一本身只是"问一下要不要切"）。
// - 真正弹框的步骤经 IConfirmPrompter 接口注入：生产默认实现用 QMessageBox
//   （纯 QtWidgets，没有额外依赖，不存在 CodeEditorWidget 那类夹具链接问题）；
//   夹具注入一个记录调用并直接返回预设答案的假实现，不会真的 exec() 模态框。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"
#include "../../../../shared/evidence/memory_workbench/MemoryWritePolicy.h"

#include <QString>

#include <cstdint>
#include <memory>

class QWidget;

namespace ks::ui
{
    // IConfirmPrompter："真正弹一个框，等用户回答"这一步的接口。
    class IConfirmPrompter
    {
    public:
        virtual ~IConfirmPrompter() = default;

        // PromptUiConfirm：普通确认框。scope/channel/targetDescription 是 B2 补上的
        // 三个参数——真正弹框的实现据此拼"通道 · 范围 · 目标"的人话正文，绝不展示
        // request.targetIdentity 这种机器可读的 IdentityKey。offerDontAskAgain 决定
        // 是否显示"本次运行不再询问"勾选框；传出：用户是否同意；dontAskAgainChecked
        // 传出勾选结果（仅在 offerDontAskAgain 为真且用户同意时才有意义，其余置 false）。
        virtual bool PromptUiConfirm(
            const ksword::memwb::UiConfirmRequest& request,
            ksword::memwb::Scope scope,
            ksword::memwb::Channel channel,
            const QString& targetDescription,
            bool offerDontAskAgain,
            bool& dontAskAgainChecked) = 0;

        // PromptApproval：强制同意框，scope/channel/targetDescription 同上。
        // offerRestOfBatch 决定"本次其余块也强制"按钮是否出现（仅
        // blocksTotal-blockIndex>1 时为真）。
        virtual ksword::memwb::ApprovalAnswer PromptApproval(
            const ksword::memwb::ApprovalRequest& request,
            ksword::memwb::Scope scope,
            ksword::memwb::Channel channel,
            const QString& targetDescription,
            bool offerRestOfBatch) = 0;

        // PromptModeSwitch：写入模式三选一框。
        virtual ksword::memwb::ModeSwitchDecision PromptModeSwitch(
            ksword::memwb::WriteMode fromMode,
            ksword::memwb::WriteMode toMode,
            std::uint64_t pendingBytes,
            std::uint64_t pendingBlocks) = 0;

        // PromptLeaveWithPending（WP-J6 Wave 3 新增，修复缺陷 3）：离开视图（身份
        // 类变更或关闭）时仍有未提交暂存补丁的三选一框——与 PromptModeSwitch 是
        // 两个不同的场景：这里**没有"切换写入模式"这件事**，原实现误用
        // PromptModeSwitch（from/to 传同一个值）导致文案自相矛盾（审核报告
        // wpJ6/wave3 发现 3）。
        // 传入：pendingBytes/pendingBlocks 待写入字节数/块数；reasonText 离开
        // 原因的人话描述（调用方按 WorkbenchTarget::LeaveReason 翻译好传入，
        // 本接口不关心具体枚举值）。
        // 传出：复用既有的 ModeSwitchDecision 枚举，语义改读成"离开前先做什么"
        // ——ApplyThenSwitch=应用并离开，DiscardThenSwitch=丢弃并离开，
        // Cancel=取消（不新增枚举，只增不改既有接口）。
        //
        // 本方法**不是纯虚**（给了保守的默认实现：恒返回 Cancel，不隐式应用也
        // 不隐式丢弃用户的未提交修改）——IConfirmPrompter 在本任务书之外还有
        // 别的实现者（例如 wpJ4 夹具的假执行器，不在本任务书允许修改的范围
        // 内），纯虚新增会让它们编译不通过。真正要测这条路径的夹具（wpJ6/wpG）
        // 应该覆写它返回预设答案；生产默认实现（QMessageBoxConfirmPrompter）
        // 必须覆写成真正弹框，不能依赖这个保守默认值。
        virtual ksword::memwb::ModeSwitchDecision PromptLeaveWithPending(
            std::uint64_t pendingBytes,
            std::uint64_t pendingBlocks,
            const QString& reasonText)
        {
            Q_UNUSED(pendingBytes);
            Q_UNUSED(pendingBlocks);
            Q_UNUSED(reasonText);
            return ksword::memwb::ModeSwitchDecision::Cancel;
        }
    };

    // WorkbenchConfirmations：IConfirmationSink 的生产实现。
    class WorkbenchConfirmations final : public ksword::memwb::IConfirmationSink
    {
    public:
        // 构造：dialogParent 用于生产默认 QMessageBox 实现的父窗口（可为空）；
        // policy 为可选的确认策略引用（用于算出是否该显示"不再询问"勾选框，
        // 并在用户勾选同意后调 NoteConfirmed），为空则永不显示该勾选框；
        // prompter 为空时内部构造一个基于 QMessageBox 的默认实现。
        explicit WorkbenchConfirmations(
            QWidget* dialogParent,
            ksword::memwb::MemoryWritePolicy* policy = nullptr,
            std::unique_ptr<IConfirmPrompter> prompter = nullptr);
        ~WorkbenchConfirmations() override;

        // SetCurrentContext：调用方在每次 Commit 之前刷新当前写入模式/范围/通道，
        // 供 ConfirmUi 据此算出是否该显示"不再询问"勾选框，也随 ConfirmUi/
        // ConfirmApproval 一起转发给弹框实现拼"通道 · 范围"前缀（B2）。
        void SetCurrentContext(
            ksword::memwb::WriteMode mode,
            ksword::memwb::Scope scope,
            ksword::memwb::Channel channel);

        // SetTargetDescription：B2——装配层把"目标"的人话描述（例如会话条目标 chip
        // 已有的"进程名 · PID · 位数"文案）喂进来，供确认框正文引用；绝不能让弹框
        // 直接展示 UiConfirmRequest::targetIdentity 那种机器可读的 IdentityKey。
        // 调用方法：每次目标变化（附加/切换进程、切换范围）时更新一次即可，本类只
        // 保存最近一次喂入的值，不做任何解析。
        void SetTargetDescription(const QString& description);

        // ConfirmUi / ConfirmApproval：IConfirmationSink 的两个虚函数实现。
        bool ConfirmUi(const ksword::memwb::UiConfirmRequest& request) override;
        ksword::memwb::ApprovalAnswer ConfirmApproval(const ksword::memwb::ApprovalRequest& request) override;

        // PromptModeSwitch：写入模式三选一，见文件头说明。
        ksword::memwb::ModeSwitchDecision PromptModeSwitch(
            ksword::memwb::WriteMode fromMode,
            ksword::memwb::WriteMode toMode,
            std::uint64_t pendingBytes,
            std::uint64_t pendingBlocks);

        // PromptLeaveWithPending：离开前存在未提交暂存补丁的三选一，见
        // IConfirmPrompter::PromptLeaveWithPending 的注释。本函数只是薄转发，
        // 不加任何逻辑（与 PromptModeSwitch 一致的转发风格）。
        ksword::memwb::ModeSwitchDecision PromptLeaveWithPending(
            std::uint64_t pendingBytes,
            std::uint64_t pendingBlocks,
            const QString& reasonText);

        // prompterForTest：供测试直接拿到注入的弹框执行器（例如核对调用次数）。
        IConfirmPrompter& prompterForTest();

    private:
        // S-d（第二轮修复）：原来这里还存着一份 m_dialogParent（QPointer<QWidget>），
        // 但构造完之后整个类从不读它——S9 真正生效的 QPointer 是
        // QMessageBoxConfirmPrompter::m_parent（见 .cpp），弹框时用的是那一份，不是
        // 这里。留着这个从未被读过的字段只会让人误以为"S9 的修复点在这里"，
        // 已删除；dialogParent 参数仍然保留，只是直接转给 m_prompter 的默认实现
        // 构造（见 .cpp 构造函数初始化列表）。
        ksword::memwb::MemoryWritePolicy* m_policy = nullptr;
        std::unique_ptr<IConfirmPrompter> m_prompter;

        ksword::memwb::WriteMode m_mode = ksword::memwb::WriteMode::Immediate;
        ksword::memwb::Scope m_scope = ksword::memwb::Scope::ProcessVirtual;
        ksword::memwb::Channel m_channel = ksword::memwb::Channel::UserMode;
        // m_targetDescription：SetTargetDescription 喂入的目标人话描述，见该函数注释。
        QString m_targetDescription;
    };
}
