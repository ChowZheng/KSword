// ============================================================
// wpJ6_tests.Nav.cpp
// 作用：地址条"切换并跳转"（N4，不静默切换范围）；前进后退栈 64 项；
//       Wave 3 复核新增：身份变化后后退栈不留幽灵记录（修复缺陷 4）、右键
//       "写入/还原此处 int3"按状态二选一且结果可见（修复缺陷 2）。
// ============================================================

#include "wpJ6_common.h"

#include <QAction>
#include <QMenu>

namespace wpj6_test
{
    // TestNeedsScopeSwitchShowsRerouteButtonAndJumpsOnClick：进程范围下跳转到
    // 内核半区地址应返回 NeedsScopeSwitch、不静默切换范围，"切换并跳转"按钮
    // 出现；点击后才真正切换并跳转。
    void TestNeedsScopeSwitchShowsRerouteButtonAndJumpsOnClick()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* reroute = harness.view->rerouteButtonForTest();
        WPJ6_CHECK(reroute != nullptr);
        if (reroute != nullptr)
        {
            WPJ6_CHECK(reroute->isHidden());
        }

        ks::ui::NavRequest request;
        request.scope = ksword::memwb::Scope::ProcessVirtual;
        request.address = 0xFFFFF78000001000ULL;
        const auto status = harness.view->openAt(request);
        WPJ6_CHECK(status == ks::ui::NavStatus::NeedsScopeSwitch);
        WPJ6_CHECK_NOTE(
            harness.view->target().session().scope == ksword::memwb::Scope::ProcessVirtual,
            QStringLiteral("NeedsScopeSwitch 不应静默切换范围"));
        WPJ6_CHECK(reroute != nullptr && !reroute->isHidden());

