// wpE_tests.Panel.Survivors.cpp
// 作用：审核报告 review-wpE.md §5.2 列出的"幸存变异补测"——排序后键盘/菜单动作必须落在
// 正确的条目上（sourceIndexForProxy 真的换过索引）、A/B 列组按钮的选中态在各条路径下都
// 正确、kind 过滤索引编码往返、右键菜单的剩余分支（右键未选中行先选中它、无选区 Del 不发
// 信号、双击跳转、值类型子菜单只改类型不碰备注）。这些用例此前完全没有被覆盖，审核报告的
// 29 个副本变异里有 10 个正是靠"这里缺一个断言"才幸存下来的。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTest>
#include <QTimer>

#include <functional>

namespace wpe_test
{
    namespace
    {
        using ks::ui::AddressBookModel;
        using ks::ui::AddressBookPanel;
        using ks::ui::AddressBookStore;
        using ks::ui::HexViewSegmented;
        using ksword::memwb::EntryKind;
        using ksword::memwb::ValueType;

        // FindView / FindButton：与 wpE_tests.Panel.cpp 里的同名函数做一样的事（借助父子树
        // 查找定位内部控件），本文件单独复制一份而不是跨 .cpp 共享——这些是匿名命名空间
        // 内部的小工具，没有导出的必要，重复几行比新增一份公共头文件简单。
        QTableView* FindView(AddressBookPanel& panel)
        {
            return panel.findChild<QTableView*>();
        }

        QPushButton* FindButton(AddressBookPanel& panel, const QString& text)
        {
            for (QPushButton* const button : panel.findChildren<QPushButton*>())
            {
                if (button->text() == text)
                {
                    return button;
                }
            }
            return nullptr;
        }

        // TriggerMenuActionByText：驱动一次会弹出模态 QMenu 的调用，轮询找到后按文本定位
        // QAction 并 trigger()，再关闭菜单；与 wpE_tests.Panel.cpp 里的同名函数做法一致。
        bool TriggerMenuActionByText(const std::function<void()>& openMenu, const QString& actionText)
        {
            bool handled = false;
            int pollCount = 0;
            QTimer poller;
            poller.setInterval(20);
            QObject::connect(&poller, &QTimer::timeout, &poller, [&]() {
                ++pollCount;
                QMenu* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (menu != nullptr)
                {
                    for (QAction* const action : menu->actions())
                    {
                        if (action->text().startsWith(actionText))
                        {
                            action->trigger();
                            handled = true;
                            break;
                        }
                    }
                    menu->close();
                    poller.stop();
                    return;
                }
                if (pollCount >= 50)
                {
                    if (QWidget* const popup = QApplication::activePopupWidget())
                    {
                        popup->close();
                    }
                    poller.stop();
                }
            });
            poller.start();
            openMenu();
            return handled;
        }

