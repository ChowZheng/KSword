// ============================================================
// WorkbenchWriteController.Undo.cpp
// 作用：
// - WorkbenchWriteController.Undo.h 声明的 WorkbenchUndoCoordinator 的实现：
//   构造、bindTarget/setIdentityKey 两个薄转发、record（转发 MemoryEditJournal::
//   Record，tick 由本类自己的 tickProvider_ 取得）、canUndo/canRedo 查询、
//   以及 replayOnce 承担的核心算法——"新建一个只覆盖一个块的临时 scratch
//   overlay，完整走一次 Commit() 管线"，详见 .Undo.h 文件头的设计说明，这里
//   不重复抄写，只在关键步骤标注对应哪一条规则。
// ============================================================

#include "WorkbenchWriteController.Undo.h"

#include "../../../../shared/evidence/memory_workbench/MemoryTargetSession.h"

#include <utility>

namespace ks::ui
{
    using ksword::memwb::BaselineLoadStatus;
    using ksword::memwb::CommitOutcome;
    using ksword::memwb::CommitReport;
    using ksword::memwb::JournalBytes;
    using ksword::memwb::JournalReplay;
    using ksword::memwb::MemoryDiffOverlay;
    using ksword::memwb::MemoryWriteTransaction;
    using ksword::memwb::StageStatus;

    // ------------------------------------------------------------
    // 构造：持有五个长期引用/指针（与 MemoryWriteTransaction 自身的生命周期约定
    // 一致——调用方必须保证它们比本对象活得更久），journal_ 用默认容量
    // （32 MiB / 256 步）构造，tickProvider_ 原样保存供 record()/后续查询使用，
    // suppressedProvider_ 同样原样保存，供 replayOnce 构造临时事务时查询当前
    // 的确认抑制状态（COMMON 裁决第 3 项）。
    // ------------------------------------------------------------
    WorkbenchUndoCoordinator::WorkbenchUndoCoordinator(
        const ksword::memwb::MemoryTargetSession& session,
        const ksword::memwb::SessionRevisions& revisions,
        ksword::memwb::IByteStore& store,
        ksword::memwb::IConfirmationSink& confirmation,
        ksword::memwb::IAuditSink& audit,
        std::function<std::uint64_t()> tickProvider,
        std::function<bool()> suppressedProvider)
        : session_(session)
        , revisions_(revisions)
        , store_(store)
        , confirmation_(confirmation)
        , audit_(audit)
        , tickProvider_(std::move(tickProvider))
        , suppressedProvider_(std::move(suppressedProvider))
        , journal_()
    {
    }

    // ------------------------------------------------------------
    // bindTarget：薄转发 MemoryEditJournal::BindTarget。身份没变（SameTarget 为
    // 真）时该函数本身是空操作，调用方可以无条件在每次 sessionChanged 时调用。
    // ------------------------------------------------------------
    bool WorkbenchUndoCoordinator::bindTarget(const ksword::memwb::MemoryTargetSession& session)
    {
        return journal_.BindTarget(session);
    }

    // ------------------------------------------------------------
    // setIdentityKey：保存供 replayOnce 构造临时 scratch overlay 时使用。
    // ------------------------------------------------------------
    void WorkbenchUndoCoordinator::setIdentityKey(std::string identityKey)
    {
        identityKey_ = std::move(identityKey);
    }

    // ------------------------------------------------------------
    // record：一次成功提交之后记账。tick 由本类自己的 tickProvider_ 取得（装配层
    // 不需要、也不应该自己算 tick——合并窗口判断全部收在 journal_ 内部）。
    // ------------------------------------------------------------
    ksword::memwb::JournalRecordResult WorkbenchUndoCoordinator::record(
        std::uint64_t address,
        const JournalBytes& before,
        const JournalBytes& after)
    {
        const std::uint64_t tick = tickProvider_ ? tickProvider_() : 0;
        return journal_.Record(address, before, after, tick);
    }

    // ------------------------------------------------------------
    // canUndo / canRedo：薄转发。
    // ------------------------------------------------------------
    bool WorkbenchUndoCoordinator::canUndo() const
    {
        return journal_.CanUndo();
    }

    bool WorkbenchUndoCoordinator::canRedo() const
    {
        return journal_.CanRedo();
    }

    // ------------------------------------------------------------
    // undo / redo：都转给 replayOnce，isUndo 区分取 PeekUndo 还是 PeekRedo、成功
    // 后调用 MarkUndone 还是 MarkRedone。
    // ------------------------------------------------------------
    UndoReplayResult WorkbenchUndoCoordinator::undo()
    {
        return replayOnce(true);
    }

    UndoReplayResult WorkbenchUndoCoordinator::redo()
    {
        return replayOnce(false);
    }

    // ------------------------------------------------------------
    // lastFailureText：最近一次 undo()/redo() 非 Replayed 时的原因细节串。
    // ------------------------------------------------------------
    const std::string& WorkbenchUndoCoordinator::lastFailureText() const noexcept
    {
        return lastFailureText_;
    }

