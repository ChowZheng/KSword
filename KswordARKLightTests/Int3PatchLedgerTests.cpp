// int3 补丁账本（shared/evidence/memory_workbench/Int3PatchLedger.h）的离线测试。
//
// 为什么这个账本值得一整套断言：它属于**写错对象不会报错**的那一类。
// 旧的"断点"功能在换进程时不还原，删除时又把旧进程的原字节写进新进程的同一个
// 地址，界面照样显示"已删除"。所以本套件最看重各拒绝路径**一个字节都没写**。
//
// 断言原则与 NumericTextParseTests.cpp 一致：期望值手算写死；边界两侧都测（0xCC
// 的相邻字节 0xCB / 0xCD；pid 相同创建时间不同、创建时间相同 pid 不同）；拒绝与
// 失败路径显式测，并用 fake store 的读写调用计数证明"零写入"；失败时输出保持安全初值。
//
// fake store 带故障注入：整体或第 N 次读写失败、写入"成功但没落地"、"成功但落了别的
// 值"，以及读取失败时往 valueOut 里塞一个**毒值**——账本若在读取失败后仍使用
// valueOut，毒值会把它引到一条可观察到的错误路径上。

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/Int3PatchLedger.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace {

using ksword::memwb::InstallResult;
using ksword::memwb::InstallStatus;
using ksword::memwb::Int3PatchLedger;
using ksword::memwb::IPatchByteStore;
using ksword::memwb::kInt3PatchByte;
using ksword::memwb::PatchEntry;
using ksword::memwb::PatchRestoreOutcome;
using ksword::memwb::PatchTarget;
using ksword::memwb::RestoreResult;
using ksword::memwb::RestoreStatus;

// 测试用的地址：取一个超过 32 位的用户态地址，顺带验证账本没有把地址截断成 32 位。
constexpr std::uint64_t kAddr = 0x00007FF612340000ULL;
constexpr std::uint64_t kAddr2 = 0x00007FF612340010ULL;
constexpr std::uint64_t kAddr3 = 0x00007FF612340020ULL;

// 手算写死的"原字节"。0x55 不是 0xCC，也不是它的相邻值。
constexpr std::uint8_t kOrig = 0x55;

// 三个进程实例：
//   kProcA       pid 100，创建时间 ...01；
//   kProcAReused pid 也是 100，但创建时间不同——pid 被复用后的"新进程"；
//   kProcB       pid 200，创建时间与 A 相同——只有 pid 不同的进程。
const PatchTarget kProcA{ 100U, 0x01DB000000000001ULL, 7ULL };
const PatchTarget kProcAReused{ 100U, 0x01DB000000000999ULL, 8ULL };
const PatchTarget kProcB{ 200U, 0x01DB000000000001ULL, 7ULL };

// 一次存取调用的记录：写了哪个地址、写了什么值。
struct WriteCall {
    std::uint64_t address = 0;   // 写入的地址
    std::uint8_t value = 0;      // 写入的字节
};

// FakeByteStore：内存数组加故障注入的 IPatchByteStore 实现。
//
// 调用方法：往 memory 里放"目标进程里的字节"，不在表里的地址视为未映射（读写都失败）。
// 各种 xxxOnCall 都是 1-based 的调用序号（0 表示不注入），按"该 store 上全部读 / 全部
// 写"各自计数，所以测试里能精确地让"回读那一次"或"回滚那一次"失败。
class FakeByteStore final : public IPatchByteStore {
public:
    std::map<std::uint64_t, std::uint8_t> memory;   // 模拟的目标内存
    int readCalls = 0;                              // ReadByte 被调用的总次数
    int writeCalls = 0;                             // WriteByte 被调用的总次数
    std::vector<WriteCall> writes;                  // 每一次写调用（含失败的）的记录
    bool failAllReads = false;                      // 为真时每次读都失败
    bool failAllWrites = false;                     // 为真时每次写都失败（字节不变）
    int failReadOnCall = 0;                         // 第 N 次读失败
    int failWriteOnCall = 0;                        // 第 N 次写失败（字节不变）
    int dropWriteOnCall = 0;                        // 第 N 次写返回成功，但字节没有落地
    int corruptWriteOnCall = 0;                     // 第 N 次写返回成功，但落地的是 corruptValue
    std::uint8_t corruptValue = 0x90;               // corruptWriteOnCall 那次实际落地的值
    std::uint8_t failedReadPoison = 0xCC;           // 读取失败时塞进 valueOut 的毒值

    // ReadByte：读取失败时返回 false，并故意把毒值留在 valueOut 里。
    bool ReadByte(std::uint64_t address, std::uint8_t& valueOut) override {
        ++readCalls;
        // 是否命中注入的读失败（全部失败，或恰好是第 N 次）。
        const bool injected = failAllReads || (failReadOnCall != 0 && readCalls == failReadOnCall);
        const auto found = memory.find(address);
        if (injected || found == memory.end()) {
            valueOut = failedReadPoison;
            return false;
        }
        valueOut = found->second;
        return true;
    }

    // WriteByte：先记录这次调用（不论成败），再按注入决定落地与否。
    bool WriteByte(std::uint64_t address, std::uint8_t value) override {
        ++writeCalls;
        WriteCall call;
        call.address = address;
        call.value = value;
        writes.push_back(call);
        // 是否命中注入的写失败。
        const bool injected = failAllWrites || (failWriteOnCall != 0 && writeCalls == failWriteOnCall);
        const auto found = memory.find(address);
        if (injected || found == memory.end()) {
            return false;
        }
        // 返回 true 但不落地：模拟只读映射、写时复制被丢弃等"调用成功但字节没变"。
        if (dropWriteOnCall != 0 && writeCalls == dropWriteOnCall) {
            return true;
        }
        // 返回 true 但落了别的值：模拟被其它线程立刻改写。
        if (corruptWriteOnCall != 0 && writeCalls == corruptWriteOnCall) {
            found->second = corruptValue;
            return true;
        }
        found->second = value;
        return true;
    }
};

// MakeStore：造一个在 address 处有一个字节的 store。
FakeByteStore MakeStore(std::uint64_t address, std::uint8_t byte) {
    FakeByteStore store;
    store.memory[address] = byte;
    return store;
}

// IdsOf：取出条目列表里的 id 序列，便于整体比较顺序。
std::vector<std::uint64_t> IdsOf(const std::vector<PatchEntry>& entries) {
    std::vector<std::uint64_t> ids;
    for (const PatchEntry& entry : entries) {
        ids.push_back(entry.id);
    }
    return ids;
}

// 一、默认值与常量：默认构造的结果绝不能看起来像成功。
void TestDefaultsAndConstants(KswordTests::Suite& suite) {
    suite.expect(kInt3PatchByte == 0xCC, L"int3 ledger: the patch byte is 0xCC");

    const InstallResult install;
    suite.expect(install.status == InstallStatus::None && install.id == 0ULL
            && !install.rollbackAttempted && !install.rollbackWriteOk,
        L"int3 ledger: a default InstallResult is None with id 0 and no rollback, never a success");

    const RestoreResult restore;
    suite.expect(restore.status == RestoreStatus::None && !restore.hasObservedByte && restore.observedByte == 0,
        L"int3 ledger: a default RestoreResult is None with nothing observed, never Restored");

    const PatchEntry entry;
    suite.expect(entry.id == 0ULL && entry.patchByte == 0xCC,
        L"int3 ledger: a default entry has the invalid id 0 and patch byte 0xCC");

    const Int3PatchLedger ledger;
    suite.expect(!ledger.HasUnrestored() && ledger.Entries().empty() && ledger.OrphanedEntries().empty(),
        L"int3 ledger: a fresh ledger holds nothing");
}

// 二、安装成功路径：记账字段、写入内容、回读次数。
void TestInstallHappyPath(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore store = MakeStore(kAddr, kOrig);

    const InstallResult result = ledger.Install(kProcA, kAddr, store, 12345ULL);
    suite.expect(result.status == InstallStatus::Installed, L"int3 ledger: a clean install succeeds");
    suite.expect(result.id == 1ULL, L"int3 ledger: the first id is 1");
    suite.expect(store.memory[kAddr] == 0xCC, L"int3 ledger: the target byte is now 0xCC");

    // 恰好一次写（0xCC）、两次读（原字节 + 回读验证），不多不少。
    suite.expect(store.writeCalls == 1 && store.writes[0].address == kAddr && store.writes[0].value == 0xCC,
        L"int3 ledger: install performs exactly one write of 0xCC to the requested address");
    suite.expect(store.readCalls == 2, L"int3 ledger: install reads the original byte and then reads back once");

    // 条目字段逐个手算核对。
    suite.expect(ledger.Entries().size() == 1U, L"int3 ledger: one entry is recorded");
    const auto found = ledger.FindById(1ULL);
    suite.expect(found.has_value(), L"int3 ledger: the entry can be found by its id");
    if (found.has_value()) {
        suite.expect(found->id == 1ULL && found->pid == 100U, L"int3 ledger: entry records id and pid");
        suite.expect(found->processCreateTime100ns == 0x01DB000000000001ULL,
            L"int3 ledger: entry records the process create time");
        suite.expect(found->attachGeneration == 7ULL, L"int3 ledger: entry records the attach generation");
        suite.expect(found->address == kAddr, L"int3 ledger: entry records the full 64-bit address");
        suite.expect(found->originalByte == kOrig, L"int3 ledger: entry records the byte that was overwritten");
        suite.expect(found->patchByte == 0xCC, L"int3 ledger: entry records patch byte 0xCC");
        suite.expect(found->installedAtTick == 12345ULL, L"int3 ledger: entry records the install tick");
    }
    suite.expect(ledger.HasUnrestored(), L"int3 ledger: an installed patch counts as unrestored");

    // 找不到的 id：0（无效）与从未发放过的 2。
    suite.expect(!ledger.FindById(0ULL).has_value() && !ledger.FindById(2ULL).has_value(),
        L"int3 ledger: id 0 and an unissued id are not found");
}

// 三、原字节边界：只有恰好 0xCC 被拒绝，相邻值与首尾值都能装。
void TestOriginalByteBoundary(KswordTests::Suite& suite) {
    // 0xCC 的两侧相邻值，加上字节取值范围的两端，全部必须安装成功并记下原值。
    const std::uint8_t okValues[] = { 0x00, 0xCB, 0xCD, 0xFF };
    for (const std::uint8_t original : okValues) {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, original);
        const InstallResult result = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(result.status == InstallStatus::Installed,
            L"int3 ledger: an original byte next to 0xCC is accepted");
        const auto found = ledger.FindById(result.id);
        suite.expect(found.has_value() && found->originalByte == original,
            L"int3 ledger: the recorded original byte equals what was read");
    }

    // 恰好 0xCC：拒绝，零写入，不记账。
    Int3PatchLedger ledger;
    FakeByteStore store = MakeStore(kAddr, 0xCC);
    const InstallResult rejected = ledger.Install(kProcA, kAddr, store, 1ULL);
    suite.expect(rejected.status == InstallStatus::AlreadyContainsPatchByte,
        L"int3 ledger: an address that already holds 0xCC is rejected");
    suite.expect(rejected.id == 0ULL, L"int3 ledger: a rejected install returns id 0");
    suite.expect(store.writeCalls == 0, L"int3 ledger: the 0xCC rejection performs zero writes");
    suite.expect(store.memory[kAddr] == 0xCC, L"int3 ledger: the 0xCC rejection leaves the byte untouched");
    suite.expect(ledger.Entries().empty() && !ledger.HasUnrestored(),
        L"int3 ledger: the 0xCC rejection records nothing");
    suite.expect(!rejected.rollbackAttempted, L"int3 ledger: the 0xCC rejection needs no rollback");
}

