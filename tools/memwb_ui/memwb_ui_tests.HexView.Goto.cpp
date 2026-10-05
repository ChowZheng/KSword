// memwb_ui_tests.HexView.Goto.cpp
// 作用：HexView 跳转条的离屏验证——
//   1) HexGotoBar::Resolve 纯函数：地址默认十六进制（纯数字 1233 就是 0x1233、绝不是十进制 0x4D1）、0x 前缀与分隔符、
//      加号相加、不支持减法/模块名/解引用的明确提示、溢出、范围外提示"超出数据范围 [first, last]"（三种模式各用各的单位）、
//      行号模式（十进制、按画布行对齐、未对齐基址、48 字节行宽、末端不回绕）、无数据；
//   2) 经真实键盘输入的界面流程：回车跳转、状态条确认、行内错误提示（不弹模态框）、输入/切换模式清错误、Esc/关闭按钮；
//   3) 历史：去重置顶、上限 10 条、失败输入不入历史、持久化到 QSettings、Up/Down 翻历史并恢复草稿、历史菜单样式与内容、
//      打开时重读；历史与设置的纯函数。

#include "memwb_ui_hexview.h"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QMenu>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexGotoBar;
        using ks::ui::HexView;
        using Mode = HexGotoBar::Mode;
        namespace hs = ks::ui::hexview_settings;

        // kHistoryKey：跳转历史的键。
        const char* kHistoryKey = "memwb/hexview/gotoHistory";

        // Space：构造目标空间。
        HexGotoBar::Space MakeSpace(std::uint64_t first, std::uint64_t last, int bytesPerRow = 16)
        {
            HexGotoBar::Space space;
            space.valid = true;
            space.first = first;
            space.last = last;
            space.bytesPerRow = bytesPerRow;
            return space;
        }

        // Resolved：解析结果。
        struct Resolved
        {
            bool ok = false;            // 是否成功
            std::uint64_t address = 0;  // 目标地址
            QString error;              // 失败原因
        };

        // Resolve：HexGotoBar::Resolve 的包装。
        Resolved Resolve(Mode mode, const QString& text, const HexGotoBar::Space& space)
        {
            Resolved result;
            result.ok = HexGotoBar::Resolve(mode, text, space, &result.address, &result.error);
            return result;
        }

        // 纯函数：绝对地址。
        void TestResolveAbsolute()
        {
            const HexGotoBar::Space low = MakeSpace(0, 0xFFFF);

            // 默认十六进制：纯数字 1233 就是 0x1233，绝不是十进制（0x4D1）。
            Resolved result = Resolve(Mode::Absolute, QStringLiteral("1233"), low);
            CHECK(result.ok && result.address == 0x1233);
            CHECK(result.address != 1233);
            for (const QString& text : { QStringLiteral("0x1233"), QStringLiteral("0X1233"), QStringLiteral("  1233  "),
                                         QStringLiteral("12`33"), QStringLiteral("1_233"), QStringLiteral("0x12_33") })
            {
                result = Resolve(Mode::Absolute, text, low);
                CHECK_NOTE(result.ok && result.address == 0x1233, text);
            }

            // 加号相加；记号之间的空白被忽略。
            result = Resolve(Mode::Absolute, QStringLiteral("1000+20"), low);
            CHECK(result.ok && result.address == 0x1020);
            result = Resolve(Mode::Absolute, QStringLiteral("0x10 + 0x20"), low);
            CHECK(result.ok && result.address == 0x30);

            // 边界：首末地址成立；范围外的提示是"超出数据范围 [起点, 终点]"，地址位数与画布一致。
            const HexGotoBar::Space window = MakeSpace(0x1000, 0x2FFF);
            CHECK(Resolve(Mode::Absolute, QStringLiteral("1000"), window).ok);
            CHECK(Resolve(Mode::Absolute, QStringLiteral("2FFF"), window).ok);
            result = Resolve(Mode::Absolute, QStringLiteral("FFF"), window);
            CHECK(!result.ok && result.error == QStringLiteral("超出数据范围 [0x00001000, 0x00002FFF]"));
            result = Resolve(Mode::Absolute, QStringLiteral("3000"), window);
            CHECK(!result.ok && result.error == QStringLiteral("超出数据范围 [0x00001000, 0x00002FFF]"));
            CHECK(result.address == 0);

            // 同样的纯数字，在更大的空间里就在范围内——确认超范围判定用的是十六进制值。
            CHECK(Resolve(Mode::Absolute, QStringLiteral("3000"), MakeSpace(0x1000, 0x3FFF)).ok);

            // 64 位空间：位数 16；地址 0 与最大地址都能表达，不回绕。
            const HexGotoBar::Space wide = MakeSpace(0x00007FF600001000ULL, 0x00007FF600001FFFULL);
            result = Resolve(Mode::Absolute, QStringLiteral("1000"), wide);
            CHECK(!result.ok && result.error == QStringLiteral("超出数据范围 [0x00007FF600001000, 0x00007FF600001FFF]"));
            CHECK(Resolve(Mode::Absolute, QStringLiteral("7FF600001500"), wide).ok);
            const HexGotoBar::Space top = MakeSpace(0xFFFFFFFFFFFFFF00ULL, 0xFFFFFFFFFFFFFFFFULL);
            result = Resolve(Mode::Absolute, QStringLiteral("FFFFFFFFFFFFFFFF"), top);
            CHECK(result.ok && result.address == 0xFFFFFFFFFFFFFFFFULL);
            CHECK(!Resolve(Mode::Absolute, QStringLiteral("0"), top).ok);

            // 错误：空输入、减法、模块名、解引用、格式、溢出、语法。
            CHECK(Resolve(Mode::Absolute, QString(), low).error == QStringLiteral("请输入地址"));
            CHECK(Resolve(Mode::Absolute, QStringLiteral("   "), low).error == QStringLiteral("请输入地址"));
            CHECK(Resolve(Mode::Absolute, QStringLiteral("1000-10"), low).error.contains(QStringLiteral("不支持减法")));
            for (const QString& text : { QStringLiteral("client.dll"), QStringLiteral("[1000]"), QStringLiteral("12zz"), QStringLiteral("中") })
            {
                result = Resolve(Mode::Absolute, text, low);
                CHECK_NOTE(!result.ok && result.error.contains(QStringLiteral("不支持模块名")), text);
            }
            result = Resolve(Mode::Absolute, QStringLiteral("0x"), low);
            CHECK(!result.ok && result.error == QStringLiteral("数字格式不正确（第 1 个字符处）"));
            result = Resolve(Mode::Absolute, QStringLiteral("1__2"), low);
            CHECK(!result.ok && result.error.contains(QStringLiteral("数字格式不正确")));
            CHECK(Resolve(Mode::Absolute, QStringLiteral("FFFFFFFFFFFFFFFFF"), low).error == QStringLiteral("数值超过 64 位范围"));
            CHECK(Resolve(Mode::Absolute, QStringLiteral("FFFFFFFFFFFFFFFF+1"), low).error == QStringLiteral("数值超过 64 位范围"));
            CHECK(Resolve(Mode::Absolute, QStringLiteral("100+"), low).error.contains(QStringLiteral("表达式语法错误")));

            // 没有数据。
            CHECK(Resolve(Mode::Absolute, QStringLiteral("10"), HexGotoBar::Space()).error == QStringLiteral("没有可跳转的数据"));
            CHECK(Resolve(Mode::Offset, QStringLiteral("10"), HexGotoBar::Space()).error == QStringLiteral("没有可跳转的数据"));
            CHECK(Resolve(Mode::Row, QStringLiteral("1"), HexGotoBar::Space()).error == QStringLiteral("没有可跳转的数据"));
            HexGotoBar::Space reversed = MakeSpace(5, 3);
            CHECK(Resolve(Mode::Absolute, QStringLiteral("4"), reversed).error == QStringLiteral("没有可跳转的数据"));

            // 出参可以为空指针，不崩溃。
            CHECK(HexGotoBar::Resolve(Mode::Absolute, QStringLiteral("10"), low, nullptr, nullptr));
            CHECK(!HexGotoBar::Resolve(Mode::Absolute, QStringLiteral("zz"), low, nullptr, nullptr));
        }

        // 纯函数：相对偏移与行号。
        void TestResolveOffsetAndRow()
        {
            // 偏移：十六进制默认，目标 = 起点 + 偏移，范围提示以偏移为单位。
            const HexGotoBar::Space window = MakeSpace(0x1000, 0x1FFF);
            CHECK(Resolve(Mode::Offset, QStringLiteral("10"), window).address == 0x1010);
            CHECK(Resolve(Mode::Offset, QStringLiteral("0"), window).address == 0x1000);
            CHECK(Resolve(Mode::Offset, QStringLiteral("FFF"), window).address == 0x1FFF);
            Resolved result = Resolve(Mode::Offset, QStringLiteral("1000"), window);
            CHECK(!result.ok && result.error == QStringLiteral("超出数据范围 [0x00000000, 0x00000FFF]"));
            result = Resolve(Mode::Offset, QStringLiteral("FFFFFFFFFFFFFFFF"), window);
            CHECK(!result.ok && result.error.contains(QStringLiteral("超出数据范围")));
            CHECK(Resolve(Mode::Offset, QString(), window).error == QStringLiteral("请输入偏移"));
            CHECK(Resolve(Mode::Offset, QStringLiteral("10+10"), window).address == 0x1020);
            const HexGotoBar::Space top = MakeSpace(0xFFFFFFFFFFFFF000ULL, 0xFFFFFFFFFFFFFFFFULL);
            CHECK(Resolve(Mode::Offset, QStringLiteral("FFF"), top).address == 0xFFFFFFFFFFFFFFFFULL);
            CHECK(!Resolve(Mode::Offset, QStringLiteral("1000"), top).ok);

            // 行号：十进制，从 0 起，按绝对地址对齐的画布行；16 行（256 字节）。
            const HexGotoBar::Space rows = MakeSpace(0x1000, 0x10FF, 16);
            CHECK(Resolve(Mode::Row, QStringLiteral("0"), rows).address == 0x1000);
            CHECK(Resolve(Mode::Row, QStringLiteral("5"), rows).address == 0x1050);
            CHECK(Resolve(Mode::Row, QStringLiteral("15"), rows).address == 0x10F0);
            CHECK(Resolve(Mode::Row, QStringLiteral(" 5 "), rows).address == 0x1050);
            CHECK(Resolve(Mode::Row, QStringLiteral("0010"), rows).address == 0x10A0);
            result = Resolve(Mode::Row, QStringLiteral("16"), rows);
            CHECK(!result.ok && result.error == QStringLiteral("超出数据范围 [0, 15]"));

            // 同一个文本在行号模式是十进制：10 -> 第 10 行 = 0x10A0，而不是十六进制的第 16 行。
            CHECK(Resolve(Mode::Row, QStringLiteral("10"), rows).address == 0x10A0);

            // 未对齐的基址：第 0 行夹取到数据起点，之后每行对齐；总行数多一行。
            const HexGotoBar::Space unaligned = MakeSpace(0x1008, 0x1107, 16);
            CHECK(Resolve(Mode::Row, QStringLiteral("0"), unaligned).address == 0x1008);
            CHECK(Resolve(Mode::Row, QStringLiteral("1"), unaligned).address == 0x1010);
            CHECK(Resolve(Mode::Row, QStringLiteral("16"), unaligned).address == 0x1100);
            CHECK(Resolve(Mode::Row, QStringLiteral("17"), unaligned).error == QStringLiteral("超出数据范围 [0, 16]"));

            // 其它行宽。
            CHECK(Resolve(Mode::Row, QStringLiteral("3"), MakeSpace(0, 0xFFF, 32)).address == 96);
            CHECK(Resolve(Mode::Row, QStringLiteral("2"), MakeSpace(0, 0xFFF, 8)).address == 16);

            // 48 字节行宽贴着 64 位末端（2^64 不是 48 的倍数）：最后一行的地址仍在范围内，下一行越界，不回绕。
            const HexGotoBar::Space tail = MakeSpace(0xFFFFFFFFFFFFFF00ULL, 0xFFFFFFFFFFFFFFFFULL, 48);
            const ksword::memwb::HexViewport geometry(tail.first, tail.last, 48U, 1U);
            const std::uint64_t rowCount = geometry.RowCount();
            CHECK(rowCount >= 5);
            result = Resolve(Mode::Row, QString::number(rowCount - 1ULL), tail);
            CHECK(result.ok && result.address >= tail.first && result.address <= tail.last);
            CHECK(!Resolve(Mode::Row, QString::number(rowCount), tail).ok);

            // 错误：不是十进制数字、符号、小数、全角数字、溢出、空。
            for (const QString& text : { QStringLiteral("1f"), QStringLiteral("-1"), QStringLiteral("+3"),
                                         QStringLiteral("3.5"), QStringLiteral("０"), QStringLiteral("0x10") })
            {
                result = Resolve(Mode::Row, text, rows);
                CHECK_NOTE(!result.ok && result.error == QStringLiteral("行号必须是十进制数字"), text);
            }
            CHECK(Resolve(Mode::Row, QStringLiteral("99999999999999999999999"), rows).error == QStringLiteral("数值超过 64 位范围"));
            CHECK(Resolve(Mode::Row, QString(), rows).error == QStringLiteral("请输入行号"));
        }

        // 界面流程：真实键盘输入、状态条确认、行内错误、清错误、Esc/关闭按钮、没有模态框。
        void TestGotoBarInteraction()
        {
            ApplyTheme(false);
            auto view = MakeHexView(0x1000, MakePattern(0x2000), false, QSize(1000, 420));
            ks::ui::HexGotoBar& bar = *view->gotoBar();
            HexCanvas& canvas = *view->canvas();
            QSignalSpy requested(&bar, &HexGotoBar::gotoRequested);
            const auto errorLabel = [&]() { return bar.errorLabel(); };

            // 工具栏按钮打开；悬停提示齐全。
            QTest::mouseClick(view->gotoButton(), Qt::LeftButton);
            Flush();
            CHECK(bar.isVisible());
            for (ks::ui::HexViewGlyphButton* button : { bar.goButton(), bar.historyButton(), bar.closeButton() })
            {
                CHECK(!button->toolTip().isEmpty());
            }
            for (int index = 0; index < 3; ++index)
            {
                CHECK(!bar.modeSegment()->segmentToolTip(index).isEmpty());
            }
            CHECK(bar.mode() == Mode::Absolute);

            // 纯数字按十六进制：1233 -> 0x1233（落在 0x1000..0x2FFF 内）；状态条确认；插入点与选区就位并居中。
            QTest::keyClicks(bar.lineEdit(), QStringLiteral("1233"));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Return);
            Flush();
            CHECK(requested.count() == 1 && requested.at(0).at(0).toULongLong() == 0x1233);
            CHECK(view->caretAddress() == 0x1233);
            CHECK(view->selectedBytes().size() == 1);
            CHECK(view->statusBar()->hasMessage());
            CHECK(view->statusBar()->messageText() == QStringLiteral("已跳转到 0x00001233"));
            CHECK(view->statusBar()->messageKind() == ks::ui::HexViewStatusBar::Kind::Info);
            CHECK(!canvas.cellRect(0x1233, HexCanvas::ActivePane::Hex).isNull());
            CHECK(bar.errorText().isEmpty());
            CHECK(bar.isVisible());

            // 范围外：行内错误（输入框下方），不弹模态框，插入点不动，不发 gotoRequested。
            bar.setInputText(QString());
            QTest::keyClicks(bar.lineEdit(), QStringLiteral("9999"));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Return);
            Flush();
            CHECK(requested.count() == 1);
            CHECK(bar.errorText() == QStringLiteral("超出数据范围 [0x00001000, 0x00002FFF]"));
            CHECK(errorLabel() != nullptr && errorLabel()->isVisible());
            CHECK(errorLabel()->kind() == ks::ui::HexViewMessageLabel::Kind::Error);
            CHECK(view->caretAddress() == 0x1233);
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                CHECK(!(widget->isVisible() && qobject_cast<QDialog*>(widget) != nullptr));
            }

            // 错误在下一次输入时清除；错误行隐藏。
            QTest::keyClick(bar.lineEdit(), Qt::Key_Backspace);
            CHECK(bar.errorText().isEmpty() && !errorLabel()->isVisible());

            // 切换模式清错误：先制造错误再点"偏移"段。
            bar.setInputText(QStringLiteral("zz"));
            CHECK(!bar.submit() && !bar.errorText().isEmpty());
            QTest::mouseClick(bar.modeSegment(), Qt::LeftButton, Qt::NoModifier, bar.modeSegment()->segmentRect(1).center());
            CHECK(bar.mode() == Mode::Offset && bar.errorText().isEmpty());

            // 偏移模式：十六进制默认，目标 = 起点 + 偏移（基址 0x1000）。
            bar.setInputText(QStringLiteral("10"));
            CHECK(bar.submit());
            Flush();
            CHECK(view->caretAddress() == 0x1010);
            bar.setInputText(QStringLiteral("2000"));
            CHECK(!bar.submit());
            CHECK(bar.errorText() == QStringLiteral("超出数据范围 [0x00000000, 0x00001FFF]"));

            // 行号模式：十进制；点"行号"段。
            QTest::mouseClick(bar.modeSegment(), Qt::LeftButton, Qt::NoModifier, bar.modeSegment()->segmentRect(2).center());
            CHECK(bar.mode() == Mode::Row);
            bar.setInputText(QStringLiteral("10"));
            CHECK(bar.submit());
            Flush();
            CHECK(view->caretAddress() == 0x1000 + 10 * 16);
            bar.setInputText(QStringLiteral("512"));
            CHECK(!bar.submit());
            CHECK(bar.errorText() == QStringLiteral("超出数据范围 [0, 511]"));

            // 空输入：明确提示。
            bar.setInputText(QString());
            QTest::keyClick(bar.lineEdit(), Qt::Key_Return);
            CHECK(bar.errorText() == QStringLiteral("请输入行号"));

            // 行宽变化后行号模式用新行宽。
            CHECK(view->setBytesPerRow(32));
            bar.setInputText(QStringLiteral("2"));
            CHECK(bar.submit());
            Flush();
            CHECK(view->caretAddress() == 0x1000 + 2 * 32);

            // 关闭按钮：条隐藏。
            QTest::mouseClick(bar.closeButton(), Qt::LeftButton);
            Flush();
            CHECK(!bar.isVisible());

            // 重新打开：保留上次的模式；Esc 关闭（需要活动窗口）。
            view->openGoto();
            Flush();
            CHECK(bar.isVisible() && bar.mode() == Mode::Row);
            if (ActivateWindow(view.get()))
            {
                bar.lineEdit()->setFocus(Qt::OtherFocusReason);
                QTest::keyClick(bar.lineEdit(), Qt::Key_Escape);
                Flush();
                CHECK(!bar.isVisible());
            }

            // 没有数据：跳转条报"没有可跳转的数据"；setSpace 非法参数等同没有数据。
            view->clearBuffer();
            view->openGoto();
            bar.setMode(Mode::Absolute);
            bar.setInputText(QStringLiteral("10"));
            CHECK(!bar.submit() && bar.errorText() == QStringLiteral("没有可跳转的数据"));
            bar.setSpace(5, 3, 16);
            CHECK(!bar.space().valid);
            bar.setSpace(0, 10, 7);
            CHECK(!bar.space().valid);
            bar.setSpace(0, 10, 16);
            CHECK(bar.space().valid && bar.space().last == 10);
            bar.clearSpace();
            CHECK(!bar.space().valid);
        }

        // 历史：去重置顶、上限、失败输入不入历史、持久化、Up/Down、菜单、打开时重读。
        void TestGotoHistory()
        {
            ApplyTheme(false);
            {
                QSettings settings;
                settings.clear();
                settings.sync();
            }
            HexGotoBar bar;
            bar.resize(900, 60);
            bar.show();
            bar.setSpace(0, 0xFFFF, 16);
            CHECK(bar.history().isEmpty());

            // 成功的输入进历史：去重置顶；失败的（格式错误、范围外、空）不进。
            for (const QString& text : { QStringLiteral("1233"), QStringLiteral("10"), QStringLiteral("1233") })
            {
                bar.setInputText(text);
                CHECK(bar.submit());
            }
            CHECK(bar.history() == (QStringList{ QStringLiteral("1233"), QStringLiteral("10") }));
            bar.setInputText(QStringLiteral("zzzz"));
            CHECK(!bar.submit());
            bar.setInputText(QStringLiteral("99999"));
            CHECK(!bar.submit());
            bar.setInputText(QString());
            CHECK(!bar.submit());
            CHECK(bar.history() == (QStringList{ QStringLiteral("1233"), QStringLiteral("10") }));

            // 上限 10 条，最近的在最前。
            for (int index = 0; index < 12; ++index)
            {
                bar.setInputText(QStringLiteral("A%1").arg(index, 2, 16, QLatin1Char('0')).toUpper());
                CHECK(bar.submit());
            }
            CHECK(bar.history().size() == 10);
            CHECK(bar.history().first() == QStringLiteral("A0B"));
            CHECK(bar.history().at(1) == QStringLiteral("A0A"));
            CHECK(!bar.history().contains(QStringLiteral("1233")));

            // 持久化：设置里的原始值、纯函数读取、新条读取三者一致。
            const QStringList expected = bar.history();
            {
                const QSettings settings;
                CHECK(settings.value(QString::fromLatin1(kHistoryKey)).toStringList() == expected);
            }
            CHECK(hs::LoadGotoHistory() == expected);
            HexGotoBar other;
            CHECK(other.history() == expected);

            // Up/Down 翻历史：Up 更旧，Down 更新，翻过最新一条恢复草稿，再 Down 不变。
            bar.setMode(Mode::Absolute);
            bar.lineEdit()->setFocus(Qt::OtherFocusReason);
            bar.setInputText(QString());
            QTest::keyClicks(bar.lineEdit(), QStringLiteral("ab"));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Up);
            CHECK(bar.lineEdit()->text() == expected.at(0));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Up);
            CHECK(bar.lineEdit()->text() == expected.at(1));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Down);
            CHECK(bar.lineEdit()->text() == expected.at(0));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Down);
            CHECK(bar.lineEdit()->text() == QStringLiteral("ab"));
            QTest::keyClick(bar.lineEdit(), Qt::Key_Down);
            CHECK(bar.lineEdit()->text() == QStringLiteral("ab"));
            for (int index = 0; index < 30; ++index)
            {
                QTest::keyClick(bar.lineEdit(), Qt::Key_Up);
            }
            CHECK(bar.lineEdit()->text() == expected.last());

            // 历史菜单：每条历史一项，样式显式不透明，点一项填入输入框。
            bar.rebuildHistoryMenu();
            QMenu* menu = bar.historyMenu();
            CHECK(menu->actions().size() == expected.size());
            for (int index = 0; index < expected.size() && index < menu->actions().size(); ++index)
            {
                CHECK(menu->actions().at(index)->text() == expected.at(index));
                CHECK(!menu->actions().at(index)->toolTip().isEmpty());
            }
            CHECK(!menu->testAttribute(Qt::WA_TranslucentBackground) && menu->toolTipsVisible());
            CHECK(MenuPaintsSurface(menu));
            ApplyTheme(true);
            CHECK(MenuPaintsSurface(menu));
            CHECK(menu->styleSheet().contains(KswordTheme::SurfaceColorHex()));
            ApplyTheme(false);
            CHECK(MenuPaintsSurface(menu));
            CHECK(menu->styleSheet().contains(KswordTheme::SurfaceColorHex()));
            CHECK(menu->styleSheet().contains(QStringLiteral("QMenu::item:selected")));
            CHECK(menu->styleSheet().contains(QStringLiteral("QMenu::item:disabled")));
            menu->actions().at(2)->trigger();
            CHECK(bar.lineEdit()->text() == expected.at(2));

            // 打开时重读：别处改了历史，打开本条后跟上。
            hs::SaveGotoHistory(QStringList{ QStringLiteral("77") });
            bar.open();
            CHECK(bar.history() == QStringList{ QStringLiteral("77") });

            // 空历史：菜单只有一个禁用的说明项。
            hs::SaveGotoHistory(QStringList());
            bar.reloadHistory();
            bar.rebuildHistoryMenu();
            CHECK(bar.history().isEmpty());
            CHECK(menu->actions().size() == 1 && !menu->actions().at(0)->isEnabled());
            QTest::keyClick(bar.lineEdit(), Qt::Key_Up);
            CHECK(bar.lineEdit()->text() == expected.at(2));

            // 纯函数：置顶去重、剪空白、空串忽略、上限、清理。
            CHECK(hs::PushHistory({ QStringLiteral("a"), QStringLiteral("b") }, QStringLiteral(" b ")) == (QStringList{ QStringLiteral("b"), QStringLiteral("a") }));
            CHECK(hs::PushHistory({ QStringLiteral("a") }, QStringLiteral("   ")) == QStringList{ QStringLiteral("a") });
            QStringList long12;
            for (int index = 0; index < 12; ++index)
            {
                long12.push_back(QString::number(index));
            }
            CHECK(hs::NormalizeHistory(long12).size() == 10);
            CHECK(hs::PushHistory(long12, QStringLiteral("x")).size() == 10 && hs::PushHistory(long12, QStringLiteral("x")).first() == QStringLiteral("x"));
            CHECK(hs::NormalizeHistory({ QStringLiteral(" a"), QString(), QStringLiteral("a "), QStringLiteral("b") })
                == (QStringList{ QStringLiteral("a"), QStringLiteral("b") }));

            // 读到非法类型的历史（不是字符串列表）：当作没有历史，不崩溃。
            {
                QSettings settings;
                settings.setValue(QString::fromLatin1(kHistoryKey), 42);
                settings.sync();
            }
            CHECK(hs::LoadGotoHistory().size() <= 1);
            {
                QSettings settings;
                settings.clear();
                settings.sync();
            }
        }
    }

    // 跳转条验证入口。
    void RunHexViewGotoTests()
    {
        TestResolveAbsolute();
        TestResolveOffsetAndRow();
        TestGotoBarInteraction();
        TestGotoHistory();
    }
}
