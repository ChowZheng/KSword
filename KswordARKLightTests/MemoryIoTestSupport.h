#pragma once

// WP-C 内存 I/O 端口相关套件（page reader / io bytestore / kernel mutation /
// patch store 四个套件共用）的脚本化假端口与小工具。
//
// FakeMemoryIoPort 把每次 Read / Write 调用按发生顺序记进 calls / writeCalls，
// 并按序消费 script / writeScript 里预先编排好的返回值——这正是测试要断言
// "端口被调用的 (address,length) 序列与手算一致"（钉死 MemoryPageReader 的
// 批量失败后有限单页核验，以及 MemoryIoByteStore 的切块/聚合规则）的基础：被测代码
// 每发起一次调用，假端口就记一笔，脚本用完还没结束则说明测试脚本没编排够，
// 报出一个能立刻定位到"不是被测对象的缺陷而是脚本缺口"的失败文案，不会悄悄
// 返回一个看起来正常的空结果掩盖过去。
//
// FakeKernelMutationPort 是 IKernelMutationPort 的同类脚手架：Prepare /
// DryRunCommit / ForceCommit / Rollback / ReadBack 五个方法各自按调用顺序消费
// 自己的脚本，并把全局调用序号记进每条调用记录（order 字段），供测试断言
// "谁先于谁被调用"——这比拼一份文本日志更不容易在重构时意外改动格式。
//
// 本阶段同步的时间/并发模型：被测对象都是同步调用，假端口也是同步返回，不模拟
// 任何真实延迟；"延迟"留给会真正并发的后续适配层（WorkbenchIoPorts）自己处理。

#include "../shared/evidence/memory_workbench/MemoryIoPort.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MemwbIoTests {

using ksword::memwb::IKernelMutationPort;
using ksword::memwb::IMemoryIoPort;
using ksword::memwb::IoLimits;
using ksword::memwb::IoReadResult;
using ksword::memwb::IoReadStatus;
using ksword::memwb::IoWriteResult;
using ksword::memwb::MemoryTargetSession;
using ksword::memwb::MutationPrepareResult;
using ksword::memwb::MutationStepResult;

// 字节序列的简写，与其余 Memwb 测试文件保持一致的拼写习惯。
using Bytes = std::vector<std::uint8_t>;

// ReadCall：FakeMemoryIoPort 记录的一次 Read 调用。
struct ReadCall {
    std::uint64_t address = 0; // 本次 Read 请求的起始地址
    std::uint64_t length = 0;  // 本次 Read 请求的长度
};

// WriteCall：FakeMemoryIoPort 记录的一次 Write 调用。page reader 套件不会
// 触发它；IoByteStore / patch store 套件用它断言切片地址/长度/同意标志的序列。
struct WriteCall {
    std::uint64_t address = 0;  // 本次 Write 请求的起始地址
    std::uint64_t byteCount = 0; // 本次 Write 请求的字节数
    bool approved = false;       // 调用方是否带着显式同意标志
};

// FakeMemoryIoPort：脚本化假端口。limits 固定返回给 Limits()；script 按调用
// 顺序被 Read() 逐个消费；calls 记录真实发生过的 (address,length) 调用序列。
class FakeMemoryIoPort final : public IMemoryIoPort {
public:
    // limits：Limits() 的固定返回值，测试据此配置 maxReadBytes 的切块边界。
    IoLimits limits;
    // script：按调用顺序消费的 Read 返回结果；用完之后的调用会得到下面的
    // "脚本用尽"失败结果，而不是悄悄复用最后一条或默认成功。
    std::vector<IoReadResult> script;
    // calls：按发生顺序记录的全部 Read 调用，断言地址/长度序列、调用次数用它。
    std::vector<ReadCall> calls;
    // writeCalls：按发生顺序记录的全部 Write 调用。
    std::vector<WriteCall> writeCalls;
    // writeScript：按调用顺序消费的 Write 返回结果；为空（page reader 套件的
    // 默认状态）时 Write 保持旧行为——返回一个明确标注"未编排"的失败结果。
    std::vector<IoWriteResult> writeScript;