// 四、重复安装：只对"同目标同地址"拒绝，且拒绝时不做任何 I/O。
void TestDuplicateInstall(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore store = MakeStore(kAddr, kOrig);
    const InstallResult first = ledger.Install(kProcA, kAddr, store, 1ULL);
    suite.expect(first.status == InstallStatus::Installed, L"int3 ledger: duplicate test setup installs");

    const int readsBefore = store.readCalls;
    const int writesBefore = store.writeCalls;

    // 同目标同地址：Duplicate，而不是 AlreadyContainsPatchByte（此时地址上确实是 0xCC，
    // 若先读再判就会误报成后者）。
    const InstallResult second = ledger.Install(kProcA, kAddr, store, 2ULL);
    suite.expect(second.status == InstallStatus::Duplicate,
        L"int3 ledger: a second install at the same target and address is a duplicate");
    suite.expect(second.id == 0ULL, L"int3 ledger: a duplicate returns id 0");
    suite.expect(store.readCalls == readsBefore && store.writeCalls == writesBefore,
        L"int3 ledger: a duplicate is detected with zero reads and zero writes");
    suite.expect(ledger.Entries().size() == 1U, L"int3 ledger: a duplicate adds no entry");

    // 附加代次不同但仍是同一个进程实例：补丁物理上还在，依旧是重复。
    const PatchTarget reattached{ kProcA.pid, kProcA.processCreateTime100ns, 99ULL };
    suite.expect(ledger.Install(reattached, kAddr, store, 3ULL).status == InstallStatus::Duplicate,
        L"int3 ledger: a different attach generation does not make it a different process");

    // 同目标不同地址：允许。
    store.memory[kAddr2] = 0x10;
    suite.expect(ledger.Install(kProcA, kAddr2, store, 4ULL).status == InstallStatus::Installed,
        L"int3 ledger: the same target at another address is allowed");

    // 同地址不同 pid：是另一个进程，允许（它有自己的 store）。
    FakeByteStore storeB = MakeStore(kAddr, 0x20);
    suite.expect(ledger.Install(kProcB, kAddr, storeB, 5ULL).status == InstallStatus::Installed,
        L"int3 ledger: the same address in a different pid is allowed");

    // 同地址同 pid 不同创建时间：pid 复用后的新进程，也是另一个进程，允许。
    FakeByteStore storeReused = MakeStore(kAddr, 0x30);
    suite.expect(ledger.Install(kProcAReused, kAddr, storeReused, 6ULL).status == InstallStatus::Installed,
        L"int3 ledger: the same address in a pid-reusing process is allowed");
    suite.expect(ledger.Entries().size() == 4U, L"int3 ledger: four distinct entries are recorded");

    // 另起一本账：A 在 kAddr 装了一条之后，另一个进程 C 的同一地址上本来就是 0xCC。
    // 同地址但不同目标，C 在账本里没有条目，应判 AlreadyContainsPatchByte 而不是 Duplicate。
    Int3PatchLedger ledger2;
    FakeByteStore storeA2 = MakeStore(kAddr, kOrig);
    ledger2.Install(kProcA, kAddr, storeA2, 1ULL);
    FakeByteStore storeNaturalCc = MakeStore(kAddr, 0xCC);
    suite.expect(ledger2.Install(kProcB, kAddr, storeNaturalCc, 2ULL).status
            == InstallStatus::AlreadyContainsPatchByte,
        L"int3 ledger: a 0xCC that is not ours is not reported as a duplicate");
    suite.expect(storeNaturalCc.writeCalls == 0, L"int3 ledger: the foreign 0xCC is never written");
}

