// 内核分步字节事务（shared/evidence/memory_workbench/MemoryKernelMutation.h）
// 的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它复刻的是旧代码里"失败了要不要把已经
// 落地的字节改回去"这条最容易出错的路径——旧实现失败即整批回滚；本类按用户
// 决策只保证"这一次 WriteKernelBytes 调用内"的回滚，但这个范围本身必须精确：
// 漏回滚一片，目标就留下一段"写了一半"的内核字节；该回滚的片因为判断错误
// 没被收进回滚列表，用户以为已经复原，其实目标上还是新值。
//
// 断言原则与 NumericTextParseTests.cpp 一致：
//   * 期望值全部手算写死（地址、字节、事务号、调用顺序都是字面量）；
//   * 边界两侧都测（0/1/64/65/129 字节的切片数量，64 恰好一片、65 恰好跨片）；
//   * 五类失败点各显式测一例，且都断言"没通过校验的片不进回滚列表"；
//   * Rollback() 自身的返回值必须被忽略——回滚是否成功只认紧随其后的那次
//     独立 ReadBack，这一条单独有一例测它。
//
// 2026-10 修复（K-1，主会话审核发现）：回滚阶段改成复刻旧编排
// （MemoryDock.DriverMemoryRw.cpp 第 1653-1715 行）的顺序——逆序的每一片先
// ReadBack 一次，已经等于写前字节就不调用 Rollback，否则才调用 Rollback 再
// ReadBack 核对；下面所有涉及回滚的测试都按"先读、按需才滚"重新手算了
// Rollback 调用次数与 ReadBack 脚本。同时 rolledBack 的语义收紧为"至少有一
// 片真的到过 ForceCommit 阶段（不论成功与否）且全部核对通过"——从未到过
// ForceCommit 的失败（例如 Prepare 之后 DryRun 就失败）即使凑巧调用了
// Rollback 并核对通过，rolledBack 仍然是 false，因为目标从未被真正改动过。
// TestDryRunFailureRollsBack 的三个子用例、TestForceCommitFailureRollsBack
// 新增的"零次 Rollback 仍 rolledBack=true"子用例，都是专门钉住这条规则的。

#include "TestSupport.h"

#include "MemoryIoTestSupport.h"
#include "../shared/evidence/memory_workbench/MemoryKernelMutation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace MemwbIoTests;
using ksword::memwb::KernelMutationResult;
using ksword::memwb::kKernelMutationSliceBytes;
using ksword::memwb::WriteKernelBytes;

// 测试用起始地址：取一个典型的内核虚拟地址，这个模块本身并不检查地址是否真
// 落在内核半区（那是调用方 MemoryIoByteStore 的职责），用它纯粹是贴合语境。
constexpr std::uint64_t kAddr = 0xFFFFF80012340000ULL;

// ReadBeforeScript：WriteKernelBytes 的 readBefore 回调的脚本化实现。
//
// 调用方法：构造时给一份"完整写前字节"allBefore 与它对应的起始地址
// baseAddress；之后每次被调用都从 allBefore 里按偏移切一段返回 Ok。
// failOnCall（1-based）命中时改为返回 Unreadable，不给数据——用于测试
// "拿不到可信写前快照"这一条路径。
class ReadBeforeScript {
public:
    ReadBeforeScript(Bytes allBefore, std::uint64_t baseAddress)
        : allBefore_(std::move(allBefore)), baseAddress_(baseAddress) {}

    int failOnCall = 0;        // 1-based；0 表示从不注入失败
    int callCount = 0;         // 已被调用的次数
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls; // (address,length) 记录

