// 写事务测试套件的第三个文件：显式同意 / 写失败 / 回读失败 / 告警位 / 审计 / 失败后重试。
// 约定与断言原则见 MemoryWriteTransactionTests.cpp 与 MemoryWriteTransactionTestSupport.h。
//
// 本文件的核心是"写之中与写之后"：
//   (f) 逐块写入与显式同意（ThisBlockOnly / RestOfBatch 的区别）
//   (g) 回读校验
//   (h) 成功吸收、失败保留尚未写入块的暂存补丁
// 同意范围的区别用三块夹具证明：第 1 块与第 3 块要求同意，第 2 块不要求。

#include "MemoryWriteTransactionTestSupport.h"

namespace {

using namespace MemwbTxnTests;

// 取补丁块的起始地址序列，便于一行断言"哪几块还留着"。
std::vector<std::uint64_t> PatchAddresses(const Rig& rig) {
    std::vector<std::uint64_t> addresses;
    for (const DiffBlock& block : rig.overlay.DiffBlocks()) {
        addresses.push_back(block.address);
    }
    return addresses;
}

// ------------------------------------------------------------
// 一、三块全部成功：调用序列、计数与叠加层吸收。
// ------------------------------------------------------------
void TestThreeBlockSuccess(KswordTests::Suite& suite) {
    Rig rig;
    rig.StageThree();
    const CommitReport report = rig.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1", "R:1006+2", "R:100c+1",
        "W:1002+1:a0", "R:1002+1",
        "W:1006+2:a0", "R:1006+2",
        "W:100c+1:a0", "R:100c+1",
        "AUDIT:CommitFinished",
    };
    suite.expect(rig.log == expected, L"success: three blocks are re-read first, then written and verified one by one");
    suite.expect(report.outcome == CommitOutcome::Committed && report.blocksTotal == 3 && report.blocksWritten == 3
        && report.bytesWritten == 4,
        L"success: the report counts 3 of 3 blocks and 4 bytes");
    suite.expect(rig.txn.CurrentState() == State::Committed && !rig.overlay.HasPendingPatches(),
        L"success: the state is Committed and every patch was absorbed");

    // 叠加层视图：基线被回读字节更新，手算 00 11 01 33 44 55 02 03 88 99 AA BB 04 DD EE FF。
    const ksword::memwb::MaterializedBytes view = rig.overlay.Materialize(kBase, 16);
    const Bytes expectedView = {
        0x00, 0x11, 0x01, 0x33, 0x44, 0x55, 0x02, 0x03, 0x88, 0x99, 0xAA, 0xBB, 0x04, 0xDD, 0xEE, 0xFF};
    suite.expect(view.ok && view.bytes == expectedView, L"success: the overlay view matches what the target now holds");
    suite.expect(MemoryAt(rig, kBase, 16) == expectedView, L"success: and the target memory holds the same 16 bytes");
}

