// ============================================================
// wpJ6_tests.DockFill.cpp
// 作用：内存 Dock "页面过长、整页滚动"修复的离屏回归（层 1 + 层 2）。
//
// 背景：MemoryDock 的 QTabWidget 会把所有页签（含隐藏页）的最小高度取最大值，DDMA 页单页就撑到
//       约 1200px；Dock 被 ADS 的外层 QScrollArea 包着时，被滚走的是整个 Dock（标题、工具栏、页签栏、
//       状态栏）。修法是每个页签自带一个"对外最小尺寸为 0"的内部滚动壳（ks::ui::EnablePageInnerScroll），
//       页签控件再用 ks::ui::IsolateMinimumSize 隔离，宿主去掉外层滚动。
//
// 层 1（只依赖 Qt + header-only 工具）：合成一个"头部 + QTabWidget + 状态条"的 Dock 形态宿主，
//       页签里放 1200px 高 / 1000px 宽的超高页，核对：
//       - 包壳后宿主最小高度很小，未包壳的对照组确实会被撑到 1000 以上（夹具有区分力）；
//       - 视口矮于内容时壳出滚动条、高于内容时内容铺满视口且无滚动条（宽度同理）；
//       - 壳滚到底，头部、页签栏、状态栏的位置不动；
//       - 宿主放进外层 QScrollArea（ADS AutoScrollArea 的模型）里不出外层滚动条——这就是用户看到的症状；
//       - 壳与内容容器、视口都不自填背景：绿色背板上的洋红色宿主，页内空白处必须透出绿色；
//       - 页面的直接子控件只有壳一个；以页面为父的控件加入布局后被重挂到内容容器；重复包壳幂等。
// 层 2（链接真实 MemoryWorkbenchView）：把真实工作台容器页（不包壳，与生产一致）放进同一个
//       Dock 形态宿主，在 640 与 420 两种高度下核对工作台铺满页签栈、宿主不被撑高、外层不出滚动条，
//       且工作台自身的 minimumSizeHint 仍然很小（它不贡献最小高度）。
//
// 变异对照（由不写实现的一方注入，见交付报告）：
//   去掉某页 EnablePageInnerScroll -> 层 1 的宿主最小高度/外层滚动条断言失败；
//   去掉壳的 Ignored 策略 -> 页面 minimumSizeHint 与壳策略断言失败；
//   setWidgetResizable(false) -> "内容高度等于视口高度"断言失败；
//   去掉 setAutoFillBackground(false)（内容或视口）-> 属性断言与背板透出断言失败；
//   去掉 IsolateMinimumSize -> 隔离对照断言失败。
// 入口：RunDockFillTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/AdaptivePageScroll.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/HexCanvas.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchHexPane.h"

