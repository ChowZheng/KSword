// wpE_tests.Store.cpp
// 作用：AddressBookStore 的离屏验证——加载（缺文件/正常/坏文件）、持久化往返、
// 防抖合并写盘次数、flushNow 跳过防抖、原子写不留 .tmp 残留、按 id 的增删改转发。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>

#include <functional>

namespace wpe_test
{
    namespace
    {
        using ks::ui::AddressBookModel;
        using ks::ui::AddressBookStore;
        using ksword::memwb::EntryKind;
        using ksword::memwb::ValueType;

        // TestLoadMissingFile：文件不存在时 load() 应该成功（空簿、不算失败）。
        void TestLoadMissingFile()
        {
            const QString path = ScratchFilePath(QStringLiteral("missing"));
            QFile::remove(path);
            AddressBookStore store(path);
            WPE_CHECK(store.load());
            WPE_CHECK(!store.lastLoadFailed());
            WPE_CHECK(store.size() == 0);
        }

        // TestRoundTrip：add 若干条目 -> flushNow 落盘 -> 新实例 load() -> 内容逐条一致。
        void TestRoundTrip()
        {
            const QString path = ScratchFilePath(QStringLiteral("roundtrip"));
            QFile::remove(path);
            {
                AddressBookStore store(path);
                const std::uint64_t idA = store.add(MakeModuleEntry(EntryKind::Bookmark, "proc-a", "client.dll", 0x1A40));
                const std::uint64_t idB = store.add(MakeAbsoluteEntry(EntryKind::Watch, "proc-a", 0x7FF600001000ULL));
                WPE_CHECK(idA != 0 && idB != 0 && idA != idB);
                store.setNote(idA, QStringLiteral("测试备注"));
                store.setValueType(idB, ValueType::U32);
                WPE_CHECK(store.flushNow());
            }
            AddressBookStore reloaded(path);
            WPE_CHECK(reloaded.load());
            WPE_CHECK_NOTE(!reloaded.lastLoadFailed(), reloaded.lastLoadErrorText());
            WPE_CHECK(reloaded.size() == 2);
            const std::vector<ksword::memwb::AddressEntry> entries = reloaded.list();
            bool foundModule = false;
            bool foundAbsolute = false;
            for (const ksword::memwb::AddressEntry& entry : entries)
            {
                if (entry.moduleName == "client.dll")
                {
                    foundModule = true;
                    WPE_CHECK(entry.rva == 0x1A40ULL);
                    WPE_CHECK(entry.note == "测试备注");
                    WPE_CHECK(entry.kind == EntryKind::Bookmark);
                }
                if (entry.moduleName.empty() && entry.absoluteAddress == 0x7FF600001000ULL)
                {
                    foundAbsolute = true;
                    WPE_CHECK(entry.valueType == ValueType::U32);
                    WPE_CHECK(entry.kind == EntryKind::Watch);
                }
            }
            WPE_CHECK(foundModule);
            WPE_CHECK(foundAbsolute);
        }

