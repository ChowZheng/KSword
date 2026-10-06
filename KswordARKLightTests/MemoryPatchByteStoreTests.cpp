// int3 补丁账本的真实读写落点（shared/evidence/memory_workbench/
// MemoryPatchByteStore.h）的离线测试。
//
// 为什么这个模块值得一整套穷举断言：它故意比 MemoryIoByteStore 窄得多——
// 内核/物理范围与磁盘传输通道一律拒绝，且约定"被拒绝时端口调用次数恒为 0"。
// 这条约定本身就是它存在的理由之一：Int3PatchLedger 的 ReadFailed 与"从未
// 尝试"必须有区别（账本按它们走不同的状态机分支），本类如果在拒绝路径上
// 偷偷调了一次端口，这条区别就被破坏了。写入侧另一条容易出错的约定是
// approved 永远传 false——一旦有人为了"图方便"改成传 true，int3 补丁就从
// "零摩擦但不越权"变成了"自动越权"。
//
// 断言原则与 Int3PatchLedgerTests.cpp 一致：期望值手算写死；四种范围/通道
// 组合（允许三种+拒绝两种中的代表）都显式测；端口调用次数用于证明"零读写"。

#include "TestSupport.h"

#include "MemoryIoTestSupport.h"
#include "../shared/evidence/memory_workbench/MemoryPatchByteStore.h"

#include <cstdint>
#include <vector>

namespace {

using namespace MemwbIoTests;
using ksword::memwb::Channel;
using ksword::memwb::MemoryPatchByteStore;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::Scope;

constexpr std::uint64_t kAddr = 0x0000000140001000ULL;

MemoryTargetSession MakeSessionWith(Scope scope, Channel channel) {
    MemoryTargetSession session;
    session.scope = scope;
    session.channel = channel;
    session.pid = (scope == Scope::ProcessVirtual) ? 4321U : 0U;
    return session;
}

// ------------------------------------------------------------
// 一、范围/通道检查：三条允许的通道都放行；内核范围、物理范围、磁盘传输
// 通道一律拒绝，且端口调用次数恒为 0。
// ------------------------------------------------------------
void TestSupportedRoutesReachThePort(KswordTests::Suite& suite) {
    for (const Channel channel : { Channel::UserMode, Channel::StandardDriver, Channel::Hvm }) {
        FakeMemoryIoPort port;
        port.script = { MakeOk(Bytes{ 0x42 }) };
        const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, channel);
        MemoryPatchByteStore store(port, session);

        std::uint8_t value = 0x00;
        const bool ok = store.ReadByte(kAddr, value);
        suite.expect(ok && value == 0x42 && port.calls.size() == 1,
            L"patch store: UserMode/StandardDriver/Hvm with ProcessVirtual all reach the port");
    }
}

void TestKernelScopeRejectedWithoutTouchingThePort(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    const MemoryTargetSession session = MakeSessionWith(Scope::KernelVirtual, Channel::StandardDriver);
    MemoryPatchByteStore store(port, session);

    std::uint8_t value = 0x99; // 毒值：失败时绝不能被改写
    const bool readOk = store.ReadByte(kAddr, value);
    suite.expect(!readOk && value == 0x99, L"patch store: KernelVirtual scope is rejected, valueOut untouched");

    const bool writeOk = store.WriteByte(kAddr, 0xCC);
    suite.expect(!writeOk, L"patch store: KernelVirtual scope rejects the write too");
    suite.expect(port.calls.empty() && port.writeCalls.empty(),
        L"patch store: a rejected kernel-scope request never touches the port");
}

void TestPhysicalScopeRejectedWithoutTouchingThePort(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    const MemoryTargetSession session = MakeSessionWith(Scope::Physical, Channel::StandardDriver);
    MemoryPatchByteStore store(port, session);

    std::uint8_t value = 0x77;
    const bool readOk = store.ReadByte(kAddr, value);
    suite.expect(!readOk && value == 0x77, L"patch store: Physical scope is rejected, valueOut untouched");
    suite.expect(port.calls.empty(), L"patch store: a rejected physical-scope read never touches the port");
}

