// memwb_ui_bench.cpp
// 作用：HexCanvas 的离屏渲染基准，以及旧机制（QTableWidget 每字节一个单元格）的对照基线。
// 结果写入 benchFile 并同时打印到 stdout。画布稳态单帧绘制有一条粗略断言；旧机制基线只测量不断言。
//
// 测量项：
//   (a) 构造画布 + setStaticData(2 MiB) + 显示 + 首屏绘制完成；
//   (b) 连续 100 次单行滚动，每次整视口 grab，取平均与 p95；
//   (c) 全选 2 MiB 后重绘；
//   (d) 在 1 MiB 区域内 1000 次鼠标移动的拖选总耗时（含逐次重绘与不含重绘两种）；
//   (e) 旧机制对照：QTableWidget 每字节一个 QTableWidgetItem 外加每行一个 ASCII 单元格
//       （每行 8 字节，约 1.125N 个单元格），先 clearContents 再整表重建，
//       在 N = 64 KiB / 1 MiB / 2 MiB 下测重建耗时与全选后的选区变化耗时。

#include "memwb_ui_common.h"

#include <QApplication>
#include <QBrush>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTextStream>

#include <algorithm>
#include <iostream>
#include <numeric>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Pane = HexCanvas::ActivePane;

        // kViewport：基准统一使用的视口大小。
        const QSize kViewport(1200, 800);

        // Ms：纳秒转毫秒。
        double Ms(qint64 nanoseconds)
        {
            return static_cast<double>(nanoseconds) / 1.0e6;
        }

        // Percentile：取已排序样本的分位数（p 为 0..1）。
        double Percentile(std::vector<double> samples, double p)
        {
            std::sort(samples.begin(), samples.end());
            const std::size_t index = std::min(samples.size() - 1, static_cast<std::size_t>(p * static_cast<double>(samples.size())));
            return samples[index];
        }

        // Mean：平均值。
        double Mean(const std::vector<double>& samples)
        {
            return std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size());
        }

        // Log：同时打印到 stdout 并追加到结果文本。
        void Log(QString& report, const QString& line)
        {
            std::cout << line.toStdString() << std::endl;
            report += line + QLatin1Char('\n');
        }

        // (a) 构造 + 2 MiB 静态数据 + 首屏绘制。
        void BenchConstruct(QString& report)
        {
            const QByteArray data = MakePattern(2 * 1024 * 1024);
            std::vector<double> runs;
            for (int run = 0; run < 6; ++run)
            {
                QElapsedTimer timer;
                timer.start();
                {
                    HexCanvas canvas;
                    canvas.resize(kViewport);
                    canvas.show();
                    canvas.setStaticData(0x400000, data);
                    const QPixmap frame = canvas.grab();
                    CHECK(!frame.isNull());
                    CHECK(canvas.cellStateAt(0x400000).hasValue);
                    runs.push_back(Ms(timer.nsecsElapsed()));
                }
            }
            std::vector<double> warm(runs.begin() + 1, runs.end());
            Log(report, QStringLiteral("(a) 构造画布 + setStaticData(2 MiB) + 显示 + 首屏绘制完成：首次 %1 ms；之后 5 次 中位数 %2 ms，最小 %3 ms，最大 %4 ms")
                .arg(runs.front(), 0, 'f', 2)
                .arg(Percentile(warm, 0.5), 0, 'f', 2)
                .arg(*std::min_element(warm.begin(), warm.end()), 0, 'f', 2)
                .arg(*std::max_element(warm.begin(), warm.end()), 0, 'f', 2));
        }

        // (b) 连续 100 次单行滚动，每次整视口 grab。
        void BenchScroll(QString& report)
        {
            auto fixture = MakeStaticFixture(0x400000, MakePattern(2 * 1024 * 1024), false, kViewport, false);
            HexCanvas& canvas = *fixture->canvas;
            canvas.setCaretAddress(0x400000 + 0x1234);
            canvas.setCaretAddress(0x400000 + 0x4321, true);
            for (int warm = 0; warm < 5; ++warm)
            {
                canvas.grab();
            }

            // 100 次：每次滚一行，再整视口 grab（grab 会同步触发一次完整绘制）。
            std::vector<double> frames;
            for (int step = 0; step < 100; ++step)
            {
                QElapsedTimer timer;
                timer.start();
                canvas.setFirstVisibleRow(1000 + static_cast<std::uint64_t>(step));
                const QPixmap frame = canvas.grab();
                frames.push_back(Ms(timer.nsecsElapsed()));
                CHECK(!frame.isNull());
            }
            const double mean = Mean(frames);
            const double p95 = Percentile(frames, 0.95);
            Log(report, QStringLiteral("(b) %1x%2 视口，100 次单行滚动 + 整视口 grab：平均 %3 ms，p95 %4 ms，最大 %5 ms；绘制行数 %6")
                .arg(kViewport.width()).arg(kViewport.height())
                .arg(mean, 0, 'f', 3).arg(p95, 0, 'f', 3)
                .arg(*std::max_element(frames.begin(), frames.end()), 0, 'f', 3)
                .arg(canvas.lastPaintedRowCount()));

            // 稳态单帧绘制的粗略断言：平均与 p95 都低于 50 ms。
            CHECK_NOTE(mean < 50.0, QString::number(mean));
            CHECK_NOTE(p95 < 50.0, QString::number(p95));
        }

        // (c) 全选 2 MiB 后重绘。
        void BenchSelectAll(QString& report)
        {
            auto fixture = MakeStaticFixture(0x400000, MakePattern(2 * 1024 * 1024), false, kViewport, false);
            HexCanvas& canvas = *fixture->canvas;
            canvas.grab();

            std::vector<double> runs;
            for (int run = 0; run < 20; ++run)
            {
                canvas.setCaretAddress(0x400000);
                canvas.grab();
                QElapsedTimer timer;
                timer.start();
                canvas.selectAll();
                canvas.grab();
                runs.push_back(Ms(timer.nsecsElapsed()));
            }
            CHECK(canvas.selectedRange()->last - canvas.selectedRange()->first + 1 == 2ULL * 1024ULL * 1024ULL);
            Log(report, QStringLiteral("(c) 全选 2 MiB 后重绘（selectAll + grab，20 次）：中位数 %1 ms，p95 %2 ms；绘制行数 %3")
                .arg(Percentile(runs, 0.5), 0, 'f', 3)
                .arg(Percentile(runs, 0.95), 0, 'f', 3)
                .arg(canvas.lastPaintedRowCount()));
        }

        // (d) 1 MiB 区域内 1000 次鼠标移动的拖选。
        void BenchDrag(QString& report)
        {
            const int oneMiB = 1024 * 1024;
            auto fixture = MakeStaticFixture(0x400000, MakePattern(oneMiB), false, kViewport, false);
            HexCanvas& canvas = *fixture->canvas;
            canvas.grab();
            QWidget* view = canvas.viewport();
            const std::uint64_t totalRows = static_cast<std::uint64_t>(oneMiB) / 16ULL;

            // 两种口径：moveOnly 只投递鼠标事件；withPaint 每次移动后处理事件循环（包含重绘）。
            for (const bool withPaint : { false, true })
            {
                canvas.setFirstVisibleRow(0);
                canvas.setCaretAddress(0x400000);
                QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, CellCenter(canvas, 0x400000, Pane::Hex));
                QElapsedTimer timer;
                timer.start();
                for (int step = 0; step < 1000; ++step)
                {
                    // 模拟拖选时的自动滚动：每次移动前把首行推进约 1/1000，整个 1 MiB 区域被扫过一遍。
                    canvas.setFirstVisibleRow(static_cast<std::uint64_t>(step) * (totalRows / 1000ULL));
                    const int x = 120 + (step * 37) % 700;
                    const int y = 40 + (step * 53) % 700;
                    DragMove(view, QPoint(x, y));
                    if (withPaint)
                    {
                        QApplication::processEvents();
                    }
                }
                const double total = Ms(timer.nsecsElapsed());
                QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, QPoint(300, 300));
                const std::uint64_t selected = canvas.selectedRange()->last - canvas.selectedRange()->first + 1ULL;
                CHECK(selected > static_cast<std::uint64_t>(oneMiB) * 9ULL / 10ULL);
                Log(report, QStringLiteral("(d) 1 MiB 区域 1000 次鼠标移动拖选（%1）：总耗时 %2 ms，平均 %3 ms/次；最终选区 %4 字节")
                    .arg(withPaint ? QStringLiteral("含逐次重绘") : QStringLiteral("仅选区更新"))
                    .arg(total, 0, 'f', 2)
                    .arg(total / 1000.0, 0, 'f', 4)
                    .arg(selected));
            }
        }

        // (e) 旧机制对照：QTableWidget 每字节一个单元格 + 每行一个 ASCII 单元格，整表重建。
        void BenchOldTable(QString& report)
        {
            for (const int bytes : { 64 * 1024, 1024 * 1024, 2 * 1024 * 1024 })
            {
                QTableWidget* table = new QTableWidget();
                table->resize(kViewport);
                table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                table->setSelectionMode(QAbstractItemView::ExtendedSelection);
                table->show();
                QApplication::processEvents();

                const int rows = bytes / 8;
                const QByteArray data = MakePattern(bytes);

                // 重建：clearContents + 设定行列 + 逐单元格 new QTableWidgetItem；更新被禁用以给旧做法最宽松的口径。
                QElapsedTimer timer;
                timer.start();
                table->setUpdatesEnabled(false);
                table->clearContents();
                table->setRowCount(rows);
                table->setColumnCount(9);
                for (int row = 0; row < rows; ++row)
                {
                    QString ascii;
                    ascii.reserve(8);
                    for (int column = 0; column < 8; ++column)
                    {
                        const unsigned char value = static_cast<unsigned char>(data.at(row * 8 + column));
                        table->setItem(row, column, new QTableWidgetItem(QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper()));
                        ascii.append((value >= 0x20 && value <= 0x7E) ? QChar(value) : QLatin1Char('.'));
                    }
                    table->setItem(row, 8, new QTableWidgetItem(ascii));
                }
                table->setUpdatesEnabled(true);
                const double buildMs = Ms(timer.nsecsElapsed());
                timer.restart();
                table->grab();
                const double firstPaintMs = Ms(timer.nsecsElapsed());

                // 全选后的选区变化：(1) 原生 selectAll（只更新 selectionModel）；
                // (2) 旧控件的做法——再给每个被选单元格设一遍选中背景画刷（"三头同步"之一）。
                timer.restart();
                table->selectAll();
                const double selectAllMs = Ms(timer.nsecsElapsed());
                timer.restart();
                table->grab();
                const double selectPaintMs = Ms(timer.nsecsElapsed());
                timer.restart();
                const QBrush brush(QColor(64, 128, 255));
                {
                    // 给旧做法最宽松的口径：刷新被禁用、模型信号被屏蔽，只统计逐单元格写画刷本身。
                    QSignalBlocker blocker(table->model());
                    table->setUpdatesEnabled(false);
                    for (int row = 0; row < rows; ++row)
                    {
                        for (int column = 0; column < 9; ++column)
                        {
                            table->item(row, column)->setBackground(brush);
                        }
                    }
                    table->setUpdatesEnabled(true);
                }
                const double brushMs = Ms(timer.nsecsElapsed());

                Log(report, QStringLiteral("(e) 旧机制 QTableWidget N=%1 KiB（%2 个单元格）：整表重建 %3 ms，重建后首屏 grab %4 ms；"
                    "全选 selectAll %5 ms，全选后重绘 %6 ms，再给全部单元格设选中画刷 %7 ms")
                    .arg(bytes / 1024)
                    .arg(static_cast<qint64>(rows) * 9)
                    .arg(buildMs, 0, 'f', 1)
                    .arg(firstPaintMs, 0, 'f', 1)
                    .arg(selectAllMs, 0, 'f', 1)
                    .arg(selectPaintMs, 0, 'f', 1)
                    .arg(brushMs, 0, 'f', 1));
                delete table;
            }
        }
    }

    // 基准入口：写 benchFile。
    void RunBenchmarks(const QString& benchFile)
    {
        ApplyTheme(false);
        QString report;
        Log(report, QStringLiteral("HexCanvas 离屏基准（offscreen 平台，视口 %1x%2，Release /O2）").arg(kViewport.width()).arg(kViewport.height()));
        BenchConstruct(report);
        BenchScroll(report);
        BenchSelectAll(report);
        BenchDrag(report);
        BenchOldTable(report);

        QFile file(benchFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            file.write(report.toUtf8());
        }
        CHECK(file.exists());
    }
}
