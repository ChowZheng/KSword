#pragma once

// ============================================================
// AddressBookStore.h
// 作用：
// - 把纯逻辑层 ksword::memwb::MemoryAddressBook（Qt-free，见
//   shared/evidence/memory_workbench/MemoryAddressBook.h）包装成一个 Qt 对象：
//   对外提供按 id 的增删改查、变更信号（供 AddressBookModel 监听刷新），并把簿的
//   内容持久化到调用方给定的文件路径。
// - 本类只负责"地址簿"这一份小文件的读写，绝不触碰目标进程/内核的内存——不含
//   Windows.h、不调用任何读写内存的接口；这一点与 AddressBookModel 头注释的
//   "本类不做任何 I/O"（指内存 I/O）是同一条边界，Store 这里做的是地址簿文件 I/O。
//
// 持久化策略：
// - 格式固定为 MemoryAddressBook 的 v1 文本格式（一次性新格式，没有旧格式兼容代码；
//   旧书签从未持久化，不存在"迁移"这一步）。
// - 写盘用"写临时文件再重命名"的原子方式（见 .cpp 的 WriteAtomic）：先把完整内容写进
//   同目录下的 `<文件名>.tmp`，`flush()` 成功后才 QFile::rename 替换正式文件；中途进程
//   崩溃或断电时，正式文件要么是旧内容要么是新内容，不会出现半截文件。
// - 每次增删改之后不立即写盘，而是启动/重置一个 500 ms 的单次定时器（防抖）：短时间内
//   连续多次操作（例如批量删除、连续改备注）只落一次盘。writeCount() 供测试断言"确实合并
//   写盘次数"而不是只看最终文件内容对不对。
// - 加载失败（见 load()）时旧内容原样保留、绝不写回原文件：MemoryAddressBook::Deserialize
//   本身保证失败路径不碰传入的簿，Store 在失败分支直接返回，不调度任何保存；解析失败还会
//   把坏文件改名备份，后续写入落在一份全新文件上（见 C4，load() 与 lastLoadBackupPath 的
//   注释）。
// - 析构会在仍有未落盘改动（pendingSave() 为真）时补一次 flushNow()（见 C6）：防抖窗口内
//   关闭窗口/退出进程本来是最容易丢最后 500ms 改动的时刻，不能指望调用方记得手动 flush。
//
// 单写者假设（重要，调用方必须遵守）：
// - 本类不做任何跨进程/跨实例的文件锁，也不监视文件被外部改动。设计前提是"同一份地址簿
//   文件在任意时刻只有一个 AddressBookStore 实例会调用它的写方法"——Phase 3 设计把这个
//   单写者角色定为进程级单例（由上层 WorkbenchShared 持有），内嵌进程详情窗口等其它实例
//   应该只调用只读方法（list/find/size）。本类自身不校验这一点，违反它的后果是"后写的实例
//   覆盖先写的实例的改动"，与普通文本文件被两个程序同时保存的后果一样。
// ============================================================

#include "../../../../shared/evidence/memory_workbench/MemoryAddressBook.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

class QTimer;

namespace ks::ui
{
    // AddressBookStore：地址簿的 Qt 持久化包装，非拷贝（QObject）。
    class AddressBookStore final : public QObject
    {
        Q_OBJECT

    public:
        // 构造：filePath 为持久化文件路径（不要求已存在，load() 时按需创建目录）；
        // 传空字符串表示"仅内存、不持久化"（供夹具或单元测试使用），此时 flushNow 直接返回 true。
        explicit AddressBookStore(QString filePath, QObject* parent = nullptr);
        ~AddressBookStore() override;

        AddressBookStore(const AddressBookStore&) = delete;
        AddressBookStore& operator=(const AddressBookStore&) = delete;

        // filePath：持久化文件路径（构造时给定，不可更改）。
        QString filePath() const;

        // load：从 filePath 读取并解析整份地址簿。
        // 传出：true 表示文件不存在（视作"空簿"，不算错误）或解析成功；false 表示文件存在但
        //       解析失败，或文件不存在但发现同名 .tmp 残留（见下）——此时簿内容保持调用前的
        //       状态，lastLoadFailed()/lastLoadErrorText() 给出"第 N 行：原因"文案，不会覆盖
        //       原文件。成功时先发一次 bookReloaded()、再发一次 reset()（见 bookReloaded
        //       的注释：两者分别对应"值缓存要不要整个清空"与"行结构要不要整表重建"）。
        // 修复 D7（下一个 id 没有带着载入前的计数器走，值缓存可能挂到号段复用后的新条目
        // 上）：失败时整本簿（含其内部的"下一个 id"计数器）保持调用前的状态不变；成功时
        // 会把 m_book 复制一份再交给 Deserialize（而不是传一个全新默认构造的对象）——
        // MemoryAddressBook::Deserialize 的契约是"成功后下一个 id = max(传入 out 载入前的
        // 下一个 id, 文件中最大 id + 1)"，这里的"传入 out"必须是带着 m_book 当前计数器的
        // 副本，否则这个承诺形同虚设（永远只看到默认构造对象的初始计数器 1）。
        // 修复 C4（载入失败后任何写操作都会覆盖坏文件）：解析失败时会把原文件改名备份为
        // "<文件名>.bad-<yyyyMMdd-HHmmss>"（见 lastLoadBackupPath），这样原路径就空出来了，
        // 后续的增删改落盘只会在原路径新建一份全新文件，绝不会碰到那份坏文件；备份本身失败
        // （例如文件被占用）时 lastLoadBackupPath() 为空，原文件原地不动，后续写入仍会覆盖它
        // ——这是改名失败时唯一能做的事，不强行删除或锁定用户的文件。
        bool load();

