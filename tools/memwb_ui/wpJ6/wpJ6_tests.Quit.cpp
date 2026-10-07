// ============================================================
// wpJ6_tests.Quit.cpp
// 作用：MemoryWorkbenchView::confirmQuit() 的专项测试（波 4：MainWindow::closeEvent 最前
//       调用，退出前问一次暂存与 int3）。confirmQuit 与离开守卫共用 runLeaveSequence，
//       所以这里既测"退出"自己的口径（原因文案固定为「退出程序」、int3 场景为
//       MainWindowClose），也测它与 requestLeave 对同一场景给出一致结果（共用同一条路径）。
//       覆盖：无暂存无 int3 不弹框；仅暂存三种决定；仅 int3 三种选择；暂存+int3 的原子性
//       （int3 取消时暂存原封不动）；暂存取消时 int3 框根本不弹。
// 入口：RunQuitTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

namespace wpj6_test
{
    namespace
    {
        // ArmClicker：轮询等模态 QMessageBox 出现后点指定文字的按钮；6 秒没出现则放弃。
        // 离屏环境里没有人能点真实的 Int3Controller::RequestLeave 框，必须这样自动点。
        struct ArmClicker
        {
            explicit ArmClicker(const QString& wantedText)
                : wanted(wantedText)
            {
                QObject::connect(&timer, &QTimer::timeout, [this]() {
                    ++ticks;
                    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                    if (box != nullptr)
                    {
                        seen = true;
                        for (auto* button : box->buttons())
                        {
                            if (button->text() == wanted)
                            {
                                clicked = true;
                                timer.stop();
                                button->click();
                                return;
                            }
                        }
                        timer.stop();
                        box->reject();
                        return;
                    }
                    if (ticks > 120)
                    {
                        timer.stop();
                    }
                });
                timer.start(50);
            }
            ~ArmClicker() { timer.stop(); }

            QTimer timer;
            QString wanted;
            bool clicked = false;
            bool seen = false;
            int ticks = 0;
        };

        // StageOne：切到暂存模式并暂存一个字节（等画布与基线窗口都覆盖该地址）。
        bool StageOne(ks::ui::MemoryWorkbenchView& view, const std::uint64_t address, const std::uint8_t value)
        {
            auto* pane = view.hexPaneForTest();
            auto* controller = view.writeControllerForTest();
            if (controller->mode() == ksword::memwb::WriteMode::Immediate)
            {
                controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply);
            }
            if (!WaitForStageable(pane, address))
            {
                return false;
            }
            QString reason;
            return pane->canvas()->stageBytes(address, QByteArray(1, static_cast<char>(value)), &reason);
        }

        // ToggleInt3：经画布右键菜单触发"写入/还原 int3"。
        void ToggleInt3(ks::ui::MemoryWorkbenchView& view, const std::uint64_t address)
        {
            QMenu* menu = view.hexPaneForTest()->canvas()->buildContextMenu(address, true);
            if (menu != nullptr)
            {
                for (auto* action : menu->actions())
                {
                    if (action->text().contains(QStringLiteral("int3")))
                    {
                        emit action->triggered();
                        break;
                    }
                }
            }
            delete menu;
        }

        // ClearInt3AndByte：清掉共享 int3 账本，并把假内存里该地址的字节恢复成 0x00。
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

        std::uint8_t BackingByte(const std::uint64_t address)
        {
            auto& backend = ConfigureSharedOnce();
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            return backend.backing->bytes[address - backend.backing->base];
        }

        // PendingChipVisible：会话条"N 字节待写入"标签当前是否对用户可见。
        bool PendingChipVisible(ks::ui::MemoryWorkbenchView& view)
        {
            auto* bar = view.sessionBarForTest();
            for (auto* label : bar->findChildren<QLabel*>())
            {
                if (label->text().contains(QStringLiteral("待写入")))
                {
                    return label->isVisibleTo(bar);
                }
            }
            return false;
        }

