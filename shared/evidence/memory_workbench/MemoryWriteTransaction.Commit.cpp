// ============================================================
// MemoryWriteTransaction.Commit.cpp
// 作用：
// - 写事务的提交管线：Commit 及 (a) 到 (h) 各阶段、中止收尾、审计辅助。
//   固定顺序与每一步的理由见 MemoryWriteTransaction.h 文件头。
// - 构造、暂存转发、模式切换与访问器在 MemoryWriteTransaction.cpp。
// - 全部是纯逻辑：所有读写、确认、审计都经注入接口，本文件不做任何真实 I/O。
// ============================================================

#include "MemoryWriteTransaction.h"

#include <algorithm>

namespace ksword::memwb
{
    // CommitRun：一次 Commit 的全部局部状态。
    // 之所以不做成成员变量：同意范围（RestOfBatch）与各项快照都必须随这一次 Commit 结束
    // 而消失，放在局部结构里就不可能漏到下一次 Commit。
    struct MemoryWriteTransaction::CommitRun
    {
        // sessionAtStart：开始时捕获的会话副本，确认返回后拿来与当前会话比较。
        MemoryTargetSession sessionAtStart;
        // revisionsAtStart：开始时捕获的两个代次快照。
        RevisionSnapshot revisionsAtStart;
        // targetIdentity：审计与确认请求里使用的目标身份串。
        std::string targetIdentity;
        // blocks：开始时从 overlay 取走的差异块副本，之后的写入只按这份副本进行。
        std::vector<DiffBlock> blocks;
        // report：正在填写的报告。
        CommitReport report;
        // batchApproved：用户是否已选择"对本次提交剩余的块都同意"。
        bool batchApproved = false;
    };

    namespace
    {
        // Absorb：把一次访问结果里的两个告警位并进报告，只置位、不清除。
        // 传入：report 正在填写的报告；result 一次 Read / Write 的结果。
        // 传出：直接修改 report。成功不得清掉告警，所以这里只能做"或"，不能做赋值。
        void Absorb(CommitReport& report, const AccessResult& result)
        {
            if (result.scratchAreaDirty)
            {
                report.scratchAreaDirty = true;
            }

            if (result.readModifyWriteWindow)
            {
                report.readModifyWriteWindow = true;
            }
        }

        // NeedsReread：按结果推出"界面是否应当重新读取"。
        // 传入：report 已填好结果与计数的报告。传出：true 表示目标可能已变或基线已陈旧。
        bool NeedsReread(const CommitReport& report)
        {
            switch (report.outcome)
            {
            case CommitOutcome::WriteFailed:
            case CommitOutcome::VerifyMismatch:
            case CommitOutcome::TargetChanged:
            case CommitOutcome::Stale:
                return true;
            case CommitOutcome::ApprovalDenied:
                // 被拒绝之前如果已经有块落地，目标已被改动，需要重读；一个字节都没写则不必。
                return report.bytesWritten > 0;
            case CommitOutcome::NoChange:
            case CommitOutcome::Committed:
            case CommitOutcome::UserCancelled:
            case CommitOutcome::InvalidSession:
            case CommitOutcome::Busy:
                return false;
            }

            return false;
        }
    }

    // Commit：提交全部暂存补丁，顺序见文件头。
    // 传出：CommitReport；失败不会丢掉未写入块的暂存补丁。
    CommitReport MemoryWriteTransaction::Commit()
    {
        // run：本次提交的局部状态。身份串先算好，Busy 的审计也要用。
        CommitRun run;
        run.targetIdentity = IdentityKey(session_, overlay_.BaseAddress(), overlay_.BaselineSize());

        // 重入保护：已有提交在进行（典型是确认窗口的嵌套事件循环里又点了一次应用）。
        // 这次什么都不做，但开始与结束两条审计照记，让重入尝试在审计里可见。
        if (busy_)
        {
            RecordStarted(run);
            run.report.outcome = CommitOutcome::Busy;
            run.report.failureText = "another commit is already in progress";
            RecordFinished(run);
            return run.report;
        }

        // 进入忙状态后跑整条管线。任何注入接口抛异常都要先清忙标志、把进行中的状态
        // 置为 Failed 再重新抛出，否则这个对象会永远停在忙状态里。
        busy_ = true;
        try
        {
            RecordStarted(run);
            RunPipeline(run);
        }
        catch (...)
        {
            busy_ = false;
            const bool inFlight = (state_ == State::ConfirmPending)
                || (state_ == State::Writing)
                || (state_ == State::Verifying);
            if (inFlight)
            {
                state_ = State::Failed;
            }

            throw;
        }

        // 管线正常结束：清忙标志，补上 needsReread，记结束审计。
        busy_ = false;
        run.report.needsReread = NeedsReread(run.report);
        RecordFinished(run);
        return run.report;
    }