        // TestSearchResultsAreNotPersisted（修复可疑点 #1）：Search 是临时的（下一轮搜索
        // 会清掉它们），不应该被写进地址簿文件——否则搜索结果灌入后每 500ms 整文件重写，
        // 下次启动还会把上一轮已失效进程的搜索结果当成"地址簿内容"载回来。落盘再重载后，
        // Search 条目必须消失，Bookmark/Watch 必须还在。
        void TestSearchResultsAreNotPersisted()
        {
            const QString path = ScratchFilePath(QStringLiteral("search-not-persisted"));
            QFile::remove(path);
            std::uint64_t bookmarkId = 0;
            std::uint64_t watchId = 0;
            {
                AddressBookStore store(path);
                store.add(MakeAbsoluteEntry(EntryKind::Search, "proc-a", 0x7000));
                store.add(MakeAbsoluteEntry(EntryKind::Search, "proc-a", 0x7004));
                bookmarkId = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x8000));
                watchId = store.add(MakeAbsoluteEntry(EntryKind::Watch, "proc-a", 0x9000));
                WPE_CHECK(store.size() == 4);  // 内存里四条都在，持久化只是落盘时过滤。
                WPE_CHECK(store.flushNow());
            }
            AddressBookStore reloaded(path);
            WPE_CHECK(reloaded.load());
            WPE_CHECK_NOTE(!reloaded.lastLoadFailed(), reloaded.lastLoadErrorText());
            WPE_CHECK_NOTE(reloaded.size() == 2, QStringLiteral("重载后只应剩 Bookmark/Watch 两条"));
            WPE_CHECK(reloaded.find(bookmarkId).has_value());
            WPE_CHECK(reloaded.find(watchId).has_value());
            for (const ksword::memwb::AddressEntry& entry : reloaded.list())
            {
                WPE_CHECK_NOTE(entry.kind != EntryKind::Search, QStringLiteral("重载后不应该出现 Search 条目"));
            }
        }

        // TestCorruptFileDoesNotOverwrite：坏文件报"第 N 行：原因"；修复 C4 后原文件不再
        // 原地保留，而是被改名备份到别处——这里核对备份文件里的字节与原始坏文件一致，
        // 并核对原路径此刻确实已经空出来。
        void TestCorruptFileDoesNotOverwrite()
        {
            const QString path = ScratchFilePath(QStringLiteral("corrupt"));
            QFile::remove(path);
            {
                AddressBookStore seedStore(path);
                seedStore.add(MakeAbsoluteEntry(EntryKind::Search, "proc-a", 0x1000));
                WPE_CHECK(seedStore.flushNow());
            }
            // 手工破坏：标题行正确，但条目行字段数不对（少一个制表符分隔的字段）。
            const QByteArray corruptBytes = "KSWORD-ADDRESS-BOOK 1\nnot-a-valid-entry-line\n";
            QFile file(path);
            WPE_CHECK(file.open(QIODevice::ReadWrite));
            file.resize(0);
            file.write(corruptBytes);
            file.close();

            AddressBookStore store(path);
            const bool loadOk = store.load();
            WPE_CHECK(!loadOk);
            WPE_CHECK(store.lastLoadFailed());
            WPE_CHECK_NOTE(
                store.lastLoadErrorText().startsWith(QStringLiteral("第 2 行：")),
                store.lastLoadErrorText());

            // 修复 C4：原文件被改名备份，原路径此刻应该已经不存在；备份文件里的字节
            // 与原始坏文件一致——没有丢失任何"可能还能手工抢救"的内容。
            WPE_CHECK_NOTE(!store.lastLoadBackupPath().isEmpty(), QStringLiteral("备份路径为空"));
            WPE_CHECK(!QFile::exists(path));
            QFile backupFile(store.lastLoadBackupPath());
            WPE_CHECK(backupFile.open(QIODevice::ReadOnly));
            WPE_CHECK(backupFile.readAll() == corruptBytes);
        }

        // TestC4_FailedLoadThenWriteMustNotDestroyOriginal：载入失败后紧接着一次 add+
        // flushNow，坏文件的原始字节必须仍然完整存在于某个地方（备份路径），不会被这次
        // 写入悄悄覆盖——这是 C4 的核心回归点：旧代码里 add/scheduleSave 完全不看
        // m_lastLoadFailed，防抖窗口过后那次落盘会直接 Truncate 打开同一个 filePath，
        // 把坏文件和它里面可能还能手工恢复的合法条目一起冲掉。
        void TestC4_FailedLoadThenWriteMustNotDestroyOriginal()
        {
            const QString path = ScratchFilePath(QStringLiteral("c4-no-overwrite"));
            QFile::remove(path);
            const QByteArray corruptBytes = "KSWORD-ADDRESS-BOOK 1\nnot-a-valid-entry-line\n";
            {
                QFile file(path);
                WPE_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
                file.write(corruptBytes);
            }

            AddressBookStore store(path);
            WPE_CHECK(!store.load());
            const QString backupPath = store.lastLoadBackupPath();
            WPE_CHECK(!backupPath.isEmpty());

            // 载入失败之后照常可以增删改——这些操作本身不该被拒绝，只是它们落盘的目标
            // 已经是改名之后空出来的原路径，不会碰到备份。
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x9999));
            WPE_CHECK(store.flushNow());

            QFile backupFile(backupPath);
            WPE_CHECK(backupFile.open(QIODevice::ReadOnly));
            WPE_CHECK_NOTE(backupFile.readAll() == corruptBytes, QStringLiteral("备份文件内容被改动"));

            QFile freshFile(path);
            WPE_CHECK(freshFile.open(QIODevice::ReadOnly));
            WPE_CHECK_NOTE(freshFile.readAll() != corruptBytes, QStringLiteral("新文件不应该等于坏文件原内容"));
        }

        // TestLoadReportsOrphanTmpFile：正式文件不存在、但同名 ".tmp" 还在——不能悄悄当成
        // 空簿；必须报失败并在错误文案里点名这份 .tmp（C5 的一角）。
        void TestLoadReportsOrphanTmpFile()
        {
            const QString path = ScratchFilePath(QStringLiteral("orphan-tmp"));
            const QString tmpPath = path + QStringLiteral(".tmp");
            QFile::remove(path);
            {
                QFile tmp(tmpPath);
                WPE_CHECK(tmp.open(QIODevice::WriteOnly | QIODevice::Truncate));
                tmp.write("KSWORD-ADDRESS-BOOK 1\n");
            }

            AddressBookStore store(path);
            WPE_CHECK(!store.load());
            WPE_CHECK(store.lastLoadFailed());
            WPE_CHECK_NOTE(store.lastLoadErrorText().contains(tmpPath), store.lastLoadErrorText());

            // 不会自动删除或覆盖那份 .tmp。
            WPE_CHECK(QFile::exists(tmpPath));
            QFile::remove(tmpPath);
        }

        // TestC6_DestructorFlushesPendingSave：防抖窗口内（还没到 500ms）析构，改动必须
        // 已经落盘——否则关闭窗口/退出进程最常见地恰好撞在防抖窗口内，白白丢掉最后一次
        // 操作。
        void TestC6_DestructorFlushesPendingSave()
        {
            const QString path = ScratchFilePath(QStringLiteral("c6-destructor-flush"));
            QFile::remove(path);
            {
                AddressBookStore store(path);
                store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x1234));
                // 没有等够 500ms，也没有手动 flushNow：离开这个作用域就是唯一的落盘机会。
            }
            WPE_CHECK_NOTE(QFile::exists(path), QStringLiteral("析构应补一次 flushNow"));
            AddressBookStore reloaded(path);
            WPE_CHECK(reloaded.load());
            WPE_CHECK(reloaded.size() == 1);
        }

        // TestC18_NulInNoteDoesNotTruncateFile：备注含 NUL 字节时落盘不应截断整份文件；
        // Store::setNote 会先过滤掉 NUL，所以重载后应有两条且备注里不含 NUL。
        void TestC18_NulInNoteDoesNotTruncateFile()
        {
            const QString path = ScratchFilePath(QStringLiteral("c18-nul-note"));
            QFile::remove(path);
            AddressBookStore store(path);
            const std::uint64_t idA = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            const std::uint64_t idB = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
            QString noteWithNul = QStringLiteral("前半");
            noteWithNul += QChar(u'\0');
            noteWithNul += QStringLiteral("后半");
            WPE_CHECK(store.setNote(idA, noteWithNul));
            WPE_CHECK(store.setNote(idB, QStringLiteral("普通备注")));
            WPE_CHECK(store.flushNow());

            AddressBookStore reloaded(path);
            WPE_CHECK(reloaded.load());
            WPE_CHECK_NOTE(!reloaded.lastLoadFailed(), reloaded.lastLoadErrorText());
            WPE_CHECK(reloaded.size() == 2);
            const std::optional<ksword::memwb::AddressEntry> entryA = reloaded.find(idA);
            WPE_CHECK(entryA.has_value());
            if (entryA.has_value())
            {
                WPE_CHECK_NOTE(
                    entryA->note.find('\0') == std::string::npos,
                    QStringLiteral("备注里不应残留 NUL"));
            }
        }

        // ================= [SURVIVOR] 补测试：杀死审核报告里幸存的副本变异 =================

        // 杀 wpE-S1：remove/promote/setValueType/setNote/removeMany 都必须各自调度恰好
        // 一次保存——之前的夹具只断言了 add/setNote 的合并写盘，这几个口子删掉
        // scheduleSave() 调用不会被任何测试发现。
        void TestEveryMutatorSchedulesExactlyOneSave()
        {
            const QString path = ScratchFilePath(QStringLiteral("persist-all"));
            QFile::remove(path);
            AddressBookStore store(path);
            const std::uint64_t a = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 0x1000));
            const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x2000));
            const std::uint64_t c = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x3000));
            const std::uint64_t d = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x4000));

            const auto expectOneSave = [&](const char* what, const std::function<void()>& action) {
                WaitMs(700);
                const int before = store.writeCount();
                action();
                WPE_CHECK_NOTE(store.pendingSave(), QString::fromLatin1(what));
                WaitMs(700);
                WPE_CHECK_NOTE(store.writeCount() == before + 1, QString::fromLatin1(what));
            };
            expectOneSave("remove", [&]() { store.remove(a); });
            expectOneSave("promote", [&]() { store.promote(b, EntryKind::Bookmark); });
            expectOneSave("setValueType", [&]() { store.setValueType(c, ValueType::U32); });
            expectOneSave("setNote", [&]() { store.setNote(c, QStringLiteral("n")); });
            expectOneSave("removeMany", [&]() { store.removeMany({ d }); });

            AddressBookStore reloaded(path);
            WPE_CHECK(reloaded.load());
            WPE_CHECK(!reloaded.find(a).has_value() && !reloaded.find(d).has_value());
            WPE_CHECK(reloaded.find(b).has_value() && reloaded.find(b)->kind == EntryKind::Bookmark);
            WPE_CHECK(reloaded.find(c).has_value()
                && reloaded.find(c)->valueType == ValueType::U32
                && reloaded.find(c)->note == "n");
        }

        // 杀 wpE-S2：flushNow 必须停掉防抖定时器，否则之后还会有一次迟到的防抖写。
        void TestFlushNowCancelsPendingSave()
        {
            const QString path = ScratchFilePath(QStringLiteral("flush-cancels"));
            QFile::remove(path);
            AddressBookStore store(path);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            store.setNote(id, QStringLiteral("x"));
            WPE_CHECK(store.pendingSave());
            WPE_CHECK(store.flushNow());
            WPE_CHECK(!store.pendingSave());
            const int writes = store.writeCount();
            WaitMs(700);
            WPE_CHECK_NOTE(store.writeCount() == writes, QStringLiteral("flushNow 之后不应再有一次迟到的防抖写"));
        }

        // 杀 wpE-S3：removeMany/clearSearchResults 删不到任何东西时必须保持安静（不发
        // reset、不调度保存），不能像"删到了"一样 reset+存盘。
        void TestNoOpBulkRemoveIsSilent()
        {
            const QString path = ScratchFilePath(QStringLiteral("noop-bulk"));
            QFile::remove(path);
            AddressBookStore store(path);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            WaitMs(700);
            const int writes = store.writeCount();
            int resets = 0;
            QObject::connect(&store, &AddressBookStore::reset, [&resets]() { ++resets; });
            WPE_CHECK(store.removeMany({ id + 100, id + 101 }) == 0);
            WPE_CHECK(store.removeMany({}) == 0);
            WPE_CHECK(store.clearSearchResults() == 0);  // 簿里没有 Search 条目。
            WPE_CHECK(resets == 0);
            WPE_CHECK(!store.pendingSave());
            WaitMs(700);
            WPE_CHECK(store.writeCount() == writes);
        }

        // 杀 wpE-S4：load() 成功必须发一次 reset()，即便模型是在 load() 之前就已经建好的
        // （WorkbenchShared 的真实顺序）——不发信号的话，先建好的模型永远不会刷新。
        void TestLoadResetsAlreadyAttachedModel()
        {
            const QString path = ScratchFilePath(QStringLiteral("load-reset"));
            QFile::remove(path);
            {
                AddressBookStore writer(path);
                writer.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
                WPE_CHECK(writer.flushNow());
            }
            AddressBookStore store(path);
            AddressBookModel model(&store);  // 模型先于 load 建立。
            WPE_CHECK(model.rowCount() == 0);
            WPE_CHECK(store.load());
            WPE_CHECK(model.rowCount() == 1);
            WPE_CHECK(model.kindCounts().all == 1 && model.kindCounts().bookmark == 1);
        }

        // 杀 wpE-S5：load() 失败路径绝不能清空内存里已有的内容——此前若把一个新解析出来
        // 的空簿换进 m_book，Store 本来就是空的用例根本看不出区别，必须先放好内容再失败。
        void TestFailedReloadKeepsMemoryContent()
        {
            const QString path = ScratchFilePath(QStringLiteral("reload-keeps"));
            QFile::remove(path);
            AddressBookStore store(path);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
            WPE_CHECK(store.flushNow());
            // 直接改坏磁盘上的文件（不经过本进程的 Store），模拟外部篡改/传输错误。
            QFile raw(path);
            WPE_CHECK(raw.open(QIODevice::WriteOnly | QIODevice::Truncate));
            raw.write("KSWORD-ADDRESS-BOOK 1\nnot-a-valid-entry-line\n");
            raw.close();
            WPE_CHECK(!store.load());
            WPE_CHECK(store.lastLoadFailed());
            WPE_CHECK_NOTE(store.size() == 1, QStringLiteral("载入失败必须保持调用前的内存内容"));
        }

        // 杀 wpE-S6：写盘失败必须如实报告（saveFailed 信号 + flushNow 返回 false），
        // 不能把失败悄悄当成功。用"把正式文件路径占成一个目录"做失败注入——QSaveFile
        // 对着一个目录 open(WriteOnly) 必然失败。
        void TestFlushNowReportsFailure()
        {
            const QString path = ScratchFilePath(QStringLiteral("flush-fail"));
            QFile::remove(path);
            QDir().mkpath(path);
            AddressBookStore store(path);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            QSignalSpy failed(&store, &AddressBookStore::saveFailed);
            WPE_CHECK(!store.flushNow());
            WPE_CHECK(failed.count() == 1);
            WPE_CHECK(QFileInfo(path).isDir());  // 原"文件"（此处是目录）没被动过。
            QDir().rmdir(path);
            QFile::remove(path + QStringLiteral(".tmp"));
        }

        // TestDebounceMergesWrites：连续多次操作只在等待防抖窗口后落一次盘；
        // flushNow 则立刻落盘，跳过防抖等待。
        void TestDebounceMergesWrites()
        {
            const QString path = ScratchFilePath(QStringLiteral("debounce"));
            QFile::remove(path);
            AddressBookStore store(path);
            const int writesBefore = store.writeCount();

            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x2000));
            store.setNote(id, QStringLiteral("a"));
            store.setNote(id, QStringLiteral("ab"));
            store.setNote(id, QStringLiteral("abc"));
            // 三次 setNote 都在 500ms 防抖窗口内发生：此刻还没有真正落盘。
            WPE_CHECK(store.writeCount() == writesBefore);
            WPE_CHECK(store.pendingSave());

            WaitMs(700);
            // 等过防抖窗口：四次操作（add+3×setNote）合并成恰好一次落盘。
            WPE_CHECK(store.writeCount() == writesBefore + 1);
            WPE_CHECK(!store.pendingSave());

            // flushNow：跳过防抖，立即生效。
            store.setNote(id, QStringLiteral("abcd"));
            WPE_CHECK(store.writeCount() == writesBefore + 1);
            WPE_CHECK(store.flushNow());
            WPE_CHECK(store.writeCount() == writesBefore + 2);
        }

        // TestAtomicWriteLeavesNoTmp：flushNow 成功后，同目录不应残留 "<文件名>.tmp"。
        void TestAtomicWriteLeavesNoTmp()
        {
            const QString path = ScratchFilePath(QStringLiteral("atomic"));
            const QString tmpPath = path + QStringLiteral(".tmp");
            QFile::remove(path);
            QFile::remove(tmpPath);

            AddressBookStore store(path);
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "proc-a", 0x3000));
            WPE_CHECK(store.flushNow());
            WPE_CHECK(QFile::exists(path));
            WPE_CHECK(!QFile::exists(tmpPath));
        }

        // TestPromoteRejectsDowngrade：Promote 到 Search 被拒绝（禁止降级），kind 不变、
        // 不触发保存。这是变异 #1 的回归点：若 AddressBookStore::promote 忘了检查
        // MemoryAddressBook::Promote 的返回值，这里会先炸在"kind 仍是 Bookmark"上。
        void TestPromoteRejectsDowngrade()
        {
            const QString path = ScratchFilePath(QStringLiteral("promote-guard"));
            QFile::remove(path);
            AddressBookStore store(path);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x4000));
            // 先让 add() 自己那次防抖落盘跑完，这样下面检查"promote 失败不调度任何保存"
            // 时 pendingSave() 为假才有意义——否则测的其实是 add() 遗留的那次防抖，不是
            // promote 到底有没有调度新的一次。
            WaitMs(700);
            const int writesBefore = store.writeCount();

            WPE_CHECK(!store.promote(id, EntryKind::Search));
            const std::optional<ksword::memwb::AddressEntry> entry = store.find(id);
            WPE_CHECK(entry.has_value());
            if (entry.has_value())
            {
                WPE_CHECK(entry->kind == EntryKind::Bookmark);
            }
            // 被拒绝的 Promote 不应调度保存——writeCount 此刻应该还没变化
            // （即便等够防抖窗口也不会变化，这里只检查没有立刻调度）。
            WPE_CHECK(store.writeCount() == writesBefore);
            WPE_CHECK(!store.pendingSave());
        }

        // TestClearSearchResults：批量删除只触发一次 reset；非 Search 条目不受影响。
        void TestClearSearchResults()
        {
            const QString path = ScratchFilePath(QStringLiteral("clear-search"));
            QFile::remove(path);
            AddressBookStore store(path);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "proc-a", 0x5000));
            store.add(MakeAbsoluteEntry(EntryKind::Search, "proc-a", 0x5004));
            const std::uint64_t bookmarkId = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "proc-a", 0x5008));

            int resetCount = 0;
            QObject::connect(&store, &AddressBookStore::reset, [&resetCount]() { ++resetCount; });

            const std::size_t removed = store.clearSearchResults();
            WPE_CHECK(removed == 2);
            WPE_CHECK(resetCount == 1);
            WPE_CHECK(store.size() == 1);
            WPE_CHECK(store.find(bookmarkId).has_value());
        }
    }

    void RunStoreTests()
    {
        TestLoadMissingFile();
        TestRoundTrip();
        TestSearchResultsAreNotPersisted();
        TestCorruptFileDoesNotOverwrite();
        TestDebounceMergesWrites();
        TestAtomicWriteLeavesNoTmp();
        TestPromoteRejectsDowngrade();
        TestClearSearchResults();
        TestC4_FailedLoadThenWriteMustNotDestroyOriginal();
        TestLoadReportsOrphanTmpFile();
        TestC6_DestructorFlushesPendingSave();
        TestC18_NulInNoteDoesNotTruncateFile();
        TestEveryMutatorSchedulesExactlyOneSave();
        TestFlushNowCancelsPendingSave();
        TestNoOpBulkRemoveIsSilent();
        TestLoadResetsAlreadyAttachedModel();
        TestFailedReloadKeepsMemoryContent();
        TestFlushNowReportsFailure();
    }
}
