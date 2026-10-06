// wpE_tests.Wave2.Store.cpp
// 作用：第二轮独立审核报告 review2-wpE.md 的 Store 相关补测（并入默认运行，不再靠环境变量
// 开关）。覆盖：D3（Store 用户可见文案经 ks::i18n）、D5（同一秒内第二次坏文件备份改名不
// 冲突）、D1（写失败后析构仍补写）、C5（原子写真的替换文件而不是原地截断）、addMany 整个
// 函数此前零覆盖、目标目录不存在时 flushNow 会先建目录、草稿里的 NUL 不破坏整份文件。
// 来源：Wave 2 第二轮独立审核给出的补测（已实测对未变异代码全部 PASS，对应缺陷修复前
// FAIL），按 Store/Model/Panel 拆成三个文件并入本夹具；
// 路径改用本夹具既有的 ScratchFilePath，审核目录专用的 helper（P/WriteFile 等）保留同名但
// 落在本夹具自己的临时目录下。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>

#include <windows.h>

using ks::ui::AddressBookModel;
using ks::ui::AddressBookStore;
using ksword::memwb::EntryKind;
using namespace wpe_test;

namespace
{
    // WriteFile / ReadFile：直接读写原始字节，不经过 AddressBookStore——用于构造"坏文件"
    // /"孤儿 .tmp"这类只有手写字节才能制造的异常输入。
    void WriteFile(const QString& path, const QByteArray& bytes)
    {
        QFile file(path);
        file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        file.write(bytes);
        file.close();
    }

    // RemoveScratchFilesWithPrefix：清掉上一轮测试在 ScratchFilePath 所在目录下留下的
    // 同前缀文件（备份名带时间戳、addMany 产物等），避免重复运行时互相干扰。
    void RemoveScratchFilesWithPrefix(const QString& prefix)
    {
        const QString dirPath = QFileInfo(ScratchFilePath(QStringLiteral("probe"))).absolutePath();
        QDir dir(dirPath);
        for (const QString& name : dir.entryList(QStringList() << (prefix + QStringLiteral("*")), QDir::Files | QDir::Hidden))
        {
            QFile::remove(dir.filePath(name));
        }
    }

    // FileIdOf：取文件的 NTFS 文件索引号（身份标识）。QSaveFile 的原子替换会让正式路径
    // 指向一份全新的文件（索引号变化）；原地截断写则是同一份文件（索引号不变）——这是
    // 黑盒下唯一能分辨"替换"与"原地写"的办法（见 C5 的修复说明：读者持有文件这个场景
    // 两种实现表现相同，分不出区别）。
    quint64 FileIdOf(const QString& path)
    {
        const std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();
        HANDLE handle = CreateFileW(
            widePath.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return 0;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        const BOOL ok = GetFileInformationByHandle(handle, &info);
        CloseHandle(handle);
        return ok ? ((static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow) : 0;
    }

    // ========================================================================================
    // [DEFECT->已修] D3：Store 的三处用户可见文案（解析失败"第 N 行：…"、孤儿 .tmp、写入
    // 失败）此前直接返回中文，没有经过 ks::i18n；语言包里已登记的词条因此是死键。
    // ========================================================================================
    void TestI18n_StoreMessagesAreTranslated()
    {
        QString err;
        ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &err);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &err);

        const auto hasHan = [](const QString& text) {
            for (const QChar ch : text)
            {
                if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF)
                {
                    return true;
                }
            }
            return false;
        };

        RemoveScratchFilesWithPrefix(QStringLiteral("w2_i18n"));
        const QString badPath = ScratchFilePath(QStringLiteral("w2_i18n_bad"));
        WriteFile(badPath, "KSWORD-ADDRESS-BOOK 1\nbad\n");
        AddressBookStore badStore(badPath);
        badStore.load();
        WPE_CHECK_NOTE(!hasHan(badStore.lastLoadErrorText()), badStore.lastLoadErrorText());

        const QString orphan = ScratchFilePath(QStringLiteral("w2_i18n_orphan"));
        WriteFile(orphan + QStringLiteral(".tmp"), "KSWORD-ADDRESS-BOOK 1\n");
        AddressBookStore orphanStore(orphan);
        orphanStore.load();
        WPE_CHECK_NOTE(!hasHan(orphanStore.lastLoadErrorText()), orphanStore.lastLoadErrorText().left(60));