    IoReadResult operator()(std::uint64_t address, std::uint64_t length) {
        ++callCount;
        calls.emplace_back(address, length);
        if (failOnCall != 0 && callCount == failOnCall) {
            return MakeUnreadable("read-before injected failure");
        }
        const std::uint64_t offset = address - baseAddress_;
        const Bytes slice(
            allBefore_.begin() + static_cast<std::ptrdiff_t>(offset),
            allBefore_.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return MakeOk(slice);
    }

private:
    Bytes allBefore_;
    std::uint64_t baseAddress_;
};

// SliceOf：从 full 里切出 [offset, offset+length) 的子序列，手算校验用。
Bytes SliceOf(const Bytes& full, std::size_t offset, std::size_t length) {
    return Bytes(full.begin() + static_cast<std::ptrdiff_t>(offset),
        full.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

// ------------------------------------------------------------
// 一、边界：0/1/64/65/129 字节的切片数量与地址/长度序列。
// ------------------------------------------------------------
void TestSliceBoundaries(KswordTests::Suite& suite) {
    // 0 字节：什么都不做，安全地报告成功，readBefore 一次都不会被调用。
    {
        FakeKernelMutationPort port;
        ReadBeforeScript readBefore(Bytes{}, kAddr);
        const KernelMutationResult result = WriteKernelBytes(port, kAddr, Bytes{}, readBefore);
        suite.expect(result.ok && result.bytesDone == 0 && !result.rolledBack,
            L"kernel mutation: zero bytes trivially succeeds");
        suite.expect(readBefore.callCount == 0 && port.prepareCalls.empty(),
            L"kernel mutation: zero bytes touches nothing");
    }

    // 1 字节：恰好一片，完整走完四步。
    {
        const Bytes before{ 0x55 };
        const Bytes after{ 0xAA };
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(1ULL, before) };
        port.dryRunScript = { MakeStepOk() };
        port.forceScript = { MakeStepOk() };
        port.readBackScript = { MakeOk(after) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(result.ok && result.bytesDone == 1 && !result.rolledBack,
            L"kernel mutation: one byte commits in a single slice");
        suite.expect(port.prepareCalls.size() == 1 && port.prepareCalls[0].address == kAddr
            && port.prepareCalls[0].after == after && port.prepareCalls[0].expectedBefore == before,
            L"kernel mutation: the single slice carries the exact address/after/before");
        suite.expect(port.dryRunCalls.size() == 1 && port.dryRunCalls[0].transactionId == 1ULL,
            L"kernel mutation: dry-run commits against the transaction Prepare returned");
        suite.expect(port.forceCalls.size() == 1 && port.readBackCalls.size() == 1
            && port.readBackCalls[0].address == kAddr && port.readBackCalls[0].length == 1,
            L"kernel mutation: force commit then one verify read-back");
        suite.expect(port.rollbackCalls.empty(), L"kernel mutation: a fully successful slice never rolls back");
    }

    // 64 字节：正好一片，不会多切出一个 0 字节的片。
    {
        const Bytes before = MakePattern(0x10, 64);
        const Bytes after = MakePattern(0x90, 64);
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(2ULL, before) };
        port.dryRunScript = { MakeStepOk() };
        port.forceScript = { MakeStepOk() };
        port.readBackScript = { MakeOk(after) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(result.ok && result.bytesDone == 64, L"kernel mutation: 64 bytes is exactly one slice");
        suite.expect(port.prepareCalls.size() == 1, L"kernel mutation: 64 bytes does not split into two slices");
    }

    // 65 字节：64 + 1 两片，地址与长度手算写死。
    {
        const Bytes before = MakePattern(0x20, 65);
        const Bytes after = MakePattern(0xA0, 65);
        FakeKernelMutationPort port;
        port.prepareScript = {
            MakePrepared(10ULL, SliceOf(before, 0, 64)),
            MakePrepared(11ULL, SliceOf(before, 64, 1)),
        };
        port.dryRunScript = { MakeStepOk(), MakeStepOk() };
        port.forceScript = { MakeStepOk(), MakeStepOk() };
        port.readBackScript = { MakeOk(SliceOf(after, 0, 64)), MakeOk(SliceOf(after, 64, 1)) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(result.ok && result.bytesDone == 65, L"kernel mutation: 65 bytes commits in two slices");
        suite.expect(port.prepareCalls.size() == 2
            && port.prepareCalls[0].address == kAddr && port.prepareCalls[0].after.size() == 64
            && port.prepareCalls[1].address == kAddr + 64 && port.prepareCalls[1].after.size() == 1,
            L"kernel mutation: 65 bytes splits into a 64-byte slice then a 1-byte slice");
    }

    // 129 字节：64 + 64 + 1 三片。
    {
        const Bytes before = MakePattern(0x30, 129);
        const Bytes after = MakePattern(0xC0, 129);
        FakeKernelMutationPort port;
        port.prepareScript = {
            MakePrepared(20ULL, SliceOf(before, 0, 64)),
            MakePrepared(21ULL, SliceOf(before, 64, 64)),
            MakePrepared(22ULL, SliceOf(before, 128, 1)),
        };
        port.dryRunScript = { MakeStepOk(), MakeStepOk(), MakeStepOk() };
        port.forceScript = { MakeStepOk(), MakeStepOk(), MakeStepOk() };
        port.readBackScript = {
            MakeOk(SliceOf(after, 0, 64)), MakeOk(SliceOf(after, 64, 64)), MakeOk(SliceOf(after, 128, 1)),
        };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(result.ok && result.bytesDone == 129, L"kernel mutation: 129 bytes commits in three slices");
        suite.expect(port.prepareCalls.size() == 3
            && port.prepareCalls[0].address == kAddr
            && port.prepareCalls[1].address == kAddr + 64
            && port.prepareCalls[2].address == kAddr + 128
            && port.prepareCalls[2].after.size() == 1,
            L"kernel mutation: 129 bytes splits into 64+64+1 at the exact addresses");
    }
}

// ------------------------------------------------------------
// 二、地址溢出：整体拒绝，不发起任何调用。
// ------------------------------------------------------------
void TestAddressOverflowRejected(KswordTests::Suite& suite) {
    FakeKernelMutationPort port;
    ReadBeforeScript readBefore(Bytes{ 1, 2, 3 }, 0xFFFFFFFFFFFFFFFEULL);
    const Bytes after{ 0xAA, 0xBB, 0xCC };
    const KernelMutationResult result =
        WriteKernelBytes(port, 0xFFFFFFFFFFFFFFFEULL, after, readBefore);
    suite.expect(!result.ok && result.bytesDone == 0 && !result.rolledBack,
        L"kernel mutation: an overflowing range is rejected outright");
    suite.expect(readBefore.callCount == 0 && port.prepareCalls.empty(),
        L"kernel mutation: an overflowing range never touches the port or the read-before callback");
    suite.expect(result.failure.find("exceeds the 64-bit address space") != std::string::npos,
        L"kernel mutation: the overflow failure text says so");
}

// ------------------------------------------------------------
// 三、五类失败点，各一例；前三类（读前失败/Prepare 失败/校验不符）都不产生
// 任何回滚尝试——它们从未拿到一个受信任的事务号。
// ------------------------------------------------------------
void TestReadBeforeFailureNeverPrepares(KswordTests::Suite& suite) {
    FakeKernelMutationPort port;
    ReadBeforeScript readBefore(Bytes{ 0x11, 0x22 }, kAddr);
    readBefore.failOnCall = 1;
    const KernelMutationResult result = WriteKernelBytes(port, kAddr, Bytes{ 0x99, 0x88 }, readBefore);

    suite.expect(!result.ok && result.bytesDone == 0 && !result.rolledBack
        && result.rollbackVerifiedBytes == 0 && result.rollbackFailedCount == 0,
        L"kernel mutation: a failed before-read leaves nothing to roll back");
    suite.expect(port.prepareCalls.empty() && port.rollbackCalls.empty(),
        L"kernel mutation: a failed before-read never calls Prepare or Rollback");
}

void TestPrepareRejectedByPort(KswordTests::Suite& suite) {
    const Bytes before{ 0x11, 0x22 };
    FakeKernelMutationPort port;
    port.prepareScript = { MakePrepareFailed("status not PREPARED") };
    ReadBeforeScript readBefore(before, kAddr);

    const KernelMutationResult result = WriteKernelBytes(port, kAddr, Bytes{ 0x99, 0x88 }, readBefore);
    suite.expect(!result.ok && result.bytesDone == 0 && !result.rolledBack,
        L"kernel mutation: a port-rejected prepare fails with nothing committed");
    suite.expect(port.prepareCalls.size() == 1 && port.dryRunCalls.empty() && port.rollbackCalls.empty(),
        L"kernel mutation: a rejected prepare never reaches dry-run or rollback");
}

void TestPrepareValidationMismatches(KswordTests::Suite& suite) {
    const Bytes before{ 0x11, 0x22, 0x33, 0x44 };
    const Bytes after{ 0x99, 0x88, 0x77, 0x66 };

    // 事务号为 0：端口声称 ok，但没有给出可信的事务号。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(0ULL, before) };
        ReadBeforeScript readBefore(before, kAddr);
        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && result.bytesDone == 0 && !result.rolledBack && port.dryRunCalls.empty(),
            L"kernel mutation: transaction id 0 is rejected before dry-run");
    }

    // 长度不符：回读到的 beforeBytes 比这一片短。
    {
        FakeKernelMutationPort port;
        MutationPrepareResult shortBefore;
        shortBefore.ok = true;
        shortBefore.transactionId = 5ULL;
        shortBefore.beforeBytes = Bytes{ before[0], before[1] }; // 只给 2 字节，这一片要 4 字节
        port.prepareScript = { shortBefore };
        ReadBeforeScript readBefore(before, kAddr);
        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && result.bytesDone == 0 && !result.rolledBack && port.dryRunCalls.empty(),
            L"kernel mutation: insufficient before-byte length is rejected before dry-run");
    }

    // before 不一致：长度够，但内容与 expectedBefore 不同。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(6ULL, Bytes{ 0xEE, 0xEE, 0xEE, 0xEE }) };
        ReadBeforeScript readBefore(before, kAddr);
        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && result.bytesDone == 0 && !result.rolledBack && port.dryRunCalls.empty(),
            L"kernel mutation: a before-byte mismatch is rejected before dry-run");
    }
}