// 五、安装时读取失败：必须是 ReadFailed，且不得使用 valueOut 里的毒值。
void TestInstallReadFailure(KswordTests::Suite& suite) {
    // 毒值取两种：0xCC（若被使用会被误判成"已含补丁字节"），
    // 0x5A（若被使用会被当成原字节继续去写）。两种都必须落到 ReadFailed。
    const std::uint8_t poisons[] = { 0xCC, 0x5A };
    for (const std::uint8_t poison : poisons) {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        store.failAllReads = true;
        store.failedReadPoison = poison;
        const InstallResult result = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(result.status == InstallStatus::ReadFailed,
            L"int3 ledger: an unreadable original byte is a ReadFailed, whatever junk valueOut holds");
        suite.expect(result.id == 0ULL, L"int3 ledger: a read failure returns id 0");
        suite.expect(store.writeCalls == 0, L"int3 ledger: a read failure performs zero writes");
        suite.expect(store.memory[kAddr] == kOrig, L"int3 ledger: a read failure leaves the byte untouched");
        suite.expect(ledger.Entries().empty(), L"int3 ledger: a read failure records nothing");
    }

    // 未映射地址（不在 memory 里）同样走 ReadFailed。
    Int3PatchLedger ledger;
    FakeByteStore empty;
    suite.expect(ledger.Install(kProcA, kAddr, empty, 1ULL).status == InstallStatus::ReadFailed,
        L"int3 ledger: an unmapped address is a ReadFailed");
    suite.expect(empty.writeCalls == 0, L"int3 ledger: an unmapped address is never written");
}

// 六、安装时写入失败：不回滚（字节未改），不记账，不消耗 id。
void TestInstallWriteFailure(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore store = MakeStore(kAddr, kOrig);
    store.failAllWrites = true;
    const InstallResult failed = ledger.Install(kProcA, kAddr, store, 1ULL);
    suite.expect(failed.status == InstallStatus::WriteFailed,
        L"int3 ledger: a failing write is reported as WriteFailed");
    suite.expect(failed.id == 0ULL && !failed.rollbackAttempted,
        L"int3 ledger: WriteFailed has id 0 and attempts no rollback");
    suite.expect(store.writeCalls == 1, L"int3 ledger: WriteFailed stops after the one failed write");
    suite.expect(store.memory[kAddr] == kOrig, L"int3 ledger: WriteFailed leaves the byte untouched");
    suite.expect(ledger.Entries().empty(), L"int3 ledger: WriteFailed records nothing");

    // 失败的安装不消耗 id：随后的成功安装拿到的仍是 1。
    store.failAllWrites = false;
    const InstallResult retry = ledger.Install(kProcA, kAddr, store, 2ULL);
    suite.expect(retry.status == InstallStatus::Installed && retry.id == 1ULL,
        L"int3 ledger: a failed install does not consume an id");
}