    // RunPipeline：运行 (a) 到 (h)。每个阶段返回 false 表示它已经填好报告并中止。
    // 传入：run 本次提交的局部状态。传出：结果写在 run.report 里。
    void MemoryWriteTransaction::RunPipeline(CommitRun& run)
    {
        // (a) 捕获会话身份、两个代次快照，并从 overlay 取走差异块副本。
        // 没有差异块就什么都不做，不触碰 store，也不打扰用户。
        run.sessionAtStart = session_;
        run.revisionsAtStart = revisions_.Capture();
        run.blocks = overlay_.DiffBlocks();
        run.report.blocksTotal = static_cast<std::uint64_t>(run.blocks.size());
        if (run.blocks.empty())
        {
            run.report.outcome = CommitOutcome::NoChange;
            return;
        }

        // (b) 会话自身不自洽就拒绝。顺序在 NoChange 之后：没有东西要写的时候不去苛责会话。
        const SessionError sessionError = Validate(session_);
        if (sessionError != SessionError::None)
        {
            run.report.outcome = CommitOutcome::InvalidSession;
            run.report.sessionError = sessionError;
            run.report.failureText = "target session is not self-consistent";
            return;
        }

        // (c) 界面确认（可被设置抑制，但抑制也要记审计），随后 (d) 重新核对新鲜度。
        if (!ConfirmWithUser(run))
        {
            return;
        }

        if (!RecheckFreshness(run))
        {
            return;
        }

        // (e) 写前复核：全部块都通过之后才会进入写阶段。
        if (!PrecheckTargets(run))
        {
            return;
        }

        // (f)(g)(h) 逐块写入、回读、吸收。
        WriteAndVerifyAll(run);
    }

    // ConfirmWithUser：(c) 状态进入 ConfirmPending，按设置决定是否调用 ConfirmUi。
    // 传入：run 本次提交的局部状态。传出：true 表示可以继续；false 表示用户取消，报告已填好。
    bool MemoryWriteTransaction::ConfirmWithUser(CommitRun& run)
    {
        state_ = State::ConfirmPending;

        // 设置抑制了界面确认：不调用 ConfirmUi，但仍然写一条审计，
        // 这样"确认被设置跳过"这件事在审计里有据可查，条数也不会比未抑制时少。
        if (uiConfirmSuppressed_)
        {
            RecordConfirm(run, AuditEvent::UiConfirmSuppressed, "ui confirmation suppressed by setting");
            return true;
        }

        // 组装确认请求：目标身份、块数、总字节与各块摘要。
        UiConfirmRequest request;
        request.targetIdentity = run.targetIdentity;
        request.blocksTotal = run.report.blocksTotal;
        for (const DiffBlock& block : run.blocks)
        {
            UiBlockInfo info;
            info.address = block.address;
            info.length = static_cast<std::uint64_t>(block.after.size());
            request.bytesTotal += info.length;
            request.blocks.push_back(info);
        }

        // 调用 ConfirmUi。真实实现里这里可能是嵌套事件循环，返回之前什么都可能发生，
        // 所以调用方在拿到结果后必须重新核对新鲜度（见 RecheckFreshness）。
        const bool accepted = confirmation_.ConfirmUi(request);
        if (!accepted)
        {
            RecordConfirm(run, AuditEvent::UiConfirmDenied, "user declined the write confirmation");
            AbortToStaged(run, CommitOutcome::UserCancelled, "user declined the write confirmation");
            return false;
        }

        RecordConfirm(run, AuditEvent::UiConfirmAccepted, "user accepted the write confirmation");
        return true;
    }

