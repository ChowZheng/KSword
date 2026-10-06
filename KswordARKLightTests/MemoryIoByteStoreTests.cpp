// 写事务真实落点（shared/evidence/memory_workbench/MemoryIoByteStore.h）的
// 离线测试。
//
// 为什么这个模块值得一整套穷举断言：它是写事务与补丁账本唯一真正碰目标内存
// 的那一跳，错了不会抛异常——切块切错边界，读到的字节就会错位；"ok 为真但
// partial 也为真按失败处理"这条契约漏判一次，就会把一次只写了一半的结果当
// 成完整成功提交给写事务，覆盖层会把"还没真正写完"的字节标成"已确认写入"；
// 内核路由条件判漏一个，物理地址或别的通道的写入就会被错误地送进内核分步
// 事务（那条路径只认内核虚拟地址）。
//
// 断言原则与 NumericTextParseTests.cpp 一致：期望值手算写死；切块边界两侧都
// 测；needsApproval / partial-as-failure / 内核路由的四个判据分别独立测一例。

#include "TestSupport.h"

#include "MemoryIoTestSupport.h"
#include "MemoryWriteTransactionTestSupport.h"
#include "../shared/evidence/memory_workbench/MemoryIoByteStore.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace MemwbIoTests;
using ksword::memwb::AccessResult;
using ksword::memwb::Channel;
using ksword::memwb::MemoryIoByteStore;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::Scope;

constexpr std::uint64_t kAddr = 0x0000000140001000ULL;         // 一个普通用户态地址
constexpr std::uint64_t kKernelAddr = 0xFFFFF80012340000ULL;   // 一个内核虚拟地址

// MakeSessionWith：构造指定 scope/channel 的会话，pid 对进程范围给个非零值。
MemoryTargetSession MakeSessionWith(Scope scope, Channel channel) {
    MemoryTargetSession session;
    session.scope = scope;
    session.channel = channel;
    session.pid = (scope == Scope::ProcessVirtual) ? 4321U : 0U;
    return session;
}

// ------------------------------------------------------------
// 一、Read：切块、聚合、四种状态的映射。
// ------------------------------------------------------------
void TestReadZeroLengthTrivial(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Read(kAddr, 0);
    suite.expect(result.ok && result.bytesDone == 0 && result.data.empty(),
        L"io bytestore: reading zero bytes trivially succeeds");
    suite.expect(port.calls.empty(), L"io bytestore: reading zero bytes touches the port zero times");
}

void TestReadSingleChunkOk(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxReadBytes = 0; // 不限，整段一块
    const Bytes data = MakePattern(0x10, 8);
    port.script = { MakeOk(data) };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Read(kAddr, 8);
    suite.expect(result.ok && !result.partial && result.data == data && result.bytesDone == 8,
        L"io bytestore: an unlimited port reads the whole range in one call");
    suite.expect(port.calls.size() == 1 && port.calls[0].address == kAddr && port.calls[0].length == 8,
        L"io bytestore: exactly one Read call covering the full length");
}

void TestReadMultiChunkAggregatesOk(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxReadBytes = 4;
    const Bytes first = MakePattern(0x00, 4);
    const Bytes second = MakePattern(0x40, 4);
    port.script = { MakeOk(first), MakeOk(second) };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Read(kAddr, 8);
    Bytes expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    suite.expect(result.ok && !result.partial && result.data == expected && result.bytesDone == 8,
        L"io bytestore: two 4-byte chunks concatenate into the full 8-byte result");
    suite.expect(port.calls.size() == 2
        && port.calls[0].address == kAddr && port.calls[0].length == 4
        && port.calls[1].address == kAddr + 4 && port.calls[1].length == 4,
        L"io bytestore: the second chunk starts exactly where the first one ended");
}