// ------------------------------------------------------------
// 二、显式同意：ThisBlockOnly 只管一块；RestOfBatch 管到本次提交结束；Deny 中止。
// ------------------------------------------------------------
void TestApprovalThisBlockOnly(KswordTests::Suite& suite) {
    // 第 1、3 块要求同意，两次都只答 ThisBlockOnly：第 3 块必须重新询问。
    Rig rig;
    rig.StageThree();
    rig.store.needApproval = {kA, kC};
    rig.sink.approvalScript = {ApprovalAnswer::ThisBlockOnly, ApprovalAnswer::ThisBlockOnly};
    const CommitReport report = rig.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1", "R:1006+2", "R:100c+1",
        "W:1002+1:a0", "APPROVE:0", "AUDIT:ApprovalAnswered", "W:1002+1:a1", "R:1002+1",
        "W:1006+2:a0", "R:1006+2",
        "W:100c+1:a0", "APPROVE:2", "AUDIT:ApprovalAnswered", "W:100c+1:a1", "R:100c+1",
        "AUDIT:CommitFinished",
    };
    suite.expect(rig.log == expected,
        L"approval: ThisBlockOnly covers one block only, the middle block is written without the flag, the third asks again");
    suite.expect(report.outcome == CommitOutcome::Committed && report.approvalsAsked == 2
        && rig.sink.approvalCalls == 2 && report.blocksWritten == 3,
        L"approval: two questions were asked and all three blocks were written");

    // 询问内容：块序号、总块数、地址、长度与后端附带的说明。
    suite.expect(rig.sink.approvals[0].blockIndex == 0 && rig.sink.approvals[0].blocksTotal == 3
        && rig.sink.approvals[0].address == 0x1002ULL && rig.sink.approvals[0].length == 1,
        L"approval: the first question describes block 0 at 0x1002");
    suite.expect(rig.sink.approvals[1].blockIndex == 2 && rig.sink.approvals[1].address == 0x100CULL
        && rig.sink.approvals[1].length == 1,
        L"approval: the second question describes block 2 at 0x100C");
    suite.expect(rig.sink.approvals[0].backendText == "needs explicit approval"
        && rig.sink.approvals[0].targetIdentity == kExpectedIdentity,
        L"approval: the question carries the backend's text and the target identity");

    // 每次询问一条审计，带回答与块信息。
    suite.expect(rig.audit.Count(AuditEvent::ApprovalAnswered) == 2, L"approval: every question is audited once");
    const std::vector<AuditRecord> records = rig.audit.records;
    std::vector<AuditRecord> answers;
    for (const AuditRecord& record : records) {
        if (record.event == AuditEvent::ApprovalAnswered) {
            answers.push_back(record);
        }
    }
    suite.expect(answers.size() == 2 && answers[0].blockIndex == 0 && answers[0].address == 0x1002ULL
        && answers[0].length == 1 && answers[0].approvalAnswer == ApprovalAnswer::ThisBlockOnly,
        L"approval: the first audit record names block 0 and the ThisBlockOnly answer");
    suite.expect(answers.size() == 2 && answers[1].blockIndex == 2 && answers[1].address == 0x100CULL
        && answers[1].approvalAnswer == ApprovalAnswer::ThisBlockOnly,
        L"approval: the second audit record names block 2");
}

void TestApprovalRestOfBatch(KswordTests::Suite& suite) {
    // 同样的三块，第一次答 RestOfBatch：第 3 块不再询问，但仍由后端先提出、再带标志重试。
    Rig rig;
    rig.StageThree();
    rig.store.needApproval = {kA, kC};
    rig.sink.approvalScript = {ApprovalAnswer::RestOfBatch};
    const CommitReport report = rig.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1", "R:1006+2", "R:100c+1",
        "W:1002+1:a0", "APPROVE:0", "AUDIT:ApprovalAnswered", "W:1002+1:a1", "R:1002+1",
        "W:1006+2:a0", "R:1006+2",
        "W:100c+1:a0", "W:100c+1:a1", "R:100c+1",
        "AUDIT:CommitFinished",
    };
    suite.expect(rig.log == expected,
        L"approval: RestOfBatch asks once and the third block is retried with the flag without asking again");
    suite.expect(report.outcome == CommitOutcome::Committed && report.approvalsAsked == 1
        && rig.sink.approvalCalls == 1 && report.blocksWritten == 3,
        L"approval: RestOfBatch means exactly one question for the whole batch");
    suite.expect(rig.audit.Count(AuditEvent::ApprovalAnswered) == 1
        && rig.audit.records[2].event == AuditEvent::ApprovalAnswered
        && rig.audit.records[2].approvalAnswer == ApprovalAnswer::RestOfBatch,
        L"approval: the single question is audited with the RestOfBatch answer");

    // RestOfBatch 的范围到本次 Commit 结束为止：下一次 Commit 的新块要求同意时必须重新询问。
    rig.store.needApproval.insert(0x1004ULL);
    rig.sink.approvalScript = {ApprovalAnswer::RestOfBatch, ApprovalAnswer::Deny};
    suite.expect(rig.txn.Stage(0x1004ULL, Bytes{0x05}) == StageStatus::Ok, L"approval: a new block is staged");
    const CommitReport second = rig.txn.Commit();
    suite.expect(second.outcome == CommitOutcome::ApprovalDenied && rig.sink.approvalCalls == 2,
        L"approval: a RestOfBatch grant does not carry over, the next commit asks again and is denied");
    suite.expect(second.approvalsAsked == 1 && MemoryAt(rig, 0x1004ULL, 1) == Bytes{0x44},
        L"approval: the per-commit question counter restarts and the denied byte stays unwritten");
}

