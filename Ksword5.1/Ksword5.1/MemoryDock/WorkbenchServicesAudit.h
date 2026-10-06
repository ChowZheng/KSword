#pragma once

// ============================================================
// WorkbenchServicesAudit.h
// 作用：
// - 内存工作台 Phase 3 WP-K1 生产审计接收器（写项目日志）里的纯函数部分：
//   审计记录 -> 日志级别、审计记录 -> 日志文本、同一次提交链路复用同一个日志事件的
//   登记表（AuditChainRegistry）。
// - 为什么拆出来：生产接收器（MemoryDock.WorkbenchServices.Audit.cpp）要包含
//   Framework.h 才能写日志，没法离线测试；而"什么级别、写什么字段、绝不写字节内容、
//   链路登记表有界"这些判断才是最容易写错的部分，必须能用手造的 AuditRecord 逐分支
//   断言。本文件只用标准库与 Core 值类型，不包含 Windows.h/Qt/Framework.h。
// - 不变式"跳过 UI 确认不跳过审计"：每个 AuditEvent（开始/确认接受/确认拒绝/确认被
//   抑制/强制同意回答/结束）都各记一条，本文件的格式化对它们一视同仁。
//
// ============================================================
// 日志文本的字段与不记录的东西
// ============================================================
// - 记录：事件名、范围名、通道名、目标身份串（含 pid/创建时间/代次/基址/长度）、块数、
//   字节数、结果名、强制同意回答、地址范围（起点+长度）、两个风险标志、补充说明。
// - **绝不记录任何字节内容**：AuditRecord 本身就没有字节字段；补充说明文本（来自
//   通道的英文失败原因）还会被清洗（控制字符换成空格）并截断，防止日志注入与超长行。
// ============================================================