void TestReadPartialStopsImmediately(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxReadBytes = 4;
    const Bytes firstChunk = MakePattern(0x00, 4);
    const Bytes partialPrefix = MakePattern(0x40, 2); // 第二块只读到一半
    port.script = { MakeOk(firstChunk), MakePartial(partialPrefix) };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Read(kAddr, 12); // 原本需要三块才能覆盖
    Bytes expected = firstChunk;
    expected.insert(expected.end(), partialPrefix.begin(), partialPrefix.end());
    suite.expect(result.ok && result.partial && result.data == expected && result.bytesDone == 6,
        L"io bytestore: a partial chunk's prefix is appended and the read stops there");
    suite.expect(port.calls.size() == 2,
        L"io bytestore: a partial result never triggers a third chunk read");
}

void TestReadUnreadableAndFailedRejectEverything(KswordTests::Suite& suite) {
    // Unreadable：即使前一块成功过，整体仍然 ok=false，data 不保留前一块的内容。
    {
        FakeMemoryIoPort port;
        port.limits.maxReadBytes = 4;
        port.script = { MakeOk(MakePattern(0x00, 4)), MakeUnreadable("page protection") };
        const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
        MemoryIoByteStore store(port, session);

        const AccessResult result = store.Read(kAddr, 12);
        suite.expect(!result.ok && result.data.empty() && result.failureText == "page protection",
            L"io bytestore: Unreadable rejects everything, even after an earlier successful chunk");
        suite.expect(port.calls.size() == 2, L"io bytestore: the read stops at the Unreadable chunk");
    }
    // Failed：同理。
    {
        FakeMemoryIoPort port;
        port.script = { MakeFailed("channel down") };
        const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
        MemoryIoByteStore store(port, session);

        const AccessResult result = store.Read(kAddr, 4);
        suite.expect(!result.ok && result.data.empty() && result.failureText == "channel down",
            L"io bytestore: Failed rejects everything on the very first chunk");
    }
}

void TestReadAggregatesDirtyAndWindowFlagsOnlySetting(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxReadBytes = 4;
    IoReadResult first = MakeOk(MakePattern(0x00, 4));
    first.readModifyWriteWindow = true;
    IoReadResult second = MakeUnreadable("stopped here");
    second.scratchAreaDirty = true;
    port.script = { first, second };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Read(kAddr, 8);
    suite.expect(!result.ok && result.readModifyWriteWindow && result.scratchAreaDirty,
        L"io bytestore: both flags are aggregated even though the second chunk failed");
}

// ------------------------------------------------------------
// 二、Write 的"其余范围/通道"分支：切片、needsApproval、partial-as-failure、
// 普通失败、脏标记聚合。
// ------------------------------------------------------------
void TestWriteZeroBytesTrivial(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Write(kAddr, Bytes{}, false);
    suite.expect(result.ok && result.bytesDone == 0, L"io bytestore: writing zero bytes trivially succeeds");
    suite.expect(port.writeCalls.empty(), L"io bytestore: writing zero bytes touches the port zero times");
}

void TestWriteMultiChunkSuccess(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxWriteBytes = 3;
    port.writeScript = { MakeWriteOk(3), MakeWriteOk(3), MakeWriteOk(1) };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const Bytes payload = MakePattern(0x50, 7);
    const AccessResult result = store.Write(kAddr, payload, true);
    suite.expect(result.ok && result.bytesDone == 7, L"io bytestore: three chunks of 3+3+1 sum to 7");
    suite.expect(port.writeCalls.size() == 3
        && port.writeCalls[0].address == kAddr && port.writeCalls[0].byteCount == 3 && port.writeCalls[0].approved
        && port.writeCalls[1].address == kAddr + 3 && port.writeCalls[1].byteCount == 3
        && port.writeCalls[2].address == kAddr + 6 && port.writeCalls[2].byteCount == 1,
        L"io bytestore: the approval flag and slice addresses/lengths are exactly 3+3+1");
}

