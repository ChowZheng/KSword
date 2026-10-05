#pragma once

// MemoryWriteTransaction 测试套件各 .cpp 共用的支撑：伪 store / 伪确认 / 伪审计、夹具与简写。
//
// 套件按职责拆成三个文件（单文件不得超过 800 行），共用同一个 KswordTests::Suite，
// 只在套件名 "MEMWB write txn" 下报一次汇总：
//   MemoryWriteTransactionTests.cpp         入口 + 暂存（Idle/Staged 不写）/ 模式 / 模式切换三选一
//   MemoryWriteTransactionTests.Commit.cpp  提交顺序 / 取消 / 抑制 / 会话无效 / 陈旧 / 写前复核 / 重入
//   MemoryWriteTransactionTests.Write.cpp   显式同意 / 写失败 / 回读失败 / 告警位 / 审计 / 重试
//
// 夹具约定：标准基线窗口为 [0x1000, 0x1010)，地址 0x1000 + i 处的字节 = i * 0x11
// （00 11 22 ... FF）。目标内存（FakeStore）初始内容与基线一致。测试里暂存的值一律取
// 0x01~0x0F 一类不会碰巧等于基线的数，免得"等于基线则消失"的规则误伤断言。
// 三块夹具：A = 0x1002 {01}（原 22）、B = 0x1006 {02 03}（原 66 77）、
// C = 0x100C {04}（原 CC），互不相邻，因此保持三个独立差异块。
//
// 所有伪对象把调用写进同一份有序日志 Log，测试按序断言整条调用序列：
//   "R:<十六进制地址>+<十进制长度>"        store.Read
//   "W:<十六进制地址>+<长度>:a<0|1>"       store.Write（a1 表示带显式同意标志）
//   "UI"                                    ConfirmUi
//   "APPROVE:<块序号>"                      ConfirmApproval
//   "AUDIT:<事件名>"                        审计记录

#include "TestSupport.h"

#include "../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MemwbTxnTests {

using ksword::memwb::AccessResult;
using ksword::memwb::ApprovalAnswer;
using ksword::memwb::ApprovalRequest;
using ksword::memwb::AuditEvent;
using ksword::memwb::AuditRecord;
using ksword::memwb::Channel;
using ksword::memwb::CommitOutcome;
using ksword::memwb::CommitReport;
using ksword::memwb::DiffBlock;
using ksword::memwb::IAuditSink;
using ksword::memwb::IByteStore;
using ksword::memwb::IConfirmationSink;
using ksword::memwb::MemoryDiffOverlay;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::MemoryWriteTransaction;
using ksword::memwb::ModeSwitchDecision;
using ksword::memwb::ModeSwitchStatus;
using ksword::memwb::Scope;
using ksword::memwb::SessionRevisions;
using ksword::memwb::StageStatus;
using ksword::memwb::UiConfirmRequest;
using ksword::memwb::WriteMode;
using State = ksword::memwb::MemoryWriteTransaction::State;

// 字节序列与有序调用日志的简写。
using Bytes = std::vector<std::uint8_t>;
using Log = std::vector<std::string>;

// 标准窗口起点；窗口为 [0x1000, 0x1010)。
inline constexpr std::uint64_t kBase = 0x1000ULL;

// 三块夹具的起始地址。
inline constexpr std::uint64_t kA = 0x1002ULL;
inline constexpr std::uint64_t kB = 0x1006ULL;
inline constexpr std::uint64_t kC = 0x100CULL;

// 把一个字节常量包成可选字节，便于与 BaselineByte 的返回值比较。
// 先显式截成 uint8_t 再构造，免得库模板里再做一次隐式窄化而触发 /W4 警告。
inline std::optional<std::uint8_t> Val(int value) {
    return std::optional<std::uint8_t>(static_cast<std::uint8_t>(value));
}

// 标准基线：第 i 个字节 = i * 0x11。
inline Bytes StandardBaseline() {
    Bytes data;
    for (int index = 0; index < 16; ++index) {
        data.push_back(static_cast<std::uint8_t>(index * 0x11));
    }
    return data;
}

// 无符号数转小写十六进制串（不带前缀），写日志用。
inline std::string Hex(std::uint64_t value) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

