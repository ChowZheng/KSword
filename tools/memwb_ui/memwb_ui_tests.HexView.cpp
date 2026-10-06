// memwb_ui_tests.HexView.cpp
// 作用：HexView 复合控件的"外壳"离屏验证——
//   1) 布局与默认状态；
//   2) 工具栏：每个按钮有悬停提示、图标按主题现取颜色、三个下拉菜单（行宽/分组/导出）的内容、
//      选中态、不透明样式与逐项提示；
//   3) 工具栏与状态条显隐、极简嵌入外观；
//   4) 解释器面板：按需创建、跟随画布、分割条真实拖动、显隐持久化（读失败/乱写退回默认、代码调用不写偏好）；
//   5) 快捷键上下文：两个 HexView 同窗口并存时只作用于有焦点的那个，Esc 的"先查找/跳转条、再无则不处理"规则；
//   6) 状态条三段文字与瞬时消息。
// 全部手势经 QTest 模拟的真实输入或公开接口进入控件。

#include "memwb_ui_hexview.h"

#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexViewFormat.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLineEdit>
#include <QShortcut>
#include <QSplitter>
#include <QSplitterHandle>
#include <QToolTip>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexView;
        using Pane = HexCanvas::ActivePane;

        // kInspectorKey：解释器面板显隐偏好的键（与 HexViewSettings.cpp 里同一个字符串）。
        const char* kInspectorKey = "memwb/hexview/inspectorVisible";

        // InkInRect：区域内与底色明显不同的像素数（"画上了图形"的正向判据）。
        // 传入：图像、矩形、底色；传出：像素数。
        int InkInRect(const QImage& image, const QRect& rect, const QColor& background)
        {
            int count = 0;
            const QRect bounded = rect.intersected(image.rect());
            for (int y = bounded.top(); y <= bounded.bottom(); ++y)
            {
                for (int x = bounded.left(); x <= bounded.right(); ++x)
                {
                    if (ColorDistance(QColor(image.pixel(x, y)), background) > 90)
                    {
                        ++count;
                    }
                }
            }
            return count;
        }

        // 布局与默认状态。
        void TestConstruction()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x1000, MakePattern(4096), false, QSize(900, 520));

            // 默认可见性：工具栏与状态条显示；查找条、跳转条、解释器面板隐藏。
            CHECK(view->toolbarVisible());
            CHECK(view->statusBarVisible());
            CHECK(view->toolbar()->isVisible());
            CHECK(!view->findBar()->isVisible());
            CHECK(!view->gotoBar()->isVisible());
            CHECK(!view->inspectorVisible());
            CHECK(!view->isEditable());
            CHECK(view->bytesPerRow() == 16);
            CHECK(view->groupSize() == 1);

            // 自上而下的顺序：工具栏 < 画布 < 状态条，且互不重叠。
            const QRect toolbarRect = view->toolbar()->geometry();
            const QRect canvasRect(view->canvas()->mapTo(view.get(), QPoint(0, 0)), view->canvas()->size());
            const QRect statusRect = view->statusBar()->geometry();
            CHECK(toolbarRect.bottom() < canvasRect.top());
            CHECK(canvasRect.bottom() < statusRect.top());
            CHECK(statusRect.bottom() <= view->rect().bottom());

            // 空控件：没有数据时所有查询都有明确的"无"。
            HexView empty;
            empty.resize(700, 400);
            empty.show();
            Flush();
            CHECK(empty.buffer().isEmpty());
            CHECK(empty.baseAddress() == 0);
            CHECK(empty.bufferSize() == 0);
            CHECK(empty.caretAddress() == 0);
            CHECK(empty.selectedBytes().isEmpty());
            std::uint64_t startOffset = 7;
            std::uint64_t endOffset = 9;
            CHECK(!empty.selectionRange(startOffset, endOffset));
            CHECK(startOffset == 7 && endOffset == 9);
            CHECK(empty.statusBar()->caretText() == QStringLiteral("无数据"));
            CHECK(!empty.jumpToAddress(0));
            CHECK(empty.statusBar()->hasMessage());
        }

        // 工具栏：提示、图标、菜单。
        void TestToolbarButtonsAndMenus()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x2000, MakePattern(2048), false, QSize(900, 520));

            // 六个按钮都有悬停提示；行宽与分组带当前值徽标。
            const std::vector<ks::ui::HexViewGlyphButton*> buttons = {
                view->rowWidthButton(), view->groupButton(), view->findButton(),
                view->gotoButton(), view->exportButton(), view->inspectorButton() };
            for (ks::ui::HexViewGlyphButton* button : buttons)
            {
                CHECK(!button->toolTip().isEmpty());
            }
            CHECK(view->rowWidthButton()->badgeText() == QStringLiteral("16"));
            CHECK(view->groupButton()->badgeText() == QStringLiteral("1"));
            CHECK(view->rowWidthButton()->toolTip().contains(QStringLiteral("16")));
            CHECK(view->inspectorButton()->isCheckable());
            CHECK(!view->inspectorButton()->isChecked());

            // 图标按主题现取：浅色/深色下每个按钮都画上了图形，且底色随主题变化（不残留旧色）。
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                Flush();
                const QImage bar = view->toolbar()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QColor background = KswordTheme::SurfaceAltColor();
                CHECK_NOTE(ColorsClose(QColor(bar.pixel(bar.width() / 2, 2)), background, 3), QStringLiteral("toolbar background follows theme"));
                for (ks::ui::HexViewGlyphButton* button : buttons)
                {
                    CHECK_NOTE(InkInRect(bar, button->geometry(), background) >= 8,
                        QStringLiteral("glyph painted, dark=%1 glyph=%2").arg(dark).arg(static_cast<int>(button->glyph())));
                }
            }
            ApplyTheme(false);

            // 行宽菜单：五项、当前值打勾、每项有提示，样式显式不透明。
            QMenu* rowMenu = view->rowWidthMenu();
            emit rowMenu->aboutToShow();
            CHECK(rowMenu->actions().size() == 5);
            CHECK(rowMenu->toolTipsVisible());
            CHECK(!rowMenu->testAttribute(Qt::WA_TranslucentBackground));
            CHECK(MenuPaintsSurface(rowMenu));
            CHECK(rowMenu->styleSheet().contains(KswordTheme::SurfaceColorHex()));
            CHECK(rowMenu->styleSheet().contains(KswordTheme::TextPrimaryColorHex()));
            CHECK(rowMenu->styleSheet().contains(QStringLiteral("QMenu::item:selected")));
            CHECK(rowMenu->styleSheet().contains(QStringLiteral("QMenu::item:disabled")));
            QAction* width16 = FindAction(rowMenu, QStringLiteral("16 字节 / 行"));
            QAction* width32 = FindAction(rowMenu, QStringLiteral("32 字节 / 行"));
            CHECK(width16 != nullptr && width32 != nullptr);
            if (width16 != nullptr && width32 != nullptr)
            {
                CHECK(width16->isChecked());
                CHECK(!width32->isChecked());
                for (QAction* action : rowMenu->actions())
                {
                    CHECK(!action->toolTip().isEmpty());
                }
                width32->trigger();
                CHECK(view->bytesPerRow() == 32);
                CHECK(view->canvas()->bytesPerRow() == 32);
                CHECK(view->rowWidthButton()->badgeText() == QStringLiteral("32"));
                CHECK(view->rowWidthButton()->toolTip().contains(QStringLiteral("32")));

                // 再次弹出：勾选移到 32。
                emit rowMenu->aboutToShow();
                CHECK(FindAction(rowMenu, QStringLiteral("32 字节 / 行"))->isChecked());
                CHECK(!FindAction(rowMenu, QStringLiteral("16 字节 / 行"))->isChecked());
            }

            // 深色主题：菜单每次弹出前重新取静态主题色，弹出后像素是深色表面色（浅色残留会让这里失败）。
            ApplyTheme(true);
            CHECK(MenuPaintsSurface(rowMenu));
            CHECK(MenuPaintsSurface(view->groupMenu()));
            ApplyTheme(false);
            CHECK(MenuPaintsSurface(rowMenu));

            // 分组菜单：四项，触发后画布分组变化。
            QMenu* groupMenu = view->groupMenu();
            emit groupMenu->aboutToShow();
            CHECK(groupMenu->actions().size() == 4);
            CHECK(!groupMenu->testAttribute(Qt::WA_TranslucentBackground));
            QAction* group4 = FindAction(groupMenu, QStringLiteral("4 字节一组"));
            CHECK(group4 != nullptr);
            if (group4 != nullptr)
            {
                for (QAction* action : groupMenu->actions())
                {
                    CHECK(!action->toolTip().isEmpty());
                }
                group4->trigger();
                CHECK(view->groupSize() == 4);
                CHECK(view->canvas()->groupSize() == 4);
                CHECK(view->groupButton()->badgeText() == QStringLiteral("4"));
            }

            // 非法取值被拒绝且不改变状态。
            CHECK(!view->setBytesPerRow(24));
            CHECK(!view->setBytesPerRow(0));
            CHECK(!view->setBytesPerRow(-16));
            CHECK(view->bytesPerRow() == 32);
            CHECK(!view->setGroupSize(3));
            CHECK(view->groupSize() == 4);
            CHECK(view->setBytesPerRow(48));
            CHECK(view->setBytesPerRow(8));
            CHECK(view->setBytesPerRow(64));

            // 导出菜单：有数据时三项可用且有提示；没有数据时全部置灰。
            QMenu* exportMenu = view->exportMenu();
            emit exportMenu->aboutToShow();
            CHECK(exportMenu->actions().size() == 3);
            for (QAction* action : exportMenu->actions())
            {
                CHECK(action->isEnabled());
                CHECK(!action->toolTip().isEmpty());
            }
            CHECK(!exportMenu->testAttribute(Qt::WA_TranslucentBackground));
            CHECK(MenuPaintsSurface(exportMenu));
            view->clearBuffer();
            emit exportMenu->aboutToShow();
            for (QAction* action : exportMenu->actions())
            {
                CHECK(!action->isEnabled());
            }
        }

        // 悬停提示：向按钮与分段按钮发真实的 ToolTip 事件，QToolTip 弹出的文字与 toolTip() 一致
        // （"所有按钮悬停要出现释义"：工具栏、查找条、跳转条、状态条）。
        void TestHoverTips()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x1000, MakePattern(512), false, QSize(1000, 420));
            view->openFind();
            view->openGoto();
            Flush();

            // tipShownFor：向控件的某点发 ToolTip 事件，返回 QToolTip 实际弹出的文字（没弹出为空串）。
            const auto tipShownFor = [](QWidget* widget, const QPoint& position) -> QString {
                QToolTip::hideText();
                QHelpEvent event(QEvent::ToolTip, position, widget->mapToGlobal(position));
                QApplication::sendEvent(widget, &event);
                const QString shown = QToolTip::isVisible() ? QToolTip::text() : QString();
                QToolTip::hideText();
                return shown;
            };

            // 工具栏、查找条、跳转条里的全部图标按钮。
            const std::vector<ks::ui::HexViewGlyphButton*> buttons = {
                view->rowWidthButton(), view->groupButton(), view->findButton(), view->gotoButton(),
                view->exportButton(), view->inspectorButton(),
                view->findBar()->caseButton(), view->findBar()->previousButton(), view->findBar()->nextButton(),
                view->findBar()->closeButton(),
                view->gotoBar()->historyButton(), view->gotoBar()->goButton(), view->gotoBar()->closeButton() };
            for (ks::ui::HexViewGlyphButton* button : buttons)
            {
                const QString shown = tipShownFor(button, button->rect().center());
                CHECK_NOTE(!shown.isEmpty() && shown == button->toolTip(),
                    QStringLiteral("glyph %1: %2").arg(static_cast<int>(button->glyph())).arg(shown));
            }

            // 两个分段按钮：每一段都有自己的悬停提示（按纯文本转义后显示）。
            for (ks::ui::HexViewSegmented* segmented : { view->findBar()->modeSegment(), view->gotoBar()->modeSegment() })
            {
                for (int index = 0; index < segmented->count(); ++index)
                {
                    const QString shown = tipShownFor(segmented, segmented->segmentRect(index).center());
                    CHECK(!shown.isEmpty());
                    CHECK(shown == ks::ui::hexview_format::PlainToolTip(segmented->segmentToolTip(index)));
                }
            }

            // 状态条：悬停提示是消息全文（可能被省略的长消息也能读到）。
            view->statusBar()->setMessage(ks::ui::HexViewStatusBar::Kind::Info, QStringLiteral("导出完成：测试消息"));
            const QString statusTip = tipShownFor(view->statusBar(), view->statusBar()->rect().center());
            CHECK(statusTip.contains(QStringLiteral("导出完成：测试消息")));
        }

        // 工具栏与状态条显隐，极简嵌入外观。
        void TestVisibility()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x1000, MakePattern(1024), false, QSize(800, 480));
            const int heightWithBars = view->canvas()->height();

            view->setToolbarVisible(false);
            Flush();
            CHECK(!view->toolbarVisible());
            CHECK(!view->toolbar()->isVisible());
            CHECK(view->canvas()->height() > heightWithBars);
            CHECK(view->statusBarVisible());

            view->setStatusBarVisible(false);
            Flush();
            CHECK(!view->statusBarVisible());
            CHECK(!view->statusBar()->isVisible());

            // 两者都隐藏：画布铺满整个控件（极简嵌入外观）。
            CHECK(view->canvas()->mapTo(view.get(), QPoint(0, 0)) == QPoint(0, 0));
            CHECK(view->canvas()->height() == view->height());
            CHECK(view->canvas()->width() == view->width());

            // 恢复显示。
            view->setToolbarVisible(true);
            view->setStatusBarVisible(true);
            Flush();
            CHECK(view->toolbar()->isVisible() && view->statusBar()->isVisible());
            CHECK(view->canvas()->height() == heightWithBars);
        }

        // 解释器面板：按需创建、跟随画布、真实拖动分割条。
        void TestInspectorBehavior()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x1000, MakePattern(4096), false, QSize(1100, 520));

            // 没有开过的控件根本没创建面板（嵌入式宿主不为它付开销）。
            CHECK(view->findChildren<ks::ui::HexInspectorPanel*>().isEmpty());

            // 点工具栏按钮：面板创建并显示，按钮勾选。
            QTest::mouseClick(view->inspectorButton(), Qt::LeftButton);
            Flush();
            CHECK(view->inspectorVisible());
            CHECK(view->inspectorButton()->isChecked());
            ks::ui::HexInspectorPanel* panel = view->panel();
            CHECK(panel != nullptr && panel->isVisible());
            CHECK(view->findChildren<ks::ui::HexInspectorPanel*>().size() == 1);
            CHECK(panel->canvas() == view->canvas());
            CHECK(panel->width() >= 240);

            // 面板跟随画布插入点。
            ClickAddress(*view->canvas(), 0x1040);
            Flush();
            CHECK(view->caretAddress() == 0x1040);
            CHECK(panel->topBar()->addressText().contains(QStringLiteral("1040")));

            // 真实拖动分割条：向左拖 100 像素，面板变宽约 100。
            QSplitterHandle* handle = view->splitter()->handle(1);
            CHECK(handle != nullptr);
            if (handle != nullptr)
            {
                const int before = panel->width();
                const QPoint center = handle->rect().center();
                QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, center);
                DragMove(handle, center + QPoint(-100, 0));
                QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, center + QPoint(-100, 0));
                Flush();
                CHECK_NOTE(panel->width() >= before + 80 && panel->width() <= before + 120,
                    QStringLiteral("before=%1 after=%2").arg(before).arg(panel->width()));
            }

            // 再点一次：隐藏，勾选取消。
            QTest::mouseClick(view->inspectorButton(), Qt::LeftButton);
            Flush();
            CHECK(!view->inspectorVisible());
            CHECK(!view->inspectorButton()->isChecked());
            CHECK(!panel->isVisible());

            // 代码调用显隐同样同步勾选状态（不发"用户点击"的持久化，见下一个测试）。
            view->setInspectorVisible(true);
            CHECK(view->inspectorVisible() && view->inspectorButton()->isChecked());
            view->setInspectorVisible(false);
            CHECK(!view->inspectorVisible() && !view->inspectorButton()->isChecked());
        }

        // 解释器显隐的持久化：用户切换写偏好；代码调用不写；乱写的值退回默认。
        void TestInspectorPersistence()
        {
            ApplyTheme(false);
            {
                QSettings settings;
                settings.clear();
                settings.sync();
            }

            // 用户点按钮：写入 true。
            {
                auto view = MakeHexView(0, MakePattern(512), false, QSize(900, 400));
                CHECK(!view->inspectorVisible());
                QTest::mouseClick(view->inspectorButton(), Qt::LeftButton);
                const QSettings settings;
                CHECK(settings.contains(QString::fromLatin1(kInspectorKey)));
                CHECK(settings.value(QString::fromLatin1(kInspectorKey)).toBool());
            }

            // 新控件恢复为显示，按钮勾选，面板可见。
            {
                auto view = MakeHexView(0, MakePattern(512), false, QSize(900, 400));
                CHECK(view->inspectorVisible());
                CHECK(view->inspectorButton()->isChecked());
                CHECK(view->panel()->isVisible());
                QTest::mouseClick(view->inspectorButton(), Qt::LeftButton);
                const QSettings settings;
                CHECK(!settings.value(QString::fromLatin1(kInspectorKey)).toBool());
            }

            // 再新建一个：恢复为隐藏。
            {
                auto view = MakeHexView(0, MakePattern(512), false, QSize(900, 400));
                CHECK(!view->inspectorVisible());
            }

            // 代码调用 setInspectorVisible 不写偏好：键保持缺失。
            {
                QSettings settings;
                settings.clear();
                settings.sync();
            }
            {
                auto view = MakeHexView(0, MakePattern(512), false, QSize(900, 400));
                view->setInspectorVisible(true);
                CHECK(view->inspectorVisible());
                view->setInspectorVisible(false);
                const QSettings settings;
                CHECK(!settings.contains(QString::fromLatin1(kInspectorKey)));
            }

            // 乱写的偏好值：读失败退回默认 false；合法的 "true" / "1" 才算真。
            const std::vector<std::pair<QVariant, bool>> cases = {
                { QVariant(QStringLiteral("garbage")), false },
                { QVariant(QStringLiteral("2")), false },
                { QVariant(QStringLiteral("")), false },
                { QVariant(QStringList{ QStringLiteral("a"), QStringLiteral("b") }), false },
                { QVariant(QStringLiteral("true")), true },
                { QVariant(QStringLiteral("1")), true },
                { QVariant(true), true },
                { QVariant(false), false },
            };
            for (const auto& entry : cases)
            {
                {
                    QSettings settings;
                    settings.setValue(QString::fromLatin1(kInspectorKey), entry.first);
                    settings.sync();
                }
                CHECK_NOTE(ks::ui::hexview_settings::LoadInspectorVisible() == entry.second,
                    entry.first.toString());
                auto view = MakeHexView(0, MakePattern(512), false, QSize(900, 400));
                CHECK_NOTE(view->inspectorVisible() == entry.second, entry.first.toString());
            }
            QSettings settings;
            settings.clear();
            settings.sync();
        }

        // 读失败退回默认：把设置位置指到一个"普通文件"下面，QSettings 状态异常，读到的全是默认值，写失败不崩溃。
        void TestSettingsFailure()
        {
            QTemporaryDir directory;
            const QString blocker = directory.filePath(QStringLiteral("blocker"));
            QFile file(blocker);
            CHECK(file.open(QIODevice::WriteOnly));
            file.write("x");
            file.close();

            // 先在正常位置写入 true，再切到不可用位置：读应回到 false / 空历史。
            ks::ui::hexview_settings::SaveInspectorVisible(true);
            ks::ui::hexview_settings::SaveGotoHistory(QStringList{ QStringLiteral("1234") });
            CHECK(ks::ui::hexview_settings::LoadInspectorVisible());
            CHECK(ks::ui::hexview_settings::LoadGotoHistory() == QStringList{ QStringLiteral("1234") });

            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, blocker);
            {
                // 打开时看不出问题，写入同步后状态异常（这就是"读写失败"的真实形态）。
                QSettings probe;
                probe.setValue(QStringLiteral("probe"), 1);
                probe.sync();
                CHECK(probe.status() != QSettings::NoError);
            }
            CHECK(!ks::ui::hexview_settings::LoadInspectorVisible());
            CHECK(ks::ui::hexview_settings::LoadGotoHistory().isEmpty());
            ks::ui::hexview_settings::SaveInspectorVisible(true);
            ks::ui::hexview_settings::SaveGotoHistory(QStringList{ QStringLiteral("zz") });
            CHECK(!ks::ui::hexview_settings::LoadInspectorVisible());
            auto view = MakeHexView(0, MakePattern(256), false, QSize(700, 400));
            CHECK(!view->inspectorVisible());
            view.reset();

            // 换回一个正常目录，供后面的测试继续使用。
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
            QSettings settings;
            settings.clear();
            settings.sync();
        }

        // 快捷键：两个 HexView 同窗口并存，只作用于有焦点的那个；Esc 的优先级；全部是 WidgetWithChildren 上下文。
        void TestShortcuts()
        {
            ApplyTheme(false);
            QWidget host;
            auto* layout = new QHBoxLayout(&host);
            HexView* left = new HexView(&host);
            HexView* right = new HexView(&host);
            layout->addWidget(left);
            layout->addWidget(right);
            host.resize(1500, 500);
            left->setBuffer(0x1000, MakePattern(4096));
            right->setBuffer(0x9000, MakePattern(4096, 3));
            const bool activated = ActivateWindow(&host);
            CHECK(activated);
            Flush();

            // 每个 HexView 的快捷键：五个，全部是 WidgetWithChildren 上下文（旧控件是窗口级，会冲突）。
            for (HexView* view : { left, right })
            {
                const QList<QShortcut*> shortcuts = view->findChildren<QShortcut*>(Qt::FindDirectChildrenOnly);
                CHECK(shortcuts.size() == 5);
                bool allScoped = true;
                for (QShortcut* shortcut : shortcuts)
                {
                    allScoped = allScoped && shortcut->context() == Qt::WidgetWithChildrenShortcut;
                }
                CHECK(allScoped);
            }

            // Esc 快捷键：没有条打开时禁用。
            const auto escapeOf = [](HexView* view) -> QShortcut* {
                for (QShortcut* shortcut : view->findChildren<QShortcut*>(Qt::FindDirectChildrenOnly))
                {
                    if (shortcut->key() == QKeySequence(Qt::Key_Escape))
                    {
                        return shortcut;
                    }
                }
                return nullptr;
            };
            CHECK(escapeOf(left) != nullptr && !escapeOf(left)->isEnabled());

            // 焦点在左边：Ctrl+F 只打开左边的查找条。
            left->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            CHECK(QApplication::focusWidget() == left->canvas());
            QTest::keyClick(left->canvas(), Qt::Key_F, Qt::ControlModifier);
            Flush();
            CHECK(left->findBar()->isVisible());
            CHECK(!right->findBar()->isVisible());
            CHECK(left->findBar()->lineEdit()->hasFocus());
            CHECK(escapeOf(left)->isEnabled());
            CHECK(!escapeOf(right)->isEnabled());

            // 焦点移到右边：Ctrl+F 只打开右边的；Ctrl+G 只打开右边的跳转条。
            right->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QTest::keyClick(right->canvas(), Qt::Key_F, Qt::ControlModifier);
            Flush();
            CHECK(right->findBar()->isVisible());
            CHECK(left->findBar()->isVisible());
            right->canvas()->setFocus(Qt::OtherFocusReason);
            QTest::keyClick(right->canvas(), Qt::Key_G, Qt::ControlModifier);
            Flush();
            CHECK(right->gotoBar()->isVisible());
            CHECK(!left->gotoBar()->isVisible());
            CHECK(right->gotoBar()->lineEdit()->hasFocus());

            // Esc：焦点在跳转条里先关跳转条（查找条保持打开），焦点回到画布。
            QTest::keyClick(right->gotoBar()->lineEdit(), Qt::Key_Escape);
            Flush();
            CHECK(!right->gotoBar()->isVisible());
            CHECK(right->findBar()->isVisible());
            CHECK(QApplication::focusWidget() == right->canvas());

            // 再 Esc：关查找条；左边的条不受影响。
            QTest::keyClick(right->canvas(), Qt::Key_Escape);
            Flush();
            CHECK(!right->findBar()->isVisible());
            CHECK(left->findBar()->isVisible());
            CHECK(!escapeOf(right)->isEnabled());

            // 两个条都关着：Esc 不被快捷键吞掉，落到画布自己的处理（折叠选区）。
            right->canvas()->setFocus(Qt::OtherFocusReason);
            right->canvas()->setCaretAddress(0x9000, false, false);
            right->canvas()->setCaretAddress(0x900F, true, false);
            CHECK(right->canvas()->selectedRange().has_value()
                && right->canvas()->selectedRange()->last - right->canvas()->selectedRange()->first == 15);
            QTest::keyClick(right->canvas(), Qt::Key_Escape);
            Flush();
            CHECK(right->canvas()->selectedRange().has_value()
                && right->canvas()->selectedRange()->first == right->canvas()->selectedRange()->last);

            // 焦点在窗口里的第三个控件时，两个 HexView 的快捷键都不触发。
            QLineEdit outsider(&host);
            outsider.setGeometry(0, 0, 100, 24);
            outsider.show();
            outsider.setFocus(Qt::OtherFocusReason);
            Flush();
            CHECK(QApplication::focusWidget() == &outsider);
            QTest::keyClick(&outsider, Qt::Key_G, Qt::ControlModifier);
            Flush();
            CHECK(!left->gotoBar()->isVisible() && !right->gotoBar()->isVisible());

            // 工具栏隐藏后快捷键仍然有效。
            right->setToolbarVisible(false);
            right->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QTest::keyClick(right->canvas(), Qt::Key_G, Qt::ControlModifier);
            Flush();
            CHECK(right->gotoBar()->isVisible());
            right->closeGotoBar();

            // F3：查找条没开时先打开；输入框为空就停在打开。
            left->closeFindBar();
            left->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QSignalSpy leftMatch(left->findBar(), &ks::ui::HexFindBar::matchFound);
            QTest::keyClick(left->canvas(), Qt::Key_F3);
            Flush();
            CHECK(left->findBar()->isVisible());
            CHECK(leftMatch.count() == 0);

            // 输入框里有文字：F3 / Shift+F3 在焦点位于画布时直接搜索。
            left->findBar()->setPatternText(QStringLiteral("%1").arg(
                static_cast<int>(static_cast<std::uint8_t>(MakePattern(4096).at(0x20))), 2, 16, QLatin1Char('0')));
            left->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QTest::keyClick(left->canvas(), Qt::Key_F3);
            CHECK(PumpUntil([&]() { return leftMatch.count() >= 1; }, 3000));
            const int afterNext = leftMatch.count();
            QTest::keyClick(left->canvas(), Qt::Key_F3, Qt::ShiftModifier);
            CHECK(PumpUntil([&]() { return leftMatch.count() > afterNext; }, 3000));
            CHECK(right->findBar()->resultText().isEmpty());
        }

        // 状态条：三段文字、64 位地址位数、瞬时消息替换与恢复。
        void TestStatusBar()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x1000, MakePattern(256), true, QSize(900, 420));
            ks::ui::HexViewStatusBar* status = view->statusBar();
            CHECK(status->caretText() == QStringLiteral("插入点 0x00001000"));
            CHECK(status->selectionText() == QStringLiteral("选区 1 字节"));
            CHECK(status->rangeText() == QStringLiteral("范围 0x00001000 - 0x000010FF，共 256 字节"));
            CHECK(!status->hasMessage());

            // 点击与扩选：插入点与选区长度跟随。
            ClickAddress(*view->canvas(), 0x1020);
            CHECK(status->caretText() == QStringLiteral("插入点 0x00001020"));
            ClickAddress(*view->canvas(), 0x102F, Pane::Hex, Qt::ShiftModifier);
            CHECK(status->selectionText() == QStringLiteral("选区 16 字节"));

            // 跳转失败：瞬时消息替换三段，种类为警告，插入点不动；下一次选区变化后恢复。
            CHECK(!view->jumpToAddress(0x3000));
            CHECK(status->hasMessage());
            CHECK(status->messageKind() == ks::ui::HexViewStatusBar::Kind::Warning);
            CHECK(status->messageText().contains(QStringLiteral("超出数据范围 [0x00001000, 0x000010FF]")));
            CHECK(status->messageText().contains(QStringLiteral("0x00003000")));
            CHECK(view->caretAddress() == 0x102F);
            ClickAddress(*view->canvas(), 0x1030);
            CHECK(!status->hasMessage());
            CHECK(status->caretText() == QStringLiteral("插入点 0x00001030"));

            // 编辑被拒绝：状态条显示原因并转发 editRejected。
            view->setEditable(false);
            QSignalSpy rejected(view.get(), &HexView::editRejected);
            view->canvas()->fillSelection(0xFF);
            CHECK(rejected.count() == 1);
            CHECK(status->hasMessage());
            CHECK(status->messageText().contains(QStringLiteral("只读")));

            // 64 位地址：位数 16。
            view->setBuffer(0x00007FF600001000ULL, MakePattern(128));
            CHECK(status->caretText() == QStringLiteral("插入点 0x00007FF600001000"));
            CHECK(status->rangeText() == QStringLiteral("范围 0x00007FF600001000 - 0x00007FF60000107F，共 128 字节"));

            // 清空：三段回到"无数据"。
            view->clearBuffer();
            CHECK(status->caretText() == QStringLiteral("无数据"));
            CHECK(status->selectionText().isEmpty());
            CHECK(status->rangeText().isEmpty());
        }
    }

    // 外壳验证入口。
    void RunHexViewCoreTests()
    {
        TestConstruction();
        TestToolbarButtonsAndMenus();
        TestHoverTips();
        TestVisibility();
        TestInspectorBehavior();
        TestInspectorPersistence();
        TestSettingsFailure();
        TestShortcuts();
        TestStatusBar();
    }
}