        // lastLoadFailed / lastLoadErrorText：上一次 load() 的结论；load() 从未调用过时
        // lastLoadFailed() 为 false、文本为空。
        bool lastLoadFailed() const;
        QString lastLoadErrorText() const;

        // lastLoadBackupPath：上一次 load() 解析失败且成功把原文件改名备份后，备份文件的
        // 完整路径；没有发生过备份（成功加载，或失败但改名本身也失败）时为空串。
        QString lastLoadBackupPath() const;

        // ---- 只读查询：转发给内部的 MemoryAddressBook ----
        std::vector<ksword::memwb::AddressEntry> list(
            const ksword::memwb::AddressFilter& filter = ksword::memwb::AddressFilter{}) const;
        std::optional<ksword::memwb::AddressEntry> find(std::uint64_t id) const;
        std::size_t size() const;

        // ---- 写操作：均会触发防抖保存（见 scheduleSave）与对应的变更信号 ----

        // add：新增一个条目；draft.id 被忽略，簿分配新 id。失败（kind/valueType 非法）返回 0，
        // 不触发任何信号或保存。
        std::uint64_t add(const ksword::memwb::AddressEntry& draft);

        // addMany：批量新增；只调度一次保存、只发一次 reset()（而不是 N 次 entryAdded + N 次
        // 整表重建），供"从搜索结果批量加入书签"一类批量场景使用。修复 C8：地址簿设计上限约
        // 一万条，逐条调用 add() 在该规模下会导致 O(n²) 的 UI 冻结（每条都触发视图整表重排、
        // 排序代理重新排序、kind 分段控件重建）；批量接口把这些代价摊成一次。
        // 传入：草稿列表，顺序即新增顺序；草稿里非法的 kind/valueType 会被 Core 的 Add 拒绝
        //       （该条跳过，不中断整批）。
        // 传出：成功新增的 id 列表，与被接受的草稿顺序一致；全部失败时为空且不触发任何事。
        std::vector<std::uint64_t> addMany(const std::vector<ksword::memwb::AddressEntry>& drafts);

        // remove：删除单个条目；id 不存在返回 false，不触发信号或保存。
        bool remove(std::uint64_t id);

        // removeMany：批量删除；只对真正存在的 id 生效。只要删掉了至少一个，就只发一次
        // reset()（而不是逐个 entryRemoved）并只调度一次保存，避免"清空搜索结果"这类批量操作
        // 在 UI 侧触发成百上千次信号。传出：实际删除的条目数。
        std::size_t removeMany(const std::vector<std::uint64_t>& ids);

        // setNote / setValueType：修改单个条目；id 不存在或类型越界返回 false。
        bool setNote(std::uint64_t id, QString note);
        bool setValueType(std::uint64_t id, ksword::memwb::ValueType valueType);

        // promote：把条目的 kind 改成 newKind；禁止把 Bookmark/Watch 降回 Search（与
        // MemoryAddressBook::Promote 的规则一致），该情况返回 false 且不触发任何事。
        bool promote(std::uint64_t id, ksword::memwb::EntryKind newKind);

        // clearSearchResults：删掉全部 kind==Search 的条目（"清空搜索结果"）。
        // 传出：实际删除的条目数；为 0 时不触发 reset 或保存。
        std::size_t clearSearchResults();

        // flushNow：跳过防抖，立即把当前内容写盘（原子方式）。供窗口关闭前的最后一次保存，
        // 以及测试直接断言文件内容。filePath 为空时直接返回 true（无需写盘）。
        bool flushNow();

        // writeCount：实际发生的磁盘写入次数（含 flushNow 触发的），供测试断言防抖确实合并了
        // 多次操作。不计 load() 本身。
        int writeCount() const;

        // pendingSave：防抖定时器此刻是否还在排队等待落盘（供测试观察防抖窗口内的状态）。
        bool pendingSave() const;