#include <QApplication>
#include <QColor>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPoint>
#include <QScrollArea>
#include <QScrollBar>
#include <QSize>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace wpj6_test
{
    namespace
    {
        // kTallPageHeight：合成超高页的内容最小高度，量级与真实 DDMA 页（约 1070px）相当。
        constexpr int kTallPageHeight = 1200;

        // kWidePageWidth：合成超宽页的内容最小宽度，量级与篡改检测页那行复选框（约 970px）相当。
        constexpr int kWidePageWidth = 1000;

        // kMaxPageMinimumExtent：包壳后每个页面的 minimumSizeHint 允许的最大边长。
        // 壳的 Ignored 策略让它恰为 0；去掉 Ignored 后会回到 QScrollArea 自己的最小提示（约 30 以上）。
        constexpr int kMaxPageMinimumExtent = 8;

        // DockShapedHost：模拟 MemoryDock 的根结构——头部标签、QTabWidget（stretch 1）、状态标签，
        // 边距 6、间距 6，与 MemoryDock::initializeUi 一致。本类自己就是宿主 QWidget，可作为顶层窗口，
        // 也可以交给外层 QScrollArea 接管（模拟 ADS AutoScrollArea）。
        class DockShapedHost final : public QWidget
        {
        public:
            // 构造：isolateTabs 为真时对页签控件调用 IsolateMinimumSize，对应生产里的加固。
            explicit DockShapedHost(const bool isolateTabs)
                : QWidget(nullptr)
            {
                // 根布局：边距与间距照搬 MemoryDock 的根布局。
                auto* rootLayout = new QVBoxLayout(this);
                rootLayout->setContentsMargins(6, 6, 6, 6);
                rootLayout->setSpacing(6);

                // 头部标签：对应 MemoryDock 的标题行。
                headerLabel_ = new QLabel(QStringLiteral("header"), this);
                rootLayout->addWidget(headerLabel_);

                // 页签控件：文档模式，与生产一致；按需隔离最小尺寸。
                tabs_ = new QTabWidget(this);
                tabs_->setDocumentMode(true);
                if (isolateTabs)
                {
                    ks::ui::IsolateMinimumSize(tabs_);
                }
                rootLayout->addWidget(tabs_, 1);

                // 状态标签：对应 MemoryDock 的底部状态栏。
                statusLabel_ = new QLabel(QStringLiteral("status"), this);
                rootLayout->addWidget(statusLabel_);
            }

            // tabs / headerLabel / statusLabel：只读访问器，供测试读取几何。
            QTabWidget* tabs() const { return tabs_; }
            QLabel* headerLabel() const { return headerLabel_; }
            QLabel* statusLabel() const { return statusLabel_; }

            // tabBar：QTabWidget::tabBar 是 protected，这里按直接子对象找回（生产代码同一手法）。
            QTabBar* tabBar() const
            {
                return tabs_->findChild<QTabBar*>(QString(), Qt::FindDirectChildrenOnly);
            }

            // stack：页签控件内部的页面栈，用来读"页面可用高度"。
            QStackedWidget* stack() const
            {
                return tabs_->findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly);
            }

        private:
            QLabel* headerLabel_ = nullptr;   // 头部标签。
            QTabWidget* tabs_ = nullptr;      // 页签控件。
            QLabel* statusLabel_ = nullptr;   // 状态标签。
        };

        // TallPage：BuildTallPage 的返回值，把后面断言要用到的几个控件指针打包。
        struct TallPage
        {
            QWidget* page = nullptr;       // 加入 QTabWidget 的页面本身（指针身份不应因包壳而变）。
            QWidget* content = nullptr;    // 建布局的容器：包壳时是内容容器，不包壳时就是页面本身。
            QLabel* headLabel = nullptr;   // 页内顶部标签，以页面为父创建再加入布局。
            QWidget* block = nullptr;      // 撑出最小尺寸的内容块。
        };

        // BuildTallPage：在 tabs 里造一个最小尺寸为 minimumWidth x minimumHeight 的页。
        // 传入：tabs 目标页签控件；wrapped 是否调用 EnablePageInnerScroll；
        //       minimumWidth / minimumHeight 内容块的最小尺寸。
        // 传出：TallPage。控件都以页面为父创建，与生产页面的写法一致，
        //       用来验证"以页面为父的控件加入内容布局后被重挂到内容容器"。
        TallPage BuildTallPage(
            QTabWidget* const tabs,
            const bool wrapped,
            const int minimumWidth,
            const int minimumHeight)
        {
            TallPage result;
            result.page = new QWidget(tabs);

            // 包壳时在页面上建壳并取内容容器；不包壳时布局直接建在页面上（旧结构）。
            result.content = wrapped ? ks::ui::EnablePageInnerScroll(result.page) : result.page;

            // 内容布局：边距与间距照搬生产页面（6/6）。
            auto* contentLayout = new QVBoxLayout(result.content);
            contentLayout->setContentsMargins(6, 6, 6, 6);
            contentLayout->setSpacing(6);

            // 页内顶部标签与撑尺寸的内容块，父控件都是页面本身。
            result.headLabel = new QLabel(QStringLiteral("page head"), result.page);
            contentLayout->addWidget(result.headLabel);
            result.block = new QWidget(result.page);
            result.block->setMinimumSize(minimumWidth, minimumHeight);
            contentLayout->addWidget(result.block, 1);

            tabs->addTab(result.page, QStringLiteral("tall page"));
            return result;
        }

        // FindShell：取页面的内部滚动壳（页面的直接子 QScrollArea，按稳定 objectName 找）。
        QScrollArea* FindShell(QWidget* const page)
        {
            return page->findChild<QScrollArea*>(
                QString::fromLatin1(ks::ui::kAdaptivePageScrollObjectName),
                Qt::FindDirectChildrenOnly);
        }

        // TopOf：控件左上角在宿主坐标系里的纵坐标，用来比较"滚动前后位置不变"。
        int TopOf(const DockShapedHost& host, const QWidget* const widget)
        {
            return widget->mapTo(&host, QPoint(0, 0)).y();
        }

        // TestShellKeepsHostShort：包壳 + 隔离后宿主最小高度很小；未包壳的对照组确实被撑高；
        // 只做隔离（页面没包壳）时宿主也不被撑高——这是 IsolateMinimumSize 加固的存在意义。
        void TestShellKeepsHostShort()
        {
            // 对照组：旧结构（未包壳、未隔离）。它必须真的被撑到 1000 以上，否则夹具没有区分力，
            // 后面所有"很小"的断言都可能是空转。
            {
                DockShapedHost control(false);
                BuildTallPage(control.tabs(), false, 100, kTallPageHeight);
                WPJ6_CHECK_NOTE(
                    control.minimumSizeHint().height() >= 1000,
                    QStringLiteral("对照组（未包壳未隔离）的宿主最小高度应被撑到 1000 以上，实际 %1，夹具没有区分力")
                        .arg(control.minimumSizeHint().height()));
            }

            // 实验组：包壳 + 隔离，再放一个普通小页，两页并存。
            DockShapedHost host(true);
            const TallPage tall = BuildTallPage(host.tabs(), true, kWidePageWidth, kTallPageHeight);
            auto* smallPage = new QWidget(host.tabs());
            host.tabs()->addTab(smallPage, QStringLiteral("small page"));

            // 宿主最小高度：只剩头部 + 状态条 + 页签栏之类，远小于 1200。
            WPJ6_CHECK_NOTE(
                host.minimumSizeHint().height() <= 200,
                QStringLiteral("包壳后宿主最小高度应 <= 200，实际 %1").arg(host.minimumSizeHint().height()));
            WPJ6_CHECK_NOTE(
                host.tabs()->minimumSizeHint().height() <= 60,
                QStringLiteral("包壳后 QTabWidget 的最小高度应 <= 60，实际 %1").arg(host.tabs()->minimumSizeHint().height()));

            // 页面本身的最小提示：壳的 Ignored 策略让它恰为 0，去掉 Ignored 会回到 QScrollArea 自己的最小提示。
            WPJ6_CHECK_NOTE(
                tall.page->minimumSizeHint().height() <= kMaxPageMinimumExtent
                    && tall.page->minimumSizeHint().width() <= kMaxPageMinimumExtent,
                QStringLiteral("包壳页面的最小提示应接近 0，实际 %1x%2")
                    .arg(tall.page->minimumSizeHint().width()).arg(tall.page->minimumSizeHint().height()));

            // 只隔离不包壳：页面没壳时超高页在宿主里被裁剪，但绝不能把宿主头部/状态条挤坏。
            {
                DockShapedHost isolatedOnly(true);
                BuildTallPage(isolatedOnly.tabs(), false, kWidePageWidth, kTallPageHeight);
                WPJ6_CHECK_NOTE(
                    isolatedOnly.minimumSizeHint().height() <= 200,
                    QStringLiteral("只隔离 QTabWidget 时（页面没包壳）宿主最小高度也应 <= 200，实际 %1")
                        .arg(isolatedOnly.minimumSizeHint().height()));
                WPJ6_CHECK(isolatedOnly.tabs()->sizePolicy().verticalPolicy() == QSizePolicy::Ignored);
                WPJ6_CHECK(isolatedOnly.tabs()->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored);
            }
        }

        // TestShellProperties：壳的结构属性——这些是让上面几何行为成立的前提，直接断言能更早、更准地定位退化。
        void TestShellProperties()
        {
            DockShapedHost host(true);
            const TallPage tall = BuildTallPage(host.tabs(), true, kWidePageWidth, kTallPageHeight);
            QScrollArea* const shell = FindShell(tall.page);
            WPJ6_CHECK_NOTE(shell != nullptr, QStringLiteral("页面的直接子对象里应有名为 ks_adaptive_page_scroll 的 QScrollArea"));
            if (shell == nullptr)
            {
                return;
            }

            // 页面指针身份不变：QTabWidget::indexOf 仍然按页面指针找得到（bindTab、setCurrentWidget 都依赖它）。
            WPJ6_CHECK(host.tabs()->indexOf(tall.page) == 0);
            WPJ6_CHECK(host.tabs()->widget(0) == tall.page);

            // 壳的几何属性：内容随视口缩放、无边框、不抢焦点、两个方向都是 Ignored 且最小尺寸为 0。
            WPJ6_CHECK(shell->widgetResizable());
            WPJ6_CHECK(shell->frameShape() == QFrame::NoFrame);
            WPJ6_CHECK(shell->focusPolicy() == Qt::NoFocus);
            WPJ6_CHECK(shell->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored);
            WPJ6_CHECK(shell->sizePolicy().verticalPolicy() == QSizePolicy::Ignored);
            WPJ6_CHECK(shell->minimumSize() == QSize(0, 0));

            // 内容容器：就是壳里的 widget，objectName 稳定，布局确实建在它上面。
            WPJ6_CHECK(shell->widget() == tall.content);
            WPJ6_CHECK(tall.content->objectName() == QString::fromLatin1(ks::ui::kAdaptivePageContentObjectName));
            WPJ6_CHECK(tall.content->layout() != nullptr);

            // 背景：QScrollArea::setWidget 会把内容容器的 autoFillBackground 置真，视口默认也自填背景；
            // 壁纸/毛玻璃模式下两者都会在页内盖出实色块，必须都是假。
            WPJ6_CHECK_NOTE(!tall.content->autoFillBackground(), QStringLiteral("内容容器不得自填背景"));
            WPJ6_CHECK_NOTE(!shell->viewport()->autoFillBackground(), QStringLiteral("壳的视口不得自填背景"));

            // 壳不得带本地样式表（跟随全局样式与调色板）。
            WPJ6_CHECK(shell->styleSheet().isEmpty());
            WPJ6_CHECK(tall.content->styleSheet().isEmpty());

            // 页面的直接子控件只有壳一个：游离在页面上而不在内容里的控件会盖在壳上面。
            const QList<QWidget*> directChildren =
                tall.page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
            WPJ6_CHECK_NOTE(
                directChildren.size() == 1 && directChildren.front() == shell,
                QStringLiteral("包壳页面的直接子控件应只有壳，实际 %1 个").arg(directChildren.size()));

            // 以页面为父创建、再加入内容布局的控件，必须被重挂到内容容器（生产页面都是这种写法）。
            WPJ6_CHECK(tall.headLabel->parentWidget() == tall.content);
            WPJ6_CHECK(tall.block->parentWidget() == tall.content);

            // 幂等：重复包壳返回同一个内容容器，不套第二层壳。
            QWidget* const again = ks::ui::EnablePageInnerScroll(tall.page);
            WPJ6_CHECK(again == tall.content);
            WPJ6_CHECK(tall.page->findChildren<QScrollArea*>(QString(), Qt::FindDirectChildrenOnly).size() == 1);

            // 空指针安全：返回空指针且不崩。
            WPJ6_CHECK(ks::ui::EnablePageInnerScroll(nullptr) == nullptr);
            ks::ui::IsolateMinimumSize(nullptr);
        }

        // TestShellScrollsWhenShortAndFillsWhenTall：视口矮于内容时壳滚动，高于内容时内容铺满视口。
        // 同时核对壳滚到底时头部、页签栏、状态栏的位置不动——整页滚走正是用户看到的症状。
        void TestShellScrollsWhenShortAndFillsWhenTall()
        {
            DockShapedHost host(true);
            const TallPage tall = BuildTallPage(host.tabs(), true, kWidePageWidth, kTallPageHeight);
            QScrollArea* const shell = FindShell(tall.page);
            WPJ6_CHECK(shell != nullptr);
            if (shell == nullptr)
            {
                return;
            }

            // 窄而矮：两个方向都放不下，壳出两个滚动条，宿主本身保持请求的尺寸。
            host.resize(600, 300);
            host.show();
            PumpFor(80);
            WPJ6_CHECK_NOTE(
                host.height() == 300 && host.width() == 600,
                QStringLiteral("宿主应能缩到 600x300，实际 %1x%2（页面最小尺寸又传播上来了）").arg(host.width()).arg(host.height()));
            WPJ6_CHECK_NOTE(
                shell->verticalScrollBar()->maximum() > 0,
                QStringLiteral("视口矮于内容时壳应出纵向滚动条，maximum=%1").arg(shell->verticalScrollBar()->maximum()));
            WPJ6_CHECK_NOTE(
                shell->horizontalScrollBar()->maximum() > 0,
                QStringLiteral("视口窄于内容时壳应出横向滚动条，maximum=%1").arg(shell->horizontalScrollBar()->maximum()));

            // 壳铺满页面（零边距）：多一圈边距会让页面内容区比页面小一圈，小页签里滚动条出现得更早。
            WPJ6_CHECK_NOTE(
                shell->geometry() == tall.page->rect(),
                QStringLiteral("壳的几何应等于页面矩形，壳 %1,%2 %3x%4，页面 %5x%6")
                    .arg(shell->geometry().x()).arg(shell->geometry().y()).arg(shell->width()).arg(shell->height())
                    .arg(tall.page->width()).arg(tall.page->height()));

            // 滚到底、滚到最右：头部、页签栏、状态栏的位置不得变化，状态栏仍在宿主之内且可见。
            const int headerTopBefore = TopOf(host, host.headerLabel());
            const int tabBarTopBefore = TopOf(host, host.tabBar());
            const int statusTopBefore = TopOf(host, host.statusLabel());
            shell->verticalScrollBar()->setValue(shell->verticalScrollBar()->maximum());
            shell->horizontalScrollBar()->setValue(shell->horizontalScrollBar()->maximum());
            PumpFor(60);
            WPJ6_CHECK_NOTE(TopOf(host, host.headerLabel()) == headerTopBefore, QStringLiteral("壳滚动后头部标签被移动了"));
            WPJ6_CHECK_NOTE(TopOf(host, host.tabBar()) == tabBarTopBefore, QStringLiteral("壳滚动后页签栏被移动了"));
            WPJ6_CHECK_NOTE(TopOf(host, host.statusLabel()) == statusTopBefore, QStringLiteral("壳滚动后状态栏被移动了"));
            WPJ6_CHECK(host.statusLabel()->isVisible());
            WPJ6_CHECK(host.statusLabel()->geometry().bottom() <= host.height());
            WPJ6_CHECK(host.headerLabel()->geometry().top() >= 0);

            // 又宽又高：内容放得下，不出滚动条，且内容容器恰好铺满视口（widgetResizable 的可观测后果）。
            host.resize(1400, 1500);
            PumpFor(80);
            WPJ6_CHECK_NOTE(
                shell->verticalScrollBar()->maximum() == 0,
                QStringLiteral("视口高于内容时不应出纵向滚动条，maximum=%1").arg(shell->verticalScrollBar()->maximum()));
            WPJ6_CHECK_NOTE(
                shell->horizontalScrollBar()->maximum() == 0,
                QStringLiteral("视口宽于内容时不应出横向滚动条，maximum=%1").arg(shell->horizontalScrollBar()->maximum()));
            WPJ6_CHECK_NOTE(
                tall.content->height() == shell->viewport()->height(),
                QStringLiteral("内容高度应等于视口高度（内容铺满），内容 %1 视口 %2")
                    .arg(tall.content->height()).arg(shell->viewport()->height()));
            WPJ6_CHECK_NOTE(
                tall.content->width() == shell->viewport()->width(),
                QStringLiteral("内容宽度应等于视口宽度（内容铺满），内容 %1 视口 %2")
                    .arg(tall.content->width()).arg(shell->viewport()->width()));
            host.hide();
        }

        // TestOuterScrollAreaNotNeeded：宿主放进外层 QScrollArea（ADS AutoScrollArea 的模型）。
        // 对照组（旧结构）外层必须出滚动条——这就是用户看到的"整页在滚"；实验组外层不得出滚动条。
        void TestOuterScrollAreaNotNeeded()
        {
            // 对照组：未包壳、未隔离。
            {
                QScrollArea outerControl;
                outerControl.setWidgetResizable(true);
                outerControl.setFrameShape(QFrame::NoFrame);
                auto* controlHost = new DockShapedHost(false);
                BuildTallPage(controlHost->tabs(), false, 100, kTallPageHeight);
                outerControl.setWidget(controlHost);
                outerControl.resize(900, 500);
                outerControl.show();
                PumpFor(80);
                WPJ6_CHECK_NOTE(
                    outerControl.verticalScrollBar()->maximum() > 0,
                    QStringLiteral("对照组（旧结构）外层应出纵向滚动条以复现症状，实际 maximum=%1，夹具没有区分力")
                        .arg(outerControl.verticalScrollBar()->maximum()));
                outerControl.hide();
            }

            // 实验组：包壳 + 隔离，两个高度都不得出外层滚动条，宿主恰好铺满外层视口。
            for (const int outerHeight : {500, 300})
            {
                QScrollArea outer;
                outer.setWidgetResizable(true);
                outer.setFrameShape(QFrame::NoFrame);
                auto* host = new DockShapedHost(true);
                BuildTallPage(host->tabs(), true, kWidePageWidth, kTallPageHeight);
                outer.setWidget(host);
                outer.resize(1100, outerHeight);
                outer.show();
                PumpFor(80);
                WPJ6_CHECK_NOTE(
                    outer.verticalScrollBar()->maximum() == 0,
                    QStringLiteral("高 %1：外层不应出纵向滚动条，maximum=%2").arg(outerHeight).arg(outer.verticalScrollBar()->maximum()));
                WPJ6_CHECK_NOTE(
                    outer.horizontalScrollBar()->maximum() == 0,
                    QStringLiteral("高 %1：外层不应出横向滚动条，maximum=%2").arg(outerHeight).arg(outer.horizontalScrollBar()->maximum()));
                WPJ6_CHECK_NOTE(
                    host->height() == outer.viewport()->height(),
                    QStringLiteral("高 %1：宿主应铺满外层视口，宿主 %2 视口 %3")
                        .arg(outerHeight).arg(host->height()).arg(outer.viewport()->height()));
                outer.hide();
            }
        }

        // RenderOverBackdrop：把包壳的 Dock 形态宿主放在绿色背板上，宿主及其子树的调色板 Window 角色是洋红色。
        // 宿主与内容都不自填背景时，页内空白处会透出背板的绿色；只要内容容器或壳的视口自填背景，
        // 就会画出洋红色的实色块。注意要抓背板而不是宿主：透明宿主自己 grab() 得到的是透明像素。
        // 传入：useTransparentStyleSheet 为真时给宿主挂与 MainWindow 同形状的本地透明样式；
        //       samplePointOut 返回页内空白处在背板坐标系里的采样点。
        // 传出：背板渲染图（统一成 ARGB32）。
        QImage RenderOverBackdrop(const bool useTransparentStyleSheet, QPoint* const samplePointOut)
        {
            QWidget backdrop;
            backdrop.resize(900, 600);
            QPalette backdropPalette = backdrop.palette();
            backdropPalette.setColor(QPalette::Window, QColor(0, 200, 0));
            backdrop.setPalette(backdropPalette);
            backdrop.setAutoFillBackground(true);

            auto* backdropLayout = new QVBoxLayout(&backdrop);
            backdropLayout->setContentsMargins(0, 0, 0, 0);
            auto* host = new DockShapedHost(true);
            backdropLayout->addWidget(host);

            // 宿主的调色板 Window 设成洋红：宿主子树里任何自填背景的控件都会画成洋红。
            QPalette hostPalette = host->palette();
            hostPalette.setColor(QPalette::Window, QColor(255, 0, 255));
            host->setPalette(hostPalette);
            if (useTransparentStyleSheet)
            {
                // 与 MainWindow::ensureDockContentInitialized 给非内核 Dock 追加的本地样式同形状。
                host->setStyleSheet(QStringLiteral(
                    "QWidget{background:transparent;background-color:transparent;}"));
            }

            const TallPage tall = BuildTallPage(host->tabs(), true, 100, kTallPageHeight);
            backdrop.show();
            PumpFor(100);

            // 采样点取页面靠下 3/4 处：那里是内容块（不画任何东西）所在的空白区域。
            if (samplePointOut != nullptr)
            {
                *samplePointOut = tall.page->mapTo(
                    &backdrop, QPoint(tall.page->width() / 2, (tall.page->height() * 3) / 4));
            }
            const QImage image = backdrop.grab().toImage().convertToFormat(QImage::Format_ARGB32);
            backdrop.hide();
            return image;
        }

        // TestShellDoesNotPaintBackground：浅色/深色主题 × 有/无本地透明样式，页内空白处都必须透出背板绿色。
        void TestShellDoesNotPaintBackground()
        {
            for (const bool dark : {false, true})
            {
                // 先切主题再构造控件，与 ApplyTheme 的使用约定一致。
                ApplyTheme(dark);
                for (const bool withStyleSheet : {false, true})
                {
                    QPoint samplePoint;
                    const QImage image = RenderOverBackdrop(withStyleSheet, &samplePoint);
                    const QString label = QStringLiteral("%1/%2")
                        .arg(dark ? QStringLiteral("深色") : QStringLiteral("浅色"))
                        .arg(withStyleSheet ? QStringLiteral("带透明样式") : QStringLiteral("无样式"));
                    WPJ6_CHECK_NOTE(
                        !image.isNull() && image.rect().contains(samplePoint),
                        QStringLiteral("%1：背板渲染图为空或采样点越界 (%2,%3)").arg(label).arg(samplePoint.x()).arg(samplePoint.y()));
                    if (image.isNull() || !image.rect().contains(samplePoint))
                    {
                        continue;
                    }

                    // 采样色必须是背板绿色（容差 3，防止渲染抗锯齿差异）；洋红说明壳画了实色块。
                    const QColor sampled = image.pixelColor(samplePoint);
                    const bool isBackdropGreen =
                        std::abs(sampled.red() - 0) <= 3
                        && std::abs(sampled.green() - 200) <= 3
                        && std::abs(sampled.blue() - 0) <= 3;
                    WPJ6_CHECK_NOTE(
                        isBackdropGreen,
                        QStringLiteral("%1：页内空白处应透出背板绿色 (0,200,0)，实际 (%2,%3,%4)——壳或内容容器画了实色块")
                            .arg(label).arg(sampled.red()).arg(sampled.green()).arg(sampled.blue()));
                }
            }
            ApplyTheme(false);
        }

        // ViewReleaser：作用域结束时把视图从宿主祖先链上摘下来，归还给 Harness 的 unique_ptr 管理。
        // 声明在宿主之后，因此先于宿主析构，避免宿主删除子控件后 unique_ptr 再删一次。
        struct ViewReleaser
        {
            QWidget* view = nullptr;   // 被摘下的视图（只读裸指针，不持有）。
            ~ViewReleaser()
            {
                if (view != nullptr)
                {
                    view->setParent(nullptr);
                }
            }
        };

        // TestWorkbenchFillsDockShapedHost：真实工作台容器页放进 Dock 形态宿主（外面再套外层滚动区）。
        // 容器页不包壳，与生产（MemoryDock::initializeWorkbenchTab）一致；另一个页签是包壳的超高页，
        // 证明有超高页并存时工作台页仍然铺满、宿主不被撑高、外层不出滚动条。
        void TestWorkbenchFillsDockShapedHost()
        {
            for (const int outerHeight : {640, 420})
            {
                Harness harness;
                harness.AttachProcess();
                PumpUntil([]() { return true; }, 10);
                auto* view = harness.view.get();

                // 宿主直接做顶层窗口：生产里内存 Dock 走 ADS 的 ForceNoScrollArea，没有外层滚动区。
                // （刻意不再套 QScrollArea：QScrollArea 在 widgetResizable 下会把子树的 heightForWidth
                //  当最小高度，而工作台会话条的 FlowLayout 恰好是 heightForWidth 的——那是旧 AutoScrollArea
                //  路径的行为，与现在的生产拓扑无关，套上去只会让本测试断言一个不存在的场景。）
                auto host = std::make_unique<DockShapedHost>(true);
                ViewReleaser releaser;
                releaser.view = view;

                // 容器页：垂直布局、无边距，视图铺满（照搬 MemoryDock::initializeWorkbenchTab / ensureWorkbenchView）。
                auto* container = new QWidget(host->tabs());
                auto* containerLayout = new QVBoxLayout(container);
                containerLayout->setContentsMargins(0, 0, 0, 0);
                containerLayout->setSpacing(0);
                view->setParent(container);
                containerLayout->addWidget(view);
                host->tabs()->addTab(container, QStringLiteral("workbench"));

                // 第二个页签：包壳的超高页。
                const TallPage tall = BuildTallPage(host->tabs(), true, kWidePageWidth, kTallPageHeight);

                host->resize(1100, outerHeight);
                host->show();
                PumpFor(150);

                // 工作台页是当前页：视图铺满页面栈，容器页同高。
                QStackedWidget* const stack = host->stack();
                WPJ6_CHECK(stack != nullptr);
                const QString label = QStringLiteral("外层高 %1").arg(outerHeight);
                if (stack != nullptr)
                {
                    WPJ6_CHECK_NOTE(
                        view->height() == stack->height(),
                        QStringLiteral("%1：工作台视图高度应等于页面栈高度，视图 %2 栈 %3")
                            .arg(label).arg(view->height()).arg(stack->height()));
                    WPJ6_CHECK_NOTE(
                        container->height() == stack->height(),
                        QStringLiteral("%1：容器页高度应等于页面栈高度，容器 %2 栈 %3")
                            .arg(label).arg(container->height()).arg(stack->height()));

                    // 十六进制画布不能被挤没：阈值取保守值（工作台自己的会话条/地址条/状态条约占 140px），
                    // 并把实际数字打出来，供人工对照。
                    auto* canvas = view->hexPaneForTest()->canvas();
                    WPJ6_CHECK(canvas != nullptr);
                    if (canvas != nullptr)
                    {
                        std::printf(
                            "DockFill: %s host=%d stack=%d view=%d canvas=%d\n",
                            label.toUtf8().constData(), host->height(), stack->height(), view->height(), canvas->height());
                        WPJ6_CHECK_NOTE(
                            canvas->height() * 10 >= stack->height() * 3,
                            QStringLiteral("%1：十六进制画布高度 %2 不足页面栈高度 %3 的三成")
                                .arg(label).arg(canvas->height()).arg(stack->height()));
                    }
                }

                // 宿主：高度就是我们给的高度（不被页内内容撑高），最小高度很小（Dock 不会把窗口顶出屏幕）。
                WPJ6_CHECK_NOTE(
                    host->height() == outerHeight,
                    QStringLiteral("%1：宿主高度应等于给定高度，实际 %2").arg(label).arg(host->height()));
                WPJ6_CHECK_NOTE(
                    host->minimumSizeHint().height() <= 100,
                    QStringLiteral("%1：宿主（Dock 形态）最小高度应很小，实际 %2").arg(label).arg(host->minimumSizeHint().height()));

                // 工作台自己不贡献最小高度：minimumSizeHint 仍然很小（宽高都不超过 100）。
                WPJ6_CHECK_NOTE(
                    view->minimumSizeHint().width() <= 100 && view->minimumSizeHint().height() <= 100,
                    QStringLiteral("%1：工作台 minimumSizeHint 应很小，实际 %2x%3")
                        .arg(label).arg(view->minimumSizeHint().width()).arg(view->minimumSizeHint().height()));

                // 切到超高页：壳在该高度下出滚动条，外层依旧没有滚动条。
                host->tabs()->setCurrentWidget(tall.page);
                PumpFor(100);
                QScrollArea* const shell = FindShell(tall.page);
                WPJ6_CHECK(shell != nullptr);
                if (shell != nullptr)
                {
                    WPJ6_CHECK_NOTE(
                        shell->verticalScrollBar()->maximum() > 0,
                        QStringLiteral("%1：超高页的壳应出纵向滚动条，maximum=%2").arg(label).arg(shell->verticalScrollBar()->maximum()));
                }

                // 切回工作台页：视图仍铺满页面栈（页签切换不改变工作台的尺寸）。
                host->tabs()->setCurrentWidget(container);
                PumpFor(100);
                if (stack != nullptr)
                {
                    WPJ6_CHECK_NOTE(
                        view->height() == stack->height(),
                        QStringLiteral("%1：切回工作台页后视图高度应等于页面栈高度，视图 %2 栈 %3")
                            .arg(label).arg(view->height()).arg(stack->height()));
                }
                host->hide();
            }
        }
    }

    // RunDockFillTests：本文件入口，由 wpJ6_main.cpp 调用。
    // 末尾复位成浅色主题，避免影响后面的测试组。
    void RunDockFillTests()
    {
        TestShellKeepsHostShort();
        TestShellProperties();
        TestShellScrollsWhenShortAndFillsWhenTall();
        TestOuterScrollAreaNotNeeded();
        TestShellDoesNotPaintBackground();
        TestWorkbenchFillsDockShapedHost();
        ApplyTheme(false);
    }
}
