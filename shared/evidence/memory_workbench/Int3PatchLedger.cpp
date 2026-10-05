#include "Int3PatchLedger.h"

// int3 补丁账本的实现。设计动机与各返回状态的含义见 Int3PatchLedger.h 顶部注释。
//
// 本文件的结构：先是两个匿名命名空间里的小判断函数，然后依次是 Install、Restore、
// RestoreAllForTarget、OnTargetGone，最后是 Discard 与几个只读查询。
// 全程不做任何真实 I/O，所有读写都经由注入的 IPatchByteStore。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ksword::memwb {

namespace {

// IsSameProcess：判断一条账本记录与给定的目标是不是同一个进程实例。
//
// 调用方法：传入账本条目与调用方给出的目标身份。
// 返回：pid 与创建时间都相等才为 true。
// 只比 pid 是不够的：pid 会被系统复用，新进程可能拿到旧进程的 pid，
// 此时创建时间不同——这正是旧缺陷里"把旧进程原字节写进新进程"的成因。
// attachGeneration 不参与比较，它只是记录。
bool IsSameProcess(const PatchEntry& entry, const PatchTarget& target) {
    // pid 是否一致。
    const bool samePid = (entry.pid == target.pid);
    // 创建时间是否一致。
    const bool sameCreateTime =
        (entry.processCreateTime100ns == target.processCreateTime100ns);
    return samePid && sameCreateTime;
}

// IsSameProcessInstance：判断条目是否属于 (pid, 创建时间) 这个进程实例。
//
// 调用方法：OnTargetGone 用，参数是散开的两个值而不是 PatchTarget。
// 返回：两个值都相等才为 true。
bool IsSameProcessInstance(
    const PatchEntry& entry,
    std::uint32_t pid,
    std::uint64_t processCreateTime100ns) {
    // pid 是否一致。
    const bool samePid = (entry.pid == pid);
    // 创建时间是否一致。
    const bool sameCreateTime = (entry.processCreateTime100ns == processCreateTime100ns);
    return samePid && sameCreateTime;
}

} // namespace

// Install：写入 0xCC 并记账。检查顺序是协议的一部分，不要调换，原因见下面各段。
InstallResult Int3PatchLedger::Install(
    const PatchTarget& target,
    std::uint64_t address,
    IPatchByteStore& store,
    std::uint64_t nowTick) {
    // 默认就是 None / id 0，所有拒绝路径直接返回它加上具体状态，不会带出半截数据。
    InstallResult result;

    // 第一步：同目标同地址是否已有条目。必须放在读取之前，而且不做任何 I/O：
    // 已经补丁过的地址读出来必然是 0xCC，若先读再判，会把"重复安装"误报成
    // "原字节已是 0xCC"，两者给用户的指引完全不同。
    for (const PatchEntry& existing : entries_) {
        // 地址是否相同。
        const bool sameAddress = (existing.address == address);
        // 目标是否相同（pid 与创建时间同时一致）。
        const bool sameTarget = IsSameProcess(existing, target);
        if (sameAddress && sameTarget) {
            result.status = InstallStatus::Duplicate;
            return result;
        }
    }

    // 第二步：读取原字节。originalByte 先置 0，读取失败时它的内容无意义，下面
    // 不会再使用；账本不依赖 store 在失败时给 valueOut 写什么。
    std::uint8_t originalByte = 0;
    const bool readOk = store.ReadByte(address, originalByte);
    if (!readOk) {
        result.status = InstallStatus::ReadFailed;
        return result;
    }

    // 第三步：原字节若已经是 0xCC，拒绝。走到这里说明账本里没有同目标同地址的
    // 条目（第一步已排除），所以这个 0xCC 不是我们写的——可能是目标自带的断点、
    // 别的调试器留下的。若照旧记账，"原字节"就是 0xCC，之后的还原等于把 0xCC
    // 写回 0xCC，看似成功实则无效，还把别人的 int3 当成了我们的。
    if (originalByte == kInt3PatchByte) {
        result.status = InstallStatus::AlreadyContainsPatchByte;
        return result;
    }

    // 第四步：写入补丁字节。按接口约定，返回 false 表示字节未被改动，所以这里
    // 不需要回滚，直接报告失败。
    const bool writeOk = store.WriteByte(address, kInt3PatchByte);
    if (!writeOk) {
        result.status = InstallStatus::WriteFailed;
        return result;
    }

    // 第五步：回读验证。WriteByte 返回 true 只说明调用成功，不说明字节真的变了
    // （只读映射、写时复制、被其它线程立刻改回都会造成这种情况）。回读失败和回读
    // 值不对一律视为验证失败：此时目标字节处于未知状态，尽力把原字节写回去，
    // 避免留下一个账本里没有记录的 0xCC。回滚写的结果如实带回给调用方。
    std::uint8_t readBackByte = 0;
    const bool readBackOk = store.ReadByte(address, readBackByte);
    const bool verified = readBackOk && (readBackByte == kInt3PatchByte);
    if (!verified) {
        result.status = InstallStatus::VerifyFailed;
        result.rollbackAttempted = true;
        result.rollbackWriteOk = store.WriteByte(address, originalByte);
        return result;
    }

    // 第六步：验证通过，才真正发放 id 并记账。id 放到最后才消耗，所以任何拒绝路径
    // 都不会在 id 序列里留下空洞；nextId_ 只增不减，所以已用过的 id 永不复用。
    PatchEntry entry;
    entry.id = nextId_;
    entry.pid = target.pid;
    entry.processCreateTime100ns = target.processCreateTime100ns;
    entry.attachGeneration = target.attachGeneration;
    entry.address = address;
    entry.originalByte = originalByte;
    entry.patchByte = kInt3PatchByte;
    entry.installedAtTick = nowTick;
    entries_.push_back(entry);
    ++nextId_;

    result.status = InstallStatus::Installed;
    result.id = entry.id;
    return result;
}

