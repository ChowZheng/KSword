// AddressBookStore.cpp
// 作用：AddressBookStore.h 声明的实现——加载/保存、防抖落盘、按 id 的增删改转发。

#include "AddressBookStore.h"

#include "../../../../shared/evidence/memory_workbench/MemoryAddressBook.h"

#include "../../Internationalization/LanguageManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>

namespace ks::ui
{
    namespace
    {
        // kSaveDebounceMs：防抖窗口（毫秒）。短时间内连续多次增删改只落一次盘。
        constexpr int kSaveDebounceMs = 500;

        // kSaveRetryBackoffMs：修复 D1——写失败后不能悄悄放弃，退避这么久再自动重试一次。
        // 比正常的防抖间隔长得多：失败往往是"文件暂时被占用"一类短暂状况，不必像正常编辑
        // 那样 500 ms 就冲一次，给外部原因（杀软扫描完、索引器让开）留出更现实的消退时间。
        constexpr int kSaveRetryBackoffMs = 2000;

        // FormatLoadError：把 DeserializeResult 拼成界面要求的"第 N 行：原因"文案。
        // 传入：失败结果（ok 必为 false）；传出：提示文本，原因取英文诊断文本（见
        //       MemoryAddressBook.h 的 DeserializeErrorText，是固定的简短英文诊断串，
        //       这里原样附在冒号之后，不翻译——它是面向排障的技术细节，不是独立词条）。
        // 修复 D3：外层模板"第 %1 行：%2"经 ks::i18n::sourceText 翻译——这个键本身已经在
        // 两个语言包里登记过（en-US 是 "Line %1: %2"），此前代码直接用 QStringLiteral 绕开
        // 了语言包，词条因此是个永远查不到的死键，en-US 界面下仍会看到这句中文模板。
        QString FormatLoadError(const ksword::memwb::DeserializeResult& result)
        {
            return ks::i18n::sourceText(QStringLiteral("第 %1 行：%2"))
                .arg(result.errorLine)
                .arg(QString::fromUtf8(result.errorText.data(), static_cast<qsizetype>(result.errorText.size())));
        }
    }

    // 构造：记录文件路径，簿从空开始；定时器懒创建（第一次真正需要防抖时才 new）。
    AddressBookStore::AddressBookStore(QString filePath, QObject* parent)
        : QObject(parent)
        , m_filePath(std::move(filePath))
    {
    }

    // 析构：Qt 会自动删除作为 child 的 m_saveTimer，这里无需手动处理。
    // 修复 C6：若此刻仍有未落盘的改动（防抖定时器还在排队），在对象真正销毁前补一次
    // flushNow()——否则防抖窗口内关闭窗口/退出进程会悄悄丢掉最后那一轮改动，而这恰恰是
    // 最常见的触发时机（用户做完最后一次操作就立刻关窗口）。flushNow 内部会自己停掉定时器，
    // 这里不需要重复处理；写失败时仍会照常 emit saveFailed，上层若已经断开连接也无妨
    // （Qt 的信号发射在没有存活的槛时本来就是安全的空操作）。
    AddressBookStore::~AddressBookStore()
    {
        if (pendingSave())
        {
            flushNow();
        }
    }

    QString AddressBookStore::filePath() const
    {
        return m_filePath;
    }

