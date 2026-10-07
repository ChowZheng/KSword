#pragma once

// int3 补丁账本 —— "写入 / 还原单个字节(0xCC)"这一件事的记账层。
//
// ============================================================
// 这个类是什么，不是什么
// ============================================================
//
// 旧版的"断点"功能实际只做一件事：往目标内存某个地址写一个 0xCC 字节，并记下
// 被覆盖的原字节。它没有调试循环，不会捕获执行，所以也谈不上命中计数；界面上的
// "断点""命中""启用/禁用"措辞都是名不副实的。用户已决定把它降级为"int3 补丁"
// 工具：名实相符，只写入、只还原，一次一个字节。
//
// 因此本类刻意**不含**：命中计数、启用/禁用切换、任何"捕获执行"的语义。账本里的每
// 一条记录都只表达一个事实——"我在这个进程的这个地址，用 0xCC 覆盖过一个字节，
// 原来的字节是 X"。
//
// 零摩擦是用户的明确决定（设计文档第 5 节、第 10 节第 1 条）：写入与还原一键直达，
// 不复用内存编辑的二次核对链路，本类内部也**不含任何二次核对逻辑**。代价是这条
// 路径比其余写入操作少一道核对，属于用户已知并接受的风险。本类能做、也只做的保
// 护，是下面这些"不会写错对象"的结构性检查，它们不需要用户介入。
//
// ============================================================
// 旧缺陷与本类的对应关系
// ============================================================
//
// 旧实现有三个会写错对象的缺陷，本类逐个堵死：
//   1. 换进程时不还原：账本按目标（pid + 进程创建时间）记账，OnTargetGone 把
//      已消失目标的条目移出待还原列表并交给界面提示；
//   2. 删除时把旧进程的原字节写进新进程的同地址：Restore 要求 currentTarget 的
//      pid 与创建时间同时与条目一致，否则返回 TargetMismatch，且**一个字节都不写**。
//      仅比较 pid 不够——pid 会被复用，所以创建时间必须参与；
//   3. 原字节记成了 0xCC：若目标地址上本来就是 0xCC，"还原"会把 0xCC 写回 0xCC，
//      看似成功实则无效。Install 对此直接拒绝（AlreadyContainsPatchByte）。
//
// 另外 Restore 写回前会先读当前字节：若它已经不是 0xCC，说明这个地址被别处改过
// （目标自己的自修改、别的调试器、重新加载的模块），此时再写回原字节只会把别人
// 的改动抹掉，所以返回 Diverged 且不写，条目保留，由调用方决定是否 Discard。
//
// ============================================================
// 用法
// ============================================================
//
//   Int3PatchLedger ledger;
//   PatchTarget target{pid, createTime100ns, attachGeneration};
//   const InstallResult installed = ledger.Install(target, address, store, nowTick);
//   if (installed.id != 0) { /* 安装或验证失败的恢复条目，保存 id */ }
//   const RestoreResult restored = ledger.Restore(installed.id, target, store);
//
// 条目一律用 id 标识，绝不用表格行号：行号会随删除、排序而变，id 单调递增且永不
// 复用（包括条目被还原、丢弃、孤立之后）。
//
// 注入的 IPatchByteStore 是唯一的 I/O 出口，本类自己不做任何真实读写，所以可以用
// 内存数组加故障注入完整测试。线程模型：非线程安全，调用方在同一线程使用。
//
// C++20、Qt-free、Win32-free。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ksword::memwb {

// 补丁字节：x86 的 int3 指令编码。条目里每一条的 patchByte 都取这个值。
inline constexpr std::uint8_t kInt3PatchByte = 0xCC;

// IPatchByteStore：账本唯一的 I/O 出口，由调用方绑定到"当前已附加的目标"。
//
// 约定（两个函数都必须遵守，账本的写入与回读判断建立在它们之上）：
//   * ReadByte  返回 false 表示没有读到，此时 valueOut 的内容无意义，账本不会使用；
//   * WriteByte 返回 false 表示该字节**没有被改动**。返回 true 表示需要回读
//     核对的写入已发生或调用已成功；后置失败不能抹掉已落地事实，账本仍会验证。
class IPatchByteStore {
public:
    virtual ~IPatchByteStore() = default;

    // 读取 address 处的单个字节。成功返回 true 并写入 valueOut。
    virtual bool ReadByte(std::uint64_t address, std::uint8_t& valueOut) = 0;

    // 把 value 写到 address 处。需回读核对时返回 true；false 保证字节未被改动。
    virtual bool WriteByte(std::uint64_t address, std::uint8_t value) = 0;
};

