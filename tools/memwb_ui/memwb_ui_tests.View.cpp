// memwb_ui_tests.View.cpp
// 作用：HexCanvas 的显示、选区、键盘、滚动、页协议、超大地址等"只读侧"行为验证。
// 全部通过 QTest 模拟真实鼠标/键盘事件驱动画布，并用 cellStateAt/cellRect/抓图像素核对结果。

#include "memwb_ui_common.h"

#include <QApplication>
#include <QLineEdit>
#include <QScrollBar>
#include <QToolTip>
#include <QHelpEvent>
#include <QVBoxLayout>
#include <QSignalSpy>
#include <QWheelEvent>

#include <climits>
#include <set>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Pane = HexCanvas::ActivePane;

        // kMax：64 位地址上限。
        constexpr std::uint64_t kMax = 0xFFFFFFFFFFFFFFFFULL;

        // RegionIsColor：检查图像里一个矩形的每个像素是否都等于给定底色。
        // 传入：图像、矩形、底色；传出：全部相同返回 true。
        bool RegionIsColor(const QImage& image, const QRect& region, const QColor& color)
        {
            const QRect clipped = region.intersected(image.rect());
            if (clipped.isEmpty())
            {
                return false;
            }
            for (int y = clipped.top(); y <= clipped.bottom(); ++y)
            {
                for (int x = clipped.left(); x <= clipped.right(); ++x)
                {
                    if (image.pixelColor(x, y) != color)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // CountNonColor：矩形内与底色不同的像素数（正向对照，证明"有字"）。
        int CountNonColor(const QImage& image, const QRect& region, const QColor& color)
        {
            int count = 0;
            const QRect clipped = region.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y)
            {
                for (int x = clipped.left(); x <= clipped.right(); ++x)
                {
                    if (image.pixelColor(x, y) != color)
                    {
                        ++count;
                    }
                }
            }
            return count;
        }

        // 基本显示：状态、文字，以及"补空位处不画字节"（像素级）。
        void TestBasicDisplay()
        {
            ApplyTheme(false);
            // 数据起点 0x1005：首行 0x1000..0x1004 是补空位。
            auto fixture = MakeStaticFixture(0x1005, MakePattern(300), false, QSize(900, 400));
            HexCanvas& canvas = *fixture->canvas;

            CHECK(!canvas.cellStateAt(0x1004).inSpace);
            CHECK(canvas.cellRect(0x1004, Pane::Hex).isNull());
            const HexCanvas::CellState first = canvas.cellStateAt(0x1005);
            CHECK(first.inSpace && first.hasValue);
            CHECK(first.value == static_cast<std::uint8_t>(fixture->data.at(0)));
            CHECK(first.hexText == QStringLiteral("%1").arg(first.value, 2, 16, QLatin1Char('0')).toUpper());
            CHECK(first.byteState == HexCanvas::ByteState::Valid);

            // 像素：补空位列必须只有底色；有效列必须有字（避免检查空转）。
            const QImage image = GrabImage(canvas);
            const QColor surface = KswordTheme::SurfaceColor();
            const QRect rect5 = canvas.cellRect(0x1005, Pane::Hex);
            const QRect rect6 = canvas.cellRect(0x1006, Pane::Hex);
            const int pitch = rect6.x() - rect5.x();
            const QRect padHex(rect5.x() - pitch, rect5.y(), rect5.width(), rect5.height());
            CHECK(RegionIsColor(image, padHex, surface));
            const QRect padHex0(rect5.x() - 5 * pitch, rect5.y(), rect5.width(), rect5.height());
            CHECK(RegionIsColor(image, padHex0, surface));
            CHECK(CountNonColor(image, rect6, surface) > 8);

            const QRect ascii5 = canvas.cellRect(0x1005, Pane::Ascii);
            const QRect padAscii(ascii5.x() - 2 * ascii5.width(), ascii5.y(), ascii5.width(), ascii5.height());
            CHECK(RegionIsColor(image, padAscii, surface));
            CHECK(CountNonColor(image, canvas.cellRect(0x1007, Pane::Ascii), surface) > 2);

            // 数据尾部：0x1005+300-1 = 0x1130，其后一格是末行补空位。
            const std::uint64_t lastAddress = 0x1005 + 300 - 1;
            CHECK(canvas.cellStateAt(lastAddress).inSpace);
            CHECK(!canvas.cellStateAt(lastAddress + 1).inSpace);
            canvas.scrollToAddress(lastAddress, HexCanvas::ScrollAlign::Nearest);
            const QImage tail = GrabImage(canvas);
            const QRect lastRect = canvas.cellRect(lastAddress, Pane::Hex);
            const QRect afterRect(lastRect.x() + pitch, lastRect.y(), lastRect.width(), lastRect.height());
            CHECK(!lastRect.isNull());
            CHECK(RegionIsColor(tail, afterRect, surface));

            // 地址列位数：上界不超过 0xFFFFFFFF 时 8 位，否则 16 位（通过几何差异间接验证）。
            const int narrowWidth = canvas.sizeHint().width();
            canvas.setAddressSpace(0, 0x100000000ULL);
            CHECK(canvas.sizeHint().width() > narrowWidth);
        }

        // 鼠标：拖选、Shift 扩选、面板切换、反向拖选、点击地址列选整行、拖出视口自动滚动。
        void TestMouseSelection()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), false, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy selectionSpy(&canvas, &HexCanvas::selectionChanged);
            QSignalSpy caretSpy(&canvas, &HexCanvas::caretMoved);

            // 单击：单字节选区，活动面板为 Hex。
            Click(canvas, 0x1010);
            CHECK(canvas.selectedRange().has_value());
            CHECK(canvas.selectedRange()->first == 0x1010 && canvas.selectedRange()->last == 0x1010);
            CHECK(canvas.activePane() == Pane::Hex);
            CHECK(canvas.caretAddress() == 0x1010);
            CHECK(!selectionSpy.isEmpty());
            CHECK(!caretSpy.isEmpty());

            // 拖选：按下 0x1010，移动到 0x1033，释放。
            QWidget* view = canvas.viewport();
            QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, CellCenter(canvas, 0x1010, Pane::Hex));
            DragMove(view, CellCenter(canvas, 0x1020, Pane::Hex));
            DragMove(view, CellCenter(canvas, 0x1033, Pane::Hex));
            QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, CellCenter(canvas, 0x1033, Pane::Hex));
            CHECK(canvas.selectedRange()->first == 0x1010 && canvas.selectedRange()->last == 0x1033);
            CHECK(canvas.caretAddress() == 0x1033);

            // Shift+点击扩选：锚点不动（锚点 0x1010）。
            Click(canvas, 0x1050, Pane::Hex, Qt::ShiftModifier);
            CHECK(canvas.selectedRange()->first == 0x1010 && canvas.selectedRange()->last == 0x1050);
            Click(canvas, 0x1004, Pane::Hex, Qt::ShiftModifier);
            CHECK(canvas.selectedRange()->first == 0x1004 && canvas.selectedRange()->last == 0x1010);
            CHECK(canvas.caretAddress() == 0x1004);

            // ASCII 面板单击：面板切换，选区折叠。
            Click(canvas, 0x1020, Pane::Ascii);
            CHECK(canvas.activePane() == Pane::Ascii);
            CHECK(canvas.selectedRange()->first == 0x1020 && canvas.selectedRange()->last == 0x1020);

            // 反向拖选（从右下拖到左上）：选区端点排序正确，插入点在拖动终点。
            QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, CellCenter(canvas, 0x1030, Pane::Ascii));
            DragMove(view, CellCenter(canvas, 0x1015, Pane::Ascii));
            QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, CellCenter(canvas, 0x1015, Pane::Ascii));
            CHECK(canvas.selectedRange()->first == 0x1015 && canvas.selectedRange()->last == 0x1030);
            CHECK(canvas.caretAddress() == 0x1015);

            // 点击地址列：选中整行 0x1030..0x103F。
            const QRect rowCell = canvas.cellRect(0x1030, Pane::Hex);
            QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, QPoint(6, rowCell.center().y()));
            CHECK(canvas.selectedRange()->first == 0x1030 && canvas.selectedRange()->last == 0x103F);

            // 拖出视口下沿：定时器自动滚动，选区向下延伸。
            canvas.setFirstVisibleRow(0);
            Click(canvas, 0x1000, Pane::Hex);
            const std::uint64_t rowBefore = canvas.firstVisibleRow();
            QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, CellCenter(canvas, 0x1000, Pane::Hex));
            DragMove(view, QPoint(CellCenter(canvas, 0x1000, Pane::Hex).x(), view->height() + 40));
            // 先不手动推进：让真实的自动滚动定时器自己滚一会儿，首行必须前进。
            const std::uint64_t rowAtEdge = canvas.firstVisibleRow();
            QTest::qWait(250);
            CHECK(canvas.firstVisibleRow() > rowAtEdge);
            for (int tick = 0; tick < 6; ++tick)
            {
                canvas.autoScrollTick();
            }
            QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, QPoint(10, view->height() + 40));
            CHECK(canvas.firstVisibleRow() > rowBefore + 5);
            CHECK(canvas.selectedRange()->last > 0x1000 + 16 * canvas.visibleRowCount());
            CHECK(canvas.selectedRange()->first == 0x1000);
        }

        // 键盘：移动、Shift 扩选、Home/End、PgUp/PgDn、Ctrl+Home/End、Ctrl+A、Tab、Esc，且不回绕不越界。
        void TestKeyboardNavigation()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), false, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            canvas.setCaretAddress(0x1000);

            Key(canvas, Qt::Key_Right);
            CHECK(canvas.caretAddress() == 0x1001);
            Key(canvas, Qt::Key_Left);
            Key(canvas, Qt::Key_Left);
            CHECK(canvas.caretAddress() == 0x1000);   // 不越过空间起点，更不会回绕
            Key(canvas, Qt::Key_Down);
            CHECK(canvas.caretAddress() == 0x1010);
            Key(canvas, Qt::Key_Up);
            Key(canvas, Qt::Key_Up);
            CHECK(canvas.caretAddress() == 0x1000);

            // Shift+方向键扩选：锚点固定。
            for (int step = 0; step < 3; ++step)
            {
                Key(canvas, Qt::Key_Right, Qt::ShiftModifier);
            }
            CHECK(canvas.selectedRange()->first == 0x1000 && canvas.selectedRange()->last == 0x1003);
            Key(canvas, Qt::Key_Down, Qt::ShiftModifier);
            CHECK(canvas.selectedRange()->first == 0x1000 && canvas.selectedRange()->last == 0x1013);

            // 不带 Shift 的移动折叠选区；Home/End 到行首/行尾。
            Key(canvas, Qt::Key_Home);
            CHECK(canvas.caretAddress() == 0x1010);
            CHECK(canvas.selectedRange()->first == 0x1010 && canvas.selectedRange()->last == 0x1010);
            Key(canvas, Qt::Key_End);
            CHECK(canvas.caretAddress() == 0x101F);
            Key(canvas, Qt::Key_Home, Qt::ShiftModifier);
            CHECK(canvas.selectedRange()->first == 0x1010 && canvas.selectedRange()->last == 0x101F);

            // PgDn/PgUp：视图与插入点一起移动整整一屏。
            canvas.setCaretAddress(0x1000);
            const std::uint64_t pageRows = canvas.visibleRowCount();
            const std::uint64_t topBefore = canvas.firstVisibleRow();
            Key(canvas, Qt::Key_PageDown);
            CHECK(canvas.firstVisibleRow() == topBefore + pageRows);
            CHECK(canvas.caretAddress() == 0x1000 + 16 * pageRows);
            Key(canvas, Qt::Key_PageUp);
            CHECK(canvas.firstVisibleRow() == topBefore);
            CHECK(canvas.caretAddress() == 0x1000);
            Key(canvas, Qt::Key_PageUp);
            CHECK(canvas.caretAddress() == 0x1000);   // 到顶不越界

            // Ctrl+End/Ctrl+Home：地址空间首尾。到达末尾后再向后/向下仍然停在末尾。
            Key(canvas, Qt::Key_End, Qt::ControlModifier);
            CHECK(canvas.caretAddress() == 0x1FFF);
            Key(canvas, Qt::Key_Right);
            Key(canvas, Qt::Key_Down);
            Key(canvas, Qt::Key_PageDown);
            CHECK(canvas.caretAddress() == 0x1FFF);
            Key(canvas, Qt::Key_Home, Qt::ControlModifier);
            CHECK(canvas.caretAddress() == 0x1000);
            CHECK(canvas.firstVisibleRow() == 0);
            Key(canvas, Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(canvas.selectedRange()->first == 0x1000 && canvas.selectedRange()->last == 0x1FFF);

            // Ctrl+A 全选；Esc 折叠到插入点。
            Key(canvas, Qt::Key_Home, Qt::ControlModifier);
            Key(canvas, Qt::Key_A, Qt::ControlModifier);
            CHECK(canvas.selectedRange()->first == 0x1000 && canvas.selectedRange()->last == 0x1FFF);
            Key(canvas, Qt::Key_Escape);
            CHECK(canvas.selectedRange()->first == canvas.selectedRange()->last);
            CHECK(canvas.selectedRange()->first == canvas.caretAddress());

            // Tab / Shift+Tab 切换面板，地址不变。
            canvas.setCaretAddress(0x1234);
            CHECK(canvas.activePane() == Pane::Hex);
            Key(canvas, Qt::Key_Tab);
            CHECK(canvas.activePane() == Pane::Ascii);
            CHECK(canvas.caretAddress() == 0x1234);
            Key(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
            CHECK(canvas.activePane() == Pane::Hex);
        }

        // 超大地址：0x00007FFF00000000 与接近 UINT64_MAX，不崩、不回绕，行列互转正确。
        void TestHugeAddresses()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            RecordingProvider provider;
            canvas.resize(900, 360);
            canvas.show();
            canvas.setAddressSpace(0, kMax);
            canvas.setPageProvider(&provider);

            // 跳到用户态高地址：首行 = 地址 / 16。
            CHECK(canvas.scrollToAddress(0x00007FFF00000000ULL, HexCanvas::ScrollAlign::Top));
            CHECK(canvas.firstVisibleRow() == 0x00007FFF00000000ULL / 16ULL);
            canvas.setCaretAddress(0x00007FFF00000000ULL);
            CHECK(canvas.caretAddress() == 0x00007FFF00000000ULL);
            CHECK(!canvas.cellRect(0x00007FFF00000000ULL, Pane::Hex).isNull());
            CHECK(!canvas.cellRect(0x00007FFF0000000FULL, Pane::Hex).isNull());
            CHECK(canvas.cellRect(0x00007FFF0000000FULL, Pane::Hex).x() > canvas.cellRect(0x00007FFF00000000ULL, Pane::Hex).x());
            CHECK(canvas.cellRect(0x00007FFF00000010ULL, Pane::Hex).y() > canvas.cellRect(0x00007FFF00000000ULL, Pane::Hex).y());

            // 接近 UINT64_MAX：首行夹取到最大首行，末字节可见，向后移动一律停住。
            const std::size_t requestMark = provider.requests.size();
            CHECK(canvas.scrollToAddress(kMax, HexCanvas::ScrollAlign::Center));
            CHECK(!canvas.cellRect(kMax, Pane::Hex).isNull());
            CHECK(!canvas.cellRect(kMax, Pane::Ascii).isNull());
            canvas.setCaretAddress(kMax);
            CHECK(canvas.caretAddress() == kMax);
            Key(canvas, Qt::Key_Right);
            Key(canvas, Qt::Key_Down);
            Key(canvas, Qt::Key_PageDown);
            Key(canvas, Qt::Key_End);
            CHECK(canvas.selectedRange()->first == kMax && canvas.selectedRange()->last == kMax);
            Key(canvas, Qt::Key_Left, Qt::ShiftModifier);
            CHECK(canvas.selectedRange()->first == kMax - 1 && canvas.selectedRange()->last == kMax);

            // 行列互转：0xFFFFFFFFFFFFFFF0 是末行第 0 列，与 kMax 同一行。
            CHECK(canvas.cellRect(kMax - 15, Pane::Hex).y() == canvas.cellRect(kMax, Pane::Hex).y());
            CHECK(canvas.cellRect(kMax - 15, Pane::Hex).x() < canvas.cellRect(kMax, Pane::Hex).x());
            CHECK(canvas.cellStateAt(kMax).inSpace);

            // 页请求：跳转之后的所有请求都在末端附近，没有回绕成小地址。
            bool allNearEnd = provider.requests.size() > requestMark;
            for (std::size_t index = requestMark; index < provider.requests.size(); ++index)
            {
                for (const ks::ui::HexFetchRange& range : provider.requests[index].ranges)
                {
                    allNearEnd = allNearEnd && range.firstPageStart >= 0xFFFFFFFFFFFF0000ULL;
                }
            }
            CHECK(allNearEnd);

            // 行宽 48：2^64 不是 48 的倍数，最后一行只有 16 个有效格，其后的格子是空位而不是回绕。
            HexCanvas wide;
            RecordingProvider wideProvider;
            wide.resize(2600, 320);
            wide.show();
            wide.setAddressSpace(0, kMax);
            wide.setPageProvider(&wideProvider);
            CHECK(wide.setBytesPerRow(48));
            wide.scrollToAddress(kMax, HexCanvas::ScrollAlign::Center);
            const QRect lastHex = wide.cellRect(kMax, Pane::Hex);
            const QRect firstOfLast = wide.cellRect(kMax - 15, Pane::Hex);
            CHECK(!lastHex.isNull() && !firstOfLast.isNull());
            CHECK(lastHex.y() == firstOfLast.y());
            const QImage image = GrabImage(wide);
            const QColor surface = KswordTheme::SurfaceColor();
            const int charWidth = lastHex.width() / 2;
            const QRect asciiLast = wide.cellRect(kMax, Pane::Ascii);
            const int asciiStart = asciiLast.x() - 15 * charWidth;
            const int hexSpaceRight = asciiStart - charWidth - 3;
            const QRect padHexRegion(lastHex.right() + 2 * charWidth, lastHex.y(), hexSpaceRight - (lastHex.right() + 2 * charWidth), lastHex.height());
            CHECK(padHexRegion.width() > 3 * charWidth);
            CHECK(RegionIsColor(image, padHexRegion, surface));
            const QRect padAsciiRegion(asciiLast.right() + 1, asciiLast.y(), 20 * charWidth, asciiLast.height());
            CHECK(RegionIsColor(image, padAsciiRegion, surface));
        }

        // 滚动条映射：小空间精确，超大空间按比例；滚轮、键盘仍按精确行数。
        void TestScrollMapping()
        {
            ApplyTheme(false);

            // 精确模式：滑块值就是首行行号。
            auto fixture = MakeStaticFixture(0, MakePattern(64 * 1024), false, QSize(900, 420));
            HexCanvas& small = *fixture->canvas;
            QScrollBar* smallBar = small.verticalScrollBar();
            const std::uint64_t smallRows = 64 * 1024 / 16;
            CHECK(static_cast<std::uint64_t>(smallBar->maximum()) == smallRows - small.visibleRowCount());
            smallBar->setValue(7);
            CHECK(small.firstVisibleRow() == 7);
            small.setFirstVisibleRow(100);
            CHECK(smallBar->value() == 100);

            // 滚轮一档 = 3 行（Qt 滚轮一档 120）。
            const std::uint64_t before = small.firstVisibleRow();
            QWheelEvent wheelDown(
                QPointF(50, 50), QPointF(50, 50), QPoint(0, 0), QPoint(0, -120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(small.viewport(), &wheelDown);
            CHECK(small.firstVisibleRow() == before + 3);
            QWheelEvent wheelUp(
                QPointF(50, 50), QPointF(50, 50), QPoint(0, 0), QPoint(0, 120),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(small.viewport(), &wheelUp);
            CHECK(small.firstVisibleRow() == before);

            // 比例模式：地址空间 2^64，滑块量程固定为 INT_MAX。
            HexCanvas huge;
            RecordingProvider provider;
            huge.resize(900, 420);
            huge.show();
            huge.setAddressSpace(0, kMax);
            huge.setPageProvider(&provider);
            QScrollBar* bar = huge.verticalScrollBar();
            CHECK(bar->maximum() == INT_MAX);
            const std::uint64_t maxFirst = (kMax / 16ULL + 1ULL) - huge.visibleRowCount();
            bar->setValue(INT_MAX);
            CHECK(huge.firstVisibleRow() == maxFirst);
            bar->setValue(0);
            CHECK(huge.firstVisibleRow() == 0);
            bar->setValue(INT_MAX / 2);
            const double ratio = static_cast<double>(huge.firstVisibleRow()) / static_cast<double>(maxFirst);
            CHECK_NOTE(ratio > 0.4999 && ratio < 0.5001, QString::number(ratio, 'f', 8));

            // 比例模式下滚轮仍是精确的 3 行，键盘 PgDn 仍是精确的一屏。
            const std::uint64_t anchorRow = huge.firstVisibleRow();
            QApplication::sendEvent(huge.viewport(), &wheelDown);
            CHECK(huge.firstVisibleRow() == anchorRow + 3);
            huge.setCaretAddress(huge.firstVisibleRow() * 16ULL);
            const std::uint64_t caretBefore = huge.caretAddress();
            const std::uint64_t topBefore = huge.firstVisibleRow();
            Key(huge, Qt::Key_PageDown);
            CHECK(huge.firstVisibleRow() == topBefore + huge.visibleRowCount());
            CHECK(huge.caretAddress() == caretBefore + 16ULL * huge.visibleRowCount());
            Key(huge, Qt::Key_Down);
            CHECK(huge.caretAddress() == caretBefore + 16ULL * huge.visibleRowCount() + 16ULL);

            // 拖滑块到底再用滚轮向下：已在末屏，保持不动，不越过。
            bar->setValue(INT_MAX);
            const std::uint64_t bottomRow = huge.firstVisibleRow();
            QApplication::sendEvent(huge.viewport(), &wheelDown);
            CHECK(huge.firstVisibleRow() == bottomRow);

            // 精确/比例分界：最大首行恰好等于 INT_MAX 时仍精确，加 1 行进入比例模式。
            HexCanvas edge;
            RecordingProvider edgeProvider;
            edge.resize(900, 420);
            edge.show();
            edge.setPageProvider(&edgeProvider);
            const std::uint64_t exactRows = static_cast<std::uint64_t>(INT_MAX) + edge.visibleRowCount();
            edge.setAddressSpace(0, exactRows * 16ULL - 1ULL);
            CHECK(edge.verticalScrollBar()->maximum() == INT_MAX);
            CHECK(edge.verticalScrollBar()->singleStep() == 1);
            edge.verticalScrollBar()->setValue(INT_MAX);
            CHECK(edge.firstVisibleRow() == static_cast<std::uint64_t>(INT_MAX));
            edge.setAddressSpace(0, (exactRows + 1ULL) * 16ULL - 1ULL);
            edge.verticalScrollBar()->setValue(INT_MAX);
            CHECK(edge.firstVisibleRow() == static_cast<std::uint64_t>(INT_MAX) + 1ULL);
            edge.verticalScrollBar()->setValue(INT_MAX - 1);
            CHECK(edge.firstVisibleRow() < static_cast<std::uint64_t>(INT_MAX) + 1ULL);
        }

        // 未加载/不可读/部分读：显示 ·· 与 ??，绝不画成 00；陈旧代次被拒收。
        void TestMissingBytes()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            RecordingProvider provider;
            canvas.resize(800, 360);
            canvas.show();
            canvas.setAddressSpace(0x10000, 0x13FFF);
            canvas.setPageProvider(&provider);

            const QString loading = QString(2, QChar(0x00B7));
            CHECK(provider.requests.size() == 1);
            const std::uint64_t revision = provider.requests.empty() ? 0 : provider.requests[0].revision;

            // 请求发出后、回填前：可见页是"在途"，画 ··，不是 00。
            HexCanvas::CellState pending = canvas.cellStateAt(0x10000);
            CHECK(pending.byteState == HexCanvas::ByteState::Pending);
            CHECK(pending.hexText == loading);
            CHECK(pending.asciiChar == QChar(0x00B7));
            CHECK(!pending.hasValue);

            // 第 1 页回填：其中 0x10010..0x1001F 的有效掩码为 0（部分读）。
            QByteArray page(4096, '\x41');
            QByteArray mask(4096, '\x01');
            for (int index = 0x10; index < 0x20; ++index)
            {
                mask[index] = '\0';
            }
            CHECK(canvas.deliverPage(0x10000, page, mask, revision) == HexCanvas::PageResult::Accepted);
            const HexCanvas::CellState readable = canvas.cellStateAt(0x10005);
            CHECK(readable.hasValue && readable.hexText == QStringLiteral("41"));
            CHECK(readable.asciiChar == QLatin1Char('A'));
            const HexCanvas::CellState partial = canvas.cellStateAt(0x10012);
            CHECK(partial.byteState == HexCanvas::ByteState::Unreadable);
            CHECK(partial.hexText == QStringLiteral("??"));
            CHECK(partial.asciiChar == QChar(0x00D7));
            CHECK(partial.asciiChar != QLatin1Char('?'));
            CHECK(!partial.hasValue);

            // 第 2 页整页不可读。
            ks::ui::HexFetchRange unreadable;
            unreadable.firstPageStart = 0x11000;
            unreadable.pageCount = 1;
            CHECK(canvas.deliverUnreadable(unreadable, revision) == HexCanvas::PageResult::Accepted);
            CHECK(canvas.cellStateAt(0x11000).hexText == QStringLiteral("??"));
            CHECK(canvas.cellStateAt(0x11FFF).hexText == QStringLiteral("??"));
            CHECK(canvas.cellStateAt(0x11000).hexText != QStringLiteral("00"));

            // 第 3 页仍在途；第 4 页（预取范围之外）从未请求，是"未加载"，同样画 ··。
            CHECK(canvas.cellStateAt(0x12000).byteState == HexCanvas::ByteState::Pending);
            CHECK(canvas.cellStateAt(0x13000).byteState == HexCanvas::ByteState::NotLoaded);
            CHECK(canvas.cellStateAt(0x13000).hexText == loading);

            // 渲染一帧不应崩溃，且不可读单元格像素里没有"00"那样的实心文字：只检查能抓图。
            CHECK(!GrabImage(canvas).isNull());

            // 陈旧代次：refresh 换新代次后，旧代次的回填被拒收，单元格回到在途状态。
            canvas.refresh();
            CHECK(canvas.deliverPage(0x10000, page, mask, revision) == HexCanvas::PageResult::RejectedStaleRevision);
            CHECK(canvas.cellStateAt(0x10005).byteState == HexCanvas::ByteState::Pending);
            CHECK(provider.requests.size() == 2);
            CHECK(provider.requests.back().revision != revision);

            // 提供者放弃范围：cancelPages 后单元格回到未加载，之后允许重试。
            ks::ui::HexFetchRange given = provider.requests.back().ranges.front();
            canvas.cancelPages(given);
            CHECK(canvas.cellStateAt(given.firstPageStart).byteState == HexCanvas::ByteState::NotLoaded);
        }

        // 页协议：先登记在途再交给提供者、不重复请求、预取前后各 2 页、可见范围信号。
        void TestFetchProtocol()
        {
            ApplyTheme(false);
            HexCanvas canvas;
            RecordingProvider provider;
            bool inFlightBeforeCall = true;
            provider.onRequest = [&](const RecordingProvider::Request& request) {
                for (const ks::ui::HexFetchRange& range : request.ranges)
                {
                    for (std::uint64_t page = 0; page < range.pageCount; ++page)
                    {
                        const std::uint64_t address = range.firstPageStart + page * 4096ULL;
                        inFlightBeforeCall = inFlightBeforeCall
                            && canvas.cellStateAt(address).byteState == HexCanvas::ByteState::Pending;
                    }
                }
            };
            canvas.resize(800, 360);
            canvas.show();
            canvas.setAddressSpace(0x100000, 0x100000 + 64 * 4096 - 1);
            QSignalSpy rangeSpy(&canvas, &HexCanvas::visibleRangeChanged);
            canvas.setPageProvider(&provider);

            CHECK(inFlightBeforeCall);
            CHECK(!provider.requests.empty());

            // 首屏在第 0 页：请求页集合 = 第 0 页 + 其后预取 2 页；前方没有页可预取。
            std::set<std::uint64_t> pages;
            for (const auto& request : provider.requests)
            {
                for (const auto& range : request.ranges)
                {
                    for (std::uint64_t page = 0; page < range.pageCount; ++page)
                    {
                        pages.insert(range.firstPageStart + page * 4096ULL);
                    }
                }
            }
            CHECK(pages.count(0x100000) == 1);
            CHECK(pages.count(0x101000) == 1);
            CHECK(pages.count(0x102000) == 1);
            CHECK(pages.count(0x103000) == 0);
            CHECK(provider.totalPages() == pages.size());   // 没有重复请求

            // 滚动到第 20 页中部：前后各预取 2 页，且不重复请求已在途的页。
            canvas.setFirstVisibleRow((0x14000ULL + 0x800ULL) / 16ULL);
            std::set<std::uint64_t> allPages;
            for (const auto& request : provider.requests)
            {
                for (const auto& range : request.ranges)
                {
                    for (std::uint64_t page = 0; page < range.pageCount; ++page)
                    {
                        allPages.insert(range.firstPageStart + page * 4096ULL);
                    }
                }
            }
            CHECK(allPages.count(0x112000) == 1);   // 可见页前 2 页
            CHECK(allPages.count(0x113000) == 1);
            CHECK(allPages.count(0x114000) == 1);   // 可见页
            CHECK(allPages.count(0x116000) == 1);   // 可见页后 2 页
            CHECK(provider.totalPages() == allPages.size());
            CHECK(inFlightBeforeCall);

            // 可见范围信号：滚动后发出，首尾地址落在新位置。
            CHECK(!rangeSpy.isEmpty());
            const QList<QVariant> lastArgs = rangeSpy.last();
            CHECK(lastArgs.at(0).toULongLong() >= 0x114000ULL - 16ULL);
            CHECK(lastArgs.at(1).toULongLong() > lastArgs.at(0).toULongLong());
        }

        // 行宽与分组：只接受 8/16/32/48/64 与 1/2/4/8，改行宽后首行地址保持可见。
        void TestBytesPerRowAndGroup()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x2000, MakePattern(8192), false, QSize(1500, 360));
            HexCanvas& canvas = *fixture->canvas;
            CHECK(canvas.bytesPerRow() == 16);
            CHECK(!canvas.setBytesPerRow(10));
            CHECK(!canvas.setBytesPerRow(0));
            CHECK(!canvas.setBytesPerRow(-8));
            CHECK(canvas.bytesPerRow() == 16);
            CHECK(!canvas.setGroupSize(3));
            CHECK(canvas.groupSize() == 1);

            canvas.setFirstVisibleRow(40);
            const std::uint64_t anchor = 0x2000 + 40 * 16;
            for (const int width : { 8, 32, 48, 64, 16 })
            {
                CHECK(canvas.setBytesPerRow(width));
                CHECK(canvas.bytesPerRow() == width);
                CHECK(!canvas.cellRect(anchor, Pane::Hex).isNull());
                // 每行 width 个字节：同一行相邻两个地址的纵坐标相同，下一行起点纵坐标更大。
                CHECK(canvas.cellRect(anchor, Pane::Hex).y() == canvas.cellRect(anchor + 1, Pane::Hex).y());
            }

            // 分组：组边界处多留空隙（2 字节一组时第 2、3 个字节之间的间距大于第 1、2 个字节）。
            canvas.setFirstVisibleRow(0);
            canvas.setGroupSize(1);
            const int plainGap = canvas.cellRect(0x2002, Pane::Hex).x() - canvas.cellRect(0x2001, Pane::Hex).x();
            CHECK(canvas.setGroupSize(2));
            const int groupedInner = canvas.cellRect(0x2001, Pane::Hex).x() - canvas.cellRect(0x2000, Pane::Hex).x();
            const int groupedGap = canvas.cellRect(0x2002, Pane::Hex).x() - canvas.cellRect(0x2001, Pane::Hex).x();
            CHECK(groupedInner == plainGap);
            CHECK(groupedGap > groupedInner);
            // 每 8 字节有中缝：第 8、9 个字节的间距大于普通间距。
            canvas.setGroupSize(1);
            const int midGap = canvas.cellRect(0x2008, Pane::Hex).x() - canvas.cellRect(0x2007, Pane::Hex).x();
            CHECK(midGap > plainGap);
        }

        // 通用高亮层：优先级（选区 > 高亮 > 变化）、层号大者在上、按层清除、提示文本。
        void TestHighlightLayers()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(1024), false, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            canvas.setCaretAddress(0x1000);

            canvas.setHighlightRanges(1, { { 0x1010, 0x101F }, { 0x1018, 0x1030 } }, QColor(255, 220, 0), QStringLiteral("搜索命中"));
            CHECK(canvas.cellStateAt(0x1015).highlighted && canvas.cellStateAt(0x1015).highlightLayerId == 1);
            CHECK(canvas.cellStateAt(0x1025).highlighted);   // 两个区间重叠相连后合并
            CHECK(!canvas.cellStateAt(0x1031).highlighted);
            CHECK(!canvas.cellStateAt(0x100F).highlighted);
            CHECK(canvas.cellToolTip(0x1015).contains(QStringLiteral("搜索命中")));

            // 第二层与第一层重叠：层号大者在上。
            canvas.setHighlightRanges(2, { { 0x1014, 0x1016 } }, QColor(0, 200, 255), QStringLiteral("书签"));
            CHECK(canvas.cellStateAt(0x1015).highlightLayerId == 2);
            CHECK(canvas.cellStateAt(0x1019).highlightLayerId == 1);
            CHECK(canvas.cellToolTip(0x1015).contains(QStringLiteral("书签")));

            // 像素：高亮层着色与未着色区分；选区压过高亮层。
            const QColor surface = KswordTheme::SurfaceColor();
            QImage image = GrabImage(canvas);
            const QRect highlighted = canvas.cellRect(0x101B, Pane::Hex);
            const QRect plain = canvas.cellRect(0x1040, Pane::Hex);
            const QPoint highlightProbe(highlighted.x() - highlighted.width() / 8, highlighted.y());
            const QPoint plainProbe(plain.x() - plain.width() / 8, plain.y());
            CHECK(image.pixelColor(plainProbe) == surface);
            CHECK(ColorDistance(image.pixelColor(highlightProbe), surface) > 25);

            canvas.setCaretAddress(0x1019);
            canvas.setCaretAddress(0x1022, true);
            image = GrabImage(canvas);
            const QRect selectedCell = canvas.cellRect(0x101B, Pane::Hex);
            const QPoint selectedProbe(selectedCell.x() - selectedCell.width() / 8, selectedCell.y());
            CHECK(ColorsClose(image.pixelColor(selectedProbe), KswordTheme::EditorSelectionColor(), 3));

            // 清除：先清第 2 层，再清全部。
            canvas.clearHighlightRanges(2);
            CHECK(canvas.cellStateAt(0x1015).highlightLayerId == 1);
            canvas.clearHighlightRanges(99);
            canvas.clearAllHighlightRanges();
            CHECK(!canvas.cellStateAt(0x1015).highlighted);
        }

        // 其它：IME 提示、可访问性名称、绘制对象数、2 MiB 按需供页。
        void TestMisc()
        {
            ApplyTheme(false);
            const std::size_t twoMiB = 2 * 1024 * 1024;
            auto fixture = MakeStaticFixture(0x400000, MakePattern(static_cast<int>(twoMiB)), false, QSize(1100, 700));
            HexCanvas& canvas = *fixture->canvas;

            // IME：声明不预测、偏好拉丁、仅拉丁。
            const int hints = canvas.inputMethodQuery(Qt::ImHints).toInt();
            CHECK((hints & Qt::ImhNoPredictiveText) != 0);
            CHECK((hints & Qt::ImhPreferLatin) != 0);
            CHECK((hints & Qt::ImhLatinOnly) != 0);
            CHECK(!canvas.accessibleName().isEmpty());

            // 绘制对象数 O(可见行数)：全选 2 MiB 后一次绘制仍只遍历可见行。
            canvas.selectAll();
            GrabImage(canvas);
            CHECK(canvas.lastPaintedRowCount() <= canvas.visibleRowCount() + 1);
            CHECK(canvas.lastPaintedRowCount() >= 1);
            CHECK(canvas.selectedRange()->first == 0x400000);
            CHECK(canvas.selectedRange()->last == 0x400000 + twoMiB - 1);

            // 2 MiB 超过 256 页（1 MiB）的缓存容量：只有可见页附近是 Valid，远处是"未加载"，滚到末尾后又变 Valid。
            canvas.setCaretAddress(0x400000);
            canvas.setFirstVisibleRow(0);
            CHECK(canvas.cellStateAt(0x400000).byteState == HexCanvas::ByteState::Valid);
            CHECK(canvas.cellStateAt(0x400000 + twoMiB - 1).byteState == HexCanvas::ByteState::NotLoaded);
            Key(canvas, Qt::Key_End, Qt::ControlModifier);
            CHECK(canvas.cellStateAt(0x400000 + twoMiB - 1).byteState == HexCanvas::ByteState::Valid);
            CHECK(canvas.cellStateAt(0x400000 + twoMiB - 1).value == static_cast<std::uint8_t>(fixture->data.at(static_cast<int>(twoMiB - 1))));
            Key(canvas, Qt::Key_Home, Qt::ControlModifier);
            CHECK(canvas.cellStateAt(0x400000).byteState == HexCanvas::ByteState::Valid);

            // 静态数据贴着 64 位地址空间末端：10 个字节只有末尾 4 个放得下，按可容纳的部分截断，不回绕。
            {
                HexCanvas tail;
                tail.resize(700, 300);
                tail.show();
                tail.setStaticData(kMax - 3, QByteArray("\x11\x22\x33\x44\x55\x66\x77\x88\x99\xAA", 10));
                CHECK(tail.cellStateAt(kMax - 3).inSpace);
                CHECK(!tail.cellStateAt(kMax - 4).inSpace);
                CHECK(tail.cellStateAt(kMax - 3).hexText == QStringLiteral("11"));
                CHECK(tail.cellStateAt(kMax).hexText == QStringLiteral("44"));
                CHECK(tail.cellStateAt(0).inSpace == false);
                CHECK(tail.selectedRange().has_value() && tail.selectedRange()->first == kMax - 3);
                tail.setCaretAddress(kMax);
                Key(tail, Qt::Key_Right);
                CHECK(tail.caretAddress() == kMax);
            }

            // 悬停提示：真实的 ToolTip 事件走到 viewportEvent，显示的文本与 cellToolTip 一致。
            {
                canvas.setFirstVisibleRow(0);
                const std::uint64_t probe = 0x400000 + 0x20;
                const QPoint pos = canvas.cellRect(probe, Pane::Hex).center();
                QHelpEvent help(QEvent::ToolTip, pos, canvas.viewport()->mapToGlobal(pos));
                QApplication::sendEvent(canvas.viewport(), &help);
                CHECK(help.isAccepted());
                CHECK(QToolTip::isVisible());
                CHECK(QToolTip::text() == canvas.cellToolTip(probe));
                CHECK(QToolTip::text().contains(QStringLiteral("0x00400020")));
                QToolTip::hideText();
            }

            // 键盘陷阱：Tab 被面板切换占用，Ctrl+Tab / Ctrl+Shift+Tab 必须能把焦点交给相邻控件。
            {
                QWidget host;
                QVBoxLayout layout(&host);
                HexCanvas inner;
                QLineEdit before;
                QLineEdit after;
                layout.addWidget(&before);
                layout.addWidget(&inner);
                layout.addWidget(&after);
                host.resize(600, 300);
                host.show();
                inner.setStaticData(0x1000, MakePattern(256));
                inner.setFocus();
                QApplication::processEvents();
                CHECK(host.focusWidget() == &inner);
                Key(inner, Qt::Key_Tab, Qt::ControlModifier);
                CHECK(host.focusWidget() == &after);
                CHECK(inner.activePane() == Pane::Hex);      // 没有被当成面板切换
                inner.setFocus();
                Key(inner, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
                CHECK(host.focusWidget() == &before);
                inner.setFocus();
                Key(inner, Qt::Key_Tab);
                CHECK(host.focusWidget() == &inner);         // 普通 Tab 只切换面板
                CHECK(inner.activePane() == Pane::Ascii);
            }

            // 空数据：回到无地址空间状态，不画字节，不崩溃。
            canvas.setStaticData(0x1000, QByteArray());
            CHECK(!canvas.selectedRange().has_value());
            CHECK(!canvas.cellStateAt(0x1000).inSpace);
            CHECK(!GrabImage(canvas).isNull());
            CHECK(canvas.lastPaintedRowCount() == 0);
        }
    }

    // 本文件全部测试的入口。
    void RunViewTests()
    {
        TestBasicDisplay();
        TestMouseSelection();
        TestKeyboardNavigation();
        TestHugeAddresses();
        TestScrollMapping();
        TestMissingBytes();
        TestFetchProtocol();
        TestBytesPerRowAndGroup();
        TestHighlightLayers();
        TestMisc();
    }
}
