// ============================================================
// memory_dock_layout_tests.cpp
// 作用：内存 Dock "页面过长/整页滚动"修复的层 3 回归——链接真实 MemoryDock（生产对象），
//       不用合成控件。由 tools/Invoke-MemoryDockLayoutTests.ps1 编译并与已有 Release 构建的
//       全部生产对象链接（做法同 tools/Invoke-MemoryEditorUiTests.ps1）；需先有 Release 构建才能跑。
//
// 背景：MemoryDock 的 QTabWidget 对所有页（含隐藏的旧页）的最小高度取最大值，DDMA 页单页约 1070px，
//       Dock 最小高度因此约 1200px；Dock 被 ADS 的外层 QScrollArea 包着时，整个 Dock（标题、工具栏、
//       页签栏、状态栏）被滚走。修法：每页自带内部滚动壳（ks::ui::EnablePageInnerScroll，工作台容器页除外），
//       页签控件用 IsolateMinimumSize 加固，MainWindow 对 memory 用 ForceNoScrollArea。
//
// 用法：exe 不带参数 = 断言模式；带 --baseline = 只打印基线表格（每页最小提示、壳是否存在）与
//       各项断言的 CHECK FAIL 行，但退出码恒为 0。改前基线应能看到至少一页最小高度 > 1000、
//       dock 最小高度 > 1000；改后每页接近 0。
//
// 测试清单（每条括号里是它能杀死的退化）：
//   1 页面最小提示：dock 最小高度 <= 200、宽度 <= 900；每页（含隐藏旧页）最小提示 <= 8；无壳页至多一个（工作台）；
//     页面直接子控件只有壳；页签控件已隔离（去掉某页 EnablePageInnerScroll / 去掉壳的 Ignored / 去掉 IsolateMinimumSize）。
//   2 缩放与滚动：560 高时 DDMA 壳出滚动条且 dock 不被撑高；滚到底后标题/页签栏/状态栏位置不动；
//     1700 高时各页壳无滚动条、DDMA 内容恰好铺满视口（壳 setWidgetResizable(false) / 页面最小高度回流）。
//   3 ADS 承载：ForceNoScrollArea 下 dock 与宿主之间没有 dockWidgetScrollArea，dock 不超出宿主；
//     对照组 AutoScrollArea 下该 QScrollArea 存在（证明断言不是空转）。
//   4 透明：绿色背板上的洋红色 dock，DDMA 页空白处必须透出绿色；浅/深色 × 无样式/带透明样式（壳或内容自填背景）。
//   5 内嵌形态：仿 ProcessDetailWindow，dock 设 Ignored 放进 900x500 宿主，dock 尺寸等于宿主。
//
// 约束：不访问驱动；不切到系统内存审计页（会触发整机快照采集）；不切到工作台页（会走 ConfigureShared 写地址簿）；
//       QSettings 重定向到临时 INI，不碰用户真实设置。
// ============================================================
#include "../Ksword5.1/Ksword5.1/MemoryDock/DdmaPage.h"
#include "../Ksword5.1/Ksword5.1/MemoryDock/MemoryDock.h"
#include "../Ksword5.1/Ksword5.1/MemoryDock/SystemMemoryAuditPage.h"
#include "../Ksword5.1/Ksword5.1/UI/AdaptivePageScroll.h"
#include "../Ksword5.1/Ksword5.1/UI/GlobalUiBaseStyle.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockManager.h"
#include "../Ksword5.1/Ksword5.1/include/ads/DockWidget.h"
#include "../Ksword5.1/Ksword5.1/theme.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPalette>
#include <QPixmap>
#include <QPoint>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // g_checks / g_failures：已执行的断言数与失败数，main 末尾汇总并决定退出码。
    int g_checks = 0;
    int g_failures = 0;

    // kMaxPageMinimumExtent：包壳后每页 minimumSizeHint 允许的最大边长（壳的 Ignored 让它恰为 0）。
    constexpr int kMaxPageMinimumExtent = 8;

    // kMaxDockMinimumHeight：整个 Dock 最小高度的上限（头部 + 工具栏 + 状态栏 + 边距，约 110px）。
    constexpr int kMaxDockMinimumHeight = 200;

    // kTargetDockWidth：任务目标——Dock 在约 900px 宽时不被横向裁切，所以最小宽度不得超过它。
    constexpr int kTargetDockWidth = 900;

    // Check：记录一条断言；失败时立即打印，便于改前基线直接看到全部失败项。
    // 传入：condition 断言条件；message 失败时的说明。
    void Check(const bool condition, const QString& message)
    {
        ++g_checks;
        if (condition)
        {
            return;
        }
        ++g_failures;
        std::printf("CHECK FAIL: %s\n", message.toUtf8().constData());
        std::fflush(stdout);
    }

    // PumpFor：驱动事件循环 milliseconds 毫秒，让布局、定时器与重绘落定。
    void PumpFor(const int milliseconds)
    {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // ApplyTheme：切换深浅主题并应用完整调色板（与 wpJ6 夹具同一套数值），再装上与生产同源的全局基础样式块。
    // 缺了全局样式块，按钮与下拉框的内边距缺失，最小尺寸会偏小，断言就成了空头支票。
    void ApplyTheme(const bool dark)
    {
        KswordTheme::SetDarkModeEnabled(dark);
        QPalette palette = QApplication::palette();
        if (dark)
        {
            palette.setColor(QPalette::Window, QColor(32, 32, 36));
            palette.setColor(QPalette::WindowText, QColor(230, 230, 232));
            palette.setColor(QPalette::Base, QColor(24, 24, 28));
            palette.setColor(QPalette::AlternateBase, QColor(40, 40, 44));
            palette.setColor(QPalette::Text, QColor(230, 230, 232));
            palette.setColor(QPalette::Button, QColor(44, 44, 48));
            palette.setColor(QPalette::ButtonText, QColor(230, 230, 232));
            palette.setColor(QPalette::Highlight, QColor(64, 128, 222));
            palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
            palette.setColor(QPalette::PlaceholderText, QColor(140, 140, 146));
            palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 124));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 124));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 124));
        }
        else
        {
            palette.setColor(QPalette::Window, QColor(244, 244, 246));
            palette.setColor(QPalette::WindowText, QColor(24, 24, 28));
            palette.setColor(QPalette::Base, QColor(255, 255, 255));
            palette.setColor(QPalette::AlternateBase, QColor(238, 238, 240));
            palette.setColor(QPalette::Text, QColor(24, 24, 28));
            palette.setColor(QPalette::Button, QColor(236, 236, 238));
            palette.setColor(QPalette::ButtonText, QColor(24, 24, 28));
            palette.setColor(QPalette::Highlight, QColor(32, 108, 212));
            palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
            palette.setColor(QPalette::PlaceholderText, QColor(120, 120, 124));
            palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(170, 170, 174));
            palette.setColor(QPalette::Disabled, QPalette::Text, QColor(170, 170, 174));
            palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(170, 170, 174));
        }
        qApp->setPalette(palette);
        qApp->setStyleSheet(ks::ui::BuildGlobalBaseControlStyleBlock());
    }

    // FindDockTabs：取 Dock 自己的 QTabWidget（直接子对象里应恰有一个）。
    QTabWidget* FindDockTabs(MemoryDock& dock)
    {
        const QList<QTabWidget*> tabWidgets =
            dock.findChildren<QTabWidget*>(QString(), Qt::FindDirectChildrenOnly);
        Check(tabWidgets.size() == 1, QStringLiteral("MemoryDock 的直接子 QTabWidget 应恰有一个，实际 %1").arg(tabWidgets.size()));
        return tabWidgets.isEmpty() ? nullptr : tabWidgets.front();
    }

    // FindShell：取页面的内部滚动壳（页面的直接子 QScrollArea，按稳定 objectName 找）；没有壳返回空指针。
    QScrollArea* FindShell(QWidget* const page)
    {
        return page == nullptr ? nullptr : page->findChild<QScrollArea*>(
            QString::fromLatin1(ks::ui::kAdaptivePageScrollObjectName), Qt::FindDirectChildrenOnly);
    }

    // IndexOfDdma：DDMA 页在页签控件里的下标；按类型找，不依赖页签文字（文字会随语言变化）。
    int IndexOfDdma(QTabWidget* const tabs)
    {
        for (int index = 0; tabs != nullptr && index < tabs->count(); ++index)
        {
            if (dynamic_cast<DdmaPage*>(tabs->widget(index)) != nullptr)
            {
                return index;
            }
        }
        return -1;
    }

    // IsAuditPage：是否系统内存审计页。切到它会触发整机快照采集，测试一律不切。
    bool IsAuditPage(QWidget* const page)
    {
        return dynamic_cast<SystemMemoryAuditPage*>(page) != nullptr;
    }

    // TopOf：控件左上角在 dock 坐标系里的纵坐标，用来比较"壳滚动前后位置不变"。
    int TopOf(MemoryDock& dock, const QWidget* const widget)
    {
        return widget == nullptr ? -1 : widget->mapTo(&dock, QPoint(0, 0)).y();
    }

    // ---- 测试 1：页面最小提示 ----

    // TestPageMinimums：打印基线表格，并断言 dock 与每页的最小提示都接近 0。
    void TestPageMinimums()
    {
        MemoryDock dock;
        QTabWidget* const tabs = FindDockTabs(dock);
        if (tabs == nullptr)
        {
            return;
        }

        // 基线：dock 的最小/建议尺寸。改前最小高度约 1200，改后约 110。
        const QSize dockMinimum = dock.minimumSizeHint();
        std::printf(
            "MEMDOCK dock.minimumSizeHint=%dx%d sizeHint=%dx%d tabs=%d\n",
            dockMinimum.width(), dockMinimum.height(), dock.sizeHint().width(), dock.sizeHint().height(), tabs->count());
        Check(
            dockMinimum.height() <= kMaxDockMinimumHeight,
            QStringLiteral("Dock 最小高度应 <= %1，实际 %2（某页的最小高度又传播上来了）").arg(kMaxDockMinimumHeight).arg(dockMinimum.height()));
        Check(
            dockMinimum.width() <= kTargetDockWidth,
            QStringLiteral("Dock 最小宽度应 <= %1（目标：约 900px 宽不被横向裁切），实际 %2（检查工具栏最小宽度贡献者）")
                .arg(kTargetDockWidth).arg(dockMinimum.width()));

        // 页签控件加固：对父布局的最小尺寸贡献为 0。
        Check(
            tabs->sizePolicy().verticalPolicy() == QSizePolicy::Ignored && tabs->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored,
            QStringLiteral("页签控件应经 IsolateMinimumSize 隔离（两个方向 Ignored）"));
        Check(tabs->minimumSize() == QSize(0, 0), QStringLiteral("页签控件显式最小尺寸应为 0x0"));
        Check(tabs->count() >= 13, QStringLiteral("页签数量应 >= 13（含隐藏的旧页与工作台页），实际 %1").arg(tabs->count()));

        // 逐页：打印基线并断言。被隐藏的旧页同样在页面栈里参与最小高度取最大值，所以不跳过。
        int shellLessPages = 0;
        int tallestPage = 0;
        for (int index = 0; index < tabs->count(); ++index)
        {
            QWidget* const page = tabs->widget(index);
            QScrollArea* const shell = FindShell(page);
            const QSize pageMinimum = page->minimumSizeHint();
            tallestPage = qMax(tallestPage, pageMinimum.height());
            std::printf(
                "MEMDOCK page[%d] text=%s tabVisible=%d shell=%d minimumSizeHint=%dx%d\n",
                index, tabs->tabText(index).toUtf8().constData(), tabs->isTabVisible(index) ? 1 : 0,
                shell != nullptr ? 1 : 0, pageMinimum.width(), pageMinimum.height());
            Check(
                pageMinimum.height() <= kMaxPageMinimumExtent && pageMinimum.width() <= kMaxPageMinimumExtent,
                QStringLiteral("页 %1（%2）的最小提示应接近 0，实际 %3x%4——未包壳或壳的 Ignored 被去掉")
                    .arg(index).arg(tabs->tabText(index)).arg(pageMinimum.width()).arg(pageMinimum.height()));

            // 没有壳的页（工作台容器页）不再检查直接子控件。
            if (shell == nullptr)
            {
                ++shellLessPages;
                continue;
            }

            // 页面的直接子控件只有壳：游离在页面上而不在内容里的控件会盖在壳上面。
            QList<QWidget*> strayChildren;
            for (QWidget* const child : page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
            {
                if (child != shell && !child->isWindow())
                {
                    strayChildren.push_back(child);
                }
            }
            QString strayText;
            for (QWidget* const stray : strayChildren)
            {
                strayText += QString::fromLatin1(stray->metaObject()->className()) + QStringLiteral(" ");
            }
            Check(
                strayChildren.isEmpty(),
                QStringLiteral("页 %1（%2）有 %3 个游离在页面上而不在壳里的直接子控件：%4")
                    .arg(index).arg(tabs->tabText(index)).arg(strayChildren.size()).arg(strayText));
        }
        std::printf("MEMDOCK tallest page minimumSizeHint height=%d (before fix expect > 1000, after expect ~0)\n", tallestPage);
        Check(
            shellLessPages <= 1,
            QStringLiteral("没有壳的页最多只能有工作台容器页一个，实际 %1 个").arg(shellLessPages));
    }

    // ---- 测试 2：缩放与滚动 ----

    // TestResizeAndScroll：矮窗口里 DDMA 壳出滚动条且 dock 不被撑高；滚到底后外壳位置不动；高窗口里各页无滚动条。
    void TestResizeAndScroll()
    {
        MemoryDock dock;
        QTabWidget* const tabs = FindDockTabs(dock);
        const int ddmaIndex = IndexOfDdma(tabs);
        Check(ddmaIndex >= 0, QStringLiteral("找不到 DDMA 页"));
        if (tabs == nullptr || ddmaIndex < 0)
        {
            return;
        }

        // 560 高：约等于一个 1080p 屏幕上被其它面板占去一部分之后的 Dock 高度。
        dock.resize(1100, 560);
        dock.show();
        tabs->setCurrentIndex(ddmaIndex);
        PumpFor(300);
        Check(
            dock.size() == QSize(1100, 560),
            QStringLiteral("dock 应能缩到 1100x560，实际 %1x%2").arg(dock.width()).arg(dock.height()));

        QScrollArea* const ddmaShell = FindShell(tabs->widget(ddmaIndex));
        Check(ddmaShell != nullptr, QStringLiteral("DDMA 页应有内部滚动壳"));
        if (ddmaShell == nullptr)
        {
            return;
        }
        Check(
            ddmaShell->verticalScrollBar()->maximum() > 0,
            QStringLiteral("560 高时 DDMA 页的壳应出纵向滚动条，maximum=%1").arg(ddmaShell->verticalScrollBar()->maximum()));

        // 滚到底：标题、页签栏、状态栏的位置不得变化——整页滚走正是用户看到的症状。
        QTabBar* const tabBar = tabs->findChild<QTabBar*>(QString(), Qt::FindDirectChildrenOnly);
        QStatusBar* const statusBar = dock.findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly);
        const QList<QLabel*> directLabels = dock.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        QLabel* const titleLabel = directLabels.isEmpty() ? nullptr : directLabels.front();
        Check(tabBar != nullptr && statusBar != nullptr && titleLabel != nullptr, QStringLiteral("应能找到页签栏、状态栏和标题标签"));
        const int titleTopBefore = TopOf(dock, titleLabel);
        const int tabBarTopBefore = TopOf(dock, tabBar);
        const int statusTopBefore = TopOf(dock, statusBar);
        ddmaShell->verticalScrollBar()->setValue(ddmaShell->verticalScrollBar()->maximum());
        PumpFor(100);
        Check(TopOf(dock, titleLabel) == titleTopBefore, QStringLiteral("DDMA 壳滚到底后标题标签被移动了"));
        Check(TopOf(dock, tabBar) == tabBarTopBefore, QStringLiteral("DDMA 壳滚到底后页签栏被移动了"));
        Check(TopOf(dock, statusBar) == statusTopBefore, QStringLiteral("DDMA 壳滚到底后状态栏被移动了"));
        Check(
            statusBar != nullptr && statusBar->isVisible() && statusBar->geometry().bottom() <= dock.height(),
            QStringLiteral("DDMA 壳滚到底后状态栏应仍在 dock 之内且可见"));
        Check(titleLabel != nullptr && titleLabel->isVisible() && titleLabel->geometry().top() >= 0,
            QStringLiteral("DDMA 壳滚到底后标题标签应仍可见"));

        // 1700 高：内容放得下，逐页（除审计页、工作台页）壳都不得出滚动条。
        dock.resize(1100, 1700);
        PumpFor(300);
        for (int index = 0; index < tabs->count(); ++index)
        {
            QWidget* const page = tabs->widget(index);
            QScrollArea* const shell = FindShell(page);
            // 审计页切入会触发整机快照；工作台页（无壳）切入会走 ConfigureShared 写地址簿；隐藏页签不能被选中。
            if (shell == nullptr || IsAuditPage(page) || !tabs->isTabVisible(index))
            {
                continue;
            }
            tabs->setCurrentIndex(index);
            PumpFor(120);
            Check(
                shell->verticalScrollBar()->maximum() == 0,
                QStringLiteral("1700 高时页 %1（%2）的壳不应出纵向滚动条，maximum=%3")
                    .arg(index).arg(tabs->tabText(index)).arg(shell->verticalScrollBar()->maximum()));
        }

        // 回到 DDMA：内容恰好铺满视口（widgetResizable 的可观测后果）。
        tabs->setCurrentIndex(ddmaIndex);
        PumpFor(150);
        Check(
            ddmaShell->widget() != nullptr && ddmaShell->widget()->height() == ddmaShell->viewport()->height(),
            QStringLiteral("1700 高时 DDMA 内容高度应等于视口高度，内容 %1 视口 %2")
                .arg(ddmaShell->widget() != nullptr ? ddmaShell->widget()->height() : -1).arg(ddmaShell->viewport()->height()));
        dock.hide();
    }

    // ---- 测试 3：ADS 承载 ----

    // kAdsScrollAreaName：ADS 自动包的外层滚动区的 objectName。
    const char* const kAdsScrollAreaName = "dockWidgetScrollArea";

    // BuildAdsHost：把一个真实 MemoryDock 装进真实 ADS 的 CDockManager 与 CDockWidget。
    // 传入：host 宿主窗口（本函数在其上建布局）；insertMode 挂载模式；memoryDockOut 返回 dock 指针；dockWidgetOut 返回 ADS 的 dock 控件。
    void BuildAdsHost(
        QWidget& host,
        const ads::CDockWidget::eInsertMode insertMode,
        MemoryDock*& memoryDockOut,
        ads::CDockWidget*& dockWidgetOut)
    {
        auto* hostLayout = new QVBoxLayout(&host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        auto* manager = new ads::CDockManager(&host);
        hostLayout->addWidget(manager);

        // 与 MainWindow 同形态：先建 ADS dock 控件，再把 MemoryDock 作为内容挂进去。
        dockWidgetOut = new ads::CDockWidget(manager, QStringLiteral("memory"));
        dockWidgetOut->setObjectName(QStringLiteral("ksDock_memory"));
        memoryDockOut = new MemoryDock();
        memoryDockOut->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        dockWidgetOut->setWidget(memoryDockOut, insertMode);
        manager->addDockWidget(ads::CenterDockWidgetArea, dockWidgetOut);
    }

    // TestAdsHosting：ForceNoScrollArea 下没有外层滚动区，dock 不超出宿主；对照组证明断言不是空转。
    void TestAdsHosting()
    {
        // DisableStylesheet 必须在创建 CDockManager 之前设置（与 MainWindow 同一约定）。
        ads::CDockManager::setConfigFlag(ads::CDockManager::DisableStylesheet, true);

        // 对照组：AutoScrollArea 下 ADS 会自动包一层名为 dockWidgetScrollArea 的 QScrollArea，
        // 若这里找不到，说明上面的 objectName 不对，下面"没有外层滚动区"的断言就是空转。
        {
            QWidget controlHost;
            MemoryDock* controlDock = nullptr;
            ads::CDockWidget* controlDockWidget = nullptr;
            BuildAdsHost(controlHost, ads::CDockWidget::AutoScrollArea, controlDock, controlDockWidget);
            controlHost.resize(1100, 560);
            controlHost.show();
            PumpFor(400);
            const int controlScrollAreas =
                controlDockWidget->findChildren<QScrollArea*>(QString::fromLatin1(kAdsScrollAreaName)).size();
            Check(
                controlScrollAreas == 1,
                QStringLiteral("对照组 AutoScrollArea 下应恰有一个 dockWidgetScrollArea，实际 %1（夹具不能区分是否套了外层滚动区）")
                    .arg(controlScrollAreas));
            controlHost.hide();
        }

        // 实验组：ForceNoScrollArea。
        QWidget host;
        MemoryDock* memoryDock = nullptr;
        ads::CDockWidget* dockWidget = nullptr;
        BuildAdsHost(host, ads::CDockWidget::ForceNoScrollArea, memoryDock, dockWidget);
        host.resize(1100, 560);
        host.show();
        PumpFor(400);

        Check(
            dockWidget->findChildren<QScrollArea*>(QString::fromLatin1(kAdsScrollAreaName)).isEmpty(),
            QStringLiteral("ForceNoScrollArea 下 Dock 与宿主之间不应有 dockWidgetScrollArea"));
        Check(
            memoryDock->height() <= host.height(),
            QStringLiteral("MemoryDock 高度 %1 不应超过宿主高度 %2").arg(memoryDock->height()).arg(host.height()));
        QStatusBar* const statusBar = memoryDock->findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly);
        Check(
            statusBar != nullptr && statusBar->mapTo(&host, QPoint(0, statusBar->height() - 1)).y() < host.height(),
            QStringLiteral("状态栏应完整落在宿主之内（Dock 不能被撑高到宿主之外）"));

        // 滚动 DDMA 壳到底：标题、页签栏、状态栏的位置不变。
        QTabWidget* const tabs = FindDockTabs(*memoryDock);
        const int ddmaIndex = IndexOfDdma(tabs);
        if (tabs == nullptr || ddmaIndex < 0)
        {
            return;
        }
        tabs->setCurrentIndex(ddmaIndex);
        PumpFor(300);
        QScrollArea* const shell = FindShell(tabs->widget(ddmaIndex));
        Check(shell != nullptr && shell->verticalScrollBar()->maximum() > 0, QStringLiteral("ADS 内 560 高时 DDMA 壳应出纵向滚动条"));
        if (shell == nullptr)
        {
            return;
        }
        QTabBar* const tabBar = tabs->findChild<QTabBar*>(QString(), Qt::FindDirectChildrenOnly);
        const int tabBarTopBefore = TopOf(*memoryDock, tabBar);
        const int statusTopBefore = TopOf(*memoryDock, statusBar);
        shell->verticalScrollBar()->setValue(shell->verticalScrollBar()->maximum());
        PumpFor(100);
        Check(TopOf(*memoryDock, tabBar) == tabBarTopBefore, QStringLiteral("ADS 内 DDMA 壳滚到底后页签栏被移动了"));
        Check(TopOf(*memoryDock, statusBar) == statusTopBefore, QStringLiteral("ADS 内 DDMA 壳滚到底后状态栏被移动了"));
        host.hide();
    }

    // ---- 测试 4：透明 ----

    // RenderDockOverBackdrop：把真实 MemoryDock 放在绿色背板上，dock 的调色板 Window 角色设成洋红色，
    // 切到 DDMA 页后抓背板整图。dock 子树里任何自填背景的控件（壳的内容容器或视口）都会画成洋红实色块；
    // 全部不自填时，页内空白处透出背板绿色。必须抓背板而不是 dock：透明 dock 自己 grab() 得到的是透明像素。
    // 传入：useStyleSheet 为真时给 dock 挂与 MainWindow 同形状的本地透明样式；samplePointOut 返回页内空白处采样点。
    // 传出：背板渲染图（统一成 ARGB32）。
    QImage RenderDockOverBackdrop(const bool useStyleSheet, QPoint* const samplePointOut)
    {
        QWidget backdrop;
        backdrop.resize(1100, 700);
        QPalette backdropPalette = backdrop.palette();
        backdropPalette.setColor(QPalette::Window, QColor(0, 200, 0));
        backdrop.setPalette(backdropPalette);
        backdrop.setAutoFillBackground(true);

        auto* backdropLayout = new QVBoxLayout(&backdrop);
        backdropLayout->setContentsMargins(0, 0, 0, 0);
        auto* dock = new MemoryDock(&backdrop);
        backdropLayout->addWidget(dock);

        // dock 子树的 Window 调色板角色设成洋红色。
        QPalette dockPalette = dock->palette();
        dockPalette.setColor(QPalette::Window, QColor(255, 0, 255));
        dock->setPalette(dockPalette);
        if (useStyleSheet)
        {
            dock->setStyleSheet(QStringLiteral("QWidget{background:transparent;background-color:transparent;}"));
        }

        backdrop.show();
        QTabWidget* const tabs = FindDockTabs(*dock);
        const int ddmaIndex = IndexOfDdma(tabs);
        if (tabs == nullptr || ddmaIndex < 0)
        {
            return QImage();
        }
        tabs->setCurrentIndex(ddmaIndex);
        PumpFor(400);

        // 采样点取页面左上角内侧 2px：那里落在内容容器 6px 的边距里，没有任何子控件。
        if (samplePointOut != nullptr)
        {
            *samplePointOut = tabs->widget(ddmaIndex)->mapTo(&backdrop, QPoint(2, 2));
        }
        const QImage image = backdrop.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        backdrop.hide();
        return image;
    }

    // TestTransparency：浅色/深色 × 无样式/带透明样式，页内空白处都必须透出背板绿色。
    void TestTransparency()
    {
        for (const bool dark : {false, true})
        {
            // 先切主题再构造控件，与夹具惯例一致。
            ApplyTheme(dark);
            for (const bool withStyleSheet : {false, true})
            {
                QPoint samplePoint;
                const QImage image = RenderDockOverBackdrop(withStyleSheet, &samplePoint);
                const QString label = QStringLiteral("%1/%2")
                    .arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))
                    .arg(withStyleSheet ? QStringLiteral("with-qss") : QStringLiteral("no-qss"));
                Check(
                    !image.isNull() && image.rect().contains(samplePoint),
                    QStringLiteral("%1：背板渲染图为空或采样点越界 (%2,%3)").arg(label).arg(samplePoint.x()).arg(samplePoint.y()));
                if (image.isNull() || !image.rect().contains(samplePoint))
                {
                    continue;
                }
                const QColor sampled = image.pixelColor(samplePoint);
                const bool isBackdropGreen =
                    std::abs(sampled.red() - 0) <= 3 && std::abs(sampled.green() - 200) <= 3 && std::abs(sampled.blue() - 0) <= 3;
                Check(
                    isBackdropGreen,
                    QStringLiteral("%1：DDMA 页空白处应透出背板绿色 (0,200,0)，实际 (%2,%3,%4)——壳或内容容器画了实色块")
                        .arg(label).arg(sampled.red()).arg(sampled.green()).arg(sampled.blue()));
            }
        }
        ApplyTheme(false);
    }

    // ---- 测试 5：内嵌形态 ----

    // TestEmbeddedInProcessDetail：仿 ProcessDetailWindow::attachEmbeddedDockToTabLayout——dock 设 setMinimumSize(0,0)
    // 与 Ignored 放进布局，并调用 setProcessDetailMemoryScope。宿主缩放时 dock 必须始终等于宿主大小。
    void TestEmbeddedInProcessDetail()
    {
        QWidget host;
        auto* hostLayout = new QVBoxLayout(&host);
        hostLayout->setContentsMargins(0, 0, 0, 0);
        auto* dock = new MemoryDock(&host);
        dock->setMinimumSize(0, 0);
        dock->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        hostLayout->addWidget(dock, 1);
        dock->setProcessDetailMemoryScope();

        host.resize(900, 500);
        host.show();
        PumpFor(300);
        Check(
            dock->size() == host.size(),
            QStringLiteral("内嵌 dock 应等于宿主 900x500，实际 %1x%2").arg(dock->width()).arg(dock->height()));

        // 再缩小一档：包壳后的最小提示更小，Ignored 加固仍应让 dock 跟着宿主走。
        host.resize(700, 320);
        PumpFor(200);
        Check(
            dock->size() == host.size(),
            QStringLiteral("宿主缩到 700x320 后内嵌 dock 应跟随，实际 %1x%2").arg(dock->width()).arg(dock->height()));
        host.hide();
    }
}

