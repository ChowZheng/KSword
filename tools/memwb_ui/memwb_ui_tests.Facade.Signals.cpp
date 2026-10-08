// memwb_ui_tests.Facade.Signals.cpp
// 作用：HexEditorWidget 门面的离屏验证（第二组：信号与交互语义）——
//   byteEdited（每个用户编辑的变化字节恰好一次、旧值/新值正确、五条编辑入口、槽内回滚）、
//   setByteAtAbsoluteAddress（不发信号、越界返回 false、keepSelection 的真实旧语义）、
//   selectedAbsoluteAddress / selectedOffset / currentAddressChanged / selectionChanged 的数值语义、
//   jumpToAbsoluteAddress、openFindPanel / openJumpPanel 与快捷键、aboutToShowContextMenu 转发、setHexOnlyView。

#include "memwb_ui_facade.h"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenu>

#include <limits>
#include <vector>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexFindBar;
        using ks::ui::HexView;
        using Pane = HexCanvas::ActivePane;
        using ChangeKind = HexCanvas::ChangeKind;

        // kMaxAddress：64 位地址的最大值，用来验证"越界"不会因回绕而误判为范围内。
        constexpr std::uint64_t kMaxAddress = std::numeric_limits<std::uint64_t>::max();

        // byteEdited：键盘、填充、粘贴、Backspace 回退各入口，每个真正变化的字节恰好一次；同值不发；只读不发；槽内回滚。
        void TestByteEdited()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(4096);
            auto widget = MakeFacade(0x1000, data, true, QSize(1000, 520));
            HexCanvas& canvas = *CanvasOf(*widget);
            QSignalSpy edited(widget.get(), &HexEditorWidget::byteEdited);
            canvas.setFocus();

            // 键盘：两位十六进制。信号参数是绝对地址与旧值/新值；data() 与画面同步更新。
            ClickAddress(canvas, 0x1010);
            const int oldValue = ByteValue(data, 0x10);
            const int newValue = OtherByte(oldValue, 0xA7);
            Type(canvas, HexByteText(newValue));
            CHECK(edited.count() == 1);
            if (edited.count() == 1)
            {
                const QList<QVariant> args = edited.at(0);
                CHECK(args.at(0).toULongLong() == 0x1010);
                CHECK(args.at(1).toInt() == oldValue);
                CHECK(args.at(2).toInt() == newValue);
            }
            CHECK(ByteValue(widget->data(), 0x10) == newValue);
            CHECK(widget->data().left(0x10) == data.left(0x10));
            CHECK(widget->data().mid(0x11) == data.mid(0x11));
            CHECK(FacadeCell(*widget, 0x1010).value == newValue);
            CHECK(widget->regionSize() == 4096 && widget->baseAddress() == 0x1000);

            // 同值再写：缓冲没有变化，不发 byteEdited。
            ClickAddress(canvas, 0x1010);
            Type(canvas, HexByteText(newValue));
            CHECK(edited.count() == 1);

            // 改回原值：仍发一次（旧值 = 编辑值，新值 = 原值），data() 回到原样。
            ClickAddress(canvas, 0x1010);
            Type(canvas, HexByteText(oldValue));
            CHECK(edited.count() == 2);
            if (edited.count() == 2)
            {
                const QList<QVariant> args = edited.at(1);
                CHECK(args.at(0).toULongLong() == 0x1010 && args.at(1).toInt() == newValue && args.at(2).toInt() == oldValue);
            }
            CHECK(widget->data() == data);

            // 填充选区：只对真正变化的字节各发一次（0x1022 事先静默改成 FF，所以不再发）。
            edited.clear();
            CHECK(widget->setByteAtAbsoluteAddress(0x1022, 0xFF, false));
            CHECK(edited.isEmpty());
            ClickAddress(canvas, 0x1020);
            ClickAddress(canvas, 0x1023, Pane::Hex, Qt::ShiftModifier);
            const QByteArray beforeFill = widget->data();
            canvas.fillSelection(0xFF);
            std::vector<std::uint64_t> expectedAddresses;
            for (std::uint64_t address = 0x1020; address <= 0x1023; ++address)
            {
                if (ByteValue(beforeFill, static_cast<qsizetype>(address - 0x1000)) != 0xFF)
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
                CHECK(args.at(1).toInt() == ByteValue(beforeFill, static_cast<qsizetype>(address - 0x1000)));
                CHECK(args.at(2).toInt() == 0xFF);
            }
            for (std::uint64_t address = 0x1020; address <= 0x1023; ++address)
            {
                CHECK(ByteValue(widget->data(), static_cast<qsizetype>(address - 0x1000)) == 0xFF);
            }

            // 粘贴十六进制文本 11 22 33：写到选区起点，每个变化的字节各一次。
            edited.clear();
            ClickAddress(canvas, 0x1030);
            const QByteArray beforePaste = widget->data();
            QGuiApplication::clipboard()->setText(QStringLiteral("11 22 33"));
            canvas.pasteFromClipboard();
            int expectedPasteSignals = 0;
            const int pasted[3] = { 0x11, 0x22, 0x33 };
            for (int index = 0; index < 3; ++index)
            {
                if (ByteValue(beforePaste, 0x30 + index) != pasted[index])
                {
                    ++expectedPasteSignals;
                }
                CHECK(ByteValue(widget->data(), 0x30 + index) == pasted[index]);
            }
            CHECK(edited.count() == expectedPasteSignals);

            // Backspace：丢弃暂存使字节回到基线值，缓冲同步回退，并对回退发 byteEdited。
            edited.clear();
            ClickAddress(canvas, 0x1040);
            const int original40 = ByteValue(data, 0x40);
            const int typed40 = OtherByte(original40, 0x5C);
            Type(canvas, HexByteText(typed40));
            CHECK(edited.count() == 1);
            QTest::keyClick(&canvas, Qt::Key_Backspace);
            CHECK(ByteValue(widget->data(), 0x40) == original40);
            CHECK(edited.count() == 2);
            if (edited.count() == 2)
            {
                const QList<QVariant> args = edited.at(1);
                CHECK(args.at(0).toULongLong() == 0x1040 && args.at(1).toInt() == typed40 && args.at(2).toInt() == original40);
            }

            // setByteArray 不发信号：整块换数据是宿主自己的动作。
            edited.clear();
            QByteArray changedData = data;
            changedData[0x50] = static_cast<char>(~changedData.at(0x50));
            widget->setByteArray(changedData, 0x1000);
            CHECK(edited.isEmpty());
            CHECK(widget->data() == changedData);

            // 槽内回滚：信号发出时缓冲与画面已落地，槽里用 setByteAtAbsoluteAddress 回滚是安全的，
            // 回滚本身不再发信号（否则宿主的"写失败回滚"会递归）。
            widget->setByteArray(data, 0x1000);
            edited.clear();
            int rollbackCalls = 0;
            const QMetaObject::Connection rollback = QObject::connect(
                widget.get(), &HexEditorWidget::byteEdited,
                [&](std::uint64_t address, std::uint8_t previousValue, std::uint8_t) {
                    ++rollbackCalls;
                    CHECK(widget->setByteAtAbsoluteAddress(address, previousValue, false));
                });
            ClickAddress(canvas, 0x1060);
            const int original60 = ByteValue(data, 0x60);
            Type(canvas, HexByteText(OtherByte(original60, 0x3C)));
            QObject::disconnect(rollback);
            CHECK(rollbackCalls == 1 && edited.count() == 1);
            CHECK(ByteValue(widget->data(), 0x60) == original60);
            CHECK(FacadeCell(*widget, 0x1060).value == original60);
            CHECK(FacadeCell(*widget, 0x1060).hasValue && FacadeCell(*widget, 0x1060).change == ChangeKind::Unchanged);
            CHECK(widget->data() == data);

            // 只读：键盘静默忽略，不发信号，缓冲不变。
            edited.clear();
            widget->setEditable(false);
            const QByteArray readOnlySnapshot = widget->data();
            ClickAddress(canvas, 0x1070);
            Type(canvas, QStringLiteral("AB"));
            CHECK(edited.isEmpty() && widget->data() == readOnlySnapshot);
        }

        // setByteAtAbsoluteAddress：不发信号；缓冲与画面更新；越界返回 false 且什么都不动；
        // keepSelection 的真实旧语义（为 true 才把插入点移到被改字节上）；橙色按"是否不同于参照"重算。
        void TestSetByteAtAbsoluteAddress()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(1024);
            auto widget = MakeFacade(0x4000, data, false, QSize(900, 400));
            QSignalSpy edited(widget.get(), &HexEditorWidget::byteEdited);
            QSignalSpy moved(widget.get(), &HexEditorWidget::currentAddressChanged);

            // 成功（只读状态下也可以静默改）：缓冲与画面更新，不发 byteEdited；keepSelection=false 不动选择。
            const int before = ByteValue(data, 5);
            const int target = OtherByte(before, 0x5A);
            CHECK(widget->setByteAtAbsoluteAddress(0x4005, static_cast<std::uint8_t>(target), false));
            CHECK(ByteValue(widget->data(), 5) == target);
            CHECK(widget->data().left(5) == data.left(5) && widget->data().mid(6) == data.mid(6));
            CHECK(FacadeCell(*widget, 0x4005).value == target);
            CHECK(edited.isEmpty());
            CHECK(moved.isEmpty() && widget->selectedAbsoluteAddress() == 0x4000);

            // 写回原值：缓冲回到原样（返回 true）。
            CHECK(widget->setByteAtAbsoluteAddress(0x4005, static_cast<std::uint8_t>(before), false));
            CHECK(widget->data() == data);
            CHECK(FacadeCell(*widget, 0x4005).value == before);

            // keepSelection=true：插入点移到被改字节上并发 currentAddressChanged；仍然不发 byteEdited。
            CHECK(widget->setByteAtAbsoluteAddress(0x4005, static_cast<std::uint8_t>(target), true));
            CHECK(widget->selectedAbsoluteAddress() == 0x4005 && widget->selectedOffset() == 5);
            CHECK(moved.count() == 1);
            if (moved.count() == 1)
            {
                CHECK(moved.at(0).at(0).toULongLong() == 0x4005);
            }
            CHECK(edited.isEmpty());

            // 默认参数是 true。
            CHECK(widget->setByteAtAbsoluteAddress(0x4006, 0x11));
            CHECK(widget->selectedAbsoluteAddress() == 0x4006);

            // 越界：基址之前、末字节之后、0、64 位最大值——一律 false，缓冲与插入点不动（即使 keepSelection=true）。
            const QByteArray snapshot = widget->data();
            const std::uint64_t caretBefore = widget->selectedAbsoluteAddress();
            moved.clear();
            for (const std::uint64_t address : { 0x3FFFULL, 0x4400ULL, 0ULL, kMaxAddress })
            {
                CHECK(!widget->setByteAtAbsoluteAddress(address, 0x77, true));
            }
            CHECK(widget->data() == snapshot);
            CHECK(widget->selectedAbsoluteAddress() == caretBefore && moved.isEmpty());

            // 首末字节成立。
            CHECK(widget->setByteAtAbsoluteAddress(0x4000, 0x01, false));
            CHECK(widget->setByteAtAbsoluteAddress(0x43FF, 0x02, false));
            CHECK(ByteValue(widget->data(), 0) == 0x01 && ByteValue(widget->data(), 1023) == 0x02);

            // 空控件：一律 false。
            HexEditorWidget empty;
            CHECK(!empty.setByteAtAbsoluteAddress(0, 0x01));
            CHECK(!empty.setByteAtAbsoluteAddress(kMaxAddress, 0x01, false));

            // HvmHookWizard 形态：逐字节写补丁，只有最后一个字节 keepSelection=true，焦点最终落在补丁末尾。
            widget->setByteArray(data, 0x4000);
            moved.clear();
            const int patch[4] = { 0xE9, 0x10, 0x20, 0x30 };
            for (int index = 0; index < 4; ++index)
            {
                CHECK(widget->setByteAtAbsoluteAddress(
                    0x4100 + static_cast<std::uint64_t>(index), static_cast<std::uint8_t>(patch[index]), index == 3));
            }
            CHECK(widget->selectedAbsoluteAddress() == 0x4103);
            CHECK(moved.count() == 1);
            for (int index = 0; index < 4; ++index)
            {
                CHECK(ByteValue(widget->data(), 0x100 + index) == patch[index]);
            }
            CHECK(edited.isEmpty());

            // 与变更参照联动：改得不同于参照 -> 橙色；写回参照值 -> 橙色消失。
            widget->setByteArray(data, 0x4000);
            widget->setChangeReferences(data);
            const int ref20 = ByteValue(data, 20);
            CHECK(widget->setByteAtAbsoluteAddress(0x4014, static_cast<std::uint8_t>(OtherByte(ref20, 0x99)), false));
            CHECK(FacadeCell(*widget, 0x4014).change == ChangeKind::Pending);
            CHECK(widget->setByteAtAbsoluteAddress(0x4014, static_cast<std::uint8_t>(ref20), false));
            CHECK(FacadeCell(*widget, 0x4014).hasValue && FacadeCell(*widget, 0x4014).change == ChangeKind::Unchanged);
        }

        // 选区与插入点：selectedAbsoluteAddress / selectedOffset / currentAddressChanged / selectionChanged 的数值语义。
        void TestSelectionSignals()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(0x4000);
            auto widget = MakeFacade(0x4000, data, false, QSize(900, 360));
            HexCanvas& canvas = *CanvasOf(*widget);
            QSignalSpy moved(widget.get(), &HexEditorWidget::currentAddressChanged);
            QSignalSpy selection(widget.get(), &HexEditorWidget::selectionChanged);

            // 刚载入：插入点在第一个字节。
            CHECK(widget->selectedAbsoluteAddress() == 0x4000 && widget->selectedOffset() == 0);

            // 单击一个字节：currentAddressChanged(绝对地址)；selectionChanged(偏移, 偏移 + 1（不含）, true)。
            ClickAddress(canvas, 0x5000);
            CHECK(widget->selectedAbsoluteAddress() == 0x5000 && widget->selectedOffset() == 0x1000);
            CHECK(moved.count() == 1);
            if (moved.count() == 1)
            {
                CHECK(moved.at(0).at(0).toULongLong() == 0x5000);
            }
            CHECK(selection.count() == 1);
            if (selection.count() == 1)
            {
                CHECK(selection.at(0).at(0).toULongLong() == 0x1000);
                CHECK(selection.at(0).at(1).toULongLong() == 0x1001);
                CHECK(selection.at(0).at(2).toBool());
            }

            // Shift 点击扩展选区：起点 = 最小偏移，止点 = 最大偏移 + 1；插入点在被点击的一端。
            ClickAddress(canvas, 0x5009, Pane::Hex, Qt::ShiftModifier);
            CHECK(!selection.isEmpty());
            if (!selection.isEmpty())
            {
                const QList<QVariant> args = selection.last();
                CHECK(args.at(0).toULongLong() == 0x1000 && args.at(1).toULongLong() == 0x100A && args.at(2).toBool());
            }
            CHECK(widget->selectedAbsoluteAddress() == 0x5009 && widget->selectedOffset() == 0x1009);
            CHECK(!moved.isEmpty() && moved.last().at(0).toULongLong() == 0x5009);

            // 反向选区：同样按升序给出区间；插入点在后点击的那一端。
            ClickAddress(canvas, 0x5100);
            ClickAddress(canvas, 0x50F0, Pane::Hex, Qt::ShiftModifier);
            CHECK(!selection.isEmpty());
            if (!selection.isEmpty())
            {
                const QList<QVariant> args = selection.last();
                CHECK(args.at(0).toULongLong() == 0x10F0 && args.at(1).toULongLong() == 0x1101 && args.at(2).toBool());
            }
            CHECK(widget->selectedAbsoluteAddress() == 0x50F0 && widget->selectedOffset() == 0x10F0);

            // 清空数据：selectionChanged(0, 0, false)；选中地址回退为基址（无选区语义），偏移为 0。
            selection.clear();
            widget->clearData();
            CHECK(selection.count() == 1);
            if (selection.count() == 1)
            {
                CHECK(selection.at(0).at(0).toULongLong() == 0 && selection.at(0).at(1).toULongLong() == 0);
                CHECK(!selection.at(0).at(2).toBool());
            }
            CHECK(widget->selectedAbsoluteAddress() == 0x4000 && widget->selectedOffset() == 0);

            // 重新载入：选中地址回到新基址。
            widget->setByteArray(data, 0x8000);
            CHECK(widget->selectedAbsoluteAddress() == 0x8000 && widget->selectedOffset() == 0);

            // 无数据且基址非零：选中地址就是基址（旧语义"未选中有效字节时返回 baseAddress"）。
            widget->setByteArray(QByteArray(), 0x12340);
            CHECK(widget->selectedAbsoluteAddress() == 0x12340 && widget->selectedOffset() == 0);
        }

        // jumpToAbsoluteAddress：范围内成功并选中、居中；范围外与无数据返回 false 且不动选择。
        void TestJump()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(0x4000);
            auto widget = MakeFacade(0x4000, data, false, QSize(900, 360));
            HexCanvas& canvas = *CanvasOf(*widget);
            QSignalSpy moved(widget.get(), &HexEditorWidget::currentAddressChanged);

            // 范围内：返回 true，选中该字节，发 currentAddressChanged，单元格可见并大致居中。
            CHECK(widget->jumpToAbsoluteAddress(0x6000));
            Flush();
            CHECK(widget->selectedAbsoluteAddress() == 0x6000 && widget->selectedOffset() == 0x2000);
            CHECK(moved.count() == 1);
            if (moved.count() == 1)
            {
                CHECK(moved.at(0).at(0).toULongLong() == 0x6000);
            }
            const QRect cell = canvas.cellRect(0x6000, Pane::Hex);
            CHECK(!cell.isNull());
            const int viewportHeight = canvas.viewport()->height();
            CHECK_NOTE(cell.center().y() > viewportHeight / 4 && cell.center().y() < viewportHeight * 3 / 4,
                QStringLiteral("cell y=%1 viewport=%2").arg(cell.center().y()).arg(viewportHeight));

            // 边界：首末字节成立。
            CHECK(widget->jumpToAbsoluteAddress(0x4000) && widget->selectedAbsoluteAddress() == 0x4000);
            CHECK(widget->jumpToAbsoluteAddress(0x7FFF) && widget->selectedAbsoluteAddress() == 0x7FFF);

            // 范围外：false，插入点不动，不发信号；状态条给出提示（不静默）。
            moved.clear();
            for (const std::uint64_t address : { 0x8000ULL, 0x3FFFULL, 0ULL, kMaxAddress })
            {
                CHECK(!widget->jumpToAbsoluteAddress(address));
            }
            CHECK(widget->selectedAbsoluteAddress() == 0x7FFF && moved.isEmpty());
            HexView* view = ViewOf(*widget);
            CHECK(view->statusBar()->hasMessage() && view->statusBar()->messageText().contains(QStringLiteral("超出数据范围")));

            // 跳转之后仍可再成功跳转（失败不留下坏状态）。
            CHECK(widget->jumpToAbsoluteAddress(0x4800) && widget->selectedAbsoluteAddress() == 0x4800);

            // 无数据：false；从未载入的控件同样。
            widget->clearData();
            CHECK(!widget->jumpToAbsoluteAddress(0x4000));
            HexEditorWidget empty;
            CHECK(!empty.jumpToAbsoluteAddress(0));
            CHECK(!empty.jumpToAbsoluteAddress(kMaxAddress));
        }

        // openFindPanel / openJumpPanel：查找条与跳转条可见并有焦点；快捷键只作用于有焦点的门面；查找端到端可用。
        void TestPanels()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(4096);
            auto widget = MakeFacade(0x1000, data, false, QSize(1000, 420));
            HexView* view = ViewOf(*widget);
            const bool activated = ActivateWindow(widget.get());
            CHECK(activated);
            Flush();
            CHECK(!view->findBar()->isVisible() && !view->gotoBar()->isVisible());

            // openFindPanel：查找条可见、输入框有焦点；跳转条不受影响。
            widget->openFindPanel();
            Flush();
            CHECK(view->findBar()->isVisible());
            CHECK(view->findBar()->lineEdit()->hasFocus());
            CHECK(!view->gotoBar()->isVisible());

            // openJumpPanel：跳转条可见、输入框有焦点；查找条保持打开。
            widget->openJumpPanel();
            Flush();
            CHECK(view->gotoBar()->isVisible() && view->gotoBar()->lineEdit()->hasFocus());
            CHECK(view->findBar()->isVisible());

            // 跳转条端到端：输入十六进制地址回车，门面的选中地址跟着变（旧"跳转面板"的功能）。
            view->gotoBar()->setMode(ks::ui::HexGotoBar::Mode::Absolute);
            view->gotoBar()->setInputText(QStringLiteral("0x1800"));
            CHECK(view->gotoBar()->submit());
            Flush();
            CHECK(widget->selectedAbsoluteAddress() == 0x1800);

            // 查找端到端：十六进制模式找一段字节，命中后选中命中起点。
            view->closeFindBar();
            view->closeGotoBar();
            Flush();
            CHECK(!view->findBar()->isVisible() && !view->gotoBar()->isVisible());
            widget->openFindPanel();
            const QByteArray needle = data.mid(0x300, 4);
            const std::vector<std::uint64_t> naive = NaiveMatches(data, 0x1000, needle);
            CHECK(!naive.empty());
            HexFindBar& bar = *view->findBar();
            bar.setMode(HexFindBar::Mode::Hex);
            bar.setPatternText(QStringLiteral("%1 %2 %3 %4")
                .arg(HexByteText(ByteValue(needle, 0)), HexByteText(ByteValue(needle, 1)),
                    HexByteText(ByteValue(needle, 2)), HexByteText(ByteValue(needle, 3))));
            QSignalSpy matches(&bar, &HexFindBar::matchFound);
            QSignalSpy done(&bar, &HexFindBar::searchCompleted);
            widget->jumpToAbsoluteAddress(0x1000);
            CHECK(bar.findNext());
            CHECK(PumpUntil([&]() { return done.count() >= 1; }, 5000));
            CHECK(done.count() >= 1 && done.at(0).at(0).toBool());
            if (!naive.empty() && matches.count() >= 1)
            {
                // 从插入点（0x1000，含）向后：第一个命中就是朴素查找的第一项。
                CHECK(matches.at(0).at(0).toULongLong() == naive.front());
                CHECK(widget->selectedAbsoluteAddress() == naive.front());
            }
            else
            {
                CHECK(false);
            }

            // 快捷键：焦点在画布内时 Ctrl+F / Ctrl+G 打开对应的条。
            view->closeFindBar();
            view->closeGotoBar();
            Flush();
            view->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QTest::keyClick(view->canvas(), Qt::Key_F, Qt::ControlModifier);
            Flush();
            CHECK(view->findBar()->isVisible());
            view->canvas()->setFocus(Qt::OtherFocusReason);
            QTest::keyClick(view->canvas(), Qt::Key_G, Qt::ControlModifier);
            Flush();
            CHECK(view->gotoBar()->isVisible());

            // 两个门面同窗口：快捷键只作用于有焦点的那个（旧控件是窗口级上下文，会二义冲突）。
            QWidget host;
            auto* hostLayout = new QHBoxLayout(&host);
            auto* left = new HexEditorWidget(&host);
            auto* right = new HexEditorWidget(&host);
            hostLayout->addWidget(left);
            hostLayout->addWidget(right);
            host.resize(1500, 500);
            left->setByteArray(MakePattern(2048), 0x1000);
            right->setByteArray(MakePattern(2048, 3), 0x9000);
            const bool hostActive = ActivateWindow(&host);
            CHECK(hostActive);
            Flush();
            ks::ui::HexView* leftView = ViewOf(*left);
            ks::ui::HexView* rightView = ViewOf(*right);
            leftView->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QTest::keyClick(leftView->canvas(), Qt::Key_F, Qt::ControlModifier);
            Flush();
            CHECK(leftView->findBar()->isVisible() && !rightView->findBar()->isVisible());
            rightView->canvas()->setFocus(Qt::OtherFocusReason);
            Flush();
            QTest::keyClick(rightView->canvas(), Qt::Key_G, Qt::ControlModifier);
            Flush();
            CHECK(rightView->gotoBar()->isVisible() && !leftView->gotoBar()->isVisible());
            CHECK(rightView->gotoBar()->lineEdit()->hasFocus());

            // 对门面本身 setFocus：焦点交给内部画布（焦点代理）。
            left->setFocus(Qt::OtherFocusReason);
            Flush();
            CHECK(QApplication::focusWidget() == leftView->canvas());
        }

        // aboutToShowContextMenu：画布构造菜单时，门面带着同一个菜单、地址、hasByte 把信号转发出来。
        void TestContextMenu()
        {
            ApplyTheme(false);
            const QByteArray data = MakePattern(0x4000);
            auto widget = MakeFacade(0x4000, data, false, QSize(900, 360));
            HexCanvas& canvas = *CanvasOf(*widget);

            // 用 lambda 取参数：QMenu* 在信号声明处只有前置声明，QSignalSpy 拿不到它的元类型。
            int menuSignals = 0;
            QMenu* seenMenu = nullptr;
            std::uint64_t seenAddress = 0;
            bool seenHasByte = false;
            const QMetaObject::Connection menuConnection = QObject::connect(
                widget.get(), &HexEditorWidget::aboutToShowContextMenu,
                [&](QMenu* signalMenu, std::uint64_t signalAddress, bool signalHasByte) {
                    ++menuSignals;
                    seenMenu = signalMenu;
                    seenAddress = signalAddress;
                    seenHasByte = signalHasByte;
                });
            QMenu* menu = canvas.buildContextMenu(0x4123, true);
            CHECK(menuSignals == 1);
            CHECK(seenMenu == menu && seenAddress == 0x4123 && seenHasByte);

            // 宿主在槽里追加动作（MemoryEditorWidget 的做法）：追加后菜单里能看到它。
            QObject::disconnect(menuConnection);
            const QMetaObject::Connection appendConnection = QObject::connect(
                widget.get(), &HexEditorWidget::aboutToShowContextMenu,
                [](QMenu* signalMenu, std::uint64_t, bool signalHasByte) {
                    if (signalHasByte)
                    {
                        signalMenu->addSeparator();
                        signalMenu->addAction(QStringLiteral("facade-test-extra-action"));
                    }
                });
            delete menu;
            menu = canvas.buildContextMenu(0x4200, true);
            CHECK(FindAction(menu, QStringLiteral("facade-test-extra-action")) != nullptr);
            delete menu;
            menu = canvas.buildContextMenu(0x4000, false);
            CHECK(FindAction(menu, QStringLiteral("facade-test-extra-action")) == nullptr);
            delete menu;
            QObject::disconnect(appendConnection);

            // hasByte 为假：参数原样转发。
            QObject::connect(widget.get(), &HexEditorWidget::aboutToShowContextMenu,
                [&](QMenu*, std::uint64_t signalAddress, bool signalHasByte) {
                    ++menuSignals;
                    seenAddress = signalAddress;
                    seenHasByte = signalHasByte;
                });
            const int signalsBefore = menuSignals;
            menu = canvas.buildContextMenu(0x4000, false);
            CHECK(menuSignals == signalsBefore + 1 && seenAddress == 0x4000 && !seenHasByte);
            delete menu;
        }

        // setHexOnlyView：隐藏 HexView 的状态条（统一编辑器自己有状态条），工具栏保留；false 恢复；可重复调用。
        void TestHexOnlyView()
        {
            ApplyTheme(false);
            auto widget = MakeFacade(0x1000, MakePattern(512), false, QSize(900, 300));
            HexView* view = ViewOf(*widget);
            CHECK(view->statusBarVisible() && view->toolbarVisible());
            CHECK(view->statusBar()->isVisible());

            widget->setHexOnlyView(true);
            QApplication::processEvents();
            CHECK(!view->statusBarVisible() && view->statusBar()->isHidden() && !view->statusBar()->isVisible());
            CHECK(view->toolbarVisible() && view->toolbar()->isVisible());
            const int heightHidden = view->canvas()->height();

            // 重复调用无害。
            widget->setHexOnlyView(true);
            QApplication::processEvents();
            CHECK(!view->statusBarVisible() && view->toolbarVisible());

            // 恢复：状态条回来并重新占用空间。
            widget->setHexOnlyView(false);
            QApplication::processEvents();
            CHECK(view->statusBarVisible() && view->statusBar()->isVisible() && view->toolbarVisible());
            CHECK(view->canvas()->height() < heightHidden);
            widget->setHexOnlyView(false);
            QApplication::processEvents();
            CHECK(view->statusBarVisible());

            // 嵌入形态：构造后、显示前就调用（MemoryEditorWidget 的做法），显示之后状态条仍然隐藏。
            auto embedded = std::make_unique<HexEditorWidget>();
            embedded->setBytesPerRow(16);
            embedded->setHexOnlyView(true);
            embedded->resize(900, 300);
            embedded->show();
            embedded->setByteArray(MakePattern(512), 0x1000);
            QApplication::processEvents();
            HexView* embeddedView = ViewOf(*embedded);
            CHECK(!embeddedView->statusBar()->isVisible() && embeddedView->toolbar()->isVisible());
            embedded->setHexOnlyView(false);
            QApplication::processEvents();
            CHECK(embeddedView->statusBar()->isVisible());
        }
    }

    // 第二组：信号与交互语义。
    void RunFacadeSignalTests()
    {
        TestByteEdited();
        TestSetByteAtAbsoluteAddress();
        TestSelectionSignals();
        TestJump();
        TestPanels();
        TestContextMenu();
        TestHexOnlyView();
    }
}
