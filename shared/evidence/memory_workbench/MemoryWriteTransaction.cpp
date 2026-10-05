// ============================================================
// MemoryWriteTransaction.cpp
// 作用：
// - 写事务的构造、暂存转发、模式切换与只读访问器。
// - 提交管线（Commit 及其各阶段、审计辅助）在 MemoryWriteTransaction.Commit.cpp。
// - 全部是纯逻辑，不依赖 Qt 与 Win32，也不做任何真实 I/O。
// ============================================================

#include "MemoryWriteTransaction.h"

namespace ksword::memwb
{
    // 构造：记下全部注入的引用，初始状态 Idle，界面确认不抑制。
    // 传入：见头文件。
    MemoryWriteTransaction::MemoryWriteTransaction(
        MemoryDiffOverlay& overlay,
        const MemoryTargetSession& session,
        const SessionRevisions& revisions,
        IByteStore& store,
        IConfirmationSink& confirmation,
        IAuditSink& audit,
        const WriteMode initialMode)
        : overlay_(overlay)
        , session_(session)
        , revisions_(revisions)
        , store_(store)
        , confirmation_(confirmation)
        , audit_(audit)
        , mode_(initialMode)
    {
    }

    // Stage：把编辑转给 overlay，成功后按 overlay 是否有补丁调整状态。
    // 传入：address 起始绝对地址；bytes 想写入的字节。传出：overlay 的暂存结果。
    StageStatus MemoryWriteTransaction::Stage(
        const std::uint64_t address,
        const std::vector<std::uint8_t>& bytes)
    {
        // 第一步：转发。本类不碰 store，Stage 永远不会触发任何真实写入。
        const StageStatus status = overlay_.Stage(address, bytes);
        if (status != StageStatus::Ok)
        {
            return status;
        }

        // 第二步：提交进行中（例如确认窗口里的嵌套事件循环）不能改状态，
        // 否则会把 Writing / Verifying 中途拽回 Staged。
        if (busy_)
        {
            return status;
        }

        // 第三步：overlay 有补丁就是 Staged；全是空操作编辑、补丁都消失时，
        // 原为 Staged 的回到 Idle，其它状态（Committed / Failed 等）保持不变。
        if (overlay_.HasPendingPatches())
        {
            state_ = State::Staged;
        }
        else if (state_ == State::Staged)
        {
            state_ = State::Idle;
        }

        return status;
    }

    // OnEditCompleted：Immediate 模式自动提交，其余模式什么也不做。
    // 传出：自动提交的报告；没有触发提交时为 nullopt。
    std::optional<CommitReport> MemoryWriteTransaction::OnEditCompleted()
    {
        // 暂存后统一应用：编辑只暂存，等调用方显式 Commit。
        if (mode_ != WriteMode::Immediate)
        {
            return std::nullopt;
        }

        // 立即写入：走与显式 Commit 完全相同的管线。
        return Commit();
    }

    // PendingOverlayGuard：切换模式前是否必须让用户决定。
    // 传入：from 当前模式；to 目标模式。传出：模式不同且 overlay 有补丁时为 true。
    bool MemoryWriteTransaction::PendingOverlayGuard(const WriteMode from, const WriteMode to) const
    {
        // 模式没变就不存在切换，更谈不上丢补丁。
        if (from == to)
        {
            return false;
        }

        return overlay_.HasPendingPatches();
    }

    // SetMode：请求切换模式，有未提交补丁时不切换并等待决定。
    // 传入：newMode 目标模式。传出：Switched / NeedsDecision / Busy。
    ModeSwitchStatus MemoryWriteTransaction::SetMode(const WriteMode newMode)
    {
        // 提交进行中不允许改模式，也不允许记待决请求。
        if (busy_)
        {
            return ModeSwitchStatus::Busy;
        }

        // 有补丁且模式确实要变：不切换，记下目标，交给调用方弹三选一。
        if (PendingOverlayGuard(mode_, newMode))
        {
            pendingMode_ = newMode;
            return ModeSwitchStatus::NeedsDecision;
        }

        // 其余情况直接切换（目标等于当前模式时等于什么都没改），旧的待决请求作废。
        mode_ = newMode;
        pendingMode_.reset();
        return ModeSwitchStatus::Switched;
    }

    // ResolveModeSwitch：执行用户对待决切换的决定。
    // 传入：decision 用户的选择。传出：见头文件。
    ModeSwitchResult MemoryWriteTransaction::ResolveModeSwitch(const ModeSwitchDecision decision)
    {
        ModeSwitchResult result;

        // 提交进行中：什么都不做，也不动待决请求。
        if (busy_)
        {
            result.status = ModeSwitchStatus::Busy;
            return result;
        }

        // 没有待决请求：绝不执行决定，尤其不能因为一次迟到的 DiscardThenSwitch 丢掉补丁。
        if (!pendingMode_.has_value())
        {
            result.status = ModeSwitchStatus::NoPendingSwitch;
            return result;
        }

        // 取出待决目标并立刻清掉，此后无论走哪条路径都不会残留过期请求。
        const WriteMode target = *pendingMode_;
        pendingMode_.reset();

        // 丢弃再切换：补丁一次清空，状态回到 Idle，然后切换。
        if (decision == ModeSwitchDecision::DiscardThenSwitch)
        {
            overlay_.DiscardAll();
            state_ = State::Idle;
            mode_ = target;
            result.status = ModeSwitchStatus::Switched;
            return result;
        }

        // 先应用再切换：走完整提交管线；只有真正提交成功（或补丁已不存在）才切换，
        // 提交失败则模式保持不变，补丁按提交失败的规则保留。
        if (decision == ModeSwitchDecision::ApplyThenSwitch)
        {
            const CommitReport report = Commit();
            result.commitReport = report;
            const bool applied = (report.outcome == CommitOutcome::Committed)
                || (report.outcome == CommitOutcome::NoChange);
            if (applied)
            {
                mode_ = target;
                result.status = ModeSwitchStatus::Switched;
            }
            else
            {
                result.status = ModeSwitchStatus::ApplyFailed;
            }

            return result;
        }

        // 取消，以及任何未识别的决定值：什么都不改。
        result.status = ModeSwitchStatus::Cancelled;
        return result;
    }

    // CurrentState：当前状态。
    MemoryWriteTransaction::State MemoryWriteTransaction::CurrentState() const
    {
        return state_;
    }

    // Mode：当前写入模式。
    WriteMode MemoryWriteTransaction::Mode() const
    {
        return mode_;
    }

    // PendingModeSwitch：待决的切换目标。
    std::optional<WriteMode> MemoryWriteTransaction::PendingModeSwitch() const
    {
        return pendingMode_;
    }

    // IsBusy：是否有 Commit 正在进行。
    bool MemoryWriteTransaction::IsBusy() const
    {
        return busy_;
    }

    // SetUiConfirmSuppressed：设置是否抑制普通界面确认。
    // 传入：suppressed 为 true 时 Commit 不再调用 ConfirmUi。
    void MemoryWriteTransaction::SetUiConfirmSuppressed(const bool suppressed)
    {
        uiConfirmSuppressed_ = suppressed;
    }

    // UiConfirmSuppressed：当前是否抑制界面确认。
    bool MemoryWriteTransaction::UiConfirmSuppressed() const
    {
        return uiConfirmSuppressed_;
    }
}
