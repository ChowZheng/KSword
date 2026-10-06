// wpE_tests.Panel.cpp
// 作用：AddressBookPanel 的离屏交互验证——A/B 列组与自定义状态、kind 过滤索引往返、
// 排序后选区按 id、previewCopyText 四种字段、键盘快捷键（Del/F2/Enter，不碰真实剪贴板）、
// 双击值列编辑并提交 valueEditRequested、右键菜单（Promote/清空搜索结果，用非阻塞技巧
// 驱动 QMenu::exec() 的嵌套事件循环）、"不碰内存 I/O"的结构性核验。

#include "wpE_common.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QHeaderView>
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
        using ksword::memwb::EntryKind;
        using ksword::memwb::ValueType;

        // FindView：取面板内部真正的 QTableView——该控件是私有实现细节，没有公开访问器，
        // 测试借助 QObject 的父子树查找（而不是绕开访问控制改源码）来驱动真实的鼠标/键盘
        // 交互路径；这是 Qt 测试里定位内部控件的常规手法。
        QTableView* FindView(AddressBookPanel& panel)
        {
            return panel.findChild<QTableView*>();
        }

        // IsEditing：某个单元格此刻是否处于编辑态。QAbstractItemView::state()/EditingState
        // 都是 protected 成员，外部测试访问不到；本面板的两个委托（默认委托与
        // ValueColumnDelegate）都用 QLineEdit 作编辑器，编辑器是挂在 viewport 下的瞬时子
        // 控件，用"viewport 下有没有活着的 QLineEdit"这个可观察效果代替直接读内部状态。
        bool IsEditing(QTableView* view)
        {
            return view != nullptr && view->viewport()->findChild<QLineEdit*>() != nullptr;
        }

        // TriggerMenuActionByText：驱动一次会弹出模态 QMenu 的调用。
        // 原理同 tools/memwb_ui/memwb_ui_tests.Edit.cpp 里"右键选区规则"用例
        // （QTimer 配合 QApplication::activePopupWidget() 在 QMenu::exec() 的嵌套事件
        // 循环里找到并关掉弹出菜单）：这里改成每 20ms 轮询一次，找到后先按文本定位
        // QAction 并 trigger()，再关闭菜单；轮询 25 次（约 500ms）还没找到就放弹保险、
        // 直接关闭任何残留弹窗，避免真出问题时测试进程被挂死。
        // 传入：真正触发菜单弹出的调用（例如 emit 一次 customContextMenuRequested）、
        //       要点击的动作文本（支持前缀匹配，例如"加入书签"匹配"加入书签"本身）。
        // 传出：是否找到并触发了该动作。
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
                        if (action->text() == actionText || action->text().startsWith(actionText))
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
                if (pollCount >= 25)
                {
                    // 保险：一直没等到菜单弹出，强制关闭任何残留弹窗并停止轮询。
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

        // TestColumnGroupsDefaultAndSwitch：构造后默认 PresetA；切到 PresetB 后列显隐
        // 与 ux.md §4.3 的定义一致；setHiddenColumns 之后退化为 Custom。
        void TestColumnGroupsDefaultAndSwitch()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            QTableView* const view = FindView(panel);
            WPE_CHECK(view != nullptr);

            WPE_CHECK(panel.columnGroup() == AddressBookPanel::ColumnGroup::PresetA);
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnKindIcon));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnAddress));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnValue));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnNote));
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnValueType));
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnModuleOffset));
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnTarget));

            panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetB);
            WPE_CHECK(panel.columnGroup() == AddressBookPanel::ColumnGroup::PresetB);
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnKindIcon));
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnValue));
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnNote));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnAddress));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnValueType));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnModuleOffset));
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnTarget));

            panel.setHiddenColumns(QList<int>{AddressBookModel::ColumnTarget});
            WPE_CHECK(panel.columnGroup() == AddressBookPanel::ColumnGroup::Custom);
            WPE_CHECK(panel.hiddenColumns() == QList<int>{AddressBookModel::ColumnTarget});

            panel.applyColumnGroup(AddressBookPanel::ColumnGroup::PresetA);
            WPE_CHECK(panel.columnGroup() == AddressBookPanel::ColumnGroup::PresetA);
        }

        // TestHeaderMenuTogglesToCustom：驱动一次真实的表头右键菜单，取消勾选"地址"列，
        // 核验列真的被隐藏、且 A/B 两个预设按钮都不再是选中态（"自定义后 A/B 钮都不着色"）。
        void TestHeaderMenuTogglesToCustom()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            panel.resize(600, 300);
            panel.show();
            QTableView* const view = FindView(panel);
            WPE_CHECK(view != nullptr);
            WPE_CHECK(!view->isColumnHidden(AddressBookModel::ColumnAddress));

            QHeaderView* const header = view->horizontalHeader();
            const bool handled = TriggerMenuActionByText(
                [header]() { emit header->customContextMenuRequested(QPoint(10, 10)); },
                QStringLiteral("地址"));
            WPE_CHECK(handled);
            WPE_CHECK(view->isColumnHidden(AddressBookModel::ColumnAddress));
            WPE_CHECK(panel.columnGroup() == AddressBookPanel::ColumnGroup::Custom);
            // 修复可疑点 #10（本文件头注释说"A/B 两个预设按钮都不再是选中态"，但原来的
            // 断言只查了 columnGroup()，从未真的查过按钮的 isChecked()）：这里补上。
            bool foundUncheckedA = false;
            bool foundUncheckedB = false;
            for (QPushButton* const button : panel.findChildren<QPushButton*>())
            {
                if (button->text() == QStringLiteral("A"))
                {
                    foundUncheckedA = !button->isChecked();
                }
                else if (button->text() == QStringLiteral("B"))
                {
                    foundUncheckedB = !button->isChecked();
                }
            }
            WPE_CHECK_NOTE(foundUncheckedA, QStringLiteral("自定义列显隐后 A 钮应不再是选中态"));
            WPE_CHECK_NOTE(foundUncheckedB, QStringLiteral("自定义列显隐后 B 钮应不再是选中态"));
            panel.hide();
        }

        // TestSelectedIdsAfterSort：排序后 selectedIds 仍按 id 换算，不按行号——
        // target.md 1.8 那个旧缺陷的正面回归点。
        void TestSelectedIdsAfterSort()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t idLow = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x9000));
            const std::uint64_t idHigh = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x1000));

            QTableView* const view = FindView(panel);
            view->sortByColumn(AddressBookModel::ColumnAddress, Qt::AscendingOrder);
            // 排序后第 0 行应是地址更小的 idHigh（见 wpE_tests.Model.cpp 同名断言的解释）。
            view->selectRow(0);
            const std::vector<std::uint64_t> selected = panel.selectedIds();
            WPE_CHECK(selected.size() == 1);
            WPE_CHECK_NOTE(!selected.empty() && selected.front() == idHigh, QStringLiteral("got id=%1").arg(selected.empty() ? 0 : selected.front()));
            WPE_CHECK(idLow != idHigh);
        }

        // TestPreviewCopyTextFields：previewCopyText 四种字段，不触碰真实剪贴板
        // （见 .claude/memory/ksword-ui-architecture.md 的"自动化验证禁止写入真实剪贴板"）。
        void TestPreviewCopyTextFields()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeModuleEntry(EntryKind::Bookmark, "p", "client.dll", 0x1A40));
            model.setValueText(id, QStringLiteral("00000001"), AddressBookModel::ValueState::Read);
            model.setData(model.indexForId(id, AddressBookModel::ColumnNote), QStringLiteral("备注"), Qt::EditRole);

            QTableView* const view = FindView(panel);
            view->selectRow(0);
            view->setCurrentIndex(view->model()->index(0, AddressBookModel::ColumnAddress));

            WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Address) == QStringLiteral("client.dll+0x1a40"));
            WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::ModuleOffset) == QStringLiteral("client.dll + 0x1a40"));
            WPE_CHECK(panel.previewCopyText(AddressBookPanel::CopyField::Value) == QStringLiteral("00000001"));
            const QString rowText = panel.previewCopyText(AddressBookPanel::CopyField::Row);
            // 默认预设 A 可见列：地址·值·备注（类型图标列恒跳过），顺序即物理列顺序。
            WPE_CHECK(rowText == QStringLiteral("client.dll+0x1a40\t00000001\t备注"));
        }

        // TestDeleteKeyEmitsRemoveRequested：Del 键作用于选中的多行，发出去重后的 id 列表。
        void TestDeleteKeyEmitsRemoveRequested()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t idA = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 1));
            const std::uint64_t idB = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 2));

            QTableView* const view = FindView(panel);
            view->selectAll();

            QSignalSpy spy(&panel, &AddressBookPanel::removeRequested);
            QTest::keyClick(view, Qt::Key_Delete);
            WPE_CHECK(spy.count() == 1);
            if (spy.count() == 1)
            {
                const QList<quint64> ids = spy.first().first().value<QList<quint64>>();
                WPE_CHECK(ids.size() == 2);
                WPE_CHECK(ids.contains(static_cast<quint64>(idA)));
                WPE_CHECK(ids.contains(static_cast<quint64>(idB)));
            }
        }

        // TestEnterKeyEmitsJumpRequested：Enter 恒跳转，不进入编辑态（哪怕当前格在可编辑列）。
        void TestEnterKeyEmitsJumpRequested()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x42));

            QTableView* const view = FindView(panel);
            view->setCurrentIndex(view->model()->index(0, AddressBookModel::ColumnNote));

            QSignalSpy spy(&panel, &AddressBookPanel::jumpRequested);
            QTest::keyClick(view, Qt::Key_Return);
            WPE_CHECK(spy.count() == 1);
            if (spy.count() == 1)
            {
                WPE_CHECK(spy.first().first().value<quint64>() == static_cast<quint64>(id));
            }
            WPE_CHECK(!IsEditing(view));
        }

        // TestF2OpensNoteEditor：F2 在备注列打开编辑器（预设 A 下备注列可见）。
        void TestF2OpensNoteEditor()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Bookmark, "p", 0x42));

            QTableView* const view = FindView(panel);
            view->setCurrentIndex(view->model()->index(0, AddressBookModel::ColumnAddress));
            QTest::keyClick(view, Qt::Key_F2);
            WPE_CHECK(IsEditing(view));
            WPE_CHECK(view->currentIndex().column() == AddressBookModel::ColumnNote);
            // 编辑器是挂在 viewport 下的瞬时子控件（非持久编辑器），真实拥有键盘焦点的是它，
            // 不是 view 自己——Esc 必须发给编辑器才会被 Qt 内置的"编辑器事件过滤器"接住并
            // 放弃编辑，发给 view 不会有任何效果。
            QLineEdit* const editor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                QTest::keyClick(editor, Qt::Key_Escape);
            }
            // Qt 关闭瞬时编辑器走 deleteLater，真正销毁要等事件循环转一圈；单次
            // processEvents() 不一定够，用 WaitMs 真正跑一段事件循环。
            WaitMs(50);
            WPE_CHECK(!IsEditing(view));
        }

        // TestDoubleClickValueThenEnterEmitsValueEditRequested：双击"值"列进入编辑、输入
        // 新文本、按 Enter 提交——必须发出 valueEditRequested(id, 当前值类型, 新文本)，
        // 真正的内存写入留给上层，这里只核验信号参数。
        void TestDoubleClickValueThenEnterEmitsValueEditRequested()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Watch, "p", 0x42));
            model.setData(model.indexForId(id, AddressBookModel::ColumnValueType), static_cast<int>(ValueType::U32), Qt::EditRole);
            model.setValueText(id, QStringLiteral("old"), AddressBookModel::ValueState::Read);
            panel.resize(500, 200);
            panel.show();
            // visualRect 要求表格已经真正布局过一轮（行高/列宽），show() 本身只是排队了
            // 第一次绘制，必须 processEvents 把这一圈事件循环转完才能拿到非空矩形。
            QApplication::processEvents();

            QTableView* const view = FindView(panel);
            const QModelIndex valueIndex = view->model()->index(0, AddressBookModel::ColumnValue);
            WPE_CHECK(valueIndex.isValid());

            // 双击只是 Qt 内置的"触发编辑"方式之一，真正叫出编辑器的调用都落在
            // QAbstractItemView::edit(index)——直接调它既是确定性的（不依赖双击间隔计时在
            // 离屏环境下能否被正确识别），又与 F2/Enter 两条已验证路径殊途同归，一样会经过
            // ValueColumnDelegate::createEditor/setEditorData。
            QSignalSpy spy(&panel, &AddressBookPanel::valueEditRequested);
            view->setCurrentIndex(valueIndex);
            view->edit(valueIndex);
            WPE_CHECK(IsEditing(view));

            // 真实拥有键盘焦点的是 ValueColumnDelegate::createEditor 挂在 viewport 下的那个
            // QLineEdit，不是 view 自己；全选、输入、Enter 都要发给它。
            QLineEdit* const editor = view->viewport()->findChild<QLineEdit*>();
            WPE_CHECK(editor != nullptr);
            if (editor != nullptr)
            {
                QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);  // 全选初始文本……
                QTest::keyClicks(editor, QStringLiteral("new"));          // ……替换成新文本。
                WPE_CHECK_NOTE(editor->text() == QStringLiteral("new"), editor->text());
                QTest::keyClick(editor, Qt::Key_Return);
                WaitMs(50);
            }

            WPE_CHECK(spy.count() == 1);
            if (spy.count() == 1)
            {
                const QList<QVariant> args = spy.first();
                WPE_CHECK(args.at(0).value<quint64>() == static_cast<quint64>(id));
                WPE_CHECK(static_cast<ValueType>(args.at(1).toInt()) == ValueType::U32);
                WPE_CHECK(args.at(2).toString() == QStringLiteral("new"));
            }
            panel.hide();
        }

        // TestPromoteViaRowMenu：右键菜单"加入书签"对单选的搜索结果生效，发
        // promoteRequested(id, Bookmark)；菜单本身从不提供"降回搜索"的选项。
        void TestPromoteViaRowMenu()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            const std::uint64_t id = store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 0x42));
            panel.resize(500, 200);
            panel.show();

            QTableView* const view = FindView(panel);
            view->selectRow(0);

            QSignalSpy spy(&panel, &AddressBookPanel::promoteRequested);
            const bool handled = TriggerMenuActionByText(
                [view]() { emit view->customContextMenuRequested(QPoint(10, 10)); },
                QStringLiteral("加入书签"));
            WPE_CHECK(handled);
            WPE_CHECK(spy.count() == 1);
            if (spy.count() == 1)
            {
                WPE_CHECK(spy.first().at(0).value<quint64>() == static_cast<quint64>(id));
                WPE_CHECK(static_cast<EntryKind>(spy.first().at(1).toInt()) == EntryKind::Bookmark);
            }
            panel.hide();
        }

        // TestClearSearchResultsViaRowMenu：右键菜单"清空搜索结果"不需要选区即可触发。
        void TestClearSearchResultsViaRowMenu()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            store.add(MakeAbsoluteEntry(EntryKind::Search, "p", 1));
            panel.resize(500, 200);
            panel.show();

            QTableView* const view = FindView(panel);
            QSignalSpy spy(&panel, &AddressBookPanel::clearSearchResultsRequested);
            const bool handled = TriggerMenuActionByText(
                [view]() { emit view->customContextMenuRequested(QPoint(10, 10)); },
                QStringLiteral("清空搜索结果"));
            WPE_CHECK(handled);
            WPE_CHECK(spy.count() == 1);
            panel.hide();
        }

        // TestLoadFailureBanner：showLoadFailure 显示/隐藏横幅文案。
        void TestLoadFailureBanner()
        {
            AddressBookStore store(QString{});
            AddressBookModel model(&store);
            AddressBookPanel panel(&model);
            panel.showLoadFailure(QStringLiteral("第 3 行：unknown entry kind"));
            // 没有公开访问器读横幅文字，这里只核验调用不崩溃且可以安全地再隐藏一次——
            // 文案是否真的显示出来、颜色对不对由 wpE_tests.Shots.cpp 的截图人工核对。
            panel.showLoadFailure(QString());
        }

        // TestNoMemoryIoCoupling：结构性核验——地址簿三个源文件不出现任何"读写目标进程/
        // 内核内存"的信号（CreateFileW、DeviceIoControl、ReadProcessMemory、Windows.h），
        // 对应设计里"不可见时读取 0 次"：本面板压根没有能发起内存读取的依赖，这个检查
        // 直接核对源码确实没有悄悄加上这类调用。
        void TestNoMemoryIoCoupling()
        {
            const std::array<QString, 3> paths{
                QStringLiteral("Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookPanel.cpp"),
                QStringLiteral("Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookPanel.Menu.cpp"),
                QStringLiteral("Ksword5.1/Ksword5.1/UI/MemoryWorkbench/AddressBookModel.cpp"),
            };
            const std::array<QString, 4> forbidden{
                QStringLiteral("CreateFileW"), QStringLiteral("DeviceIoControl"),
                QStringLiteral("ReadProcessMemory"), QStringLiteral("Windows.h")
            };
            for (const QString& relativePath : paths)
            {
                QFile file(relativePath);
                WPE_CHECK_NOTE(file.open(QIODevice::ReadOnly), relativePath);
                const QString text = QString::fromUtf8(file.readAll());
                for (const QString& token : forbidden)
                {
                    WPE_CHECK_NOTE(!text.contains(token), relativePath + QStringLiteral(" contains ") + token);
                }
            }
        }
    }

    void RunPanelTests()
    {
        TestColumnGroupsDefaultAndSwitch();
        TestHeaderMenuTogglesToCustom();
        TestSelectedIdsAfterSort();
        TestPreviewCopyTextFields();
        TestDeleteKeyEmitsRemoveRequested();
        TestEnterKeyEmitsJumpRequested();
        TestF2OpensNoteEditor();
        TestDoubleClickValueThenEnterEmitsValueEditRequested();
        TestPromoteViaRowMenu();
        TestClearSearchResultsViaRowMenu();
        TestLoadFailureBanner();
        TestNoMemoryIoCoupling();
    }
}