    // cancelToArmAfterCall / cancelArmIndex：模拟"另一个线程在两次 Read 之间
    // 把取消标志置位"。ReadPages 本身是同步单线程调用，测试没有真正的并发可
    // 以插进去，所以用这对钩子在假端口里产生同样的可观测效果：当 calls.size()
    // 达到 cancelArmIndex（即刚完成第 cancelArmIndex 次调用）时，把
    // *cancelToArmAfterCall 置为 true，下一次 ReadPages 发起 Read 之前的取消
    // 检查就会命中。cancelToArmAfterCall 为空（默认）时什么都不做。
    std::atomic<bool>* cancelToArmAfterCall = nullptr;
    std::size_t cancelArmIndex = 0;

    // Limits：直接返回固定配置，不看 session 的内容——假端口不关心会话字段，
    // 只关心测试想要的切块边界。
    IoLimits Limits(const MemoryTargetSession& /*session*/) const override {
        return limits;
    }

    // Read：记一笔调用，再按调用序号从 script 里取对应的编排结果；之后检查
    // 是否到了该置位取消标志的那一次调用。
    IoReadResult Read(
        const MemoryTargetSession& /*session*/,
        std::uint64_t address,
        std::uint64_t length) override {
        ReadCall call;
        call.address = address;
        call.length = length;
        calls.push_back(call);
        if (cancelToArmAfterCall != nullptr && calls.size() == cancelArmIndex) {
            cancelToArmAfterCall->store(true);
        }
        const std::size_t index = calls.size() - 1;
        if (index < script.size()) {
            return script[index];
        }
        // 脚本用尽：这是测试配置缺口，不是被测对象的缺陷，用显式失败文案暴露它。
        IoReadResult overrun;
        overrun.status = IoReadStatus::Failed;
        overrun.failure = "FakeMemoryIoPort::Read script exhausted";
        return overrun;
    }

    // Write：记一笔调用，再按调用序号从 writeScript 里取对应的编排结果；
    // writeScript 为空时（page reader 套件从不配置它）保持旧行为，返回一个
    // 明确标注"未编排"的失败结果，一旦误触发也能立刻从失败文案看出是哪里
    // 调错了接口。
    IoWriteResult Write(
        const MemoryTargetSession& /*session*/,
        std::uint64_t address,
        const Bytes& bytes,
        bool approved) override {
        WriteCall call;
        call.address = address;
        call.byteCount = static_cast<std::uint64_t>(bytes.size());
        call.approved = approved;
        writeCalls.push_back(call);
        const std::size_t index = writeCalls.size() - 1;
        if (index < writeScript.size()) {
            return writeScript[index];
        }
        IoWriteResult result;
        result.failure = "FakeMemoryIoPort::Write script exhausted (or not scripted by this suite)";
        return result;
    }
};

// MakeSession：page reader 套件不读取会话的任何字段（假端口的 Limits/Read 都
// 忽略 session 参数），用一个自洽的进程虚拟地址会话占位，避免每个测试都重复
// 手写同样的六个字段。
inline MemoryTargetSession MakeSession() {
    MemoryTargetSession session;
    session.pid = 4321;
    return session;
}

// MakePattern：生成长度为 length 的确定性字节序列，第 i 个字节 = (startValue+i)
// 对 256 取模。测试拿它既当"假端口返回的 data"，也当"核对页内字节"的期望值，
// 两边用同一个函数算，不必分别手抄一份容易对不齐的字面量数组。
inline Bytes MakePattern(std::uint8_t startValue, std::size_t length) {
    Bytes data(length);
    for (std::size_t index = 0; index < length; ++index) {
        data[index] = static_cast<std::uint8_t>(startValue + static_cast<std::uint8_t>(index));
    }
    return data;
}

// MakeOk：构造一条 Ok 结果，data 为 bytes。
inline IoReadResult MakeOk(Bytes bytes) {
    IoReadResult result;
    result.status = IoReadStatus::Ok;
    result.data = std::move(bytes);
    return result;
}

// MakePartial：构造一条 Partial 结果，data 为 prefix（必须比请求短，调用方负责）。
inline IoReadResult MakePartial(Bytes prefix) {
    IoReadResult result;
    result.status = IoReadStatus::Partial;
    result.data = std::move(prefix);
    return result;
}