void TestApprovalDeny(KswordTests::Suite& suite) {
    // 第 1 块就被拒绝：零个字节落地，三块补丁都在。
    Rig first;
    first.StageThree();
    first.store.needApproval = {kA, kC};
    first.sink.approvalScript = {ApprovalAnswer::Deny};
    const CommitReport denied = first.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1", "R:1006+2", "R:100c+1",
        "W:1002+1:a0", "APPROVE:0", "AUDIT:ApprovalAnswered",
        "AUDIT:CommitFinished",
    };
    suite.expect(first.log == expected, L"deny: a denial stops right after the question, no retry and no further block");
    suite.expect(denied.outcome == CommitOutcome::ApprovalDenied && first.txn.CurrentState() == State::Failed,
        L"deny: the outcome is ApprovalDenied and the state is Failed");
    suite.expect(denied.blocksWritten == 0 && denied.bytesWritten == 0 && !denied.needsReread
        && denied.approvalsAsked == 1 && !denied.failureText.empty(),
        L"deny: nothing was written, so no re-read is needed");
    suite.expect(MemoryAt(first, kA, 1) == Bytes{0x22} && first.store.writeCalls == 1,
        L"deny: the target is untouched");
    suite.expect(PatchAddresses(first) == std::vector<std::uint64_t>({kA, kB, kC}),
        L"deny: all three staged patches are kept");
    suite.expect(first.audit.records.back().outcome == CommitOutcome::ApprovalDenied,
        L"deny: the finish audit carries ApprovalDenied");

    // 第 3 块被拒绝：前两块已落地并被吸收，第 3 块的补丁保留。
    Rig third;
    third.StageThree();
    third.store.needApproval = {kA, kC};
    third.sink.approvalScript = {ApprovalAnswer::ThisBlockOnly, ApprovalAnswer::Deny};
    const CommitReport partial = third.txn.Commit();
    suite.expect(partial.outcome == CommitOutcome::ApprovalDenied && partial.blocksWritten == 2
        && partial.bytesWritten == 3 && partial.needsReread,
        L"deny: a late denial reports 2 written blocks, 3 bytes and asks for a re-read");
    suite.expect(PatchAddresses(third) == std::vector<std::uint64_t>({kC}) && MemoryAt(third, kC, 1) == Bytes{0xCC},
        L"deny: only the denied block keeps its patch and its target byte");
    suite.expect(MemoryAt(third, kA, 1) == Bytes{0x01} && MemoryAt(third, kB, 2) == (Bytes{0x02, 0x03}),
        L"deny: the blocks written before the denial stay written");
    suite.expect(third.txn.CurrentState() == State::Failed, L"deny: a late denial also ends Failed");

    // 没有给出回答（脚本用完）一律按拒绝。
    Rig silent;
    silent.StageA();
    silent.store.needApproval = {kA};
    suite.expect(silent.txn.Commit().outcome == CommitOutcome::ApprovalDenied && silent.sink.approvalCalls == 1,
        L"deny: a sink that has no answer denies by default");

    // 未识别的回答值按拒绝处理：失败即拒绝，而不是失败即放行。
    Rig garbage;
    garbage.StageA();
    garbage.store.needApproval = {kA};
    garbage.sink.approvalScript = {static_cast<ApprovalAnswer>(99)};
    const CommitReport garbageReport = garbage.txn.Commit();
    suite.expect(garbageReport.outcome == CommitOutcome::ApprovalDenied && garbage.store.writeCalls == 1
        && MemoryAt(garbage, kA, 1) == Bytes{0x22},
        L"deny: an unrecognised answer value is treated as a denial and nothing is written");
}