    // RecheckFreshness：(d) 确认返回之后重新核对会话身份与两个代次。
    // 这一步不受 uiConfirmSuppressed 影响：即使没有弹框，也不省这一道核对。
    // 传入：run 本次提交的局部状态。传出：true 表示一切如常；false 表示 Stale，报告已填好。
    bool MemoryWriteTransaction::RecheckFreshness(CommitRun& run)
    {
        // 会话身份：七个字段任一变了就是换了目标。
        if (!SameTarget(run.sessionAtStart, session_))
        {
            AbortToStaged(run, CommitOutcome::Stale, "target session changed while waiting for confirmation");
            return false;
        }

        // 两个代次：任一变了都算陈旧，分别报出是哪一个，便于排查。
        if (revisions_.IsStale(run.revisionsAtStart))
        {
            const RevisionSnapshot now = revisions_.Capture();
            if (now.source != run.revisionsAtStart.source)
            {
                AbortToStaged(run, CommitOutcome::Stale, "source revision changed while waiting for confirmation");
            }
            else
            {
                AbortToStaged(run, CommitOutcome::Stale, "content revision changed while waiting for confirmation");
            }

            return false;
        }

        return true;
    }

    // PrecheckTargets：(e) 对每个差异块读当前真实字节，与暂存时的 before 核对。
    // 全部块都核对完毕之前不会发出任何写。读失败、partial、不一致都算目标已变。
    // 传入：run 本次提交的局部状态。传出：true 表示全部一致；false 表示 TargetChanged，报告已填好。
    bool MemoryWriteTransaction::PrecheckTargets(CommitRun& run)
    {
        for (const DiffBlock& block : run.blocks)
        {
            // 读当前真实字节。长度取 before 的长度，与块等长。
            const AccessResult current = store_.Read(block.address, static_cast<std::uint64_t>(block.before.size()));
            Absorb(run.report, current);

            // 读不出来：无法确认目标没变，同样按"目标已变"处理，处置都是重新读取。
            if (!current.ok)
            {
                AbortToStaged(run, CommitOutcome::TargetChanged, "pre-write re-read failed: " + current.failureText);
                return false;
            }

            // 只读到一部分：缺的那部分没法核对。
            if (current.partial)
            {
                AbortToStaged(run, CommitOutcome::TargetChanged, "pre-write re-read was partial");
                return false;
            }

            // 字节与暂存时的原字节不同：目标在暂存之后被别人改过。
            if (current.data != block.before)
            {
                AbortToStaged(run, CommitOutcome::TargetChanged, "target bytes differ from the staged original bytes");
                return false;
            }
        }

        return true;
    }

    // WriteAndVerifyAll：(f)(g)(h) 逐块写入并回读，全部通过则状态 Committed。
    // 每块"写、回读、吸收"做完才开始下一块，因此某块失败时后面的块一个字节都没写。
    // 传入：run 本次提交的局部状态。传出：true 表示全部成功；false 表示已中止，报告已填好。
    bool MemoryWriteTransaction::WriteAndVerifyAll(CommitRun& run)
    {
        for (std::size_t index = 0; index < run.blocks.size(); ++index)
        {
            if (!WriteOneBlock(run, index))
            {
                return false;
            }

            if (!VerifyOneBlock(run, index))
            {
                return false;
            }
        }

        state_ = State::Committed;
        run.report.outcome = CommitOutcome::Committed;
        return true;
    }