// Restore：把原字节写回。任何一道检查不过都不写，条目保留（Restored 除外）。
RestoreResult Int3PatchLedger::Restore(
    std::uint64_t id,
    const PatchTarget& currentTarget,
    IPatchByteStore& store) {
    // 默认 None，所有路径显式设置状态；observedByte 只在 hasObservedByte 为真时有效。
    RestoreResult result;

    // 第一步：找条目。找不到时再区分"已被孤立"和"从未存在 / 已处理"，
    // 让界面能说出准确的原因，而不是一律"没有这一条"。
    const std::size_t index = IndexOfId(id);
    if (index == entries_.size()) {
        // 是否在孤立列表里。
        const bool isOrphaned = std::any_of(
            orphaned_.begin(),
            orphaned_.end(),
            [id](const PatchEntry& orphan) { return orphan.id == id; });
        result.status = isOrphaned ? RestoreStatus::Orphaned : RestoreStatus::NotFound;
        return result;
    }

    // 拷一份条目到局部变量：后面要 erase，引用会悬空。
    const PatchEntry entry = entries_[index];

    // 第二步：目标核对。这是堵"把 A 进程的原字节写进 B 进程"的那道门，
    // 必须在任何读写之前，连读都不读——读 B 进程的内存也没有意义。
    if (!IsSameProcess(entry, currentTarget)) {
        result.status = RestoreStatus::TargetMismatch;
        return result;
    }

    // 第三步：读当前字节。读不到就不能判断目标上现在是什么，不写，条目保留。
    std::uint8_t currentByte = 0;
    const bool readOk = store.ReadByte(entry.address, currentByte);
    if (!readOk) {
        result.status = RestoreStatus::ReadFailed;
        return result;
    }

    // 第四步：当前字节必须仍是我们写的补丁字节。若不是，说明这个地址被别处改过，
    // 此时写回原字节会抹掉别人的改动。不写，条目保留，把读到的字节带回去，
    // 由调用方决定是否 Discard。
    if (currentByte != entry.patchByte) {
        result.status = RestoreStatus::Diverged;
        result.hasObservedByte = true;
        result.observedByte = currentByte;
        return result;
    }

    // 第五步：写回原字节。失败时按接口约定目标未被改动，条目保留以便重试。
    const bool writeOk = store.WriteByte(entry.address, entry.originalByte);
    if (!writeOk) {
        result.status = RestoreStatus::WriteFailed;
        return result;
    }

    // 第六步：回读验证。读不到、或读到的不是原字节，都不能宣布还原成功，
    // 条目保留。回读读到了值就带回去给界面显示。
    std::uint8_t readBackByte = 0;
    const bool readBackOk = store.ReadByte(entry.address, readBackByte);
    if (!readBackOk) {
        result.status = RestoreStatus::VerifyFailed;
        return result;
    }
    if (readBackByte != entry.originalByte) {
        result.status = RestoreStatus::VerifyFailed;
        result.hasObservedByte = true;
        result.observedByte = readBackByte;
        return result;
    }

    // 第七步：真正还原成功，才把条目从账本删除。
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    result.status = RestoreStatus::Restored;
    return result;
}