// ------------------------------------------------------------
// 四、DryRun / ForceCommit / ReadBack 三类失败点：Prepare 已经成功，所以这一
// 片已经进了回滚列表，必须尝试回滚；分别测"回滚核对通过"与"回滚核对不通过"。
// 另外单独验证 Rollback() 自身的返回值被忽略——只认紧跟着的那次 ReadBack。
// ------------------------------------------------------------
void TestDryRunFailureRollsBack(KswordTests::Suite& suite) {
    const Bytes before{ 0xAA, 0xBB };
    const Bytes after{ 0x11, 0x22 };

    // DryRun 失败：这一片从未到过 ForceCommit，目标理应还是写前字节。第一次
    // ReadBack 就已经等于 expectedBefore，因此 Rollback 必须零次调用——这是
    // K-1 要求的"先读、按需才滚"，也钉死 rolledBack 的新语义：即使回滚核对
    // 全部通过，没有任何片到过 ForceCommit，rolledBack 仍然是 false。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(30ULL, before) };
        port.dryRunScript = { MakeStepFailed("dry run rejected") };
        port.readBackScript = { MakeOk(before) }; // 唯一一次 ReadBack：已经等于写前字节
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && !result.rolledBack && result.bytesDone == 0
            && result.rollbackVerifiedBytes == 2 && result.rollbackFailedCount == 0,
            L"kernel mutation: a dry-run failure that never reached force-commit reports rolledBack=false");
        suite.expect(port.rollbackCalls.empty() && port.readBackCalls.size() == 1,
            L"kernel mutation: an already-restored slice calls Rollback zero times");
    }

    // DryRun 失败，但第一次 ReadBack 回读到的不是 before（模拟"看起来被
    // 改动过"）：必须调用一次 Rollback，第二次 ReadBack 核对确实复原。
    // 即便如此，这一片依然没有到过 ForceCommit，rolledBack 仍然是 false——
    // 这正是"凑巧回滚成功也不算 rolledBack"这条新规则最容易被看错的地方。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(31ULL, before) };
        port.dryRunScript = { MakeStepFailed("dry run rejected") };
        port.readBackScript = { MakeOk(Bytes{ 0xCC, 0xDD }), MakeOk(before) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && !result.rolledBack && result.bytesDone == 0
            && result.rollbackVerifiedBytes == 2 && result.rollbackFailedCount == 0,
            L"kernel mutation: rollback verifying restored still reports rolledBack=false "
            L"when the slice never reached force-commit");
        suite.expect(port.rollbackCalls.size() == 1 && port.rollbackCalls[0].transactionId == 31ULL
            && port.readBackCalls.size() == 2,
            L"kernel mutation: a mismatching pre-check calls rollback exactly once, then re-reads");
    }

    // Rollback() 自身返回失败，但第二次 ReadBack 核对确实已经复原：回滚结果
    // 只认 ReadBack，不认 Rollback() 的返回值；rolledBack 仍然是 false
    // （同上，这一片也没到过 ForceCommit）。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(32ULL, before) };
        port.dryRunScript = { MakeStepFailed("dry run rejected") };
        port.rollbackScript = { MakeStepFailed("rollback reported failure") };
        port.readBackScript = { MakeOk(Bytes{ 0xCC, 0xDD }), MakeOk(before) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.rolledBack && result.rollbackFailedCount == 0
            && result.rollbackVerifiedBytes == 2,
            L"kernel mutation: Rollback()'s own return value is ignored; only the verifying read-back counts");
        suite.expect(port.rollbackCalls.size() == 1,
            L"kernel mutation: exactly one rollback call happens after the mismatching pre-check");
    }
}

