// ============================================================
// wpJ5_tests.Visual.cpp
// 作用：视觉类回归——不只断言 width()>0，而是断言子控件的几何关系（左右不
// 重叠、查找条打开后真的占据一段高度并把分割条推下去）；并抓真实截图落盘，
// 供人工用图像读取工具逐张查看（自查清单 l 条：上一波的回归正是"截图是坏的
// 却没人看、断言只测宽度>0"漏掉的，本包不重复这个坑）。
// 截图目录：环境变量 MEMWB_OUT 指定的目录下的 shots 子目录，与构建脚本的
// 产物目录约定一致；未设置时退回 .codex-tmp/wpJ5-shots（相对当前工作目录，
// 构建脚本以仓库根目录为 cwd 启动本程序）。
// ============================================================

#include "wpJ5_common.h"

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QSplitter>
#include <QTest>

namespace wpj5_test
{
    namespace
    {
        // ShotsDir：截图目录，惰性创建。
        QString ShotsDir()
        {
            QByteArray fromEnv = qgetenv("MEMWB_OUT");
            QString base = fromEnv.isEmpty() ? QStringLiteral(".codex-tmp/wpJ5-shots") : (QString::fromLocal8Bit(fromEnv) + QStringLiteral("/shots"));
            QDir().mkpath(base);
            return base;
        }

        // SaveShot：抓取 widget 当前外观并保存为 PNG，返回是否成功写盘——
        // 调用方必须用 WPJ5_CHECK 断言这个返回值，截图保存失败不能被静默忽略
        // （否则"之后去看截图"这件事会落空而没人知道）。
        bool SaveShot(QWidget* widget, const QString& name)
        {
            const QPixmap pixmap = widget->grab();
            const QString path = ShotsDir() + QLatin1Char('/') + name + QStringLiteral(".png");
            return pixmap.save(path, "PNG");
        }

        // V1：基本布局——画布在左、解释器面板在右，彼此不重叠，查找条默认
        // 不占用任何可见高度。
        void TestBasicLayoutGeometry()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFFF, "visual-basic");
            pane.show();
            QApplication::processEvents();

            const QRect canvasRect = pane.canvas()->geometry();
            const QRect inspectorRect = pane.inspector()->geometry();

            WPJ5_CHECK_NOTE(canvasRect.width() > 0 && canvasRect.height() > 0, "画布必须有实际可见面积");
            WPJ5_CHECK_NOTE(inspectorRect.width() > 0 && inspectorRect.height() > 0, "解释器面板必须有实际可见面积");
            WPJ5_CHECK_NOTE(
                canvasRect.right() <= inspectorRect.left(),
                QString("画布右边界 %1 必须不越过解释器面板左边界 %2，两者是左右分割，不能重叠")
                    .arg(canvasRect.right())
                    .arg(inspectorRect.left()));
            // 不止断言 isVisible()==false：隐藏的 QWidget 仍可能保留之前布局
            // 算出的几何矩形（height() 不保证归零），真正要看的是它有没有在
            // 布局里占用空间——分割条（画布的父容器）顶边必须紧贴 0，查找条
            // 关闭时不应该挤占任何高度。
            WPJ5_CHECK(!pane.findBar()->isVisible());
            const int splitterTopWhenClosed = pane.canvas()->parentWidget()->geometry().top();
            WPJ5_CHECK_NOTE(
                splitterTopWhenClosed == 0,
                QString("查找条关闭时分割条顶边应为 0，实际 %1（说明查找条仍占用了布局空间）")
                    .arg(splitterTopWhenClosed));