void TestWriteNeedsApprovalStopsImmediately(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxWriteBytes = 3;
    port.writeScript = { MakeWriteOk(3), MakeWriteNeedsApproval("force flag required") };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Write(kAddr, MakePattern(0x00, 7), false);
    suite.expect(!result.ok && !result.needsExplicitApproval && result.partial && result.bytesDone == 3,
        L"io bytestore: late approval is a partial failure, never an authorization to retry the persisted prefix");
    suite.expect(port.writeCalls.size() == 2,
        L"io bytestore: the third chunk is never attempted after needsApproval");
}

// 首片批准尚未写任何字节；后片回滚只能撤回自己，不能抹掉此前成功片的计数。
void TestWriteApprovalAndRollbackScope(KswordTests::Suite& suite) {
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    FakeMemoryIoPort first;
    first.limits.maxWriteBytes = 3;
    first.writeScript = { MakeWriteNeedsApproval() };
    MemoryIoByteStore firstStore(first, session);
    const AccessResult approval = firstStore.Write(kAddr, MakePattern(0xA0, 7), false);
    suite.expect(approval.needsExplicitApproval && !approval.partial && approval.bytesDone == 0
        && first.writeCalls.size() == 1,
        L"io bytestore: first-slice approval permits retry only because zero bytes persisted");

    // rolledBack 只指当前端口调用；其声明的 bytesDone 已撤回，不加入持久计数。
    IoWriteResult rolledBack = MakeWriteFailed("slice rolled back");
    rolledBack.rolledBack = true;
    rolledBack.bytesDone = 2;
    FakeMemoryIoPort later;
    later.limits.maxWriteBytes = 3;
    later.writeScript = { MakeWriteOk(3), rolledBack };
    MemoryIoByteStore laterStore(later, session);
    const AccessResult partial = laterStore.Write(kAddr, MakePattern(0xA0, 7), false);
    suite.expect(!partial.ok && !partial.rolledBack && partial.partial && partial.bytesDone == 3,
        L"io bytestore: a late-slice rollback leaves the earlier three bytes persisted");

    FakeMemoryIoPort only;
    only.writeScript = { rolledBack };
    MemoryIoByteStore onlyStore(only, session);
    const AccessResult clean = onlyStore.Write(kAddr, MakePattern(0xA0, 3), false);
    suite.expect(!clean.ok && clean.rolledBack && clean.bytesDone == 0,
        L"io bytestore: a sole rolled-back slice leaves zero persisted bytes");
}

// 用真实字节存储接入写事务，防止各自单测通过却对批准/回滚的聚合语义理解不同。
void TestChunkFailureThroughWriteTransaction(KswordTests::Suite& suite) {
    for (const bool rollback : { false, true }) {
        MemwbTxnTests::Rig rig;
        FakeMemoryIoPort port;
        port.limits.maxWriteBytes = 3;
        port.script = { MakeOk(Bytes{0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}) };
        IoWriteResult second = rollback ? MakeWriteFailed("slice rolled back") : MakeWriteNeedsApproval();
        second.rolledBack = rollback;
        port.writeScript = { MakeWriteOk(3), second };
        MemoryIoByteStore store(port, rig.session);
        ksword::memwb::MemoryWriteTransaction transaction(
            rig.overlay, rig.session, rig.revisions, store, rig.sink, rig.audit);
        transaction.Stage(MemwbTxnTests::kA, MakePattern(0x01, 7));
        const auto report = transaction.Commit();
        suite.expect(report.outcome == ksword::memwb::CommitOutcome::WriteFailed
            && report.bytesWritten == 3 && report.blocksWritten == 0 && report.needsReread,
            L"io bytestore integration: chunk failure reports the persisted prefix and requires a re-read");
        suite.expect(transaction.CurrentState() == ksword::memwb::MemoryWriteTransaction::State::Failed
            && rig.overlay.PendingByteCount() == 7 && rig.sink.approvalCalls == 0
            && port.writeCalls.size() == 2,
            L"io bytestore integration: no whole-block retry or false rollback, pending edits are retained");
    }
}