void TestApprovalIgnoresUiSuppression(KswordTests::Suite& suite) {
    // 抑制了界面确认，显式同意照问不误：给同意则写，不给则不写。
    Rig granted;
    granted.txn.SetUiConfirmSuppressed(true);
    granted.StageA();
    granted.store.needApproval = {kA};
    granted.sink.approvalScript = {ApprovalAnswer::ThisBlockOnly};
    const CommitReport report = granted.txn.Commit();
    suite.expect(granted.sink.uiCalls == 0 && granted.sink.approvalCalls == 1,
        L"suppress: the ui confirmation is skipped but the explicit approval is still asked");
    suite.expect(report.outcome == CommitOutcome::Committed && report.approvalsAsked == 1
        && MemoryAt(granted, kA, 1) == Bytes{0x01},
        L"suppress: with the approval granted the byte is written");

    Rig refused;
    refused.txn.SetUiConfirmSuppressed(true);
    refused.StageA();
    refused.store.needApproval = {kA};
    const CommitReport refusedReport = refused.txn.Commit();
    suite.expect(refusedReport.outcome == CommitOutcome::ApprovalDenied && refused.sink.approvalCalls == 1
        && MemoryAt(refused, kA, 1) == Bytes{0x22},
        L"suppress: suppressing the ui confirmation never turns a missing approval into a write");

    // 审计：抑制时同样每次询问一条，另有一条抑制记录，条数不少于未抑制时。
    Rig loud;
    loud.StageA();
    loud.store.needApproval = {kA};
    loud.sink.approvalScript = {ApprovalAnswer::ThisBlockOnly};
    (void)loud.txn.Commit();
    suite.expect(granted.audit.Count(AuditEvent::ApprovalAnswered) == 1 && loud.audit.Count(AuditEvent::ApprovalAnswered) == 1
        && granted.audit.records.size() >= loud.audit.records.size(),
        L"suppress: the approval is audited the same way with or without suppression");
}

void TestApprovalProtocolEdges(KswordTests::Suite& suite) {
    // 已经同意了后端还要求同意：不再询问，按写入失败处理，不死循环。
    Rig rig;
    rig.StageA();
    rig.store.alwaysNeedApproval = {kA};
    rig.sink.approvalScript = {ApprovalAnswer::ThisBlockOnly, ApprovalAnswer::ThisBlockOnly};
    const CommitReport report = rig.txn.Commit();
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1002+1",
        "W:1002+1:a0", "APPROVE:0", "AUDIT:ApprovalAnswered", "W:1002+1:a1",
        "AUDIT:CommitFinished",
    };
    suite.expect(rig.log == expected, L"approval: a backend that keeps asking is asked about once and then fails the write");
    suite.expect(report.outcome == CommitOutcome::WriteFailed && rig.sink.approvalCalls == 1
        && rig.txn.CurrentState() == State::Failed && PatchAddresses(rig) == std::vector<std::uint64_t>({kA}),
        L"approval: that failure ends Failed and keeps the patch");

    // RestOfBatch 之后后端仍然要求同意：同样失败，而不是无限重试。
    Rig batch;
    batch.StageA();
    batch.store.alwaysNeedApproval = {kA};
    batch.sink.approvalScript = {ApprovalAnswer::RestOfBatch};
    suite.expect(batch.txn.Commit().outcome == CommitOutcome::WriteFailed && batch.store.writeCalls == 2,
        L"approval: a backend that ignores a batch approval fails after exactly two write attempts");
}

