// memwb_ui_tests.HexView.Compat.cpp
// 作用：HexView"旧缓冲模型兼容层"的离屏验证（接口承诺逐个函数测试）——
//   setBuffer（清选区/暂存/查找、滚动回顶、叠加层基线、64 位末端截断）、
//   byteEdited（每个真正变化的字节恰好一次且旧值/新值正确、键盘/填充/粘贴/Backspace/解释器行内编辑五条入口、
//   信号发出时状态已落地且槽里可以安全回滚）、
//   setByteQuiet（不发信号但缓冲与画面更新、与基线的橙色联动、边界）、
//   jumpToAddress / selectionRange / selectedBytes / 信号转发 / 右键菜单转发、规模读数。
// setReference / clearReference 在 memwb_ui_tests.HexView.Reference.cpp。

#include "memwb_ui_hexview.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>

#include <iostream>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexView;
        using Pane = HexCanvas::ActivePane;
        using ChangeKind = HexCanvas::ChangeKind;

        // HexByte：字节的两位大写十六进制文本，用来向画布键入。
        QString HexByte(int value)
        {
            return QStringLiteral("%1").arg(value & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
        }

        // ByteAt：缓冲里第 index 个字节。
        int ByteAt(const QByteArray& buffer, qsizetype index)
        {
            return static_cast<std::uint8_t>(buffer.at(index));
        }

        // OtherThan：取一个与 value 不同的字节值（用来保证"这次编辑一定真的改变了字节"）。
        int OtherThan(int value, int preferred)
        {
            return (preferred & 0xFF) == (value & 0xFF) ? ((preferred + 1) & 0xFF) : (preferred & 0xFF);
        }

        // setBuffer：整块替换内容，清掉选区、暂存、查找状态，滚动回顶，叠加层基线换新；64 位末端截断；空数据。
        void TestSetBuffer()
        {
            ApplyTheme(false);
            const QByteArray first = MakePattern(8192);
            auto view = MakeHexView(0x1000, first, true, QSize(900, 500));

            // 基本读数：基址、字节数、缓冲、单元格显示、叠加层基线。
            CHECK(view->baseAddress() == 0x1000);
            CHECK(view->bufferSize() == 8192);
            CHECK(view->buffer() == first);
            for (const std::uint64_t address : { 0x1000ULL, 0x1001ULL, 0x1010ULL })
            {
                const HexCanvas::CellState cell = CellOf(*view, address);
                CHECK(cell.hasValue && cell.value == static_cast<std::uint8_t>(first.at(static_cast<qsizetype>(address - 0x1000))));
                CHECK(cell.change == ChangeKind::Unchanged);
            }
            ksword::memwb::MemoryDiffOverlay* overlay = view->canvas()->overlay();
            CHECK(overlay != nullptr && overlay->HasBaseline());
            CHECK(overlay->BaseAddress() == 0x1000 && overlay->BaselineSize() == 8192);

            // 弄脏全部状态：滚动、选区、一次编辑、一次查找。
            view->canvas()->scrollToAddress(0x2800, HexCanvas::ScrollAlign::Top);
            ClickAddress(*view->canvas(), 0x2800);
            ClickAddress(*view->canvas(), 0x2806, Pane::Hex, Qt::ShiftModifier);
            view->canvas()->setFocus();
            Type(*view->canvas(), HexByte(OtherThan(ByteAt(first, 0x1807), 0xA5)));
            CHECK(overlay->HasPendingPatches());
            view->findBar()->setMode(ks::ui::HexFindBar::Mode::Hex);
            view->findBar()->setPatternText(HexByte(ByteAt(first, 0x1900)));
            QSignalSpy completed(view->findBar(), &ks::ui::HexFindBar::searchCompleted);
            CHECK(view->findBar()->findNext());
            CHECK(PumpUntil([&]() { return completed.count() == 1; }, 3000));
            CHECK(view->findBar()->highlightActive());
            CHECK(view->canvas()->firstVisibleRow() > 0);

            // 整块替换：旧状态全部清空。
            const QByteArray second = MakePattern(2048, 9);
            view->setBuffer(0x5000, second);
            CHECK(view->baseAddress() == 0x5000);
            CHECK(view->bufferSize() == 2048);
            CHECK(view->buffer() == second);
            CHECK(!view->canvas()->overlay()->HasPendingPatches());
            CHECK(view->canvas()->overlay()->BaseAddress() == 0x5000);
            CHECK(view->canvas()->overlay()->BaselineSize() == 2048);
            CHECK(view->canvas()->overlay()->BaselineByte(0x5000) == std::optional<std::uint8_t>(static_cast<std::uint8_t>(second.at(0))));
            CHECK(view->canvas()->firstVisibleRow() == 0);
            CHECK(view->caretAddress() == 0x5000);
            std::uint64_t startOffset = 99;
            std::uint64_t endOffset = 99;
            CHECK(view->selectionRange(startOffset, endOffset) && startOffset == 0 && endOffset == 1);
            CHECK(!view->findBar()->highlightActive());
            CHECK(view->findBar()->highlightRanges().empty());
            CHECK(!view->hasReference());
            CHECK(CellOf(*view, 0x5000).hasValue && CellOf(*view, 0x5000).change == ChangeKind::Unchanged);
            CHECK(!CellOf(*view, 0x1000).inSpace);
            CHECK(!CellOf(*view, 0x1000).highlighted);
            Flush();
            CHECK(!CellOf(*view, 0x5010).highlighted);

            // 空数据：等价清空，基址保留。
            view->setBuffer(0x10, QByteArray());
            CHECK(view->bufferSize() == 0 && view->baseAddress() == 0x10);
            CHECK(!CellOf(*view, 0x10).inSpace);
            CHECK(view->statusBar()->caretText() == QStringLiteral("无数据"));
            view->setBuffer(0x20, second);
            view->clearBuffer();
            CHECK(view->bufferSize() == 0 && view->baseAddress() == 0x20);
            CHECK(view->buffer().isEmpty());

            // 贴着 64 位末端：截断到放得下的部分；叠加层无基线（终点不可表示）所以可看不可编辑，
            // 但静默改字节仍然成立（回滚路径依赖）。
            const std::uint64_t nearEnd = 0xFFFFFFFFFFFFFFFCULL;
            view->setBuffer(nearEnd, MakePattern(10));
            CHECK(view->bufferSize() == 4);
            CHECK(view->buffer() == MakePattern(10).left(4));
            CHECK(CellOf(*view, 0xFFFFFFFFFFFFFFFFULL).inSpace && CellOf(*view, 0xFFFFFFFFFFFFFFFFULL).hasValue);
            QString reason;
            CHECK(!view->canvas()->stageBytes(nearEnd, QByteArray(1, '\x11'), &reason));
            CHECK(!reason.isEmpty());
            CHECK(view->setByteQuiet(0xFFFFFFFFFFFFFFFFULL, 0x77));
            CHECK(ByteAt(view->buffer(), 3) == 0x77);
            CHECK(CellOf(*view, 0xFFFFFFFFFFFFFFFFULL).value == 0x77);
            CHECK(!view->setByteQuiet(0, 0x01));
        }

        // byteEdited：键盘、填充、粘贴、Backspace、解释器行内编辑，每个真正变化的字节恰好一次。
        void TestByteEdited()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(4096);
            auto view = MakeHexView(0x1000, data, true, QSize(1000, 520));
            HexCanvas& canvas = *view->canvas();
            QSignalSpy edited(view.get(), &HexView::byteEdited);
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            canvas.setFocus();

            // ---- 键盘：两位十六进制 ----
            ClickAddress(canvas, 0x1010);
            const int oldValue = ByteAt(data, 0x10);
            const int newValue = OtherThan(oldValue, 0xA7);
            Type(canvas, HexByte(newValue));
            CHECK(edited.count() == 1);
            if (edited.count() == 1)
            {
                const QList<QVariant> args = edited.at(0);
                CHECK(args.at(0).toULongLong() == 0x1010);
                CHECK(args.at(1).toInt() == oldValue);
                CHECK(args.at(2).toInt() == newValue);
            }
            CHECK(ByteAt(view->buffer(), 0x10) == newValue);
            CHECK(view->buffer().left(0x10) == data.left(0x10));
            CHECK(view->buffer().mid(0x11) == data.mid(0x11));
            CHECK(CellOf(*view, 0x1010).change == ChangeKind::Pending);
            CHECK(CellOf(*view, 0x1010).value == newValue);
            CHECK(view->caretAddress() == 0x1011);
            const std::vector<ksword::memwb::DiffBlock> blocks = canvas.overlay()->DiffBlocks();
            CHECK(blocks.size() == 1 && blocks[0].address == 0x1010
                && blocks[0].before == std::vector<std::uint8_t>{ static_cast<std::uint8_t>(oldValue) }
                && blocks[0].after == std::vector<std::uint8_t>{ static_cast<std::uint8_t>(newValue) });

            // ---- 同值再写：暂存成功（editStaged 照发）但缓冲没变，不发 byteEdited ----
            ClickAddress(canvas, 0x1010);
            const int stagedBefore = staged.count();
            Type(canvas, HexByte(newValue));
            CHECK(staged.count() == stagedBefore + 1);
            CHECK(edited.count() == 1);

            // ---- 改回基线值：补丁消失、橙色消失，仍然发一次 byteEdited（旧值 = 编辑值，新值 = 原值）----
            ClickAddress(canvas, 0x1010);
            Type(canvas, HexByte(oldValue));
            CHECK(edited.count() == 2);
            if (edited.count() == 2)
            {
                const QList<QVariant> args = edited.at(1);
                CHECK(args.at(0).toULongLong() == 0x1010 && args.at(1).toInt() == newValue && args.at(2).toInt() == oldValue);
            }
            CHECK(view->buffer() == data);
            CHECK(CellOf(*view, 0x1010).change == ChangeKind::Unchanged);
            CHECK(!canvas.overlay()->HasPendingPatches());

            // ---- 填充：只对真正变化的字节发信号（0x1022 已经是 FF，不再发）----
            edited.clear();
            CHECK(view->setByteQuiet(0x1022, 0xFF));
            ClickAddress(canvas, 0x1020);
            ClickAddress(canvas, 0x1023, Pane::Hex, Qt::ShiftModifier);
            const QByteArray beforeFill = view->buffer();
            canvas.fillSelection(0xFF);
            std::vector<std::uint64_t> expectedAddresses;
            for (std::uint64_t address = 0x1020; address <= 0x1023; ++address)
            {
                if (ByteAt(beforeFill, static_cast<qsizetype>(address - 0x1000)) != 0xFF)
                {
                    expectedAddresses.push_back(address);
                }
            }
            CHECK(expectedAddresses.size() == 3);
            CHECK(edited.count() == static_cast<int>(expectedAddresses.size()));
            for (int index = 0; index < edited.count() && index < static_cast<int>(expectedAddresses.size()); ++index)
            {
                const QList<QVariant> args = edited.at(index);
                const std::uint64_t address = expectedAddresses[static_cast<std::size_t>(index)];
                CHECK(args.at(0).toULongLong() == address);
                CHECK(args.at(1).toInt() == ByteAt(beforeFill, static_cast<qsizetype>(address - 0x1000)));
                CHECK(args.at(2).toInt() == 0xFF);
            }
            for (std::uint64_t address = 0x1020; address <= 0x1023; ++address)
            {
                CHECK(ByteAt(view->buffer(), static_cast<qsizetype>(address - 0x1000)) == 0xFF);
            }

            // ---- 粘贴：十六进制文本 11 22 33 写到选区起点，三个字节各一次 ----
            edited.clear();
            ClickAddress(canvas, 0x1030);
            const QByteArray beforePaste = view->buffer();
            QGuiApplication::clipboard()->setText(QStringLiteral("11 22 33"));
            canvas.pasteFromClipboard();
            int expectedPasteSignals = 0;
            const int pasted[3] = { 0x11, 0x22, 0x33 };
            for (int index = 0; index < 3; ++index)
            {
                if (ByteAt(beforePaste, 0x30 + index) != pasted[index])
                {
                    ++expectedPasteSignals;
                }
                CHECK(ByteAt(view->buffer(), 0x30 + index) == pasted[index]);
            }
            CHECK(edited.count() == expectedPasteSignals);

            // ---- Backspace：丢弃暂存使字节回到基线值，缓冲同步回退，并对回退发 byteEdited ----
            edited.clear();
            ClickAddress(canvas, 0x1040);
            const int original40 = ByteAt(data, 0x40);
            const int typed40 = OtherThan(original40, 0x5C);
            Type(canvas, HexByte(typed40));
            CHECK(edited.count() == 1 && view->caretAddress() == 0x1041);
            QTest::keyClick(&canvas, Qt::Key_Backspace);
            CHECK(view->caretAddress() == 0x1040);
            CHECK(ByteAt(view->buffer(), 0x40) == original40);
            CHECK(edited.count() == 2);
            if (edited.count() == 2)
            {
                const QList<QVariant> args = edited.at(1);
                CHECK(args.at(0).toULongLong() == 0x1040 && args.at(1).toInt() == typed40 && args.at(2).toInt() == original40);
            }
            CHECK(CellOf(*view, 0x1040).change == ChangeKind::Unchanged);

            // ---- 只读：键盘静默忽略，填充发 editRejected，缓冲不变 ----
            edited.clear();
            view->setEditable(false);
            QSignalSpy rejected(view.get(), &HexView::editRejected);
            const QByteArray readOnlySnapshot = view->buffer();
            ClickAddress(canvas, 0x1050);
            Type(canvas, QStringLiteral("AB"));
            CHECK(edited.isEmpty() && view->buffer() == readOnlySnapshot);
            canvas.fillSelection(0x00);
            CHECK(rejected.count() == 1 && edited.isEmpty() && view->buffer() == readOnlySnapshot);
            view->setEditable(true);
            CHECK(view->isEditable());

            // ---- 槽里回滚：信号发出时缓冲与画面已落地；槽调用 setByteQuiet 回滚，不递归、不再发信号 ----
            edited.clear();
            bool consistentAtSignal = true;
            const QMetaObject::Connection rollback = QObject::connect(
                view.get(), &HexView::byteEdited,
                [&](quint64 address, quint8 previousValue, quint8 currentValue) {
                    consistentAtSignal = consistentAtSignal
                        && ByteAt(view->buffer(), static_cast<qsizetype>(address - 0x1000)) == currentValue
                        && CellOf(*view, address).value == currentValue;
                    view->setByteQuiet(address, previousValue);
                });
            ClickAddress(canvas, 0x1060);
            const int old60 = ByteAt(data, 0x60);
            Type(canvas, HexByte(OtherThan(old60, 0x3C)));
            QObject::disconnect(rollback);
            CHECK(consistentAtSignal);
            CHECK(edited.count() == 1);
            CHECK(ByteAt(view->buffer(), 0x60) == old60);
            CHECK(CellOf(*view, 0x1060).value == old60);
            CHECK(CellOf(*view, 0x1060).change == ChangeKind::Unchanged);
        }

        // byteEdited：解释器面板的行内编辑也经同一入口，恰好各发一次。
        void TestByteEditedFromInspector()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(4096);
            auto view = MakeHexView(0x1000, data, true, QSize(1300, 560));
            view->setInspectorVisible(true);
            Flush();
            ks::ui::HexInspectorPanel* panel = view->panel();
            QSignalSpy edited(view.get(), &HexView::byteEdited);
            ClickAddress(*view->canvas(), 0x1010);

            // u32 行写 0x11223344（小端：44 33 22 11）。
            DoubleClickRow(*panel, QStringLiteral("u32"));
            TypeIntoEditor(*panel, QStringLiteral("0x11223344"));
            PressInEditor(*panel, Qt::Key_Return);
            FlushDeferred();
            const int expected[4] = { 0x44, 0x33, 0x22, 0x11 };
            int expectedSignals = 0;
            for (int index = 0; index < 4; ++index)
            {
                if (ByteAt(data, 0x10 + index) != expected[index])
                {
                    ++expectedSignals;
                }
                CHECK(ByteAt(view->buffer(), 0x10 + index) == expected[index]);
            }
            CHECK(edited.count() == expectedSignals);
            if (edited.count() >= 1)
            {
                const QList<QVariant> args = edited.at(0);
                CHECK(args.at(0).toULongLong() == 0x1010);
                CHECK(args.at(1).toInt() == ByteAt(data, 0x10));
                CHECK(args.at(2).toInt() == 0x44);
            }
        }

        // setByteQuiet：不发信号，缓冲与画面更新，橙色与"是否不同于基线"联动，边界。
        void TestSetByteQuiet()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(2048);
            auto view = MakeHexView(0x3000, data, true, QSize(1000, 480));
            HexCanvas& canvas = *view->canvas();
            QSignalSpy edited(view.get(), &HexView::byteEdited);
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);

            // 改一个字节：不发任何编辑信号，缓冲、单元格、橙色、像素都更新。
            const int oldValue = ByteAt(data, 0x20);
            const int newValue = OtherThan(oldValue, 0xC3);
            const QImage before = GrabImage(canvas);
            const QRect cell = canvas.cellRect(0x3020, Pane::Hex);
            CHECK(view->setByteQuiet(0x3020, static_cast<std::uint8_t>(newValue)));
            Flush();
            CHECK(edited.isEmpty() && staged.isEmpty() && rejected.isEmpty());
            CHECK(ByteAt(view->buffer(), 0x20) == newValue);
            CHECK(CellOf(*view, 0x3020).value == newValue);
            CHECK(CellOf(*view, 0x3020).change == ChangeKind::Pending);
            CHECK(canvas.overlay()->HasPendingPatches());
            CHECK(!cell.isNull() && before.copy(cell) != GrabImage(canvas).copy(cell));
            CHECK(view->buffer().left(0x20) == data.left(0x20) && view->buffer().mid(0x21) == data.mid(0x21));

            // 改回基线值：橙色消失，补丁没有了。
            CHECK(view->setByteQuiet(0x3020, static_cast<std::uint8_t>(oldValue)));
            CHECK(CellOf(*view, 0x3020).change == ChangeKind::Unchanged);
            CHECK(!canvas.overlay()->HasPendingPatches());
            CHECK(view->buffer() == data);

            // 再改成第三个值，再改成另一个值：始终只有一个补丁，after 是最新值。
            CHECK(view->setByteQuiet(0x3020, 0x12));
            CHECK(view->setByteQuiet(0x3020, 0x34));
            CHECK(canvas.overlay()->DiffBlocks().size() == 1);
            CHECK(canvas.overlay()->DiffBlocks()[0].after == std::vector<std::uint8_t>{ 0x34 });
            CHECK(canvas.overlay()->DiffBlocks()[0].before == std::vector<std::uint8_t>{ static_cast<std::uint8_t>(oldValue) });
            CHECK(view->setByteQuiet(0x3020, 0x34));
            CHECK(edited.isEmpty() && staged.isEmpty());

            // 回滚路径：用户编辑（byteEdited）之后宿主写失败，用旧值静默回滚。
            const int old30 = ByteAt(data, 0x30);
            ClickAddress(canvas, 0x3030);
            canvas.setFocus();
            Type(canvas, HexByte(OtherThan(old30, 0x99)));
            CHECK(edited.count() == 1);
            const int rolledBack = edited.at(0).at(1).toInt();
            CHECK(view->setByteQuiet(0x3030, static_cast<std::uint8_t>(rolledBack)));
            CHECK(edited.count() == 1);
            CHECK(ByteAt(view->buffer(), 0x30) == old30);
            CHECK(CellOf(*view, 0x3030).change == ChangeKind::Unchanged);

            // 边界：首字节与末字节成立，越界一律 false 且什么都不改（含地址下溢与 64 位最大地址）。
            const QByteArray snapshot = view->buffer();
            CHECK(view->setByteQuiet(0x3000, 0x01));
            CHECK(view->setByteQuiet(0x3000 + 2047, 0x02));
            CHECK(!view->setByteQuiet(0x3000 + 2048, 0x03));
            CHECK(!view->setByteQuiet(0x2FFF, 0x04));
            CHECK(!view->setByteQuiet(0, 0x05));
            CHECK(!view->setByteQuiet(0xFFFFFFFFFFFFFFFFULL, 0x06));
            CHECK(ByteAt(view->buffer(), 0) == 0x01 && ByteAt(view->buffer(), 2047) == 0x02);
            CHECK(view->buffer().mid(1, 2046) == snapshot.mid(1, 2046));

            // 空缓冲：一律 false。
            view->clearBuffer();
            CHECK(!view->setByteQuiet(0x3000, 0x01));
        }

        // jumpToAddress / selectionRange / selectedBytes / 信号转发 / 右键菜单转发。
        void TestNavigationAndSignals()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(0x4000);
            auto view = MakeHexView(0x4000, data, false, QSize(900, 360));
            HexCanvas& canvas = *view->canvas();

            // 跳转：范围内选中该字节并居中。
            CHECK(view->jumpToAddress(0x6000));
            Flush();
            CHECK(view->caretAddress() == 0x6000);
            CHECK(view->selectedBytes() == data.mid(0x2000, 1));
            const QRect cell = canvas.cellRect(0x6000, Pane::Hex);
            CHECK(!cell.isNull());
            const int viewportHeight = canvas.viewport()->height();
            CHECK_NOTE(cell.center().y() > viewportHeight / 4 && cell.center().y() < viewportHeight * 3 / 4,
                QStringLiteral("cell y=%1 viewport=%2").arg(cell.center().y()).arg(viewportHeight));

            // 边界：首末字节成立；越界返回 false，插入点不动，状态条给出提示（不静默）。
            CHECK(view->jumpToAddress(0x4000) && view->caretAddress() == 0x4000);
            CHECK(view->jumpToAddress(0x7FFF) && view->caretAddress() == 0x7FFF);
            CHECK(!view->jumpToAddress(0x8000));
            CHECK(view->statusBar()->hasMessage() && view->statusBar()->messageText().contains(QStringLiteral("超出数据范围")));
            CHECK(view->caretAddress() == 0x7FFF);
            CHECK(!view->jumpToAddress(0x3FFF));
            CHECK(!view->jumpToAddress(0));
            CHECK(!view->jumpToAddress(0xFFFFFFFFFFFFFFFFULL));
            CHECK(view->caretAddress() == 0x7FFF);

            // 选区：选区区间相对基址、止偏移不含；字节来自缓冲。
            QSignalSpy caretSpy(view.get(), &HexView::caretMoved);
            QSignalSpy selectionSpy(view.get(), &HexView::selectionChanged);
            ClickAddress(canvas, 0x5000);
            CHECK(caretSpy.count() == 1 && caretSpy.at(0).at(0).toULongLong() == 0x5000);
            CHECK(selectionSpy.count() == 1);
            if (selectionSpy.count() == 1)
            {
                CHECK(selectionSpy.at(0).at(0).toULongLong() == 0x1000);
                CHECK(selectionSpy.at(0).at(1).toULongLong() == 0x1001);
                CHECK(selectionSpy.at(0).at(2).toBool());
            }
            ClickAddress(canvas, 0x5009, Pane::Hex, Qt::ShiftModifier);
            std::uint64_t startOffset = 0;
            std::uint64_t endOffset = 0;
            CHECK(view->selectionRange(startOffset, endOffset));
            CHECK(startOffset == 0x1000 && endOffset == 0x100A);
            CHECK(view->selectedBytes() == data.mid(0x1000, 10));
            CHECK(view->caretAddress() == 0x5009);

            // 反向选区同样按升序给出。
            ClickAddress(canvas, 0x5100);
            ClickAddress(canvas, 0x50F0, Pane::Hex, Qt::ShiftModifier);
            CHECK(view->selectionRange(startOffset, endOffset) && startOffset == 0x10F0 && endOffset == 0x1101);
            CHECK(view->selectedBytes() == data.mid(0x10F0, 0x11));

            // 清空：selectionChanged(0, 0, false)，查询回到"无"。
            selectionSpy.clear();
            view->clearBuffer();
            CHECK(selectionSpy.count() == 1);
            if (selectionSpy.count() == 1)
            {
                CHECK(selectionSpy.at(0).at(0).toULongLong() == 0 && selectionSpy.at(0).at(1).toULongLong() == 0);
                CHECK(!selectionSpy.at(0).at(2).toBool());
            }
            CHECK(view->selectedBytes().isEmpty());

            // 右键菜单转发：画布构造菜单时，HexView 的 aboutToShowContextMenu 带着同一个菜单、地址、hasByte 发出。
            // 用 lambda 取参数：QMenu* 在信号声明处只有前置声明，QSignalSpy 拿不到它的元类型。
            view->setBuffer(0x4000, data);
            int menuSignals = 0;
            QMenu* seenMenu = nullptr;
            std::uint64_t seenAddress = 0;
            bool seenHasByte = false;
            const QMetaObject::Connection menuConnection = QObject::connect(
                view.get(), &HexView::aboutToShowContextMenu,
                [&](QMenu* signalMenu, quint64 signalAddress, bool signalHasByte) {
                    ++menuSignals;
                    seenMenu = signalMenu;
                    seenAddress = signalAddress;
                    seenHasByte = signalHasByte;
                });
            QMenu* menu = canvas.buildContextMenu(0x4123, true);
            CHECK(menuSignals == 1);
            CHECK(seenMenu == menu && seenAddress == 0x4123 && seenHasByte);
            delete menu;
            menu = canvas.buildContextMenu(0x4000, false);
            CHECK(menuSignals == 2 && seenMenu == menu && seenAddress == 0x4000 && !seenHasByte);
            delete menu;
            QObject::disconnect(menuConnection);
        }

        // 行宽与分组：成功后徽标、跳转条行宽同步；行号模式使用新行宽。
        void TestRowWidthSync()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0, MakePattern(4096), false, QSize(900, 400));
            CHECK(view->gotoBar()->space().bytesPerRow == 16);
            CHECK(view->setBytesPerRow(32));
            CHECK(view->gotoBar()->space().bytesPerRow == 32);
            view->gotoBar()->setMode(ks::ui::HexGotoBar::Mode::Row);
            view->gotoBar()->setInputText(QStringLiteral("3"));
            CHECK(view->gotoBar()->submit());
            Flush();
            CHECK(view->caretAddress() == 96);
            CHECK(!view->setBytesPerRow(5));
            CHECK(view->gotoBar()->space().bytesPerRow == 32);
            CHECK(view->gotoBar()->space().first == 0 && view->gotoBar()->space().last == 4095);
        }

        // 规模读数：16 MiB 缓冲的载入、第一次编辑（缓冲与基线分离的一次整块拷贝）、静默改字节的耗时。
        void TestScale()
        {
            ApplyTheme(false);
            const int size = 16 * 1024 * 1024;
            const QByteArray big = MakePattern(size);
            auto view = std::make_unique<HexView>();
            view->resize(1000, 520);
            view->show();
            view->setEditable(true);

            QElapsedTimer timer;
            timer.start();
            view->setBuffer(0x10000000, big);
            const qint64 loadMs = timer.elapsed();
            CHECK(view->bufferSize() == static_cast<std::uint64_t>(size));

            // 第一次编辑：叠加层暂存 + 缓冲写入（写时分离，一次整块拷贝）。
            view->canvas()->setFocus();
            ClickAddress(*view->canvas(), 0x10000010);
            const int before = ByteAt(view->buffer(), 0x10);
            timer.restart();
            CHECK(view->canvas()->stageBytes(0x10000010, QByteArray(1, static_cast<char>(OtherThan(before, 0x42)))));
            const qint64 firstEditMs = timer.elapsed();
            timer.restart();
            CHECK(view->canvas()->stageBytes(0x10000011, QByteArray(1, static_cast<char>(OtherThan(ByteAt(view->buffer(), 0x11), 0x43)))));
            const qint64 secondEditMs = timer.elapsed();
            timer.restart();
            CHECK(view->setByteQuiet(0x10000012, 0x99));
            const qint64 quietMs = timer.elapsed();
            CHECK(ByteAt(view->buffer(), 0x10) == OtherThan(before, 0x42));

            std::cout << "hexview scale (16 MiB): setBuffer " << loadMs << " ms, first edit " << firstEditMs
                      << " ms, second edit " << secondEditMs << " ms, setByteQuiet " << quietMs << " ms" << std::endl;
            CHECK(loadMs < 5000);
            CHECK(firstEditMs < 1500);
            CHECK(secondEditMs < 200);
            CHECK(quietMs < 200);
        }
    }

    // 兼容层（缓冲 / 编辑 / 静默 / 导航）验证入口。
    void RunHexViewCompatTests()
    {
        TestSetBuffer();
        TestByteEdited();
        TestByteEditedFromInspector();
        TestSetByteQuiet();
        TestNavigationAndSignals();
        TestRowWidthSync();
        TestScale();
    }
}
