// memwb_ui_tests.Inspector.Shots.cpp
// 作用：HexInspectorPanel 的布局、主题切换、截图与基准。
// - 布局：宽度可拖（真实拖动 QSplitter 手柄）、窄到最小宽度时列重新分配且没有横向滚动条、
//   高度不足时出现竖向滚动条且最后一行可到达；
// - 主题：切换深浅色不通知任何控件，下一次绘制就是新主题，回到原主题逐像素还原；
// - 截图：画布与面板并排，深/浅色各多个场景，输出到 shotsDir；
// - 基准：每次插入点移动的刷新耗时、整面板绘制耗时，结果写入 bench-inspector.txt。

#include "memwb_ui_inspector.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QScrollBar>
#include <QTextStream>

#include <iostream>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexInspectorPanel;
        using ksword::memwb::ByteOrder;
        using Pane = HexCanvas::ActivePane;

        // ShotPath：截图文件路径，形如 <shotsDir>/inspector-dark-basic.png。
        QString ShotPath(const QString& shotsDir, bool dark, const char* scene)
        {
            return QStringLiteral("%1/inspector-%2-%3.png")
                .arg(shotsDir)
                .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                .arg(QLatin1String(scene));
        }

        // Save：把控件抓图保存，并断言保存成功、尺寸非空。
        void Save(QWidget& widget, const QString& path)
        {
            const QPixmap shot = widget.grab();
            CHECK_NOTE(shot.save(path), path);
            CHECK(!shot.isNull() && shot.width() > 0 && shot.height() > 0);
        }

        // 布局：真实拖动分隔条、最小宽度、竖向滚动。
        void TestLayout()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 600), 400);
            HexInspectorPanel& panel = *scene->panel;
            ks::ui::HexInspectorRowView* rows = panel.rowView();
            ClickAddress(*scene->canvas, kInspectorBase + 0x10);
            CHECK(panel.width() == 400);

            // 真实拖动分隔条手柄向左 100 像素：面板变宽到约 500，画布变窄，面板内容不崩。
            QSplitterHandle* handle = scene->splitter->handle(1);
            const QPoint center = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, center);
            DragMove(handle, center + QPoint(-100, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, center + QPoint(-100, 0));
            QApplication::processEvents();
            CHECK_NOTE(panel.width() >= 495 && panel.width() <= 505, QString::number(panel.width()));

            // 宽面板：值列比十六进制列宽（值列拿走全部余量），所有列宽为正。
            CHECK(rows->valueCellRect(0).width() > rows->hexCellRect(0).width());
            CHECK(!rows->horizontalScrollBar()->isVisible());

            // 窄到最小宽度：每一行的值列与十六进制列都还有可用宽度，没有横向滚动条，面板不会比最小尺寸更窄。
            scene->splitter->setSizes({ 1100 - 260, 260 });
            QApplication::processEvents();
            CHECK_NOTE(panel.width() >= panel.minimumSizeHint().width(), QString::number(panel.width()));
            for (int index = 0; index < rows->rowCount(); ++index)
            {
                CHECK(rows->valueCellRect(index).width() >= 40);
                CHECK(rows->hexCellRect(index).width() >= 40);
            }
            CHECK(!rows->horizontalScrollBar()->isVisible());

            // 高度不足：出现竖向滚动条，选中最后一行后它完整落在视口内。
            scene->splitter->resize(1100, 300);
            QApplication::processEvents();
            CHECK(rows->verticalScrollBar()->maximum() > 0);
            rows->setCurrentRow(rows->rowCount() - 1);
            QApplication::processEvents();
            CHECK(rows->rowRect(rows->rowCount() - 1).bottom() <= rows->viewport()->height());
            CHECK(rows->rowRect(rows->rowCount() - 1).top() >= rows->headerHeight());
            rows->setCurrentRow(0);
            QApplication::processEvents();
            CHECK(rows->verticalScrollBar()->value() == 0);

            // 滚动之后点击的行号与屏幕位置一致：滚到底后点最后一行，当前行就是最后一行。
            rows->verticalScrollBar()->setValue(rows->verticalScrollBar()->maximum());
            QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, rows->rowRect(rows->rowCount() - 1).center());
            CHECK(rows->currentRow() == rows->rowCount() - 1);
        }

        // 主题：切换深浅色不通知任何控件，像素立刻跟随；回到原主题逐像素还原；图标按钮选中态在两套主题下都可区分。
        void TestThemeFollow()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            ks::ui::HexInspectorRowView* rows = panel.rowView();
            ClickAddress(*scene->canvas, kInspectorBase + 0x10);
            rows->setCurrentRow(0);

            // 取样点：行 5 的左边缘（普通行，底色就是表面色）、顶栏左上角（次表面色）、状态条右下角（表面色）。
            const auto sample = [&]() {
                struct Samples
                {
                    QImage rows;
                    QColor rowBackground;
                    QColor barBackground;
                    QColor statusBackground;
                };
                Samples result;
                result.rows = rows->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                result.rowBackground = result.rows.pixelColor(2, rows->rowRect(5).center().y());
                const QImage bar = panel.topBar()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                result.barBackground = bar.pixelColor(3, 3);
                const QImage status = panel.statusBar()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                result.statusBackground = status.pixelColor(status.width() - 3, status.height() - 3);
                return result;
            };

            const auto light = sample();
            CHECK(light.rowBackground == KswordTheme::SurfaceColor());
            CHECK(light.barBackground == KswordTheme::SurfaceAltColor());
            CHECK(light.statusBackground == KswordTheme::SurfaceColor());

            ApplyTheme(true);
            const auto dark = sample();
            CHECK(dark.rowBackground == KswordTheme::SurfaceColor());
            CHECK(dark.barBackground == KswordTheme::SurfaceAltColor());
            CHECK(dark.statusBackground == KswordTheme::SurfaceColor());
            CHECK(dark.rowBackground != light.rowBackground);
            CHECK(dark.rows != light.rows);

            // 图标按钮：选中与未选中在深浅两套主题下底色都明显不同（取按钮左缘、字形之外的底板像素）。
            for (const bool useDark : { false, true })
            {
                ApplyTheme(useDark);
                const QImage bar = panel.topBar()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QPoint checkedAt = panel.byteOrderButton(ByteOrder::Little)->geometry().topLeft() + QPoint(3, 12);
                const QPoint uncheckedAt = panel.byteOrderButton(ByteOrder::Big)->geometry().topLeft() + QPoint(3, 12);
                CHECK_NOTE(ColorDistance(bar.pixelColor(checkedAt), bar.pixelColor(uncheckedAt)) >= 30,
                    QStringLiteral("dark=%1 %2 vs %3").arg(useDark).arg(bar.pixelColor(checkedAt).name()).arg(bar.pixelColor(uncheckedAt).name()));
            }

            // 回到浅色：逐像素回到原样，没有任何缓存颜色残留。
            ApplyTheme(false);
            CHECK(sample().rows == light.rows);
        }

        // 截图：深/浅主题各 7 个场景。
        void SaveShots(const QString& shotsDir)
        {
            for (const bool dark : { true, false })
            {
                ApplyTheme(dark);

                // 1) 基础：画布选中 8 个字节，面板列出各类型，当前行是第一行。
                {
                    auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 540), 400);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x10);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x17, Pane::Hex, Qt::ShiftModifier);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x10);
                    scene->panel->rowView()->setCurrentRow(RowIndexOf(*scene->panel, QStringLiteral("u32")));
                    QTest::mouseMove(scene->panel->rowView()->viewport(), scene->panel->rowView()->rowRect(RowIndexOf(*scene->panel, QStringLiteral("f32"))).center());
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "basic"));
                }

                // 2) 指针与字符串：插入点在指针处，命名器给出描述；窄面板（最小宽度）看省略。
                {
                    auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 540), 400);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x40);
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "pointer"));
                    scene->splitter->setSizes({ 1100 - 270, 270 });
                    ClickAddress(*scene->canvas, kInspectorBase + 0x20);
                    QApplication::processEvents();
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "narrow-strings"));
                }

                // 3) 未读字节 + 大端：页里有 2 个读不到的字节，下一页整页不可读；面板里长一些的类型显示"不可用"。
                {
                    auto scene = MakeAsyncInspectorScene(false, QSize(1100, 540), 400);
                    QByteArray page = MakePattern(4096, 3);
                    QByteArray mask(4096, '\x01');
                    mask[0x12] = '\0';
                    mask[0x13] = '\0';
                    scene->canvas->deliverPage(kInspectorBase, page, mask, scene->canvas->sourceRevision());
                    scene->canvas->scrollToAddress(kInspectorBase, HexCanvas::ScrollAlign::Top);
                    scene->canvas->setCaretAddress(kInspectorBase + 0x10);
                    scene->panel->setByteOrder(ByteOrder::Big);
                    QApplication::processEvents();
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "unread-bigendian"));
                }

                // 4) 行内编辑出错：编辑器边框红色，状态条给出原因。
                {
                    auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 540), 400);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x10);
                    DoubleClickRow(*scene->panel, QStringLiteral("u32"));
                    TypeIntoEditor(*scene->panel, QStringLiteral("0x1FFFFFFFF"));
                    PressInEditor(*scene->panel, Qt::Key_Return);
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "edit-error"));

                    // 改正后提交：编辑器关闭，画布里出现暂存着色，状态条显示结果。
                    TypeIntoEditor(*scene->panel, QStringLiteral("0xCAFEBABE"));
                    PressInEditor(*scene->panel, Qt::Key_Return);
                    FlushDeferred();
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "edit-staged"));
                }

                // 5) 只读视图：双击被拒，状态条说明原因。
                {
                    auto scene = MakeInspectorScene(MakeInspectorData(), false, true, QSize(1100, 540), 400);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x10);
                    DoubleClickRow(*scene->panel, QStringLiteral("u32"));
                    Save(*scene->splitter, ShotPath(shotsDir, dark, "readonly"));
                }

                // 6) 右键菜单：不透明背景、图标、置灰项。
                {
                    auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 540), 400);
                    ClickAddress(*scene->canvas, kInspectorBase + 0x60);
                    QMenu* menu = scene->panel->buildRowMenu(RowIndexOf(*scene->panel, QStringLiteral("filetime")));
                    menu->popup(QPoint(20, 20));
                    QApplication::processEvents();
                    Save(*menu, ShotPath(shotsDir, dark, "context-menu"));
                    menu->close();
                    delete menu;
                }
            }
            ApplyTheme(false);
        }

        // 基准：插入点移动一次的刷新耗时（取字节 + 解释 + 重建行）、整面板绘制耗时、编辑提交耗时。
        void RunBench(const QString& shotsDir)
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexCanvas& canvas = *scene->canvas;

            // 刷新：3000 次插入点移动（不处理事件，只量同步刷新的工作量）。
            const int moves = 3000;
            QElapsedTimer timer;
            timer.start();
            for (int index = 0; index < moves; ++index)
            {
                canvas.setCaretAddress(kInspectorBase + static_cast<std::uint64_t>((index * 37) % 0x1F00), false, false);
            }
            const double refreshMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;

            // 绘制：整面板抓图 200 次（含 17 行、顶栏、状态条）。
            timer.restart();
            const int frames = 200;
            for (int index = 0; index < frames; ++index)
            {
                scene->panel->grab();
            }
            const double paintMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;

            // 编辑提交：双击 -> 键入 -> Enter，重复 50 次（含叠加层暂存与面板重建）。
            ClickAddress(canvas, kInspectorBase + 0x100);
            timer.restart();
            const int edits = 50;
            for (int index = 0; index < edits; ++index)
            {
                DoubleClickRow(*scene->panel, QStringLiteral("u32"));
                TypeIntoEditor(*scene->panel, QString::number(index + 1));
                PressInEditor(*scene->panel, Qt::Key_Return);
                FlushDeferred();
            }
            const double editMs = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;

            const QString line = QStringLiteral(
                "inspector bench: %1 caret moves refresh %2 ms total (%3 us/move); "
                "%4 panel grabs %5 ms total (%6 ms/frame); %7 inline edits %8 ms total (%9 ms/edit)")
                .arg(moves).arg(refreshMs, 0, 'f', 2).arg(refreshMs * 1000.0 / moves, 0, 'f', 1)
                .arg(frames).arg(paintMs, 0, 'f', 2).arg(paintMs / frames, 0, 'f', 3)
                .arg(edits).arg(editMs, 0, 'f', 2).arg(editMs / edits, 0, 'f', 3);
            std::cout << line.toStdString() << std::endl;
            QFile file(QDir(shotsDir).filePath(QStringLiteral("../bench-inspector.txt")));
            if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                QTextStream stream(&file);
                stream << line << "\n";
            }

            // 回归护栏（很宽松，只拦数量级退化）：单次刷新 < 2 ms，单帧绘制 < 20 ms。
            CHECK_NOTE(refreshMs / moves < 2.0, line);
            CHECK_NOTE(paintMs / frames < 20.0, line);
        }
    }

    // 本文件全部测试、截图与基准的入口。
    void RunInspectorShotsAndBench(const QString& shotsDir)
    {
        QDir().mkpath(shotsDir);
        TestLayout();
        TestThemeFollow();
        SaveShots(shotsDir);
        RunBench(shotsDir);
        ApplyTheme(false);
    }
}