// ------------------------------------------------------------
// 三、(f) 写失败：留在目标上的字节数、回滚标签、保留尚未写入块的补丁。
// ------------------------------------------------------------
void TestWriteFailures(KswordTests::Suite& suite) {
    // B 写了一个字节后失败：A 已落地被吸收，B、C 的补丁保留，C 一次都没尝试。
    Rig rig;
    rig.StageThree();
    WriteFault fault;
    fault.fail = true;
    fault.bytesDone = 1;
    fault.text = "disk error";
    rig.store.writeFault[kB] = fault;
    const CommitReport report = rig.txn.Commit();
    suite.expect(report.outcome == CommitOutcome::WriteFailed && rig.txn.CurrentState() == State::Failed,
        L"write failure: the outcome is WriteFailed and the state is Failed");
    suite.expect(report.blocksTotal == 3 && report.blocksWritten == 1 && report.bytesWritten == 2,
        L"write failure: one verified block plus one persisted partial byte make 2 bytes");
    suite.expect(report.needsReread && report.failureText.find("disk error") != std::string::npos,
        L"write failure: the report asks for a re-read and carries the store's text");
    suite.expect(MemoryAt(rig, kB, 2) == (Bytes{0x02, 0x77}),
        L"write failure: exactly the one partial byte reached the target");
    suite.expect(rig.store.writeCalls == 2 && rig.store.readCalls == 4,
        L"write failure: block C was never attempted and the failed block was not read back");
    suite.expect(PatchAddresses(rig) == std::vector<std::uint64_t>({kB, kC}),
        L"write failure: the patches of the failed block and the untouched block are kept");
    const std::vector<DiffBlock> kept = rig.overlay.DiffBlocks();
    suite.expect(kept[0].before == (Bytes{0x66, 0x77}) && kept[0].after == (Bytes{0x02, 0x03})
        && kept[1].before == Bytes{0xCC} && kept[1].after == Bytes{0x04},
        L"write failure: the kept patches still hold their original and new bytes");
    suite.expect(rig.overlay.BaselineByte(kA) == Val(0x01)
        && rig.overlay.ChangeKind(kA) == ksword::memwb::ByteChangeKind::SelfWritten,
        L"write failure: the block written before the failure was absorbed by the overlay");
    suite.expect(rig.audit.records.back().outcome == CommitOutcome::WriteFailed
        && rig.audit.records.back().blocksWritten == 1 && rig.audit.records.back().bytesWritten == 2,
        L"write failure: the finish audit carries the outcome and the counts");

    // 单块失败且后端已回滚：RolledBack，回滚掉的字节不算留在目标上。
    Rig single;
    single.StageA();
    WriteFault rolled;
    rolled.fail = true;
    rolled.bytesDone = 1;
    rolled.rolledBack = true;
    single.store.writeFault[kA] = rolled;
    const CommitReport rolledReport = single.txn.Commit();
    suite.expect(rolledReport.outcome == CommitOutcome::WriteFailed && single.txn.CurrentState() == State::RolledBack,
        L"rollback: a failed write that the backend rolled back ends RolledBack");
    suite.expect(rolledReport.bytesWritten == 0 && rolledReport.blocksWritten == 0
        && MemoryAt(single, kA, 1) == Bytes{0x22},
        L"rollback: a rolled-back byte is not counted and the target is unchanged");
    suite.expect(PatchAddresses(single) == std::vector<std::uint64_t>({kA}) && rolledReport.needsReread,
        L"rollback: the patch is kept and a re-read is still requested");

    // 更早的块已落地：即使后面的块被回滚，也不能声称"目标没被改动"。
    Rig later;
    later.StageThree();
    later.store.writeFault[kB] = rolled;
    const CommitReport laterReport = later.txn.Commit();
    suite.expect(laterReport.outcome == CommitOutcome::WriteFailed && later.txn.CurrentState() == State::Failed,
        L"rollback: a rollback after an earlier block landed is Failed, not RolledBack");
    suite.expect(laterReport.blocksWritten == 1 && laterReport.bytesWritten == 1,
        L"rollback: the earlier block is still counted as written");

    // ok 但 partial：写了一半也是失败，写入的字节数按声明计。
    Rig partialOk;
    partialOk.StageThree();
    WriteFault half;
    half.partialOk = true;
    half.bytesDone = 1;
    partialOk.store.writeFault[kB] = half;
    const CommitReport halfReport = partialOk.txn.Commit();
    suite.expect(halfReport.outcome == CommitOutcome::WriteFailed && halfReport.bytesWritten == 2
        && partialOk.store.writeCalls == 2,
        L"write failure: a write that reports ok but partial is a failure and is not read back");

    // 声明完成字节数超过块长度：按块长度截断，不会多报。
    Rig clamp;
    clamp.StageA();
    WriteFault huge;
    huge.fail = true;
    huge.bytesDone = 99;
    clamp.store.writeFault[kA] = huge;
    suite.expect(clamp.txn.Commit().bytesWritten == 1, L"write failure: a claimed byte count above the block length is clamped");

    // 第一块就失败：后面的块一次都不尝试，三块补丁全保留。
    Rig firstFails;
    firstFails.StageThree();
    firstFails.store.writeFault[kA].fail = true;
    const CommitReport firstReport = firstFails.txn.Commit();
    suite.expect(firstFails.store.writeCalls == 1 && firstReport.blocksWritten == 0 && firstReport.bytesWritten == 0,
        L"write failure: a failing first block stops the commit before any other write");
    suite.expect(PatchAddresses(firstFails) == std::vector<std::uint64_t>({kA, kB, kC}),
        L"write failure: all three patches are kept when the first block fails");
}