void TestForceCommitFailureRollsBack(KswordTests::Suite& suite) {
    const Bytes before{ 0x01, 0x02, 0x03 };
    const Bytes after{ 0xF1, 0xF2, 0xF3 };

    // 子用例一：ForceCommit 失败且什么都没落地（第一次 ReadBack 已经等于
    // before）：这一片到过 ForceCommit 阶段（调用过，不论成不成功），所以
    // rolledBack=true；但既然目标本来就没变，Rollback 必须零次调用——这正
    // 是"rolledBack 为真不代表一定发生过 Rollback 调用"的例子。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(40ULL, before) };
        port.dryRunScript = { MakeStepOk() };
        port.forceScript = { MakeStepFailed("force commit rejected") };
        port.readBackScript = { MakeOk(before) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && result.rolledBack && result.bytesDone == 0
            && result.rollbackVerifiedBytes == 3 && result.rollbackFailedCount == 0,
            L"kernel mutation: a force-commit failure that touched nothing still reports rolledBack=true");
        suite.expect(port.forceCalls.size() == 1 && port.rollbackCalls.empty()
            && port.readBackCalls.size() == 1,
            L"kernel mutation: reaching force-commit with an already-matching target calls rollback zero times");
    }

    // 子用例二："失败的提交也可能已部分落地"：第一次 ReadBack 显示目标确实
    // 已经被改成了 after（ForceCommit 失败前已经写了一部分），必须真的调用
    // 一次 Rollback，第二次 ReadBack 核对复原——这是"确实被改动的片 Rollback
    // 恰好一次"的例子，且因为到过 ForceCommit，rolledBack=true。
    {
        FakeKernelMutationPort port;
        port.prepareScript = { MakePrepared(41ULL, before) };
        port.dryRunScript = { MakeStepOk() };
        port.forceScript = { MakeStepFailed("force commit rejected, partially landed") };
        port.readBackScript = { MakeOk(after), MakeOk(before) };
        ReadBeforeScript readBefore(before, kAddr);

        const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
        suite.expect(!result.ok && result.rolledBack && result.bytesDone == 0
            && result.rollbackVerifiedBytes == 3 && result.rollbackFailedCount == 0,
            L"kernel mutation: a force-commit failure that partially landed rolls back and verifies restored");
        suite.expect(port.forceCalls.size() == 1 && port.rollbackCalls.size() == 1
            && port.rollbackCalls[0].transactionId == 41ULL && port.readBackCalls.size() == 2,
            L"kernel mutation: exactly one force attempt then exactly one rollback for a truly-changed slice");
    }
}

