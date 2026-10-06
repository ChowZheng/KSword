// wpE_tests.Wave2.NewMutations.cpp
// 作用：本轮（wave2）主会话自己补的新变异回归，覆盖审核报告列出的变异之外、这一轮修复
// 新写的代码里仍然缺牙齿的地方——都是"行为判断，不同于前两轮"：
// - D1：写失败后的退避重试必须自己到点就成功，不依赖对象恰好在这之前被销毁。
// - D4：运行期切换语言必须刷新自绘的 kind 分段文字。
// - D6（两部分）：隐藏条目的编辑不该触发整表 reset；批量删除之后存活的选区要能恢复。
// - D7（两部分）：load() 整本替换后旧值缓存必须整个清空；下一个 id 必须带着载入前的
//   计数器走，不会被重置回文件自己的号段。
// - D8：Ctrl+C 多选复制的行序是视觉顺序，不是点选顺序。
// - D9：Store 先于 Model 销毁时，Model 要主动清空"幽灵行"。
// - D10：多选范围内右键某一行，当前格要跟过去，单行动作落在被右键的行上。
// - D12：单条 add() 改成增量计数后，四个计数在混合 kind 的连续添加下仍必须精确正确，
//   且全程不应触发任何一次 modelReset。
// 10 个测试，超过任务要求的"至少 8 个新变异"。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/Internationalization/LanguageManager.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QItemSelectionModel>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <memory>

using ks::ui::AddressBookModel;
using ks::ui::AddressBookPanel;
using ks::ui::AddressBookStore;
using ks::ui::HexViewSegmented;
using ksword::memwb::EntryKind;
using namespace wpe_test;

namespace
{
    QTableView* FindView(AddressBookPanel& panel) { return panel.findChild<QTableView*>(); }