// MakeUnreadable：构造一条 Unreadable 结果。
inline IoReadResult MakeUnreadable(std::string text = "target byte is unreadable") {
    IoReadResult result;
    result.status = IoReadStatus::Unreadable;
    result.failure = std::move(text);
    return result;
}

// MakeFailed：构造一条 Failed 结果。
inline IoReadResult MakeFailed(std::string text = "channel failed") {
    IoReadResult result;
    result.status = IoReadStatus::Failed;
    result.failure = std::move(text);
    return result;
}

// MakeWriteOk：构造一条成功的 IoWriteResult，bytesDone 为 bytes。
inline IoWriteResult MakeWriteOk(std::uint64_t bytesDone) {
    IoWriteResult result;
    result.ok = true;
    result.bytesDone = bytesDone;
    return result;
}

// MakeWritePartial：构造一条"ok 为真但 partial 也为真"的结果——按接口契约，
// 调用方必须把它当失败处理，这个构造函数专门用来测试那条规则有没有被遵守。
inline IoWriteResult MakeWritePartial(std::uint64_t bytesDone, std::string text = "short write") {
    IoWriteResult result;
    result.ok = true;
    result.partial = true;
    result.bytesDone = bytesDone;
    result.failure = std::move(text);
    return result;
}

// MakeWriteNeedsApproval：构造一条要求显式同意的结果，按约定未写入任何字节。
inline IoWriteResult MakeWriteNeedsApproval(std::string text = "force flag required") {
    IoWriteResult result;
    result.needsApproval = true;
    result.failure = std::move(text);
    return result;
}

// MakeWriteFailed：构造一条普通失败结果。
inline IoWriteResult MakeWriteFailed(std::string text = "write failed") {
    IoWriteResult result;
    result.failure = std::move(text);
    return result;
}

// ------------------------------------------------------------
// FakeKernelMutationPort：IKernelMutationPort 的脚本化假端口。
//
// Prepare / DryRunCommit / ForceCommit / Rollback / ReadBack 各自维护一份
// "脚本 + 调用记录"：脚本按调用顺序消费，用尽时返回显式标注"脚本用尽"的失败
// 结果（与 FakeMemoryIoPort 同一套设计理由：这是测试脚本的缺口，不是被测
// 对象的缺陷）。每条调用记录都带一个全局单调递增的 order 字段（取自同一个
// callSequence_ 计数器，五个方法共用），测试据此断言"谁先于谁被调用"，
// 不必依赖另外拼一份容易在重构时悄悄走形的文本日志。
// ------------------------------------------------------------

// PrepareCallRecord：一次 Prepare 调用的完整参数快照。
struct PrepareCallRecord {
    int order = 0;              // 全局调用序号（从 1 起）
    std::uint64_t address = 0;  // 这一片的起始地址
    Bytes after;                // 想写入的新字节
    Bytes expectedBefore;       // 调用方记忆中的写前字节
};

// TransactionCallRecord：DryRunCommit / ForceCommit / Rollback 共用的调用记录。
struct TransactionCallRecord {
    int order = 0;                     // 全局调用序号
    std::uint64_t transactionId = 0;   // 被操作的事务号
};

// ReadBackCallRecord：ReadBack 的调用记录。
struct ReadBackCallRecord {
    int order = 0;              // 全局调用序号
    std::uint64_t address = 0;  // 回读起始地址
    std::uint64_t length = 0;   // 回读长度
};

class FakeKernelMutationPort final : public IKernelMutationPort {
public:
    // 五份脚本，各自按自己方法的调用顺序消费。
    std::vector<MutationPrepareResult> prepareScript;
    std::vector<MutationStepResult> dryRunScript;
    std::vector<MutationStepResult> forceScript;
    std::vector<MutationStepResult> rollbackScript;
    std::vector<IoReadResult> readBackScript;

    // 五份调用记录，下标即该方法自己的第几次调用（0-based）。
    std::vector<PrepareCallRecord> prepareCalls;
    std::vector<TransactionCallRecord> dryRunCalls;
    std::vector<TransactionCallRecord> forceCalls;
    std::vector<TransactionCallRecord> rollbackCalls;
    std::vector<ReadBackCallRecord> readBackCalls;