// 七、回读验证失败：必须尝试把原字节写回，且不记账。
void TestInstallVerifyFailureRollsBack(KswordTests::Suite& suite) {
    // (a) 写入落了别的值 0x90：回读 0x90 != 0xCC -> VerifyFailed，回滚把 0x55 写回。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        store.corruptWriteOnCall = 1;
        store.corruptValue = 0x90;
        const InstallResult result = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(result.status == InstallStatus::VerifyFailed,
            L"int3 ledger: a read-back that differs from 0xCC is a VerifyFailed");
        suite.expect(result.id == 0ULL, L"int3 ledger: VerifyFailed returns id 0");
        suite.expect(result.rollbackAttempted && result.rollbackWriteOk,
            L"int3 ledger: VerifyFailed attempts the rollback and reports it succeeded");
        suite.expect(store.writes.size() == 2U && store.writes[0].value == 0xCC
                && store.writes[1].value == kOrig && store.writes[1].address == kAddr,
            L"int3 ledger: the rollback writes the original byte back at the same address");
        suite.expect(store.memory[kAddr] == kOrig, L"int3 ledger: the byte is back to its original after rollback");
        suite.expect(ledger.Entries().empty() && !ledger.HasUnrestored(),
            L"int3 ledger: VerifyFailed records nothing");
    }

    // (b) 写入返回成功但没落地：回读仍是 0x55，同样判 VerifyFailed 并尝试回滚。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        store.dropWriteOnCall = 1;
        const InstallResult result = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(result.status == InstallStatus::VerifyFailed,
            L"int3 ledger: a write that reports success but did not land is a VerifyFailed");
        suite.expect(result.rollbackAttempted && store.writeCalls == 2,
            L"int3 ledger: a dropped write is still followed by a rollback attempt");
    }

    // (c) 回读本身失败（第 2 次读）：字节其实已经被写成 0xCC，目标状态未知。
    //     必须仍然尝试回滚，否则会留下一个账本里没有记录的 0xCC。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        store.failReadOnCall = 2;
        const InstallResult result = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(result.status == InstallStatus::VerifyFailed,
            L"int3 ledger: a failing read-back is a VerifyFailed, not a success");
        suite.expect(result.rollbackAttempted && result.rollbackWriteOk,
            L"int3 ledger: a failing read-back still triggers the rollback");
        suite.expect(store.memory[kAddr] == kOrig,
            L"int3 ledger: the rollback removes the 0xCC that the failed read-back could not see");
        suite.expect(ledger.Entries().empty(), L"int3 ledger: a failing read-back records nothing");
    }

    // (d) 回滚写本身也失败（第 2 次写）：如实报告 rollbackWriteOk 为假，
    //     字节停在被写坏的 0x90，账本仍然不记账。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        store.corruptWriteOnCall = 1;
        store.failWriteOnCall = 2;
        const InstallResult result = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(result.status == InstallStatus::VerifyFailed && result.rollbackAttempted,
            L"int3 ledger: a failing rollback is still reported as VerifyFailed");
        suite.expect(!result.rollbackWriteOk, L"int3 ledger: a failing rollback is reported as failed");
        suite.expect(store.memory[kAddr] == 0x90, L"int3 ledger: a failed rollback leaves the byte where it was");
        suite.expect(ledger.Entries().empty(), L"int3 ledger: a failed rollback records nothing");

        // VerifyFailed 同样不消耗 id。
        store.corruptWriteOnCall = 0;
        store.failWriteOnCall = 0;
        store.memory[kAddr] = kOrig;
        const InstallResult retry = ledger.Install(kProcA, kAddr, store, 2ULL);
        suite.expect(retry.status == InstallStatus::Installed && retry.id == 1ULL,
            L"int3 ledger: a VerifyFailed install does not consume an id");
    }
}

// 八、还原成功路径。
void TestRestoreHappyPath(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore store = MakeStore(kAddr, kOrig);
    const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);

    // 附加代次与安装时不同（重新附加到同一进程）：身份只看 pid + 创建时间，仍可还原。
    const PatchTarget reattached{ kProcA.pid, kProcA.processCreateTime100ns, 42ULL };
    const RestoreResult restored = ledger.Restore(installed.id, reattached, store);
    suite.expect(restored.status == RestoreStatus::Restored, L"int3 ledger: a matching restore succeeds");
    suite.expect(store.memory[kAddr] == kOrig, L"int3 ledger: restore puts the original byte back");

    // 写调用恰好两次：安装的 0xCC，还原的原字节。
    suite.expect(store.writeCalls == 2 && store.writes[1].address == kAddr && store.writes[1].value == kOrig,
        L"int3 ledger: restore performs exactly one write of the original byte");
    // 读调用恰好四次：安装 2 次（原字节、回读），还原 2 次（当前字节、回读）。
    suite.expect(store.readCalls == 4, L"int3 ledger: restore reads the current byte and then reads back");
    suite.expect(ledger.Entries().empty() && !ledger.HasUnrestored(),
        L"int3 ledger: a restored entry is removed from the ledger");
    suite.expect(!ledger.FindById(installed.id).has_value(), L"int3 ledger: a restored entry is no longer found");

    // 再还原同一个 id：条目已经没了，NotFound，且零 I/O。
    const int readsBefore = store.readCalls;
    const int writesBefore = store.writeCalls;
    const RestoreResult again = ledger.Restore(installed.id, kProcA, store);
    suite.expect(again.status == RestoreStatus::NotFound, L"int3 ledger: restoring a restored id is NotFound");
    suite.expect(store.readCalls == readsBefore && store.writeCalls == writesBefore,
        L"int3 ledger: NotFound performs zero reads and zero writes");

    // id 0（无效 id）同样 NotFound。
    suite.expect(ledger.Restore(0ULL, kProcA, store).status == RestoreStatus::NotFound,
        L"int3 ledger: restoring id 0 is NotFound");

    // 还原之后同一地址可以重新安装（旧条目已不存在，不是 Duplicate）。
    suite.expect(ledger.Install(kProcA, kAddr, store, 2ULL).status == InstallStatus::Installed,
        L"int3 ledger: an address can be patched again after it was restored");
}

