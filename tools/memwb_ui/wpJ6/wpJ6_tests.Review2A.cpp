// ============================================================
// wpJ6_tests.Review2A.cpp
// 作用：MemoryWorkbenchView 第二轮独立复核给出的补测（第一批，原名 proposed-tests）。
//       覆盖第一轮缺陷修复里"撤回后夹具抓不住"的真缺口：离开守卫的应用/丢弃/int3 一半
//       （T1/T2/T4）、后退栈同地址不压栈与新跳转清前进栈（T5/T6）、各层最小尺寸防线
//       单独断言（T7/T8）、真实 QMessageBox 版离开确认框的按钮映射与默认按钮（T9）。
//       复核者在变异下逐条验证过：撤回对应修复，本文件各自出现失败行。
// 入口：RunReview2TestsA（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>

namespace wpj6_test
{
    namespace
    {
        // ArmClicker：见 probe-q.cpp。轮询等模态 QMessageBox，点指定文字的按钮。
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
                        defaultText = (box->defaultButton() != nullptr) ? box->defaultButton()->text() : QString();
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
            int ticks = 0;
            QString defaultText;
        };

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

        void ToggleInt3(ks::ui::MemoryWorkbenchView& view, const std::uint64_t address)
        {
            QMenu* menu = view.hexPaneForTest()->canvas()->buildContextMenu(address, true);
            if (menu == nullptr)
            {
                return;
            }
            for (auto* action : menu->actions())
            {
                if (action->text().contains(QStringLiteral("int3")))
                {
                    emit action->triggered();
                    break;
                }
            }
            delete menu;
        }

        void ClearInt3()
        {
            auto& int3 = ks::ui::WorkbenchShared::Instance().Int3();
            const auto entries = int3.Entries();
            for (const auto& entry : entries)
            {
                int3.Discard(entry.id);
            }
        }

        // T1（杀 a13）：离开守卫选"应用并离开"必须真的提交并放行。
        void TestLeaveGuardApplyCommitsAndAllows()
        {
            auto& backend = ConfigureSharedOnce();
            Harness h;
            h.AttachProcess(6001);
            PumpUntil([]() { return true; }, 10);
            WPJ6_CHECK(StageOne(*h.view, 0x28ULL, 0x5C));
            h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
            const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
            WPJ6_CHECK_NOTE(allowed, QStringLiteral("应用并离开：提交成功后必须放行"));
            std::uint8_t value = 0;
            {
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                value = backend.backing->bytes[0x28ULL - backend.backing->base];
            }
            WPJ6_CHECK_NOTE(value == 0x5C, QStringLiteral("应用并离开：假内存应已被写入 0x5C"));
            WPJ6_CHECK(!h.view->hexPaneForTest()->overlay().HasPendingPatches());
        }

