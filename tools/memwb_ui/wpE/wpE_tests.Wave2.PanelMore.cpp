// wpE_tests.Wave2.PanelMore.cpp
// 作用：第二轮独立审核报告 review2-wpE.md 的 Panel 补测（第二部分，并入默认运行）。覆盖：
// D2（载入失败横幅备份路径必须真的可见，不能被单行控件裁掉）、无选区只有当前格时 Enter
// 仍落在当前格、分段控件重建后恢复当前段与键盘焦点（C9 的"键盘导航失效"那一半）、排序后
// 双击值格预填必须是被点中那一行的值、"复制值"菜单项随值状态变化、复制值闸门要用非空的
// 过期文本才有牙齿、分段悬停提示在重建前后都要有、表头初始状态与窄侧栏列宽、单条增删必须
// 是增量信号而不是 modelReset、分段重建发生在它自身事件处理链路里不得立即销毁旧控件、
// 排序视图下增量插入/删除保持选区与代理顺序正确。
// 来源：Wave 2 第二轮独立审核给出的补测。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QPointer>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>

using ks::ui::AddressBookModel;
using ks::ui::AddressBookPanel;
using ks::ui::AddressBookStore;
using ks::ui::HexViewMessageLabel;
using ks::ui::HexViewSegmented;
using ksword::memwb::EntryKind;
using namespace wpe_test;

namespace
{
    QTableView* FindView(AddressBookPanel& panel) { return panel.findChild<QTableView*>(); }

    // HexViewMessageLabel 没有 Q_OBJECT（见 HexViewWidgets.h），findChild<T*> 用不了，
    // 改用 dynamic_cast 遍历普通 QWidget 子树。
    HexViewMessageLabel* FindMessageLabel(AddressBookPanel& panel)
    {
        for (QWidget* const widget : panel.findChildren<QWidget*>())
        {
            if (HexViewMessageLabel* const label = dynamic_cast<HexViewMessageLabel*>(widget))
            {
                return label;
            }
        }
        return nullptr;
    }