        std::size_t Int3Count()
        {
            return ks::ui::WorkbenchShared::Instance().Int3().Entries().size();
        }

        // 无暂存、无 int3：直接放行，不弹任何框，也不问暂存三选一。
        void TestQuitNothingToAsk()
        {
            Harness h;
            h.AttachProcess(6401);
            PumpUntil([]() { return true; }, 10);
            ArmClicker watcher(QStringLiteral("取消"));
            const bool allowed = h.view->confirmQuit();
            PumpFor(200);
            WPJ6_CHECK_NOTE(allowed, QStringLiteral("无暂存无 int3：必须放行"));
            WPJ6_CHECK_NOTE(h.prompter->leaveWithPendingCallCount == 0, QStringLiteral("不得问暂存三选一"));
            WPJ6_CHECK_NOTE(!watcher.seen, QStringLiteral("不得弹任何模态框"));
        }

        // 仅暂存：取消 / 丢弃 / 应用三种决定。
        void TestQuitPendingOnly()
        {
            // 取消：返回 false，暂存与芯片原封不动，原因文案是「退出程序」。
            {
                Harness h;
                h.AttachProcess(6402);
                PumpUntil([]() { return true; }, 10);
                WPJ6_CHECK(StageOne(*h.view, 0x24ULL, 0xAB));
                WPJ6_CHECK(PendingChipVisible(*h.view));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::Cancel;
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK_NOTE(!allowed, QStringLiteral("暂存框取消：不得退出"));
                WPJ6_CHECK_NOTE(h.prompter->leaveWithPendingCallCount == 1, QStringLiteral("暂存三选一恰好问一次"));
                WPJ6_CHECK_NOTE(
                    h.prompter->lastLeaveReasonText == QStringLiteral("退出程序"),
                    QStringLiteral("原因文案应是「退出程序」，实际 %1").arg(h.prompter->lastLeaveReasonText));
                WPJ6_CHECK(h.view->hexPaneForTest()->overlay().HasPendingPatches());
                WPJ6_CHECK(PendingChipVisible(*h.view));
            }
            // 丢弃：放行，暂存清空，芯片清掉。
            {
                Harness h;
                h.AttachProcess(6403);
                PumpUntil([]() { return true; }, 10);
                WPJ6_CHECK(StageOne(*h.view, 0x28ULL, 0xAC));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                // beforeDiscard：既有内容快照，真正丢弃后必须被判为陈旧；空退出不得重复推进。
                const auto beforeDiscard = h.view->target().capture().rev;
                const bool allowed = h.view->confirmQuit();
                PumpFor(60);
                WPJ6_CHECK(allowed);
                WPJ6_CHECK(!h.view->hexPaneForTest()->overlay().HasPendingPatches());
                WPJ6_CHECK_NOTE(!PendingChipVisible(*h.view), QStringLiteral("丢弃并退出：待写入芯片必须清掉"));
                const auto afterDiscard = h.view->target().capture().rev;
                WPJ6_CHECK(afterDiscard.content > beforeDiscard.content);
                WPJ6_CHECK(h.view->target().isStale(beforeDiscard));
                WPJ6_CHECK(h.view->confirmQuit());
                WPJ6_CHECK(h.view->target().capture().rev.content == afterDiscard.content);
            }
            // 应用：放行，字节真的写进假内存。
            {
                Harness h;
                h.AttachProcess(6404);
                PumpUntil([]() { return true; }, 10);
                const std::uint64_t address = 0x2CULL;
                const std::uint8_t before = BackingByte(address);
                const std::uint8_t wanted = static_cast<std::uint8_t>(before ^ 0x6B);
                WPJ6_CHECK(StageOne(*h.view, address, wanted));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(allowed);
                WPJ6_CHECK_NOTE(BackingByte(address) == wanted, QStringLiteral("应用并退出：字节应已写入目标"));
                WPJ6_CHECK(!h.view->hexPaneForTest()->overlay().HasPendingPatches());
                // 还原被应用的字节，避免影响后面的测试。
                auto& backend = ConfigureSharedOnce();
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                backend.backing->bytes[address - backend.backing->base] = before;
            }
        }

