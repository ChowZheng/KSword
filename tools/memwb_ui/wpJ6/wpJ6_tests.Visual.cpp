// ============================================================
// wpJ6_tests.Visual.cpp
// 作用：窄/宽 × 深/浅四张截图（任务书明确要求"亲自看截图"，本文件只负责
//       产出几何断言 + 落盘，图像由另一步用图像读取工具打开核对）+
//       heightForWidth 坑位的几何断言（Wave 2 截图报告过的"窄宽下待写入
//       ✓/✗ 钮被容器裁掉一截"）。
//
// Wave 3 复核修复记录（写进报告的依据）：
// - 独立审核（wave3 视图审核报告发现 1）三次独立
//   重建稳定复现 TestNarrowWidthDoesNotClipSessionBar 的硬断言失败
//   （wide=narrow=36，900px→420px 乃至 160px 高度恒不变），而上一版把这条
//   硬断言改成了"撞到硬下限就跳过"——这是把真缺陷用放宽断言掩盖了。生产
//   代码侧已经修复根因（MemoryWorkbenchView.Ui.cpp 的 buildUi 把 root 切成
//   QLayout::SetNoConstraint + resizeEvent 主动同步会话条高度 + 侧栏自动
//   折叠 + 三个只读子页 minimumSizeHint 覆盖），本文件对应还原为硬断言。
// - 深色截图改成"先 ApplyTheme(dark) 再新建视图"，不对已构造视图运行期切换
//   （修复缺陷 7；与 wpG_common.cpp 的 ApplyTheme 同一套惯例，见 wpJ6_common
//   新增的 ApplyTheme）。
// ============================================================

#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/theme.h"

#include <QDir>
#include <QKeySequence>
#include <QLayout>
#include <QPixmap>
#include <QShortcut>

#include <cstdlib>

namespace wpj6_test
{
    namespace
    {
        QString ShotsDir()
        {
            // qEnvironmentVariable 是 Qt 的可移植封装，不会触发 MSVC 对
            // CRT getenv 的弃用警告（/W4 /WX 下会被当成错误）。
            const QString fromEnv = qEnvironmentVariable("MEMWB_OUT");
            const QString base = !fromEnv.isEmpty() ? fromEnv : QStringLiteral(".codex-tmp/memwb-wpJ6");
            return base + QStringLiteral("/shots");
        }

        void SaveShot(QWidget* widget, const QString& name)
        {
            QDir().mkpath(ShotsDir());
            const QPixmap pixmap = widget->grab();
            pixmap.save(ShotsDir() + QStringLiteral("/") + name + QStringLiteral(".png"));
        }
    }

    // TestNarrowWidthDoesNotClipSessionBar（Wave 3 还原为硬断言，修复缺陷 1）：
    // 窄宽度下会话条必须真正长高（内部 FlowLayout 换行），外层布局必须采纳
    // 这个新高度；窗口本身必须能被 resize() 到请求的窄宽度（不再被 Qt 自动
    // 回灌的硬性最小尺寸拦住）；窄宽度下侧栏必须已自动折叠；画布宽度不能
    // 被压成一条缝。
    void TestNarrowWidthDoesNotClipSessionBar()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* view = harness.view.get();
        auto* sessionBar = harness.view->sessionBarForTest();
        auto* hexPane = harness.view->hexPaneForTest();
        WPJ6_CHECK(sessionBar != nullptr);
        WPJ6_CHECK(hexPane != nullptr);
        if (sessionBar == nullptr || hexPane == nullptr)
        {
            return;
        }

        view->resize(900, 700);
        view->show();
        PumpFor(80);
        const int wideHeight = sessionBar->height();
        SaveShot(view, QStringLiteral("wpJ6_wide_light"));