void TestWritePartialIsTreatedAsFailure(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.writeScript = { MakeWritePartial(2, "short write") };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Write(kAddr, MakePattern(0x00, 5), false);
    suite.expect(!result.ok && result.partial && result.bytesDone == 2 && result.failureText == "short write",
        L"io bytestore: ok-but-partial from the port is treated as a write failure, not success");
}

void TestWritePlainFailureStops(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.writeScript = { MakeWriteFailed("access denied") };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Write(kAddr, MakePattern(0x00, 4), false);
    suite.expect(!result.ok && result.bytesDone == 0 && result.failureText == "access denied",
        L"io bytestore: a plain write failure reports zero bytes done and the port's reason");
}

void TestWriteAggregatesDirtyFlagsOnlySetting(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxWriteBytes = 3;
    IoWriteResult first = MakeWriteOk(3);
    first.scratchAreaDirty = true;
    IoWriteResult second = MakeWriteFailed("second chunk failed");
    second.readModifyWriteWindow = true;
    port.writeScript = { first, second };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);

    const AccessResult result = store.Write(kAddr, MakePattern(0x00, 6), false);
    suite.expect(!result.ok && result.scratchAreaDirty && result.readModifyWriteWindow && result.bytesDone == 3,
        L"io bytestore: dirty/window flags aggregate across the successful and the failing chunk");
}

// ------------------------------------------------------------
// 三、内核路由：三个条件（channel==StandardDriver、scope==KernelVirtual、
// IsKernelVirtualAddress）必须同时成立才走内核分步事务，否则都走通用分支。
// ------------------------------------------------------------
void TestKernelRouteRequiresAllThreeConditions(KswordTests::Suite& suite) {
    // 条件齐全但没有内核事务端口：必须直接失败，连常规端口都不碰。
    {
        FakeMemoryIoPort port;
        const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
        MemoryIoByteStore store(port, session, nullptr);
        const AccessResult result = store.Write(kKernelAddr, Bytes{ 0xAA }, false);
        suite.expect(!result.ok && result.failureText.find("kernel mutation port") != std::string::npos,
            L"io bytestore: kernel route without a kernel mutation port fails with a clear reason");
        suite.expect(port.calls.empty() && port.writeCalls.empty(),
            L"io bytestore: the missing-port failure never touches the regular port");
    }
    // scope 不是 KernelVirtual：即使地址在内核半区、通道是标准驱动，也走通用分支。
    {
        FakeMemoryIoPort port;
        port.writeScript = { MakeWriteOk(1) };
        FakeKernelMutationPort kernelPort;
        const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::StandardDriver);
        MemoryIoByteStore store(port, session, &kernelPort);
        const AccessResult result = store.Write(kKernelAddr, Bytes{ 0xAA }, false);
        suite.expect(result.ok && port.writeCalls.size() == 1 && kernelPort.prepareCalls.empty(),
            L"io bytestore: a non-KernelVirtual scope never routes through the kernel mutation port");
    }
    // channel 不是 StandardDriver：即使 scope 是内核、地址在内核半区，也走通用分支。
    {
        FakeMemoryIoPort port;
        port.writeScript = { MakeWriteOk(1) };
        FakeKernelMutationPort kernelPort;
        const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::Hvm);
        MemoryIoByteStore store(port, session, &kernelPort);
        const AccessResult result = store.Write(kKernelAddr, Bytes{ 0xAA }, false);
        suite.expect(result.ok && port.writeCalls.size() == 1 && kernelPort.prepareCalls.empty(),
            L"io bytestore: a non-StandardDriver channel never routes through the kernel mutation port");
    }
    // 地址不在内核半区：即使 scope/channel 都是内核组合，也走通用分支。
    {
        FakeMemoryIoPort port;
        port.writeScript = { MakeWriteOk(1) };
        FakeKernelMutationPort kernelPort;
        const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
        MemoryIoByteStore store(port, session, &kernelPort);
        const AccessResult result = store.Write(0x1000ULL, Bytes{ 0xAA }, false);
        suite.expect(result.ok && port.writeCalls.size() == 1 && kernelPort.prepareCalls.empty(),
            L"io bytestore: a low address never routes through the kernel mutation port");
    }
}

