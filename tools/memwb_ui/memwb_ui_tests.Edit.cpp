// memwb_ui_tests.Edit.cpp
// 作用：HexCanvas 编辑侧行为验证——半字节输入、ASCII 输入、粘贴、填充/NOP、被拒编辑、
// 复制三种以上格式、只读模式、右键菜单，以及纯文本格式化函数。
// 所有手势都经 QTest 模拟的真实按键/鼠标事件进入画布，结果通过 MemoryDiffOverlay 的公开接口核对。

#include "memwb_ui_common.h"

#include "../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvasFormat.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QSignalSpy>
#include <QTimer>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using Pane = HexCanvas::ActivePane;
        namespace fmt = ks::ui::hexcanvas_format;

        // SetClip：写剪贴板文本。
        void SetClip(const QString& text)
        {
            QGuiApplication::clipboard()->setText(text);
        }

        // Clip：读剪贴板文本。
        QString Clip()
        {
            return QGuiApplication::clipboard()->text();
        }

        // Bytes：初始化列表转字节向量，便于和 Materialize 的结果比较。
        std::vector<std::uint8_t> Bytes(std::initializer_list<int> values)
        {
            std::vector<std::uint8_t> result;
            for (const int value : values)
            {
                result.push_back(static_cast<std::uint8_t>(value));
            }
            return result;
        }

        // Effective：取叠加后的字节视图。
        std::vector<std::uint8_t> Effective(Fixture& fixture, std::uint64_t address, std::uint64_t length)
        {
            return fixture.overlay.Materialize(address, length).bytes;
        }

        // FindAction：按文字在菜单里找动作，找不到返回空指针。
        QAction* FindAction(QMenu* menu, const QString& text)
        {
            for (QAction* action : menu->actions())
            {
                if (action->text() == text)
                {
                    return action;
                }
            }
            return nullptr;
        }

        // 半字节输入流程：预览、暂存时机、自动前进、取消路径。
        void TestNibbleInput()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
            QSignalSpy discarded(&canvas, &HexCanvas::editDiscarded);
            canvas.setCaretAddress(0x1010);
            CHECK(fixture->data.at(0x10) == static_cast<char>(0x5B));

            // 高半字节：只显示预览，不暂存，不前进。
            Type(canvas, QStringLiteral("4"));
            HexCanvas::CellState preview = canvas.cellStateAt(0x1010);
            CHECK(preview.nibblePreview);
            CHECK(preview.hexText == QStringLiteral("4_"));
            CHECK(fixture->overlay.DiffBlocks().empty());
            CHECK(staged.isEmpty());
            CHECK(canvas.caretAddress() == 0x1010);

            // 低半字节：暂存 0x4A（高半字节在前），after 正确，caret 自动前进。
            Type(canvas, QStringLiteral("A"));
            std::vector<ksword::memwb::DiffBlock> blocks = fixture->overlay.DiffBlocks();
            CHECK(blocks.size() == 1);
            CHECK(!blocks.empty() && blocks[0].address == 0x1010);
            CHECK(!blocks.empty() && blocks[0].after == Bytes({ 0x4A }));
            CHECK(!blocks.empty() && blocks[0].before == Bytes({ 0x5B }));
            CHECK(staged.count() == 1);
            CHECK(staged.count() == 1 && staged.at(0).at(0).toULongLong() == 0x1010 && staged.at(0).at(1).toULongLong() == 1);
            CHECK(canvas.caretAddress() == 0x1011);
            const HexCanvas::CellState written = canvas.cellStateAt(0x1010);
            CHECK(!written.nibblePreview);
            CHECK(written.hexText == QStringLiteral("4A"));
            CHECK(written.change == HexCanvas::ChangeKind::Pending);
            CHECK(rejected.isEmpty());

            // 连续输入：下一个字节接着写，两个相邻补丁合并成一块。
            Type(canvas, QStringLiteral("BC"));
            blocks = fixture->overlay.DiffBlocks();
            CHECK(blocks.size() == 1 && blocks[0].after == Bytes({ 0x4A, 0xBC }));
            CHECK(canvas.caretAddress() == 0x1012);

            // 移动插入点取消半字节：Right 之后预览消失，也没有任何暂存。
            Type(canvas, QStringLiteral("D"));
            CHECK(canvas.cellStateAt(0x1012).nibblePreview);
            Key(canvas, Qt::Key_Right);
            CHECK(!canvas.cellStateAt(0x1012).nibblePreview);
            CHECK(canvas.caretAddress() == 0x1013);
            CHECK(fixture->overlay.DiffBlocks().size() == 1);
            CHECK(fixture->overlay.DiffBlocks()[0].after.size() == 2);

            // 小写十六进制也接受；与前块之间隔着未改的 0x1012，所以是第二个块。
            Type(canvas, QStringLiteral("ef"));
            blocks = fixture->overlay.DiffBlocks();
            CHECK(blocks.size() == 2);
            CHECK(blocks.size() == 2 && blocks[1].address == 0x1013 && blocks[1].after == Bytes({ 0xEF }));
            CHECK(canvas.caretAddress() == 0x1014);

            // Backspace：先取消半字节；没有半字节时回退一个字节并丢弃它的暂存。
            Type(canvas, QStringLiteral("5"));
            CHECK(canvas.cellStateAt(0x1014).nibblePreview);
            Key(canvas, Qt::Key_Backspace);
            CHECK(!canvas.cellStateAt(0x1014).nibblePreview);
            CHECK(canvas.caretAddress() == 0x1014);
            Type(canvas, QStringLiteral("67"));
            CHECK(fixture->overlay.DiffBlocks().size() == 2);
            CHECK(canvas.caretAddress() == 0x1015);
            const std::uint64_t beforeDiscard = fixture->overlay.PendingByteCount();
            Key(canvas, Qt::Key_Backspace);
            CHECK(canvas.caretAddress() == 0x1014);
            CHECK(fixture->overlay.PendingByteCount() == beforeDiscard - 1);
            CHECK(discarded.count() == 1);
            CHECK(discarded.count() == 1 && discarded.at(0).at(0).toULongLong() == 0x1014);

            // Esc 只取消半字节，不折叠选区也不移动。
            canvas.setCaretAddress(0x1030);
            canvas.setCaretAddress(0x1032, true);
            Type(canvas, QStringLiteral("9"));
            CHECK(canvas.cellStateAt(0x1032).nibblePreview);
            Key(canvas, Qt::Key_Escape);
            CHECK(!canvas.cellStateAt(0x1032).nibblePreview);
            CHECK(canvas.selectedRange()->first == 0x1030 && canvas.selectedRange()->last == 0x1032);

            // 切换面板、鼠标点击都会取消半字节。
            Type(canvas, QStringLiteral("9"));
            CHECK(canvas.cellStateAt(0x1032).nibblePreview);
            Key(canvas, Qt::Key_Tab);
            CHECK(!canvas.cellStateAt(0x1032).nibblePreview);
            Key(canvas, Qt::Key_Tab);
            Type(canvas, QStringLiteral("9"));
            Click(canvas, 0x1050);
            CHECK(!canvas.cellStateAt(0x1032).nibblePreview);

            // 选区多于一个字节时，输入作用在插入点并折叠选区，并前进到下一字节。
            canvas.setCaretAddress(0x1060);
            canvas.setCaretAddress(0x1065, true);
            Type(canvas, QStringLiteral("11"));
            CHECK(Effective(*fixture, 0x1065, 1) == Bytes({ 0x11 }));
            CHECK(canvas.caretAddress() == 0x1066);
            CHECK(canvas.selectedRange()->first == canvas.selectedRange()->last);

            // 非十六进制字符不被消费，不产生预览。
            const int before = staged.count();
            Type(canvas, QStringLiteral("g"));
            CHECK(!canvas.cellStateAt(canvas.caretAddress()).nibblePreview);
            CHECK(staged.count() == before);
            CHECK(rejected.isEmpty());
        }

        // ASCII 面板：可见字符直接写入并前进；不可见与非 ASCII 字符不写。
        void TestAsciiInput()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            canvas.setCaretAddress(0x1100);
            Key(canvas, Qt::Key_Tab);
            CHECK(canvas.activePane() == Pane::Ascii);

            Type(canvas, QStringLiteral("Z"));
            CHECK(Effective(*fixture, 0x1100, 1) == Bytes({ 0x5A }));
            CHECK(canvas.caretAddress() == 0x1101);
            CHECK(staged.count() == 1);
            CHECK(!canvas.cellStateAt(0x1101).nibblePreview);

            // 空格（0x20）是可见 ASCII 范围的下界。
            QTest::keyClick(&canvas, Qt::Key_Space);
            CHECK(Effective(*fixture, 0x1101, 1) == Bytes({ 0x20 }));
            CHECK(canvas.caretAddress() == 0x1102);

            // '~'（0x7E）是上界；Del 字符（0x7F）与回车不写。
            Type(canvas, QStringLiteral("~"));
            CHECK(Effective(*fixture, 0x1102, 1) == Bytes({ 0x7E }));
            const int count = staged.count();
            QKeyEvent deleteKey(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier, QString(QChar(0x7F)));
            QApplication::sendEvent(&canvas, &deleteKey);
            QKeyEvent enterKey(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            QApplication::sendEvent(&canvas, &enterKey);
            QKeyEvent chineseKey(QEvent::KeyPress, 0, Qt::NoModifier, QStringLiteral("中"));
            QApplication::sendEvent(&canvas, &chineseKey);
            CHECK(staged.count() == count);
            CHECK(canvas.caretAddress() == 0x1103);

            // Hex 面板不接受 ASCII 面板才有的字符（如 'z'）。
            Key(canvas, Qt::Key_Tab);
            Type(canvas, QStringLiteral("z"));
            CHECK(staged.count() == count);
        }

        // 粘贴：多种十六进制写法都接受；非法整体拒绝；越界拒绝；ASCII 面板按 UTF-8 原样写。
        void TestPaste()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);

            // 合法写法：空格、0x+逗号、换行、连续、花括号。
            const std::vector<std::pair<QString, std::vector<std::uint8_t>>> cases = {
                { QStringLiteral("DE AD BE EF"), Bytes({ 0xDE, 0xAD, 0xBE, 0xEF }) },
                { QStringLiteral("0xDE,0xAD, 0xBE ,0xEF"), Bytes({ 0xDE, 0xAD, 0xBE, 0xEF }) },
                { QStringLiteral("de\nad\r\nbe\tef"), Bytes({ 0xDE, 0xAD, 0xBE, 0xEF }) },
                { QStringLiteral("DEADBEEF"), Bytes({ 0xDE, 0xAD, 0xBE, 0xEF }) },
                { QStringLiteral("{ 0xDE, 0xAD }"), Bytes({ 0xDE, 0xAD }) },
            };
            std::uint64_t address = 0x1200;
            for (const auto& testCase : cases)
            {
                canvas.setCaretAddress(address);
                SetClip(testCase.first);
                Key(canvas, Qt::Key_V, Qt::ControlModifier);
                CHECK_NOTE(Effective(*fixture, address, testCase.second.size()) == testCase.second, testCase.first);
                CHECK(canvas.selectedRange()->first == address);
                CHECK(canvas.selectedRange()->last == address + testCase.second.size() - 1);
                address += 0x10;
            }
            CHECK(staged.count() == static_cast<int>(cases.size()));
            CHECK(rejected.isEmpty());

            // 非法内容整体拒绝：叠加层完全不变。
            const auto snapshot = fixture->overlay.DiffBlocks();
            const QStringList bad = {
                QStringLiteral("DE AD G1"), QStringLiteral("ABC"), QStringLiteral(""),
                QStringLiteral("   \n "), QStringLiteral("0x"), QStringLiteral("DE AD ZZ"),
                QStringLiteral("DE-AD") };
            int expectedRejects = 0;
            canvas.setCaretAddress(0x1400);
            for (const QString& text : bad)
            {
                SetClip(text);
                Key(canvas, Qt::Key_V, Qt::ControlModifier);
                ++expectedRejects;
                CHECK_NOTE(rejected.count() == expectedRejects, text);
            }
            CHECK(fixture->overlay.DiffBlocks() == snapshot);

            // 越界：从末字节开始粘 2 字节，整体拒绝，不截断。
            canvas.setCaretAddress(0x1FFF);
            SetClip(QStringLiteral("11 22"));
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(rejected.count() == expectedRejects + 1);
            CHECK(fixture->overlay.DiffBlocks() == snapshot);

            // ASCII 面板：文本按 UTF-8 字节原样写入（"中" 是 E4 B8 AD）。
            canvas.setCaretAddress(0x1300);
            Key(canvas, Qt::Key_Tab);
            SetClip(QStringLiteral("Hello"));
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(Effective(*fixture, 0x1300, 5) == Bytes({ 'H', 'e', 'l', 'l', 'o' }));
            canvas.setCaretAddress(0x1310);
            SetClip(QStringLiteral("中"));
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(Effective(*fixture, 0x1310, 3) == Bytes({ 0xE4, 0xB8, 0xAD }));
            SetClip(QString());
            const int rejectsBefore = rejected.count();
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(rejected.count() == rejectsBefore + 1);
        }

        // 填充 00 / FF / NOP：作用于选区，经槽与右键菜单两条路径；超大选区拒绝且不分配。
        void TestFillAndNop()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);

            canvas.setCaretAddress(0x1020);
            canvas.setCaretAddress(0x102F, true);
            QMetaObject::invokeMethod(&canvas, "fillSelection", Q_ARG(quint8, 0));
            CHECK(Effective(*fixture, 0x1020, 16) == std::vector<std::uint8_t>(16, 0x00));
            CHECK(staged.count() == 1);
            CHECK(staged.count() == 1 && staged.at(0).at(0).toULongLong() == 0x1020 && staged.at(0).at(1).toULongLong() == 16);
            CHECK(canvas.cellStateAt(0x1025).change == HexCanvas::ChangeKind::Pending);
            CHECK(canvas.cellStateAt(0x1025).hexText == QStringLiteral("00"));

            canvas.fillSelection(0xFF);
            CHECK(Effective(*fixture, 0x1020, 16) == std::vector<std::uint8_t>(16, 0xFF));

            // 经右键菜单的 NOP 填充（不弹对话框，直接生效）。
            QMenu* menu = canvas.buildContextMenu(0x1025, true);
            QAction* nop = FindAction(menu, QStringLiteral("NOP 填充（0x90）"));
            CHECK(nop != nullptr);
            CHECK(nop != nullptr && nop->isEnabled());
            if (nop != nullptr)
            {
                nop->trigger();
            }
            CHECK(Effective(*fixture, 0x1020, 16) == std::vector<std::uint8_t>(16, 0x90));
            QAction* zero = FindAction(menu, QStringLiteral("填充 00"));
            if (zero != nullptr)
            {
                zero->trigger();
            }
            CHECK(Effective(*fixture, 0x1020, 16) == std::vector<std::uint8_t>(16, 0x00));
            delete menu;

            // 单字节选区也能填充。
            canvas.setCaretAddress(0x1100);
            canvas.fillSelection(0x90);
            CHECK(Effective(*fixture, 0x1100, 1) == Bytes({ 0x90 }));
            CHECK(rejected.isEmpty());

            // 超大选区：整个 64 位空间全选后填充，直接拒绝（不会去分配 16 EiB）。
            HexCanvas giant;
            RecordingProvider provider;
            ksword::memwb::MemoryDiffOverlay giantOverlay;
            giant.resize(600, 300);
            giant.show();
            giant.setAddressSpace(0, 0xFFFFFFFFFFFFFFFFULL);
            giant.setPageProvider(&provider);
            giant.setOverlay(&giantOverlay);
            giant.setEditable(true);
            QSignalSpy giantRejected(&giant, &HexCanvas::editRejected);
            giant.selectAll();
            giant.fillSelection(0x00);
            CHECK(giantRejected.count() == 1);
            CHECK(!giantOverlay.HasPendingPatches());
        }

        // 被拒编辑：叠加层窗口外、基线里没读到的字节、页缓存里未加载/不可读的字节。
        void TestRejectedEdits()
        {
            ApplyTheme(false);

            // (a) 叠加层基线里 0x1040..0x104F 没读到，但页缓存里是有效的：
            //     第一个半字节通过预检，第二个半字节暂存时被叠加层拒绝。
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420), false);
            HexCanvas& canvas = *fixture->canvas;
            {
                std::vector<std::uint8_t> bytes(fixture->data.size());
                std::vector<std::uint8_t> mask(bytes.size(), 1);
                for (int index = 0; index < fixture->data.size(); ++index)
                {
                    bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(fixture->data.at(index));
                }
                for (int index = 0x40; index < 0x50; ++index)
                {
                    mask[static_cast<std::size_t>(index)] = 0;
                }
                fixture->overlay.LoadBaseline("memwb-rejected", 0x1000, bytes, mask);
            }
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);

            canvas.setCaretAddress(0x1044);
            Type(canvas, QStringLiteral("AA"));
            CHECK(rejected.count() == 1);
            CHECK(rejected.count() == 1 && rejected.at(0).at(0).toString().contains(QStringLiteral("尚未读取")));
            CHECK(staged.isEmpty());
            CHECK(fixture->overlay.DiffBlocks().empty());
            CHECK(!canvas.cellStateAt(0x1044).nibblePreview);

            // 跨过没读到字节的填充与粘贴也整体拒绝，一个字节都不暂存。
            canvas.setCaretAddress(0x103C);
            canvas.setCaretAddress(0x1050, true);
            canvas.fillSelection(0x00);
            CHECK(rejected.count() == 2);
            canvas.setCaretAddress(0x1048);
            SetClip(QStringLiteral("11 22 33"));
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(rejected.count() == 3);
            CHECK(fixture->overlay.DiffBlocks().empty());

            // (b) 叠加层窗口之外：基线只覆盖前 256 字节，编辑 0x1500 被拒（超出数据窗口）。
            {
                std::vector<std::uint8_t> bytes(256, 0x11);
                std::vector<std::uint8_t> mask(256, 1);
                fixture->overlay.LoadBaseline("memwb-window", 0x1000, bytes, mask);
            }
            canvas.setCaretAddress(0x1500);
            Type(canvas, QStringLiteral("AA"));
            CHECK(rejected.count() == 4);
            CHECK(rejected.count() == 4 && rejected.at(3).at(0).toString().contains(QStringLiteral("数据窗口")));
            CHECK(fixture->overlay.DiffBlocks().empty());

            // (c) 页缓存层：在途/未加载的字节 -> "尚未加载"；整页不可读 -> "不可读"。
            HexCanvas missing;
            RecordingProvider provider;
            ksword::memwb::MemoryDiffOverlay overlay;
            missing.resize(800, 360);
            missing.show();
            missing.setAddressSpace(0x10000, 0x13FFF);
            missing.setPageProvider(&provider);
            missing.setOverlay(&overlay);
            missing.setEditable(true);
            overlay.LoadBaseline("memwb-missing", 0x10000, std::vector<std::uint8_t>(0x4000, 0x22), std::vector<std::uint8_t>(0x4000, 1));
            QSignalSpy missingRejected(&missing, &HexCanvas::editRejected);
            missing.setCaretAddress(0x10005);
            Type(missing, QStringLiteral("4"));
            CHECK(missingRejected.count() == 1);
            CHECK(missingRejected.count() == 1 && missingRejected.at(0).at(0).toString().contains(QStringLiteral("尚未加载")));
            CHECK(!missing.cellStateAt(0x10005).nibblePreview);

            ks::ui::HexFetchRange unreadable;
            unreadable.firstPageStart = 0x10000;
            unreadable.pageCount = 1;
            missing.deliverUnreadable(unreadable, missing.sourceRevision());
            Type(missing, QStringLiteral("4"));
            CHECK(missingRejected.count() == 2);
            CHECK(missingRejected.count() == 2 && missingRejected.at(1).at(0).toString().contains(QStringLiteral("不可读")));
            CHECK(!overlay.HasPendingPatches());
        }

        // 复制：三个以上格式、Ctrl+C / Ctrl+Shift+C、含暂存补丁、拒绝路径。
        void TestCopyFormats()
        {
            ApplyTheme(false);
            QByteArray data = MakePattern(256);
            const QByteArray head("\x41\x42\x00\xFF\x01\x41\x22\x5C", 8);
            data.replace(0, head.size(), head);
            auto fixture = MakeStaticFixture(0x1000, data, true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy copyRejected(&canvas, &HexCanvas::copyRejected);

            // Hex 面板选 4 字节：Ctrl+C = 十六进制，Ctrl+Shift+C = ASCII。
            Click(canvas, 0x1000);
            Click(canvas, 0x1003, Pane::Hex, Qt::ShiftModifier);
            SetClip(QStringLiteral("sentinel"));
            Key(canvas, Qt::Key_C, Qt::ControlModifier);
            CHECK(Clip() == QStringLiteral("41 42 00 FF"));
            Key(canvas, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(Clip() == QStringLiteral("AB.."));

            // ASCII 面板：Ctrl+C = ASCII，Ctrl+Shift+C = 十六进制。
            Key(canvas, Qt::Key_Tab);
            Key(canvas, Qt::Key_C, Qt::ControlModifier);
            CHECK(Clip() == QStringLiteral("AB.."));
            Key(canvas, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
            CHECK(Clip() == QStringLiteral("41 42 00 FF"));
            Key(canvas, Qt::Key_Tab);

            // 右键菜单里的各种格式。
            QMenu* menu = canvas.buildContextMenu(0x1001, true);
            const auto copyVia = [&](const QString& text) {
                QAction* action = FindAction(menu, text);
                CHECK_NOTE(action != nullptr, text);
                if (action != nullptr)
                {
                    action->trigger();
                }
                return Clip();
            };
            CHECK(copyVia(QStringLiteral("复制十六进制")) == QStringLiteral("41 42 00 FF"));
            CHECK(copyVia(QStringLiteral("复制 ASCII 文本")) == QStringLiteral("AB.."));
            CHECK(copyVia(QStringLiteral("复制为 C 数组")) == QStringLiteral("{ 0x41, 0x42, 0x00, 0xFF }"));
            CHECK(copyVia(QStringLiteral("复制为 Python bytes")) == QStringLiteral("b'\\x41\\x42\\x00\\xFF'"));
            CHECK(copyVia(QStringLiteral("复制为转义字符串")) == QStringLiteral("\"AB\\x00\\xFF\""));
            CHECK(copyVia(QStringLiteral("复制地址")) == QStringLiteral("0x00001000"));
            delete menu;

            // 转义字符串的两个陷阱：\x 后面紧跟十六进制数字字符、引号与反斜杠。
            canvas.setCaretAddress(0x1004);
            canvas.setCaretAddress(0x1007, true);
            CHECK(canvas.selectionText(HexCanvas::CopyFormat::EscapedString, nullptr) == QStringLiteral("\"\\x01\\x41\\\"\\\\\""));

            // 复制的是所见值：暂存补丁（0x1001 -> 0x7A）出现在复制结果里。
            canvas.setCaretAddress(0x1001);
            Type(canvas, QStringLiteral("7A"));
            canvas.setCaretAddress(0x1000);
            canvas.setCaretAddress(0x1003, true);
            CHECK(canvas.selectionText(HexCanvas::CopyFormat::HexText, nullptr) == QStringLiteral("41 7A 00 FF"));

            // 选区含未加载字节：整体拒绝，剪贴板保持原样，不拿 00/?? 充数。
            HexCanvas pending;
            RecordingProvider provider;
            pending.resize(800, 360);
            pending.show();
            pending.setAddressSpace(0x10000, 0x13FFF);
            pending.setPageProvider(&provider);
            QSignalSpy pendingRejected(&pending, &HexCanvas::copyRejected);
            pending.setCaretAddress(0x10000);
            pending.setCaretAddress(0x10003, true);
            SetClip(QStringLiteral("sentinel"));
            Key(pending, Qt::Key_C, Qt::ControlModifier);
            CHECK(pendingRejected.count() == 1);
            CHECK(Clip() == QStringLiteral("sentinel"));
            CHECK(!pending.copySelection(HexCanvas::CopyFormat::CArray));
            CHECK(pendingRejected.count() == 2);
            // 仅复制地址不需要字节内容，不受影响。
            CHECK(pending.copySelection(HexCanvas::CopyFormat::AddressOnly));
            CHECK(Clip() == QStringLiteral("0x00010000"));

            // 整个 64 位空间全选：过大，拒绝。
            HexCanvas giant;
            RecordingProvider giantProvider;
            giant.resize(600, 300);
            giant.show();
            giant.setAddressSpace(0, 0xFFFFFFFFFFFFFFFFULL);
            giant.setPageProvider(&giantProvider);
            QSignalSpy giantRejected(&giant, &HexCanvas::copyRejected);
            giant.selectAll();
            CHECK(!giant.copySelection(HexCanvas::CopyFormat::HexText));
            CHECK(giantRejected.count() == 1);
            CHECK(giant.copySelection(HexCanvas::CopyFormat::AddressOnly));
            CHECK(Clip() == QStringLiteral("0x0000000000000000"));
            CHECK(copyRejected.isEmpty());
        }

        // 只读模式：所有编辑手势都不产生暂存；显式命令发 editRejected；菜单里编辑项置灰。
        void TestReadOnly()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), false, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&canvas, &HexCanvas::editRejected);
            QSignalSpy discarded(&canvas, &HexCanvas::editDiscarded);
            CHECK(!canvas.isEditable());

            // 按键手势：静默忽略，没有预览、没有暂存、没有拒绝提示。
            canvas.setCaretAddress(0x1010);
            Type(canvas, QStringLiteral("4A"));
            CHECK(!canvas.cellStateAt(0x1010).nibblePreview);
            Key(canvas, Qt::Key_Tab);
            Type(canvas, QStringLiteral("Z"));
            Key(canvas, Qt::Key_Backspace);
            Key(canvas, Qt::Key_Tab);
            CHECK(fixture->overlay.DiffBlocks().empty());
            CHECK(staged.isEmpty());
            CHECK(rejected.isEmpty());
            CHECK(discarded.isEmpty());
            CHECK(canvas.caretAddress() == 0x1010);

            // 显式命令：粘贴/填充被拒绝并说明原因，同样不产生暂存。
            SetClip(QStringLiteral("DE AD"));
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            canvas.fillSelection(0x00);
            CHECK(rejected.count() == 2);
            CHECK(rejected.count() == 2 && rejected.at(0).at(0).toString().contains(QStringLiteral("只读")));
            CHECK(fixture->overlay.DiffBlocks().empty());
            CHECK(!fixture->overlay.HasPendingPatches());

            // 菜单：复制可用，粘贴与三种填充置灰。
            QMenu* menu = canvas.buildContextMenu(0x1010, true);
            CHECK(FindAction(menu, QStringLiteral("复制十六进制"))->isEnabled());
            CHECK(!FindAction(menu, QStringLiteral("粘贴"))->isEnabled());
            CHECK(!FindAction(menu, QStringLiteral("填充 00"))->isEnabled());
            CHECK(!FindAction(menu, QStringLiteral("填充 FF"))->isEnabled());
            CHECK(!FindAction(menu, QStringLiteral("NOP 填充（0x90）"))->isEnabled());
            delete menu;

            // 可编辑但没有叠加层：同样什么都不写，也不崩溃。
            canvas.setOverlay(nullptr);
            canvas.setEditable(true);
            Type(canvas, QStringLiteral("4A"));
            CHECK(staged.isEmpty());
            Key(canvas, Qt::Key_V, Qt::ControlModifier);
            CHECK(staged.isEmpty());
            CHECK(rejected.count() == 3);

            // 编辑中途切到只读：半字节被取消。
            canvas.setOverlay(&fixture->overlay);
            Type(canvas, QStringLiteral("4"));
            CHECK(canvas.cellStateAt(canvas.caretAddress()).nibblePreview);
            canvas.setEditable(false);
            CHECK(!canvas.cellStateAt(canvas.caretAddress()).nibblePreview);
        }

        // 右键菜单：内置项、图标、悬停提示、不透明样式、宿主追加、右键选区规则、深浅主题渲染。
        void TestContextMenu()
        {
            ApplyTheme(false);
            auto fixture = MakeStaticFixture(0x1000, MakePattern(4096), true, QSize(900, 420));
            HexCanvas& canvas = *fixture->canvas;
            canvas.setCaretAddress(0x1010);
            canvas.setCaretAddress(0x1013, true);

            // 宿主在信号槽里追加自己的项（取代旧控件的 aboutToShowContextMenu）。
            QSignalSpy aboutToShow(&canvas, &HexCanvas::contextMenuAboutToShow);
            QMenu* seenMenu = nullptr;          // 槽里看到的菜单指针
            quint64 seenAddress = 0;            // 槽里看到的地址
            bool seenHasByte = false;           // 槽里看到的 hasByte
            QObject::connect(&canvas, &HexCanvas::contextMenuAboutToShow, &canvas, [&](QMenu* menu, quint64 address, bool hasByte) {
                seenMenu = menu;
                seenAddress = address;
                seenHasByte = hasByte;
                menu->addSeparator();
                menu->addAction(QStringLiteral("宿主追加项"));
            });
            QMenu* menu = canvas.buildContextMenu(0x1012, true);
            CHECK(aboutToShow.count() == 1);
            CHECK(seenMenu == menu);
            CHECK(seenAddress == 0x1012);
            CHECK(seenHasByte);
            CHECK(FindAction(menu, QStringLiteral("宿主追加项")) != nullptr);

            // 内置项：每项都有悬停提示与图标（夹具内嵌了精简 qrc）。
            int builtin = 0;
            bool allTips = true;
            bool allIcons = true;
            for (QAction* action : menu->actions())
            {
                if (action->isSeparator() || action->text() == QStringLiteral("宿主追加项"))
                {
                    continue;
                }
                ++builtin;
                allTips = allTips && !action->toolTip().isEmpty() && action->toolTip() != action->text();
                allIcons = allIcons && !action->icon().isNull();
            }
            CHECK(builtin == 10);
            CHECK(allTips);
            CHECK(allIcons);

            // 样式：不透明、显式背景/文字/选中/禁用，使用主题静态色。
            CHECK(!menu->testAttribute(Qt::WA_TranslucentBackground));
            CHECK(menu->autoFillBackground());
            CHECK(menu->toolTipsVisible());
            CHECK(menu->styleSheet().contains(KswordTheme::SurfaceColorHex()));
            CHECK(menu->styleSheet().contains(KswordTheme::TextDisabledColorHex()));
            CHECK(menu->styleSheet().contains(QStringLiteral("QMenu::item:selected")));
            CHECK(menu->styleSheet().contains(QStringLiteral("QMenu::item:disabled")));
            CHECK(!menu->styleSheet().contains(QStringLiteral("palette(")));

            // 实际渲染：浅色与深色下菜单背景都是对应主题的表面色，而不是黑底。
            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                QMenu* themed = canvas.buildContextMenu(0x1012, true);
                themed->popup(QPoint(40, 40));
                QApplication::processEvents();
                const QImage image = themed->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QColor probe = image.pixelColor(image.width() - 6, image.height() / 2);
                CHECK_NOTE(ColorsClose(probe, KswordTheme::SurfaceColor(), 2), probe.name());
                themed->close();
                delete themed;
            }
            ApplyTheme(false);
            delete menu;

            // 右键选区规则：落在未选中字节上先选中该字节；落在选区内保持原选区。
            // contextMenuEvent 会 exec 弹出菜单，用定时器在菜单出现后把它关掉。
            QTimer closer;
            closer.setInterval(30);
            QObject::connect(&closer, &QTimer::timeout, &closer, [&]() {
                if (QWidget* popup = QApplication::activePopupWidget())
                {
                    popup->close();
                }
            });
            closer.start();
            QPoint outside = canvas.cellRect(0x1050, Pane::Hex).center();
            QContextMenuEvent rightOutside(QContextMenuEvent::Mouse, outside, canvas.viewport()->mapToGlobal(outside));
            QApplication::sendEvent(canvas.viewport(), &rightOutside);
            CHECK(canvas.selectedRange()->first == 0x1050 && canvas.selectedRange()->last == 0x1050);

            canvas.setCaretAddress(0x1010);
            canvas.setCaretAddress(0x1013, true);
            QPoint inside = canvas.cellRect(0x1012, Pane::Hex).center();
            QContextMenuEvent rightInside(QContextMenuEvent::Mouse, inside, canvas.viewport()->mapToGlobal(inside));
            QApplication::sendEvent(canvas.viewport(), &rightInside);
            CHECK(canvas.selectedRange()->first == 0x1010 && canvas.selectedRange()->last == 0x1013);

            // 右键落在 ASCII 面板：面板随之切换。
            QPoint asciiPoint = canvas.cellRect(0x1060, Pane::Ascii).center();
            QContextMenuEvent rightAscii(QContextMenuEvent::Mouse, asciiPoint, canvas.viewport()->mapToGlobal(asciiPoint));
            QApplication::sendEvent(canvas.viewport(), &rightAscii);
            CHECK(canvas.activePane() == Pane::Ascii);
            CHECK(canvas.selectedRange()->first == 0x1060);
            closer.stop();
        }

        // 纯文本格式化与解析函数。
        void TestFormatHelpers()
        {
            // 可见 ASCII 边界。
            CHECK(!fmt::IsPrintableAscii(0x1F));
            CHECK(fmt::IsPrintableAscii(0x20));
            CHECK(fmt::IsPrintableAscii(0x7E));
            CHECK(!fmt::IsPrintableAscii(0x7F));
            CHECK(!fmt::IsPrintableAscii(0x80));

            // 空与单字节。
            CHECK(fmt::FormatHexText(QByteArray()).isEmpty());
            CHECK(fmt::FormatCArray(QByteArray()) == QStringLiteral("{ }"));
            CHECK(fmt::FormatPythonBytes(QByteArray()) == QStringLiteral("b''"));
            CHECK(fmt::FormatEscapedString(QByteArray()) == QStringLiteral("\"\""));
            CHECK(fmt::FormatHexText(QByteArray("\x0A", 1)) == QStringLiteral("0A"));

            // 超过 16 字节的 C 数组按行折叠，缩进四空格，末行无逗号。
            QByteArray seventeen;
            for (int index = 0; index < 17; ++index)
            {
                seventeen.append(static_cast<char>(index));
            }
            const QString array = fmt::FormatCArray(seventeen);
            CHECK(array.startsWith(QStringLiteral("{\n    0x00, 0x01")));
            CHECK(array.endsWith(QStringLiteral("0x0F,\n    0x10\n}")));
            CHECK(array.count(QLatin1Char('\n')) == 3);

            // 地址位数。
            CHECK(fmt::FormatAddress(0x1000, 8) == QStringLiteral("0x00001000"));
            CHECK(fmt::FormatAddress(0x00007FFF00000000ULL, 16) == QStringLiteral("0x00007FFF00000000"));
            CHECK(fmt::FormatAddress(0x00007FFF00000000ULL, 8) == QStringLiteral("0x7FFF00000000"));
            CHECK(fmt::FormatAddress(0xFFFFFFFFFFFFFFFFULL, 16) == QStringLiteral("0xFFFFFFFFFFFFFFFF"));

            // 解析：成功与失败，失败时不改结果缓冲。
            QByteArray out("keep");
            QString error;
            CHECK(!fmt::ParseHexText(QStringLiteral("12 3"), &out, &error));
            CHECK(out == QByteArray("keep"));
            CHECK(!error.isEmpty());
            CHECK(fmt::ParseHexText(QStringLiteral(" 0X1f ,0xA0\n"), &out, &error));
            CHECK(out == QByteArray("\x1F\xA0", 2));
            CHECK(fmt::ParseHexText(QStringLiteral("1f"), &out, nullptr));
            CHECK(!fmt::ParseHexText(QStringLiteral("0x1"), &out, nullptr));
            CHECK(!fmt::ParseHexText(QStringLiteral("1f g"), &out, nullptr));
        }
    }

    // 本文件全部测试的入口。
    void RunEditTests()
    {
        TestNibbleInput();
        TestAsciiInput();
        TestPaste();
        TestFillAndNop();
        TestRejectedEdits();
        TestCopyFormats();
        TestReadOnly();
        TestContextMenu();
        TestFormatHelpers();
    }
}