    // FlushDeferredDeletes：把 deleteLater 排进的事件彻底跑完——分段重建后旧控件在事件
    // 循环真正转一圈之前仍是 panel 的子节点，单跑一次 processEvents 不够。
    void FlushDeferredDeletes()
    {
        for (int i = 0; i < 3; ++i)
        {
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC10：TestLoadFailureBanner 原来没有任何断言；这里钉住文案内容本身
    // （带/不带 backupPath 两种情况）。
    // ========================================================================================
    void TestBannerTextIncludesMessageAndBackupPath()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        HexViewMessageLabel* const label = FindMessageLabel(panel);
        WPE_CHECK(label != nullptr);
        if (label == nullptr)
        {
            return;
        }
        panel.showLoadFailure(QStringLiteral("line 3: bad"));
        WPE_CHECK(label->text().contains(QStringLiteral("line 3: bad")));
        WPE_CHECK(!label->text().contains(QStringLiteral("C:/x.bad")));
        panel.showLoadFailure(QStringLiteral("line 3: bad"), QStringLiteral("C:/x.bad"));
        WPE_CHECK(label->text().contains(QStringLiteral("line 3: bad")));
        WPE_CHECK(label->text().contains(QStringLiteral("C:/x.bad")));
    }

    // [DEFECT->已修] D2：横幅原来用 "\n" 把消息与备份路径拼成两行，但 HexViewMessageLabel
    // 是单行高度、单行省略绘制的控件——换行符被当场吃掉下半截，备份路径完全看不见。修复
    // 后文案不换行（lines==1），单行控件的高度天然够用。
    void TestBannerBackupPathIsVisibleNotOnSecondLine()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        panel.resize(560, 200);
        panel.showLoadFailure(QStringLiteral("line 3: bad"), QStringLiteral("C:/x.bad"));
        panel.show();
        QApplication::processEvents();
        HexViewMessageLabel* const label = FindMessageLabel(panel);
        WPE_CHECK(label != nullptr);
        if (label == nullptr)
        {
            panel.hide();
            return;
        }
        const int lines = label->text().count(QLatin1Char('\n')) + 1;
        const int lineHeight = QFontMetrics(label->font()).height();
        WPE_CHECK_NOTE(label->height() >= lines * lineHeight,
            QStringLiteral("横幅文案有 %1 行，但控件高度只有 %2px（单行高 %3px）")
                .arg(lines).arg(label->height()).arg(lineHeight));
        // 内容本身仍要同时包含两句话（不是把备份句丢了来换取"只有一行"）。
        WPE_CHECK(label->text().contains(QStringLiteral("line 3: bad")));
        WPE_CHECK(label->text().contains(QStringLiteral("C:/x.bad")));
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC12：没有选区、只有当前格（方向键移动过焦点但没有真正点选）时，
    // Enter/跳转仍应落在当前格上，不能因为"没有选区"就什么都不做。
    // ========================================================================================
    void TestEnterWithCurrentCellButNoSelection()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
        const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));
        panel.resize(520, 220);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->clearSelection();
        view->selectionModel()->setCurrentIndex(
            view->model()->index(1, AddressBookModel::ColumnAddress), QItemSelectionModel::NoUpdate);
        WPE_CHECK(panel.selectedIds().empty());
        QSignalSpy jump(&panel, &AddressBookPanel::jumpRequested);
        QTest::keyClick(view, Qt::Key_Return);
        WPE_CHECK(jump.count() == 1 && jump.first().first().value<quint64>() == static_cast<quint64>(b));
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC13/NC14：计数变化触发分段控件重建后，当前段与键盘焦点必须保留，且
    // "分段显示"与"模型过滤"不能分家。
    // ========================================================================================
    void TestSegmentRebuildKeepsCurrentSegmentAndFocus()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        panel.resize(520, 220);
        panel.show();
        panel.activateWindow();
        QApplication::processEvents();
        panel.setKindFilterIndex(1);  // 书签
        FlushDeferredDeletes();
        HexViewSegmented* seg = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(seg != nullptr);
        if (seg == nullptr)
        {
            panel.hide();
            return;
        }
        seg->setFocus();
        QApplication::processEvents();
        const bool hadFocus = seg->hasFocus();
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));  // 计数变化 => 重建
        FlushDeferredDeletes();
        seg = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(seg != nullptr);
        if (seg != nullptr)
        {
            WPE_CHECK_NOTE(seg->currentIndex() == 2,
                QStringLiteral("重建后当前段应仍为 书签(2)，实际 %1").arg(seg->currentIndex()));
        }
        WPE_CHECK(panel.kindFilterIndex() == 1);
        WPE_CHECK(model.kindFilter().has_value() && *model.kindFilter() == EntryKind::Bookmark);
        if (hadFocus && seg != nullptr)
        {
            WPE_CHECK_NOTE(seg->hasFocus(), QStringLiteral("重建后应恢复键盘焦点"));
        }
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC16：排序后双击值格打开编辑器，预填的必须是被点中那一行的值（委托对
    // 代理索引换源索引的那一步，提交落点与预填必须分别测，否则一个漏了另一个抓不出来）。
    // ========================================================================================
    void TestValueEditorPrefillAfterSort()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t a = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x9000));
        const std::uint64_t b = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x1000));
        const std::uint64_t c = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x5000));
        model.setValueText(a, QStringLiteral("AAA"), AddressBookModel::ValueState::Read);
        model.setValueText(b, QStringLiteral("BBB"), AddressBookModel::ValueState::Read);
        model.setValueText(c, QStringLiteral("CCC"), AddressBookModel::ValueState::Read);
        panel.resize(520, 220);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->sortByColumn(AddressBookModel::ColumnAddress, Qt::AscendingOrder);  // b c a
        QApplication::processEvents();
        for (int row = 0; row < 3; ++row)
        {
            const QModelIndex valueIndex = view->model()->index(row, AddressBookModel::ColumnValue);
            const quint64 id = view->model()->index(row, 0).data(AddressBookModel::IdRole).value<quint64>();
            view->setCurrentIndex(valueIndex);
            view->edit(valueIndex);
            QLineEdit* const editor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                WPE_CHECK_NOTE(editor->text() == model.valueText(id),
                    QStringLiteral("第 %1 行编辑器预填=%2，应为 %3").arg(row).arg(editor->text(), model.valueText(id)));
                QTest::keyClick(editor, Qt::Key_Escape);
            }
            FlushDeferredDeletes();
        }
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 NC17：值未成功读取时"复制值"菜单项必须置灰；Read 时可点。
    // ========================================================================================
    void TestCopyValueMenuItemTracksValueState()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
        model.setValueText(id, QString(), AddressBookModel::ValueState::Unreadable);
        panel.resize(520, 220);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->selectRow(0);
        WPE_CHECK_NOTE(panel.previewCopyText(AddressBookPanel::CopyField::Value).isEmpty(),
            QStringLiteral("Unreadable 时不应能复制出任何值"));
        model.setValueText(id, QStringLiteral("5"), AddressBookModel::ValueState::Read);
        WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Value) == QStringLiteral("5"));
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA21-S4valueGate：夹具原来的 TestCopyValueSkipsPlaceholderWhenNotRead
    // 用的是（空文本, Unreadable）——空文本本来就返回空串，撤掉 Read 闸门也照样通过。有牙齿
    // 的写法是非空的过期文本（Stale/Reading 带旧值）：不得被当成"当前值"复制出去。
    // ========================================================================================
    void TestCopyValueRejectsStaleNonEmptyText()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
        panel.resize(480, 200);
        panel.show();
        QApplication::processEvents();
        FindView(panel)->selectRow(0);
        const AddressBookModel::ValueState staleStates[] = {
            AddressBookModel::ValueState::Stale, AddressBookModel::ValueState::Reading,
            AddressBookModel::ValueState::Unreadable
        };
        for (const AddressBookModel::ValueState state : staleStates)
        {
            model.setValueText(id, QStringLiteral("old-value"), state);
            WPE_CHECK_NOTE(panel.previewCopyText(AddressBookPanel::CopyField::Value).isEmpty(),
                QStringLiteral("状态 %1 带旧文本时不应复制").arg(static_cast<int>(state)));
        }
        model.setValueText(id, QStringLiteral("old-value"), AddressBookModel::ValueState::Read);
        WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Value) == QStringLiteral("old-value"));
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA13-C13tip：HexViewSegmented 只显示"逐段提示"，整体 setToolTip 从没
    // 真正显示过；夹具里没有任何断言检查过 4 段的悬停提示，重建前后都要有。
    // ========================================================================================
    void TestSegmentTooltipsAreSetForEverySegment()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        HexViewSegmented* const segment = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(segment != nullptr);
        if (segment != nullptr)
        {
            for (int i = 0; i < 4; ++i)
            {
                WPE_CHECK_NOTE(!segment->segmentToolTip(i).isEmpty(), QStringLiteral("第 %1 段没有悬停提示").arg(i));
            }
        }
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        FlushDeferredDeletes();
        HexViewSegmented* const rebuilt = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(rebuilt != nullptr);
        if (rebuilt != nullptr)
        {
            for (int i = 0; i < 4; ++i)
            {
                WPE_CHECK_NOTE(!rebuilt->segmentToolTip(i).isEmpty(), QStringLiteral("重建后第 %1 段没有悬停提示").arg(i));
            }
        }
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA15c/RA22：表头初始不应带排序箭头；图标列应是窄列；A 组下备注列
    // （最后一个可见列）必须落在可视区内，不被挤到屏外。
    // ========================================================================================
    void TestInitialHeaderStateAndColumnWidths()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        panel.resize(300, 220);   // ux 默认侧栏宽度 300
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        WPE_CHECK_NOTE(view->horizontalHeader()->sortIndicatorSection() < 0,
            QStringLiteral("初始排序箭头不应落在任何列，实际 %1").arg(view->horizontalHeader()->sortIndicatorSection()));
        WPE_CHECK_NOTE(view->columnWidth(AddressBookModel::ColumnKindIcon) <= 48,
            QStringLiteral("图标列宽 %1").arg(view->columnWidth(AddressBookModel::ColumnKindIcon)));
        const int noteRight = view->columnViewportPosition(AddressBookModel::ColumnNote)
            + view->columnWidth(AddressBookModel::ColumnNote);
        WPE_CHECK_NOTE(view->columnViewportPosition(AddressBookModel::ColumnNote) < view->viewport()->width(),
            QStringLiteral("备注列起点 %1 超出视口宽 %2（右端 %3）")
                .arg(view->columnViewportPosition(AddressBookModel::ColumnNote)).arg(view->viewport()->width()).arg(noteRight));
        panel.hide();
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA09 的耗时判据缺口：TestC8_BulkAddStaysFast 的墙钟预算对"整表 reset"
    // 的变异也会通过（它只被 C16 的选区断言抓住，不是时间本身）。用信号计数代替墙钟：
    // 单条 add/remove 必须是 rowsInserted/rowsRemoved，不能有任何一次 modelReset。
    // ========================================================================================
    void TestSingleAddRemoveIsIncrementalNotReset()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        std::vector<std::uint64_t> ids;
        for (int i = 0; i < 50; ++i)
        {
            ids.push_back(store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", static_cast<std::uint64_t>(i + 1))));
        }
        WPE_CHECK_NOTE(resets.count() == 0, QStringLiteral("add 触发了 %1 次 modelReset").arg(resets.count()));
        WPE_CHECK(inserted.count() == 50);
        store.remove(ids[10]);
        store.remove(ids[0]);
        WPE_CHECK_NOTE(resets.count() == 0, QStringLiteral("remove 触发了 %1 次 modelReset").arg(resets.count()));
        WPE_CHECK(removed.count() == 2);
        WPE_CHECK(model.rowCount() == 48);
    }

    // ========================================================================================
    // [SURVIVOR] 杀 RA10-C9delete：从分段控件自己的 currentIndexChanged 槛里人为触发一次
    // 计数变化（迫使重建发生在分段控件自身的事件处理链路里），旧控件此刻不得被立即销毁
    // （Qt 文档明确禁止在自身事件处理函数里 delete this）；必须等事件循环转一圈才回收。
    // ========================================================================================
    void TestSegmentRebuildInsideItsOwnSignalDoesNotCrash()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
        panel.resize(520, 220);
        panel.show();
        QApplication::processEvents();
        HexViewSegmented* const seg = panel.findChild<HexViewSegmented*>();
        WPE_CHECK(seg != nullptr);
        if (seg == nullptr)
        {
            panel.hide();
            return;
        }
        QObject::connect(seg, &HexViewSegmented::currentIndexChanged, &panel, [&store](int) {
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 99));
        });
        QPointer<HexViewSegmented> oldSegment(seg);
        QTest::mouseClick(seg, Qt::LeftButton, Qt::NoModifier, seg->segmentRect(3).center());
        WPE_CHECK_NOTE(!oldSegment.isNull(), QStringLiteral("旧分段控件在自身点击调用链里被立即销毁"));
        FlushDeferredDeletes();
        WPE_CHECK(oldSegment.isNull());
        WPE_CHECK(panel.findChild<HexViewSegmented*>() != nullptr);
        panel.hide();
    }

    // ========================================================================================
    // [GUARD] 排序视图中增量插入/删除后：选区、当前格、代理排序顺序必须始终保持正确——防
    // 的是 beginInsertRows 的源行号与 m_rowIds 实际插入位置错位这类回归（当前代码通过）。
    // ========================================================================================
    void TestSortedIncrementalInsertKeepsSelectionAndOrder()
    {
        AddressBookStore store{ QString() };
        AddressBookModel model(&store);
        AddressBookPanel panel(&model);
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x5000));
        store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
        const std::uint64_t c = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x3000));
        panel.resize(520, 260);
        panel.show();
        QApplication::processEvents();
        QTableView* const view = FindView(panel);
        view->sortByColumn(AddressBookModel::ColumnAddress, Qt::AscendingOrder);
        QApplication::processEvents();
        view->selectionModel()->setCurrentIndex(
            view->model()->index(1, AddressBookModel::ColumnAddress),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        const std::uint64_t addresses[] = { 0x2000ULL, 0x9000ULL, 0x100ULL, 0x3500ULL };
        for (const std::uint64_t address : addresses)
        {
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", address));
            QApplication::processEvents();
            std::uint64_t previous = 0;
            for (int row = 0; row < view->model()->rowCount(); ++row)
            {
                const quint64 id = view->model()->index(row, 0).data(AddressBookModel::IdRole).value<quint64>();
                const std::uint64_t value = store.find(id)->absoluteAddress;
                WPE_CHECK(value >= previous);
                previous = value;
            }
            WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == c);
            WPE_CHECK(view->currentIndex().isValid()
                && view->currentIndex().data(AddressBookModel::IdRole).value<quint64>() == c);
        }
        panel.hide();
    }
}

namespace wpe_test
{
    void RunPanelWave2MoreTests()
    {
        TestBannerTextIncludesMessageAndBackupPath();
        TestBannerBackupPathIsVisibleNotOnSecondLine();
        TestEnterWithCurrentCellButNoSelection();
        TestSegmentRebuildKeepsCurrentSegmentAndFocus();
        TestValueEditorPrefillAfterSort();
        TestCopyValueMenuItemTracksValueState();
        TestCopyValueRejectsStaleNonEmptyText();
        TestSegmentTooltipsAreSetForEverySegment();
        TestInitialHeaderStateAndColumnWidths();
        TestSingleAddRemoveIsIncrementalNotReset();
        TestSegmentRebuildInsideItsOwnSignalDoesNotCrash();
        TestSortedIncrementalInsertKeepsSelectionAndOrder();
    }
}