void TestKernelRouteEndToEndSuccess(KswordTests::Suite& suite) {
    const Bytes before{ 0x01, 0x02, 0x03 };
    const Bytes after{ 0xAA, 0xBB, 0xCC };

    FakeMemoryIoPort port;
    port.script = { MakeOk(before) }; // 预取整段写前快照的那一次 Read
    FakeKernelMutationPort kernelPort;
    kernelPort.prepareScript = { MakePrepared(77ULL, before) };
    kernelPort.dryRunScript = { MakeStepOk() };
    kernelPort.forceScript = { MakeStepOk() };
    kernelPort.readBackScript = { MakeOk(after) };

    const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
    MemoryIoByteStore store(port, session, &kernelPort);

    const AccessResult result = store.Write(kKernelAddr, after, false);
    suite.expect(result.ok && result.bytesDone == 3 && !result.needsExplicitApproval,
        L"io bytestore: a fully scripted kernel route reports success through to the top");
    suite.expect(port.calls.size() == 1 && port.calls[0].address == kKernelAddr && port.calls[0].length == 3,
        L"io bytestore: exactly one prefetch read happens before the kernel mutation");
    suite.expect(kernelPort.prepareCalls.size() == 1 && kernelPort.prepareCalls[0].address == kKernelAddr
        && kernelPort.prepareCalls[0].after == after && kernelPort.prepareCalls[0].expectedBefore == before,
        L"io bytestore: the prefetched snapshot becomes the kernel mutation's expectedBefore");
}

void TestKernelRoutePrefetchFailureNeverCallsKernelPort(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.script = { MakeUnreadable("page not present") };
    FakeKernelMutationPort kernelPort;
    const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
    MemoryIoByteStore store(port, session, &kernelPort);

    const AccessResult result = store.Write(kKernelAddr, Bytes{ 0xAA, 0xBB }, false);
    suite.expect(!result.ok && result.failureText.find("before-snapshot") != std::string::npos,
        L"io bytestore: a failed prefetch read reports a before-snapshot failure");
    suite.expect(kernelPort.prepareCalls.empty(),
        L"io bytestore: a failed prefetch never reaches the kernel mutation port");
}

void TestKernelRoutePartialPrefetchFailsSliceBeyondPrefix(KswordTests::Suite& suite) {
    // 65 字节写入，预取只读到前 64 字节（Partial）：第一片（64 字节）落在前缀
    // 内，能顺利走完四步；第二片（1 字节）超出前缀，拿不到可信快照，整体失败
    // 并把第一片回滚。
    const Bytes before = MakePattern(0x10, 64); // 只有这 64 字节是"真的"
    const Bytes after = MakePattern(0xA0, 65);
    const Bytes after0(after.begin(), after.begin() + 64);

    FakeMemoryIoPort port;
    port.script = { MakePartial(before) }; // 请求 65 字节，只给回 64 字节的前缀
    FakeKernelMutationPort kernelPort;
    kernelPort.prepareScript = { MakePrepared(88ULL, before) };
    kernelPort.dryRunScript = { MakeStepOk() };
    kernelPort.forceScript = { MakeStepOk() };
    // 第一次 ReadBack 是 slice0 的提交后核对；第二次是失败路径里 slice0 的回滚核对。
    kernelPort.readBackScript = { MakeOk(after0), MakeOk(before) };

    const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
    MemoryIoByteStore store(port, session, &kernelPort);

    const AccessResult result = store.Write(kKernelAddr, after, false);
    suite.expect(!result.ok && result.rolledBack && result.bytesDone == 0,
        L"io bytestore: a slice beyond the partial prefetch fails, rolling back the earlier committed slice");
    suite.expect(kernelPort.prepareCalls.size() == 1,
        L"io bytestore: only the in-prefix slice ever reaches Prepare");
}