    // WriteOneBlock：(f) 写一个块，必要时询问显式同意并重试。
    // 同意标志只附在"后端刚刚要求同意"的那次重试上：ThisBlockOnly 只管这一块；
    // RestOfBatch 记在 run 里（局部），后面的块后端再要求时不再询问，但仍由后端先提出。
    // 传入：run 本次提交的局部状态；index 块序号。传出：true 表示写入成功。
    bool MemoryWriteTransaction::WriteOneBlock(CommitRun& run, const std::size_t index)
    {
        state_ = State::Writing;
        CommitReport& report = run.report;
        const DiffBlock& block = run.blocks[index];
        const std::uint64_t length = static_cast<std::uint64_t>(block.after.size());
        // persistedBefore：此前各块留在目标上的字节数，判断能不能说"整体已回滚"。
        const std::uint64_t persistedBefore = report.bytesWritten;

        // 第一次尝试一律不带同意标志，让后端自己决定要不要同意。
        AccessResult result = store_.Write(block.address, block.after, false);
        Absorb(report, result);

        // 后端要求显式同意。
        if (result.needsExplicitApproval)
        {
            // 本次提交还没有"剩余块都同意"的授权，就必须问用户。
            // 这一步永远会问，不看 uiConfirmSuppressed_。
            if (!run.batchApproved)
            {
                ApprovalRequest request;
                request.targetIdentity = run.targetIdentity;
                request.blockIndex = static_cast<std::uint64_t>(index);
                request.blocksTotal = report.blocksTotal;
                request.address = block.address;
                request.length = length;
                request.backendText = result.failureText;

                ++report.approvalsAsked;
                const ApprovalAnswer answer = confirmation_.ConfirmApproval(request);
                RecordApproval(run, index, block, answer);

                // 只有两个明确的"同意"值放行；Deny 与任何未识别的取值一律按拒绝处理。
                if (answer == ApprovalAnswer::RestOfBatch)
                {
                    run.batchApproved = true;
                }
                else if (answer != ApprovalAnswer::ThisBlockOnly)
                {
                    AbortWithFailure(
                        run,
                        CommitOutcome::ApprovalDenied,
                        "user denied the explicit approval requested by the backend",
                        false);
                    return false;
                }
            }

            // 带同意标志重试同一块。同意只在这一次调用里有效，下一块重新从"不带标志"开始。
            result = store_.Write(block.address, block.after, true);
            Absorb(report, result);

            // 已经同意了后端还要求同意：不再询问，避免死循环，按写入失败处理。
            if (result.needsExplicitApproval)
            {
                AbortWithFailure(
                    run,
                    CommitOutcome::WriteFailed,
                    "backend requested approval again after it was granted",
                    false);
                return false;
            }
        }

        // 写入失败或只写了一部分：报告留在目标上的字节数，被后端回滚的不算。
        if (!result.ok || result.partial)
        {
            if (!result.rolledBack)
            {
                report.bytesWritten += std::min(result.bytesDone, length);
            }

            // 只有此前没有任何块落地，才能说目标没被这次提交改动（RolledBack）。
            const bool cleanRollback = result.rolledBack && (persistedBefore == 0);
            AbortWithFailure(run, CommitOutcome::WriteFailed, "write failed: " + result.failureText, cleanRollback);
            return false;
        }

        // 写入成功：整块字节都留在目标上了。
        report.bytesWritten += length;
        return true;
    }

    // VerifyOneBlock：(g)(h) 回读刚写入的块，通过后立即交给 overlay 吸收。
    // 传入：run 本次提交的局部状态；index 块序号。传出：true 表示回读通过且已吸收。
    bool MemoryWriteTransaction::VerifyOneBlock(CommitRun& run, const std::size_t index)
    {
        state_ = State::Verifying;
        const DiffBlock& block = run.blocks[index];

        // 回读。读失败、partial、与 after 不一致都算失败，不能把"读不出来"当成"写对了"。
        const AccessResult readBack = store_.Read(block.address, static_cast<std::uint64_t>(block.after.size()));
        Absorb(run.report, readBack);
        if (!readBack.ok || readBack.partial || readBack.data != block.after)
        {
            AbortWithFailure(
                run,
                CommitOutcome::VerifyMismatch,
                "read-back verification failed: " + readBack.failureText,
                false);
            return false;
        }

        // 回读通过：用回读字节让 overlay 吸收这一块（更新基线、移除补丁、标记自己写入）。
        // 按构造这里不会被拒绝（长度一致、地址不溢出、非空）；万一被拒绝，说明叠加层
        // 与目标已经对不上，必须当作失败而不是悄悄吞掉。
        const AcceptWriteStatus accepted = overlay_.AcceptWrite(block, readBack.data);
        if (accepted != AcceptWriteStatus::Ok)
        {
            AbortWithFailure(
                run,
                CommitOutcome::VerifyMismatch,
                "overlay refused to accept the verified write",
                false);
            return false;
        }

        ++run.report.blocksWritten;
        return true;
    }