void TestReadBackMismatchRollsBack(KswordTests::Suite& suite) {
    const Bytes before{ 0x01, 0x02, 0x03 };
    const Bytes after{ 0xF1, 0xF2, 0xF3 };
    FakeKernelMutationPort port;
    port.prepareScript = { MakePrepared(50ULL, before) };
    port.dryRunScript = { MakeStepOk() };
    port.forceScript = { MakeStepOk() };
    // 三次 ReadBack：[0] 提交后的主流程核对，回读到的不是 after（触发失败）；
    // [1] 回滚阶段的第一次回读（pre-check），显示目标确实已经是 after（即
    // ForceCommit 其实真的落地了，只是上一步核对读到了别的东西），与
    // expectedBefore 不一致，必须调用 Rollback；[2] Rollback 之后的第二次
    // 回读，核对确实已经复原成 before。
    port.readBackScript = { MakeOk(Bytes{ 0x00, 0x00, 0x00 }), MakeOk(after), MakeOk(before) };
    ReadBeforeScript readBefore(before, kAddr);

    const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);
    suite.expect(!result.ok && result.rolledBack && result.bytesDone == 0
        && result.rollbackVerifiedBytes == 3 && result.rollbackFailedCount == 0,
        L"kernel mutation: a verify read-back mismatch rolls back and verifies restored");
    suite.expect(port.readBackCalls.size() == 3
        && port.readBackCalls[0].address == kAddr && port.readBackCalls[1].address == kAddr
        && port.readBackCalls[2].address == kAddr,
        L"kernel mutation: one verify read-back, one pre-check, one post-rollback read-back, all at the slice address");
    suite.expect(port.rollbackCalls.size() == 1 && port.rollbackCalls[0].transactionId == 50ULL,
        L"kernel mutation: the mismatching pre-check triggers exactly one rollback call");
}