        // 任务书明确要求：420 与 360 两个窄宽度都必须被窗口原样接受
        // （view->width()==请求值），不得被硬性最小尺寸拉回更宽的值——这正是
        // 修复 root 的 SetDefaultConstraint 回灌问题要验证的效果。
        for (const int narrowWidth : {420, 360})
        {
            view->resize(narrowWidth, 700);
            PumpFor(80);
            WPJ6_CHECK_NOTE(
                view->width() == narrowWidth,
                QStringLiteral("请求宽度 %1 未被窗口原样接受，实际 %2——说明硬性最小尺寸仍在拦住收窄")
                    .arg(narrowWidth)
                    .arg(view->width()));

            const int narrowHeight = sessionBar->height();
            WPJ6_CHECK_NOTE(
                narrowHeight > wideHeight,
                QStringLiteral("窄宽度 %1 下会话条高度 %2 应大于宽屏高度 %3（FlowLayout 应该已经换行）")
                    .arg(narrowWidth)
                    .arg(narrowHeight)
                    .arg(wideHeight));

            // 会话条自己认为需要的高度与实际分配到的高度不能差太多——差很多
            // 就是"换行了但外层布局没采纳"，Wave 2 报告的裁切现象正是这个
            // 差值；总差值容忍 4px（圆整误差）。
            if (sessionBar->layout() != nullptr && sessionBar->layout()->hasHeightForWidth())
            {
                const int wantedHeight = sessionBar->layout()->totalHeightForWidth(sessionBar->width());
                WPJ6_CHECK_NOTE(
                    std::abs(wantedHeight - narrowHeight) <= 4,
                    QStringLiteral("会话条 heightForWidth=%1 与实际 height=%2 不一致，说明被外层裁切")
                        .arg(wantedHeight)
                        .arg(narrowHeight));
            }

            // 侍栏自动折叠：窄宽度 < 760 必须已经收起（sidebarContainer_ 不在
            // 本类的白盒访问器里，用 mainSplitterForTest 的第二个子控件
            // isHidden() 间接核对，不新增访问器）。
            auto* splitter = harness.view->mainSplitterForTest();
            WPJ6_CHECK(splitter != nullptr);
            if (splitter != nullptr && splitter->count() == 2)
            {
                auto* sidebar = splitter->widget(1);
                WPJ6_CHECK_NOTE(
                    sidebar != nullptr && sidebar->isHidden(),
                    QStringLiteral("窄宽度 %1 下侧栏应已自动折叠").arg(narrowWidth));
            }

            const int canvasWidth = hexPane->canvas()->geometry().width();
            WPJ6_CHECK_NOTE(
                canvasWidth >= 200,
                QStringLiteral("窄宽度 %1 下画布宽 %2 不应被压成一条缝（<200px）")
                    .arg(narrowWidth)
                    .arg(canvasWidth));
        }