        const QString dirPath = ScratchFilePath(QStringLiteral("w2_i18n_dir"));
        QDir().mkpath(dirPath);
        AddressBookStore dirStore(dirPath);
        dirStore.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        QSignalSpy failed(&dirStore, &AddressBookStore::saveFailed);
        dirStore.flushNow();
        WPE_CHECK(failed.count() == 1);
        if (failed.count() == 1)
        {
            WPE_CHECK_NOTE(!hasHan(failed.first().first().toString()), failed.first().first().toString());
        }
        QDir().rmdir(dirPath);

        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA06-C5nonAtomic：黑盒下用文件身份（NTFS 文件索引号）分辨"替换"与
    // "原地截断写"——TestAtomicWriteLeavesNoTmp 只查 .tmp 不存在，对两种实现都恒真。
    // ========================================================================================
    void TestC5_FlushReplacesFileInsteadOfWritingInPlace()
    {
        RemoveScratchFilesWithPrefix(QStringLiteral("w2_c5"));
        const QString path = ScratchFilePath(QStringLiteral("w2_c5"));
        AddressBookStore store(path);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        WPE_CHECK(store.flushNow());
        const quint64 before = FileIdOf(path);
        WPE_CHECK(before != 0);
        store.setNote(id, QStringLiteral("v2"));
        WPE_CHECK(store.flushNow());
        const quint64 after = FileIdOf(path);
        WPE_CHECK_NOTE(after != 0 && after != before,
            QStringLiteral("原子写应替换正式文件（文件索引号变化）：before=%1 after=%2").arg(before).arg(after));
        AddressBookStore reloaded(path);
        WPE_CHECK(reloaded.load());
        WPE_CHECK(reloaded.find(id).has_value() && reloaded.find(id)->note == "v2");
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC01/NC02/NC03：addMany 整个函数此前零覆盖。
    // ========================================================================================
    void TestAddManyBasic()
    {
        RemoveScratchFilesWithPrefix(QStringLiteral("w2_many"));
        const QString path = ScratchFilePath(QStringLiteral("w2_many"));
        AddressBookStore store(path);
        AddressBookModel model(&store);
        int resets = 0;
        QObject::connect(&store, &AddressBookStore::reset, [&resets]() { ++resets; });
        ksword::memwb::AddressEntry invalid = MakeAbsoluteEntry(EntryKind::Bookmark, "p", 9);
        invalid.kind = static_cast<EntryKind>(99);
        const std::vector<ksword::memwb::AddressEntry> drafts{
            MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1), invalid,
            MakeAbsoluteEntry(EntryKind::Watch, "p", 2), MakeAbsoluteEntry(EntryKind::Search, "p", 3)
        };
        const std::vector<std::uint64_t> ids = store.addMany(drafts);
        WPE_CHECK(ids.size() == 3);
        WPE_CHECK(store.size() == 3);
        WPE_CHECK_NOTE(resets == 1, QString::number(resets));
        WPE_CHECK(model.rowCount() == 3);
        WPE_CHECK(model.kindCounts().all == 3 && model.kindCounts().bookmark == 1
            && model.kindCounts().watch == 1 && model.kindCounts().search == 1);
        WPE_CHECK(store.pendingSave());
        WaitMs(700);
        WPE_CHECK(store.writeCount() == 1);
        AddressBookStore reloaded(path);
        WPE_CHECK(reloaded.load());
        WPE_CHECK_NOTE(reloaded.size() == 2, QStringLiteral("Search 不落盘，重载后应为 2"));
        const std::vector<std::uint64_t> none = store.addMany({ invalid });
        WPE_CHECK(none.empty());
        WPE_CHECK(resets == 1);
    }

    // ========================================================================================
    // [DEFECT->已修] D5：备份名时间戳只精确到秒，同一秒内第二次坏文件没被备份、原地保留，
    // 随后被覆盖。
    // ========================================================================================
    void TestBackupNamesAreUniqueAcrossRepeatedFailures()
    {
        RemoveScratchFilesWithPrefix(QStringLiteral("w2_bk"));
        const QString path = ScratchFilePath(QStringLiteral("w2_bk"));
        WriteFile(path, "KSWORD-ADDRESS-BOOK 1\nbad-one\n");
        AddressBookStore s1(path);
        WPE_CHECK(!s1.load());
        WriteFile(path, "KSWORD-ADDRESS-BOOK 1\nbad-two\n");
        AddressBookStore s2(path);
        WPE_CHECK(!s2.load());
        WPE_CHECK_NOTE(!s2.lastLoadBackupPath().isEmpty(), QStringLiteral("第二份坏文件没有被备份"));
        WPE_CHECK(!QFile::exists(path));
        WPE_CHECK_NOTE(s1.lastLoadBackupPath() != s2.lastLoadBackupPath(), QStringLiteral("两份备份名不应相同"));
        WPE_CHECK(QFile::exists(s1.lastLoadBackupPath()) && QFile::exists(s2.lastLoadBackupPath()));
    }