void TestWriteValidationRoutesAndEmptyCalls(KswordTests::Suite& suite) {
    for (const auto channel : {Channel::UserMode, Channel::StandardDriver, Channel::Hvm, Channel::Ddma}) {
        for (const bool approved : {false, true}) {
            FakeMemoryIoPort port;
            const auto session = MakeSessionWith(Scope::ProcessVirtual, channel);
            MemoryIoByteStore store(port, session);
            int validations = 0;
            store.SetWriteValidationCallback([&](const MemoryTargetSession& target, std::uint64_t address,
                                                std::uint64_t length, std::string& reason) {
                ++validations;
                suite.expect(&target == &session && address == kAddr && length == 2,
                    L"write validation: receives actual session and fragment range");
                reason = "chain changed";
                return false;
            });
            const auto result = store.Write(kAddr, Bytes{0xAA, 0xBB}, approved);
            suite.expect(!result.ok && !result.partial && result.bytesDone == 0 && result.failureText == "chain changed",
                L"write validation: rejects both approved and ordinary writes in every process channel");
            suite.expect(validations == 1 && port.writeCalls.empty(),
                L"write validation: rejected fragment never reaches the underlying port");
            const auto empty = store.Write(kAddr, {}, approved);
            port.script = {MakeOk(Bytes{0x11})};
            const auto read = store.Read(kAddr, 1);
            suite.expect(empty.ok && read.ok && validations == 1,
                L"write validation: empty writes and reads never invoke the validator");
        }
    }
    FakeMemoryIoPort port;
    FakeKernelMutationPort kernel;
    const auto session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
    MemoryIoByteStore store(port, session, &kernel);
    store.SetWriteValidationCallback([](const MemoryTargetSession&, std::uint64_t, std::uint64_t, std::string& reason) {
        reason = "kernel range rejected";
        return false;
    });
    const auto rejected = store.Write(kKernelAddr, Bytes{0xAA}, true);
    suite.expect(!rejected.ok && rejected.failureText == "kernel range rejected"
        && port.calls.empty() && port.writeCalls.empty() && kernel.prepareCalls.empty(),
        L"write validation: kernel route rejection occurs before prefetch or mutation entry");
    store.SetWriteValidationCallback([](const MemoryTargetSession&, std::uint64_t, std::uint64_t, std::string&) {
        return true;
    });
    port.script = {MakeOk(Bytes{0x11})};
    kernel.prepareScript = {MakePrepared(44, Bytes{0x11})};
    kernel.dryRunScript = {MakeStepOk()};
    kernel.forceScript = {MakeStepOk()};
    kernel.readBackScript = {MakeOk(Bytes{0xAA})};
    suite.expect(store.Write(kKernelAddr, Bytes{0xAA}, true).ok && kernel.prepareCalls.size() == 1,
        L"write validation: an accepting callback permits the normal kernel mutation route");
}

