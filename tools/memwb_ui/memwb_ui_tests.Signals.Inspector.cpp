// memwb_ui_tests.Signals.Inspector.cpp
// 作用：第二轮接口补全的"面板侧"离屏验证——
//   1) 解释器面板不再有轮询定时器（对象树、元对象、行为三个角度）：绕过画布改叠加层后，
//      面板在远超旧轮询间隔的等待里纹丝不动，直到宿主 notifyOverlayChanged() 才跟上；
//   2) 面板订阅 contentChanged / editableChanged：页回填、refresh、换叠加层、可编辑切换都能让面板自己更新；
//   3) 行内编辑只经画布公开的 stageBytes：画布 editStaged 恰好一次、面板没有自己的 editStaged，
//      画布拒绝的原因只发画布的 editRejected、面板自己的拒绝只发面板的 editRejected（互不重复）。
// 全部手势经 QTest 模拟的真实输入或公开接口进入，结果从面板的公开访问器与信号读回。

#include "memwb_ui_signals.h"

#include <QApplication>
#include <QMetaObject>
#include <QSignalSpy>
#include <QTimer>

namespace memwb_test
{
    namespace
    {
        using ks::ui::HexCanvas;
        using ks::ui::HexInspectorPanel;
        using ks::ui::HexInspectorStatusBar;
        using Bytes = std::vector<std::uint8_t>;

        // LoadPatternBaseline：把 MakePattern(4096) 载入叠加层基线（起点 kInspectorBase，掩码全 1），返回该页内容。
        QByteArray LoadPatternBaseline(ksword::memwb::MemoryDiffOverlay& overlay)
        {
            const QByteArray pattern = MakePattern(4096);
            const Bytes bytes(
                reinterpret_cast<const std::uint8_t*>(pattern.constData()),
                reinterpret_cast<const std::uint8_t*>(pattern.constData()) + pattern.size());
            overlay.LoadBaseline("memwb-signals-inspector", kInspectorBase, bytes, Bytes(bytes.size(), 1));
            return pattern;
        }

        // 面板没有轮询定时器：对象树、元对象与行为三个角度。
        void TestNoPollingTimer()
        {
            ApplyTheme(false);
            auto scene = MakeAsyncInspectorScene(true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;

            // 对象树里没有任何 QTimer（旧实现的定时器是值成员、不在对象树里，所以这条只挡"改成有父对象的定时器"，
            // 真正的判据是下面的行为测试）。元对象里没有 onPendingTick 槽，也没有面板自己的 editStaged 信号。
            CHECK(panel.findChildren<QTimer*>().isEmpty());
            CHECK(panel.metaObject()->indexOfSlot("onPendingTick()") < 0);
            CHECK(panel.metaObject()->indexOfSignal("editStaged(quint64,quint64)") < 0);
            CHECK(panel.metaObject()->indexOfSignal("editRejected(QString)") >= 0);

            // 起点处是"未加载"字节（旧实现据此启动 150 ms 间隔的轮询，最多 10 秒）。
            canvas.setCaretAddress(kInspectorBase + 0x10);
            Flush();
            CHECK(!RowOf(panel, QStringLiteral("u8")).available);

            // 绕过画布直接暂存：画布此刻显示的是补丁值 0x77（Pending 补丁让该字节"有值"），但没有任何信号。
            LoadPatternBaseline(scene->overlay);
            CHECK(scene->overlay.Stage(kInspectorBase + 0x10, Bytes({ 0x77 })) == ksword::memwb::StageStatus::Ok);
            CHECK(canvas.cellStateAt(kInspectorBase + 0x10).hasValue);
            CHECK(canvas.cellStateAt(kInspectorBase + 0x10).value == 0x77);

            // 等待 500 ms（旧轮询间隔的三倍多）：没有轮询就没有任何东西会重读，面板保持旧状态。
            QTest::qWait(500);
            CHECK(!RowOf(panel, QStringLiteral("u8")).available);

            // 宿主通知之后：信号是排队的（还没发），处理一轮事件后面板跟上，u8 = 0x77 = 119。
            QSignalSpy content(&canvas, &HexCanvas::contentChanged);
            canvas.notifyOverlayChanged();
            CHECK(content.count() == 0);
            CHECK(!RowOf(panel, QStringLiteral("u8")).available);
            Flush();
            CHECK(content.count() == 1);
            CHECK(RowOf(panel, QStringLiteral("u8")).available);
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QStringLiteral("119"));
        }