    // 用 Windows 共享标志确定性禁止改名，但允许读取。占用解除后，任何保存入口
    // 仍必须保护没有备份的原文件；不能只依赖操作系统的暂时锁定来防止数据丢失。
    void TestFailedBackupBlocksAllSavePaths()
    {
        const QByteArray original = "KSWORD-ADDRESS-BOOK 1\nrecoverable-but-invalid-entry\n";
        for (const bool destructOnly : {false, true})
        {
            const QString prefix = destructOnly ? QStringLiteral("w2_backup_destruct") : QStringLiteral("w2_backup_retry");
            RemoveScratchFilesWithPrefix(prefix);
            const QString path = ScratchFilePath(prefix);
            WriteFile(path, original);
            const std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();
            HANDLE lock = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            WPE_CHECK(lock != INVALID_HANDLE_VALUE);
            if (lock == INVALID_HANDLE_VALUE)
            {
                continue;
            }
            {
                AddressBookStore store(path);
                WPE_CHECK(!store.load());
                WPE_CHECK(store.lastLoadBackupPath().isEmpty());
                WPE_CHECK(CloseHandle(lock) != 0);
                store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x8888));
                if (!destructOnly)
                {
                    QSignalSpy failed(&store, &AddressBookStore::saveFailed);
                    WPE_CHECK(!store.flushNow());
                    WPE_CHECK(failed.count() == 1 && store.pendingSave());
                    WaitMs(2200); // 退避后的自动保存同样不能覆盖原文件。
                    WPE_CHECK(failed.count() >= 2 && store.writeCount() == 0);
                }
            }
            QFile preserved(path);
            WPE_CHECK(preserved.open(QIODevice::ReadOnly));
            WPE_CHECK(preserved.readAll() == original);
            preserved.close();
        }

        // 用户把原文件移走后，内存中的编辑仍可以保存，保护状态不会永久封死地址簿。
        const QString path = ScratchFilePath(QStringLiteral("w2_backup_recover"));
        RemoveScratchFilesWithPrefix(QStringLiteral("w2_backup_recover"));
        WriteFile(path, original);
        const std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();
        HANDLE lock = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        WPE_CHECK(lock != INVALID_HANDLE_VALUE);
        if (lock == INVALID_HANDLE_VALUE)
        {
            return;
        }
        AddressBookStore store(path);
        WPE_CHECK(!store.load());
        WPE_CHECK(CloseHandle(lock) != 0);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x7777));
        WPE_CHECK(QFile::rename(path, path + QStringLiteral(".manual-backup")));
        WPE_CHECK(store.flushNow());
        AddressBookStore reloaded(path);
        WPE_CHECK(reloaded.load() && reloaded.size() == 1);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC19：目标目录不存在时 flushNow 要先建目录。
    // ========================================================================================
    void TestMissingDirectoryIsCreated()
    {
        const QString baseDir = QFileInfo(ScratchFilePath(QStringLiteral("probe"))).absolutePath();
        const QString nested = baseDir + QStringLiteral("/w2_nodir/a/b/book.addrbook");
        QDir(baseDir + QStringLiteral("/w2_nodir")).removeRecursively();
        AddressBookStore store(nested);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        WPE_CHECK(store.flushNow());
        WPE_CHECK(QFile::exists(nested));
        QDir(baseDir + QStringLiteral("/w2_nodir")).removeRecursively();
    }

    // ========================================================================================
    // [DEFECT->已修] D1：写失败后"脏标记"丢失——saveFailed 发出后原实现里 pendingSave()
    // 变为 false，析构不会再补写；外部原因消除后（腾出磁盘、解除占用）改动仍然永久丢失。
    // ========================================================================================
    void TestFailedSaveIsRetriedOnDestruction()
    {
        RemoveScratchFilesWithPrefix(QStringLiteral("w2_dirty"));
        const QString path = ScratchFilePath(QStringLiteral("w2_dirty"));
        QDir().mkpath(path);
        {
            AddressBookStore store(path);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1234));
            QSignalSpy failed(&store, &AddressBookStore::saveFailed);
            WaitMs(700);
            WPE_CHECK(failed.count() == 1);
            QDir().rmdir(path);
        }
        WPE_CHECK_NOTE(QFile::exists(path), QStringLiteral("写失败之后的改动在析构时应再试一次落盘"));
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA17b-C18bylen：草稿里的 NUL（add() 直接收草稿，绕过 setNote 的剥除）
    // 不应破坏整份文件；实测当前代码落盘后可正常重载，此前没有任何用例钉住"按长度序列化"
    // 这一半。
    // ========================================================================================
    void TestDraftWithNulDoesNotCorruptFile()
    {
        RemoveScratchFilesWithPrefix(QStringLiteral("w2_nul"));
        const QString path = ScratchFilePath(QStringLiteral("w2_nul"));
        {
            AddressBookStore store(path);
            ksword::memwb::AddressEntry draft = MakeAbsoluteEntry(EntryKind::Bookmark, "t", 0x10);
            draft.note = std::string("ab\0cd", 5);
            store.add(draft);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "t2", 0x20));
            store.flushNow();
        }
        AddressBookStore reloaded(path);
        WPE_CHECK_NOTE(reloaded.load(), reloaded.lastLoadErrorText());
        WPE_CHECK(reloaded.size() == 2);
    }
}

namespace wpe_test
{
    void RunStoreWave2Tests()
    {
        TestI18n_StoreMessagesAreTranslated();
        TestC5_FlushReplacesFileInsteadOfWritingInPlace();
        TestAddManyBasic();
        TestBackupNamesAreUniqueAcrossRepeatedFailures();
        TestFailedBackupBlocksAllSavePaths();
        TestMissingDirectoryIsCreated();
        TestFailedSaveIsRetriedOnDestruction();
        TestDraftWithNulDoesNotCorruptFile();
    }
}