        // 仅 int3：取消 / 保留 / 全部还原三种选择。
        void TestQuitInt3Only()
        {
            const std::uint64_t address = 0xB0ULL;
            // 取消：不得退出，补丁仍在。
            {
                Harness h;
                h.AttachProcess(6411);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                WPJ6_CHECK(Int3Count() == 1U);
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK_NOTE(!allowed, QStringLiteral("int3 框取消：不得退出"));
                WPJ6_CHECK(Int3Count() == 1U);
                WPJ6_CHECK(h.prompter->leaveWithPendingCallCount == 0);
                ClearInt3AndByte(address);
            }
            // 保留补丁继续：放行，补丁与 0xCC 都还在。
            {
                Harness h;
                h.AttachProcess(6412);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                ArmClicker clicker(QStringLiteral("保留补丁继续"));
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(allowed);
                WPJ6_CHECK(Int3Count() == 1U);
                WPJ6_CHECK_NOTE(BackingByte(address) == 0xCC, QStringLiteral("保留补丁：目标里的 0xCC 不得被动"));
                ClearInt3AndByte(address);
            }
            // 全部还原后继续：放行，账本清空，字节还原。
            {
                Harness h;
                h.AttachProcess(6413);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                ArmClicker clicker(QStringLiteral("全部还原后继续"));
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(allowed);
                WPJ6_CHECK(Int3Count() == 0U);
                WPJ6_CHECK_NOTE(BackingByte(address) != 0xCC, QStringLiteral("全部还原：目标字节不应还是 0xCC"));
                ClearInt3AndByte(address);
            }
        }

        // 暂存 + int3：原子性。
        void TestQuitAtomicWithBoth()
        {
            const std::uint64_t int3Address = 0xB0ULL;
            // 暂存选丢弃、int3 选取消：不得退出，暂存不得已被丢，补丁仍在。
            {
                Harness h;
                h.AttachProcess(6421);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                WPJ6_CHECK(StageOne(*h.view, 0x40ULL, 0x99));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(!allowed);
                WPJ6_CHECK_NOTE(
                    h.view->hexPaneForTest()->overlay().HasPendingPatches(),
                    QStringLiteral("退出被取消时，用户的未提交编辑不得已被丢弃"));
                WPJ6_CHECK(PendingChipVisible(*h.view));
                WPJ6_CHECK(Int3Count() == 1U);
                ClearInt3AndByte(int3Address);
            }
            // 暂存选应用、int3 选取消：不得退出，暂存不得已被写进目标。
            {
                Harness h;
                h.AttachProcess(6422);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                const std::uint64_t stagedAddress = 0x44ULL;
                const std::uint8_t before = BackingByte(stagedAddress);
                WPJ6_CHECK(StageOne(*h.view, stagedAddress, static_cast<std::uint8_t>(before ^ 0x5A)));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(!allowed);
                WPJ6_CHECK_NOTE(BackingByte(stagedAddress) == before, QStringLiteral("退出被取消时，暂存不得已写入目标"));
                WPJ6_CHECK(h.view->hexPaneForTest()->overlay().HasPendingPatches());
                ClearInt3AndByte(int3Address);
            }
            // 暂存框取消：int3 框根本不弹。
            {
                Harness h;
                h.AttachProcess(6423);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                WPJ6_CHECK(StageOne(*h.view, 0x48ULL, 0x98));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::Cancel;
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->confirmQuit();
                PumpFor(400);
                WPJ6_CHECK(!allowed);
                WPJ6_CHECK_NOTE(!clicker.seen, QStringLiteral("暂存框已取消，不应再弹 int3 框"));
                WPJ6_CHECK(h.view->hexPaneForTest()->overlay().HasPendingPatches());
                ClearInt3AndByte(int3Address);
            }
            // 两个都同意（丢弃 + 保留补丁）：放行，暂存清空，补丁保留。
            {
                Harness h;
                h.AttachProcess(6424);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                WPJ6_CHECK(StageOne(*h.view, 0x4CULL, 0x97));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                ArmClicker clicker(QStringLiteral("保留补丁继续"));
                const bool allowed = h.view->confirmQuit();
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(allowed);
                WPJ6_CHECK(!h.view->hexPaneForTest()->overlay().HasPendingPatches());
                WPJ6_CHECK(Int3Count() == 1U);
                ClearInt3AndByte(int3Address);
            }
        }