// PatchTarget：一个"目标进程实例"的身份。
//
// pid 加创建时间才唯一标识一个进程实例（pid 会被系统复用）。attachGeneration 是
// 调用方的附加代次，只被记录进条目供界面分组与显示，不参与身份判断：同一个进程
// 重新附加后，补丁仍然物理地留在它身上，依旧可以还原。
struct PatchTarget {
    std::uint32_t pid = 0;                      // 进程 id
    std::uint64_t processCreateTime100ns = 0;   // 进程创建时间（100ns 单位，FILETIME 口径）
    std::uint64_t attachGeneration = 0;         // 调用方的附加代次，仅记录
};

// PatchEntry：账本里的一条记录——"在这个目标的这个地址写过 0xCC，原字节是 X"。
struct PatchEntry {
    std::uint64_t id = 0;                       // 条目 id，从 1 起单调递增，永不复用，0 表示无效
    std::uint32_t pid = 0;                      // 目标进程 id
    std::uint64_t processCreateTime100ns = 0;   // 目标进程创建时间
    std::uint64_t attachGeneration = 0;         // 安装时的附加代次，仅记录
    std::uint64_t address = 0;                  // 被补丁的目标虚拟地址
    std::uint8_t originalByte = 0;              // 被覆盖的原字节，还原时写回它
    std::uint8_t patchByte = kInt3PatchByte;    // 写入的补丁字节
    std::uint64_t installedAtTick = 0;          // 安装时刻，由调用方给定的时钟读数
};

// InstallStatus：一次 Install 的结果。每种拒绝都是独立状态，调用方要据此告诉用户。
enum class InstallStatus {
    None = 0,                   // 默认构造的结果，Install 从不返回它
    Installed,                  // 已写入并回读验证通过，InstallResult::id 有效
    Duplicate,                  // 账本里已有同目标同地址的条目；未做任何读写
    ReadFailed,                 // 读不到原字节；未写任何东西
    AlreadyContainsPatchByte,   // 该地址上原本就是 0xCC；未写任何东西
    WriteFailed,                // WriteByte 返回失败；按约定目标字节未被改动，无需回滚
    VerifyFailed,               // 回读未确认 0xCC；不盲写回原字节，未证实原字节仍在时保留条目
};

// InstallResult：Installed 的 id 有效；VerifyFailed 未证实原字节仍在时也带恢复条目 id。
// rollback 字段保留接口兼容；Install 不再对已改变或未知的字节自动回滚，两者恒为 false。
struct InstallResult {
    InstallStatus status = InstallStatus::None;     // 结果状态
    std::uint64_t id = 0;                           // 安装或验证失败的恢复条目 id；0 表示未留条目
    bool rollbackAttempted = false;                 // 兼容字段；当前 Install 不自动回滚，恒为 false
    bool rollbackWriteOk = false;                   // 兼容字段；没有回滚写入，恒为 false
};

// RestoreStatus：一次 Restore 的结果。
enum class RestoreStatus {
    None = 0,           // 默认构造的结果，Restore 从不返回它
    Restored,           // 已写回原字节并回读验证通过，条目已从账本删除
    NotFound,           // 账本里没有这个 id（从未存在，或已被还原 / 丢弃）；未做任何读写
    Orphaned,           // 该条目所属进程已不存在（OnTargetGone 移出），无从还原；未做任何读写
    TargetMismatch,     // currentTarget 与条目的 pid 或创建时间不一致；未做任何读写
    ReadFailed,         // 读不到当前字节；未写任何东西，条目保留
    Diverged,           // 当前字节已不是 0xCC（被别处改过）；未写任何东西，条目保留
    WriteFailed,        // 写回失败；条目保留，可重试
    VerifyFailed,       // 写回后回读不等于原字节（或回读失败）；条目保留，可重试
};

// RestoreResult：Restore 的返回值。
struct RestoreResult {
    RestoreStatus status = RestoreStatus::None;     // 结果状态
    bool hasObservedByte = false;                   // observedByte 是否有效
    std::uint8_t observedByte = 0;                  // Diverged 时为当前读到的字节，VerifyFailed 时为回读到的字节
};

// PatchRestoreOutcome：RestoreAllForTarget 里一条条目的处理结果。
//
// 把 id 与 address 一并带出，是因为成功的条目已经从账本删除，调用方事后查不到它了。
struct PatchRestoreOutcome {
    std::uint64_t id = 0;               // 被处理的条目 id
    std::uint64_t address = 0;          // 该条目的补丁地址
    RestoreResult result;               // 这一条的还原结果
};