// 九、目标不符：旧缺陷的核心——绝不能把 A 进程的原字节写进 B 进程。
void TestRestoreTargetMismatchWritesNothing(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore storeA = MakeStore(kAddr, kOrig);
    const InstallResult installed = ledger.Install(kProcA, kAddr, storeA, 1ULL);
    const int storeAWrites = storeA.writeCalls;
    const int storeAReads = storeA.readCalls;

    // "新进程"的同一个地址上，原本就是它自己的 0xCC。旧缺陷会把 0x55 写进这里。
    FakeByteStore storeNew = MakeStore(kAddr, 0xCC);

    // 三种不符：只有 pid 不同、只有创建时间不同（pid 复用）、两者都不同。
    const PatchTarget onlyPidDiffers{ 200U, kProcA.processCreateTime100ns, kProcA.attachGeneration };
    const PatchTarget onlyCreateTimeDiffers{ kProcA.pid, 0x01DB000000000FFFULL, kProcA.attachGeneration };
    const PatchTarget bothDiffer{ 200U, 0x01DB000000000FFFULL, kProcA.attachGeneration };
    const PatchTarget mismatches[] = { onlyPidDiffers, onlyCreateTimeDiffers, bothDiffer, kProcAReused, kProcB };
    for (const PatchTarget& wrong : mismatches) {
        const RestoreResult result = ledger.Restore(installed.id, wrong, storeNew);
        suite.expect(result.status == RestoreStatus::TargetMismatch,
            L"int3 ledger: restoring against a different process is a TargetMismatch");
        suite.expect(storeNew.writeCalls == 0, L"int3 ledger: a TargetMismatch performs zero writes");
        suite.expect(storeNew.readCalls == 0, L"int3 ledger: a TargetMismatch does not even read the wrong process");
        suite.expect(storeNew.memory[kAddr] == 0xCC,
            L"int3 ledger: the other process keeps its own byte, not ours");
        suite.expect(ledger.Entries().size() == 1U, L"int3 ledger: a TargetMismatch keeps the entry");
        suite.expect(!result.hasObservedByte, L"int3 ledger: a TargetMismatch carries no observed byte");
    }

    // 就算传进来的是 A 的 store，只要目标身份不符照样不写——判据是 target，不是 store。
    const RestoreResult wrongTargetRightStore = ledger.Restore(installed.id, kProcAReused, storeA);
    suite.expect(wrongTargetRightStore.status == RestoreStatus::TargetMismatch,
        L"int3 ledger: the target identity is checked, not the store that was passed");
    suite.expect(storeA.writeCalls == storeAWrites && storeA.readCalls == storeAReads,
        L"int3 ledger: the original process is also untouched by a mismatched restore");

    // 用正确的目标才能还原，且不碰另一个进程。
    suite.expect(ledger.Restore(installed.id, kProcA, storeA).status == RestoreStatus::Restored,
        L"int3 ledger: the matching target can still restore afterwards");
    suite.expect(storeNew.memory[kAddr] == 0xCC && storeNew.writeCalls == 0,
        L"int3 ledger: the other process was never written during the whole sequence");
}

// 十、Diverged 与读取失败：当前字节不是 0xCC 就不写。
void TestRestoreDivergedAndReadFailure(KswordTests::Suite& suite) {
    // 当前字节 = 0xCB / 0xCD（0xCC 的相邻值）、0x55（恰好等于原字节）、0x00：都是 Diverged。
    const std::uint8_t divergedValues[] = { 0xCB, 0xCD, kOrig, 0x00 };
    for (const std::uint8_t changedTo : divergedValues) {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
        store.memory[kAddr] = changedTo;   // 目标内容被别处改过
        const RestoreResult result = ledger.Restore(installed.id, kProcA, store);
        suite.expect(result.status == RestoreStatus::Diverged,
            L"int3 ledger: a byte that is no longer 0xCC is Diverged");
        suite.expect(result.hasObservedByte && result.observedByte == changedTo,
            L"int3 ledger: Diverged reports the byte that was actually found");
        suite.expect(store.writeCalls == 1, L"int3 ledger: Diverged performs no write beyond the install");
        suite.expect(store.memory[kAddr] == changedTo, L"int3 ledger: Diverged leaves the foreign byte alone");
        suite.expect(ledger.Entries().size() == 1U, L"int3 ledger: Diverged keeps the entry");
    }

    // 仍是 0xCC 时正常还原（与上面的相邻值形成两侧对照）。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
        suite.expect(ledger.Restore(installed.id, kProcA, store).status == RestoreStatus::Restored,
            L"int3 ledger: an unchanged 0xCC restores normally");
    }

    // Diverged 之后 Discard：不做 I/O、删除条目；重复 Discard 与未知 id 返回 false。
    Int3PatchLedger ledger;
    FakeByteStore store = MakeStore(kAddr, kOrig);
    const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
    store.memory[kAddr] = 0x90;
    ledger.Restore(installed.id, kProcA, store);
    const int readsBefore = store.readCalls;
    const int writesBefore = store.writeCalls;
    suite.expect(ledger.Discard(installed.id), L"int3 ledger: a diverged entry can be discarded");
    suite.expect(store.readCalls == readsBefore && store.writeCalls == writesBefore,
        L"int3 ledger: discarding performs no I/O");
    suite.expect(ledger.Entries().empty() && !ledger.HasUnrestored(),
        L"int3 ledger: a discarded entry is gone");
    suite.expect(!ledger.Discard(installed.id), L"int3 ledger: discarding twice reports false the second time");
    suite.expect(!ledger.Discard(0ULL) && !ledger.Discard(999ULL),
        L"int3 ledger: discarding an unknown id reports false");

    // 读取当前字节失败：毒值 0xCC 若被误用会通过 Diverged 检查并去写，必须不写。
    Int3PatchLedger ledger2;
    FakeByteStore store2 = MakeStore(kAddr, kOrig);
    const InstallResult installed2 = ledger2.Install(kProcA, kAddr, store2, 1ULL);
    store2.failAllReads = true;
    store2.failedReadPoison = 0xCC;
    const RestoreResult readFailed = ledger2.Restore(installed2.id, kProcA, store2);
    suite.expect(readFailed.status == RestoreStatus::ReadFailed,
        L"int3 ledger: an unreadable current byte is a ReadFailed, whatever junk valueOut holds");
    suite.expect(store2.writeCalls == 1 && store2.memory[kAddr] == 0xCC,
        L"int3 ledger: ReadFailed performs no write beyond the install");
    suite.expect(ledger2.Entries().size() == 1U && !readFailed.hasObservedByte,
        L"int3 ledger: ReadFailed keeps the entry and observes nothing");
    store2.failAllReads = false;
    suite.expect(ledger2.Restore(installed2.id, kProcA, store2).status == RestoreStatus::Restored,
        L"int3 ledger: a retry after the fault clears restores normally");
}