    bool HasHan(const QString& text)
    {
        for (const QChar ch : text)
        {
            if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF)
            {
                return true;
            }
        }
        return false;
    }

    // ClosePopupSoon：右键菜单 menu.exec() 是模态调用，需要一个定时器异步把它关掉才能让
    // 测试函数继续往下走；AddressBookPanel::showRowContextMenu 的"移动当前格"这个副作用
    // 在 exec() 之前就已经同步发生，不需要等菜单弹出来再操作。
    void ClosePopupSoon()
    {
        QTimer* const closer = new QTimer();
        closer->setInterval(20);
        QObject::connect(closer, &QTimer::timeout, closer, [closer]() {
            if (QWidget* const popup = QApplication::activePopupWidget())
            {
                popup->close();
                closer->stop();
                closer->deleteLater();
            }
        });
        closer->start();
    }

    // ========================================================================================
    // D1 新变异：退避重试必须自己到点就成功，不依赖"对象恰好在这之前被销毁"——
    // suggested_tests2.cpp 的 TestFailedSaveIsRetriedOnDestruction 只验证了析构路径，这里
    // 验证对象还活着的情况下，退避定时器自己到点触发第二次写入并成功。
    // ========================================================================================
    void TestD1_RetryWithoutDestructionEventuallySucceeds()
    {
        const QString dirPath = QStringLiteral(".codex-tmp/memwb-wpE/scratch");
        QDir().mkpath(dirPath);
        const QString path = QDir(dirPath).filePath(QStringLiteral("w2mut_retry.addrbook"));
        QFile::remove(path);
        QDir(path).removeRecursively();
        QDir().mkpath(path);  // 先把路径占成目录，制造写失败。

        AddressBookStore store(path);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        QSignalSpy failed(&store, &AddressBookStore::saveFailed);
        WaitMs(700);  // 正常 500ms 防抖到期，第一次写失败。
        WPE_CHECK(failed.count() == 1);
        WPE_CHECK_NOTE(store.pendingSave(), QStringLiteral("写失败后应该重新排一次退避重试"));
        QDir().rmdir(path);  // 腾出路径：这次重试应该能真正落盘成功。
        WaitMs(2300);  // kSaveRetryBackoffMs=2000，留足余量等它自己触发。
        WPE_CHECK_NOTE(QFile::exists(path), QStringLiteral("退避重试到点后应该已经成功落盘"));
        WPE_CHECK(store.writeCount() == 1);
        WPE_CHECK(!store.pendingSave());
        QFile::remove(path);
    }

    // ========================================================================================
    // D4 新变异：运行期切换语言必须刷新自绘的 kind 分段文字——LanguageManager 的运行期
    // 遍历够不到 HexViewSegmented（它不是 QLabel/QAbstractButton），必须靠 changeEvent
    // 主动重算。
    // ========================================================================================
    void TestD4_LanguageChangeRefreshesSegmentLabels()
    {
        QString err;
        ks::i18n::LanguageManager::instance().initialize(QStringLiteral("zh-CN"), &err);
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);

        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        AddressBookPanel panel(&model);
        panel.show();
        QApplication::processEvents();

        HexViewSegmented* segment = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(segment != nullptr);
        if (segment != nullptr)
        {
            WPE_CHECK_NOTE(HasHan(segment->labelAt(0)), QStringLiteral("zh-CN 下第 0 段应含汉字：%1").arg(segment->labelAt(0)));
        }

        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("en-US"), &err);
        QApplication::processEvents();

        segment = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(segment != nullptr);
        if (segment != nullptr)
        {
            WPE_CHECK_NOTE(!HasHan(segment->labelAt(0)),
                QStringLiteral("切到 en-US 后第 0 段不应再含汉字：%1").arg(segment->labelAt(0)));
        }
        panel.hide();
        ks::i18n::LanguageManager::instance().setLanguage(QStringLiteral("zh-CN"), &err);
    }

    // ========================================================================================
    // D6 第一部分新变异：过滤隐藏的条目被编辑（备注）时，可见行集合其实没有任何变化
    // （row<0 且 shouldBeVisible 仍是 false），不应该整表 reset、不应该清掉用户在当前可见
    // 分段里的选区。
    // ========================================================================================
    void TestD6a_HiddenEntryEditDoesNotResetVisibleSelection()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t visibleBookmark = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        const std::uint64_t hiddenWatch = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 2));
        model.setKindFilter(EntryKind::Bookmark);
        panel.resize(480, 200);
        panel.show();
        QApplication::processEvents();
        FindView(panel)->selectRow(0);
        WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == visibleBookmark);

        QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
        store.setNote(hiddenWatch, QStringLiteral("edit on a hidden row"));
        WPE_CHECK_NOTE(resetSpy.count() == 0, QStringLiteral("隐藏条目的编辑不应触发整表 reset"));
        WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == visibleBookmark);
        panel.hide();
    }

    // ========================================================================================
    // D6 第二部分新变异：批量删除（走 reset()）之后，没被这次删除动到的条目的选区应该能
    // 按 id 恢复，不因为"整表重建过一次"就被清空。
    // ========================================================================================
    void TestD6b_BatchRemoveRestoresSurvivingSelection()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t a = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
        const std::uint64_t c = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
        panel.resize(480, 200);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->selectRow(1);
        const QPoint posC = view->visualRect(view->model()->index(2, AddressBookModel::ColumnAddress)).center();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, posC);
        WPE_CHECK(panel.selectedIds().size() == 2);

        store.removeMany({ a });  // 走 reset()，与 B/C 的选区无关。
        QApplication::processEvents();
        const std::vector<std::uint64_t> survivingSelection = panel.selectedIds();
        WPE_CHECK_NOTE(survivingSelection.size() == 2,
            QStringLiteral("批量删除别的行之后，B/C 的选区应该被恢复，实际剩 %1 条").arg(survivingSelection.size()));
        WPE_CHECK(std::find(survivingSelection.begin(), survivingSelection.end(), b) != survivingSelection.end());
        WPE_CHECK(std::find(survivingSelection.begin(), survivingSelection.end(), c) != survivingSelection.end());
        panel.hide();
    }

    // ========================================================================================
    // D7 第一部分新变异：load() 整本替换后，旧值缓存必须整个清空——即便文件里恰好也有一条
    // 相同 id 的记录（号段复用/文件独立编号），也不能让旧值显示到这条新记录上。
    // ========================================================================================
    void TestD7a_ReloadWipesStaleValueCacheOnIdReuse()
    {
        const QString dirPath = QStringLiteral(".codex-tmp/memwb-wpE/scratch");
        QDir().mkpath(dirPath);
        const QString path = QDir(dirPath).filePath(QStringLiteral("w2mut_reload.addrbook"));
        QFile::remove(path);

        AddressBookStore store(path);
        AddressBookModel model(&store);
        const std::uint64_t id1 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
        WPE_CHECK(id1 == 1);
        model.setValueText(id1, QStringLiteral("before-reload"), AddressBookModel::ValueState::Read);
        WPE_CHECK(store.flushNow());

        // 直接手写一份"另一个会话"的文件：同样分配 id=1，但目标/地址/备注完全不同。
        QFile file(path);
        WPE_CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("KSWORD-ADDRESS-BOOK 1\n1\tbookmark\thex8\tother-target\t\t0x0\t0x2000\tfrom-another-session\n");
        file.close();

        WPE_CHECK(store.load());
        WPE_CHECK_NOTE(model.valueText(id1).isEmpty(),
            QStringLiteral("load() 整本替换后旧值缓存必须清空，实际仍是 %1").arg(model.valueText(id1)));
        WPE_CHECK(model.valueState(id1) == AddressBookModel::ValueState::NotRead);
        QFile::remove(path);
    }

    // ========================================================================================
    // D7 第二部分新变异：下一个 id 必须带着载入前的计数器走——load() 不能把它重置回
    // "文件里最大 id + 1"，否则载入前已经分配过的 id 会被后续新增重新分配出去。
    // ========================================================================================
    void TestD7b_NextIdStaysMonotonicAcrossReload()
    {
        const QString dirPath = QStringLiteral(".codex-tmp/memwb-wpE/scratch");
        QDir().mkpath(dirPath);
        const QString path = QDir(dirPath).filePath(QStringLiteral("w2mut_nextid.addrbook"));
        QFile::remove(path);

        AddressBookStore store(path);
        const std::uint64_t id1 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));  // 落盘。
        WPE_CHECK(store.flushNow());
        const std::uint64_t id2 = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 2));    // 不落盘，但占了 id=2。
        WPE_CHECK(id1 == 1 && id2 == 2);

        WPE_CHECK(store.load());  // 文件里只有 id=1；若 nextId 被重置成 fileMax+1=2 就会复用 2。
        const std::uint64_t id3 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
        WPE_CHECK_NOTE(id3 == 3, QStringLiteral("load() 后下一个 id 应为 3（延续载入前的计数器），实际 %1").arg(id3));
        QFile::remove(path);
    }

    // ========================================================================================
    // D8 新变异：Ctrl+C 多选复制的行序必须是视觉顺序（按屏幕上从上到下），不是依次点选的
    // 顺序——先点第 2 行、再 Ctrl+点第 0 行、再 Ctrl+点第 1 行，selectedIds() 必须仍按
    // 第 0/1/2 行的顺序返回。
    // ========================================================================================
    void TestD8_CopyOrderFollowsVisualRowNotClickOrder()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        // 注意：id 分配顺序（1,2,3）恰好与插入顺序一致，默认（未排序）视图下行号顺序与
        // id 顺序重合——"按行号排序"和"按 id 排序"在这种数据下会得到相同结果，测不出区别
        // （踩过一次：mutrun.ps1 实测把排序依据从"行号"改成"id" 之后这条测试仍然 SURVIVED）。
        // 必须让行号顺序与 id 顺序不一致才有牙齿：这里对地址列降序排序，行号顺序因此变成
        // id 的反序。
        const std::uint64_t idRow0 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
        const std::uint64_t idRow1 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));
        const std::uint64_t idRow2 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x3000));
        panel.resize(480, 200);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->sortByColumn(AddressBookModel::ColumnAddress, Qt::DescendingOrder);
        QApplication::processEvents();
        // 降序后：视觉第 0 行 = idRow2(0x3000)，第 1 行 = idRow1(0x2000)，第 2 行 = idRow0。
        const QPoint pos0 = view->visualRect(view->model()->index(0, AddressBookModel::ColumnAddress)).center();
        const QPoint pos1 = view->visualRect(view->model()->index(1, AddressBookModel::ColumnAddress)).center();
        const QPoint pos2 = view->visualRect(view->model()->index(2, AddressBookModel::ColumnAddress)).center();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, pos2);      // 先点视觉第 2 行（idRow0）
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, pos0); // 再点视觉第 0 行（idRow2）
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, pos1); // 再点视觉第 1 行（idRow1）
        const std::vector<std::uint64_t> ids = panel.selectedIds();
        WPE_CHECK(ids.size() == 3);
        // 视觉顺序（第 0/1/2 行）= idRow2, idRow1, idRow0——与点选顺序（idRow0,idRow2,idRow1）
        // 和 id 升序（idRow0,idRow1,idRow2）都不一样，三者都不同才真正测到"按行号"这件事。
        WPE_CHECK_NOTE(ids == (std::vector<std::uint64_t>{ idRow2, idRow1, idRow0 }),
            QStringLiteral("selectedIds 应按视觉行序，不是点选顺序也不是 id 顺序"));
        panel.hide();
    }

    // ========================================================================================
    // D9 新变异：Store 先于 Model 销毁时，Model 必须主动清空"幽灵行"——rowCount/计数都
    // 应归零，不留着指向已销毁对象的旧行。
    // ========================================================================================
    void TestD9_StoreDestroyedClearsModelRowsAndCounts()
    {
        std::unique_ptr<AddressBookStore> store = std::make_unique<AddressBookStore>(QString());
        AddressBookModel model(store.get());
        store->add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        store->add(MakeAbsoluteEntry(EntryKind::Watch, "p", 2));
        WPE_CHECK(model.rowCount() == 2);
        WPE_CHECK(model.kindCounts().all == 2);

        store.reset();  // 同步触发 QObject::destroyed -> AddressBookModel::onStoreDestroyed。

        WPE_CHECK_NOTE(model.rowCount() == 0, QStringLiteral("Store 销毁后应整表清空，实际 %1 行").arg(model.rowCount()));
        WPE_CHECK(model.kindCounts().all == 0);
    }

    // ========================================================================================
    // D10 新变异：多选范围内右键某一行，当前格要挪到被右键的行上（选区整体不变）——单行
    // 动作（复制地址）应落在被右键的行，而不是右键之前停留的那一行。
    // ========================================================================================
    void TestD10_RightClickWithinMultiSelectionMovesCurrentToClickedRow()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));
        const std::uint64_t idRow2 = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x3000));
        panel.resize(480, 200);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->selectAll();
        WPE_CHECK(panel.selectedIds().size() == 3);
        view->selectionModel()->setCurrentIndex(
            view->model()->index(0, AddressBookModel::ColumnAddress), QItemSelectionModel::NoUpdate);

        const QPoint posRow2 = view->visualRect(view->model()->index(2, AddressBookModel::ColumnAddress)).center();
        ClosePopupSoon();
        emit view->customContextMenuRequested(posRow2);

        WPE_CHECK_NOTE(panel.selectedIds().size() == 3, QStringLiteral("整个选区不应被右键改变"));
        WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Address) == QStringLiteral("0x3000"));
        // 本测试没有排序/过滤，代理行号与源模型行号恰好一致，这里仍改用源模型自己构造的
        // 索引（不是直接拿代理索引当源索引用），避免把"巧合一致"误当成正确的换算方式。
        const int currentRow = view->currentIndex().row();
        WPE_CHECK_NOTE(model.idAt(model.index(currentRow, 0)) == idRow2, QStringLiteral("当前格应落在被右键的行上"));
        panel.hide();
    }

    // ========================================================================================
    // D12 新变异：单条 add() 改成增量计数之后，混合 kind 连续添加时四个计数仍必须精确
    // 正确，且全程不应触发任何一次 modelReset（确认走的真的是增量路径，不是悄悄退回整表
    // 重算）。
    // ========================================================================================
    void TestD12_IncrementalAddCountsMatchExpectedAfterMixedKinds()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        const EntryKind sequence[] = {
            EntryKind::Search, EntryKind::Bookmark, EntryKind::Bookmark,
            EntryKind::Watch, EntryKind::Search, EntryKind::Watch, EntryKind::Watch
        };
        int expectedAll = 0;
        int expectedSearch = 0;
        int expectedBookmark = 0;
        int expectedWatch = 0;
        for (const EntryKind kind : sequence)
        {
            store.add(MakeAbsoluteEntry(kind, "p", static_cast<std::uint64_t>(++expectedAll)));
            switch (kind)
            {
            case EntryKind::Search: ++expectedSearch; break;
            case EntryKind::Bookmark: ++expectedBookmark; break;
            case EntryKind::Watch: ++expectedWatch; break;
            }
            const AddressBookModel::KindCounts counts = model.kindCounts();
            WPE_CHECK_NOTE(counts.all == expectedAll && counts.search == expectedSearch
                    && counts.bookmark == expectedBookmark && counts.watch == expectedWatch,
                QStringLiteral("第 %1 次 add 后计数应为 all=%2 search=%3 bookmark=%4 watch=%5，实际 all=%6 search=%7 bookmark=%8 watch=%9")
                    .arg(expectedAll).arg(expectedAll).arg(expectedSearch).arg(expectedBookmark).arg(expectedWatch)
                    .arg(counts.all).arg(counts.search).arg(counts.bookmark).arg(counts.watch));
        }
        WPE_CHECK_NOTE(resets.count() == 0, QStringLiteral("连续单条 add 不应触发任何一次 modelReset，实际 %1 次").arg(resets.count()));
    }
}

namespace wpe_test
{
    void RunWave2NewMutationTests()
    {
        TestD1_RetryWithoutDestructionEventuallySucceeds();
        TestD4_LanguageChangeRefreshesSegmentLabels();
        TestD6a_HiddenEntryEditDoesNotResetVisibleSelection();
        TestD6b_BatchRemoveRestoresSurvivingSelection();
        TestD7a_ReloadWipesStaleValueCacheOnIdReuse();
        TestD7b_NextIdStaysMonotonicAcrossReload();
        TestD8_CopyOrderFollowsVisualRowNotClickOrder();
        TestD9_StoreDestroyedClearsModelRowsAndCounts();
        TestD10_RightClickWithinMultiSelectionMovesCurrentToClickedRow();
        TestD12_IncrementalAddCountsMatchExpectedAfterMixedKinds();
    }
}
