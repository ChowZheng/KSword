// memwb_ui_tests.Inspector.Edit.cpp
// 作用：HexInspectorPanel 的"编辑与复制侧"离屏验证——行内编辑（整数/浮点/指针，小端与大端）、
// 暂存进叠加层、非法输入被拒且无暂存、Esc/失焦/插入点移动取消、只读下禁用并说明原因、
// 复制值/十六进制/整行、悬停复制图标、右键菜单（不透明样式、禁用项提示）。
// 所有手势都经 QTest 模拟的真实鼠标与键盘事件进入面板，暂存结果从 MemoryDiffOverlay 的公开接口核对。

#include "memwb_ui_inspector.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMenu>
#include <QSignalSpy>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexInspectorPanel;
        using ks::ui::HexInspectorRowView;
        using ks::ui::HexInspectorStatusBar;
        using ksword::memwb::ByteOrder;
        using Bytes = std::vector<std::uint8_t>;

        // SetClip / Clip：写与读剪贴板文本。
        void SetClip(const QString& text)
        {
            QGuiApplication::clipboard()->setText(text);
        }

        QString Clip()
        {
            return QGuiApplication::clipboard()->text();
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

        // OnlyBlock：叠加层里恰好一个补丁块时返回它的副本，否则返回空块（地址 0、无字节）。
        ksword::memwb::DiffBlock OnlyBlock(const ksword::memwb::MemoryDiffOverlay& overlay)
        {
            const std::vector<ksword::memwb::DiffBlock> blocks = overlay.DiffBlocks();
            return blocks.size() == 1 ? blocks[0] : ksword::memwb::DiffBlock();
        }

        // Effective：叠加后的字节视图。暂存会丢掉"与原值相同"的字节（空操作不留痕），
        // 所以判断"写进去的是什么"要看叠加后的视图，而不是补丁块的 after。
        Bytes Effective(const ksword::memwb::MemoryDiffOverlay& overlay, std::uint64_t address, std::uint64_t length)
        {
            return overlay.Materialize(address, length).bytes;
        }

        // StatusText：状态条当前文字。
        QString StatusText(const HexInspectorPanel& panel)
        {
            return panel.statusBar()->messageText();
        }

        // 整数编辑：双击 -> 预填 -> 键入 -> Enter -> 叠加层里出现正确暂存；小端与大端、Enter/F2 入口。
        void TestIntegerEdit()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&panel, &HexInspectorPanel::editRejected);
            ClickAddress(canvas, kInspectorBase + 0x10);

            // 双击 u32 行：编辑器出现，预填该行的复制值。
            DoubleClickRow(panel, QStringLiteral("u32"));
            CHECK(panel.rowView()->isEditing());
            CHECK(panel.rowView()->editingRow() == RowIndexOf(panel, QStringLiteral("u32")));
            CHECK(panel.rowView()->editor() != nullptr && panel.rowView()->editor()->text() == QStringLiteral("305419896"));

            // 键入十六进制并回车：暂存 44 33 22 11（小端），before 是原字节 78 56 34 12。
            TypeIntoEditor(panel, QStringLiteral("0x11223344"));
            PressInEditor(panel, Qt::Key_Return);
            const ksword::memwb::DiffBlock block = OnlyBlock(scene->overlay);
            CHECK(block.address == kInspectorBase + 0x10);
            CHECK(block.after == Bytes({ 0x44, 0x33, 0x22, 0x11 }));
            CHECK(block.before == Bytes({ 0x78, 0x56, 0x34, 0x12 }));
            CHECK(staged.count() == 1);
            CHECK(staged.count() == 1 && staged.at(0).at(0).toULongLong() == kInspectorBase + 0x10 && staged.at(0).at(1).toULongLong() == 4);
            CHECK(rejected.isEmpty());
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(canvas.cellStateAt(kInspectorBase + 0x10).change == HexCanvas::ChangeKind::Pending);
            CHECK(canvas.cellStateAt(kInspectorBase + 0x13).value == 0x11);
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QStringLiteral("287454020"));
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Info);
            CHECK_NOTE(StatusText(panel).contains(QStringLiteral("已暂存")), StatusText(panel));

            // 大端：Enter 键入口（选中行 + Enter），0xAABBCCDD 按大端写成 AA BB CC DD，与已有暂存合并成一块。
            panel.setByteOrder(ByteOrder::Big);
            panel.rowView()->setCurrentRow(RowIndexOf(panel, QStringLiteral("u32")));
            QTest::keyClick(panel.rowView(), Qt::Key_Return);
            CHECK(panel.rowView()->isEditing());
            TypeIntoEditor(panel, QStringLiteral("0xAABBCCDD"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(OnlyBlock(scene->overlay).after == Bytes({ 0xAA, 0xBB, 0xCC, 0xDD }));
            CHECK(staged.count() == 2);
            FlushDeferred();

            // 无符号 8 位：F2 入口，十进制 255 写成 FF，只改一个字节（与前一块相邻合并，块长仍为 4 但首字节变 FF）。
            panel.setByteOrder(ByteOrder::Little);
            panel.rowView()->setCurrentRow(RowIndexOf(panel, QStringLiteral("u8")));
            QTest::keyClick(panel.rowView(), Qt::Key_F2);
            CHECK(panel.rowView()->isEditing());
            TypeIntoEditor(panel, QStringLiteral("255"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(OnlyBlock(scene->overlay).after == Bytes({ 0xFF, 0xBB, 0xCC, 0xDD }));
            FlushDeferred();

            // 有符号负数：i16 键入 -2 写成 FE FF（补码小端），写在插入点 0x10，覆盖前两个字节。
            DoubleClickRow(panel, QStringLiteral("i16"));
            TypeIntoEditor(panel, QStringLiteral("-2"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(OnlyBlock(scene->overlay).after == Bytes({ 0xFE, 0xFF, 0xCC, 0xDD }));
            FlushDeferred();
        }

        // 浮点与指针编辑：f32 写出 IEEE 位模式；ptr 随指针宽度写 8 或 4 个字节。
        void TestFloatAndPointerEdit()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;

            // f32：2.5 写成 00 00 20 40。
            ClickAddress(canvas, kInspectorBase + 0x50);
            DoubleClickRow(panel, QStringLiteral("f32"));
            CHECK(panel.rowView()->editor() != nullptr && panel.rowView()->editor()->text() == QStringLiteral("1.5"));
            TypeIntoEditor(panel, QStringLiteral("2.5"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(Effective(scene->overlay, kInspectorBase + 0x50, 4) == Bytes({ 0x00, 0x00, 0x20, 0x40 }));
            CHECK(OnlyBlock(scene->overlay).address == kInspectorBase + 0x52);
            FlushDeferred();
            CHECK(RowOf(panel, QStringLiteral("f32")).valueText == QStringLiteral("2.5"));

            // 解释器显示的文字可以原样键回：NaN 与 -Inf 都被接受。
            DoubleClickRow(panel, QStringLiteral("f32"));
            TypeIntoEditor(panel, QStringLiteral("-Inf"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(Effective(scene->overlay, kInspectorBase + 0x50, 4) == Bytes({ 0x00, 0x00, 0x80, 0xFF }));
            FlushDeferred();
            scene->overlay.DiscardAll();
            canvas.viewport()->update();

            // ptr（8 字节）：0x1000 写成 00 10 00 00 00 00 00 00。
            ClickAddress(canvas, kInspectorBase + 0x40);
            DoubleClickRow(panel, QStringLiteral("ptr"));
            CHECK(panel.rowView()->editor() != nullptr && panel.rowView()->editor()->text() == QStringLiteral("0x00007FF612341A40"));
            TypeIntoEditor(panel, QStringLiteral("0x1000"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(Effective(scene->overlay, kInspectorBase + 0x40, 8) == Bytes({ 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }));
            FlushDeferred();
            scene->overlay.DiscardAll();

            // ptr（4 字节）：只写 4 个字节。
            panel.setPointerWidthBytes(4);
            ClickAddress(canvas, kInspectorBase + 0x40);
            DoubleClickRow(panel, QStringLiteral("ptr"));
            TypeIntoEditor(panel, QStringLiteral("0x1000"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(Effective(scene->overlay, kInspectorBase + 0x40, 8) == Bytes({ 0x00, 0x10, 0x00, 0x00, 0xF6, 0x7F, 0x00, 0x00 }));
            FlushDeferred();
        }

        // 非法输入被拒、无暂存、编辑器保持打开并显示原因；改正后可以提交；Esc/失焦/移动插入点取消。
        void TestRejectAndCancel()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            QSignalSpy staged(&canvas, &HexCanvas::editStaged);
            QSignalSpy rejected(&panel, &HexInspectorPanel::editRejected);
            ClickAddress(canvas, kInspectorBase + 0x10);

            // 语法错误：编辑器还在，状态条是错误并说明该键入什么，边框变红，没有任何暂存。
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("abc"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(panel.rowView()->isEditing());
            CHECK(!scene->overlay.HasPendingPatches());
            CHECK(staged.isEmpty());
            CHECK(rejected.count() == 1);
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Error);
            CHECK_NOTE(StatusText(panel).contains(QStringLiteral("不是合法的整数")), StatusText(panel));
            const QString errorHex = KswordTheme::ThemeColorName(
                KswordTheme::EnsureTextContrast(KswordTheme::ErrorColor(), KswordTheme::SurfaceAltColor()));
            CHECK(panel.rowView()->editor()->styleSheet().contains(errorHex));

            // 继续键入：边框恢复强调色（不再含错误色），状态条里的原因保留到下一次提交。
            QTest::keyClicks(panel.rowView()->editor(), QStringLiteral("1"));
            CHECK(!panel.rowView()->editor()->styleSheet().contains(errorHex));

            // 其它拒绝原因：范围、溢出、负数、浮点语法、浮点范围。每一次都保持编辑器、无暂存。
            struct Case
            {
                const char* typeKey;    // 行类型键
                const char* text;       // 键入文本
                const char* expected;   // 状态条应包含的原因片段
            };
            const Case cases[] = {
                { "i8", "300", "超出 int8 的取值范围" },
                { "u8", "-1", "超出 uint8 的取值范围" },
                { "u8", "99999999999999999999", "超过 64 位" },
                { "u16", "", "不是合法的整数" },
                { "f32", "1.2.3", "不是合法的浮点数" },
                { "f32", "3.5e38", "超出 float 的取值范围" },
                { "ptr", "zzz", "不是合法的指针值" },
            };
            for (const Case& item : cases)
            {
                panel.rowView()->endEdit();
                FlushDeferred();
                DoubleClickRow(panel, QString::fromLatin1(item.typeKey));
                TypeIntoEditor(panel, QString::fromUtf8(item.text));
                if (QString::fromUtf8(item.text).isEmpty())
                {
                    panel.rowView()->editor()->clear();
                }
                PressInEditor(panel, Qt::Key_Return);
                const QString note = QStringLiteral("%1 %2 -> %3").arg(QLatin1String(item.typeKey), QString::fromUtf8(item.text), StatusText(panel));
                CHECK_NOTE(panel.rowView()->isEditing(), note);
                CHECK_NOTE(StatusText(panel).contains(QString::fromUtf8(item.expected)), note);
                CHECK_NOTE(!scene->overlay.HasPendingPatches(), note);
            }
            CHECK(staged.isEmpty());

            // 在错误之后改成合法值：这一次暂存成功，编辑器关闭。
            panel.rowView()->endEdit();
            FlushDeferred();
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("abc"));
            PressInEditor(panel, Qt::Key_Return);
            TypeIntoEditor(panel, QStringLiteral("7"));
            PressInEditor(panel, Qt::Key_Return);
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(OnlyBlock(scene->overlay).after == Bytes({ 0x07, 0x00, 0x00, 0x00 }));
            CHECK(staged.count() == 1);
            scene->overlay.DiscardAll();
            canvas.viewport()->update();

            // Esc：编辑器关闭，没有暂存，错误消息被清掉。
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("xyz"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Error);
            PressInEditor(panel, Qt::Key_Escape);
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(!scene->overlay.HasPendingPatches());
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Hint);

            // 失焦：点到别处（鼠标焦点原因）取消；窗口失活不取消。
            DoubleClickRow(panel, QStringLiteral("u32"));
            QLineEdit* editor = panel.rowView()->editor();
            QFocusEvent deactivate(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
            QApplication::sendEvent(editor, &deactivate);
            CHECK(panel.rowView()->isEditing());
            QFocusEvent clickAway(QEvent::FocusOut, Qt::MouseFocusReason);
            QApplication::sendEvent(editor, &clickAway);
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(!scene->overlay.HasPendingPatches());

            // 移动插入点：真实点击画布其它字节，正在进行的编辑被放弃，旧消息被清掉。
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("5"));
            ClickAddress(canvas, kInspectorBase + 0x30);
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(!scene->overlay.HasPendingPatches());
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Hint);

            // 同样的事，但不经过鼠标焦点：用键盘与程序接口移动插入点，编辑器不会因失焦而关闭，
            // 必须是面板自己因为插入点移动而放弃编辑（与点击路径互相独立）。
            ClickAddress(canvas, kInspectorBase + 0x10);
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("5"));
            CHECK(panel.rowView()->isEditing());
            Key(canvas, Qt::Key_Right);
            CHECK(canvas.caretAddress() == kInspectorBase + 0x11);
            CHECK(!panel.rowView()->isEditing());
            CHECK(!scene->overlay.HasPendingPatches());
            DoubleClickRow(panel, QStringLiteral("u32"));
            CHECK(panel.rowView()->isEditing());
            canvas.setCaretAddress(kInspectorBase + 0x40);
            CHECK(!panel.rowView()->isEditing());
            FlushDeferred();

            // 与当前值相同的编辑：不产生暂存，状态条说明"值没有变化"。
            ClickAddress(canvas, kInspectorBase + 0x10);
            DoubleClickRow(panel, QStringLiteral("u8"));
            TypeIntoEditor(panel, QStringLiteral("120"));
            PressInEditor(panel, Qt::Key_Return);
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(!scene->overlay.HasPendingPatches());
            CHECK_NOTE(StatusText(panel).contains(QStringLiteral("值没有变化")), StatusText(panel));

            // Stage 自己的拒绝：把叠加层基线换成只覆盖前 0x20 字节的小窗口，在窗口外编辑被 Stage 拒绝，原因显示、编辑器保持。
            const std::vector<std::uint8_t> small(0x20, 1);
            scene->overlay.LoadBaseline(std::string("small"), kInspectorBase, small, small);
            ClickAddress(canvas, kInspectorBase + 0x100);
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("9"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(panel.rowView()->isEditing());
            CHECK_NOTE(StatusText(panel).contains(QStringLiteral("超出当前已读取的数据窗口")), StatusText(panel));
            CHECK(!scene->overlay.HasPendingPatches());
        }

        // 编辑期间目标字节变成"尚未加载"（画布 refresh 换了来源代次）：提交时的预检必须拒绝，不能在看不到值的字节上暂存。
        void TestUnloadedAtCommit()
        {
            ApplyTheme(false);
            auto scene = MakeAsyncInspectorScene(true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;

            // 一页数据全部读到，叠加层基线与之一致。
            const QByteArray page = MakePattern(4096);
            const QByteArray mask(4096, '');
            canvas.deliverPage(kInspectorBase, page, mask, canvas.sourceRevision());
            const std::vector<std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(page.constData()),
                reinterpret_cast<const std::uint8_t*>(page.constData()) + page.size());
            scene->overlay.LoadBaseline(std::string("async-test"), kInspectorBase, bytes, std::vector<std::uint8_t>(bytes.size(), 1));
            canvas.setCaretAddress(kInspectorBase + 0x10);
            panel.refreshFromCanvas();
            CHECK(RowOf(panel, QStringLiteral("u32")).available);

            // 开始编辑并键入合法值，然后画布 refresh：缓存清空，页回到"未加载"，提供者不再回填。
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("7"));
            CHECK(panel.rowView()->isEditing());
            canvas.refresh();
            CHECK(!canvas.cellStateAt(kInspectorBase + 0x10).hasValue);
            PressInEditor(panel, Qt::Key_Return);
            CHECK(panel.rowView()->isEditing());
            CHECK_NOTE(StatusText(panel).contains(QStringLiteral("尚未加载")), StatusText(panel));
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Error);
            CHECK(!scene->overlay.HasPendingPatches());
            panel.rowView()->endEdit();
            FlushDeferred();
        }

        // 只读：画布只读、没有叠加层、类型只读、字节不足，四种情况都不打开编辑器并说明原因；菜单项置灰且提示里有原因。
        void TestReadOnly()
        {
            ApplyTheme(false);

            // 画布只读（有叠加层）。
            {
                auto scene = MakeInspectorScene(MakeInspectorData(), false, true, QSize(1100, 560), 400);
                HexInspectorPanel& panel = *scene->panel;
                QSignalSpy rejected(&panel, &HexInspectorPanel::editRejected);
                ClickAddress(*scene->canvas, kInspectorBase + 0x10);
                CHECK(panel.editBlockedReason(RowIndexOf(panel, QStringLiteral("u32"))) == QStringLiteral("当前为只读视图，不能编辑"));
                CHECK(!RowOf(panel, QStringLiteral("u32")).editEnabled);
                DoubleClickRow(panel, QStringLiteral("u32"));
                CHECK(!panel.rowView()->isEditing());
                CHECK(rejected.count() == 1);
                CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Error);
                CHECK_NOTE(StatusText(panel).contains(QStringLiteral("只读视图")), StatusText(panel));
                CHECK(!scene->overlay.HasPendingPatches());

                // Enter 入口同样被拒；beginEditRow 返回 false。
                panel.rowView()->setCurrentRow(RowIndexOf(panel, QStringLiteral("u32")));
                QTest::keyClick(panel.rowView(), Qt::Key_Return);
                CHECK(!panel.rowView()->isEditing());
                CHECK(rejected.count() == 2);
                CHECK(!panel.beginEditRow(RowIndexOf(panel, QStringLiteral("u8"))));

                // 菜单里的"编辑此值"置灰，提示写明原因；行提示里也说明只读视图。
                QMenu* menu = panel.buildRowMenu(RowIndexOf(panel, QStringLiteral("u32")));
                QAction* edit = menu != nullptr ? FindAction(menu, QStringLiteral("编辑此值")) : nullptr;
                CHECK(edit != nullptr && !edit->isEnabled());
                CHECK(edit != nullptr && edit->toolTip().contains(QStringLiteral("只读视图")));
                CHECK(RowOf(panel, QStringLiteral("u32")).toolTip.contains(QStringLiteral("只读视图")));
                delete menu;
            }

            // 可编辑但没有叠加层：同样只读。
            {
                auto scene = MakeInspectorScene(MakeInspectorData(), true, false, QSize(1100, 560), 400);
                HexInspectorPanel& panel = *scene->panel;
                ClickAddress(*scene->canvas, kInspectorBase + 0x10);
                CHECK(panel.editBlockedReason(RowIndexOf(panel, QStringLiteral("u32"))) == QStringLiteral("当前为只读视图，不能编辑"));
                DoubleClickRow(panel, QStringLiteral("u32"));
                CHECK(!panel.rowView()->isEditing());
            }

            // 可编辑画布：只读类型（FILETIME）与字节不足的行同样不能编辑，原因各不相同。
            {
                auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
                HexInspectorPanel& panel = *scene->panel;
                ClickAddress(*scene->canvas, kInspectorBase + 0x60);
                CHECK(panel.editBlockedReason(RowIndexOf(panel, QStringLiteral("filetime"))).contains(QStringLiteral("只读类型")));
                CHECK(RowOf(panel, QStringLiteral("u32")).editEnabled && RowOf(panel, QStringLiteral("f64")).editEnabled);
                CHECK(!RowOf(panel, QStringLiteral("filetime")).editEnabled && !RowOf(panel, QStringLiteral("guid")).editEnabled);
                DoubleClickRow(panel, QStringLiteral("filetime"));
                CHECK(!panel.rowView()->isEditing());
                CHECK(StatusText(panel).contains(QStringLiteral("只读类型")));
                ClickAddress(*scene->canvas, kInspectorBase + 0x1FFD);
                CHECK(panel.editBlockedReason(RowIndexOf(panel, QStringLiteral("i32"))).contains(QStringLiteral("字节不足")));
                DoubleClickRow(panel, QStringLiteral("i32"));
                CHECK(!panel.rowView()->isEditing());
                CHECK(!scene->overlay.HasPendingPatches());
            }
        }

        // 复制：右键菜单三项、指针只复制十六进制、时间复制 ISO 8601、悬停复制图标、Ctrl+C、不可用行不复制空串。
        void TestCopy()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            ClickAddress(canvas, kInspectorBase + 0x10);

            // 菜单：三个复制项 + 编辑项，复制项都有图标与提示；触发后剪贴板与状态条正确。
            const int u32Row = RowIndexOf(panel, QStringLiteral("u32"));
            QMenu* menu = panel.buildRowMenu(u32Row);
            CHECK(menu != nullptr);
            QAction* copyValue = FindAction(menu, QStringLiteral("复制值"));
            QAction* copyHex = FindAction(menu, QStringLiteral("复制十六进制"));
            QAction* copyRow = FindAction(menu, QStringLiteral("复制整行"));
            QAction* edit = FindAction(menu, QStringLiteral("编辑此值"));
            CHECK(copyValue != nullptr && copyHex != nullptr && copyRow != nullptr && edit != nullptr);
            CHECK(copyValue != nullptr && copyValue->isEnabled() && !copyValue->toolTip().isEmpty() && !copyValue->icon().isNull());
            CHECK(edit != nullptr && edit->isEnabled() && !edit->icon().isNull());
            SetClip(QStringLiteral("sentinel"));
            copyValue->trigger();
            CHECK(Clip() == QStringLiteral("305419896"));
            CHECK(panel.statusBar()->kind() == HexInspectorStatusBar::Kind::Info);
            CHECK(StatusText(panel).contains(QStringLiteral("305419896")));
            copyHex->trigger();
            CHECK(Clip() == QStringLiteral("0x12345678"));
            copyRow->trigger();
            CHECK(Clip() == QStringLiteral("uint32\t305419896\t0x12345678"));

            // 菜单里的"编辑此值"与双击是同一条路径。
            edit->trigger();
            CHECK(panel.rowView()->isEditing());
            panel.rowView()->endEdit();
            FlushDeferred();
            delete menu;

            // 指针：值列显示描述，但"复制值"得到的是纯十六进制地址。
            ClickAddress(canvas, kInspectorBase + 0x40);
            menu = panel.buildRowMenu(RowIndexOf(panel, QStringLiteral("ptr")));
            FindAction(menu, QStringLiteral("复制值"))->trigger();
            CHECK(Clip() == QStringLiteral("0x00007FF612341A40"));
            delete menu;

            // 时间：复制 ISO 8601。
            ClickAddress(canvas, kInspectorBase + 0x60);
            menu = panel.buildRowMenu(RowIndexOf(panel, QStringLiteral("filetime")));
            FindAction(menu, QStringLiteral("复制值"))->trigger();
            CHECK(Clip() == QStringLiteral("2024-01-01T00:00:00.0000000Z"));
            QAction* disabledEdit = FindAction(menu, QStringLiteral("编辑此值"));
            CHECK(disabledEdit != nullptr && !disabledEdit->isEnabled() && disabledEdit->toolTip().contains(QStringLiteral("只读类型")));
            delete menu;

            // 悬停行尾的复制图标并点击：复制该行的值。
            ClickAddress(canvas, kInspectorBase + 0x10);
            HexInspectorRowView* rows = panel.rowView();
            SetClip(QStringLiteral("sentinel"));
            const QPoint glyph = rows->copyGlyphRect(u32Row).center();
            QTest::mouseMove(rows->viewport(), glyph);
            QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier, glyph);
            CHECK(Clip() == QStringLiteral("305419896"));

            // Ctrl+C：复制当前行的值。
            SetClip(QStringLiteral("sentinel"));
            rows->setCurrentRow(RowIndexOf(panel, QStringLiteral("u16")));
            QTest::keyClick(rows, Qt::Key_C, Qt::ControlModifier);
            CHECK(Clip() == QStringLiteral("22136"));

            // Up/Down/Home/End 移动当前行。
            rows->setCurrentRow(3);
            QTest::keyClick(rows, Qt::Key_Down);
            CHECK(rows->currentRow() == 4);
            QTest::keyClick(rows, Qt::Key_Up);
            CHECK(rows->currentRow() == 3);
            QTest::keyClick(rows, Qt::Key_End);
            CHECK(rows->currentRow() == rows->rowCount() - 1);
            QTest::keyClick(rows, Qt::Key_Home);
            CHECK(rows->currentRow() == 0);

            // 右键事件：行列表发 contextMenuRequested，面板弹出非阻塞菜单；测试里立刻关掉。
            QSignalSpy contextSpy(rows, &HexInspectorRowView::contextMenuRequested);
            const QPoint rowCenter = rows->rowRect(5).center();
            QContextMenuEvent contextEvent(QContextMenuEvent::Mouse, rowCenter, rows->viewport()->mapToGlobal(rowCenter));
            QApplication::sendEvent(rows->viewport(), &contextEvent);
            CHECK(contextSpy.count() == 1 && contextSpy.at(0).at(0).toInt() == 5);
            CHECK(rows->currentRow() == 5);
            for (QMenu* popup : panel.findChildren<QMenu*>())
            {
                popup->close();
                delete popup;
            }

            // 不可用行：没有复制图标可点，菜单复制项置灰；复制请求被拒绝而不是复制空串。
            auto tail = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            ClickAddress(*tail->canvas, kInspectorBase + 0x1FFD);
            const int i32Row = RowIndexOf(*tail->panel, QStringLiteral("i32"));
            SetClip(QStringLiteral("sentinel"));
            const QPoint deadGlyph = tail->panel->rowView()->copyGlyphRect(i32Row).center();
            QTest::mouseClick(tail->panel->rowView()->viewport(), Qt::LeftButton, Qt::NoModifier, deadGlyph);
            CHECK(Clip() == QStringLiteral("sentinel"));
            QMenu* deadMenu = tail->panel->buildRowMenu(i32Row);
            CHECK(deadMenu != nullptr && !FindAction(deadMenu, QStringLiteral("复制值"))->isEnabled());
            CHECK(deadMenu != nullptr && !FindAction(deadMenu, QStringLiteral("复制整行"))->isEnabled());
            delete deadMenu;
            tail->panel->rowView()->setCurrentRow(i32Row);
            QTest::keyClick(tail->panel->rowView(), Qt::Key_C, Qt::ControlModifier);
            CHECK(Clip() == QStringLiteral("sentinel"));
            CHECK(tail->panel->statusBar()->kind() == HexInspectorStatusBar::Kind::Error);
            CHECK(StatusText(*tail->panel).contains(QStringLiteral("不可用")));
        }

        // 右键菜单：显式不透明样式（深浅两套主题下像素实测）、开启悬停提示、行号越界返回空指针。
        void TestMenuStyle()
        {
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            ClickAddress(*scene->canvas, kInspectorBase + 0x10);
            CHECK(panel.buildRowMenu(-1) == nullptr);
            CHECK(panel.buildRowMenu(999) == nullptr);

            for (const bool dark : { false, true })
            {
                ApplyTheme(dark);
                QMenu* menu = panel.buildRowMenu(RowIndexOf(panel, QStringLiteral("u32")));
                CHECK(menu != nullptr);
                const QString sheet = menu->styleSheet();
                CHECK(sheet.contains(KswordTheme::SurfaceColorHex()));
                CHECK(sheet.contains(KswordTheme::TextPrimaryColorHex()));
                CHECK(sheet.contains(KswordTheme::BorderColorHex()));
                CHECK(sheet.contains(KswordTheme::TextDisabledColorHex()));
                CHECK(sheet.contains(QStringLiteral("QMenu::item:selected")));
                CHECK(sheet.contains(QStringLiteral("QMenu::item:disabled")));
                CHECK(!menu->testAttribute(Qt::WA_TranslucentBackground));
                CHECK(menu->autoFillBackground());
                CHECK(menu->toolTipsVisible());

                // 像素实测：菜单右侧靠边的空白处就是菜单底色（不透明、不是黑底）。
                menu->popup(QPoint(40, 40));
                QApplication::processEvents();
                const QImage image = menu->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                const QColor probe = image.pixelColor(image.width() - 6, image.height() / 2);
                CHECK_NOTE(ColorsClose(probe, KswordTheme::SurfaceColor(), 8), probe.name());
                CHECK(probe.alpha() == 255);
                menu->close();
                delete menu;
            }
            ApplyTheme(false);
        }
    }

    // 本文件全部测试的入口。
    void RunInspectorEditTests()
    {
        TestIntegerEdit();
        TestFloatAndPointerEdit();
        TestRejectAndCancel();
        TestUnloadedAtCommit();
        TestReadOnly();
        TestCopy();
        TestMenuStyle();
        ApplyTheme(false);
    }
}