// ------------------------------------------------------------
// 四、(g) 回读校验：读失败、partial、与 after 不一致都是失败，且不再写后面的块。
// ------------------------------------------------------------
void TestVerifyFailures(KswordTests::Suite& suite) {
    // 只有 B 的最后一个字节回读不符：必须整段比较。
    Rig rig;
    rig.StageThree();
    rig.store.corruptAfterWrite = {kB};
    const CommitReport report = rig.txn.Commit();
    suite.expect(report.outcome == CommitOutcome::VerifyMismatch && rig.txn.CurrentState() == State::Failed,
        L"verify: a read-back that differs in the last byte is a VerifyMismatch");
    suite.expect(report.blocksWritten == 1 && report.bytesWritten == 3 && report.needsReread,
        L"verify: the mismatching block's bytes are on the target but it is not counted as verified");
    suite.expect(rig.store.writeCalls == 2 && MemoryAt(rig, kC, 1) == Bytes{0xCC},
        L"verify: no block after the mismatch was written");
    suite.expect(MemoryAt(rig, kB, 2) == (Bytes{0x02, 0xFC}), L"verify: the corrupted byte is what the target really holds");
    suite.expect(PatchAddresses(rig) == std::vector<std::uint64_t>({kB, kC}),
        L"verify: the unverified block and the unwritten block keep their patches");
    suite.expect(rig.overlay.BaselineByte(kB) == Val(0x66),
        L"verify: the overlay baseline was not updated for the unverified block");
    const Log tail = {"W:1006+2:a0", "R:1006+2", "AUDIT:CommitFinished"};
    suite.expect(Log(rig.log.end() - 3, rig.log.end()) == tail,
        L"verify: the call log ends with the failed write, its read-back and the finish audit");

    // 第一块回读就不符：一个字节都不再写别的块。
    Rig first;
    first.StageThree();
    first.store.corruptAfterWrite = {kA};
    const CommitReport firstReport = first.txn.Commit();
    suite.expect(firstReport.outcome == CommitOutcome::VerifyMismatch && first.store.writeCalls == 1
        && firstReport.blocksWritten == 0 && firstReport.bytesWritten == 1,
        L"verify: a mismatch on the first block stops everything after it");
    suite.expect(PatchAddresses(first) == std::vector<std::uint64_t>({kA, kB, kC}),
        L"verify: all three patches are kept when the first block does not verify");

    // 回读 partial：即便读到的部分一致也不算通过。
    Rig partial;
    partial.StageA();
    partial.store.readHook = [&partial](std::uint64_t address, std::uint64_t, AccessResult& out) {
        if (partial.store.writeCalls == 0 || address != kA) {
            return false;
        }
        out.ok = true;
        out.partial = true;
        out.data = Bytes{0x01};
        out.bytesDone = 1;
        return true;
    };
    suite.expect(partial.txn.Commit().outcome == CommitOutcome::VerifyMismatch,
        L"verify: a partial read-back fails even when the bytes read match");

    // 回读失败：读不出来不能当成写对了，原因要带出。
    Rig unreadable;
    unreadable.StageA();
    unreadable.store.readHook = [&unreadable](std::uint64_t address, std::uint64_t, AccessResult& out) {
        if (unreadable.store.writeCalls == 0 || address != kA) {
            return false;
        }
        // 数据故意给对：只有"读失败"这一个标志能让它判失败。
        out.ok = false;
        out.data = Bytes{0x01};
        out.bytesDone = 1;
        out.failureText = "read-back failed";
        return true;
    };
    const CommitReport unreadableReport = unreadable.txn.Commit();
    suite.expect(unreadableReport.outcome == CommitOutcome::VerifyMismatch
        && unreadableReport.failureText.find("read-back failed") != std::string::npos,
        L"verify: an unreadable read-back fails and carries the store's text");

    // 写入被悄悄丢弃：后端声称成功，目标上字节没变，回读抓得出来。
    Rig dropped;
    dropped.StageA();
    dropped.store.writeFault[kA] = WriteFault();
    const CommitReport droppedReport = dropped.txn.Commit();
    suite.expect(droppedReport.outcome == CommitOutcome::VerifyMismatch && MemoryAt(dropped, kA, 1) == Bytes{0x22}
        && PatchAddresses(dropped) == std::vector<std::uint64_t>({kA}),
        L"verify: a write the backend silently dropped is caught by the read-back and keeps its patch");
}