// 十一、还原时写入失败与回读失败：条目保留，可重试，不假报成功。
void TestRestoreWriteAndVerifyFailures(KswordTests::Suite& suite) {
    // 读写序号：安装占 读1 读2 写1；还原占 读3 读4 写2。

    // (a) 还原的写失败（第 2 次写）：条目保留，字节仍是 0xCC，清除故障后重试成功。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
        store.failWriteOnCall = 2;
        const RestoreResult result = ledger.Restore(installed.id, kProcA, store);
        suite.expect(result.status == RestoreStatus::WriteFailed, L"int3 ledger: a failing restore write is WriteFailed");
        suite.expect(store.memory[kAddr] == 0xCC && ledger.Entries().size() == 1U,
            L"int3 ledger: WriteFailed keeps both the patch and its entry");
        store.failWriteOnCall = 0;
        suite.expect(ledger.Restore(installed.id, kProcA, store).status == RestoreStatus::Restored,
            L"int3 ledger: a WriteFailed restore can be retried");
    }

    // (b) 写成"成功"但落了别的值 0x90：回读不等于原字节 -> VerifyFailed，并带回回读值。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
        store.corruptWriteOnCall = 2;
        const RestoreResult result = ledger.Restore(installed.id, kProcA, store);
        suite.expect(result.status == RestoreStatus::VerifyFailed,
            L"int3 ledger: a restore that landed the wrong byte is a VerifyFailed");
        suite.expect(result.hasObservedByte && result.observedByte == 0x90,
            L"int3 ledger: VerifyFailed reports the byte that the read-back saw");
        suite.expect(ledger.Entries().size() == 1U, L"int3 ledger: VerifyFailed keeps the entry");
    }

    // (c) 写成"成功"但没落地：回读仍是 0xCC -> VerifyFailed。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
        store.dropWriteOnCall = 2;
        const RestoreResult result = ledger.Restore(installed.id, kProcA, store);
        suite.expect(result.status == RestoreStatus::VerifyFailed && result.observedByte == 0xCC,
            L"int3 ledger: a dropped restore write is a VerifyFailed that observed 0xCC");
        suite.expect(ledger.HasUnrestored(), L"int3 ledger: a dropped restore write is not reported as restored");
    }

    // (d) 回读本身失败（第 4 次读）：无从证明还原成功，条目保留，且没有 observedByte。
    {
        Int3PatchLedger ledger;
        FakeByteStore store = MakeStore(kAddr, kOrig);
        const InstallResult installed = ledger.Install(kProcA, kAddr, store, 1ULL);
        store.failReadOnCall = 4;
        const RestoreResult result = ledger.Restore(installed.id, kProcA, store);
        suite.expect(result.status == RestoreStatus::VerifyFailed && !result.hasObservedByte,
            L"int3 ledger: a failing restore read-back is a VerifyFailed with nothing observed");
        suite.expect(ledger.Entries().size() == 1U, L"int3 ledger: an unproven restore keeps the entry");
    }
}