    // AbortToStaged：写阶段之前的中止（用户取消、Stale、TargetChanged）。
    // 传入：run 本次提交的局部状态；outcome 结果；text 失败原因。
    // 传出：填好报告；状态放回 Staged（overlay 里没有补丁了则是 Idle）。补丁原样保留。
    void MemoryWriteTransaction::AbortToStaged(
        CommitRun& run,
        const CommitOutcome outcome,
        const std::string& text)
    {
        run.report.outcome = outcome;
        run.report.failureText = text;
        state_ = overlay_.HasPendingPatches() ? State::Staged : State::Idle;
    }

    // AbortWithFailure：写阶段的中止（拒绝同意、写失败、回读失败）。
    // 传入：run 本次提交的局部状态；outcome 结果；text 失败原因；
    //       rolledBack 为 true 表示目标没有被这次提交改动，状态记为 RolledBack，否则 Failed。
    // 传出：填好报告。尚未写入的块的补丁一个都没动，用户的输入不会因为失败丢掉。
    void MemoryWriteTransaction::AbortWithFailure(
        CommitRun& run,
        const CommitOutcome outcome,
        const std::string& text,
        const bool rolledBack)
    {
        run.report.outcome = outcome;
        run.report.failureText = text;
        state_ = rolledBack ? State::RolledBack : State::Failed;
    }

    // RecordStarted：记"开始"审计。每次 Commit 调用都有，含 NoChange 与 Busy。
    void MemoryWriteTransaction::RecordStarted(const CommitRun& run)
    {
        AuditRecord record;
        record.event = AuditEvent::CommitStarted;
        record.targetIdentity = run.targetIdentity;
        record.text = "commit started";
        audit_.Record(record);
    }

    // RecordConfirm：记"界面确认"审计（同意 / 拒绝 / 被设置抑制）。
    // 传入：run 本次提交；event 事件种类；text 说明。
    void MemoryWriteTransaction::RecordConfirm(
        const CommitRun& run,
        const AuditEvent event,
        const std::string& text)
    {
        AuditRecord record;
        record.event = event;
        record.targetIdentity = run.targetIdentity;
        record.blocksTotal = run.report.blocksTotal;
        record.text = text;
        audit_.Record(record);
    }

    // RecordApproval：每次 ConfirmApproval 之后记一条审计，带回答。
    // 传入：run 本次提交；index 块序号；block 当前块；answer 用户回答。
    void MemoryWriteTransaction::RecordApproval(
        const CommitRun& run,
        const std::size_t index,
        const DiffBlock& block,
        const ApprovalAnswer answer)
    {
        AuditRecord record;
        record.event = AuditEvent::ApprovalAnswered;
        record.targetIdentity = run.targetIdentity;
        record.blocksTotal = run.report.blocksTotal;
        record.blockIndex = static_cast<std::uint64_t>(index);
        record.address = block.address;
        record.length = static_cast<std::uint64_t>(block.after.size());
        record.approvalAnswer = answer;
        record.text = "explicit approval answered";
        audit_.Record(record);
    }

    // RecordFinished：记"结束"审计，带结果与计数。每个出口都有。
    void MemoryWriteTransaction::RecordFinished(const CommitRun& run)
    {
        AuditRecord record;
        record.event = AuditEvent::CommitFinished;
        record.targetIdentity = run.targetIdentity;
        record.blocksTotal = run.report.blocksTotal;
        record.outcome = run.report.outcome;
        record.blocksWritten = run.report.blocksWritten;
        record.bytesWritten = run.report.bytesWritten;
        record.scratchAreaDirty = run.report.scratchAreaDirty;
        record.readModifyWriteWindow = run.report.readModifyWriteWindow;
        record.text = run.report.failureText;
        audit_.Record(record);
    }
}