    bool AddressBookStore::load()
    {
        m_lastLoadFailed = false;
        m_lastLoadErrorText.clear();
        m_lastLoadBackupPath.clear();

        // 空路径：纯内存模式，没有文件可加载，视作成功（簿维持原样）。
        if (m_filePath.isEmpty())
        {
            return true;
        }

        QFile file(m_filePath);
        if (!file.exists())
        {
            // 修复 C5 的一角：正式文件不存在，但同名 ".tmp" 还在——可能是旧版本"先删正式
            // 文件、再改名临时文件"方案在两步之间被打断（崩溃/杀软占用）留下的唯一好副本。
            // 不能像以前一样悄悄把这种情况当成"空簿"：下一次保存会用全新的临时文件名在
            // 同目录写一份正式文件，把这份 .tmp 晾在一边，而且再也没有任何提示——用户只会
            // 发现簿"突然空了"。给出明确的失败提示，让用户先去核实那份 .tmp。
            const QString staleTmpPath = m_filePath + QStringLiteral(".tmp");
            if (QFile::exists(staleTmpPath))
            {
                m_lastLoadFailed = true;
                // 修复 D3：这句整句都在 Store 里（没有可以拆出去的外层模板），直接用
                // ks::i18n::sourceText 包住整句——键已经登记在两个语言包里。
                m_lastLoadErrorText = ks::i18n::sourceText(QStringLiteral(
                    "第 0 行：未找到正式文件，但发现同名临时文件 %1，可能是上一次保存被中断后"
                    "留下的；请手动确认其内容后再处理，这里不会自动删除或覆盖它。"))
                    .arg(staleTmpPath);
                return false;
            }
            // 文件不存在不是错误：等同于一份新的空簿，不触碰 m_book，也不发 reset
            // （没有任何旧内容被替换，没有变化可言）。
            return true;
        }

        // 二进制方式打开：地址簿格式只认 LF，Qt 的文本模式会把 LF 悄悄转成平台换行，
        // 在 Windows 上就是 CRLF，Deserialize 会把它当成"裸回车"拒绝——必须避免。
        if (!file.open(QIODevice::ReadOnly))
        {
            m_lastLoadFailed = true;
            // 修复 D3：同样整句经语言包（这个键也已经登记过，含"第 0 行："前缀）。
            m_lastLoadErrorText = ks::i18n::sourceText(QStringLiteral("第 0 行：无法打开文件（%1）")).arg(file.errorString());
            // 打不开也尝试备份一次：多数情况下（例如文件被其它进程以独占方式打开）改名也会
            // 失败，backupCorruptFile 内部已经处理了"改名失败就什么都不做"，这里不需要
            // 额外判断。
            backupCorruptFile();
            return false;
        }
        const QByteArray raw = file.readAll();
        file.close();

        // 解析到临时簿，成功才整体替换：Deserialize 本身已保证失败路径不碰传入对象，
        // 这里再按同样的原则处理——先解析，成功才赋给 m_book。
        // 修复 D7：传入 Deserialize 的 out 参数是 m_book 的副本（不是默认构造的全新对象）
        // ——Deserialize 的"下一个 id = max(out 载入前的下一个 id, 文件中最大 id + 1)"
        // 承诺指的正是这个 out 自己的计数器；如果传一个全新对象，它的计数器永远是初始值 1，
        // m_book 在 load() 之前已经分配过的 id（例如 load() 前调用方先 add() 了几条）就被
        // 彻底遗忘，文件里恰好出现同一个 id 时会与内存里那条完全无关的旧条目撞号。失败时
        // 这份副本直接在函数退出时丢弃，m_book 本身毫发无伤，与"失败时 out 完全不变"的
        // 契约一致（这里的 out 是副本，契约保护的也是这个副本，m_book 从未暴露给
        // Deserialize）。
        ksword::memwb::MemoryAddressBook loaded = m_book;
        const std::string text(raw.constData(), static_cast<std::size_t>(raw.size()));
        const ksword::memwb::DeserializeResult result =
            ksword::memwb::MemoryAddressBook::Deserialize(text, loaded);
        if (!result.ok)
        {
            m_lastLoadFailed = true;
            m_lastLoadErrorText = FormatLoadError(result);
            // 修复 C4：把解析失败的原文件改名备份，后续任何写操作都只会在原路径新建一份
            // 全新文件，绝不会覆盖这份坏文件（也就不会连带抹掉里面可能还能手工抢救的内容）。
            backupCorruptFile();
            return false;
        }

        m_book = std::move(loaded);
        // 修复 D7 后半：整本被替换，旧的值缓存（AddressBookModel::m_valueCells）可能挂在
        // 一个"id 虽然还在、但其实已经是另一条完全不同的记录"上——只靠"id 是否仍存在"分辨
        // 不出这种情况，必须整个清空；bookReloaded 专门承担这个职责，与下面的 reset()
        // （行结构重建）分开发，顺序在前。
        emit bookReloaded();
        emit reset();
        return true;
    }

    bool AddressBookStore::lastLoadFailed() const
    {
        return m_lastLoadFailed;
    }

    QString AddressBookStore::lastLoadErrorText() const
    {
        return m_lastLoadErrorText;
    }

    QString AddressBookStore::lastLoadBackupPath() const
    {
        return m_lastLoadBackupPath;
    }