    // callSequence_：五个方法共用的全局调用计数器，每次任意方法被调用都加一，
    // 新值写进那次调用记录的 order 字段。
    int callSequence_ = 0;

    MutationPrepareResult Prepare(
        std::uint64_t address, const Bytes& after, const Bytes& expectedBefore) override {
        PrepareCallRecord record;
        record.order = ++callSequence_;
        record.address = address;
        record.after = after;
        record.expectedBefore = expectedBefore;
        prepareCalls.push_back(record);
        const std::size_t index = prepareCalls.size() - 1;
        if (index < prepareScript.size()) {
            return prepareScript[index];
        }
        MutationPrepareResult overrun;
        overrun.failure = "FakeKernelMutationPort::Prepare script exhausted";
        return overrun;
    }

    MutationStepResult DryRunCommit(std::uint64_t transactionId) override {
        TransactionCallRecord record;
        record.order = ++callSequence_;
        record.transactionId = transactionId;
        dryRunCalls.push_back(record);
        const std::size_t index = dryRunCalls.size() - 1;
        if (index < dryRunScript.size()) {
            return dryRunScript[index];
        }
        MutationStepResult overrun;
        overrun.failure = "FakeKernelMutationPort::DryRunCommit script exhausted";
        return overrun;
    }

    MutationStepResult ForceCommit(std::uint64_t transactionId) override {
        TransactionCallRecord record;
        record.order = ++callSequence_;
        record.transactionId = transactionId;
        forceCalls.push_back(record);
        const std::size_t index = forceCalls.size() - 1;
        if (index < forceScript.size()) {
            return forceScript[index];
        }
        MutationStepResult overrun;
        overrun.failure = "FakeKernelMutationPort::ForceCommit script exhausted";
        return overrun;
    }

    MutationStepResult Rollback(std::uint64_t transactionId) override {
        TransactionCallRecord record;
        record.order = ++callSequence_;
        record.transactionId = transactionId;
        rollbackCalls.push_back(record);
        const std::size_t index = rollbackCalls.size() - 1;
        if (index < rollbackScript.size()) {
            return rollbackScript[index];
        }
        MutationStepResult overrun;
        overrun.failure = "FakeKernelMutationPort::Rollback script exhausted";
        return overrun;
    }

    IoReadResult ReadBack(std::uint64_t address, std::uint64_t length) override {
        ReadBackCallRecord record;
        record.order = ++callSequence_;
        record.address = address;
        record.length = length;
        readBackCalls.push_back(record);
        const std::size_t index = readBackCalls.size() - 1;
        if (index < readBackScript.size()) {
            return readBackScript[index];
        }
        IoReadResult overrun;
        overrun.status = IoReadStatus::Failed;
        overrun.failure = "FakeKernelMutationPort::ReadBack script exhausted";
        return overrun;
    }
};

// MakePrepared：构造一条 Prepare 成功结果；beforeBytes 通常应等于调用方传入
// 的 expectedBefore（测试逐例决定是否故意传一份不一致的，用来命中"before
// 不一致"失败路径）。
inline MutationPrepareResult MakePrepared(std::uint64_t transactionId, Bytes beforeBytes) {
    MutationPrepareResult result;
    result.ok = true;
    result.transactionId = transactionId;
    result.beforeBytes = std::move(beforeBytes);
    return result;
}

// MakePrepareFailed：构造一条 Prepare 失败结果（端口自己判定拒绝，例如状态不对）。
inline MutationPrepareResult MakePrepareFailed(std::string text = "prepare rejected") {
    MutationPrepareResult result;
    result.failure = std::move(text);
    return result;
}

// MakeStepOk：构造一条成功的 MutationStepResult（DryRunCommit/ForceCommit/Rollback 共用）。
inline MutationStepResult MakeStepOk() {
    MutationStepResult result;
    result.ok = true;
    return result;
}

// MakeStepFailed：构造一条失败的 MutationStepResult。
inline MutationStepResult MakeStepFailed(std::string text = "step failed") {
    MutationStepResult result;
    result.failure = std::move(text);
    return result;
}

} // namespace MemwbIoTests