// 十二、全部还原：逐条结果，一条失败不中止后续，别的目标不碰。
void TestRestoreAllForTarget(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore storeA;
    storeA.memory[kAddr] = 0x11;
    storeA.memory[kAddr2] = 0x22;
    storeA.memory[kAddr3] = 0x33;
    ledger.Install(kProcA, kAddr, storeA, 1ULL);    // id 1
    ledger.Install(kProcA, kAddr2, storeA, 2ULL);   // id 2
    ledger.Install(kProcA, kAddr3, storeA, 3ULL);   // id 3
    FakeByteStore storeB = MakeStore(kAddr, 0x44);
    const InstallResult otherTarget = ledger.Install(kProcB, kAddr, storeB, 4ULL);   // id 4

    // 故障设计：A 的写序号 1-3 是安装，第 4 次写是还原 id 1 -> 让它失败；
    // id 2 的地址被别处改过 -> Diverged；id 3 正常。
    // 失败排在前面，证明既不是"遇到第一个失败就中止"，也不是"遇到第二个失败才中止"。
    storeA.failWriteOnCall = 4;
    storeA.memory[kAddr2] = 0x90;
    const int storeBReads = storeB.readCalls;
    const int storeBWrites = storeB.writeCalls;

    const std::vector<PatchRestoreOutcome> outcomes = ledger.RestoreAllForTarget(kProcA, storeA);
    suite.expect(outcomes.size() == 3U, L"int3 ledger: restore-all reports one outcome per entry of the target");
    if (outcomes.size() == 3U) {
        suite.expect(outcomes[0].id == 1ULL && outcomes[1].id == 2ULL && outcomes[2].id == 3ULL,
            L"int3 ledger: restore-all reports in install order");
        suite.expect(outcomes[0].address == kAddr && outcomes[1].address == kAddr2 && outcomes[2].address == kAddr3,
            L"int3 ledger: restore-all carries each entry's address");
        suite.expect(outcomes[0].result.status == RestoreStatus::WriteFailed,
            L"int3 ledger: the first entry failed to write");
        suite.expect(outcomes[1].result.status == RestoreStatus::Diverged,
            L"int3 ledger: the second entry diverged");
        suite.expect(outcomes[2].result.status == RestoreStatus::Restored,
            L"int3 ledger: the third entry is still restored after two failures");
    }
    suite.expect(storeA.memory[kAddr3] == 0x33, L"int3 ledger: restore-all really wrote the third original back");
    suite.expect(storeA.memory[kAddr2] == 0x90, L"int3 ledger: the diverged byte was left alone");

    // 账本里：id 1、2 保留（失败 / 偏离），id 3 已删，别的目标的 id 4 原封不动。
    suite.expect(IdsOf(ledger.Entries()) == std::vector<std::uint64_t>({ 1ULL, 2ULL, 4ULL }),
        L"int3 ledger: only the successfully restored entry is removed");
    suite.expect(storeB.readCalls == storeBReads && storeB.writeCalls == storeBWrites,
        L"int3 ledger: restore-all never touches another target's store");
    suite.expect(ledger.FindById(otherTarget.id).has_value() && storeB.memory[kAddr] == 0xCC,
        L"int3 ledger: another target's patch stays installed");

    // 没有匹配条目：返回空，零 I/O；pid 复用的"新进程"也算没有匹配。
    FakeByteStore untouched;
    suite.expect(ledger.RestoreAllForTarget(kProcAReused, untouched).empty(),
        L"int3 ledger: restore-all for a pid-reusing process finds nothing");
    suite.expect(untouched.readCalls == 0 && untouched.writeCalls == 0,
        L"int3 ledger: restore-all with no match performs zero I/O");
    suite.expect(Int3PatchLedger().RestoreAllForTarget(kProcA, untouched).empty(),
        L"int3 ledger: restore-all on an empty ledger returns an empty list");
}

// 十三、目标消失：孤立条目移出待还原列表，不可再还原。
void TestOnTargetGone(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore storeA;
    storeA.memory[kAddr] = 0x11;
    storeA.memory[kAddr2] = 0x22;
    ledger.Install(kProcA, kAddr, storeA, 1ULL);    // id 1
    ledger.Install(kProcA, kAddr2, storeA, 2ULL);   // id 2
    FakeByteStore storeB = MakeStore(kAddr, 0x33);
    ledger.Install(kProcB, kAddr, storeB, 3ULL);    // id 3
    FakeByteStore storeReused = MakeStore(kAddr, 0x44);
    ledger.Install(kProcAReused, kAddr, storeReused, 4ULL);   // id 4：同 pid、不同创建时间

    // 没有匹配的目标：空返回，账本不变。pid 相同但创建时间不同的不算匹配。
    suite.expect(ledger.OnTargetGone(555U, kProcA.processCreateTime100ns).empty(),
        L"int3 ledger: a gone target without entries orphans nothing");
    suite.expect(ledger.OnTargetGone(kProcA.pid, 0x01DB000000000FFFULL).empty(),
        L"int3 ledger: a matching pid with another create time orphans nothing");
    suite.expect(ledger.Entries().size() == 4U && ledger.OrphanedEntries().empty(),
        L"int3 ledger: unmatched OnTargetGone calls leave the ledger unchanged");

    // A 消失：id 1、2 被孤立，B 与 pid 复用的新进程不受影响。
    const std::vector<PatchEntry> orphaned = ledger.OnTargetGone(kProcA.pid, kProcA.processCreateTime100ns);
    suite.expect(IdsOf(orphaned) == std::vector<std::uint64_t>({ 1ULL, 2ULL }),
        L"int3 ledger: OnTargetGone returns exactly the entries of that process instance");
    if (orphaned.size() == 2U) {
        suite.expect(orphaned[0].address == kAddr && orphaned[0].originalByte == 0x11
                && orphaned[1].address == kAddr2 && orphaned[1].originalByte == 0x22,
            L"int3 ledger: orphaned entries keep their address and original byte for display");
    }
    suite.expect(IdsOf(ledger.Entries()) == std::vector<std::uint64_t>({ 3ULL, 4ULL }),
        L"int3 ledger: orphaned entries leave the to-restore list");
    suite.expect(IdsOf(ledger.OrphanedEntries()) == std::vector<std::uint64_t>({ 1ULL, 2ULL }),
        L"int3 ledger: orphaned entries are listed separately");
    suite.expect(ledger.HasUnrestored(), L"int3 ledger: other processes' patches still count as unrestored");
    suite.expect(!ledger.FindById(1ULL).has_value(), L"int3 ledger: an orphaned entry is not a to-restore entry");

    // 孤立条目不可还原：Orphaned，零 I/O；从未存在的 id 仍是 NotFound，两者可区分。
    const int readsBefore = storeReused.readCalls;
    const int writesBefore = storeReused.writeCalls;
    suite.expect(ledger.Restore(1ULL, kProcAReused, storeReused).status == RestoreStatus::Orphaned,
        L"int3 ledger: restoring an orphaned id is Orphaned, even against a pid-reusing process");
    suite.expect(storeReused.readCalls == readsBefore && storeReused.writeCalls == writesBefore,
        L"int3 ledger: the pid-reusing process is never touched on behalf of an orphan");
    suite.expect(ledger.Restore(99ULL, kProcA, storeA).status == RestoreStatus::NotFound,
        L"int3 ledger: an id that never existed stays NotFound");
    suite.expect(ledger.RestoreAllForTarget(kProcA, storeA).empty(),
        L"int3 ledger: restore-all no longer finds the orphaned entries");

    // 孤立之后再装：id 继续增长，不复用被孤立条目的 id；同 pid 的新进程允许在同一地址安装。
    FakeByteStore storeNew = MakeStore(kAddr2, 0x55);
    const PatchTarget newProcess{ kProcA.pid, 0x01DB000000001234ULL, 9ULL };
    const InstallResult afterOrphan = ledger.Install(newProcess, kAddr2, storeNew, 5ULL);
    suite.expect(afterOrphan.status == InstallStatus::Installed && afterOrphan.id == 5ULL,
        L"int3 ledger: ids keep growing past orphaned entries");

    // 界面提示过之后清空孤立列表：待还原条目与 id 计数不受影响。
    ledger.ClearOrphaned();
    suite.expect(ledger.OrphanedEntries().empty(), L"int3 ledger: ClearOrphaned empties the orphan list");
    suite.expect(ledger.Restore(1ULL, kProcA, storeA).status == RestoreStatus::NotFound,
        L"int3 ledger: a cleared orphan is NotFound afterwards");
    suite.expect(IdsOf(ledger.Entries()) == std::vector<std::uint64_t>({ 3ULL, 4ULL, 5ULL }),
        L"int3 ledger: ClearOrphaned leaves to-restore entries alone");

    // 其余目标也消失之后，不再有未还原补丁。
    ledger.OnTargetGone(kProcB.pid, kProcB.processCreateTime100ns);
    ledger.OnTargetGone(kProcAReused.pid, kProcAReused.processCreateTime100ns);
    ledger.OnTargetGone(newProcess.pid, newProcess.processCreateTime100ns);
    suite.expect(!ledger.HasUnrestored() && ledger.Entries().empty(),
        L"int3 ledger: once every target is gone nothing is left to restore");
}