// RestoreAllForTarget：逐条还原，一条失败不中止后续。
std::vector<PatchRestoreOutcome> Int3PatchLedger::RestoreAllForTarget(
    const PatchTarget& currentTarget,
    IPatchByteStore& store) {
    // 先把属于该目标的条目 id 与地址快照下来。不能一边遍历 entries_ 一边调用
    // Restore：Restore 成功会删条目，遍历会错位。
    std::vector<PatchRestoreOutcome> outcomes;
    for (const PatchEntry& entry : entries_) {
        if (IsSameProcess(entry, currentTarget)) {
            PatchRestoreOutcome outcome;
            outcome.id = entry.id;
            outcome.address = entry.address;
            outcomes.push_back(outcome);
        }
    }

    // 再逐条走完整的 Restore（含 Diverged、回读验证等全部检查）。
    // 每一条都无条件处理，不因前面的失败而 break。
    for (PatchRestoreOutcome& outcome : outcomes) {
        outcome.result = Restore(outcome.id, currentTarget, store);
    }
    return outcomes;
}

// OnTargetGone：把已消失进程实例的条目移出待还原列表。
std::vector<PatchEntry> Int3PatchLedger::OnTargetGone(
    std::uint32_t pid,
    std::uint64_t processCreateTime100ns) {
    // 把条目分成两堆：仍待还原的，和这次被孤立的。保持各自原有的顺序。
    std::vector<PatchEntry> stillPending;
    std::vector<PatchEntry> newlyOrphaned;
    for (const PatchEntry& entry : entries_) {
        if (IsSameProcessInstance(entry, pid, processCreateTime100ns)) {
            newlyOrphaned.push_back(entry);
        } else {
            stillPending.push_back(entry);
        }
    }

    // 用剩下的覆盖待还原列表，被孤立的追加进孤立列表。这些条目不会再被 Restore
    // 触及，所以不可能把它们的原字节写进别的进程。
    entries_ = std::move(stillPending);
    orphaned_.insert(orphaned_.end(), newlyOrphaned.begin(), newlyOrphaned.end());
    return newlyOrphaned;
}

// Discard：直接删除待还原条目，不做 I/O。
bool Int3PatchLedger::Discard(std::uint64_t id) {
    // 找不到就如实返回 false，不假装删除成功。
    const std::size_t index = IndexOfId(id);
    if (index == entries_.size()) {
        return false;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// ClearOrphaned：清空孤立列表。不动待还原列表，也不动 nextId_。
void Int3PatchLedger::ClearOrphaned() {
    orphaned_.clear();
}

// HasUnrestored：只看待还原列表，孤立条目无从还原所以不算。
bool Int3PatchLedger::HasUnrestored() const {
    return !entries_.empty();
}

// Entries：待还原条目的只读视图。
const std::vector<PatchEntry>& Int3PatchLedger::Entries() const {
    return entries_;
}

// OrphanedEntries：孤立条目的只读视图。
const std::vector<PatchEntry>& Int3PatchLedger::OrphanedEntries() const {
    return orphaned_;
}

// FindById：按 id 查待还原条目，返回拷贝。
std::optional<PatchEntry> Int3PatchLedger::FindById(std::uint64_t id) const {
    const std::size_t index = IndexOfId(id);
    if (index == entries_.size()) {
        return std::nullopt;
    }
    return entries_[index];
}

// IndexOfId：线性查找。条目数量是人手点出来的量级，不需要更复杂的结构。
std::size_t Int3PatchLedger::IndexOfId(std::uint64_t id) const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].id == id) {
            return index;
        }
    }
    return entries_.size();
}

} // namespace ksword::memwb
