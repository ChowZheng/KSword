// wpE_tests.Panel.Defects.cpp
// 作用：审核报告 review-wpE.md §5.3 的缺陷回归测试（C1/C2/C3/C7/C8/C9/C10/C11/C15/C16）+
// 可疑点 #4 的回归（Ctrl+C 多选复制所选各行、"复制值"在非 Read 状态置灰且不复制占位符）。
// 每个测试在修复前的旧代码上应该 FAIL，修复后应该 PASS——这是"先复现再修"方法论在本包
// 夹具里的落地位置。

#include "wpE_common.h"

#include "../../../Ksword5.1/Ksword5.1/theme.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewWidgets.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
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

        // TestC1_FeedMustNotOverwriteEditor：编辑器里敲了 "999" 之后，外部用完全相同的
        // ("100", Read) 再喂一次——编辑器文本必须仍是用户敲的 "999"，不能被原样喂回来的
        // 旧文本覆盖、还被全选。
        void TestC1_FeedMustNotOverwriteEditor()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            model.setData(model.indexForId(id, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::U32), Qt::EditRole);
            model.setValueText(id, QStringLiteral("100"), AddressBookModel::ValueState::Read);
            panel.resize(480, 180);
            panel.show();
            QApplication::processEvents();

            QTableView* const view = FindView(panel);
            const QModelIndex valueIndex = view->model()->index(0, AddressBookModel::ColumnValue);
            view->setCurrentIndex(valueIndex);
            view->edit(valueIndex);
            QLineEdit* const editor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClicks(editor, QStringLiteral("999"));
                WPE_CHECK_NOTE(editor->text() == QStringLiteral("999"), editor->text());

                // 外部完全没变地再喂一次——模拟常驻监视 1Hz 轮询读到同一个旧值。
                model.setValueText(id, QStringLiteral("100"), AddressBookModel::ValueState::Read);
                QApplication::processEvents();
                WPE_CHECK_NOTE(editor->text() == QStringLiteral("999"), editor->text());
            }
            panel.hide();
        }

        // TestC1b_DifferentFeedDuringEditDoesNotOverwrite：C1 的第一道防线（Model 侧
        // "文本/状态都没变就不发 dataChanged"）只挡得住"喂入完全相同的值"这一种情况；
        // 常驻监视的真实值在编辑期间也可能恰好变化（喂入的是一个不同的新值），这时必须
        // 靠委托侧"只在 createEditor 后第一次 setEditorData 才真正填充"这第二道防线才能
        // 挡住——这里直接喂一个与初始值不同的新值，验证编辑器里用户刚敲的字不会被覆盖。
        void TestC1b_DifferentFeedDuringEditDoesNotOverwrite()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            model.setData(model.indexForId(id, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::U32), Qt::EditRole);
            model.setValueText(id, QStringLiteral("100"), AddressBookModel::ValueState::Read);
            panel.resize(480, 180);
            panel.show();
            QApplication::processEvents();

            QTableView* const view = FindView(panel);
            const QModelIndex valueIndex = view->model()->index(0, AddressBookModel::ColumnValue);
            view->setCurrentIndex(valueIndex);
            view->edit(valueIndex);
            QLineEdit* const editor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClicks(editor, QStringLiteral("999"));
                WPE_CHECK_NOTE(editor->text() == QStringLiteral("999"), editor->text());

                // 喂一个真的不同的新值（不是 "100"）——Model 侧的去重挡不住这种情况，
                // dataChanged 会真的发出来，只能靠委托"只填一次"的保护。
                model.setValueText(id, QStringLiteral("200"), AddressBookModel::ValueState::Read);
                QApplication::processEvents();
                WPE_CHECK_NOTE(
                    editor->text() == QStringLiteral("999"),
                    QStringLiteral("C1：编辑期间喂入不同的新值也不应该覆盖用户正在敲的文字，实际=%1").arg(editor->text()));
            }
            panel.hide();
        }

        // TestC2_UnchangedCommitMustNotEmit：双击进入编辑态、什么都不输入直接按 Tab 提交
        // ——不应该发出 valueEditRequested（否则一次误双击 + 点别处失焦，就会把当前显示
        // 的文本原样当成"用户想写的新值"发给上层写事务）。
        void TestC2_UnchangedCommitMustNotEmit()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            model.setValueText(id, QStringLiteral("42"), AddressBookModel::ValueState::Read);
            panel.resize(480, 180);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            const QModelIndex valueIndex = view->model()->index(0, AddressBookModel::ColumnValue);
            view->setCurrentIndex(valueIndex);
            view->edit(valueIndex);
            QLineEdit* const editor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                WPE_CHECK(editor->text() == QStringLiteral("42"));
                QSignalSpy spy(&panel, &AddressBookPanel::valueEditRequested);
                // 什么都不输入，直接 Tab 提交。
                QTest::keyClick(editor, Qt::Key_Tab);
                WaitMs(50);
                WPE_CHECK_NOTE(spy.count() == 0, QStringLiteral("C2：未改动的提交不应该发信号"));
            }
            panel.hide();
        }

        // TestC3_ValueTypeColumnEditMustNotResetType：值类型列不再标可编辑（C3），双击它
        // 不应该打开任何编辑器，类型必须保持不变。
        void TestC3_ValueTypeColumnEditMustNotResetType()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            model.setData(model.indexForId(id, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::U32), Qt::EditRole);
            panel.resize(480, 180);
            panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetB);  // B 组才显示值类型列。
            panel.show();
            QApplication::processEvents();

            QTableView* const view = FindView(panel);
            const QModelIndex typeIndex = view->model()->index(0, AddressBookModel::ColumnValueType);
            view->setCurrentIndex(typeIndex);
            // QAbstractItemView::edit(const QModelIndex&) 这个公开槛返回 void，判断"到底
            // 有没有真的开出编辑器"只能看效果——viewport 下是否出现了一个编辑器控件。
            view->edit(typeIndex);
            QApplication::processEvents();
            WPE_CHECK_NOTE(
                view->viewport()->findChild<QLineEdit*>() == nullptr,
                QStringLiteral("C3：值类型列不应该再能从表格里直接进入编辑态"));
            WPE_CHECK(store.find(id)->valueType == ValueType::U32);
            panel.hide();
        }

        // TestC7_MenuActsOnSelectionNotOnCurrentCell：选区={A}、当前格停在 B（Ctrl+点击
        // 先选中 B 再取消选中它，焦点留在 B）——右键 A、点"跳转"必须得到 A 的 id，不能像
        // 旧代码一样读"当前格"B。
        void TestC7_MenuActsOnSelectionNotOnCurrentCell()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t idA = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));
            const std::uint64_t idB = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));
            panel.resize(480, 200);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            QAbstractItemModel* const proxy = view->model();

            // 构造"选区={A}、当前格=B"：选中 A，再 Ctrl+点击 B 两次（选中又取消选中），
            // 当前格会停在 B，但 B 已经不在选区里。
            view->selectRow(0);
            const QPoint posB = view->visualRect(proxy->index(1, AddressBookModel::ColumnAddress)).center();
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, posB);
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, posB);
            WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == idA);
            WPE_CHECK(view->currentIndex().row() == 1);  // 当前格仍停在 B 那一行。

            QSignalSpy jump(&panel, &AddressBookPanel::jumpRequested);
            const QPoint posA = view->visualRect(proxy->index(0, AddressBookModel::ColumnAddress)).center();
            WPE_CHECK(TriggerMenuActionByText(
                [view, posA]() { emit view->customContextMenuRequested(posA); }, QStringLiteral("跳转")));
            WPE_CHECK(jump.count() == 1);
            if (jump.count() == 1)
            {
                WPE_CHECK_NOTE(
                    jump.first().first().value<quint64>() == static_cast<quint64>(idA),
                    QStringLiteral("C7：菜单动作应该落在被选中/被右键的 A 上，不是当前格 B"));
            }
            Q_UNUSED(idB);
            panel.hide();
        }

        // TestC7b_CurrentOutsideMultiSelectionIsRejected：targetRowId() 的 currentInSelection
        // 判断只有在选区是 0 条或 ≥2 条时才会真正被用到（恰好 1 条时直接早返回那一条，不看
        // 当前格）——选中 A、C 两行（选区 2 条），再把当前格移到完全没被选中的第三行 B，
        // 跳转不应该发出任何信号：B 既不是唯一选区，也不在选区里，没有"该对谁生效"的答案。
        void TestC7b_CurrentOutsideMultiSelectionIsRejected()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));  // A
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));  // B
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x3000));  // C
            panel.resize(480, 220);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            QAbstractItemModel* const proxy = view->model();

            view->selectRow(0);  // 选中 A。
            const QPoint posC = view->visualRect(proxy->index(2, AddressBookModel::ColumnAddress)).center();
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, posC);  // 加选 C：选区={A,C}。
            WPE_CHECK(panel.selectedIds().size() == 2);

            // 当前格移到 B（第三行，不在选区里）：只移动焦点，不碰选区——
            // QAbstractItemView::setCurrentIndex 按文档会连带选中目标格（除非选择模式是
            // NoSelection），必须经 selectionModel()->setCurrentIndex(index, NoUpdate) 才能
            // 真正做到"只挪焦点"。
            view->selectionModel()->setCurrentIndex(
                proxy->index(1, AddressBookModel::ColumnAddress), QItemSelectionModel::NoUpdate);
            WPE_CHECK(panel.selectedIds().size() == 2);  // 选区没有被这次移动焦点改变。

            QSignalSpy jump(&panel, &AddressBookPanel::jumpRequested);
            QTest::keyClick(view, Qt::Key_Return);
            WPE_CHECK_NOTE(
                jump.count() == 0,
                QStringLiteral("C7：当前格在多选之外时，跳转不该对任何行生效"));
            panel.hide();
        }

        // TestC8_BulkAddStaysFast：连续 2000 次单条 add（视图已挂载显示），耗时必须明显
        // 低于旧的整表重建代价——审核报告量到 2000 条要 2.5s，改成增量 insert 后应该远小于
        // 1000ms。这里给一个宽松但有意义的上限，避免机器性能波动导致假失败。
        void TestC8_BulkAddStaysFast()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            panel.resize(600, 300);
            panel.show();
            QApplication::processEvents();

            QElapsedTimer timer;
            timer.start();
            for (int i = 0; i < 2000; ++i)
            {
                store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", static_cast<std::uint64_t>(i + 1)));
            }
            const qint64 elapsedMs = timer.elapsed();
            // 预算留得比审核报告的 1000ms 宽松一些（1500ms），只为避开机器性能波动造成的
            // 假失败——真正要分辨的是"几百毫秒"和旧代码"2.5s"的量级差异，不是卡着
            // 1000ms 这根线精确判定。
            WPE_CHECK_NOTE(elapsedMs < 1500, QStringLiteral("2000 次 add 耗时 %1 ms").arg(elapsedMs));
            WPE_CHECK(model.rowCount() == 2000);
            panel.hide();
        }

        // TestC9_SwitchingKindFilterDoesNotRebuildSegment：连续点击 kind 分段（切换过滤）
        // 不应该重建 HexViewSegmented 控件——旧代码里 kindCountsChanged 对纯过滤切换也会
        // 发出，Panel 侧因此把分段控件删了重建，审核报告指出这正好发生在分段控件自己的
        // mousePressEvent 调用链里（delete this 的风险）。这里用"控件指针身份不变"验证
        // 它没有被销毁重建。
        void TestC9_SwitchingKindFilterDoesNotRebuildSegment()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
            store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 3));
            panel.resize(480, 200);
            panel.show();
            QApplication::processEvents();

            HexViewSegmented* const segmentBefore = panel.findChild<HexViewSegmented*>();
            WPE_CHECK(segmentBefore != nullptr);
            for (const int index : { 0, 1, 2, -1, 0 })
            {
                panel.setKindFilterIndex(index);
                QApplication::processEvents();
            }
            HexViewSegmented* const segmentAfter = panel.findChild<HexViewSegmented*>();
            WPE_CHECK_NOTE(
                segmentBefore == segmentAfter,
                QStringLiteral("C9：纯粹切换 kind 过滤不应该重建分段控件"));

            // 再用一次真实的鼠标点击重复同一件事——审核报告 P8c 指出的具体风险点是
            // "HexViewSegmented::mousePressEvent -> setCurrentIndex -> emit
            // currentIndexChanged -> ... -> delete this"，程序化调用 setKindFilterIndex
            // 不会经过这条调用栈，这里补一次真正的点击，让回归测试对得上原始机制。
            if (segmentAfter != nullptr)
            {
                const QRect segmentRect = segmentAfter->segmentRect(1);
                QTest::mouseClick(segmentAfter, Qt::LeftButton, Qt::NoModifier, segmentRect.center());
                QApplication::processEvents();
                WPE_CHECK_NOTE(
                    panel.findChild<HexViewSegmented*>() == segmentAfter,
                    QStringLiteral("C9：真实鼠标点击分段控件也不应该让它把自己删了重建"));
            }
            panel.hide();
        }

        // TestC10_ColumnButtonsFollowThemeSwitch：亮色下建面板、记一次样式表文本，切到
        // 暗色后样式表文本必须不变——这才是"用 palette(...) 动态占位符"真正要证明的事：
        // 不需要在主题切换时重新 setStyleSheet，Qt 的样式引擎会在每次绘制时现查当前
        // QPalette。旧代码（*ColorHex() 在构造那一刻把颜色定格成字面 "#RRGGBB"）在这个
        // 测试下会表现为"样式表文本压根没有变化"——看似通过，但审核报告 P14b 的读数
        // 证明那份文本锚定的其实是亮色主题的字面值，暗色主题下界面仍然显示亮色；本测试
        // 另外直接断言文本必须含 "palette(" 占位符，排除这种假阳性。
        void TestC10_ColumnButtonsFollowThemeSwitch()
        {
            ApplyTheme(false);
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            QPushButton* const buttonA = FindButton(panel, QStringLiteral("A"));
            WPE_CHECK(buttonA != nullptr);
            if (buttonA != nullptr)
            {
                const QString lightStyle = buttonA->styleSheet();
                WPE_CHECK_NOTE(
                    lightStyle.contains(QStringLiteral("palette(")),
                    QStringLiteral("C10：未选中态应该用 palette(...) 动态占位符，而不是字面十六进制"));
                ApplyTheme(true);
                const QString darkStyle = buttonA->styleSheet();
                WPE_CHECK_NOTE(
                    darkStyle == lightStyle,
                    QStringLiteral("C10：样式表文本本身不该随主题切换而改变——改变颜色的是 Qt 对 palette(...) 的现场求值"));
            }
            ApplyTheme(false);  // 恢复默认，不影响后续测试/截图。
        }

        // TestC11_ColumnButtonsAdjacentEvenWhenNarrow：窄窗口下 A、B 两个按钮必须始终在
        // 同一行、紧贴在一起，不能被 FlowLayout 拆成"A 在第一行右端、B 在第二行最左"。
        void TestC11_ColumnButtonsAdjacentEvenWhenNarrow()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            panel.resize(280, 220);
            panel.show();
            QApplication::processEvents();
            QPushButton* const buttonA = FindButton(panel, QStringLiteral("A"));
            QPushButton* const buttonB = FindButton(panel, QStringLiteral("B"));
            WPE_CHECK(buttonA != nullptr && buttonB != nullptr);
            if (buttonA != nullptr && buttonB != nullptr)
            {
                const QPoint globalA = buttonA->mapToGlobal(QPoint(0, 0));
                const QPoint globalB = buttonB->mapToGlobal(QPoint(0, 0));
                WPE_CHECK_NOTE(
                    globalA.y() == globalB.y(),
                    QStringLiteral("C11：窄窗口下 A/B 应该始终同一行"));
                WPE_CHECK_NOTE(
                    globalB.x() > globalA.x() && (globalB.x() - (globalA.x() + buttonA->width())) <= 1,
                    QStringLiteral("C11：A/B 应该左右紧贴，不留明显间隙"));
            }
            panel.hide();
        }

        // TestC15_AddressColumnSortsNumerically：通过真正的 Panel（链了 SortRole 的排序
        // 代理）按"地址"列升序排序，三条绝对地址必须按数值序排列——旧的字符串序会把
        // 0x10000 排到 0x2000 前面。
        void TestC15_AddressColumnSortsNumerically()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x10000));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x2000));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0xa00));
            panel.resize(480, 200);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            view->sortByColumn(AddressBookModel::ColumnAddress, Qt::AscendingOrder);
            QApplication::processEvents();

            QStringList actual;
            for (int row = 0; row < view->model()->rowCount(); ++row)
            {
                actual << view->model()->index(row, AddressBookModel::ColumnAddress).data(Qt::DisplayRole).toString();
            }
            const QStringList expected{ QStringLiteral("0xa00"), QStringLiteral("0x2000"), QStringLiteral("0x10000") };
            WPE_CHECK_NOTE(actual == expected, actual.join(QStringLiteral(",")));
            panel.hide();
        }

        // TestC16_AddKeepsSelection：选中第二行后，别处（不同 id）新增一条，选区与当前格
        // 都必须保留——旧的整表 reset 会把它们一起清空。
        void TestC16_AddKeepsSelection()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            const std::uint64_t idSecond = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
            panel.resize(480, 200);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            view->selectRow(1);
            WPE_CHECK(panel.selectedIds().size() == 1 && panel.selectedIds().front() == idSecond);

            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 3));
            QApplication::processEvents();
            WPE_CHECK_NOTE(
                panel.selectedIds().size() == 1 && panel.selectedIds().front() == idSecond,
                QStringLiteral("C16：别处新增一条不应该清空既有选区"));
            WPE_CHECK_NOTE(view->currentIndex().isValid(), QStringLiteral("C16：当前格也不应该被清空"));
            panel.hide();
        }

        // 可疑点 #4a：多选时 Ctrl+C 应该复制"所选各行"，用换行分隔，不是只复制当前一行。
        void TestCtrlCCopiesAllSelectedRowsWhenMultiSelected()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));
            panel.resize(480, 200);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            view->selectAll();
            QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier);
            const QString clipboardText = QGuiApplication::clipboard()->text();
            WPE_CHECK_NOTE(
                clipboardText.contains(QStringLiteral("\n")),
                QStringLiteral("可疑点 #4：多选 Ctrl+C 应该是多行，实际='%1'").arg(clipboardText));
            WPE_CHECK(clipboardText.split(QStringLiteral("\n")).size() == 2);
            panel.hide();
        }

        // 可疑点 #4b："复制值"在值不是 Read 状态时不应该复制占位符（previewCopyText 应该
        // 返回空串，让 copyCurrentRowField 跳过真正的剪贴板写入）。
        void TestCopyValueSkipsPlaceholderWhenNotRead()
        {
            AddressBookStore store{ QString() };
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 1));
            model.setValueText(id, QString(), AddressBookModel::ValueState::Unreadable);
            panel.resize(480, 200);
            panel.show();
            QApplication::processEvents();
            QTableView* const view = FindView(panel);
            view->selectRow(0);
            WPE_CHECK_NOTE(
                panel.previewCopyText(AddressBookPanel::CopyField::Value).isEmpty(),
                QStringLiteral("可疑点 #4：Unreadable 状态不应该复制占位符文本"));

            model.setValueText(id, QStringLiteral("real-value"), AddressBookModel::ValueState::Read);
            WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Value) == QStringLiteral("real-value"));
            panel.hide();
        }
    }

    void RunPanelDefectTests()
    {
        TestC1_FeedMustNotOverwriteEditor();
        TestC1b_DifferentFeedDuringEditDoesNotOverwrite();
        TestC2_UnchangedCommitMustNotEmit();
        TestC3_ValueTypeColumnEditMustNotResetType();
        TestC7_MenuActsOnSelectionNotOnCurrentCell();
        TestC7b_CurrentOutsideMultiSelectionIsRejected();
        TestC8_BulkAddStaysFast();
        TestC9_SwitchingKindFilterDoesNotRebuildSegment();
        TestC10_ColumnButtonsFollowThemeSwitch();
        TestC11_ColumnButtonsAdjacentEvenWhenNarrow();
        TestC15_AddressColumnSortsNumerically();
        TestC16_AddKeepsSelection();
        TestCtrlCCopiesAllSelectedRowsWhenMultiSelected();
        TestCopyValueSkipsPlaceholderWhenNotRead();
    }
}