// 十四、id 单调递增且永不复用：还原、丢弃、孤立之后都不回收。
void TestIdsAreMonotonicAndNeverReused(KswordTests::Suite& suite) {
    Int3PatchLedger ledger;
    FakeByteStore store;
    store.memory[kAddr] = 0x11;
    store.memory[kAddr2] = 0x22;
    store.memory[kAddr3] = 0x33;
    const std::uint64_t id1 = ledger.Install(kProcA, kAddr, store, 1ULL).id;
    const std::uint64_t id2 = ledger.Install(kProcA, kAddr2, store, 2ULL).id;
    suite.expect(id1 == 1ULL && id2 == 2ULL, L"int3 ledger: ids are issued 1, 2 in order");

    // 拒绝的安装（重复）夹在中间不产生空洞。
    suite.expect(ledger.Install(kProcA, kAddr, store, 3ULL).status == InstallStatus::Duplicate,
        L"int3 ledger: id test setup duplicate");

    // 还原当前最大的 id（2），再装一条：必须是 3，不能回到 2。
    suite.expect(ledger.Restore(id2, kProcA, store).status == RestoreStatus::Restored,
        L"int3 ledger: id test restores the highest id");
    const std::uint64_t id3 = ledger.Install(kProcA, kAddr3, store, 4ULL).id;
    suite.expect(id3 == 3ULL, L"int3 ledger: the id of a restored entry is not reused");

    // 丢弃当前最大的 id（3），再装：必须是 4。
    suite.expect(ledger.Discard(id3), L"int3 ledger: id test discards the highest id");
    const std::uint64_t id4 = ledger.Install(kProcA, kAddr2, store, 5ULL).id;
    suite.expect(id4 == 4ULL, L"int3 ledger: the id of a discarded entry is not reused");

    // 孤立当前所有条目（含最大的 4），再装：必须是 5。
    ledger.OnTargetGone(kProcA.pid, kProcA.processCreateTime100ns);
    FakeByteStore storeB = MakeStore(kAddr, 0x44);
    const std::uint64_t id5 = ledger.Install(kProcB, kAddr, storeB, 6ULL).id;
    suite.expect(id5 == 5ULL, L"int3 ledger: the id of an orphaned entry is not reused");

    // 清空孤立列表也不会让计数回退。
    ledger.ClearOrphaned();
    FakeByteStore storeC = MakeStore(kAddr2, 0x55);
    const std::uint64_t id6 = ledger.Install(kProcB, kAddr2, storeC, 7ULL).id;
    suite.expect(id6 == 6ULL, L"int3 ledger: clearing the orphan list does not rewind the id counter");

    // 待还原列表按安装顺序，id 严格递增。
    const std::vector<PatchEntry>& entries = ledger.Entries();
    suite.expect(IdsOf(entries) == std::vector<std::uint64_t>({ 5ULL, 6ULL }),
        L"int3 ledger: entries are listed in install order with strictly increasing ids");
    if (entries.size() == 2U) {
        suite.expect(entries[0].installedAtTick == 6ULL && entries[1].installedAtTick == 7ULL,
            L"int3 ledger: each entry keeps its own install tick");
    }
}

} // namespace

int RunMemwbInt3LedgerTests() {
    KswordTests::Suite suite(L"MEMWB int3 ledger");
    TestDefaultsAndConstants(suite);
    TestInstallHappyPath(suite);
    TestOriginalByteBoundary(suite);
    TestDuplicateInstall(suite);
    TestInstallReadFailure(suite);
    TestInstallWriteFailure(suite);
    TestInstallVerifyFailureRollsBack(suite);
    TestRestoreHappyPath(suite);
    TestRestoreTargetMismatchWritesNothing(suite);
    TestRestoreDivergedAndReadFailure(suite);
    TestRestoreWriteAndVerifyFailures(suite);
    TestRestoreAllForTarget(suite);
    TestOnTargetGone(suite);
    TestIdsAreMonotonicAndNeverReused(suite);
    suite.report();
    return suite.failures();
}