    signals:
        // entryAdded/entryRemoved/entryChanged：单个条目的增量变更，供 Model 做精确的行增删改。
        void entryAdded(quint64 id);
        void entryRemoved(quint64 id);
        void entryChanged(quint64 id);
        // reset：批量变更（load 成功、removeMany、addMany、clearSearchResults），Model 应整表重建。
        void reset();
        // bookReloaded：修复 D7 的后半——专门标记"整本被 load() 整体替换"这件事，与批量
        // 增删（reset()）区分开。load() 成功时会先发这个信号、再发 reset()：load() 之后
        // 旧的 id 号段可能被文件里的内容复用（见 .cpp load() 的注释与 MemoryAddressBook.h
        // 关于"下一个 id"的规则），同一个 id 在载入前后可能对应着完全不同的真实条目，
        // Model 那一侧按"id 是否仍存在"做的值缓存清理（computeCountsAndRowIds 的
        // existingIds 差集）分辨不出这种情况——必须整个清空值缓存，而 removeMany/addMany
        // 那种"部分修改"场景不能这么做（会把存活条目的值缓存一起冲掉，见 D7 与 NC20 的
        // 测试）。
        void bookReloaded();
        // saveFailed：防抖落盘或 flushNow 时原子写失败（磁盘满、权限不足等极少见情况）；
        // reason 是供诊断展示的简短英文/路径文本，簿在内存里的内容不受影响。修复 D1：
        // 失败后不会就此放弃——见 onSaveTimerTimeout/flushNow 的退避重试注释。
        void saveFailed(QString reason);

    private slots:
        // onSaveTimerTimeout：防抖定时器到期后真正落盘。
        void onSaveTimerTimeout();

    private:
        // scheduleSave：filePath 非空时启动/重置 500 ms 单次定时器；为空时什么都不做。
        // 内部转调 scheduleSaveInternal(kSaveDebounceMs)。
        void scheduleSave();

        // scheduleSaveInternal：scheduleSave 的通用实现，delayMs 可调。
        // 修复 D1（写失败后"脏标记"丢失）：onSaveTimerTimeout/flushNow 写失败时会用更长的
        // delayMs（kSaveRetryBackoffMs）再调一次本函数——单次定时器一旦触发就会变回"未激活"
        // （pendingSave() 据此判断），原先失败后什么都不做，相当于这次改动既不重试、析构
        // 时 pendingSave() 也已经是 false 不会补写，彻底丢失。重新把定时器排起来之后，
        // pendingSave() 会再次变真：防抖窗口内若又有新的改动，scheduleSave() 的正常 500 ms
        // 调用会顶掉这次退避重试（符合预期，不是竞争）；若窗口内没人动它，到点后
        // onSaveTimerTimeout 会再试一次；析构时若这次重试还没来得及触发，~AddressBookStore
        // 现有的 `if (pendingSave()) flushNow();` 会立即把它补上——不需要额外新增成员变量，
        // 复用既有的"定时器是否激活"这个状态就足够表达"是否还有未落盘的改动"。
        void scheduleSaveInternal(int delayMs);

        // serializePersistableBytes：把 m_book 按 v1 格式编码成即将写盘的字节序列。
        // 修复可疑点 #1（Search 结果被持久化）：只保存 Bookmark / Watch，不保存临时的
        // Search 结果——MemoryAddressBook.h 头注释本就建议如此，否则搜索结果灌入后每
        // 500ms 整文件重写，下次启动还会把上一轮已失效进程的搜索结果当"地址簿内容"载回来。
        // 修复 C18：直接按长度构造 QByteArray，不经过 QString::fromUtf8(std::string::c_str())
        // ——后者遇到备注里的嵌入 NUL 字节会在第一个 NUL 处截断，导致整份文件被悄悄砍掉一部分
        // 又不以 LF 结尾，下次加载会连同前面合法的条目一起判定为损坏。
        QByteArray serializePersistableBytes() const;

        // writeAtomic：把 bytes 写进 filePath，原子替换正式文件。
        // 传出：成功返回 true；失败（含提交失败）返回 false 且不改变已存在的正式文件。
        bool writeAtomic(const QByteArray& bytes);

        // backupCorruptFile：load() 解析失败后，把原文件改名备份为
        // "<文件名>.bad-<yyyyMMdd-HHmmss>"；成功则记录到 m_lastLoadBackupPath，失败
        // （例如文件被占用）则保持为空、原文件原地不动。见 C4。
        // 修复 D5：时间戳只精确到秒，同一秒内第二次坏文件备份会撞上第一份已经占用的文件名
        // ——QFile::rename 在目标已存在时直接返回 false，第二份坏文件就被当成"备份失败"晾
        // 在原路径，之后任何写入都会把它覆盖掉。目标名已存在时改追加 "-1"/"-2"/… 后缀直到
        // 找到一个未占用的名字，而不是直接认输。
        void backupCorruptFile();

        QString m_filePath;                       // 持久化文件路径，空表示纯内存模式。
        ksword::memwb::MemoryAddressBook m_book;   // 纯逻辑层的地址簿本体。
        QTimer* m_saveTimer = nullptr;             // 500 ms 单次防抖定时器，懒创建。
        bool m_lastLoadFailed = false;             // 上一次 load() 是否失败。
        QString m_lastLoadErrorText;               // 上一次 load() 失败时的"第 N 行：原因"文案。
        QString m_lastLoadBackupPath;              // 上一次 load() 失败后坏文件的备份路径，见上。
        int m_writeCount = 0;                      // 实际落盘次数（测试用）。
    };
}