        if (reroute != nullptr)
        {
            emit reroute->clicked();
        }
        WPJ6_CHECK(harness.view->target().session().scope == ksword::memwb::Scope::KernelVirtual);
        WPJ6_CHECK(reroute != nullptr && reroute->isHidden());
    }

    // TestBackForwardStackCapAt64：连续跳转 70 次不同地址，后退栈容量应封顶
    // 在 64 项（最旧的被丢弃），后退到底之后再前进应该能回到最近跳转的位置。
    void TestBackForwardStackCapAt64()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x10ULL));

        for (int i = 0; i < 70; ++i)
        {
            ks::ui::NavRequest request;
            request.scope = ksword::memwb::Scope::ProcessVirtual;
            request.address = 0x10ULL + static_cast<std::uint64_t>(i);
            const auto status = harness.view->openAt(request);
            WPJ6_CHECK(status == ks::ui::NavStatus::Ok);
        }
        WPJ6_CHECK_NOTE(
            harness.view->backStackForTest().size() == 64U,
            QStringLiteral("后退栈应封顶 64 项，实际 %1").arg(harness.view->backStackForTest().size()));

        // 点击后退按钮：当前地址（最后一次跳转的目标）应压入前进栈，插入点
        // 回到后退栈的最后一项。
        const std::uint64_t currentAddress = pane->insertionAddress();
        const std::uint64_t expectedBack = harness.view->backStackForTest().back();
        // 独立核对栈顶确实是"倒数第二次跳转的真实目标地址"（0x10+68），不能只
        // 拿 expectedBack 跟自己比——那样如果 pushBackStackEntry 把错误的值
        // （例如恒为 0）压进了栈，下面两处比较会拿同一个错误值互相对照，
        // 测不出问题。连续跳转的地址是已知的等差序列，这里用它独立算出
        // 应该存在栈顶的真值。
        WPJ6_CHECK_NOTE(
            expectedBack == currentAddress - 1ULL,
            QStringLiteral("后退栈顶应是上一次跳转的真实地址 0x%1，实际 0x%2")
                .arg(currentAddress - 1ULL, 0, 16)
                .arg(expectedBack, 0, 16));
        auto* backButton = harness.view->backButtonForTest();
        WPJ6_CHECK(backButton != nullptr);
        if (backButton != nullptr)
        {
            emit backButton->clicked();
        }
        WPJ6_CHECK(pane->insertionAddress() == expectedBack);
        WPJ6_CHECK_NOTE(
            !harness.view->forwardStackForTest().empty() &&
                harness.view->forwardStackForTest().back() == currentAddress,
            QStringLiteral("后退应把原地址压入前进栈"));

        // 再点前进：应该回到后退之前的地址。
        auto* forwardButton = harness.view->forwardButtonForTest();
        WPJ6_CHECK(forwardButton != nullptr);
        if (forwardButton != nullptr)
        {
            emit forwardButton->clicked();
        }
        WPJ6_CHECK(pane->insertionAddress() == currentAddress);
    }

    // TestForwardClickNoOpWhenStackEmpty / TestBackClickNoOpWhenStackEmpty：
    // 前进栈/后退栈为空时点对应按钮必须是安全的空操作——这是
    // onGoForwardRequested/onGoBackRequested 用 "||" 而不是 "&&" 组合"栈空
    // 或画布为空"两个早退条件这条不变式最常见的真实触发场景（刚附加、还
    // 没做过任何导航就点"前进"/"后退"）；如果误改成 "&&"，栈空但画布非空
    // 时会从空容器 back()/pop_back()，是未定义行为。TestBackForwardStackCapAt64
    // 只在两个栈都已经有内容之后点按钮，测不出"恰好只有一个栈为空"这个分支。
    void TestForwardClickNoOpWhenStackEmpty()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x60ULL));
        pane->jumpTo(0x60ULL);

        WPJ6_CHECK(harness.view->forwardStackForTest().empty());
        const std::uint64_t before = pane->insertionAddress();

        auto* forwardButton = harness.view->forwardButtonForTest();
        WPJ6_CHECK(forwardButton != nullptr);
        if (forwardButton != nullptr)
        {
            emit forwardButton->clicked();
        }
        WPJ6_CHECK_NOTE(
            pane->insertionAddress() == before,
            QStringLiteral("前进栈为空时点前进不应改变当前位置"));
    }

    void TestBackClickNoOpWhenStackEmpty()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x64ULL));
        pane->jumpTo(0x64ULL);

        WPJ6_CHECK(harness.view->backStackForTest().empty());
        const std::uint64_t before = pane->insertionAddress();

        auto* backButton = harness.view->backButtonForTest();
        WPJ6_CHECK(backButton != nullptr);
        if (backButton != nullptr)
        {
            emit backButton->clicked();
        }
        WPJ6_CHECK_NOTE(
            pane->insertionAddress() == before,
            QStringLiteral("后退栈为空时点后退不应改变当前位置"));
    }

    // TestIdentityChangeClearsBackStackWithoutGhost（修复缺陷 4，并入审核报告
    // wpJ6/wave3 的 P5 探针）：同目标内先攒几条后退栈记录，再触发一次身份
    // 变化类导航（"切换并跳转"到内核范围）；修复前的缺陷是 openAt 在
    // requestIdentity 之后才读 hexPane_->insertionAddress() 当"旧地址"压栈，
    // 读到的已经是身份变化重置后的新地址空间起点（幽灵地址），导致
    // handleIdentityChange 刚清空的栈又多出一条不属于新目标的记录。
    void TestIdentityChangeClearsBackStackWithoutGhost()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        WPJ6_CHECK(WaitForStageable(harness.view->hexPaneForTest(), 0x80ULL));

        ks::ui::NavRequest first;
        first.scope = ksword::memwb::Scope::ProcessVirtual;
        first.address = 0x80ULL;
        WPJ6_CHECK(harness.view->openAt(first) == ks::ui::NavStatus::Ok);

        ks::ui::NavRequest second;
        second.scope = ksword::memwb::Scope::ProcessVirtual;
        second.address = 0x90ULL;
        WPJ6_CHECK(harness.view->openAt(second) == ks::ui::NavStatus::Ok);
        WPJ6_CHECK_NOTE(
            !harness.view->backStackForTest().empty(),
            QStringLiteral("同目标内跳转后应有后退栈记录（后续身份变化要清空它）"));

        ks::ui::NavRequest kernelRequest;
        kernelRequest.scope = ksword::memwb::Scope::ProcessVirtual;
        kernelRequest.address = 0xFFFFF78000002000ULL;
        WPJ6_CHECK(harness.view->openAt(kernelRequest) == ks::ui::NavStatus::NeedsScopeSwitch);

        auto* reroute = harness.view->rerouteButtonForTest();
        WPJ6_CHECK(reroute != nullptr);
        if (reroute != nullptr)
        {
            emit reroute->clicked();
        }
        WPJ6_CHECK(harness.view->target().session().scope == ksword::memwb::Scope::KernelVirtual);

        WPJ6_CHECK_NOTE(
            harness.view->backStackForTest().empty(),
            QStringLiteral("身份变化后后退栈应清空，实际剩 %1 项（幽灵记录：0x%2）")
                .arg(harness.view->backStackForTest().size())
                .arg(harness.view->backStackForTest().empty() ? 0ULL : harness.view->backStackForTest().back(), 0, 16));
        WPJ6_CHECK(harness.view->forwardStackForTest().empty());
    }

    // TestInt3ContextMenuTogglesInstallAndRestore（修复缺陷 2）：右键"写入/
    // 还原此处 int3"必须按当前状态二选一（原实现永远只 Install、从不
    // Restore，结果被直接丢弃），菜单文字随状态变化，结果经状态条可见。
    // 用 HexCanvas::buildContextMenu 直接构造菜单（与 wpJ5_tests.Wiring.cpp
    // 的 TestEditRejectedAndContextMenuForwarding 同一手法），走的是生产代码
    // 的真实信号链（WorkbenchHexPane::contextMenuAboutToShow →
    // MemoryWorkbenchView::onHexPaneContextMenuAboutToShow），不是绕过接线
    // 直接调用私有方法。
    void TestInt3ContextMenuTogglesInstallAndRestore()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        auto* statusBar = harness.view->statusBarForTest();
        WPJ6_CHECK(statusBar != nullptr);
        const std::uint64_t address = 0xB0ULL;
        WPJ6_CHECK(WaitForStageable(pane, address));

        auto findToggleAction = [](QMenu* menu) -> QAction* {
            if (menu == nullptr)
            {
                return nullptr;
            }
            for (auto* action : menu->actions())
            {
                if (action->text().contains(QStringLiteral("int3")))
                {
                    return action;
                }
            }
            return nullptr;
        };

        // 第一次：当前目标在该地址没有补丁 -> 菜单项应显示"写入"。
        {
            QMenu* menu = pane->canvas()->buildContextMenu(address, true);
            WPJ6_CHECK(menu != nullptr);
            QAction* toggle = findToggleAction(menu);
            WPJ6_CHECK(toggle != nullptr);
            if (toggle != nullptr)
            {
                WPJ6_CHECK_NOTE(
                    toggle->text() == QStringLiteral("写入 int3 补丁"),
                    QStringLiteral("首次对该地址应显示「写入」，实际文字=%1").arg(toggle->text()));
                emit toggle->triggered();
            }
            delete menu;
        }
        WPJ6_CHECK_NOTE(
            statusBar != nullptr && statusBar->summaryText().contains(QStringLiteral("已写入 int3")),
            QStringLiteral("写入成功后状态条应可见反馈，实际=%1")
                .arg(statusBar != nullptr ? statusBar->summaryText() : QString()));

        // 第二次：该地址已有待还原条目 -> 菜单项应变成"还原"，触发后真的调用
        // Restore（而不是原实现那样永远 Install、对已安装地址报 Duplicate）。
        {
            QMenu* menu = pane->canvas()->buildContextMenu(address, true);
            WPJ6_CHECK(menu != nullptr);
            QAction* toggle = findToggleAction(menu);
            WPJ6_CHECK(toggle != nullptr);
            if (toggle != nullptr)
            {
                WPJ6_CHECK_NOTE(
                    toggle->text() == QStringLiteral("还原 int3 补丁"),
                    QStringLiteral("已有待还原条目时应显示「还原」，实际文字=%1").arg(toggle->text()));
                emit toggle->triggered();
            }
            delete menu;
        }
        WPJ6_CHECK_NOTE(
            statusBar != nullptr && statusBar->summaryText().contains(QStringLiteral("已还原")),
            QStringLiteral("还原成功后状态条应可见反馈，实际=%1")
                .arg(statusBar != nullptr ? statusBar->summaryText() : QString()));

        // 第三次：已经还原，账本里不再有该条目 -> 菜单项应回到"写入"。
        {
            QMenu* menu = pane->canvas()->buildContextMenu(address, true);
            QAction* toggle = findToggleAction(menu);
            WPJ6_CHECK_NOTE(
                toggle != nullptr && toggle->text() == QStringLiteral("写入 int3 补丁"),
                QStringLiteral("还原之后再次打开菜单应回到「写入」"));
            delete menu;
        }
    }

    void RunNavTests()
    {
        TestNeedsScopeSwitchShowsRerouteButtonAndJumpsOnClick();
        TestBackForwardStackCapAt64();
        TestForwardClickNoOpWhenStackEmpty();
        TestBackClickNoOpWhenStackEmpty();
        TestIdentityChangeClearsBackStackWithoutGhost();
        TestInt3ContextMenuTogglesInstallAndRestore();
    }
}