        // confirmQuit 与 requestLeave 共用同一条路径：同一暂存场景、同一决定，两个入口结果一致；
        // 唯一区别是传给确认框的原因文案。
        void TestQuitSharesPathWithRequestLeave()
        {
            const ksword::memwb::ModeSwitchDecision decisions[] = {
                ksword::memwb::ModeSwitchDecision::Cancel,
                ksword::memwb::ModeSwitchDecision::DiscardThenSwitch,
                ksword::memwb::ModeSwitchDecision::ApplyThenSwitch,
            };
            int pid = 6431;
            for (const auto decision : decisions)
            {
                bool viaQuit = false;
                bool viaLeave = false;
                bool pendingAfterQuit = false;
                bool pendingAfterLeave = false;
                QString quitReason;
                QString leaveReason;
                {
                    Harness h;
                    h.AttachProcess(static_cast<std::uint32_t>(pid++));
                    PumpUntil([]() { return true; }, 10);
                    WPJ6_CHECK(StageOne(*h.view, 0x30ULL, 0x31));
                    h.prompter->leaveWithPendingDecision = decision;
                    viaQuit = h.view->confirmQuit();
                    pendingAfterQuit = h.view->hexPaneForTest()->overlay().HasPendingPatches();
                    quitReason = h.prompter->lastLeaveReasonText;
                }
                // 应用决定会把 0x31 写进假内存：先还原，否则下一段"暂存 0x31"是无变化的空暂存，
                // 不会弹确认框（上一版就因此在应用决定上误报）。
                {
                    auto& backend = ConfigureSharedOnce();
                    std::lock_guard<std::mutex> lock(backend.backing->mutex);
                    backend.backing->bytes[0x30ULL - backend.backing->base] = 0x00;
                }
                {
                    Harness h;
                    h.AttachProcess(static_cast<std::uint32_t>(pid++));
                    PumpUntil([]() { return true; }, 10);
                    WPJ6_CHECK(StageOne(*h.view, 0x30ULL, 0x31));
                    h.prompter->leaveWithPendingDecision = decision;
                    viaLeave = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                    pendingAfterLeave = h.view->hexPaneForTest()->overlay().HasPendingPatches();
                    leaveReason = h.prompter->lastLeaveReasonText;
                }
                WPJ6_CHECK_NOTE(
                    viaQuit == viaLeave,
                    QStringLiteral("决定 %1：confirmQuit=%2 与 requestLeave=%3 必须一致")
                        .arg(static_cast<int>(decision))
                        .arg(viaQuit)
                        .arg(viaLeave));
                WPJ6_CHECK_NOTE(
                    pendingAfterQuit == pendingAfterLeave,
                    QStringLiteral("决定 %1：两个入口之后暂存状态必须一致").arg(static_cast<int>(decision)));
                WPJ6_CHECK_NOTE(
                    quitReason == QStringLiteral("退出程序") && leaveReason == QStringLiteral("切换范围"),
                    QStringLiteral("原因文案：退出=%1 切换范围=%2").arg(quitReason, leaveReason));
                // 应用决定会把 0x31 写进假内存：还原。
                {
                    auto& backend = ConfigureSharedOnce();
                    std::lock_guard<std::mutex> lock(backend.backing->mutex);
                    backend.backing->bytes[0x30ULL - backend.backing->base] = 0x00;
                }
            }
        }
    }

    namespace
    {
        // Dock 分离：用户在离开守卫里选了"保留补丁继续"之后，紧接着的 aboutToDetach 安全网不得
        // 再强制还原补丁；没经过守卫的分离才由安全网兜底还原；选"全部还原"则守卫已还原。
        void TestKeepChoiceSurvivesDockDetach()
        {
            const std::uint64_t address = 0xB0ULL;
            // 保留：守卫放行后分离，补丁与 0xCC 都还在。
            {
                Harness h;
                h.AttachProcess(6441);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                WPJ6_CHECK(Int3Count() == 1U);
                {
                    ArmClicker clicker(QStringLiteral("保留补丁继续"));
                    const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::DockAttachChange);
                    WPJ6_CHECK(clicker.clicked);
                    WPJ6_CHECK(allowed);
                }
                h.view->target().onDockAboutToDetach();
                WPJ6_CHECK_NOTE(Int3Count() == 1U, QStringLiteral("用户选了保留：分离安全网不得强制还原"));
                WPJ6_CHECK(BackingByte(address) == 0xCC);
                // 记号一次性：再来一次未经守卫的分离，安全网应当兜底还原。
                h.view->target().onDockAboutToDetach();
                WPJ6_CHECK_NOTE(Int3Count() == 0U, QStringLiteral("下一次未经提示的分离应由安全网兜底还原"));
                WPJ6_CHECK(BackingByte(address) != 0xCC);
                ClearInt3AndByte(address);
            }
            // 没经过守卫的分离：安全网直接还原。
            {
                Harness h;
                h.AttachProcess(6442);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                h.view->target().onDockAboutToDetach();
                WPJ6_CHECK_NOTE(Int3Count() == 0U, QStringLiteral("未经提示的分离：安全网应还原补丁"));
                ClearInt3AndByte(address);
            }
            // 守卫里选"全部还原后继续"：守卫已还原，分离时无事可做。
            {
                Harness h;
                h.AttachProcess(6443);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                {
                    ArmClicker clicker(QStringLiteral("全部还原后继续"));
                    const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::DockAttachChange);
                    WPJ6_CHECK(clicker.clicked);
                    WPJ6_CHECK(allowed);
                }
                WPJ6_CHECK(Int3Count() == 0U);
                h.view->target().onDockAboutToDetach();
                WPJ6_CHECK(Int3Count() == 0U);
                ClearInt3AndByte(address);
            }
            // 切范围的守卫（非 Dock 分离）选了保留：不能给后面的分离留下"已保留"记号。
            {
                Harness h;
                h.AttachProcess(6444);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                {
                    ArmClicker clicker(QStringLiteral("保留补丁继续"));
                    const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                    WPJ6_CHECK(clicker.clicked);
                    WPJ6_CHECK(allowed);
                }
                h.view->target().onDockAboutToDetach();
                WPJ6_CHECK_NOTE(
                    Int3Count() == 0U,
                    QStringLiteral("范围切换守卫里的保留不适用于之后的 Dock 分离，安全网应还原"));
                ClearInt3AndByte(address);
            }
            // 全局退出中本视图已选保留、另一视图取消：撤销退出许可后，下一次分离必须仍有安全网。
            {
                Harness h;
                h.AttachProcess(6445);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                {
                    ArmClicker clicker(QStringLiteral("保留补丁继续"));
                    WPJ6_CHECK(h.view->confirmQuit());
                    WPJ6_CHECK(clicker.clicked);
                }
                WPJ6_CHECK(Int3Count() == 1U);
                h.view->cancelQuitPreparation();
                h.view->target().onDockAboutToDetach();
                WPJ6_CHECK_NOTE(Int3Count() == 0U,
                    QStringLiteral("全局退出取消后，先前保留许可不得跳过下一次分离安全网"));
                ClearInt3AndByte(address);
            }
        }
    }

    void RunQuitTests()
    {
        TestKeepChoiceSurvivesDockDetach();
        TestQuitNothingToAsk();
        TestQuitPendingOnly();
        TestQuitInt3Only();
        TestQuitAtomicWithBoth();
        TestQuitSharesPathWithRequestLeave();
    }
}