// ------------------------------------------------------------
// 五、告警位：scratchAreaDirty 与 readModifyWriteWindow 即使整体成功也必须保持为真。
// ------------------------------------------------------------
void TestWarningBits(KswordTests::Suite& suite) {
    // 只有第一块的写入报告了暂存区脏：后面两块干净的结果不能把它清掉。
    Rig scratch;
    scratch.StageThree();
    scratch.store.dirtyOnWrite = {kA};
    const CommitReport scratchReport = scratch.txn.Commit();
    suite.expect(scratchReport.outcome == CommitOutcome::Committed && scratchReport.scratchAreaDirty,
        L"warning: a dirty scratch area reported by one write survives a fully successful commit");
    suite.expect(!scratchReport.readModifyWriteWindow, L"warning: the other warning bit is not invented");
    suite.expect(scratch.audit.records.back().scratchAreaDirty && !scratch.audit.records.back().readModifyWriteWindow,
        L"warning: the finish audit carries the same warning bits");

    // 只有最后一块的写入走了读-改-写窗口。
    Rig window;
    window.StageThree();
    window.store.rmwOnWrite = {kC};
    const CommitReport windowReport = window.txn.Commit();
    suite.expect(windowReport.outcome == CommitOutcome::Committed && windowReport.readModifyWriteWindow
        && !windowReport.scratchAreaDirty,
        L"warning: a read-modify-write window on the last block is reported on success");

    // 告警位来自读结果（回读）同样要带出。
    Rig reads;
    reads.StageThree();
    reads.store.dirtyOnRead = {kB};
    reads.store.rmwOnRead = {kB};
    const CommitReport readsReport = reads.txn.Commit();
    suite.expect(readsReport.outcome == CommitOutcome::Committed && readsReport.scratchAreaDirty
        && readsReport.readModifyWriteWindow,
        L"warning: warning bits set on read results are carried too");

    // 对照：干净的访问不报任何告警。
    Rig clean;
    clean.StageThree();
    const CommitReport cleanReport = clean.txn.Commit();
    suite.expect(!cleanReport.scratchAreaDirty && !cleanReport.readModifyWriteWindow,
        L"warning: a clean commit reports no warning bits");

    // 失败的提交同样带出告警位。
    Rig failing;
    failing.StageThree();
    failing.store.dirtyOnWrite = {kA};
    failing.store.rmwOnWrite = {kA};
    failing.store.writeFault[kB].fail = true;
    const CommitReport failingReport = failing.txn.Commit();
    suite.expect(failingReport.outcome == CommitOutcome::WriteFailed && failingReport.scratchAreaDirty
        && failingReport.readModifyWriteWindow,
        L"warning: a failed commit still carries the warning bits of the writes that did happen");
}