void TestDdmaChannelRejectedEvenWithProcessScope(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::Ddma);
    MemoryPatchByteStore store(port, session);

    std::uint8_t value = 0x55;
    const bool readOk = store.ReadByte(kAddr, value);
    suite.expect(!readOk && value == 0x55,
        L"patch store: Ddma channel is rejected even with ProcessVirtual scope");
    const bool writeOk = store.WriteByte(kAddr, 0xCC);
    suite.expect(!writeOk && port.calls.empty() && port.writeCalls.empty(),
        L"patch store: a rejected Ddma-channel request never touches the port");
}

// ------------------------------------------------------------
// 二、ReadByte 的四种端口结果映射。
// ------------------------------------------------------------
void TestReadByteMapsPortResults(KswordTests::Suite& suite) {
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);

    // Ok 且恰好 1 字节：成功，valueOut 被设置。
    {
        FakeMemoryIoPort port;
        port.script = { MakeOk(Bytes{ 0x7A }) };
        MemoryPatchByteStore store(port, session);
        std::uint8_t value = 0;
        suite.expect(store.ReadByte(kAddr, value) && value == 0x7A,
            L"patch store: an Ok single byte read succeeds");
    }
    // Ok 但端口违反契约给了 2 字节：防御性地按失败处理。
    {
        FakeMemoryIoPort port;
        port.script = { MakeOk(Bytes{ 0x11, 0x22 }) };
        MemoryPatchByteStore store(port, session);
        std::uint8_t value = 0x33;
        suite.expect(!store.ReadByte(kAddr, value) && value == 0x33,
            L"patch store: an Ok result with the wrong length is rejected defensively");
    }
    // Partial：失败。
    {
        FakeMemoryIoPort port;
        port.script = { MakePartial(Bytes{}) };
        MemoryPatchByteStore store(port, session);
        std::uint8_t value = 0x44;
        suite.expect(!store.ReadByte(kAddr, value) && value == 0x44, L"patch store: Partial is rejected");
    }
    // Unreadable：失败。
    {
        FakeMemoryIoPort port;
        port.script = { MakeUnreadable() };
        MemoryPatchByteStore store(port, session);
        std::uint8_t value = 0x55;
        suite.expect(!store.ReadByte(kAddr, value) && value == 0x55, L"patch store: Unreadable is rejected");
    }
    // Failed：失败。
    {
        FakeMemoryIoPort port;
        port.script = { MakeFailed() };
        MemoryPatchByteStore store(port, session);
        std::uint8_t value = 0x66;
        suite.expect(!store.ReadByte(kAddr, value) && value == 0x66, L"patch store: Failed is rejected");
    }
}

// ------------------------------------------------------------
// 三、WriteByte：approved 永远传 false；已落地的一字节必须交账本回读，
// 不能因后置失败抹掉事实；零字节批准请求不自动重试。
// ------------------------------------------------------------
void TestWriteByteAlwaysPassesUnapproved(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.writeScript = { MakeWriteOk(1) };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::StandardDriver);
    MemoryPatchByteStore store(port, session);

    suite.expect(store.WriteByte(kAddr, 0xCC), L"patch store: a single-byte ok write succeeds");
    suite.expect(port.writeCalls.size() == 1 && !port.writeCalls[0].approved && port.writeCalls[0].byteCount == 1,
        L"patch store: the port is always called with approved=false");
}

void TestWriteByteNeedsApprovalIsRejectedNotRetried(KswordTests::Suite& suite) {
    FakeMemoryIoPort port;
    port.writeScript = { MakeWriteNeedsApproval("force flag required") };
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::StandardDriver);
    MemoryPatchByteStore store(port, session);

    suite.expect(!store.WriteByte(kAddr, 0xCC),
        L"patch store: needsApproval is treated as a plain failure, never auto-retried with approval");
    suite.expect(port.writeCalls.size() == 1,
        L"patch store: a needsApproval response is not retried a second time");
}