void TestWriteValidationFragmentsAndDynamicBindings(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.limits.maxWriteBytes = 2;
    port.writeScript = {MakeWriteOk(2), MakeWriteOk(2)};
    const auto session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);
    std::vector<ReadCall> checks;
    store.SetWriteValidationCallback([&](const MemoryTargetSession&, std::uint64_t address,
                                        std::uint64_t length, std::string&) {
        suite.expect(checks.size() == port.writeCalls.size(),
            L"write validation: each successful fragment validates immediately before its port write");
        checks.push_back({address, length});
        return true;
    });
    const auto allowed = store.Write(kAddr, Bytes{1, 2, 3, 4}, false);
    suite.expect(allowed.ok && allowed.bytesDone == 4 && checks.size() == 2
        && checks[0].address == kAddr && checks[1].address == kAddr + 2
        && checks[0].length == 2 && checks[1].length == 2,
        L"write validation: allows valid fragments with exact ranges");

    store.SetWriteValidationCallback([](const MemoryTargetSession&, std::uint64_t, std::uint64_t, std::string&) {
        return false;
    });
    const auto updated = store.Write(kAddr, Bytes{9}, false);
    suite.expect(!updated.ok && !updated.failureText.empty() && port.writeCalls.size() == 2,
        L"write validation: replacing a live binding takes effect without recreating the store");
    store.SetWriteValidationCallback({});
    port.writeScript.push_back(MakeWriteOk(1));
    suite.expect(store.Write(kAddr, Bytes{9}, true).ok && port.writeCalls.size() == 3,
        L"write validation: clearing the binding restores unchanged ordinary write behavior");

    FakeMemoryIoPort partialPort;
    partialPort.limits.maxWriteBytes = 2;
    auto first = MakeWriteOk(2); first.readModifyWriteWindow = true;
    partialPort.writeScript = {first};
    MemoryIoByteStore partialStore(partialPort, session);
    partialStore.SetWriteValidationCallback([](const MemoryTargetSession&, std::uint64_t address,
                                             std::uint64_t, std::string& reason) {
        reason = "later fragment moved";
        return address == kAddr;
    });
    const auto partial = partialStore.Write(kAddr, Bytes{1, 2, 3, 4}, false);
    suite.expect(!partial.ok && partial.partial && partial.bytesDone == 2 && partial.readModifyWriteWindow
        && !partial.needsExplicitApproval && !partial.rolledBack && partial.failureText == "later fragment moved"
        && partialPort.writeCalls.size() == 1,
        L"write validation: rejection preserves previously written bytes and flags without writing rejected fragment");
}

void TestWriteValidationExceptionsAndCallbackReplacement(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    const auto session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    MemoryIoByteStore store(port, session);
    for (const bool standardException : {true, false}) {
        store.SetWriteValidationCallback([standardException](const MemoryTargetSession&, std::uint64_t,
                                                           std::uint64_t, std::string&) -> bool {
            if (standardException) throw std::runtime_error("validator error");
            throw 7;
        });
        const auto failure = store.Write(kAddr, Bytes{1}, false);
        suite.expect(!failure.ok && !failure.failureText.empty() && port.writeCalls.empty(),
            L"write validation: both standard and unknown exceptions become explicit zero-write failures");
    }
    FakeMemoryIoPort partialPort;
    partialPort.limits.maxWriteBytes = 1;
    partialPort.writeScript = {MakeWriteOk(1)};
    MemoryIoByteStore partialStore(partialPort, session);
    partialStore.SetWriteValidationCallback([](const MemoryTargetSession&, std::uint64_t address,
                                             std::uint64_t, std::string&) {
        if (address != kAddr) throw std::runtime_error("later validation failed");
        return true;
    });
    const auto partial = partialStore.Write(kAddr, Bytes{1, 2}, false);
    suite.expect(!partial.ok && partial.partial && partial.bytesDone == 1 && partialPort.writeCalls.size() == 1,
        L"write validation: an exception in a later fragment preserves earlier persistent bytes");
    store.SetWriteValidationCallback([&](const MemoryTargetSession&, std::uint64_t, std::uint64_t, std::string&) {
        store.SetWriteValidationCallback({});
        return true;
    });
    port.writeScript = {MakeWriteOk(1), MakeWriteOk(1)};
    suite.expect(store.Write(kAddr, Bytes{1}, false).ok && store.Write(kAddr, Bytes{2}, false).ok,
        L"write validation: callable remains alive when it clears its own binding");
}

