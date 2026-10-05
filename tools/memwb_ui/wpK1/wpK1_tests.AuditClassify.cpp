// ============================================================
// wpK1_tests.AuditClassify.cpp
// 作用：WorkbenchServicesAudit 里"审计记录 -> 日志级别"与三个枚举名函数的逐分支断言。
// 重点：每个 AuditEvent 都有明确级别（跳过 UI 确认不跳过审计）；风险标志只升级不降级；
//       越界枚举值取保守级别，不能借默认值被报成 Info。
// ============================================================

#include "wpK1_common.h"

#include "../../../Ksword5.1/Ksword5.1/MemoryDock/WorkbenchServicesAudit.h"

#include <set>
#include <string>

namespace svc = ksword::memwb_services_detail;

namespace
{
    // MakeRecord：构造一条给定事件的审计记录。
    ksword::memwb::AuditRecord MakeRecord(const ksword::memwb::AuditEvent event)
    {
        ksword::memwb::AuditRecord record;
        record.event = event;
        record.targetIdentity = "memwb-target/1|scope=1|pid=0|ct=0|gen=0|ch=1|ddma=0|bits=64|base=0x0|len=0";
        return record;
    }

    // MakeFinished：构造一条 CommitFinished 记录。
    ksword::memwb::AuditRecord MakeFinished(
        const ksword::memwb::CommitOutcome outcome,
        const bool scratchDirty,
        const bool rmwWindow)
    {
        ksword::memwb::AuditRecord record = MakeRecord(ksword::memwb::AuditEvent::CommitFinished);
        record.outcome = outcome;
        record.scratchAreaDirty = scratchDirty;
        record.readModifyWriteWindow = rmwWindow;
        return record;
    }