    void AddressBookStore::backupCorruptFile()
    {
        const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        QString backupPath = m_filePath + QStringLiteral(".bad-") + timestamp;
        // 修复 D5：时间戳只精确到秒，同一秒内第二次坏文件备份会撞上第一份已经占用的文件名
        // ——QFile::rename 在目标已存在时直接返回 false，第二份坏文件就被当成"备份失败"晾
        // 在原路径上，之后任何写入都会把它覆盖掉。目标名已被占用时追加 "-1"/"-2"/… 后缀，
        // 直到找到一个未占用的名字再改名；上限只是防御性的，正常不会真的循环到那么多次。
        if (QFile::exists(backupPath))
        {
            int suffix = 1;
            QString candidate;
            do
            {
                candidate = backupPath + QStringLiteral("-") + QString::number(suffix);
                ++suffix;
            } while (QFile::exists(candidate) && suffix < 10000);
            backupPath = candidate;
        }
        if (QFile::rename(m_filePath, backupPath))
        {
            m_lastLoadBackupPath = backupPath;
        }
        // 改名失败（文件被占用、权限不足等）：m_lastLoadBackupPath 保持为空，原文件原地
        // 不动；writeAtomic 会阻止后续保存覆盖这份唯一副本，直到成功重载或用户把原文件
        // 移走。内存里的编辑仍保留，写失败继续按原来的退避规则重试。
    }

    std::vector<ksword::memwb::AddressEntry> AddressBookStore::list(
        const ksword::memwb::AddressFilter& filter) const
    {
        return m_book.List(filter);
    }

    std::optional<ksword::memwb::AddressEntry> AddressBookStore::find(const std::uint64_t id) const
    {
        return m_book.Find(id);
    }

    std::size_t AddressBookStore::size() const
    {
        return m_book.Size();
    }

    std::uint64_t AddressBookStore::add(const ksword::memwb::AddressEntry& draft)
    {
        const std::uint64_t newId = m_book.Add(draft);
        if (newId == 0)
        {
            return 0;
        }
        scheduleSave();
        emit entryAdded(static_cast<quint64>(newId));
        return newId;
    }

    std::vector<std::uint64_t> AddressBookStore::addMany(const std::vector<ksword::memwb::AddressEntry>& drafts)
    {
        // 批量新增：逐条调用 Core 的 Add（Core 本身是 O(log n) 的 map 插入，代价不大），
        // 但只在全部完成后调度一次保存、发一次 reset——修复 C8 的关键在于避免 N 次
        // entryAdded 各自触发一次模型整表重建 + 排序代理重排 + kind 分段控件重建。
        std::vector<std::uint64_t> newIds;
        newIds.reserve(drafts.size());
        for (const ksword::memwb::AddressEntry& draft : drafts)
        {
            const std::uint64_t newId = m_book.Add(draft);
            if (newId != 0)
            {
                newIds.push_back(newId);
            }
        }
        if (newIds.empty())
        {
            return newIds;
        }
        scheduleSave();
        emit reset();
        return newIds;
    }

    bool AddressBookStore::remove(const std::uint64_t id)
    {
        if (!m_book.Remove(id))
        {
            return false;
        }
        scheduleSave();
        emit entryRemoved(static_cast<quint64>(id));
        return true;
    }

    std::size_t AddressBookStore::removeMany(const std::vector<std::uint64_t>& ids)
    {
        // removedCount：真正删掉的条目数；id 不存在的那些被 MemoryAddressBook::Remove
        // 悄悄忽略，不计入。
        std::size_t removedCount = 0;
        for (const std::uint64_t id : ids)
        {
            if (m_book.Remove(id))
            {
                ++removedCount;
            }
        }
        if (removedCount == 0)
        {
            return 0;
        }
        scheduleSave();
        emit reset();
        return removedCount;
    }

    bool AddressBookStore::setNote(const std::uint64_t id, QString note)
    {
        // 修复 C18 的另一半：备注本身若含 NUL 字节，std::string 存得下，但
        // serializePersistableBytes 写盘时虽然已经改成按长度拷贝不会在 NUL 处截断整份文件，
        // 落盘后的这一行里却会嵌进去一个不可见的 NUL——下次 Deserialize 对这一行做字段切分
        // 时結果难以预测（取决于 Core 如何处理嵌入 NUL 的 string_view）。备注是用户敲的自由
        // 文本，不该出现 NUL，这里直接过滤掉，不依赖 Core 来兜底。
        note.remove(QChar(u'\0'));
        if (!m_book.SetNote(id, note.toStdString()))
        {
            return false;
        }
        scheduleSave();
        emit entryChanged(static_cast<quint64>(id));
        return true;
    }