        // 杀 wpE-P1（最要紧的一个）：排序后 Enter/F2 备注/值编辑全部必须落在"视图当前行"
        // 对应的条目上，不能假设行号等于插入顺序——这正是 sourceIndexForProxy 必须真的
        // mapToSource 的回归点，之前的用例都只有 1~2 行、排序前后行号相同，看不出区别。
        void TestKeyboardActionsLandOnRightEntryAfterSort()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t idA = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x9000));
            const std::uint64_t idB = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x1000));
            const std::uint64_t idC = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x5000));
            model.setValueText(idC, QStringLiteral("33"), AddressBookModel::ValueState::Read);
            panel.resize(520, 220);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            view->sortByColumn(AddressBookModel::ColumnAddress, Qt::AscendingOrder);
            QApplication::processEvents();
            // 升序：0x1000(idB) 0x5000(idC) 0x9000(idA)。选第 1 行 = idC。
            view->selectRow(1);
            view->setCurrentIndex(view->model()->index(1, AddressBookModel::ColumnAddress));

            QSignalSpy jump(&panel, &AddressBookPanel::jumpRequested);
            QTest::keyClick(view, Qt::Key_Return);
            WPE_CHECK(jump.count() == 1 && jump.first().first().value<quint64>() == static_cast<quint64>(idC));
            WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Address) == QStringLiteral("0x5000"));

            QTest::keyClick(view, Qt::Key_F2);  // 备注编辑应该落在 idC。
            QLineEdit* noteEditor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(noteEditor != nullptr);
            if (noteEditor != nullptr)
            {
                noteEditor->setText(QStringLiteral("only-C"));
                QTest::keyClick(noteEditor, Qt::Key_Return);
                WaitMs(50);
            }
            WPE_CHECK(store.find(idC)->note == "only-C");
            WPE_CHECK(store.find(idA)->note.empty() && store.find(idB)->note.empty());

            QSignalSpy valueEdit(&panel, &AddressBookPanel::valueEditRequested);  // 值编辑应该落在 idC。
            const QModelIndex valueIndex = view->model()->index(1, AddressBookModel::ColumnValue);
            view->setCurrentIndex(valueIndex);
            view->edit(valueIndex);
            QLineEdit* valueEditor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(valueEditor != nullptr);
            if (valueEditor != nullptr)
            {
                valueEditor->setText(QStringLiteral("44"));
                QTest::keyClick(valueEditor, Qt::Key_Return);
                WaitMs(50);
            }
            WPE_CHECK(valueEdit.count() == 1 && valueEdit.first().at(0).value<quint64>() == static_cast<quint64>(idC));
            panel.hide();
        }

        // 杀 wpE-P2：A/B 钮选中态——默认 A 选中、切 B 后 B 选中、改列显隐变成自定义后两者
        // 都不选中，再切回 A 又恢复选中；表头菜单改列同样要让两者都不选中。之前的用例
        // 只断言了 columnGroup() 的枚举值，从没查过按钮本身的 isChecked()。
        void TestColumnGroupButtonsCheckedState()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            panel.resize(600, 220);
            panel.show();
            QApplication::processEvents();
            QPushButton* const buttonA = FindButton(panel, QStringLiteral("A"));
            QPushButton* const buttonB = FindButton(panel, QStringLiteral("B"));
            WPE_CHECK(buttonA != nullptr && buttonB != nullptr);
            if (buttonA == nullptr || buttonB == nullptr)
            {
                return;
            }
            WPE_CHECK(buttonA->isChecked() && !buttonB->isChecked());
            panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetB);
            WPE_CHECK(!buttonA->isChecked() && buttonB->isChecked());
            panel.setHiddenColumns(QList<int>{ AddressBookModel::ColumnTarget });
            WPE_CHECK(!buttonA->isChecked() && !buttonB->isChecked());
            panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetA);
            WPE_CHECK(buttonA->isChecked() && !buttonB->isChecked());
            QHeaderView* const header = FindView(panel)->horizontalHeader();
            WPE_CHECK(TriggerMenuActionByText(
                [header]() { emit header->customContextMenuRequested(QPoint(10, 10)); }, QStringLiteral("地址")));
            WPE_CHECK(!buttonA->isChecked() && !buttonB->isChecked());
            panel.hide();
        }

        // FlushDeferredDeletes：彻底处理掉所有待处理的 deleteLater() 调用。
        // 原因：HexViewSegmented 重建走的是 removeWidget + deleteLater（见 C9 的修复——
        // 不能在控件自己的事件处理链路里同步 delete this）；旧控件在"下一轮事件循环"前
        // 仍然是 panel 的子对象。真实使用中，两次独立的用户操作之间天然隔着事件循环，
        // deleteLater 排到的队总会被清空；但测试里背靠背快速调用多次 add（不像用户单击
        // 那样每次都等一轮事件循环），一次 QApplication::processEvents() 不足以处理完
        // 全部排队的 DeferredDelete 事件（Qt 按"调用 deleteLater 时的循环深度"决定何时
        // 真正处理，需要再多跑几轮才会清空）。这里连续处理几次，确保调用方之后
        // findChild<HexViewSegmented*>() 摸到的一定是当前唯一存活的那个，不是某个旧的
        // 还没真正销毁的实例。
        void FlushDeferredDeletes()
        {
            for (int i = 0; i < 3; ++i)
            {
                QApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
        }

        // 杀 wpE-P3 / wpE-P4：kind 过滤索引编码往返（-1/0/1/2 与分段下标的换算）+ 分段文字
        // 带的计数——之前全文件没有一处断言过 setKindFilterIndex/kindFilterIndex，分段文字
        // 更是从未核对过。
        void TestKindIndexRoundTripAndSegmentLabels()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 4));
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 5));
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 6));
            FlushDeferredDeletes();
            for (const int index : { -1, 0, 1, 2, -1, 2, 0 })
            {
                panel.setKindFilterIndex(index);
                WPE_CHECK_NOTE(panel.kindFilterIndex() == index, QString::number(index));
                const std::optional<EntryKind> expected =
                    index < 0 ? std::optional<EntryKind>{} : std::optional<EntryKind>{ static_cast<EntryKind>(index) };
                WPE_CHECK_NOTE(model.kindFilter() == expected, QString::number(index));
            }
            HexViewSegmented* const segment = panel.findChild<HexViewSegmented*>();
            WPE_CHECK(segment != nullptr);
            if (segment != nullptr)
            {
                WPE_CHECK(segment->labelAt(0) == QStringLiteral("全部(6)"));
                WPE_CHECK(segment->labelAt(1) == QStringLiteral("搜索(1)"));
                WPE_CHECK(segment->labelAt(2) == QStringLiteral("书签(2)"));
                WPE_CHECK(segment->labelAt(3) == QStringLiteral("监视(3)"));
            }
        }

        // 杀 wpE-P5 / wpE-P6 / wpE-P7 / wpE-P8 / wpE-P10：菜单与键盘的剩余分支——右键一个
        // 未选中的行必须先选中它、菜单动作作用于被右键的行而不是旧选区；"加入监视"真的是
        // Watch 不是 Bookmark；无选区时 Del 不发信号；双击"地址"列等于跳转；值类型子菜单
        // 只改类型、不碰备注。
        void TestMenuWiringAndRightClickSelection()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t idA = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 0x1000));
            const std::uint64_t idB = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 0x2000));
            const std::uint64_t idC = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 0x3000));
            panel.resize(560, 260);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            QAbstractItemModel* const proxy = view->model();

            // P6：右键一个"未选中"的行，必须先选中它；菜单动作作用于它而不是旧选区。
            view->selectRow(0);
            QSignalSpy promote(&panel, &AddressBookPanel::promoteRequested);
            const QPoint onRow2 = view->visualRect(proxy->index(2, AddressBookModel::ColumnAddress)).center();
            WPE_CHECK(TriggerMenuActionByText(
                [view, onRow2]() { emit view->customContextMenuRequested(onRow2); }, QStringLiteral("加入监视")));
            WPE_CHECK(promote.count() == 1);
            if (promote.count() == 1)
            {
                WPE_CHECK(promote.first().at(0).value<quint64>() == static_cast<quint64>(idC));  // 落在被右键的行。
                WPE_CHECK(static_cast<EntryKind>(promote.first().at(1).toInt()) == EntryKind::Watch);  // P5：监视≠书签。
            }
            WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == idC);

            // P10：值类型▸ 子菜单只改"值类型"，不碰备注。
            QMenu* typeMenu = nullptr;
            view->selectRow(1);
            const QPoint onRow1 = view->visualRect(proxy->index(1, AddressBookModel::ColumnAddress)).center();
            QTimer opener;
            opener.setInterval(20);
            int ticks = 0;
            QObject::connect(&opener, &QTimer::timeout, &opener, [&]() {
                ++ticks;
                if (QMenu* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()))
                {
                    for (QAction* const action : menu->actions())
                    {
                        if (action->menu() != nullptr && action->text().startsWith(QStringLiteral("值类型")))
                        {
                            typeMenu = action->menu();
                            for (QAction* const typeAction : typeMenu->actions())
                            {
                                if (typeAction->text() == AddressBookModel::ValueTypeDisplayName(ValueType::F32))
                                {
                                    typeAction->trigger();
                                }
                            }
                        }
                    }
                    menu->close();
                    opener.stop();
                }
                else if (ticks >= 50)
                {
                    opener.stop();
                }
            });
            opener.start();
            emit view->customContextMenuRequested(onRow1);
            WPE_CHECK(typeMenu != nullptr);
            const std::optional<ksword::memwb::AddressEntry> entryB = store.find(idB);
            WPE_CHECK(entryB.has_value());
            if (entryB.has_value())
            {
                WPE_CHECK(entryB->valueType == ValueType::F32);
                WPE_CHECK_NOTE(entryB->note.empty(), QStringLiteral("值类型子菜单不应该碰备注"));
            }

            // P7：没有选区时按 Del 不发"删除"请求。
            view->clearSelection();
            view->setCurrentIndex(QModelIndex());
            QSignalSpy removed(&panel, &AddressBookPanel::removeRequested);
            QTest::keyClick(view, Qt::Key_Delete);
            WPE_CHECK(removed.count() == 0);

            // P8：双击"地址"列 = 跳转（按 press/release/DblClick 序列手工投递，离屏下
            // QTest::mouseDClick 不触发 doubleClicked）。
            QSignalSpy jump(&panel, &AddressBookPanel::jumpRequested);
            const QPoint pos = view->visualRect(proxy->index(0, AddressBookModel::ColumnAddress)).center();
            QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
            QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(pos), QPointF(view->viewport()->mapToGlobal(pos)),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &dbl);
            QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
            WPE_CHECK(jump.count() == 1 && jump.first().first().value<quint64>() == static_cast<quint64>(idA));
            panel.hide();
        }
    }

    void RunPanelSurvivorTests()
    {
        TestKeyboardActionsLandOnRightEntryAfterSort();
        TestColumnGroupButtonsCheckedState();
        TestKindIndexRoundTripAndSegmentLabels();
        TestMenuWiringAndRightClickSelection();
    }
}