            WPJ5_CHECK(SaveShot(&pane, QStringLiteral("basic_layout_findbar_closed")));
        }

        // V2：打开查找条之后，它必须真的获得一段非零高度并把下面的分割条推
        // 下去（不是"isVisible()==true 但高度仍是 0"这种半吊子状态——上一波
        // 回归正是这个形状：实现者自己的截图已经是坏的，只断言了宽度）。
        void TestFindBarOpenPushesSplitterDown()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(900, 420);
            pane.setAddressSpace(0, 0xFFFF, "visual-findbar");
            pane.show();
            QApplication::processEvents();

            pane.openFind();
            QApplication::processEvents();

            const int findBarHeight = pane.findBar()->geometry().height();
            WPJ5_CHECK_NOTE(
                pane.findBar()->isVisible() && findBarHeight > 8,
                QString("查找条打开后必须有明显的可见高度，实际 %1 像素").arg(findBarHeight));

            // 分割条（画布所在的那个容器）必须整体被查找条推下去，顶边不再是
            // 0——否则查找条只是叠在画布上方，布局并没有真正让位。
            const int splitterTop = pane.canvas()->parentWidget()->geometry().top();
            WPJ5_CHECK_NOTE(
                splitterTop >= findBarHeight - 2,
                QString("分割条顶边 %1 应当不小于查找条高度 %2（允许 2 像素边框误差）")
                    .arg(splitterTop)
                    .arg(findBarHeight));

            WPJ5_CHECK(SaveShot(&pane, QStringLiteral("findbar_open_pushes_splitter_down")));
        }

        // V3：窄窗口下文字不被裁切——解释器面板的建议宽度留出足够空间，验证
        // 缩到最小宽度之后画布依然非零宽（没有被解释器面板挤到负数区域）。
        void TestNarrowWidthDoesNotCollapseCanvas()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(420, 420);
            pane.setAddressSpace(0, 0xFFF, "visual-narrow");
            pane.show();
            QApplication::processEvents();

            const QRect canvasRect = pane.canvas()->geometry();
            WPJ5_CHECK_NOTE(
                canvasRect.width() > 0,
                QString("窄窗口（420px）下画布宽度仍必须为正，实际 %1").arg(canvasRect.width()));

            WPJ5_CHECK(SaveShot(&pane, QStringLiteral("narrow_width_420px")));
        }
        // V4（独立审核 wave3 review-wpJ5.md §1.5 夹具缺口补测，Wave 3 分割比例
        // 修复时重写）：分割条拉伸比例——画布占满剩余空间（拉伸因子 1），
        // 解释器面板按内容固定宽度（拉伸因子 0）。
        //
        // **本测试取代了旧版本"inspectorDelta<=4"（解释器宽度在任意窗口宽度
        // 下都应保持绝对像素不变）的断言**：旧断言成立的前提是"构造时从不
        // 显式 setSizes，分割条宽度只靠 Qt 的拉伸因子在 resize 时分配增量"；
        // 本次修复给 WorkbenchHexPane 新增了 showEvent/resizeEvent 驱动的
        // applyInitialSplitterSizes()（见 WorkbenchHexPane.h 增量④），只要
        // 用户没有手动拖过分割条，**每一次**窗口尺寸变化都会按文档公式
        // "解释器宽度=clamp(面板 sizeHint 宽, 240, 分割器宽×36%)"重新收敛，
        // 这是刻意的设计（否则"先在宽窗口摆好、再把窗口拖窄"这条路径会让
        // 画布被压成一条缝，正是任务书要修的根因之一），解释器宽度本身就应该
        // 随窗口变窄而收紧，不能再假设它绝对不变。
        //
        // 新断言直接对应任务书原文列出的三条判据，覆盖任务书点名的三种宽度
        // （900/1400 代表正常宽屏；420 是任务书点名的窄窗口样例，触发"画布
        // 不能被压成一条缝"的下限保护分支）：
        //   ①解释器宽 ≤ 分割器宽的 36%+1（允许整数除法的 1 像素误差）；
        //   ②画布宽 ≥ 200（任何宽度下都不能被压成一条缝）；
        //   ③900/1400 两个正常宽度下，画布宽 ≥ 解释器宽（画布占主导——如果
        //     拉伸因子被颠倒或 clamp 公式的上下界被写反，画布反而会比解释器
        //     窄，这条断言能抓住）。420 窄窗口不做"画布≥解释器"要求：解释器
        //     的 240 下限本身就可能比画布的半行下限还大，两者谁大谁小不是
        //     任务书原文要求的判据。
        void TestSplitterRatioAcrossWidths()
        {
            const int widths[] = { 900, 1400, 420 };
            for (int totalWidth : widths)
            {
                ks::ui::WorkbenchHexPane pane;
                pane.resize(totalWidth, 420);
                pane.setAddressSpace(0, 0xFFFF, std::string("visual-ratio-") + std::to_string(totalWidth));
                pane.show();
                QApplication::processEvents();

                // "分割器宽"直接读画布父容器（即 splitter_）当前的真实宽度，
                // 不是窗口本身的宽度——两者理应相等（root_ 外边距为 0），但
                // 直接读分割条自身的宽度更贴合公式原文"分割器宽×36%"的字面
                // 意思，不依赖这条"理应相等"的间接推断。
                const int splitterWidth = pane.canvas()->parentWidget()->width();
                const int canvasWidth = pane.canvas()->geometry().width();
                const int inspectorWidth = pane.inspector()->geometry().width();

                if (totalWidth == 900 || totalWidth == 1400)
                {
                    // 900/1400 两个正常宽度下，分割器宽×36% 这个上界恒大于
                    // HexInspectorPanel 自己的硬性最小宽度（约 260px，见
                    // HexInspectorPanel::minimumSizeHint），公式本身就是实际
                    // 生效的那个上界，可以直接按任务书原文的三条判据核对。
                    const int maxInspectorWidth = splitterWidth * 36 / 100 + 1;
                    WPJ5_CHECK_NOTE(
                        inspectorWidth <= maxInspectorWidth,
                        QString("宽度 %1 下解释器宽 %2 不应超过分割器宽 %3 的 36%%（上限 %4）")
                            .arg(totalWidth)
                            .arg(inspectorWidth)
                            .arg(splitterWidth)
                            .arg(maxInspectorWidth));

                    WPJ5_CHECK_NOTE(
                        canvasWidth >= 200,
                        QString("宽度 %1 下画布宽 %2 应不小于 200（不能被压成一条缝）")
                            .arg(totalWidth)
                            .arg(canvasWidth));

                    WPJ5_CHECK_NOTE(
                        canvasWidth >= inspectorWidth,
                        QString("宽度 %1 下画布宽 %2 应不小于解释器宽 %3（画布应占主导）")
                            .arg(totalWidth)
                            .arg(canvasWidth)
                            .arg(inspectorWidth));
                }
                else
                {
                    // 420（窄窗口）：HexInspectorPanel 自己的硬性最小宽度
                    // （约 260px）此时已经比"分割器宽×36%"（约 151px）和
                    // "画布≥200"这两条正常宽度下的判据更紧——QSplitter 不会
                    // 把任何子控件压到它自己 minimumSizeHint 以下，所以这两条
                    // 判据在 420px 这个具体宽度下物理上不可能同时成立，不能
                    // 照抄到窄窗口分支。窄窗口下真正该核对的是"画布拿到了
                    // 硬性约束允许范围内尽力而为的那一份"，与
                    // TestNarrowWidthCanvasFloorIsHalfOneRow 共用同一条
                    // best-effort 公式（见该测试的注释）。
                    const int inspectorHardMin = pane.inspector()->minimumSizeHint().width();
                    const int bestEffortCanvasWidth = totalWidth - inspectorHardMin;
                    WPJ5_CHECK_NOTE(
                        canvasWidth >= bestEffortCanvasWidth - 8 && canvasWidth <= bestEffortCanvasWidth + 1,
                        QString("宽度 %1 下画布宽 %2 应接近"
                                "（总宽-解释器硬下限 %3）的尽力而为值 %4（允许分割条手柄宽度误差）")
                            .arg(totalWidth)
                            .arg(canvasWidth)
                            .arg(inspectorHardMin)
                            .arg(bestEffortCanvasWidth));
                    WPJ5_CHECK_NOTE(canvasWidth > 0, QStringLiteral("420px 下画布宽至少要为正"));
                }

                WPJ5_CHECK(SaveShot(&pane, QStringLiteral("splitter_ratio_width_%1").arg(totalWidth)));
            }
        }

        // V5（Wave 3 分割比例修复新增）：窄窗口下限保护的专项断言。
        //
        // **实测记录（写进报告，供审核核对）**：HexInspectorPanel::
        // minimumSizeHint 固定约 260px（HexInspectorPanel.cpp:
        // "max(rows.width(), 260)"），QSplitter::setSizes 不允许任何子控件
        // 被压到它自己这条硬性最小宽度以下——这个硬下限（260）比本函数公式
        // 的下界常量（240）还大，而窄窗口分支（<600px）的第一步 clamp 在
        // totalWidth<667 时上下界都会收敛到 240，所以解释器宽度最终恒被硬
        // 下限 260 决定，与"半行宽度"这条第二层保护（oneRowWidth/2，本机
        // HexCanvas::sizeHint 实测约 569px，半行约 284px）完全无关——260 已经
        // 比 284 更紧地决定了画布能拿到多少空间，"半行保护"这条分支在**当前
        // 这个具体面板实现**下恒为死代码（hardMin=260 使它永远先于半行逻辑
        // 介入）。这不是本函数的 bug：半行保护仍然是为"未来某个声明更小硬下限
        // 的解释器实现"准备的第二道防线，只是在今天这个 260px 的真实面板上
        // 测不出区别——变异测试因此不把"除以 2 改成除以 4"这类半行除数本身
        // 列为可被本包夹具捕获的变异目标（见 fix-layout.md 对这条限制的说明），
        // 改为覆盖"硬下限补偿"这一步真正生效的逻辑。
        void TestNarrowWidthCanvasFloorIsHardMinAware()
        {
            ks::ui::WorkbenchHexPane pane;
            pane.resize(420, 420);
            pane.setAddressSpace(0, 0xFFFF, "visual-floor-420");
            pane.show();
            QApplication::processEvents();

            const int canvasWidth = pane.canvas()->geometry().width();
            const int inspectorHardMin = pane.inspector()->minimumSizeHint().width();
            // 分割条手柄本身也占几像素宽度，允许个位数误差；下界再放宽到
            // -8（手柄+四舍五入），上界不放宽（画布不应该比"总宽-硬下限"更
            // 宽，否则说明硬下限补偿步骤没有生效，解释器会被压到硬下限以下，
            // 这才是本测试真正要抓的缺陷）。
            const int expectedCanvasWidth = 420 - inspectorHardMin;

            WPJ5_CHECK_NOTE(
                canvasWidth >= expectedCanvasWidth - 8 && canvasWidth <= expectedCanvasWidth,
                QString("420px 窄窗口下画布宽 %1 应接近（总宽 420 - 解释器硬下限 %2 = %3），"
                        "允许分割条手柄宽度造成的个位数误差")
                    .arg(canvasWidth)
                    .arg(inspectorHardMin)
                    .arg(expectedCanvasWidth));
            WPJ5_CHECK_NOTE(
                canvasWidth > 0,
                QStringLiteral("420px 窄窗口下画布宽必须为正——不能被压成一条缝"));

            WPJ5_CHECK(SaveShot(&pane, QStringLiteral("narrow_floor_hard_min_aware")));
        }
    }

    // V5b（Wave 3 分割比例修复新增）：已经显示过一次之后，窗口再被 resize——
    // 自动摆放必须跟着重新收敛，不能停留在首次显示时算出的旧比例。单独拆成
    // 一个测试：既有的 TestSplitterRatioAcrossWidths 每个宽度都是"show() 之前
    // 先 resize 好、只显示一次"，从未覆盖"先显示、再 resize"这条路径
    // （resizeEvent 单独驱动 applyInitialSplitterSizes 的那一半），
    // TestUserDragDisablesAutoSizing 虽然也在 show() 之后 resize，但那条测试
    // 专门验证"拖过之后不应该再摆"，反而测不出"没拖过时应该摆"这件事。
    void TestResizeAfterShowReappliesRatio()
    {
        ks::ui::WorkbenchHexPane pane;
        pane.resize(900, 420);
        pane.setAddressSpace(0, 0xFFFF, "visual-resize-after-show");
        pane.show();
        QApplication::processEvents();

        const int inspectorWidthAt900 = pane.inspector()->geometry().width();

        // 显示之后再收窄——用户没有拖过分割条，resizeEvent 必须重新按公式
        // 摆一次，解释器宽度应该跟着变小（36% 上界随总宽度变窄而变窄）。
        pane.resize(420, 420);
        QApplication::processEvents();
        const int inspectorWidthAt420 = pane.inspector()->geometry().width();

        WPJ5_CHECK_NOTE(
            inspectorWidthAt420 < inspectorWidthAt900,
            QString("显示后再把窗口从 900px 收窄到 420px，解释器宽度应当跟着变小"
                    "（900px 时 %1，420px 时 %2）——resizeEvent 没有重新摆放")
                .arg(inspectorWidthAt900)
                .arg(inspectorWidthAt420));

        WPJ5_CHECK(SaveShot(&pane, QStringLiteral("resize_after_show_reapplies_ratio")));
    }

    // V6（Wave 3 分割比例修复新增）：用户真的拖动过分割条之后，自动摆放必须
    // 永久停用——任务书原文"一旦用户拖过分割条（splitterMoved）就不再自动
    // 改"。用 QTest 对分割条手柄（QSplitterHandle，公开可取）发送真实的
    // 按下/移动/释放鼠标事件序列，走与用户真正拖拽完全相同的 Qt 内部路径
    // （`QSplitter::moveSplitter` 是 protected 成员，测试代码访问不到；
    // 直接调用 `splitter_->setSizes(...)` 则根本不会触发 splitterMoved，
    // 测不出这条分支）。
    void TestUserDragDisablesAutoSizing()
    {
        ks::ui::WorkbenchHexPane pane;
        pane.resize(900, 420);
        pane.setAddressSpace(0, 0xFFFF, "visual-drag");
        pane.show();
        QApplication::processEvents();

        auto* splitter = qobject_cast<QSplitter*>(pane.canvas()->parentWidget());
        WPJ5_CHECK(splitter != nullptr);
        if (splitter == nullptr)
        {
            return;
        }

        // index 1 是画布与解释器面板之间那一条手柄；把它从当前位置向左拖
        // 150 像素（解释器面板因此变宽），模拟用户真的用鼠标拖拽过。
        QSplitterHandle* dragHandle = splitter->handle(1);
        WPJ5_CHECK(dragHandle != nullptr);
        if (dragHandle == nullptr)
        {
            return;
        }
        const QPoint handleCenter(dragHandle->width() / 2, dragHandle->height() / 2);
        QTest::mousePress(dragHandle, Qt::LeftButton, Qt::NoModifier, handleCenter);
        QTest::mouseMove(dragHandle, handleCenter - QPoint(150, 0));
        QTest::mouseRelease(dragHandle, Qt::LeftButton, Qt::NoModifier, handleCenter - QPoint(150, 0));
        QApplication::processEvents();
        const int inspectorWidthAfterDrag = pane.inspector()->geometry().width();

        pane.resize(1400, 420);
        QApplication::processEvents();
        const int inspectorWidthAfterResize = pane.inspector()->geometry().width();

        WPJ5_CHECK_NOTE(
            qAbs(inspectorWidthAfterResize - inspectorWidthAfterDrag) <= 8,
            QString("用户拖动分割条后（解释器宽 %1），后续 resize 不应该把解释器宽度重新摆回"
                    "公式计算值（resize 后变成 %2）——splitterMoved 应当让自动摆放永久停用")
                .arg(inspectorWidthAfterDrag)
                .arg(inspectorWidthAfterResize));

        WPJ5_CHECK(SaveShot(&pane, QStringLiteral("user_drag_disables_auto_sizing")));
    }

    void RunVisualTests()
    {
        TestBasicLayoutGeometry();
        TestFindBarOpenPushesSplitterDown();
        TestNarrowWidthDoesNotCollapseCanvas();
        TestSplitterRatioAcrossWidths();
        TestNarrowWidthCanvasFloorIsHardMinAware();
        TestResizeAfterShowReappliesRatio();
        TestUserDragDisablesAutoSizing();
    }
}
