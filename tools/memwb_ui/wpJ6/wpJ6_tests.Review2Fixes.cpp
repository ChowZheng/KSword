// ============================================================
// wpJ6_tests.Review2Fixes.cpp
// 作用：MemoryWorkbenchView 第二轮独立复核确认的 6 个缺陷（B1~B6）的永久回归测试。
//       每条都对应复核者给出的探针读数：修复前红、修复后绿；撤回对应修复，本文件
//       必须各自出现失败行（见 tools/memwb_ui/Invoke-MemwbMutation.ps1 的变异重放）。
//       - B1 侧栏状态机与 loadSettings/saveSettings：默认/已存的"隐藏"在宽屏下不得被
//         自动展开；loadSettings 在 show 之前调用时画布不得 0px；窄窗口自动折叠不得
//         覆盖宽屏时存下的"可见"偏好；内嵌模式恒隐藏且无展开钮。
//       - B2 openAt 的返回栈按"身份是否真的变了"而不是"请求里写了哪些字段"判断。
//       - B3 离开守卫原子：int3 框取消时暂存必须原封不动（既不丢弃也不写入）。
//       - B4 丢弃并离开 / 换目标之后会话条"N 字节待写入"必须同步清掉。
//       - B6 "切换并跳转"钮必须有文字、有悬停说明、图标不同于"前进"，后退后须隐藏。
//       B5（en-US 下 int3 反馈无汉字）因为必须在 initialize("en-US") 之后断言，并入
//       wpJ6_main.cpp 的 RunI18nSmokeTest。
// 入口：RunReview2FixTests（由 wpJ6_main.cpp 调用）。
// ============================================================
#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchSettings.h"
#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>

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

        // ClearInt3AndByte：清掉共享 int3 账本，并把假内存里该地址的字节恢复成 0x00
        // （否则下一次安装会因为"原本就是 0xCC"被拒绝）。
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

        QWidget* SidebarOf(ks::ui::MemoryWorkbenchView* view)
        {
            auto* splitter = view->mainSplitterForTest();
            return (splitter != nullptr && splitter->count() == 2) ? splitter->widget(1) : nullptr;
        }

        QWidget* MainBodyOf(ks::ui::MemoryWorkbenchView* view)
        {
            auto* splitter = view->mainSplitterForTest();
            return (splitter != nullptr && splitter->count() == 2) ? splitter->widget(0) : nullptr;
        }

        // ResetSettings：把本文件会读写的设置键恢复成默认，避免污染后面的测试。
        void ResetSettings()
        {
            using namespace ks::ui::workbench_settings;
            SaveSidebarVisible(false);
            SaveSidebarWidth(300);
            SaveScope(0U);
            SaveWriteMode(0U);
        }

        // ---------------- B1：侧栏状态机与设置 ----------------

        // 默认（key 缺失=隐藏）与已存隐藏：loadSettings 在 show 之前调用，宽屏下侧栏
        // 必须保持隐藏、展开钮可见、画布有真实宽度；再放宽 1px 也不得被自动展开。
        void TestSavedHiddenSidebarStaysHiddenOnWideWindow()
        {
            using namespace ks::ui::workbench_settings;
            for (const int wideWidth : {900, 1200, 1600})
            {
                ResetSettings();
                Harness h;
                h.AttachProcess();
                auto* view = h.view.get();
                view->loadSettings();
                view->resize(wideWidth, 700);
                view->show();
                PumpFor(120);
                auto* sidebar = SidebarOf(view);
                auto* expand = FindExpandButton(view);
                WPJ6_CHECK(sidebar != nullptr && expand != nullptr);
                if (sidebar == nullptr || expand == nullptr)
                {
                    view->hide();
                    continue;
                }
                WPJ6_CHECK_NOTE(
                    sidebar->isHidden(),
                    QStringLiteral("已存/默认隐藏：%1 宽下侧栏必须保持隐藏").arg(wideWidth));
                WPJ6_CHECK_NOTE(!expand->isHidden(), QStringLiteral("侧栏隐藏时展开钮必须可见"));
                WPJ6_CHECK_NOTE(
                    view->hexPaneForTest()->canvas()->width() >= 300,
                    QStringLiteral("画布应有真实宽度，实际 %1").arg(view->hexPaneForTest()->canvas()->width()));
                view->resize(wideWidth + 1, 700);
                PumpFor(80);
                WPJ6_CHECK_NOTE(
                    sidebar->isHidden(),
                    QStringLiteral("只放宽 1px 也不得把已存为隐藏的侧栏自动放出来（%1 宽）").arg(wideWidth));
                view->hide();
            }
            ResetSettings();
        }

        // 已存可见：loadSettings 在 show 之前调用，首次显示时侧栏宽度≈已存宽度、主体不被挤成 0px。
        // 两种调用顺序都要覆盖：先 loadSettings 后 resize（此刻视图还是默认的小宽度），
        // 以及先 resize 后 loadSettings（视图已经"够宽"、侧栏保持可见，但分割条在 show 之前
        // 还没有真实宽度——这时若直接 setSizes，会拿未布局的宽度做减法，把宽度全分给侧栏）。
        void CheckSavedVisibleSidebarFirstShow(const bool resizeFirst, const int wideWidth)
        {
            using namespace ks::ui::workbench_settings;
            ResetSettings();
            SaveSidebarVisible(true);
            SaveSidebarWidth(320);
            Harness h;
            h.AttachProcess();
            auto* view = h.view.get();
            if (resizeFirst)
            {
                view->resize(wideWidth, 700);
                view->loadSettings();
            }
            else
            {
                view->loadSettings();
                view->resize(wideWidth, 700);
            }
            view->show();
            PumpFor(150);
            auto* sidebar = SidebarOf(view);
            auto* body = MainBodyOf(view);
            WPJ6_CHECK(sidebar != nullptr && body != nullptr);
            if (sidebar == nullptr || body == nullptr)
            {
                view->hide();
                return;
            }
            const QString where = QStringLiteral("（%1 宽，%2）")
                .arg(wideWidth)
                .arg(resizeFirst ? QStringLiteral("先 resize 后 load") : QStringLiteral("先 load 后 resize"));
            WPJ6_CHECK_NOTE(!sidebar->isHidden(), QStringLiteral("已存可见：侧栏应显示") + where);
            WPJ6_CHECK_NOTE(
                sidebar->width() >= 300 && sidebar->width() <= 340,
                QStringLiteral("侧栏宽度应≈已存的 320，实际 %1").arg(sidebar->width()) + where);
            WPJ6_CHECK_NOTE(
                body->width() >= 400,
                QStringLiteral("主体不得被挤成缝，实际 %1").arg(body->width()) + where);
            WPJ6_CHECK_NOTE(
                view->hexPaneForTest()->canvas()->width() >= 200,
                QStringLiteral("首次显示画布不得 0px，实际 %1").arg(view->hexPaneForTest()->canvas()->width()) + where);
            view->hide();
        }

        void TestSavedVisibleSidebarFirstShowGeometry()
        {
            for (const bool resizeFirst : {false, true})
            {
                for (const int wideWidth : {900, 1200})
                {
                    CheckSavedVisibleSidebarFirstShow(resizeFirst, wideWidth);
                }
            }
            ResetSettings();
        }

        // 窗口已经 show 之后再 loadSettings：已存可见 → 显示且宽度正确；已存隐藏 → 隐藏，且放宽 1px 不展开。
        void TestLoadSettingsAfterShow()
        {
            using namespace ks::ui::workbench_settings;
            {
                ResetSettings();
                SaveSidebarVisible(true);
                SaveSidebarWidth(300);
                Harness h;
                h.AttachProcess();
                auto* view = h.view.get();
                view->resize(900, 700);
                view->show();
                PumpFor(80);
                view->loadSettings();
                PumpFor(100);
                auto* sidebar = SidebarOf(view);
                WPJ6_CHECK(sidebar != nullptr);
                if (sidebar != nullptr)
                {
                    WPJ6_CHECK_NOTE(!sidebar->isHidden(), QStringLiteral("show 之后 loadSettings(可见) 侧栏应显示"));
                    WPJ6_CHECK_NOTE(
                        sidebar->width() >= 280 && sidebar->width() <= 320,
                        QStringLiteral("侧栏宽度应≈300，实际 %1").arg(sidebar->width()));
                }
                view->hide();
            }
            {
                ResetSettings();
                Harness h;
                h.AttachProcess();
                auto* view = h.view.get();
                view->resize(900, 700);
                view->show();
                PumpFor(80);
                WPJ6_CHECK(!SidebarOf(view)->isHidden());   // 前置：未 loadSettings 时默认可见
                view->loadSettings();
                PumpFor(80);
                WPJ6_CHECK_NOTE(SidebarOf(view)->isHidden(), QStringLiteral("show 之后 loadSettings(隐藏) 应隐藏侧栏"));
                view->resize(901, 700);
                PumpFor(80);
                WPJ6_CHECK_NOTE(SidebarOf(view)->isHidden(), QStringLiteral("随后放宽 1px 不得自动展开"));
                view->hide();
            }
            ResetSettings();
        }

        // saveSettings 存的是"想要可见"：窄窗口自动折叠不得覆盖宽屏时存下的可见偏好；
        // 手动折叠则必须存成隐藏；侧栏隐藏期间不得把宽度冲成 0。
        void TestSaveSettingsKeepsPreferenceNotAutoCollapseSideEffect()
        {
            using namespace ks::ui::workbench_settings;
            ResetSettings();
            SaveSidebarVisible(true);
            SaveSidebarWidth(310);
            Harness h;
            h.AttachProcess();
            auto* view = h.view.get();
            view->setSettingsAuthoritative(true);
            view->loadSettings();
            view->resize(420, 700);
            view->show();
            PumpFor(120);
            WPJ6_CHECK_NOTE(SidebarOf(view)->isHidden(), QStringLiteral("420 宽应自动折叠（前置条件）"));
            view->saveSettings();
            WPJ6_CHECK_NOTE(
                LoadSidebarVisible(),
                QStringLiteral("窄窗口自动折叠不得把已存的「可见」偏好覆盖成隐藏"));
            WPJ6_CHECK_NOTE(
                LoadSidebarWidth() == 310,
                QStringLiteral("侧栏隐藏期间不得改写已存宽度，实际 %1").arg(LoadSidebarWidth()));

            view->resize(900, 700);
            PumpFor(120);
            WPJ6_CHECK_NOTE(!SidebarOf(view)->isHidden(), QStringLiteral("放宽回 900 应自动展开（前置条件）"));
            auto* toggle = FindShortcut(view, QStringLiteral("Ctrl+Shift+B"));
            WPJ6_CHECK(toggle != nullptr);
            if (toggle != nullptr)
            {
                emit toggle->activated();
                PumpFor(80);
                WPJ6_CHECK(SidebarOf(view)->isHidden());
                view->saveSettings();
                WPJ6_CHECK_NOTE(!LoadSidebarVisible(), QStringLiteral("用户手动折叠后必须存成隐藏"));
                emit toggle->activated();
                PumpFor(80);
                WPJ6_CHECK(!SidebarOf(view)->isHidden());
                view->saveSettings();
                WPJ6_CHECK_NOTE(LoadSidebarVisible(), QStringLiteral("用户手动展开后必须存成可见"));
            }
            view->hide();
            ResetSettings();
        }

        // 内嵌：loadSettings 不得把侧栏放出来；先窄后内嵌展开钮必须消失；退出内嵌后侧栏与分割条恢复正常。
        void TestEmbeddedSidebarAndLoadSettings()
        {
            using namespace ks::ui::workbench_settings;
            {
                ResetSettings();
                SaveSidebarVisible(true);
                Harness h;
                h.AttachProcess();
                auto* view = h.view.get();
                view->setEmbeddedProcessMode(true);
                view->loadSettings();
                view->resize(900, 700);
                view->show();
                PumpFor(120);
                WPJ6_CHECK_NOTE(SidebarOf(view)->isHidden(), QStringLiteral("内嵌模式下 loadSettings(可见) 不得放出侧栏"));
                WPJ6_CHECK_NOTE(FindExpandButton(view)->isHidden(), QStringLiteral("内嵌模式不应有展开钮"));
                view->hide();
            }
            {
                ResetSettings();
                Harness h;
                h.AttachProcess();
                auto* view = h.view.get();
                view->resize(420, 700);
                view->show();
                PumpFor(120);
                WPJ6_CHECK_NOTE(!FindExpandButton(view)->isHidden(), QStringLiteral("420 宽自动折叠后展开钮可见（前置条件）"));
                view->setEmbeddedProcessMode(true);
                PumpFor(60);
                WPJ6_CHECK_NOTE(FindExpandButton(view)->isHidden(), QStringLiteral("切入内嵌模式后展开钮必须消失"));
                view->hide();
            }
            {
                ResetSettings();
                Harness h;
                h.AttachProcess();
                auto* view = h.view.get();
                view->resize(900, 700);
                view->show();
                PumpFor(100);
                view->setEmbeddedProcessMode(true);
                PumpFor(60);
                WPJ6_CHECK(SidebarOf(view)->isHidden());
                view->setEmbeddedProcessMode(false);
                PumpFor(150);
                auto* sidebar = SidebarOf(view);
                auto* body = MainBodyOf(view);
                WPJ6_CHECK_NOTE(!sidebar->isHidden(), QStringLiteral("退出内嵌后 900 宽应恢复侧栏"));
                WPJ6_CHECK_NOTE(
                    body->width() >= 400 && sidebar->width() >= 200,
                    QStringLiteral("退出内嵌后分割条尺寸应正常，主体 %1 侧栏 %2").arg(body->width()).arg(sidebar->width()));
                view->hide();
            }
            ResetSettings();
        }

        // ---------------- B2：返回栈按"身份是否真的变了"判断 ----------------

        void TestBackStackWithExplicitPidAndChannel()
        {
            const auto selfPid = static_cast<std::uint32_t>(QCoreApplication::applicationPid());
            std::size_t sizes[3] = {0, 0, 0};
            for (int variant = 0; variant < 3; ++variant)
            {
                Harness h;
                h.AttachProcess(selfPid, 1);
                PumpUntil([]() { return true; }, 10);
                auto* pane = h.view->hexPaneForTest();
                WaitForStageable(pane, 0x10ULL);
                for (int i = 0; i < 3; ++i)
                {
                    ks::ui::NavRequest request;
                    request.scope = ksword::memwb::Scope::ProcessVirtual;
                    request.address = 0x10ULL + static_cast<std::uint64_t>(i) * 0x10ULL;
                    if (variant == 1)
                    {
                        request.pid = selfPid;
                    }
                    if (variant == 2)
                    {
                        request.channel = ksword::memwb::Channel::UserMode;
                    }
                    WPJ6_CHECK(h.view->openAt(request) == ks::ui::NavStatus::Ok);
                }
                sizes[variant] = h.view->backStackForTest().size();
            }
            WPJ6_CHECK_NOTE(sizes[0] >= 2U, QStringLiteral("无显式字段：三次不同地址的跳转后退栈应 >=2，实际 %1").arg(sizes[0]));
            WPJ6_CHECK_NOTE(
                sizes[1] == sizes[0],
                QStringLiteral("带显式 pid 的同目标跳转后退栈应与无字段一致（%1），实际 %2").arg(sizes[0]).arg(sizes[1]));
            WPJ6_CHECK_NOTE(
                sizes[2] == sizes[0],
                QStringLiteral("带显式 channel 的同目标跳转后退栈应与无字段一致（%1），实际 %2").arg(sizes[0]).arg(sizes[2]));
        }

        // ---------------- B3：离开守卫原子 ----------------

        void TestLeaveGuardAtomicWhenInt3Cancels()
        {
            const std::uint64_t int3Address = 0xB0ULL;
            // 暂存框选"丢弃并离开"、int3 框选"取消"：离开被拒，暂存必须原封不动，int3 补丁仍在。
            {
                Harness h;
                h.AttachProcess(6301);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                WPJ6_CHECK(StageOne(*h.view, 0x40ULL, 0x99));
                WPJ6_CHECK(PendingChipVisible(*h.view));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                h.prompter->leaveWithPendingCallCount = 0;
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK_NOTE(!allowed, QStringLiteral("int3 框取消：离开必须被拒绝"));
                WPJ6_CHECK_NOTE(
                    h.view->hexPaneForTest()->overlay().HasPendingPatches(),
                    QStringLiteral("离开被拒绝时，用户的未提交编辑不得已被丢弃"));
                WPJ6_CHECK_NOTE(PendingChipVisible(*h.view), QStringLiteral("暂存还在，待写入芯片也应还在"));
                WPJ6_CHECK(ks::ui::WorkbenchShared::Instance().Int3().Entries().size() == 1U);
                WPJ6_CHECK(h.prompter->leaveWithPendingCallCount == 1);
                ClearInt3AndByte(int3Address);
            }
            // 暂存框选"应用并离开"、int3 框选"取消"：离开被拒，暂存不得已被写入目标。
            {
                Harness h;
                h.AttachProcess(6302);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                const std::uint64_t stagedAddress = 0x44ULL;
                const std::uint8_t before = BackingByte(stagedAddress);
                WPJ6_CHECK(StageOne(*h.view, stagedAddress, static_cast<std::uint8_t>(before ^ 0x5A)));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(!allowed);
                WPJ6_CHECK_NOTE(
                    BackingByte(stagedAddress) == before,
                    QStringLiteral("离开被拒绝时，暂存不得已被写进目标（应保持 0x%1，实际 0x%2）")
                        .arg(before, 0, 16)
                        .arg(BackingByte(stagedAddress), 0, 16));
                WPJ6_CHECK(h.view->hexPaneForTest()->overlay().HasPendingPatches());
                ClearInt3AndByte(int3Address);
            }
            // 暂存框选"取消"：int3 框根本不应弹出（不问第二次），暂存原封不动。
            {
                Harness h;
                h.AttachProcess(6303);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                WPJ6_CHECK(StageOne(*h.view, 0x48ULL, 0x98));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::Cancel;
                ArmClicker clicker(QStringLiteral("取消"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                PumpFor(400);
                WPJ6_CHECK(!allowed);
                WPJ6_CHECK_NOTE(!clicker.seen, QStringLiteral("暂存框已取消，不应再弹 int3 框"));
                WPJ6_CHECK(h.view->hexPaneForTest()->overlay().HasPendingPatches());
                ClearInt3AndByte(int3Address);
            }
        }

        void TestLeaveGuardBothApprovedExecutesAfterwards()
        {
            const std::uint64_t int3Address = 0xB0ULL;
            // 丢弃并离开 + 保留补丁继续：放行；暂存被丢弃、芯片清空、int3 补丁保留。
            {
                Harness h;
                h.AttachProcess(6311);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                WPJ6_CHECK(StageOne(*h.view, 0x4CULL, 0x97));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                ArmClicker clicker(QStringLiteral("保留补丁继续"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(allowed);
                WPJ6_CHECK(!h.view->hexPaneForTest()->overlay().HasPendingPatches());
                WPJ6_CHECK_NOTE(!PendingChipVisible(*h.view), QStringLiteral("丢弃并离开之后，待写入芯片必须清掉"));
                WPJ6_CHECK(ks::ui::WorkbenchShared::Instance().Int3().Entries().size() == 1U);
                ClearInt3AndByte(int3Address);
            }
            // 应用并离开 + 保留补丁继续：放行，暂存字节真的写进了目标。
            {
                Harness h;
                h.AttachProcess(6312);
                PumpUntil([]() { return true; }, 10);
                WaitForStageable(h.view->hexPaneForTest(), int3Address);
                ToggleInt3(*h.view, int3Address);
                const std::uint64_t stagedAddress = 0x50ULL;
                const std::uint8_t before = BackingByte(stagedAddress);
                const std::uint8_t wanted = static_cast<std::uint8_t>(before ^ 0x3C);
                WPJ6_CHECK(StageOne(*h.view, stagedAddress, wanted));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::ApplyThenSwitch;
                ArmClicker clicker(QStringLiteral("保留补丁继续"));
                const bool allowed = h.view->requestLeave(ks::ui::LeaveReason::ScopeChange);
                WPJ6_CHECK(clicker.clicked);
                WPJ6_CHECK(allowed);
                WPJ6_CHECK_NOTE(BackingByte(stagedAddress) == wanted, QStringLiteral("应用并离开应真的写入目标"));
                WPJ6_CHECK(!h.view->hexPaneForTest()->overlay().HasPendingPatches());
                ClearInt3AndByte(int3Address);
                // 还原被应用的字节，避免影响后面的测试。
                auto& backend = ConfigureSharedOnce();
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                backend.backing->bytes[stagedAddress - backend.backing->base] = before;
            }
        }

        // ---------------- B4：待写入芯片同步 ----------------

        void TestPendingChipClearedAfterDiscardPaths()
        {
            // 直接 requestLeave(丢弃)：没有随后的身份变化，芯片也必须清掉。
            {
                Harness h;
                h.AttachProcess(6321);
                PumpUntil([]() { return true; }, 10);
                WPJ6_CHECK(StageOne(*h.view, 0x24ULL, 0xAB));
                WPJ6_CHECK_NOTE(PendingChipVisible(*h.view), QStringLiteral("暂存之后待写入芯片应可见（前置条件）"));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                WPJ6_CHECK(h.view->requestLeave(ks::ui::LeaveReason::DockAttachChange));
                PumpFor(60);
                WPJ6_CHECK_NOTE(!PendingChipVisible(*h.view), QStringLiteral("requestLeave(丢弃) 之后待写入芯片必须清掉"));
            }
            // 丢弃 + 随后的身份变化（换范围）：芯片也必须清掉。
            {
                Harness h;
                h.AttachProcess(6322);
                PumpUntil([]() { return true; }, 10);
                WPJ6_CHECK(StageOne(*h.view, 0x20ULL, 0xAA));
                WPJ6_CHECK(PendingChipVisible(*h.view));
                h.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
                ks::ui::NavRequest request;
                request.scope = ksword::memwb::Scope::KernelVirtual;
                request.address = 0xFFFFF78000000000ULL;
                h.view->openAt(request);
                PumpFor(100);
                WPJ6_CHECK_NOTE(
                    !PendingChipVisible(*h.view),
                    QStringLiteral("丢弃并切换范围之后待写入芯片必须清掉"));
            }
            // 不经离开守卫的身份变化（Dock 直接分离/目标进程退出）：叠加层随换目标清空，
            // 芯片也必须同步，不能残留"N 字节待写入"。这条路径只有 handleIdentityChange
            // 自己的刷新能救——守卫里的刷新够不到它。
            {
                Harness h;
                h.AttachProcess(6323);
                PumpUntil([]() { return true; }, 10);
                WPJ6_CHECK(StageOne(*h.view, 0x28ULL, 0xAC));
                WPJ6_CHECK(PendingChipVisible(*h.view));
                h.view->target().onDockDetached();
                PumpFor(100);
                WPJ6_CHECK_NOTE(
                    !h.view->hexPaneForTest()->overlay().HasPendingPatches(),
                    QStringLiteral("目标分离后叠加层应已清空（前置条件）"));
                WPJ6_CHECK_NOTE(
                    !PendingChipVisible(*h.view),
                    QStringLiteral("目标分离（不经守卫）之后待写入芯片必须清掉"));
            }
        }

        // ---------------- B6："切换并跳转"钮 ----------------

        void TestRerouteButtonPresentationAndBackClearsIt()
        {
            Harness h;
            h.AttachProcess(6331);
            PumpUntil([]() { return true; }, 10);
            auto* pane = h.view->hexPaneForTest();
            WPJ6_CHECK(WaitForStageable(pane, 0x10ULL));
            for (int i = 0; i < 3; ++i)
            {
                ks::ui::NavRequest request;
                request.address = 0x10ULL + static_cast<std::uint64_t>(i) * 0x10ULL;
                WPJ6_CHECK(h.view->openAt(request) == ks::ui::NavStatus::Ok);
            }
            ks::ui::NavRequest kernelRequest;
            kernelRequest.address = 0xFFFFF78000003000ULL;
            WPJ6_CHECK(h.view->openAt(kernelRequest) == ks::ui::NavStatus::NeedsScopeSwitch);
            auto* reroute = h.view->rerouteButtonForTest();
            auto* forward = h.view->forwardButtonForTest();
            WPJ6_CHECK(reroute != nullptr && forward != nullptr);
            if (reroute == nullptr || forward == nullptr)
            {
                return;
            }
            WPJ6_CHECK(!reroute->isHidden());
            WPJ6_CHECK_NOTE(
                reroute->text() == QStringLiteral("切换并跳转"),
                QStringLiteral("按钮文字应是「切换并跳转」，实际 %1").arg(reroute->text()));
            WPJ6_CHECK_NOTE(
                reroute->toolButtonStyle() != Qt::ToolButtonIconOnly,
                QStringLiteral("状态条提示点击「切换并跳转」，按钮必须真的显示文字（不能是 IconOnly）"));
            WPJ6_CHECK_NOTE(!reroute->toolTip().isEmpty(), QStringLiteral("按钮悬停说明不得为空"));
            // 比渲染出来的像素而不是 QIcon::cacheKey：每个 QIcon 实例的 cacheKey 本来就不同，
            // 比它是一条恒真的空断言（复核时的变异重放证明了这一点）。
            const QImage rerouteImage = reroute->icon().pixmap(16, 16).toImage();
            const QImage forwardImage = forward->icon().pixmap(16, 16).toImage();
            WPJ6_CHECK_NOTE(!rerouteImage.isNull() && !forwardImage.isNull(), QStringLiteral("两个图标都应能渲染出像素"));
            WPJ6_CHECK_NOTE(
                rerouteImage != forwardImage,
                QStringLiteral("图标不得与「前进」钮相同"));

            // 之后点后退：挂起的"切换并跳转"请求随之作废，按钮必须隐藏。
            emit h.view->backButtonForTest()->clicked();
            WPJ6_CHECK_NOTE(reroute->isHidden(), QStringLiteral("后退之后「切换并跳转」钮必须隐藏"));
        }
    }

    void RunReview2FixTests()
    {
        TestSavedHiddenSidebarStaysHiddenOnWideWindow();
        TestSavedVisibleSidebarFirstShowGeometry();
        TestLoadSettingsAfterShow();
        TestSaveSettingsKeepsPreferenceNotAutoCollapseSideEffect();
        TestEmbeddedSidebarAndLoadSettings();
        TestBackStackWithExplicitPidAndChannel();
        TestLeaveGuardAtomicWhenInt3Cancels();
        TestLeaveGuardBothApprovedExecutesAfterwards();
        TestPendingChipClearedAfterDiscardPaths();
        TestRerouteButtonPresentationAndBackClearsIt();
    }
}