        // T2（杀 a14）：选"丢弃并离开"，在没有随后身份变化的 requestLeave 路径上，暂存必须真的被丢。
        void TestLeaveGuardDiscardActuallyDiscards()
        {
            Harness h;
            h.AttachProcess(6002);
            PumpUntil([]() { return true; }, 10);
            WPJ6_CHECK(StageOne(*h.view, 0x24ULL, 0xAB));
            h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
            const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::DockAttachChange);
            WPJ6_CHECK(allowed);
            WPJ6_CHECK_NOTE(
                !h.view->hexPaneForTest()->overlay().HasPendingPatches(),
                QStringLiteral("丢弃并离开：放行之后暂存必须已被丢弃"));
        }

        // T4（杀 a17）：离开守卫的 int3 一半——取消阻止离开；全部还原后继续放行并真的还原。
        void TestLeaveGuardInt3HalfCancelAndRestoreAll()
        {
            auto& backend = ConfigureSharedOnce();
            const std::uint64_t address = 0xB0ULL;
            {
                Harness h;
                h.AttachProcess(6004);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), address);
                ToggleInt3(*h.view, address);
                WPJ6_CHECK(ks::ui::WorkbenchShared::Instance().Int3().Entries().size() == 1U);
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK_NOTE(!allowed, QStringLiteral("有未还原 int3 且用户取消时必须阻止离开"));
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(ks::ui::WorkbenchShared::Instance().Int3().Entries().size() == 1U);
            }
            {
                Harness h;
                h.AttachProcess(6004);
                PumpUntil([]() { return true; }, 10);
                ArmClicker clicker(QStringLiteral("全部还原后继续"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK_NOTE(allowed, QStringLiteral("全部还原后继续应放行"));
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(ks::ui::WorkbenchShared::Instance().Int3().Entries().empty());
                std::uint8_t value = 0xEE;
                {
                    std::lock_guard<std::mutex> lock(backend.backing->mutex);
                    value = backend.backing->bytes[address - backend.backing->base];
                }
                WPJ6_CHECK_NOTE(value != 0xCC, QStringLiteral("全部还原后目标字节不应还是 0xCC"));
            }
            ClearInt3();
        }

        // T5/T6（杀 a19/a20）：同地址不压栈；新跳转清前进栈。
        void TestBackStackSameAddressAndForwardClear()
        {
            Harness h;
            h.AttachProcess(6005);
            PumpUntil([]() { return true; }, 10);
            auto* pane = h.view->hexPaneForTest();
            WPJ6_CHECK(WaitForStageable(pane, 0x10ULL));
            const auto open = [&](const std::uint64_t address) {
                ks::ui::NavRequest request;
                request.address = address;
                return h.view->openAt(request);
            };
            WPJ6_CHECK(open(0x10ULL) == ks::ui::NavStatus::Ok);
            const std::size_t afterFirst = h.view->backStackForTest().size();
            WPJ6_CHECK(open(0x10ULL) == ks::ui::NavStatus::Ok);
            WPJ6_CHECK_NOTE(
                h.view->backStackForTest().size() == afterFirst,
                QStringLiteral("跳到当前就在的地址不应再压一条后退记录，之前 %1 之后 %2")
                    .arg(afterFirst)
                    .arg(h.view->backStackForTest().size()));
            WPJ6_CHECK(open(0x20ULL) == ks::ui::NavStatus::Ok);
            WPJ6_CHECK(open(0x30ULL) == ks::ui::NavStatus::Ok);
            emit h.view->backButtonForTest()->clicked();
            WPJ6_CHECK(!h.view->forwardStackForTest().empty());
            WPJ6_CHECK(open(0x50ULL) == ks::ui::NavStatus::Ok);
            WPJ6_CHECK_NOTE(
                h.view->forwardStackForTest().empty(),
                QStringLiteral("后退之后又做新跳转，前进栈必须被清空，实际剩 %1 项").arg(h.view->forwardStackForTest().size()));
        }

        // T7/T8（杀 a01/a06/a07/a08）：各层最小尺寸防线单独断言（视图级断言只看合力，抓不到单层回退）。
        void TestMinimumSizeLayersIndividually()
        {
            Harness h;
            h.AttachProcess(6007);
            PumpUntil([]() { return true; }, 10);
            auto* view = h.view.get();
            view->resize(900, 700);
            view->show();
            PumpFor(100);
            WPJ6_CHECK_NOTE(
                view->minimumSize().width() <= 100,
                QStringLiteral("侧栏展开时视图本身的硬性最小宽度应不被布局回灌，实际 %1").arg(view->minimumSize().width()));
            auto* stack = view->subTabStackForTest();
            for (int i = 0; i < stack->count(); ++i)
            {
                WPJ6_CHECK_NOTE(
                    stack->widget(i)->minimumSizeHint().width() <= 100,
                    QStringLiteral("子页 %1 minimumSizeHint 宽 %2 应保持很小")
                        .arg(i)
                        .arg(stack->widget(i)->minimumSizeHint().width()));
            }
            view->hide();
        }

        // T9（杀 d08/d09）：真实 QMessageBox 版 PromptLeaveWithPending 的按钮映射与默认按钮（取消）。
        void TestRealLeavePromptButtonMapping()
        {
            for (const auto& pick : std::vector<std::pair<QString, ksword::memwb::ModeSwitchDecision>>{
                     {QStringLiteral("应用并离开"), ksword::memwb::ModeSwitchDecision::ApplyThenSwitch},
                     {QStringLiteral("丢弃并离开"), ksword::memwb::ModeSwitchDecision::DiscardThenSwitch},
                     {QStringLiteral("取消"), ksword::memwb::ModeSwitchDecision::Cancel}})
            {
                ks::ui::WorkbenchConfirmations confirmations(nullptr, nullptr, nullptr);
                ArmClicker clicker(pick.first);
                const auto decision = confirmations.PromptLeaveWithPending(12ULL, 1ULL, QStringLiteral("切换范围"));
                WPJ6_CHECK_NOTE(clicker.clicked, QStringLiteral("没找到按钮 %1").arg(pick.first));
                WPJ6_CHECK_NOTE(
                    decision == pick.second,
                    QStringLiteral("点「%1」应得到对应决定，实际=%2").arg(pick.first).arg(static_cast<int>(decision)));
                WPJ6_CHECK_NOTE(
                    clicker.defaultText == QStringLiteral("取消"),
                    QStringLiteral("默认按钮必须是取消，实际=%1").arg(clicker.defaultText));
            }
        }
    }

    void RunReview2TestsA()
    {
        TestRealLeavePromptButtonMapping();
        TestLeaveGuardApplyCommitsAndAllows();
        TestLeaveGuardDiscardActuallyDiscards();
        TestLeaveGuardInt3HalfCancelAndRestoreAll();
        TestBackStackSameAddressAndForwardClear();
        TestMinimumSizeLayersIndividually();
    }
}