        // 面板跟随画布的 contentChanged：页回填、refresh 换代次、换叠加层，都不需要手动 refreshFromCanvas。
        void TestFollowsContentChanged()
        {
            ApplyTheme(false);
            auto scene = MakeAsyncInspectorScene(true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            canvas.setCaretAddress(kInspectorBase + 0x40);
            Flush();
            CHECK(!RowOf(panel, QStringLiteral("u32")).available);

            // 页回填：u32 = 该页内偏移 0x40 处的四个字节（小端）。
            const QByteArray pattern = LoadPatternBaseline(scene->overlay);
            canvas.deliverPage(kInspectorBase, pattern, QByteArray(4096, '\x01'), canvas.sourceRevision());
            CHECK(!RowOf(panel, QStringLiteral("u32")).available);
            Flush();
            const quint32 expected = static_cast<quint32>(static_cast<quint8>(pattern.at(0x40)))
                | (static_cast<quint32>(static_cast<quint8>(pattern.at(0x41))) << 8)
                | (static_cast<quint32>(static_cast<quint8>(pattern.at(0x42))) << 16)
                | (static_cast<quint32>(static_cast<quint8>(pattern.at(0x43))) << 24);
            CHECK(RowOf(panel, QStringLiteral("u32")).available);
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QString::number(expected));

            // refresh：缓存清空，提供者不再回填，页回到"未加载"；面板自己变成不可用，不需要手动 refreshFromCanvas。
            canvas.refresh();
            Flush();
            CHECK(!RowOf(panel, QStringLiteral("u32")).available);
            CHECK(RowOf(panel, QStringLiteral("u32")).toolTip.contains(QStringLiteral("尚未加载")));

            // 换叠加层：没有叠加层就不能编辑，各行的"可编辑"状态与原因随之变化；装回去恢复。
            const int u32Row = RowIndexOf(panel, QStringLiteral("u32"));
            CHECK(RowOf(panel, QStringLiteral("u32")).editEnabled);
            canvas.setOverlay(nullptr);
            Flush();
            CHECK(!RowOf(panel, QStringLiteral("u32")).editEnabled);
            CHECK(panel.editBlockedReason(u32Row).contains(QStringLiteral("只读")));
            canvas.setOverlay(&scene->overlay);
            Flush();
            CHECK(RowOf(panel, QStringLiteral("u32")).editEnabled);

            // 宿主绕过面板直接暂存（stageBytes 保持插入点不动）：排队的 contentChanged 让面板读到补丁值。
            // 先让页回来，使 u8 本来就可用，这样变化的是"值"。
            canvas.refresh();
            canvas.deliverPage(kInspectorBase, pattern, QByteArray(4096, '\x01'), canvas.sourceRevision());
            Flush();
            const int original = static_cast<unsigned char>(pattern.at(0x40));
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QString::number(original));
            CHECK(canvas.stageBytes(kInspectorBase + 0x40, QByteArray(1, '\x4A')));
            CHECK(canvas.caretAddress() == kInspectorBase + 0x40);
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QString::number(original));
            Flush();
            CHECK(RowOf(panel, QStringLiteral("u8")).valueText == QStringLiteral("74"));
        }

        // 画布的 editableChanged 同步到达面板：各行"可编辑"状态、原因、状态条提示、双击行为立即跟随。
        void TestFollowsEditableChanged()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            QSignalSpy rejected(&panel, &HexInspectorPanel::editRejected);
            ClickAddress(canvas, kInspectorBase + 0x10);
            const int u32Row = RowIndexOf(panel, QStringLiteral("u32"));
            CHECK(RowOf(panel, QStringLiteral("u32")).editEnabled);
            CHECK(panel.editBlockedReason(u32Row).isEmpty());
            CHECK(panel.statusBar()->messageText().contains(QStringLiteral("双击")));

            // 切到只读：editableChanged 是同步信号，不需要处理事件。
            canvas.setEditable(false);
            CHECK(!RowOf(panel, QStringLiteral("u32")).editEnabled);
            CHECK(panel.editBlockedReason(u32Row).contains(QStringLiteral("只读")));
            CHECK_NOTE(panel.statusBar()->messageText().contains(QStringLiteral("只读视图")), panel.statusBar()->messageText());
            DoubleClickRow(panel, QStringLiteral("u32"));
            CHECK(!panel.rowView()->isEditing());
            CHECK(rejected.count() == 1);

            // 切回可编辑：双击又能打开编辑器。
            canvas.setEditable(true);
            CHECK(RowOf(panel, QStringLiteral("u32")).editEnabled);
            CHECK(panel.editBlockedReason(u32Row).isEmpty());
            CHECK(panel.statusBar()->messageText().contains(QStringLiteral("双击")));
            DoubleClickRow(panel, QStringLiteral("u32"));
            CHECK(panel.rowView()->isEditing());
            panel.rowView()->endEdit();
            FlushDeferred();
        }

        // 编辑只经画布 stageBytes，信号不重复：成功只有画布的 editStaged 一次；
        // 画布拒绝的原因只发画布的 editRejected；面板自己的拒绝（编码失败、不能开始编辑）只发面板的 editRejected。
        void TestNoDuplicateSignals()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            QSignalSpy canvasStaged(&canvas, &HexCanvas::editStaged);
            QSignalSpy canvasRejected(&canvas, &HexCanvas::editRejected);
            QSignalSpy panelRejected(&panel, &HexInspectorPanel::editRejected);
            QSignalSpy content(&canvas, &HexCanvas::contentChanged);
            ClickAddress(canvas, kInspectorBase + 0x10);
            Flush();
            content.clear();

            // (1) 成功：画布 editStaged 恰好一次（地址、长度正确），contentChanged 排队一次，两处 editRejected 都没有。
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("7"));
            PressInEditor(panel, Qt::Key_Return);
            FlushDeferred();
            CHECK(canvasStaged.count() == 1);
            CHECK(canvasStaged.count() == 1 && canvasStaged.at(0).at(0).toULongLong() == kInspectorBase + 0x10
                && canvasStaged.at(0).at(1).toULongLong() == 4);
            CHECK(canvasRejected.isEmpty());
            CHECK(panelRejected.isEmpty());
            CHECK(content.count() == 1);
            CHECK(RowOf(panel, QStringLiteral("u32")).valueText == QStringLiteral("7"));

            // (2) 面板自己的拒绝：编码失败。只有面板的 editRejected，画布没有收到任何东西。
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("abc"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(panelRejected.count() == 1);
            CHECK(canvasRejected.isEmpty());
            CHECK(canvasStaged.count() == 1);
            panel.rowView()->endEdit();
            FlushDeferred();

            // (3) 画布拒绝的原因：叠加层基线换成只覆盖前 0x20 字节的小窗口，窗口外编辑被 Stage 拒绝。
            // 画布发 editRejected 一次（原因与状态条一致），面板不再发第二遍；编辑器保持打开。
            const Bytes small(0x20, 1);
            scene->overlay.LoadBaseline(std::string("small"), kInspectorBase, small, small);
            ClickAddress(canvas, kInspectorBase + 0x100);
            DoubleClickRow(panel, QStringLiteral("u32"));
            TypeIntoEditor(panel, QStringLiteral("9"));
            PressInEditor(panel, Qt::Key_Return);
            CHECK(panel.rowView()->isEditing());
            CHECK(canvasRejected.count() == 1);
            CHECK(panelRejected.count() == 1);
            CHECK(canvasRejected.count() == 1
                && canvasRejected.at(0).at(0).toString() == panel.statusBar()->messageText());
            CHECK(canvasRejected.count() == 1
                && canvasRejected.at(0).at(0).toString().contains(QStringLiteral("数据窗口")));
            CHECK(canvasStaged.count() == 1);
            panel.rowView()->endEdit();
            FlushDeferred();

            // (4) 面板自己的拒绝：只读画布上双击。面板 editRejected 加一，画布没有任何拒绝。
            canvas.setEditable(false);
            DoubleClickRow(panel, QStringLiteral("u32"));
            CHECK(panelRejected.count() == 2);
            CHECK(canvasRejected.count() == 1);
            canvas.setEditable(true);
        }

        // 与当前值相同的行内编辑：不暂存、不发任何画布信号（editStaged、contentChanged 都没有），状态条说明。
        void TestUnchangedEditEmitsNothing()
        {
            ApplyTheme(false);
            auto scene = MakeInspectorScene(MakeInspectorData(), true, true, QSize(1100, 560), 400);
            HexInspectorPanel& panel = *scene->panel;
            HexCanvas& canvas = *scene->canvas;
            ClickAddress(canvas, kInspectorBase + 0x10);
            Flush();
            QSignalSpy canvasStaged(&canvas, &HexCanvas::editStaged);
            QSignalSpy canvasRejected(&canvas, &HexCanvas::editRejected);
            QSignalSpy content(&canvas, &HexCanvas::contentChanged);

            DoubleClickRow(panel, QStringLiteral("u8"));
            TypeIntoEditor(panel, QStringLiteral("120"));
            PressInEditor(panel, Qt::Key_Return);
            FlushDeferred();
            CHECK(!panel.rowView()->isEditing());
            CHECK(panel.statusBar()->messageText().contains(QStringLiteral("值没有变化")));
            CHECK(canvasStaged.isEmpty());
            CHECK(canvasRejected.isEmpty());
            CHECK(content.isEmpty());
            CHECK(!scene->overlay.HasPendingPatches());
        }
    }

    // 本文件全部测试的入口。
    void RunInspectorSignalTests()
    {
        TestNoPollingTimer();
        TestFollowsContentChanged();
        TestFollowsEditableChanged();
        TestNoDuplicateSignals();
        TestUnchangedEditEmitsNothing();
        ApplyTheme(false);
    }
}