    // TestNonFinishedEvents：五个非结束事件的级别。
    void TestNonFinishedEvents()
    {
        using ksword::memwb::AuditEvent;
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeRecord(AuditEvent::CommitStarted)) == svc::AuditLogLevel::Info);
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeRecord(AuditEvent::UiConfirmAccepted)) == svc::AuditLogLevel::Info);
        // 确认被设置抑制：只是留痕，Info（"不弹框不等于不审计"）。
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeRecord(AuditEvent::UiConfirmSuppressed)) == svc::AuditLogLevel::Info);
        // 用户拒绝：Warn。
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeRecord(AuditEvent::UiConfirmDenied)) == svc::AuditLogLevel::Warn);

        // 强制同意回答：Deny -> Warn；两种同意 -> Info。
        ksword::memwb::AuditRecord approval = MakeRecord(AuditEvent::ApprovalAnswered);
        approval.approvalAnswer = ksword::memwb::ApprovalAnswer::Deny;
        WPK1_CHECK(svc::ClassifyAuditRecord(approval) == svc::AuditLogLevel::Warn);
        approval.approvalAnswer = ksword::memwb::ApprovalAnswer::ThisBlockOnly;
        WPK1_CHECK(svc::ClassifyAuditRecord(approval) == svc::AuditLogLevel::Info);
        approval.approvalAnswer = ksword::memwb::ApprovalAnswer::RestOfBatch;
        WPK1_CHECK(svc::ClassifyAuditRecord(approval) == svc::AuditLogLevel::Info);

        // 非结束事件携带结束字段（异常记录）不影响级别：事件决定级别。
        ksword::memwb::AuditRecord started = MakeRecord(AuditEvent::CommitStarted);
        started.outcome = ksword::memwb::CommitOutcome::WriteFailed;
        started.scratchAreaDirty = true;
        WPK1_CHECK(svc::ClassifyAuditRecord(started) == svc::AuditLogLevel::Info);

        // 越界事件值：没有结果语义，取保守级别 Warn（不能借默认 NoChange 变成 Info）。
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeRecord(static_cast<AuditEvent>(99))) == svc::AuditLogLevel::Warn);
    }

    // TestFinishedOutcomes：CommitFinished 的十种结果与越界值，无风险标志。
    void TestFinishedOutcomes()
    {
        using ksword::memwb::CommitOutcome;
        const auto level = [](const CommitOutcome outcome) {
            return svc::ClassifyAuditRecord(MakeFinished(outcome, false, false));
        };
        WPK1_CHECK(level(CommitOutcome::NoChange) == svc::AuditLogLevel::Info);
        WPK1_CHECK(level(CommitOutcome::Committed) == svc::AuditLogLevel::Info);

        WPK1_CHECK(level(CommitOutcome::UserCancelled) == svc::AuditLogLevel::Warn);
        WPK1_CHECK(level(CommitOutcome::Stale) == svc::AuditLogLevel::Warn);
        WPK1_CHECK(level(CommitOutcome::ApprovalDenied) == svc::AuditLogLevel::Warn);
        WPK1_CHECK(level(CommitOutcome::Busy) == svc::AuditLogLevel::Warn);

        WPK1_CHECK(level(CommitOutcome::TargetChanged) == svc::AuditLogLevel::Error);
        WPK1_CHECK(level(CommitOutcome::WriteFailed) == svc::AuditLogLevel::Error);
        WPK1_CHECK(level(CommitOutcome::VerifyMismatch) == svc::AuditLogLevel::Error);
        WPK1_CHECK(level(CommitOutcome::InvalidSession) == svc::AuditLogLevel::Error);

        // 越界结果值：Warn。
        WPK1_CHECK(level(static_cast<CommitOutcome>(99)) == svc::AuditLogLevel::Warn);
    }

    // TestRiskFlags：风险标志只升级不降级。
    void TestRiskFlags()
    {
        using ksword::memwb::CommitOutcome;

        // 暂存区未还原：无论成败都是 Error（成功也要最醒目地留痕）。
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::Committed, true, false)) ==
            svc::AuditLogLevel::Error);
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::NoChange, true, false)) ==
            svc::AuditLogLevel::Error);
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::UserCancelled, true, false)) ==
            svc::AuditLogLevel::Error);
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::WriteFailed, true, false)) ==
            svc::AuditLogLevel::Error);

        // 读-改-写窗口：把 Info 提升为 Warn……
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::Committed, false, true)) ==
            svc::AuditLogLevel::Warn);
        // ……但绝不降级：Error 仍是 Error，Warn 仍是 Warn。
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::VerifyMismatch, false, true)) ==
            svc::AuditLogLevel::Error);
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::Stale, false, true)) ==
            svc::AuditLogLevel::Warn);

        // 两个标志同时为真：取最高（Error）。
        WPK1_CHECK(svc::ClassifyAuditRecord(MakeFinished(CommitOutcome::Committed, true, true)) ==
            svc::AuditLogLevel::Error);

        // 风险标志只在 CommitFinished 上生效：其它事件携带标志不升级（已在上面断言）。
    }

    // TestNames：三个枚举名函数——名字稳定、互不相同、越界为 Unknown。
    void TestNames()
    {
        using ksword::memwb::ApprovalAnswer;
        using ksword::memwb::AuditEvent;
        using ksword::memwb::CommitOutcome;

        const AuditEvent events[] = {
            AuditEvent::CommitStarted, AuditEvent::UiConfirmAccepted, AuditEvent::UiConfirmDenied,
            AuditEvent::UiConfirmSuppressed, AuditEvent::ApprovalAnswered, AuditEvent::CommitFinished};
        std::set<std::string> eventNames;
        for (const AuditEvent event : events)
        {
            const std::string name = svc::AuditEventName(event);
            WPK1_CHECK(name != "Unknown");
            eventNames.insert(name);
        }
        // 六个事件名两两不同。
        WPK1_CHECK(eventNames.size() == 6U);
        WPK1_CHECK(std::string(svc::AuditEventName(AuditEvent::CommitStarted)) == "CommitStarted");
        WPK1_CHECK(std::string(svc::AuditEventName(AuditEvent::CommitFinished)) == "CommitFinished");
        WPK1_CHECK(std::string(svc::AuditEventName(static_cast<AuditEvent>(99))) == "Unknown");

        const CommitOutcome outcomes[] = {
            CommitOutcome::NoChange, CommitOutcome::Committed, CommitOutcome::UserCancelled,
            CommitOutcome::Stale, CommitOutcome::TargetChanged, CommitOutcome::ApprovalDenied,
            CommitOutcome::WriteFailed, CommitOutcome::VerifyMismatch, CommitOutcome::InvalidSession,
            CommitOutcome::Busy};
        std::set<std::string> outcomeNames;
        for (const CommitOutcome outcome : outcomes)
        {
            const std::string name = svc::CommitOutcomeName(outcome);
            WPK1_CHECK(name != "Unknown");
            outcomeNames.insert(name);
        }
        WPK1_CHECK(outcomeNames.size() == 10U);
        WPK1_CHECK(std::string(svc::CommitOutcomeName(CommitOutcome::VerifyMismatch)) == "VerifyMismatch");
        WPK1_CHECK(std::string(svc::CommitOutcomeName(static_cast<CommitOutcome>(99))) == "Unknown");

        WPK1_CHECK(std::string(svc::ApprovalAnswerName(ApprovalAnswer::Deny)) == "Deny");
        WPK1_CHECK(std::string(svc::ApprovalAnswerName(ApprovalAnswer::ThisBlockOnly)) == "ThisBlockOnly");
        WPK1_CHECK(std::string(svc::ApprovalAnswerName(ApprovalAnswer::RestOfBatch)) == "RestOfBatch");
        WPK1_CHECK(std::string(svc::ApprovalAnswerName(static_cast<ApprovalAnswer>(99))) == "Unknown");
    }
}

namespace wpK1_test
{
    void RunAuditClassifyTests()
    {
        TestNonFinishedEvents();
        TestFinishedOutcomes();
        TestRiskFlags();
        TestNames();
    }
}