// 审计事件名，写日志用。
inline const char* EventName(AuditEvent event) {
    switch (event) {
    case AuditEvent::CommitStarted: return "CommitStarted";
    case AuditEvent::UiConfirmAccepted: return "UiConfirmAccepted";
    case AuditEvent::UiConfirmDenied: return "UiConfirmDenied";
    case AuditEvent::UiConfirmSuppressed: return "UiConfirmSuppressed";
    case AuditEvent::ApprovalAnswered: return "ApprovalAnswered";
    case AuditEvent::CommitFinished: return "CommitFinished";
    }
    return "Unknown";
}

// 一块地址的写故障脚本。
struct WriteFault {
    // fail：写失败（ok = false）。
    bool fail = false;
    // partialOk：返回 ok = true 但 partial = true。
    bool partialOk = false;
    // bytesDone：声明完成的字节数；非回滚时这些字节真的落进目标内存。
    std::uint64_t bytesDone = 0;
    // rolledBack：后端声明已回滚；此时任何字节都不落地。
    bool rolledBack = false;
    // text：失败原因。
    std::string text = "scripted write fault";
};

// FakeStore：可编排故障的伪目标内存。
class FakeStore final : public IByteStore {
public:
    // memory：目标内存，绝对地址 -> 字节。
    std::map<std::uint64_t, std::uint8_t> memory;
    // log：共享有序日志。
    Log* log = nullptr;
    // readCalls / writeCalls：Read / Write 调用次数（writeCalls 含因需要同意而未写入的）。
    int readCalls = 0;
    int writeCalls = 0;
    // needApproval：这些起始地址的写入在没有同意标志时要求同意。
    std::set<std::uint64_t> needApproval;
    // alwaysNeedApproval：这些起始地址即使带了同意标志也继续要求同意。
    std::set<std::uint64_t> alwaysNeedApproval;
    // writeFault：按起始地址编排的写故障。
    std::map<std::uint64_t, WriteFault> writeFault;
    // corruptAfterWrite：写成功后把块最后一个字节取反，使回读与 after 不符。
    std::set<std::uint64_t> corruptAfterWrite;
    // dirtyOnWrite / rmwOnWrite / dirtyOnRead / rmwOnRead：在这些起始地址的访问结果里置告警位。
    std::set<std::uint64_t> dirtyOnWrite;
    std::set<std::uint64_t> rmwOnWrite;
    std::set<std::uint64_t> dirtyOnRead;
    std::set<std::uint64_t> rmwOnRead;
    // readHook：返回 true 时用 out 替换默认读取结果。参数：地址、长度、输出。
    std::function<bool(std::uint64_t, std::uint64_t, AccessResult&)> readHook;
    // probe：每次 Read / Write 进入时调用，测试用它记录当时的状态机状态。
    std::function<void()> probe;
    // onWrite：每次 Write 进入时调用（在日志之后），测试用它抛异常。
    std::function<void()> onWrite;

    // Read：默认从 memory 取字节；缺一个字节就返回 partial（取到的前缀）。
    AccessResult Read(std::uint64_t address, std::uint64_t length) override {
        ++readCalls;
        if (log != nullptr) {
            log->push_back("R:" + Hex(address) + "+" + std::to_string(length));
        }
        if (probe) {
            probe();
        }

        AccessResult result;
        result.ok = true;
        for (std::uint64_t offset = 0; offset < length; ++offset) {
            const auto found = memory.find(address + offset);
            if (found == memory.end()) {
                result.ok = false;
                result.partial = result.bytesDone > 0;
                result.failureText = "unmapped";
                break;
            }
            result.data.push_back(found->second);
            ++result.bytesDone;
        }
        result.scratchAreaDirty = dirtyOnRead.count(address) != 0;
        result.readModifyWriteWindow = rmwOnRead.count(address) != 0;

        AccessResult overridden;
        if (readHook && readHook(address, length, overridden)) {
            return overridden;
        }
        return result;
    }