    bool AddressBookStore::setValueType(const std::uint64_t id, const ksword::memwb::ValueType valueType)
    {
        if (!m_book.SetValueType(id, valueType))
        {
            return false;
        }
        scheduleSave();
        emit entryChanged(static_cast<quint64>(id));
        return true;
    }

    bool AddressBookStore::setPointerChain(const std::uint64_t id,
        const ksword::memwb::PointerBookmarkDefinition& definition,
        QString moduleName, const std::uint64_t rootRva)
    {
        if (!m_book.SetPointerChain(id, definition, moduleName.toStdString(), rootRva))
        {
            return false;
        }
        scheduleSave();
        emit entryChanged(static_cast<quint64>(id));
        return true;
    }

    bool AddressBookStore::promote(const std::uint64_t id, const ksword::memwb::EntryKind newKind)
    {
        if (!m_book.Promote(id, newKind))
        {
            return false;
        }
        scheduleSave();
        emit entryChanged(static_cast<quint64>(id));
        return true;
    }

    std::size_t AddressBookStore::clearSearchResults()
    {
        // searchIds：先列出全部 Search 条目的 id，再逐个删——List 返回的是副本，
        // 不会在遍历期间被 Remove 影响。
        ksword::memwb::AddressFilter filter;
        filter.kinds.push_back(ksword::memwb::EntryKind::Search);
        std::vector<std::uint64_t> searchIds;
        for (const ksword::memwb::AddressEntry& entry : m_book.List(filter))
        {
            searchIds.push_back(entry.id);
        }
        return removeMany(searchIds);
    }

    bool AddressBookStore::flushNow()
    {
        if (m_filePath.isEmpty())
        {
            return true;
        }
        if (m_saveTimer != nullptr)
        {
            m_saveTimer->stop();
        }
        const bool ok = writeAtomic(serializePersistableBytes());
        if (!ok)
        {
            // 修复 D3：键已登记在两个语言包里（en-US 是 "Write failed: %1"）。
            emit saveFailed(ks::i18n::sourceText(QStringLiteral("写入失败：%1")).arg(m_filePath));
            // 修复 D1：显式调用 flushNow 也会失败（例如权限问题、磁盘满这类一次性原因，
            // 也可能是文件被另一个进程短暂占用）；不能让这次改动因为"用户恰好在此刻手动
            // flush 了一次"就彻底失去重试机会——按退避重新排一次防抖定时器，与
            // onSaveTimerTimeout 失败时的处理方式一致。
            scheduleSaveInternal(kSaveRetryBackoffMs);
        }
        return ok;
    }

    int AddressBookStore::writeCount() const
    {
        return m_writeCount;
    }

    bool AddressBookStore::pendingSave() const
    {
        return m_saveTimer != nullptr && m_saveTimer->isActive();
    }

    void AddressBookStore::scheduleSave()
    {
        scheduleSaveInternal(kSaveDebounceMs);
    }

    void AddressBookStore::scheduleSaveInternal(const int delayMs)
    {
        if (m_filePath.isEmpty())
        {
            return;
        }
        // 懒创建定时器：单次触发（SingleShot），每次调度都重新起计时——这就是"防抖"：
        // 只要操作还在连续发生，落盘就一直被推迟。delayMs 平时是 kSaveDebounceMs（500ms），
        // 写失败后的重试改用更长的 kSaveRetryBackoffMs（见 D1 的修复注释）。
        if (m_saveTimer == nullptr)
        {
            m_saveTimer = new QTimer(this);
            m_saveTimer->setSingleShot(true);
            connect(m_saveTimer, &QTimer::timeout, this, &AddressBookStore::onSaveTimerTimeout);
        }
        m_saveTimer->start(delayMs);
    }