int main(int argc, char** argv)
{
    // 基线模式：只打印，不让失败影响退出码（用于对比改前改后的每页最小提示）。
    bool baselineOnly = false;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--baseline") == 0)
        {
            baselineOnly = true;
        }
    }

    // 测试专用的组织/应用名与测试模式标准路径：绝不碰用户真实设置与 AppLocalData。
    QCoreApplication::setOrganizationName(QStringLiteral("KSwordMemoryDockLayoutFixture"));
    QCoreApplication::setApplicationName(QStringLiteral("memory_dock_layout_tests"));
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);

    // QSettings 重定向到 exe 同目录下的 INI，启动时清空，保证每次都从默认值开始。
    {
        const QString settingsDirectory = QCoreApplication::applicationDirPath() + QStringLiteral("/settings");
        QDir(settingsDirectory).removeRecursively();
        QDir().mkpath(settingsDirectory);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory);
    }

    // 字体：offscreen 平台默认没有系统字体，不显式加载会让中文全部变成缺字方块，文字宽度也会失真。
    const int chineseFontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    if (chineseFontId >= 0)
    {
        QFont applicationFont(QStringLiteral("Microsoft YaHei UI"));
        applicationFont.setPointSize(9);
        application.setFont(applicationFont);
    }
    else
    {
        std::printf("memory_dock_layout_tests: WARNING msyh.ttc not loaded, text widths may be underestimated\n");
    }

    // 初始主题与全局样式块（见 ApplyTheme 的注释）。
    ApplyTheme(false);

    TestPageMinimums();
    TestResizeAndScroll();
    TestAdsHosting();
    TestTransparency();
    TestEmbeddedInProcessDetail();

    std::printf("memory_dock_layout_tests: %d checks, %d failures%s\n", g_checks, g_failures, baselineOnly ? " (baseline mode, exit code forced to 0)" : "");
    return (baselineOnly || g_failures == 0) ? 0 : 1;
}