        SaveShot(view, QStringLiteral("wpJ6_narrow_light"));
        view->hide();
    }

    // TestFourScreenshotsNarrowWideLightDark（修复缺陷 7）：四张截图落盘，供
    // 人工图像检查。深色两张用一个"先 ApplyTheme(true) 再新建视图"的独立
    // Harness，不对已经在浅色主题下构造/展示过的视图运行期切换主题——那样
    // 窗口背景仍是浅色而文字取了深色主题的浅色文字色，会"字画在同色底上"，
    // 造成产品缺陷的假象（审核报告 wpJ6/wave3 对本坑的描述）。
    void TestFourScreenshotsNarrowWideLightDark()
    {
        // ---- 浅色：构造前先确保主题是浅色 ----
        ApplyTheme(false);
        {
            Harness lightHarness;
            lightHarness.AttachProcess();
            PumpUntil([&]() { return true; }, 10);
            auto* view = lightHarness.view.get();

            view->resize(900, 700);
            view->show();
            PumpFor(50);
            SaveShot(view, QStringLiteral("wpJ6_wide_light_2"));

            view->resize(420, 700);
            PumpFor(50);
            SaveShot(view, QStringLiteral("wpJ6_narrow_light_2"));
            view->hide();
        }

        // ---- 深色：先切主题，再构造一个全新的视图 ----
        ApplyTheme(true);
        {
            Harness darkHarness;
            darkHarness.AttachProcess();
            PumpUntil([&]() { return true; }, 10);
            auto* view = darkHarness.view.get();

            view->resize(420, 700);
            view->show();
            PumpFor(50);
            SaveShot(view, QStringLiteral("wpJ6_narrow_dark"));

            view->resize(900, 700);
            PumpFor(50);
            SaveShot(view, QStringLiteral("wpJ6_wide_dark"));
            view->hide();
        }

        // 还原，不让本测试的深色设置污染后续测试/进程里的其它断言。
        ApplyTheme(false);
        WPJ6_CHECK(true);
    }

    // TestSidebarDoesNotOverlapMainArea：侧栏与主体不重叠，侧栏默认宽度接近
    // 300（ux.md §1），分割条生效。
    void TestSidebarDoesNotOverlapMainArea()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* view = harness.view.get();
        view->resize(900, 700);
        view->show();
        PumpFor(80);

        auto* splitter = harness.view->mainSplitterForTest();
        WPJ6_CHECK(splitter != nullptr);
        if (splitter != nullptr && splitter->count() == 2)
        {
            auto* left = splitter->widget(0);
            auto* right = splitter->widget(1);
            WPJ6_CHECK(left != nullptr && right != nullptr);
            if (left != nullptr && right != nullptr)
            {
                const QRect leftRect = left->geometry();
                const QRect rightRect = right->geometry();
                WPJ6_CHECK_NOTE(!leftRect.intersects(rightRect), QStringLiteral("主体与侧栏几何不应重叠"));
                WPJ6_CHECK_NOTE(rightRect.width() > 150 && rightRect.width() < 450,
                    QStringLiteral("侧栏宽度应接近 300，实际 %1").arg(rightRect.width()));
            }
        }
        view->hide();
    }

    // TestHexPaneCanvasRatioAtViewWidth（Wave 3 分割比例修复新增）：
    // WorkbenchHexPane 嵌在 MemoryWorkbenchView 主体区域里时，分割条实际可用
    // 宽度比窗口总宽度更小——左侧还有会话条占掉一截高度、右侧侧栏占掉约
    // 300px 宽度（见 TestSidebarDoesNotOverlapMainArea），这正是任务书描述
    // 的真实缺陷现场："MemoryWorkbenchView 的宽屏截图里画布甚至只有约 60
    // 像素"。本测试在真实装配（真实会话条/侧栏/标签栈，不是裸的
    // WorkbenchHexPane 单测）下核对修复在这个更挤的场景里仍然生效。
    void TestHexPaneCanvasRatioAtViewWidth()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* view = harness.view.get();
        auto* hexPane = harness.view->hexPaneForTest();
        WPJ6_CHECK(hexPane != nullptr);
        if (hexPane == nullptr)
        {
            return;
        }

        // 宽屏：900px（任务书原文点名的具体宽度）。十六进制画布宽应不小于
        // 解释器面板宽，且不小于 280——修复前实测约 60px，280 是留了余量的
        // 回归线，既能明确区分"修好"与"没修好"，又不会对未来微调公式的
        // 具体系数过分敏感。
        view->resize(900, 700);
        view->show();
        PumpFor(80);

        const int canvasWidthWide = hexPane->canvas()->geometry().width();
        const int inspectorWidthWide = hexPane->inspector()->geometry().width();
        WPJ6_CHECK_NOTE(
            canvasWidthWide >= inspectorWidthWide,
            QStringLiteral("900px 宽视图下十六进制画布宽 %1 应不小于解释器面板宽 %2")
                .arg(canvasWidthWide)
                .arg(inspectorWidthWide));
        WPJ6_CHECK_NOTE(
            canvasWidthWide >= 280,
            QStringLiteral("900px 宽视图下十六进制画布宽 %1 应不小于 280（修复前实测约 60px）")
                .arg(canvasWidthWide));
        SaveShot(view, QStringLiteral("wpJ6_hexpane_ratio_wide"));

        // 窄屏：420px，核对画布没有被压成一条缝（WorkbenchHexPane 自身的
        // 窄窗口下限保护在真实装配里仍然生效；装配层比裸单测挤得多，这里只
        // 要求一个明显比"一条缝"宽得多的下界，不重复 wpJ5 那边对具体公式的
        // 精确核对）。
        view->resize(420, 700);
        PumpFor(80);
        const int canvasWidthNarrow = hexPane->canvas()->geometry().width();
        WPJ6_CHECK_NOTE(
            canvasWidthNarrow >= 100,
            QStringLiteral("420px 窄视图下十六进制画布宽 %1 不应被压成一条缝（<100px）")
                .arg(canvasWidthNarrow));
        SaveShot(view, QStringLiteral("wpJ6_hexpane_ratio_narrow"));

        view->hide();
    }

    // TestManualSidebarExpandRespectedAtNarrowWidth（修复缺陷 1 的配套要求，
    // 任务书原话"用户手动展开后尊重用户"）：窄宽度下侧栏先被自动折叠，用户
    // 按 Ctrl+Shift+B 手动展开之后，哪怕窗口继续在窄宽度范围内变化
    // （resize 触发 maybeAutoCollapseSidebar 重新判断），自动折叠逻辑也不应
    // 再把它收回去——sidebarUserOverride_ 一旦置真就不再自作主张。
    void TestManualSidebarExpandRespectedAtNarrowWidth()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* view = harness.view.get();
        auto* splitter = view->mainSplitterForTest();
        WPJ6_CHECK(splitter != nullptr && splitter->count() == 2);
        if (splitter == nullptr || splitter->count() != 2)
        {
            return;
        }
        auto* sidebar = splitter->widget(1);
        WPJ6_CHECK(sidebar != nullptr);

        view->resize(420, 700);
        view->show();
        PumpFor(80);
        WPJ6_CHECK_NOTE(sidebar->isHidden(), QStringLiteral("窄宽度下侧栏应已自动折叠（前置条件）"));

        // Ctrl+Shift+B：手动展开。
        QShortcut* toggle = nullptr;
        for (auto* shortcut : view->findChildren<QShortcut*>())
        {
            if (shortcut->key() == QKeySequence(QStringLiteral("Ctrl+Shift+B")))
            {
                toggle = shortcut;
                break;
            }
        }
        WPJ6_CHECK_NOTE(toggle != nullptr, QStringLiteral("未找到 Ctrl+Shift+B 快捷键"));
        if (toggle == nullptr)
        {
            return;
        }
        emit toggle->activated();
        WPJ6_CHECK_NOTE(!sidebar->isHidden(), QStringLiteral("手动按 Ctrl+Shift+B 后侧栏应展开"));

        // 窗口继续在窄宽度区间内变化（仍然 < 760），自动折叠逻辑不应再把它
        // 收回去——这正是"用户手动展开后尊重用户"要验证的行为。
        view->resize(500, 700);
        PumpFor(80);
        WPJ6_CHECK_NOTE(
            !sidebar->isHidden(),
            QStringLiteral("用户手动展开后，窗口在窄宽度区间内继续变化不应被自动折叠逻辑收回"));
        view->resize(360, 700);
        PumpFor(80);
        WPJ6_CHECK_NOTE(
            !sidebar->isHidden(),
            QStringLiteral("用户手动展开后，窗口收到更窄仍不应被自动折叠逻辑收回"));

        view->hide();
    }

    void RunVisualTests()
    {
        TestNarrowWidthDoesNotClipSessionBar();
        TestFourScreenshotsNarrowWideLightDark();
        TestSidebarDoesNotOverlapMainArea();
        TestHexPaneCanvasRatioAtViewWidth();
        TestManualSidebarExpandRespectedAtNarrowWidth();
    }
}