void TestWriteBytePartialAndPlainFailureAreRejected(KswordTests::Suite& suite) {
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::StandardDriver);
    {
        FakeMemoryIoPort port;
        port.writeScript = { MakeWritePartial(0) };
        MemoryPatchByteStore store(port, session);
        suite.expect(!store.WriteByte(kAddr, 0xCC), L"patch store: a partial single-byte write is rejected");
    }
    {
        FakeMemoryIoPort port;
        port.writeScript = { MakeWriteFailed() };
        MemoryPatchByteStore store(port, session);
        suite.expect(!store.WriteByte(kAddr, 0xCC), L"patch store: a plain write failure is rejected");
    }
    {
        // ok 为真但 bytesDone 为 0（端口违反契约）：防御性地按失败处理。
        FakeMemoryIoPort port;
        port.writeScript = { MakeWriteOk(0) };
        MemoryPatchByteStore store(port, session);
        suite.expect(!store.WriteByte(kAddr, 0xCC),
            L"patch store: an ok write reporting zero bytes done is rejected defensively");
    }
}

void TestPersistedByteSurvivesPostWriteFailure(KswordTests::Suite& suite) {
    const MemoryTargetSession session = MakeSessionWith(Scope::ProcessVirtual, Channel::UserMode);
    // 真实 R3 端口的形状：WriteProcessMemory 写了 1 字节，FlushInstructionCache
    // 失败，因此 ok=false，但 bytesDone=1。账本必须保留可还原的原字节。
    auto flushedFailed = MakeWriteFailed("instruction cache flush failed after write");
    flushedFailed.bytesDone = 1;
    FakeMemoryIoPort port;
    port.script = { MakeOk(Bytes{0x55}), MakeOk(Bytes{0xCC}),
        MakeOk(Bytes{0xCC}), MakeOk(Bytes{0x55}) };
    port.writeScript = { flushedFailed, MakeWriteOk(1) };
    MemoryPatchByteStore store(port, session);
    ksword::memwb::Int3PatchLedger ledger;
    const ksword::memwb::PatchTarget target{ session.pid, 19, 7 };
    const auto installed = ledger.Install(target, kAddr, store, 1);
    suite.expect(installed.status == ksword::memwb::InstallStatus::Installed
        && installed.id == 1 && ledger.HasUnrestored()
        && ledger.Entries()[0].originalByte == 0x55,
        L"patch store integration: post-write failure cannot hide the persisted 0xCC or lose its original byte");
    const auto restored = ledger.Restore(installed.id, target, store);
    suite.expect(restored.status == ksword::memwb::RestoreStatus::Restored
        && !ledger.HasUnrestored() && port.writeCalls.size() == 2
        && !port.writeCalls[0].approved && !port.writeCalls[1].approved,
        L"patch store integration: the saved original can be restored without an automatic approval retry");

    for (const bool rolledBack : { false, true }) {
        FakeMemoryIoPort other;
        auto result = MakeWritePartial(1);
        result.rolledBack = rolledBack;
        other.writeScript = { result };
        MemoryPatchByteStore otherStore(other, session);
        suite.expect(otherStore.WriteByte(kAddr, 0xCC) == !rolledBack,
            L"patch store: only an unrolled-back persisted byte enters ledger verification, even with partial status");
    }
}

} // namespace

int RunMemwbPatchStoreTests() {
    KswordTests::Suite suite(L"MEMWB patch store");
    TestSupportedRoutesReachThePort(suite);
    TestKernelScopeRejectedWithoutTouchingThePort(suite);
    TestPhysicalScopeRejectedWithoutTouchingThePort(suite);
    TestDdmaChannelRejectedEvenWithProcessScope(suite);
    TestReadByteMapsPortResults(suite);
    TestWriteByteAlwaysPassesUnapproved(suite);
    TestWriteByteNeedsApprovalIsRejectedNotRetried(suite);
    TestWriteBytePartialAndPlainFailureAreRejected(suite);
    TestPersistedByteSurvivesPostWriteFailure(suite);
    suite.report();
    return suite.failures();
}
