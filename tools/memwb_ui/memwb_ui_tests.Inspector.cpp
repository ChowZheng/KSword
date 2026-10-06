// memwb_ui_tests.Inspector.cpp
// 作用：HexInspectorPanel 的"显示侧"离屏验证——跟随插入点、字节序/指针宽度切换与持久化、
// 未读/未加载字节显示不可用（绝不补 0）、字符串行、悬停提示、布局。
// 所有手势都经 QTest 模拟的真实鼠标与键盘事件进入画布或面板，结果从面板的公开访问器读回。
// 编辑、复制、只读、右键菜单在 memwb_ui_tests.Inspector.Edit.cpp。

#include "memwb_ui_inspector.h"

#include <QApplication>
#include <QFontInfo>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSettings>
#include <QToolTip>

#include <algorithm>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexInspectorPanel;
        using ks::ui::HexInspectorRowData;
        using ks::ui::HexInspectorGlyphButton;
        using Pane = HexCanvas::ActivePane;
        using ksword::memwb::ByteOrder;

        // Expected：用 DecodeAll 对"数据里同一位置的字节"独立算出期望的 17 行。
        // 传入：数据、地址、字节序、指针宽度、命名器；传出：DecodeAll 的结果（窗口最多 16 字节，不超出数据末尾）。
        std::vector<ksword::memwb::DecodedRow> Expected(
            const QByteArray& data,
            std::uint64_t address,
            ByteOrder order,
            std::uint32_t pointerWidth,
            ksword::memwb::IPointerNamer* namer)
        {
            const int offset = static_cast<int>(address - kInspectorBase);
            const int count = std::min(16, static_cast<int>(data.size()) - offset);
            const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(data.constData()) + offset;
            return ksword::memwb::DecodeAll(bytes, static_cast<std::size_t>(count), order, pointerWidth, namer);
        }

        // AddressText：期望的顶栏地址文字。
        QString AddressText(std::uint64_t address)
        {
            return QStringLiteral("0x") + QString::number(address, 16).toUpper().rightJustified(8, QLatin1Char('0'));
        }

        // 跟随插入点：真实点击/按键移动插入点后，每一行都与独立计算的期望一致。
        void TestFollowsCaret()
        {
            ApplyTheme(false);
            FakeNamer reference;
            auto scene = MakeInspectorScene(MakeInspectorData(), false, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            CHECK(panel.rowView()->rowCount() == 17);

            // 逐个偏移点击画布单元格，行内容必须与 DecodeAll 的结果逐行一致；最后一个偏移靠近数据末尾，窗口不足 16 字节。
            const int offsets[] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0x11, 0x1FF8, 0x1FFF };
            for (const int offset : offsets)
            {
                const std::uint64_t address = kInspectorBase + static_cast<std::uint64_t>(offset);
                ClickAddress(canvas, address);
                CHECK_NOTE(canvas.caretAddress() == address, QString::number(offset, 16));
                CHECK_NOTE(panel.topBar()->addressText() == AddressText(address), panel.topBar()->addressText());
                const std::vector<ksword::memwb::DecodedRow> expected = Expected(scene->data, address, ByteOrder::Little, 8, &reference);
                CHECK(panel.rowView()->rowCount() == static_cast<int>(expected.size()));
                for (std::size_t index = 0; index < expected.size() && index < static_cast<std::size_t>(panel.rowView()->rowCount()); ++index)
                {
                    const HexInspectorRowData& row = panel.rowView()->rowAt(static_cast<int>(index));
                    const QString note = QStringLiteral("offset=%1 row=%2").arg(offset, 0, 16).arg(row.typeKey);
                    CHECK_NOTE(row.typeKey.toStdString() == expected[index].label, note);
                    CHECK_NOTE(row.available == expected[index].available, note);
                    CHECK_NOTE(row.valid == expected[index].valid, note);
                    CHECK_NOTE(row.valueCopy.toStdString() == (expected[index].available ? expected[index].copyText : std::string()), note);
                }
            }

            // 手写的期望值：整数、浮点、指针（带描述）、时间、GUID 都在固定偏移上。
            ClickAddress(canvas, kInspectorBase + 0x10);
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QStringLiteral("305419896"));
            CHECK(RowOf(panel, QStringLiteral("u32")).hexText == QStringLiteral("0x12345678"));
            CHECK(RowOf(panel, QStringLiteral("i8")).valueText == QStringLiteral("120"));
            CHECK(RowOf(panel, QStringLiteral("u16")).valueText == QStringLiteral("22136"));
            CHECK(RowOf(panel, QStringLiteral("u64")).valueText == QStringLiteral("10424652189165442680"));
            CHECK(RowOf(panel, QStringLiteral("u64")).hexText == QStringLiteral("0x90ABCDEF12345678"));

            // 内容不合法的行：0x90ABCDEF12345678 作为 FILETIME 超过 9999 年，显示"超出范围"，valid 为假，复制得到原始十进制数值。
            CHECK(!RowOf(panel, QStringLiteral("filetime")).valid);
            CHECK(RowOf(panel, QStringLiteral("filetime")).available);
            CHECK(RowOf(panel, QStringLiteral("filetime")).valueText == QStringLiteral("超出范围"));
            CHECK(RowOf(panel, QStringLiteral("filetime")).valueCopy == QStringLiteral("10424652189165442680"));
            CHECK(RowOf(panel, QStringLiteral("filetime")).toolTip.contains(QStringLiteral("超出可显示范围")));
            ClickAddress(canvas, kInspectorBase + 0x50);
            CHECK(RowOf(panel, QStringLiteral("f32")).valueText == QStringLiteral("1.5"));
            CHECK(RowOf(panel, QStringLiteral("f32")).hexText == QStringLiteral("0x3FC00000"));
            ClickAddress(canvas, kInspectorBase + 0x40);
            CHECK(RowOf(panel, QStringLiteral("ptr")).typeName == QStringLiteral("ptr64"));
            CHECK(RowOf(panel, QStringLiteral("ptr")).valueText == QStringLiteral("kernel32.dll+0x1A40"));
            CHECK(RowOf(panel, QStringLiteral("ptr")).hexText == QStringLiteral("0x00007FF612341A40"));
            ClickAddress(canvas, kInspectorBase + 0x60);
            CHECK(RowOf(panel, QStringLiteral("filetime")).valueText == QStringLiteral("2024-01-01 00:00:00.0000000 UTC"));
            ClickAddress(canvas, kInspectorBase + 0x70);
            CHECK(RowOf(panel, QStringLiteral("guid")).valueText == QStringLiteral("00112233-4455-6677-8899-aabbccddeeff"));

            // 指针描述里自带括号：值列是完整描述，十六进制列是纯地址，复制得到纯地址；有符号数全 FF 显示 -1 与满位模式。
            ClickAddress(canvas, kInspectorBase + 0xA0);
            CHECK(RowOf(panel, QStringLiteral("ptr")).valueText == QStringLiteral("ntdll!Foo (bar)"));
            CHECK(RowOf(panel, QStringLiteral("ptr")).hexText == QStringLiteral("0x00007FF600000123"));
            CHECK(RowOf(panel, QStringLiteral("ptr")).valueCopy == QStringLiteral("0x00007FF600000123"));
            ClickAddress(canvas, kInspectorBase + 0xB0);
            CHECK(RowOf(panel, QStringLiteral("i8")).valueText == QStringLiteral("-1"));
            CHECK(RowOf(panel, QStringLiteral("i8")).hexText == QStringLiteral("0xFF"));
            CHECK(RowOf(panel, QStringLiteral("i64")).valueText == QStringLiteral("-1"));
            CHECK(RowOf(panel, QStringLiteral("i64")).hexText == QStringLiteral("0xFFFFFFFFFFFFFFFF"));
            CHECK(RowOf(panel, QStringLiteral("u64")).valueText == QStringLiteral("18446744073709551615"));

            // 键盘：Right 移动插入点一个字节，面板跟着重读（0x11 处的 u8 是 0x56 = 86）。
            ClickAddress(canvas, kInspectorBase + 0x10);
            Key(canvas, Qt::Key_Right);
            CHECK(canvas.caretAddress() == kInspectorBase + 0x11);
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QStringLiteral("86"));
            CHECK(panel.topBar()->addressText() == AddressText(kInspectorBase + 0x11));

            // 选区：从 0x30 拖选到 0x20（Shift+点击），插入点在 0x20，面板从插入点解释，顶栏提示"已选 17 字节"。
            ClickAddress(canvas, kInspectorBase + 0x30);
            CHECK(panel.topBar()->hintText().isEmpty());
            ClickAddress(canvas, kInspectorBase + 0x20, Pane::Hex, Qt::ShiftModifier);
            CHECK(canvas.caretAddress() == kInspectorBase + 0x20);
            CHECK(panel.topBar()->addressText() == AddressText(kInspectorBase + 0x20));
            CHECK_NOTE(panel.topBar()->hintText() == QStringLiteral("已选 17 字节"), panel.topBar()->hintText());
            CHECK(RowOf(panel, QStringLiteral("ascii")).valueText.startsWith(QStringLiteral("Hello")));

            // 没有关联画布的面板：所有行不可用，地址是破折号。
            HexInspectorPanel orphan;
            orphan.setSettingsFile(scene->settingsPath());
            CHECK(orphan.rowView()->rowCount() == 17);
            bool anyAvailable = false;
            for (int index = 0; index < orphan.rowView()->rowCount(); ++index)
            {
                anyAvailable = anyAvailable || orphan.rowView()->rowAt(index).available;
            }
            CHECK(!anyAvailable);
            CHECK(orphan.topBar()->addressText() == QStringLiteral("—"));
        }

        // 字节序与指针宽度：真实点击图标按钮；u32 值互换；GUID/字符串不随字节序变；选择持久化。
        void TestToggles()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), false, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            ClickAddress(canvas, kInspectorBase + 0x10);
            CHECK(panel.byteOrder() == ByteOrder::Little);
            CHECK(panel.byteOrderButton(ByteOrder::Little)->isChecked());
            CHECK(!panel.byteOrderButton(ByteOrder::Big)->isChecked());
            CHECK(panel.pointerWidthButton(8)->isChecked());
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QStringLiteral("305419896"));

            // 记下不应随字节序变化的三行（GUID、ASCII、UTF-16）在 0x70 / 0x20 / 0x80 处的文字。
            const auto snapshotText = [&](std::uint64_t offset) {
                ClickAddress(canvas, kInspectorBase + offset);
                return RowOf(panel, QStringLiteral("guid")).valueText
                    + QLatin1Char('|') + RowOf(panel, QStringLiteral("ascii")).valueText
                    + QLatin1Char('|') + RowOf(panel, QStringLiteral("utf16")).valueText;
            };
            const QString guidLittle = snapshotText(0x70);
            const QString asciiLittle = snapshotText(0x20);
            const QString utf16Little = snapshotText(0x80);
            ClickAddress(canvas, kInspectorBase + 0x10);

            // 切到大端：u32 = 0x78563412，u16 = 0x7856，i8 不变，按钮勾选互斥。
            QSignalSpy orderSpy(&panel, &HexInspectorPanel::byteOrderChanged);
            QTest::mouseClick(panel.byteOrderButton(ByteOrder::Big), Qt::LeftButton);
            CHECK(panel.byteOrder() == ByteOrder::Big);
            CHECK(panel.byteOrderButton(ByteOrder::Big)->isChecked());
            CHECK(!panel.byteOrderButton(ByteOrder::Little)->isChecked());
            CHECK(orderSpy.count() == 1 && orderSpy.at(0).at(0).toInt() == 1);
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QStringLiteral("2018915346"));
            CHECK(RowOf(panel, QStringLiteral("u32")).hexText == QStringLiteral("0x78563412"));
            CHECK(RowOf(panel, QStringLiteral("u16")).valueText == QStringLiteral("30806"));
            CHECK(RowOf(panel, QStringLiteral("i8")).valueText == QStringLiteral("120"));
            CHECK(snapshotText(0x70) == guidLittle);
            CHECK(snapshotText(0x20) == asciiLittle);
            CHECK(snapshotText(0x80) == utf16Little);
            ClickAddress(canvas, kInspectorBase + 0x10);

            // 指针宽度：切到 4 字节，ptr 行改名为 ptr32，十六进制只有 8 位。
            QSignalSpy widthSpy(&panel, &HexInspectorPanel::pointerWidthChanged);
            QTest::mouseClick(panel.pointerWidthButton(4), Qt::LeftButton);
            CHECK(panel.pointerWidthBytes() == 4U);
            CHECK(panel.pointerWidthButton(4)->isChecked() && !panel.pointerWidthButton(8)->isChecked());
            CHECK(widthSpy.count() == 1 && widthSpy.at(0).at(0).toUInt() == 4U);
            CHECK(RowOf(panel, QStringLiteral("ptr")).typeName == QStringLiteral("ptr32"));
            CHECK(RowOf(panel, QStringLiteral("ptr")).hexText == QStringLiteral("0x78563412"));

            // 重复点击已选中的按钮：不发信号，状态不变。
            QTest::mouseClick(panel.pointerWidthButton(4), Qt::LeftButton);
            CHECK(widthSpy.count() == 1);
            CHECK(panel.pointerWidthButton(4)->isChecked());

            // 持久化：INI 里是 1 / 4；新建的面板读到同样的选择，按钮状态一致。
            {
                QSettings ini(scene->settingsPath(), QSettings::IniFormat);
                CHECK(ini.value(QStringLiteral("memwb/inspector/byteOrder")).toInt() == 1);
                CHECK(ini.value(QStringLiteral("memwb/inspector/pointerWidth")).toInt() == 4);
            }
            HexInspectorPanel reloaded;
            reloaded.setSettingsFile(scene->settingsPath());
            CHECK(reloaded.byteOrder() == ByteOrder::Big);
            CHECK(reloaded.pointerWidthBytes() == 4U);
            CHECK(reloaded.byteOrderButton(ByteOrder::Big)->isChecked());
            CHECK(reloaded.pointerWidthButton(4)->isChecked());

            // 切回小端、8 字节，INI 同步回写。
            QTest::mouseClick(panel.byteOrderButton(ByteOrder::Little), Qt::LeftButton);
            QTest::mouseClick(panel.pointerWidthButton(8), Qt::LeftButton);
            QSettings back(scene->settingsPath(), QSettings::IniFormat);
            CHECK(back.value(QStringLiteral("memwb/inspector/byteOrder")).toInt() == 0);
            CHECK(back.value(QStringLiteral("memwb/inspector/pointerWidth")).toInt() == 8);
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QStringLiteral("305419896"));
        }

        // 设置读取失败/非法值：一律退回默认（小端、8 字节），不崩溃。
        void TestSettingsFallback()
        {
            ApplyTheme(false);
            QTemporaryDir dir;
            const QString path = dir.filePath(QStringLiteral("bad.ini"));

            // 非法值：字节序写成乱码、指针宽度写成 7，都退回默认。
            {
                QSettings ini(path, QSettings::IniFormat);
                ini.setValue(QStringLiteral("memwb/inspector/byteOrder"), QStringLiteral("abc"));
                ini.setValue(QStringLiteral("memwb/inspector/pointerWidth"), 7);
                ini.sync();
            }
            HexInspectorPanel panel;
            panel.setSettingsFile(path);
            CHECK(panel.byteOrder() == ByteOrder::Little);
            CHECK(panel.pointerWidthBytes() == 8U);
            CHECK(panel.byteOrderButton(ByteOrder::Little)->isChecked());
            CHECK(panel.pointerWidthButton(8)->isChecked());

            // 部分合法：字节序超出范围（5）退回小端，指针宽度 4 合法被采用。
            {
                QSettings ini(path, QSettings::IniFormat);
                ini.setValue(QStringLiteral("memwb/inspector/byteOrder"), 5);
                ini.setValue(QStringLiteral("memwb/inspector/pointerWidth"), 4);
                ini.sync();
            }
            panel.setSettingsFile(path);
            CHECK(panel.byteOrder() == ByteOrder::Little);
            CHECK(panel.pointerWidthBytes() == 4U);

            // 文件不存在：默认值。
            HexInspectorPanel missing;
            missing.setSettingsFile(dir.filePath(QStringLiteral("not-there.ini")));
            CHECK(missing.byteOrder() == ByteOrder::Little && missing.pointerWidthBytes() == 8U);

            // 设置位置不可用（把"文件"当目录用）：读取失败，退回默认，写入不崩溃。
            HexInspectorPanel broken;
            broken.setSettingsFile(path + QStringLiteral("/sub/x.ini"));
            CHECK(broken.byteOrder() == ByteOrder::Little && broken.pointerWidthBytes() == 8U);
            broken.setByteOrder(ByteOrder::Big);
            CHECK(broken.byteOrder() == ByteOrder::Big);

            // 非法的指针宽度参数被忽略。
            broken.setPointerWidthBytes(3);
            CHECK(broken.pointerWidthBytes() == 8U);
        }

        // 未读/未加载/不可读/地址空间末尾：对应行不可用，绝不补 0；页异步到达后画布发 contentChanged，面板据此重读。
        void TestUnavailable()
        {
            ApplyTheme(false);

            // 异步场景：一页都没到，插入点在地址空间起点，所有行都不可用，u8 不是"0"。
            auto scene = MakeAsyncInspectorScene(false, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            canvas.setCaretAddress(kInspectorBase + 0x10);
            int available = 0;
            for (int index = 0; index < panel.rowView()->rowCount(); ++index)
            {
                available += panel.rowView()->rowAt(index).available ? 1 : 0;
            }
            CHECK(available == 0);
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QStringLiteral("不可用"));
            CHECK(RowOf(panel, QStringLiteral("u8")).valueCopy.isEmpty());
            CHECK(RowOf(panel, QStringLiteral("u32")).toolTip.contains(QStringLiteral("尚未加载")));

            // 页到达：数据里 0x12..0x13 两个字节标记为没读到。画布排队发 contentChanged，面板订阅它重读
            // （不再有定时器兜底）。回填后、事件循环处理之前信号还没发出，处理一轮事件后恰好发出一次。
            QSignalSpy contentSpy(&canvas, &HexCanvas::contentChanged);
            QByteArray page = MakePattern(4096);
            QByteArray mask(4096, '\x01');
            mask[0x12] = '\0';
            mask[0x13] = '\0';
            canvas.deliverPage(kInspectorBase, page, mask, canvas.sourceRevision());
            CHECK(contentSpy.count() == 0);
            QApplication::processEvents();
            CHECK(contentSpy.count() == 1);
            CHECK(RowOf(panel, QStringLiteral("u8")).available);
            CHECK(RowOf(panel, QStringLiteral("i16")).available);
            CHECK(!RowOf(panel, QStringLiteral("i32")).available);
            CHECK(RowOf(panel, QStringLiteral("i32")).valueText == QStringLiteral("不可用"));
            CHECK_NOTE(RowOf(panel, QStringLiteral("i32")).toolTip.contains(QStringLiteral("需要 4")), RowOf(panel, QStringLiteral("i32")).toolTip);
            CHECK(RowOf(panel, QStringLiteral("i32")).toolTip.contains(QStringLiteral("只有 2")));
            CHECK(RowOf(panel, QStringLiteral("i32")).toolTip.contains(QStringLiteral("不可读")));
            CHECK(!RowOf(panel, QStringLiteral("f64")).available);
            CHECK(!RowOf(panel, QStringLiteral("guid")).available);

            // 可用的两行取到的就是页里的真实字节，不是 0。
            const int expectedU8 = static_cast<unsigned char>(page.at(0x10));
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QString::number(expectedU8));

            // 整页不可读：插入点移到第二页，所有行不可用，提示"不可读"。
            ks::ui::HexFetchRange unreadable;
            unreadable.firstPageStart = kInspectorBase + 0x1000;
            unreadable.pageCount = 1;
            canvas.deliverUnreadable(unreadable, canvas.sourceRevision());
            canvas.setCaretAddress(kInspectorBase + 0x1000);
            CHECK(!RowOf(panel, QStringLiteral("u8")).available);
            CHECK(RowOf(panel, QStringLiteral("u8")).toolTip.contains(QStringLiteral("不可读")));

            // 地址空间末尾：静态场景最后 3 个字节，i16 可用、i32 不可用，提示"已到地址空间末尾"。
            auto tail = MakeInspectorScene(MakeInspectorData(), false, true, QSize(1100, 560), 400);
            ClickAddress(*tail->canvas, kInspectorBase + 0x1FFD);
            CHECK(RowOf(*tail->panel, QStringLiteral("i16")).available);
            CHECK(!RowOf(*tail->panel, QStringLiteral("i32")).available);
            CHECK(RowOf(*tail->panel, QStringLiteral("i32")).toolTip.contains(QStringLiteral("已到地址空间末尾")));
            const int tailU8 = static_cast<unsigned char>(tail->data.at(0x1FFD));
            CHECK(RowOf(*tail->panel, QStringLiteral("u8")).valueText == QString::number(tailU8));
        }

        // 字符串行：等宽字体、16 字节无结束符时的截断提示、有结束符不加省略号、UTF-16、提示按纯文本显示。
        void TestStringsAndTooltips()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), false, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            CHECK(QFontInfo(panel.rowView()->font()).fixedPitch());

            // 16 个可见字符、没有结束符：值末尾"…"，提示注明只解析前 16 字节；复制得到的是不含省略号的字符串。
            ClickAddress(canvas, kInspectorBase + 0x20);
            const HexInspectorRowData ascii = RowOf(panel, QStringLiteral("ascii"));
            CHECK_NOTE(ascii.valueText == QStringLiteral("Hello, KSword!!!…"), ascii.valueText);
            CHECK(ascii.valueCopy == QStringLiteral("Hello, KSword!!!"));
            CHECK(ascii.toolTip.contains(QStringLiteral("仅解析前 16 字节")));
            CHECK_NOTE(ascii.hexText == QStringLiteral("48 65 6C 6C 6F 2C 20 4B …"), ascii.hexText);

            // 有结束符：没有省略号。
            ClickAddress(canvas, kInspectorBase + 0x30);
            CHECK(RowOf(panel, QStringLiteral("ascii")).valueText == QStringLiteral("Hi"));
            CHECK(!RowOf(panel, QStringLiteral("ascii")).toolTip.contains(QStringLiteral("仅解析前 16 字节")));
            CHECK(RowOf(panel, QStringLiteral("ascii")).hexText == QStringLiteral("48 69"));

            // 起点处不是字符（0x32 是结束符 NUL）：字节够但没有内容，值列写"无可显示的字符"而不是"不可用"，不可复制，提示说明原因。
            ClickAddress(canvas, kInspectorBase + 0x32);
            const HexInspectorRowData noText = RowOf(panel, QStringLiteral("ascii"));
            CHECK(!noText.available);
            CHECK(noText.valueText == QStringLiteral("无可显示的字符"));
            CHECK(noText.valueCopy.isEmpty());
            CHECK(noText.toolTip.contains(QStringLiteral("没有可显示的字符")));
            CHECK(RowOf(panel, QStringLiteral("u8")).available);

            // UTF-16：A B 字 NUL，输出 UTF-8 转成的 QString。
            ClickAddress(canvas, kInspectorBase + 0x80);
            CHECK(RowOf(panel, QStringLiteral("utf16")).valueText == QStringLiteral("AB") + QChar(0x5B57));

            // 含 HTML 标记的字符串：行提示里是原样文本，真正显示时被转义，不会被当成富文本。
            ClickAddress(canvas, kInspectorBase + 0x90);
            CHECK(RowOf(panel, QStringLiteral("ascii")).valueText == QStringLiteral("<b>x</b>"));
            const int row = RowIndexOf(panel, QStringLiteral("ascii"));
            const QString tip = panel.rowView()->toolTipAt(panel.rowView()->valueCellRect(row).center());
            CHECK(tip.contains(QStringLiteral("<b>x</b>")));
            const QString rich = ks::ui::HexInspectorRichToolTip(tip);
            CHECK(!rich.contains(QStringLiteral("<b>")));
            CHECK(rich.contains(QStringLiteral("&lt;b&gt;x&lt;/b&gt;")));

            // 悬停提示：列标题、复制图标、整行都有说明；四个图标按钮都有提示。
            ks::ui::HexInspectorRowView* rows = panel.rowView();
            CHECK(rows->toolTipAt(QPoint(rows->valueCellRect(0).left() + 4, 3)).contains(QStringLiteral("按该类型解释出的值")));
            CHECK(rows->toolTipAt(QPoint(rows->hexCellRect(0).left() + 4, 3)).contains(QStringLiteral("位模式")));
            CHECK(rows->toolTipAt(QPoint(rows->copyGlyphRect(0).center())) == QStringLiteral("复制该行的值"));
            CHECK(rows->toolTipAt(rows->rowRect(3).center()) == rows->rowAt(3).toolTip);
            CHECK(!panel.byteOrderButton(ByteOrder::Little)->toolTip().isEmpty());
            CHECK(!panel.byteOrderButton(ByteOrder::Big)->toolTip().isEmpty());
            CHECK(!panel.pointerWidthButton(4)->toolTip().isEmpty());
            CHECK(!panel.pointerWidthButton(8)->toolTip().isEmpty());
        }
    }

    // 本文件全部测试的入口。
    void RunInspectorViewTests()
    {
        TestFollowsCaret();
        TestToggles();
        TestSettingsFallback();
        TestUnavailable();
        TestStringsAndTooltips();
        ApplyTheme(false);
    }
}