// Int3PatchLedger：账本本体。
class Int3PatchLedger final {
public:
    // Install：在 target 的 address 处写入 0xCC 并记账。
    //
    // 调用方法：store 必须是已绑定到 target 的存取器；nowTick 由调用方给定（本类
    // 不读时钟，保持可测）。传出 InstallResult，成功时 id 为新条目 id。
    //
    // 检查顺序固定为：同目标同地址已有条目 -> Duplicate（先于任何读写，因为已补丁
    // 的地址读出来必然是 0xCC，若先读会被误判成 AlreadyContainsPatchByte）；
    // 读原字节 -> ReadFailed；原字节已是 0xCC -> AlreadyContainsPatchByte；
    // 写入 -> WriteFailed；回读验证 -> VerifyFailed。回读为原字节时不重复写回、不记账；
    // 回读为其它值或读取失败时不盲写，保留带有效 id 的恢复条目，仍返回 VerifyFailed，
    // 不冒充 Installed。之后显式 Restore 会再次读取当前字节：其它值返回 Diverged 且
    // 不覆盖第三方改动；只有读到 0xCC 才写回已记录的原字节。
    InstallResult Install(
        const PatchTarget& target,
        std::uint64_t address,
        IPatchByteStore& store,
        std::uint64_t nowTick);

    // Restore：把条目 id 的原字节写回，成功则删除条目。
    //
    // 调用方法：currentTarget 是调用方此刻认为的目标身份，store 绑定到它。
    // 只有 currentTarget 的 pid 与创建时间都与条目一致才会进行任何 I/O。
    // 检查顺序：id 不存在 -> NotFound；id 属于已孤立条目 -> Orphaned；目标不符 ->
    // TargetMismatch；读当前字节 -> ReadFailed；当前不是 0xCC -> Diverged；写回 ->
    // WriteFailed；回读验证 -> VerifyFailed。除 Restored 外条目一律保留。
    RestoreResult Restore(
        std::uint64_t id,
        const PatchTarget& currentTarget,
        IPatchByteStore& store);

    // RestoreAllForTarget：逐条还原属于 currentTarget 的全部条目。
    //
    // 返回逐条结果，按安装顺序（id 升序）排列。某一条失败不会中止后续条目——退出
    // 前一键还原的场景下，一条坏了不能让其余几条也留在目标里。属于其它目标的条目
    // 不会被碰，也不会出现在返回里。没有匹配条目时返回空列表。
    std::vector<PatchRestoreOutcome> RestoreAllForTarget(
        const PatchTarget& currentTarget,
        IPatchByteStore& store);

    // OnTargetGone：通知账本"这个进程实例已经不存在了"。
    //
    // 调用方法：进程退出被检测到、或调用方放弃该进程时调用，传入它的 pid 与创建
    // 时间（两者必须同时一致，pid 相同而创建时间不同的是另一个进程，不受影响）。
    // 匹配的条目被标记为 Orphaned：从待还原列表移出（进程不存在，无从还原，更不能
    // 把它的原字节写进别的进程），转入孤立列表。返回被孤立的条目供界面提示，按
    // id 升序。孤立条目仍占着自己的 id，不会被复用。
    std::vector<PatchEntry> OnTargetGone(
        std::uint32_t pid,
        std::uint64_t processCreateTime100ns);

    // Discard：把待还原条目直接从账本删掉，不做任何 I/O。
    //
    // 用于 Restore 返回 Diverged 之后，调用方决定放弃这条记录（目标字节已被别处
    // 改过，保留记录只会反复提示）。成功返回 true；id 不在待还原列表里返回 false。
    bool Discard(std::uint64_t id);

    // ClearOrphaned：界面提示过孤立条目之后清空孤立列表。不影响待还原条目与 id 计数。
    void ClearOrphaned();

    // HasUnrestored：是否存在待还原的补丁。供程序退出 / 切换目标前提示用。
    // 已孤立的条目不计入——它们无从还原，提示"请先还原"没有意义。
    bool HasUnrestored() const;

    // Entries：待还原条目，按安装顺序（id 升序）。返回的引用在下一次修改账本前有效。
    const std::vector<PatchEntry>& Entries() const;

    // OrphanedEntries：已孤立条目，按孤立发生的先后顺序追加。
    const std::vector<PatchEntry>& OrphanedEntries() const;

    // FindById：按 id 查待还原条目。找不到返回 std::nullopt；返回的是拷贝，不会悬空。
    std::optional<PatchEntry> FindById(std::uint64_t id) const;

private:
    // 在待还原列表里按 id 找下标，找不到返回 entries_.size()。
    std::size_t IndexOfId(std::uint64_t id) const;

    std::vector<PatchEntry> entries_;       // 待还原条目，按 id 升序（只在尾部追加）
    std::vector<PatchEntry> orphaned_;      // 已孤立条目
    std::uint64_t nextId_ = 1;              // 下一个要发放的 id；只增不减，所以永不复用
};

} // namespace ksword::memwb