    // Write：按脚本模拟需要同意、各种写故障与回读被破坏。
    AccessResult Write(std::uint64_t address, const Bytes& bytes, bool explicitApproval) override {
        ++writeCalls;
        if (log != nullptr) {
            log->push_back("W:" + Hex(address) + "+" + std::to_string(bytes.size())
                + (explicitApproval ? ":a1" : ":a0"));
        }
        if (probe) {
            probe();
        }
        if (onWrite) {
            onWrite();
        }

        AccessResult result;
        result.scratchAreaDirty = dirtyOnWrite.count(address) != 0;
        result.readModifyWriteWindow = rmwOnWrite.count(address) != 0;

        // 需要显式同意：没有写入任何字节。
        const bool wantsApproval = alwaysNeedApproval.count(address) != 0
            || (needApproval.count(address) != 0 && !explicitApproval);
        if (wantsApproval) {
            result.needsExplicitApproval = true;
            result.failureText = "needs explicit approval";
            return result;
        }

        // 编排的写故障。
        const auto fault = writeFault.find(address);
        if (fault != writeFault.end()) {
            const WriteFault& f = fault->second;
            result.partial = f.partialOk;
            result.ok = !f.fail;
            result.bytesDone = f.bytesDone;
            result.rolledBack = f.rolledBack;
            result.failureText = f.text;
            if (!f.rolledBack) {
                for (std::uint64_t offset = 0; offset < f.bytesDone && offset < bytes.size(); ++offset) {
                    memory[address + offset] = bytes[static_cast<std::size_t>(offset)];
                }
            }
            return result;
        }

        // 正常写入。
        for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
            memory[address + offset] = bytes[offset];
        }
        if (corruptAfterWrite.count(address) != 0 && !bytes.empty()) {
            memory[address + bytes.size() - 1] = static_cast<std::uint8_t>(bytes.back() ^ 0xFF);
        }
        result.ok = true;
        result.bytesDone = static_cast<std::uint64_t>(bytes.size());
        return result;
    }
};

// FakeSink：可编排的伪确认接口。
class FakeSink final : public IConfirmationSink {
public:
    // log：共享有序日志。
    Log* log = nullptr;
    // uiAnswer：ConfirmUi 的回答。
    bool uiAnswer = true;
    // uiCalls / approvalCalls：两个接口各被调用的次数。
    int uiCalls = 0;
    int approvalCalls = 0;
    // onUi：ConfirmUi 进入时调用，模拟嵌套事件循环里发生的事（改会话、改代次、重入）。
    std::function<void()> onUi;
    // lastUi：最近一次 ConfirmUi 收到的请求。
    UiConfirmRequest lastUi;
    // approvalScript：按序给出的显式同意回答；用完后一律 Deny。
    std::vector<ApprovalAnswer> approvalScript;
    // approvals：收到的全部同意请求。
    std::vector<ApprovalRequest> approvals;

    bool ConfirmUi(const UiConfirmRequest& request) override {
        ++uiCalls;
        lastUi = request;
        if (log != nullptr) {
            log->push_back("UI");
        }
        if (onUi) {
            onUi();
        }
        return uiAnswer;
    }

    ApprovalAnswer ConfirmApproval(const ApprovalRequest& request) override {
        const std::size_t index = static_cast<std::size_t>(approvalCalls);
        ++approvalCalls;
        approvals.push_back(request);
        if (log != nullptr) {
            log->push_back("APPROVE:" + std::to_string(request.blockIndex));
        }
        return index < approvalScript.size() ? approvalScript[index] : ApprovalAnswer::Deny;
    }
};

// FakeAudit：记录全部审计的伪接口。
class FakeAudit final : public IAuditSink {
public:
    // log：共享有序日志。
    Log* log = nullptr;
    // records：按序收到的全部记录。
    std::vector<AuditRecord> records;
    // onRecord：每条记录到达时调用，测试用它在"确认被抑制"的那一刻改代次。
    std::function<void(const AuditRecord&)> onRecord;

    void Record(const AuditRecord& record) override {
        records.push_back(record);
        if (log != nullptr) {
            log->push_back(std::string("AUDIT:") + EventName(record.event));
        }
        if (onRecord) {
            onRecord(record);
        }
    }

    // Count：某种事件出现的次数。
    int Count(AuditEvent event) const {
        int total = 0;
        for (const AuditRecord& record : records) {
            if (record.event == event) {
                ++total;
            }
        }
        return total;
    }