// ------------------------------------------------------------
// 六、审计：开始与结束各一条，每次询问一条，字段手算写死。
// ------------------------------------------------------------
void TestAuditRecords(KswordTests::Suite& suite) {
    Rig rig;
    rig.StageA();
    (void)rig.txn.Commit();
    const std::vector<AuditEvent> expectedEvents = {
        AuditEvent::CommitStarted, AuditEvent::UiConfirmAccepted, AuditEvent::CommitFinished};
    suite.expect(rig.audit.Events() == expectedEvents,
        L"audit: a confirmed single-block commit audits start, confirmation and finish");
    const AuditRecord& finished = rig.audit.records.back();
    suite.expect(finished.outcome == CommitOutcome::Committed && finished.blocksTotal == 1
        && finished.blocksWritten == 1 && finished.bytesWritten == 1,
        L"audit: the finish record carries outcome Committed and the counts 1 / 1 / 1");
    suite.expect(!finished.scratchAreaDirty && !finished.readModifyWriteWindow,
        L"audit: the finish record of a clean commit carries no warnings");
    suite.expect(rig.audit.records[0].targetIdentity == kExpectedIdentity && rig.audit.records[1].blocksTotal == 1,
        L"audit: records name the target and the confirmation record knows the block count");

    // 多种结果都必须首尾成对。
    struct Case {
        const wchar_t* tag;
        void (*prepare)(Rig&);
    };
    const Case cases[] = {
        {L"nothing staged", [](Rig&) {}},
        {L"user cancelled", [](Rig& r) {
            r.StageA();
            r.sink.uiAnswer = false;
        }},
        {L"invalid session", [](Rig& r) {
            r.StageA();
            r.session.pid = 0;
        }},
        {L"target changed", [](Rig& r) {
            r.StageA();
            r.store.memory[kA] = 0x23;
        }},
        {L"approval denied", [](Rig& r) {
            r.StageA();
            r.store.needApproval = {kA};
        }},
        {L"write failed", [](Rig& r) {
            r.StageA();
            r.store.writeFault[kA].fail = true;
        }},
        {L"verify mismatch", [](Rig& r) {
            r.StageA();
            r.store.corruptAfterWrite = {kA};
        }},
        {L"suppressed", [](Rig& r) {
            r.StageA();
            r.txn.SetUiConfirmSuppressed(true);
        }},
    };
    for (const Case& item : cases) {
        const std::wstring tag = std::wstring(L"audit (") + item.tag + L")";
        Rig each;
        item.prepare(each);
        const CommitReport report = each.txn.Commit();
        ExpectAuditBracketed(suite, tag, each);
        suite.expect(each.audit.records.back().outcome == report.outcome
            && each.audit.records.back().text == report.failureText,
            (tag + L": the finish record repeats the report outcome and failure text").c_str());
    }
}

// ------------------------------------------------------------
// 七、失败后重试：已吸收的块不会被当成"目标已变化"，只重写剩下的。
// ------------------------------------------------------------
void TestRetryAfterFailure(KswordTests::Suite& suite) {
    Rig rig;
    rig.StageThree();
    rig.store.writeFault[kB].fail = true;
    const CommitReport first = rig.txn.Commit();
    suite.expect(first.outcome == CommitOutcome::WriteFailed && PatchAddresses(rig) == std::vector<std::uint64_t>({kB, kC}),
        L"retry: the first attempt fails at block B and leaves B and C staged");

    // 故障排除后重试：只剩两块，写前复核照样通过，一次成功。
    rig.store.writeFault.clear();
    const std::size_t logBefore = rig.log.size();
    const CommitReport second = rig.txn.Commit();
    suite.expect(second.outcome == CommitOutcome::Committed && second.blocksTotal == 2 && second.blocksWritten == 2
        && second.bytesWritten == 3,
        L"retry: the second attempt commits exactly the two remaining blocks");
    const Log expected = {
        "AUDIT:CommitStarted", "UI", "AUDIT:UiConfirmAccepted",
        "R:1006+2", "R:100c+1",
        "W:1006+2:a0", "R:1006+2",
        "W:100c+1:a0", "R:100c+1",
        "AUDIT:CommitFinished",
    };
    suite.expect(Log(rig.log.begin() + static_cast<std::ptrdiff_t>(logBefore), rig.log.end()) == expected,
        L"retry: block A, already written and absorbed, is not touched again");
    suite.expect(rig.txn.CurrentState() == State::Committed && rig.store.writeCalls == 4,
        L"retry: the state is Committed after 4 writes in total");
    suite.expect(MemoryAt(rig, kBase, 16) == (Bytes{
        0x00, 0x11, 0x01, 0x33, 0x44, 0x55, 0x02, 0x03, 0x88, 0x99, 0xAA, 0xBB, 0x04, 0xDD, 0xEE, 0xFF}),
        L"retry: the target ends up with all three edits");
}

} // namespace

namespace MemwbTxnTests {

// 本文件的测试组：写入、同意、失败、回读、告警位、审计、重试。
void RunWriteOutcomeGroups(KswordTests::Suite& suite) {
    TestThreeBlockSuccess(suite);
    TestApprovalThisBlockOnly(suite);
    TestApprovalRestOfBatch(suite);
    TestApprovalDeny(suite);
    TestApprovalIgnoresUiSuppression(suite);
    TestApprovalProtocolEdges(suite);
    TestWriteFailures(suite);
    TestVerifyFailures(suite);
    TestWarningBits(suite);
    TestAuditRecords(suite);
    TestRetryAfterFailure(suite);
}

} // namespace MemwbTxnTests