// ------------------------------------------------------------
// 五、多片同一次调用：前两片完整落地并核对通过，第三片在 ForceCommit 失败——
// 全部三片（含已经成功的两片）必须按逆序一起回滚，这正是"单次 Write 调用内"
// 回滚范围的精确边界。三片都到过 ForceCommit 阶段（第三片调用过，只是失败），
// 所以回滚阶段每一片的"先回读"都会看到与 expectedBefore 不一致的当前内容
// （分别是已经落地的 after0/after1，以及"失败的提交部分落地"的 after2），
// 因此三片都会真的调用一次 Rollback——这与 TestDryRunFailureRollsBack 里
// "已经等于写前字节、零次 Rollback"的例子正好互补。
// ------------------------------------------------------------
void TestMultiSliceRollbackCoversTheWholeCall(KswordTests::Suite& suite) {
    const Bytes before = MakePattern(0x30, 129);
    const Bytes after = MakePattern(0xC0, 129);
    const Bytes before0 = SliceOf(before, 0, 64);
    const Bytes before1 = SliceOf(before, 64, 64);
    const Bytes before2 = SliceOf(before, 128, 1);
    const Bytes after0 = SliceOf(after, 0, 64);
    const Bytes after1 = SliceOf(after, 64, 64);
    const Bytes after2 = SliceOf(after, 128, 1);

    FakeKernelMutationPort port;
    port.prepareScript = {
        MakePrepared(101ULL, before0),
        MakePrepared(102ULL, before1),
        MakePrepared(103ULL, before2),
    };
    port.dryRunScript = { MakeStepOk(), MakeStepOk(), MakeStepOk() };
    // 第三片（下标 2）在 ForceCommit 失败；前两片成功。
    port.forceScript = { MakeStepOk(), MakeStepOk(), MakeStepFailed("force commit rejected") };
    // ReadBack 消费顺序：
    //   [0][1]  正向流程里 slice0/slice1 的提交后核对（都等于 after，成功）；
    //           slice2 的 Force 失败，正向流程没有核对读。
    //   回滚阶段逆序，每片两次读（先 pre-check 看是否已经等于 before，不等
    //   才 Rollback 再 post-check 核对）：
    //   [2] slice2 pre-check=after2（已部分落地，不等于 before2）
    //   [3] slice2 post-check=before2（Rollback 之后核对复原）
    //   [4] slice1 pre-check=after1（已落地，不等于 before1）
    //   [5] slice1 post-check=before1（复原）
    //   [6] slice0 pre-check=after0（已落地，不等于 before0）
    //   [7] slice0 post-check=before0（复原）
    port.readBackScript = {
        MakeOk(after0), MakeOk(after1),
        MakeOk(after2), MakeOk(before2),
        MakeOk(after1), MakeOk(before1),
        MakeOk(after0), MakeOk(before0),
    };
    ReadBeforeScript readBefore(before, kAddr);

    const KernelMutationResult result = WriteKernelBytes(port, kAddr, after, readBefore);

    suite.expect(!result.ok && result.rolledBack && result.bytesDone == 0
        && result.rollbackVerifiedBytes == 129 && result.rollbackFailedCount == 0,
        L"kernel mutation: all three slices (two already-verified, one failing) roll back together");
    suite.expect(port.rollbackCalls.size() == 3
        && port.rollbackCalls[0].transactionId == 103ULL
        && port.rollbackCalls[1].transactionId == 102ULL
        && port.rollbackCalls[2].transactionId == 101ULL,
        L"kernel mutation: rollback runs in strict reverse order of commitment");
    suite.expect(port.readBackCalls.size() == 8,
        L"kernel mutation: two forward verifies plus a pre-check and a post-check per rolled-back slice");
    // 每一片都是"pre-check 不等 -> 紧接着 Rollback -> 紧接着 post-check"，
    // 三片严格背靠背，调用序号必须是连续的三元组。
    suite.expect(port.readBackCalls[2].order + 1 == port.rollbackCalls[0].order
        && port.rollbackCalls[0].order + 1 == port.readBackCalls[3].order
        && port.readBackCalls[4].order + 1 == port.rollbackCalls[1].order
        && port.rollbackCalls[1].order + 1 == port.readBackCalls[5].order
        && port.readBackCalls[6].order + 1 == port.rollbackCalls[2].order
        && port.rollbackCalls[2].order + 1 == port.readBackCalls[7].order,
        L"kernel mutation: each rollback is immediately preceded by its mismatching pre-check "
        L"and immediately followed by its own verifying read-back");

    // 对照：第一片（slice0）的回滚核对失败时，只有那一片计入
    // bytesDone/rollbackFailedCount；slice2/slice1 仍然正常复原。
    FakeKernelMutationPort portPartial;
    portPartial.prepareScript = port.prepareScript;
    portPartial.dryRunScript = port.dryRunScript;
    portPartial.forceScript = port.forceScript;
    const Bytes stillAfter0 = after0; // slice0 核对失败：回读到的仍是 after0，没有真的复原
    portPartial.readBackScript = {
        MakeOk(after0), MakeOk(after1),
        MakeOk(after2), MakeOk(before2),
        MakeOk(after1), MakeOk(before1),
        MakeOk(after0), MakeOk(stillAfter0),
    };
    ReadBeforeScript readBeforePartial(before, kAddr);
    const KernelMutationResult partialResult =
        WriteKernelBytes(portPartial, kAddr, after, readBeforePartial);
    suite.expect(!partialResult.ok && !partialResult.rolledBack
        && partialResult.bytesDone == 64 && partialResult.rollbackFailedCount == 1
        && partialResult.rollbackVerifiedBytes == 65,
        L"kernel mutation: only the slice that fails to verify restored counts toward bytesDone");
}

} // namespace

int RunMemwbKernelMutationTests() {
    KswordTests::Suite suite(L"MEMWB kernel mutation");
    TestSliceBoundaries(suite);
    TestAddressOverflowRejected(suite);
    TestReadBeforeFailureNeverPrepares(suite);
    TestPrepareRejectedByPort(suite);
    TestPrepareValidationMismatches(suite);
    TestDryRunFailureRollsBack(suite);
    TestForceCommitFailureRollsBack(suite);
    TestReadBackMismatchRollsBack(suite);
    TestMultiSliceRollbackCoversTheWholeCall(suite);
    suite.report();
    return suite.failures();
}