void TestWriteValidationThroughTransactionModesAndApproval(KswordTests::Suite& suite) {
    for (const auto mode : {ksword::memwb::WriteMode::Immediate, ksword::memwb::WriteMode::StagedThenApply}) {
        MemwbTxnTests::Rig rig;
        FakeMemoryIoPort port;
        port.script = {MakeOk(Bytes{0x22})};
        MemoryIoByteStore store(port, rig.session);
        store.SetWriteValidationCallback([](const MemoryTargetSession&, std::uint64_t, std::uint64_t, std::string& reason) {
            reason = "chain changed"; return false;
        });
        ksword::memwb::MemoryWriteTransaction transaction(
            rig.overlay, rig.session, rig.revisions, store, rig.sink, rig.audit, mode);
        transaction.Stage(MemwbTxnTests::kA, Bytes{0xAA});
        auto automatic = transaction.OnEditCompleted();
        const auto report = automatic ? *automatic : transaction.Commit();
        suite.expect(report.outcome == ksword::memwb::CommitOutcome::WriteFailed
            && report.bytesWritten == 0 && port.writeCalls.empty(),
            L"write validation: both immediate and staged transactions honor the common store guard");
    }
    MemwbTxnTests::Rig rig;
    FakeMemoryIoPort port;
    port.script = {MakeOk(Bytes{0x22}), MakeOk(Bytes{0x22})};
    port.writeScript = {MakeWriteNeedsApproval()};
    MemoryIoByteStore store(port, rig.session);
    bool valid = true;
    int checks = 0;
    store.SetWriteValidationCallback([&](const MemoryTargetSession&, std::uint64_t, std::uint64_t, std::string& reason) {
        ++checks; reason = "chain changed during confirmation"; return valid;
    });
    rig.sink.approvalScript = {ksword::memwb::ApprovalAnswer::ThisBlockOnly};
    rig.sink.onApproval = [&]() { valid = false; };
    ksword::memwb::MemoryWriteTransaction transaction(
        rig.overlay, rig.session, rig.revisions, store, rig.sink, rig.audit);
    transaction.Stage(MemwbTxnTests::kA, Bytes{0xAA});
    const auto report = transaction.Commit();
    suite.expect(report.outcome == ksword::memwb::CommitOutcome::WriteFailed && report.bytesWritten == 0
        && rig.sink.approvalCalls == 1 && checks == 2 && port.writeCalls.size() == 1,
        L"write validation: confirmation retry revalidates and cannot bypass a newly rejected binding");
}

} // namespace

int RunMemwbIoByteStoreTests() {
    KswordTests::Suite suite(L"MEMWB io bytestore");
    TestReadZeroLengthTrivial(suite);
    TestReadSingleChunkOk(suite);
    TestReadMultiChunkAggregatesOk(suite);
    TestReadPartialStopsImmediately(suite);
    TestReadUnreadableAndFailedRejectEverything(suite);
    TestReadAggregatesDirtyAndWindowFlagsOnlySetting(suite);
    TestWriteZeroBytesTrivial(suite);
    TestWriteMultiChunkSuccess(suite);
    TestWriteNeedsApprovalStopsImmediately(suite);
    TestWriteApprovalAndRollbackScope(suite);
    TestChunkFailureThroughWriteTransaction(suite);
    TestWritePartialIsTreatedAsFailure(suite);
    TestWritePlainFailureStops(suite);
    TestWriteAggregatesDirtyFlagsOnlySetting(suite);
    TestKernelRouteRequiresAllThreeConditions(suite);
    TestKernelRouteEndToEndSuccess(suite);
    TestKernelRoutePrefetchFailureNeverCallsKernelPort(suite);
    TestKernelRoutePartialPrefetchFailsSliceBeyondPrefix(suite);
    TestWriteValidationRoutesAndEmptyCalls(suite);
    TestWriteValidationFragmentsAndDynamicBindings(suite);
    TestWriteValidationExceptionsAndCallbackReplacement(suite);
    TestWriteValidationThroughTransactionModesAndApproval(suite);
    suite.report();
    return suite.failures();
}