    // ------------------------------------------------------------
    // replayOnce：核心算法，步骤与 .Undo.h 文件头"为什么撤销要绕开基线窗口"一节
    // 逐条对应（下面的编号沿用那段注释的 1-4 步）。**COMMON 裁决第 3 项**：返回值
    // 改为 UndoReplayResult，把回放地址/字节与完整 CommitReport 一起带出去，供
    // 外层 WorkbenchWriteController 做 W4 收尾；且临时事务不再永远以"不抑制"
    // 构造，改为继承 suppressedProvider_ 取到的当前值。
    // ------------------------------------------------------------
    UndoReplayResult WorkbenchUndoCoordinator::replayOnce(bool isUndo)
    {
        UndoReplayResult result;

        // 第 1 步：取得这一步的回放内容，只读取不移动游标。
        const std::optional<JournalReplay> replay =
            isUndo ? journal_.PeekUndo() : journal_.PeekRedo();
        if (!replay.has_value())
        {
            lastFailureText_ = "journal 没有可撤销/可重做的步骤";
            result.outcome = UndoReplayOutcome::NothingToReplay;
            return result;
        }

        // 第 2 步：新建一个只覆盖这一个块的临时 scratch overlay（栈上对象，函数
        // 返回即销毁，不是本类的长期成员）。用"期望的当前字节"当基线，不需要先
        // 真的去读一次目标——这正是为什么要绕开主 overlay 的基线窗口：这个临时
        // 窗口恰好只有一块大小，Stage 必然成功，不受视口滚动位置影响。
        MemoryDiffOverlay scratch;
        const std::vector<std::uint8_t> validMask(replay->expectedCurrent.size(), 1);
        const BaselineLoadStatus loadStatus = scratch.LoadBaseline(
            identityKey_, replay->address, replay->expectedCurrent, validMask);
        if (loadStatus != BaselineLoadStatus::Ok)
        {
            // 理论上不会发生：expectedCurrent 与刚构造的 validMask 等长，地址来自
            // journal 自身已经校验过的回放记录；仍然显式报错而不是静默假装成功，
            // 避免把"内部不一致"误判成"目标已改动"这类用户可见状态。
            lastFailureText_ = "撤销/重做的临时基线载入失败（内部不一致）";
            result.outcome = UndoReplayOutcome::CommitRejected;
            return result;
        }

        // 第 3 步：在这个只有一块大小的临时窗口里 Stage 要写入的新字节，范围
        // 当然"在窗口内"，字节当然"已经读到"，Stage 必然成功。
        const StageStatus stageStatus = scratch.Stage(replay->address, replay->restore);
        if (stageStatus != StageStatus::Ok)
        {
            lastFailureText_ = "撤销/重做的临时暂存失败（内部不一致）";
            result.outcome = UndoReplayOutcome::CommitRejected;
            return result;
        }

        // 第 4 步：用（scratch、真实 session_、真实 revisions_、真实 store_、真实
        // confirmation_、真实 audit_）构造一个临时事务并完整走一次 Commit()——
        // 复用确认、Stale 复核、写前核对当前字节是否等于 before（Commit 的 (e)
        // 步天然涵盖 CheckReplay 语义）、写入、回读、AcceptWrite、审计，不重新
        // 发明任何一步。**COMMON 裁决第 3 项修复**：继承控制器当前的
        // uiConfirmSuppressed——撤销/重做与正常提交共用同一条"确认/审计/回读"
        // 链，策略判定"这个范围/通道不弹确认"时撤销也不应该额外弹一次。
        MemoryWriteTransaction scratchTransaction(
            scratch, session_, revisions_, store_, confirmation_, audit_);
        scratchTransaction.SetUiConfirmSuppressed(suppressedProvider_ ? suppressedProvider_() : false);
        const CommitReport report = scratchTransaction.Commit();

        // 不论结果如何，先把这次回放的地址/字节/完整报告记进结果——即使最终
        // outcome 不是 Replayed，外层也可能需要 report.failureText 之外的字段
        // （失败也要交给控制器做报告收尾，但不移动日志游标、不记录新历史）。
        result.commitAttempted = true;
        result.address = replay->address;
        result.before = replay->expectedCurrent;
        result.after = replay->restore;
        result.report = report;

        if (report.outcome == CommitOutcome::Committed)
        {
            const bool marked = isUndo ? journal_.MarkUndone() : journal_.MarkRedone();
            if (!marked)
            {
                // 防御性兜底：PeekUndo/PeekRedo 刚刚还有值，MarkUndone/MarkRedone
                // 却说没有——说明两次调用之间状态被别处改动过，这是实现缺陷，不
                // 是"目标已变"这类用户可见状态，单独归一类不要混进 TargetChanged。
                lastFailureText_ = "撤销/重做提交成功但游标未能移动（内部不一致）";
                result.outcome = UndoReplayOutcome::CommitRejected;
                return result;
            }
            lastFailureText_.clear();
            result.outcome = UndoReplayOutcome::Replayed;
            return result;
        }
        if (report.outcome == CommitOutcome::TargetChanged)
        {
            // 与 MemoryEditJournal.h 文档"目标已被其他程序改动，无法自动撤销"的
            // 描述完全对应：整步拒绝，journal 的游标不动（不调用 MarkUndone/
            // MarkRedone）。
            lastFailureText_ = report.failureText.empty()
                ? std::string("目标已被其他程序改动，无法自动撤销/重做")
                : report.failureText;
            result.outcome = UndoReplayOutcome::TargetChanged;
            return result;
        }
        // 其余结果（UserCancelled/Stale/ApprovalDenied/WriteFailed/
        // VerifyMismatch/InvalidSession/Busy/NoChange）统一归为 CommitRejected，
        // 具体原因留在 lastFailureText_ 里。
        lastFailureText_ = report.failureText;
        result.outcome = UndoReplayOutcome::CommitRejected;
        return result;
    }
}