    void AddressBookStore::onSaveTimerTimeout()
    {
        if (writeAtomic(serializePersistableBytes()))
        {
            return;
        }
        // 修复 D3：键已登记在两个语言包里。
        emit saveFailed(ks::i18n::sourceText(QStringLiteral("写入失败：%1")).arg(m_filePath));
        // 修复 D1：单次定时器一旦触发就会变回"未激活"（pendingSave() 据此判断"是否还有
        // 未落盘的改动"），原来的实现在这里什么都不做，于是这次改动既不会自动重试，
        // 析构时 pendingSave() 也已经是 false、不会补写——用户的这次改动就这样彻底丢了，
        // 界面上只有一条一闪而过的 saveFailed 提示。这里按退避重新把定时器排起来：窗口内
        // 若有新的改动会被 scheduleSave() 的正常调用顶掉（符合预期）；没有的话到点后本函数
        // 会再试一次，直到写成功——写成功的那一次会让 pendingSave() 变回 false，不会无限
        // 重试下去。
        scheduleSaveInternal(kSaveRetryBackoffMs);
    }

    QByteArray AddressBookStore::serializePersistableBytes() const
    {
        // 只持久化 Bookmark / Watch，不持久化 Search（可疑点 #1）：MemoryAddressBook.h
        // 头注释本就建议"不保存临时的搜索结果"，否则每一轮搜索灌入结果后都会每 500ms
        // 整文件重写一次，下次启动还会把上一轮已经失效的进程的搜索结果当成地址簿内容载回来。
        ksword::memwb::AddressFilter filter;
        filter.kinds.push_back(ksword::memwb::EntryKind::Bookmark);
        filter.kinds.push_back(ksword::memwb::EntryKind::Watch);
        const std::string serialized = m_book.Serialize(filter);
        // 修复 C18：直接按长度构造 QByteArray，不经过 QString::fromUtf8(std::string::c_str())
        // ——c_str() 在遇到备注里嵌入的 NUL 字节时会被当成"字符串结束"，整份文件从那里被
        // 悄悄砍断，而且剩下的半截不以 LF 结尾，下次加载会连同前面全部合法的条目一起判定
        // 为损坏（对应 C4 的覆盖链条：载入失败 -> 下一次写就覆盖了唯一的好副本）。
        return QByteArray(serialized.data(), static_cast<qsizetype>(serialized.size()));
    }

    bool AddressBookStore::writeAtomic(const QByteArray& bytes)
    {
        if (bytes.startsWith("KSWORD-ADDRESS-BOOK 2\n")
            && static_cast<std::size_t>(bytes.size()) > ksword::memwb::kAddressBookV2TextLimit)
        {
            return false;
        }
        // 载入失败且备份失败时，原文件可能仍含能手工恢复的数据。即使导致改名失败的
        // 临时占用已经解除，也不能让防抖保存或析构保存把这份唯一副本覆盖掉。
        if (m_lastLoadFailed && m_lastLoadBackupPath.isEmpty() && QFile::exists(m_filePath))
        {
            return false;
        }

        // 目标目录不存在时先创建（例如首次运行、用户自定义了一个新目录）。
        const QFileInfo info(m_filePath);
        const QDir dir = info.dir();
        if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        {
            return false;
        }

        // 修复 C5："先删正式文件、再把临时文件改名过去"这两步之间存在一个正式文件完全
        // 不存在的窗口：改名失败（Windows 上杀软/索引器短暂占住刚写完的临时文件会
        // ACCESS_DENIED/SHARING_VIOLATION）或进程在两步之间被杀，正式文件就凭空消失了，
        // 而 load() 把"文件不存在"当成合法的空簿、不会去看 .tmp，下一次保存还会用同一个
        // 临时文件名 Truncate 打开并覆盖掉那份唯一的好副本。QSaveFile 把"写临时文件"与
        // "原子替换正式文件"这两步都交给 Qt：临时文件用随机后缀、与正式文件同目录，
        // commit() 内部在 Windows 上通过可以处理"目标已存在"的原子替换调用完成切换，
        // 不会出现"正式文件暂时不存在"的中间状态。
        QSaveFile saveFile(m_filePath);
        // 二进制方式写入：地址簿格式只认 LF，必须避免 Qt 文本模式在 Windows 上转成 CRLF。
        if (!saveFile.open(QIODevice::WriteOnly))
        {
            return false;
        }
        const qint64 written = saveFile.write(bytes);
        if (written != static_cast<qint64>(bytes.size()))
        {
            // 写入长度不对（例如磁盘写满，返回值小于请求长度）：取消这次提交，
            // QSaveFile 会自己清理掉还没提交的临时文件，不会在同目录留下垃圾。
            saveFile.cancelWriting();
            return false;
        }
        if (!saveFile.commit())
        {
            return false;
        }
        ++m_writeCount;
        return true;
    }
}