    // Events：全部事件按序列出。
    std::vector<AuditEvent> Events() const {
        std::vector<AuditEvent> events;
        for (const AuditRecord& record : records) {
            events.push_back(record.event);
        }
        return events;
    }
};

// 一个完整的、可有效使用的会话：进程虚拟地址空间，pid 1234，标准驱动通道。
inline MemoryTargetSession MakeSession() {
    MemoryTargetSession session;
    session.scope = Scope::ProcessVirtual;
    session.pid = 1234;
    session.processCreateTime100ns = 5000;
    session.attachGeneration = 7;
    session.channel = Channel::StandardDriver;
    session.ddmaGeneration = 3;
    session.addressBits = 64;
    return session;
}

// Rig：把被测对象与全部伪依赖装在一起。成员顺序即构造顺序，txn 最后构造。
struct Rig {
    MemoryDiffOverlay overlay;
    MemoryTargetSession session;
    SessionRevisions revisions;
    Log log;
    FakeStore store;
    FakeSink sink;
    FakeAudit audit;
    MemoryWriteTransaction txn;

    // 构造：载入标准基线（全部读到），目标内存与基线一致，伪对象共用一份日志。
    explicit Rig(WriteMode mode = WriteMode::Immediate)
        : overlay()
        , session(MakeSession())
        , revisions()
        , log()
        , store()
        , sink()
        , audit()
        , txn(overlay, session, revisions, store, sink, audit, mode) {
        overlay.LoadBaseline("target-A", kBase, StandardBaseline(), Bytes(16, 1));
        const Bytes baseline = StandardBaseline();
        for (std::size_t index = 0; index < baseline.size(); ++index) {
            store.memory[kBase + index] = baseline[index];
        }
        store.log = &log;
        sink.log = &log;
        audit.log = &log;
    }

    // StageA / StageB / StageC / StageThree：暂存夹具块。
    void StageA() { txn.Stage(kA, Bytes{0x01}); }
    void StageB() { txn.Stage(kB, Bytes{0x02, 0x03}); }
    void StageC() { txn.Stage(kC, Bytes{0x04}); }
    void StageThree() {
        StageA();
        StageB();
        StageC();
    }
};

// 直接从伪目标内存里取字节（不经 Read，不计数）。
inline Bytes MemoryAt(const Rig& rig, std::uint64_t address, std::size_t length) {
    Bytes data;
    for (std::size_t offset = 0; offset < length; ++offset) {
        const auto found = rig.store.memory.find(address + offset);
        data.push_back(found == rig.store.memory.end() ? static_cast<std::uint8_t>(0xEE) : found->second);
    }
    return data;
}

// 手写的身份串：会话 scope=0 pid=1234 ct=5000 gen=7 ch=1 ddma=3 bits=64，窗口 base=0x1000 len=16。
inline const char* const kExpectedIdentity =
    "memwb-target/1|scope=0|pid=1234|ct=5000|gen=7|ch=1|ddma=3|bits=64|base=0x0000000000001000|len=16";

// 审计首尾断言：第一条是 CommitStarted，最后一条是 CommitFinished，各恰好一条。
// 入参：suite 断言容器；tag 场景名；rig 已经跑过一次 Commit 的夹具。
inline void ExpectAuditBracketed(KswordTests::Suite& suite, const std::wstring& tag, const Rig& rig) {
    const std::vector<AuditEvent> events = rig.audit.Events();
    suite.expect(!events.empty() && events.front() == AuditEvent::CommitStarted
        && events.back() == AuditEvent::CommitFinished,
        (tag + L": the audit opens with CommitStarted and closes with CommitFinished").c_str());
    suite.expect(rig.audit.Count(AuditEvent::CommitStarted) == 1 && rig.audit.Count(AuditEvent::CommitFinished) == 1,
        (tag + L": the audit has exactly one start and one finish").c_str());
}

// 三个测试组：分别定义在三个 .cpp 里。入参：共用的断言容器。
void RunStagingAndModeGroups(KswordTests::Suite& suite);
void RunCommitFlowGroups(KswordTests::Suite& suite);
void RunWriteOutcomeGroups(KswordTests::Suite& suite);

} // namespace MemwbTxnTests