#include "../../../shared/evidence/memory_workbench/MemoryWriteTransaction.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ksword::memwb_services_detail
{
    // AuditLogLevel：一条审计记录应当写入的日志级别。
    enum class AuditLogLevel
    {
        Info,
        Warn,
        Error
    };

    // kMaxAuditNoteBytes：补充说明文本在日志里的最大字节数（超出截断并追加 "..."）。
    inline constexpr std::size_t kMaxAuditNoteBytes = 240U;

    // ClassifyAuditRecord：审计记录 -> 日志级别。
    // 规则：
    //   CommitStarted / UiConfirmAccepted / UiConfirmSuppressed            -> Info
    //   UiConfirmDenied（用户在界面确认里拒绝）                              -> Warn
    //   ApprovalAnswered：回答 Deny -> Warn；同意（本块/其余块）-> Info
    //   CommitFinished：
    //     Committed / NoChange                                              -> Info
    //     UserCancelled / Stale / ApprovalDenied / Busy                     -> Warn
    //     TargetChanged / WriteFailed / VerifyMismatch / InvalidSession     -> Error
    //     未识别的结果值                                                    -> Warn
    //     暂存区未还原（scratchAreaDirty）无论结果都提升为 Error（磁盘上留了脏扇区，
    //     成功也要最醒目地留痕）；读-改-写窗口把 Info 提升为 Warn。
    AuditLogLevel ClassifyAuditRecord(const ksword::memwb::AuditRecord& record);

    // AuditEventName / CommitOutcomeName / ApprovalAnswerName：枚举的稳定 ASCII 名
    // （日志文本用，不是界面文案）；越界值返回 "Unknown"。
    const char* AuditEventName(ksword::memwb::AuditEvent event) noexcept;
    const char* CommitOutcomeName(ksword::memwb::CommitOutcome outcome) noexcept;
    const char* ApprovalAnswerName(ksword::memwb::ApprovalAnswer answer) noexcept;

    // ExtractIdentityField：从目标身份串（IdentityKey 的 "memwb-target/1|scope=0|pid=…"
    // 格式）里取某个标签的取值文本。
    // 传入：identity 身份串；label 标签（如 "scope"、"ch"）。
    // 传出：标签对应的值（到下一个 '|' 或串尾）；没有该标签返回空串。
    std::string ExtractIdentityField(const std::string& identity, const std::string& label);

    // SanitizeAuditNote：清洗并截断补充说明。
    // 规则：ASCII 控制字符（含换行、制表）与 DEL 换成空格；双引号换成单引号；
    //       超过 kMaxAuditNoteBytes 截断（不切在 UTF-8 多字节序列中间）并追加 "..."。
    std::string SanitizeAuditNote(const std::string& text);

    // FormatAuditRecord：审计记录 -> 一行日志文本（不含级别前缀与换行）。
    // 格式示例（实际为单行，字段间一个空格）：
    //   event=CommitFinished scope=KernelVirtual channel=StandardDriver pid=0
    //   target=memwb-target/1|scope=1|pid=0|... blocksTotal=2 outcome=VerifyMismatch
    //   blocksWritten=1 bytesWritten=4 scratchDirty=0 rmwWindow=0 note="..."
    std::string FormatAuditRecord(const ksword::memwb::AuditRecord& record);

    // AuditChainRegistry：把"同一次提交链路"映射到同一个日志事件对象的登记表。
    // AGENTS 日志规范要求连续过程只用同一个 kLogEvent；一次 Commit 会产生开始/确认/
    // 强制同意/结束多条审计，它们必须挂在同一个事件上。本表以目标身份串为键。
    // 为什么是模板：真实的链路对象是 kLogEvent（在 Framework.h 里），本文件不能包含
    // 它；夹具用 int 当链路对象就能把登记/淘汰逻辑完整测一遍。
    // 为什么条目用 unique_ptr 存放：kLogEvent 含 const 成员（GUID 不可修改），既不可
    // 拷贝赋值也不可移动赋值，直接放进 vector 会让 erase 编译失败；链路对象放在堆上、
    // 表里只搬动指针，就不要求 Chain 可赋值。
    // 嵌套计数：同一个键重入（例如 Commit 进行中又来一次 Commit，第二次会以 Busy 结束）
    // 时复用同一条链路并把深度加一，每次结束把深度减一，减到零才真正移除——这样重入的
    // 两次调用产生的全部审计都挂在同一个事件上，且第一次的结束不会过早拆掉链路。
    // 有界：最多同时保留 maxChains 条链路；注入接口抛异常会留下"只有开始没有结束"的
    // 落单链路，超过上限时淘汰最早登记的，避免无限增长。
    // 线程安全：本类自身不加锁，调用方（接收器）在外面持锁。
    template <typename Chain>
    class AuditChainRegistry
    {
    public:
        // 构造：maxChains 链路数上限（0 按 1 处理）。
        explicit AuditChainRegistry(std::size_t maxChains)
            : maxChains_(maxChains == 0U ? 1U : maxChains)
        {
        }

        // Open：为 key 打开一层链路。
        // 传入：key 链路键；fresh 键不存在时用来新建链路的对象（键已存在时被丢弃）。
        // 传出：链路引用（直到下一次修改登记表前有效）。键已存在则深度加一并返回原链路；
        //       不存在则新建（深度为 1），超过上限时先淘汰最早登记的一条。
        Chain& Open(const std::string& key, Chain fresh)
        {
            Entry* existing = FindEntry(key);
            if (existing != nullptr)
            {
                ++existing->depth;
                return *existing->chain;
            }
            while (entries_.size() >= maxChains_)
            {
                entries_.erase(entries_.begin());
            }
            Entry entry;
            entry.key = key;
            entry.chain = std::make_unique<Chain>(std::move(fresh));
            entry.depth = 1U;
            entries_.push_back(std::move(entry));
            return *entries_.back().chain;
        }

        // Find：按键查找链路；没有返回空指针。不改变深度。
        Chain* Find(const std::string& key)
        {
            Entry* entry = FindEntry(key);
            return entry == nullptr ? nullptr : entry->chain.get();
        }

        // Close：关闭 key 的一层链路。
        // 传出：深度减到零而真正移除链路时返回 true；仍有嵌套层或键不存在返回 false。
        bool Close(const std::string& key)
        {
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
            {
                if (it->key != key)
                {
                    continue;
                }
                if (it->depth > 1U)
                {
                    --it->depth;
                    return false;
                }
                entries_.erase(it);
                return true;
            }
            return false;
        }

        // Size：当前登记的链路数。
        std::size_t Size() const
        {
            return entries_.size();
        }

        // DepthOf：key 的当前嵌套深度（不存在为 0），供测试与诊断读取。
        std::size_t DepthOf(const std::string& key) const
        {
            for (const Entry& entry : entries_)
            {
                if (entry.key == key)
                {
                    return entry.depth;
                }
            }
            return 0U;
        }

    private:
        // Entry：登记表的一条记录。
        struct Entry
        {
            // key：链路键（目标身份串）。
            std::string key;
            // chain：链路对象，放在堆上以免要求 Chain 可赋值。
            std::unique_ptr<Chain> chain;
            // depth：嵌套深度，至少为 1。
            std::size_t depth = 1U;
        };

        // FindEntry：按键找条目，没有返回空指针。
        Entry* FindEntry(const std::string& key)
        {
            for (Entry& entry : entries_)
            {
                if (entry.key == key)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // maxChains_：登记上限。
        std::size_t maxChains_;
        // entries_：按登记顺序排列的条目；数量有界，线性查找足够。
        std::vector<Entry> entries_;
    };
}
