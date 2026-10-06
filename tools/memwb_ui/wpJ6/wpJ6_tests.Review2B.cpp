// ============================================================
// wpJ6_tests.Review2B.cpp
// 作用：MemoryWorkbenchView 第二轮独立复核给出的补测（第二批，原名 proposed-tests2）。
//       针对"夹具完全没有断言"的行为：侧栏自动折叠阈值与展开钮可见性（T10）、内嵌模式
//       侧栏恒隐藏（T11）、Ctrl+I 手动覆盖（T12）、视图自身 minimumSizeHint（T13）、
//       返回栈与"切换并跳转"钮（T14）、int3 菜单项各分支（T15）、离开守卫正文参数与
//       应用失败拒绝离开（T16）。复核者在变异下逐条验证过。
// 入口：RunReview2TestsB（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QMenu>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace wpj6_test
{
    namespace
    {
        QShortcut* FindShortcut(QWidget* view, const QString& key)
        {
            for (auto* shortcut : view->findChildren<QShortcut*>())
            {
                if (shortcut->key() == QKeySequence(key))
                {
                    return shortcut;
                }
            }
            return nullptr;
        }

        QToolButton* FindExpandButton(QWidget* view)
        {
            for (auto* button : view->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
            {
                if (button->toolTip().contains(QStringLiteral("展开侧栏")))
                {
                    return button;
                }
            }
            return nullptr;
        }

        QWidget* SidebarOf(ks::ui::MemoryWorkbenchView* view)
        {
            auto* splitter = view->mainSplitterForTest();
            return (splitter != nullptr && splitter->count() == 2) ? splitter->widget(1) : nullptr;
        }

        void ResizeAndPump(QWidget* view, const int width)
        {
            view->resize(width, 700);
            PumpFor(60);
        }

        QString ToggleInt3At(ks::ui::MemoryWorkbenchView& view, const std::uint64_t address)
        {
            QMenu* menu = view.hexPaneForTest()->canvas()->buildContextMenu(address, true);
            QString label;
            if (menu != nullptr)
            {
                for (auto* action : menu->actions())
                {
                    if (action->text().contains(QStringLiteral("int3")))
                    {
                        label = action->text();
                        emit action->triggered();
                        break;
                    }
                }
            }
            delete menu;
            return label;
        }

        void ClearInt3AndByte(const std::uint64_t address)
        {
            auto& backend = ConfigureSharedOnce();
            auto& int3 = ks::ui::WorkbenchShared::Instance().Int3();
            const auto entries = int3.Entries();
            for (const auto& entry : entries)
            {
                int3.Discard(entry.id);
            }
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            backend.backing->bytes[address - backend.backing->base] = 0x00;
        }

        // T10（杀 c01/c02/c03/c04）：侧栏自动折叠的阈值 760 两侧各取一点，展开钮可见性随状态同步。
        void TestSidebarThresholdAndExpandButton()
        {
            Harness h;
            h.AttachProcess(6101);
            PumpUntil([]() { return true; }, 10);
            auto* view = h.view.get();
            view->resize(900, 700);
            view->show();
            PumpFor(100);
            auto* sidebar = SidebarOf(view);
            auto* expand = FindExpandButton(view);
            WPJ6_CHECK(sidebar != nullptr && expand != nullptr);
            if (sidebar == nullptr || expand == nullptr)
            {
                return;
            }
            WPJ6_CHECK_NOTE(!sidebar->isHidden() && expand->isHidden(), QStringLiteral("900 宽：侧栏展开、展开钮隐藏"));
            ResizeAndPump(view, 780);
            WPJ6_CHECK_NOTE(!sidebar->isHidden() && expand->isHidden(), QStringLiteral("780 宽（>=760）：侧栏仍应展开"));
            ResizeAndPump(view, 740);
            WPJ6_CHECK_NOTE(sidebar->isHidden() && !expand->isHidden(), QStringLiteral("740 宽（<760）：侧栏应折叠且展开钮可见"));
            ResizeAndPump(view, 780);
            WPJ6_CHECK_NOTE(!sidebar->isHidden() && expand->isHidden(), QStringLiteral("放宽回 780：应自动展开、展开钮隐藏"));
            // 手动折叠（宽屏下）：展开钮应出现；再手动展开：展开钮应消失。
            auto* toggle = FindShortcut(view, QStringLiteral("Ctrl+Shift+B"));
            WPJ6_CHECK(toggle != nullptr);
            if (toggle != nullptr)
            {
                emit toggle->activated();
                PumpFor(60);
                WPJ6_CHECK_NOTE(sidebar->isHidden() && !expand->isHidden(), QStringLiteral("手动折叠后展开钮应可见"));
                emit toggle->activated();
                PumpFor(60);
                WPJ6_CHECK_NOTE(!sidebar->isHidden() && expand->isHidden(), QStringLiteral("手动展开后展开钮应隐藏"));
            }
            view->hide();
        }

        // T11（杀 c05/c06）：内嵌模式下无论宽窄、无论快捷键，侧栏与展开钮都不应出现。
        void TestEmbeddedNeverShowsSidebar()
        {
            Harness h;
            h.AttachProcess(6102);
            PumpUntil([]() { return true; }, 10);
            auto* view = h.view.get();
            view->setEmbeddedProcessMode(true);
            view->resize(900, 700);
            view->show();
            PumpFor(100);
            auto* sidebar = SidebarOf(view);
            auto* expand = FindExpandButton(view);
            WPJ6_CHECK(sidebar != nullptr && expand != nullptr);
            if (sidebar == nullptr || expand == nullptr)
            {
                return;
            }
            WPJ6_CHECK_NOTE(sidebar->isHidden(), QStringLiteral("内嵌模式 900 宽：侧栏必须保持隐藏（自动折叠逻辑不得把它放出来）"));
            WPJ6_CHECK_NOTE(expand->isHidden(), QStringLiteral("内嵌模式：展开钮不应出现"));
            auto* toggle = FindShortcut(view, QStringLiteral("Ctrl+Shift+B"));
            if (toggle != nullptr)
            {
                emit toggle->activated();
                PumpFor(60);
                WPJ6_CHECK_NOTE(sidebar->isHidden(), QStringLiteral("内嵌模式：Ctrl+Shift+B 不得展开侧栏"));
            }
            view->hide();
        }

        // T12（杀 c07/c08）：Ctrl+I 手动显示解释器之后，窗口继续变窄也不应被自动隐藏逻辑收回。
        void TestInspectorUserOverrideRespected()
        {
            Harness h;
            h.AttachProcess(6103);
            PumpUntil([]() { return true; }, 10);
            auto* view = h.view.get();
            view->resize(420, 700);
            view->show();
            PumpFor(100);
            auto* inspector = view->hexPaneForTest()->inspector();
            WPJ6_CHECK(inspector != nullptr);
            if (inspector == nullptr)
            {
                return;
            }
            WPJ6_CHECK_NOTE(inspector->isHidden(), QStringLiteral("420 宽：解释器面板应被自动隐藏（前置条件）"));
            auto* toggle = FindShortcut(view, QStringLiteral("Ctrl+I"));
            WPJ6_CHECK(toggle != nullptr);
            if (toggle == nullptr)
            {
                return;
            }
            emit toggle->activated();
            PumpFor(60);
            WPJ6_CHECK_NOTE(!inspector->isHidden(), QStringLiteral("Ctrl+I 应把解释器面板显示出来"));
            ResizeAndPump(view, 380);
            WPJ6_CHECK_NOTE(!inspector->isHidden(), QStringLiteral("用户手动显示后，继续收窄不应被自动隐藏逻辑收回"));
            view->hide();
        }

        // T13（杀 c10）：视图自己的 minimumSizeHint 必须很小——生产里它是宿主布局里的子控件，
        // 宿主问的就是这个值；夹具把它当顶层窗口展示，窗口 resize 不读它，视图级几何断言抓不到。
        void TestViewMinimumSizeHintSmallWhenEmbeddedInHostLayout()
        {
            Harness h;
            h.AttachProcess(6104);
            PumpUntil([]() { return true; }, 10);
            WPJ6_CHECK_NOTE(
                h.view->minimumSizeHint().width() <= 100,
                QStringLiteral("视图 minimumSizeHint 宽 %1 应很小").arg(h.view->minimumSizeHint().width()));
            QWidget host;
            auto* layout = new QVBoxLayout(&host);
            layout->setContentsMargins(0, 0, 0, 0);
            h.view->setParent(&host);
            layout->addWidget(h.view.get());
            WPJ6_CHECK_NOTE(
                host.minimumSizeHint().width() <= 100,
                QStringLiteral("放进宿主布局后，宿主 minimumSizeHint 宽 %1 不应被视图钉在一个很宽的下限").arg(host.minimumSizeHint().width()));
            h.view->setParent(nullptr);
        }

        // T14（杀 c24/c27/c28）：后退栈记录"向后"跳转；Ok 路径隐藏过期的"切换并跳转"钮；点"切换并跳转"后真的落在原请求地址。
        void TestNavRegressions()
        {
            Harness h;
            h.AttachProcess(6105);
            PumpUntil([]() { return true; }, 10);
            auto* pane = h.view->hexPaneForTest();
            WPJ6_CHECK(WaitForStageable(pane, 0x10ULL));
            const auto open = [&](const std::uint64_t address) {
                ks::ui::NavRequest request;
                request.address = address;
                return h.view->openAt(request);
            };
            WPJ6_CHECK(open(0x30ULL) == ks::ui::NavStatus::Ok);
            WPJ6_CHECK(open(0x10ULL) == ks::ui::NavStatus::Ok);
            WPJ6_CHECK_NOTE(
                !h.view->backStackForTest().empty() && h.view->backStackForTest().back() == 0x30ULL,
                QStringLiteral("从 0x30 向后跳到 0x10：后退栈顶应是 0x30"));

            auto* reroute = h.view->rerouteButtonForTest();
            WPJ6_CHECK(open(0xFFFFF78000003000ULL) == ks::ui::NavStatus::NeedsScopeSwitch);
            WPJ6_CHECK(reroute != nullptr && !reroute->isHidden());
            WPJ6_CHECK(open(0x40ULL) == ks::ui::NavStatus::Ok);
            WPJ6_CHECK_NOTE(
                reroute != nullptr && reroute->isHidden(),
                QStringLiteral("之后又成功跳转到别处，过期的「切换并跳转」钮必须隐藏"));

            WPJ6_CHECK(open(0xFFFFF78000003000ULL) == ks::ui::NavStatus::NeedsScopeSwitch);
            if (reroute != nullptr)
            {
                emit reroute->clicked();
            }
            PumpFor(100);
            WPJ6_CHECK(h.view->target().session().scope == ksword::memwb::Scope::KernelVirtual);
            WPJ6_CHECK_NOTE(
                pane->insertionAddress() == 0xFFFFF78000003000ULL,
                QStringLiteral("点「切换并跳转」后应落在原请求地址，实际 0x%1").arg(pane->insertionAddress(), 0, 16));
        }

        // T15（杀 c13/c14/c16/c15）：int3 菜单项——对另一个地址不得误还原；成功不展开诊断抽屉、失败展开；attachGeneration 被记录。
        void TestInt3MenuBranches()
        {
            auto& backend = ConfigureSharedOnce();
            Harness h;
            h.AttachProcess(6106, 7ULL);
            PumpUntil([]() { return true; }, 10);
            auto* pane = h.view->hexPaneForTest();
            auto* statusBar = h.view->statusBarForTest();
            auto& int3 = ks::ui::WorkbenchShared::Instance().Int3();
            WPJ6_CHECK(WaitForStageable(pane, 0xB0ULL));
            WPJ6_CHECK(WaitForStageable(pane, 0xB4ULL));
            ClearInt3AndByte(0xB0ULL);
            ClearInt3AndByte(0xB4ULL);

            statusBar->setDrawerExpanded(false);
            ToggleInt3At(*h.view, 0xB0ULL);
            WPJ6_CHECK(int3.Entries().size() == 1U);
            WPJ6_CHECK_NOTE(
                !int3.Entries().empty() && int3.Entries().front().attachGeneration == 7ULL,
                QStringLiteral("安装时应记录当前附加代次 7"));
            WPJ6_CHECK_NOTE(!statusBar->isDrawerExpanded(), QStringLiteral("成功写入不应展开诊断抽屉"));

            // 另一个地址：必须是新装，不得误还原 0xB0 的那条。
            const QString label = ToggleInt3At(*h.view, 0xB4ULL);
            WPJ6_CHECK_NOTE(label == QStringLiteral("写入 int3 补丁"), QStringLiteral("0xB4 没有补丁，菜单应是写入，实际 %1").arg(label));
            WPJ6_CHECK_NOTE(
                int3.Entries().size() == 2U,
                QStringLiteral("对另一个地址点击应新增第二条补丁（而不是误还原第一条），实际 %1 条").arg(int3.Entries().size()));

            // 还原成功：不展开抽屉。
            statusBar->setDrawerExpanded(false);
            ToggleInt3At(*h.view, 0xB4ULL);
            WPJ6_CHECK_NOTE(!statusBar->isDrawerExpanded(), QStringLiteral("还原成功不应展开诊断抽屉"));

            // Diverged：别处改写了字节 -> 还原失败，必须展开抽屉。
            {
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                backend.backing->bytes[0xB0ULL - backend.backing->base] = 0x90;
            }
            statusBar->setDrawerExpanded(false);
            ToggleInt3At(*h.view, 0xB0ULL);
            WPJ6_CHECK_NOTE(statusBar->isDrawerExpanded(), QStringLiteral("还原失败（Diverged）应展开诊断抽屉"));
            ClearInt3AndByte(0xB0ULL);
            ClearInt3AndByte(0xB4ULL);
        }

        // T16（杀 c18/c19/c21）：离开守卫正文参数精确；Apply 失败必须拒绝离开。
        void TestLeaveGuardTextAndApplyFailure()
        {
            auto& backend = ConfigureSharedOnce();
            {
                Harness h;
                h.AttachProcess(6107);
                PumpUntil([]() { return true; }, 10);
                auto* pane = h.view->hexPaneForTest();
                auto* controller = h.view->writeControllerForTest();
                controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply);
                WPJ6_CHECK(WaitForStageable(pane, 0x50ULL));
                WPJ6_CHECK(WaitForStageable(pane, 0x52ULL));
                QString reason;
                WPJ6_CHECK(pane->canvas()->stageBytes(0x50ULL, QByteArray(3, '\x11'), &reason));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::Cancel;
                h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK_NOTE(
                    h.prompter->lastLeavePendingBytes == 3ULL && h.prompter->lastLeavePendingBlocks == 1ULL,
                    QStringLiteral("3 字节 1 块：正文参数应是 bytes=3 blocks=1，实际 bytes=%1 blocks=%2")
                        .arg(h.prompter->lastLeavePendingBytes)
                        .arg(h.prompter->lastLeavePendingBlocks));
                WPJ6_CHECK_NOTE(
                    h.prompter->lastLeaveReasonText == QStringLiteral("切换范围"),
                    QStringLiteral("ScopeChange 的离开原因应是「切换范围」，实际 %1").arg(h.prompter->lastLeaveReasonText));
            }
            {
                Harness h;
                h.AttachProcess(6108);
                PumpUntil([]() { return true; }, 10);
                auto* pane = h.view->hexPaneForTest();
                auto* controller = h.view->writeControllerForTest();
                controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply);
                const std::uint64_t address = 0x3CULL;
                WPJ6_CHECK(WaitForStageable(pane, address));
                QString reason;
                WPJ6_CHECK(pane->canvas()->stageBytes(address, QByteArray(1, '\x6D'), &reason));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
                std::size_t originalSize = 0;
                {
                    std::lock_guard<std::mutex> lock(backend.backing->mutex);
                    originalSize = backend.backing->bytes.size();
                    backend.backing->bytes.resize(static_cast<std::size_t>(address));
                }
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                {
                    std::lock_guard<std::mutex> lock(backend.backing->mutex);
                    backend.backing->bytes.resize(originalSize);
                }
                WPJ6_CHECK_NOTE(!allowed, QStringLiteral("应用并离开：提交失败时必须拒绝离开"));
                WPJ6_CHECK_NOTE(
                    h.view->statusBarForTest()->isDrawerExpanded(),
                    QStringLiteral("提交失败后诊断抽屉应自动展开"));
            }
        }
    }

    void RunReview2TestsB()
    {
        TestSidebarThresholdAndExpandButton();
        TestEmbeddedNeverShowsSidebar();
        TestInspectorUserOverrideRespected();
        TestViewMinimumSizeHintSmallWhenEmbeddedInHostLayout();
        TestNavRegressions();
        TestInt3MenuBranches();
        TestLeaveGuardTextAndApplyFailure();
    }
}
