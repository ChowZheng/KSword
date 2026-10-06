// ============================================================
// wpJ6_tests.Identity.cpp
// 作用：身份变化的离开守卫（N2 一次合并）、守卫否决后会话不变、Reload 不清
//       补丁但身份变化必清补丁。
// ============================================================

#include "wpJ6_common.h"

#include <QKeySequence>
#include <QShortcut>

#include <optional>

namespace wpj6_test
{
    namespace
    {
        // StagePendingEdit：直接经 canvas()->stageBytes 暂存一个补丁，不经
        // 画布的键盘/鼠标输入路径（夹具白盒访问，见 hexPaneForTest 的注释）。
        // stageBytes 要求目标字节"屏幕上看得到值"（已经读到，见 HexCanvas.h
        // "五、编辑"一节），因此先等一次异步页读取真正落地。
        // 默认写入模式是 Immediate——HexCanvas::editStaged 已经接到
        // WriteController::onEditCompleted（见 buildUi），Immediate 模式下
        // stageBytes 会同步触发一次完整提交，补丁不会保持"待写入"状态。本函数
        // 因此先切到 Staged 模式（没有任何待写入补丁时 requestModeSwitch 恒
        // 直接 Switched，不需要经过确认框），让调用方能真正观察到一个暂存态。
        void StagePendingEdit(ks::ui::MemoryWorkbenchView& view, std::uint64_t address, std::uint8_t value)
        {
            auto* pane = view.hexPaneForTest();
            WPJ6_CHECK(pane != nullptr);
            if (pane == nullptr || pane->canvas() == nullptr)
            {
                return;
            }
            auto* controller = view.writeControllerForTest();
            if (controller != nullptr && controller->mode() == ksword::memwb::WriteMode::Immediate)
            {
                const auto switched = controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply);
                WPJ6_CHECK(switched == ksword::memwb::ModeSwitchStatus::Switched);
            }
            const bool settled = WaitForStageable(pane, address);
            WPJ6_CHECK_NOTE(settled, QStringLiteral("地址 0x%1 未能在超时内变得可暂存").arg(address, 0, 16));
            QByteArray bytes(1, static_cast<char>(value));
            QString reason;
            const bool staged = pane->canvas()->stageBytes(address, bytes, &reason);
            WPJ6_CHECK_NOTE(staged, reason);
        }
    }

    // TestIdentityChangeAsksGuardExactlyOnce：有一个暂存补丁时，切换范围触发
    // 的身份变化只应经离开守卫问一次（不是两次或三次）。
    // Wave 3 修复缺陷 3 之后：installLeaveGuard 改用专门的 PromptLeaveWithPending
    // （不再误用 PromptModeSwitch，见该函数的修复说明），本测试同步改成核对
    // leaveWithPendingCallCount/leaveWithPendingDecision。
    void TestIdentityChangeAsksGuardExactlyOnce()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        StagePendingEdit(*harness.view, 0x20ULL, 0xAB);
        WPJ6_CHECK(harness.view->hexPaneForTest()->overlay().HasPendingPatches());

        int writesBefore = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            writesBefore = backend.backing->writeCallCount;
        }

        harness.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
        harness.prompter->leaveWithPendingCallCount = 0;

        ks::ui::NavRequest request;
        request.scope = ksword::memwb::Scope::KernelVirtual;
        request.address = 0xFFFFF78000000000ULL;
        const auto status = harness.view->openAt(request);

        WPJ6_CHECK_NOTE(
            harness.prompter->leaveWithPendingCallCount == 1,
            QStringLiteral("count=%1").arg(harness.prompter->leaveWithPendingCallCount));
        // 脚本答案是 DiscardThenSwitch：必须真的丢弃，不能误判成 ApplyThenSwitch
        // 走去提交——用"假内存端口写入次数恒为 0"核对，只看 NavStatus::Ok 与
        // 范围确实切换这两件事测不出 Apply/Discard 分支被互换（两条分支都能让
        // 离开成功、范围照样切换，必须看"有没有真的写盘"才能分辨）。
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            WPJ6_CHECK_NOTE(
                backend.backing->writeCallCount == writesBefore,
                QStringLiteral("DiscardThenSwitch 不应触发任何端口写入，before=%1 after=%2")
                    .arg(writesBefore)
                    .arg(backend.backing->writeCallCount));
        }
        // 正文参数必须是真实的待写入字节数/块数与一句翻译好的离开原因，不是
        // 空字符串或默认值（修复缺陷 3 的核心：原来传了两个相同的写入模式值，
        // 文案对不上；现在传的是字节数/块数+原因短句）。
        WPJ6_CHECK(harness.prompter->lastLeavePendingBytes > 0);
        WPJ6_CHECK(!harness.prompter->lastLeaveReasonText.isEmpty());
        WPJ6_CHECK(status == ks::ui::NavStatus::Ok);
        WPJ6_CHECK(harness.view->target().session().scope == ksword::memwb::Scope::KernelVirtual);
    }

    // TestGuardRefusalKeepsSessionUnchanged：守卫否决（Cancel）时会话逐字段不
    // 变，画布/overlay 也不被触碰。
    void TestGuardRefusalKeepsSessionUnchanged()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        StagePendingEdit(*harness.view, 0x20ULL, 0xCD);
        const auto beforeSession = harness.view->target().session();
        const auto pendingBefore = harness.view->hexPaneForTest()->overlay().PendingByteCount();

        harness.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::Cancel;
        harness.prompter->leaveWithPendingCallCount = 0;

        ks::ui::NavRequest request;
        request.scope = ksword::memwb::Scope::KernelVirtual;
        request.address = 0xFFFFF78000000000ULL;
        const auto status = harness.view->openAt(request);

        WPJ6_CHECK(harness.prompter->leaveWithPendingCallCount == 1);
        WPJ6_CHECK(status == ks::ui::NavStatus::LeaveRefused);
        const auto afterSession = harness.view->target().session();
        WPJ6_CHECK(afterSession.scope == beforeSession.scope);
        WPJ6_CHECK(afterSession.pid == beforeSession.pid);
        WPJ6_CHECK(afterSession.channel == beforeSession.channel);
        WPJ6_CHECK(harness.view->hexPaneForTest()->overlay().PendingByteCount() == pendingBefore);
    }

    // TestReloadKeepsPatchesIdentityChangeClears：Reload 掩码不清补丁；身份
    // 变化（这里用换通道触发，不需要暂存补丁阻拦）必清补丁。附带核对纯
    // Reload 确实触发了一次真实的"原位重读"（handleReloadOnly 调用
    // hexPane_->rereadWindow()）——只看补丁状态不够：如果 rereadWindow() 被
    // 误删，补丁确实也不会被清（本就不该清），这个判据本身测不出重读这一步
    // 有没有发生，必须另外核对端口 Read 调用次数真的增加了。
    void TestReloadKeepsPatchesIdentityChangeClears()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        StagePendingEdit(*harness.view, 0x20ULL, 0xEF);
        WPJ6_CHECK(harness.view->hexPaneForTest()->overlay().HasPendingPatches());

        int readsBeforeReload = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            readsBeforeReload = backend.backing->readCallCount;
        }

        // 纯 Reload：直接调用 target().requestReload()，不经过任何身份请求。
        harness.view->target().requestReload();
        WPJ6_CHECK_NOTE(
            harness.view->hexPaneForTest()->overlay().HasPendingPatches(),
            QStringLiteral("Reload 不应清补丁"));
        PumpFor(300);
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            WPJ6_CHECK_NOTE(
                backend.backing->readCallCount > readsBeforeReload,
                QStringLiteral("纯 Reload 应该真的触发一次原位重读（端口 Read 调用次数应增加）"));
        }

        // 身份变化：换通道（不需要离开守卫阻拦，直接允许——没有未还原 int3、
        // 这次我们让离开守卫的三选一决定为 DiscardThenSwitch 以便观察清空；
        // 走的是 installLeaveGuard→PromptLeaveWithPending 这条路径，不是
        // PromptModeSwitch，见修复缺陷 3 的说明）。
        harness.prompter->leaveWithPendingDecision = ksword::memwb::ModeSwitchDecision::DiscardThenSwitch;
        const bool ok = harness.view->target().requestChannel(ksword::memwb::Channel::StandardDriver);
        WPJ6_CHECK(ok);
        WPJ6_CHECK_NOTE(
            !harness.view->hexPaneForTest()->overlay().HasPendingPatches(),
            QStringLiteral("身份变化必须清补丁"));
    }

    // TestScopeSwitchRestoresRememberedChannel：ux.md §2 第 1 条"显式记忆"——
    // 切换范围时通道取该范围"上次使用"的记忆，不是自动回退到某个默认值。用
    // 两个范围各自记一个不同的通道，来回切换验证记忆真的生效（不是巧合碰上
    // 同一个默认值）。
    void TestScopeSwitchRestoresRememberedChannel()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* sessionBar = harness.view->sessionBarForTest();
        WPJ6_CHECK(sessionBar != nullptr);
        if (sessionBar == nullptr)
        {
            return;
        }

        // 进程范围记为标准驱动（R0）。
        emit sessionBar->channelRequested(ksword::memwb::Channel::StandardDriver);
        WPJ6_CHECK(harness.view->target().session().channel == ksword::memwb::Channel::StandardDriver);

        // 切到内核范围，记为 HVM（刻意选一个与"内核默认 R0"不同的通道，避免
        // 两个范围碰巧落在同一个默认值上掩盖掉记忆没生效这件事）。
        emit sessionBar->scopeRequested(ksword::memwb::Scope::KernelVirtual);
        emit sessionBar->channelRequested(ksword::memwb::Channel::Hvm);
        WPJ6_CHECK(harness.view->target().session().scope == ksword::memwb::Scope::KernelVirtual);
        WPJ6_CHECK(harness.view->target().session().channel == ksword::memwb::Channel::Hvm);

        // 切回进程范围：应该恢复第一步记的 R0，不是停在刚才内核范围用的 HVM，
        // 也不是悄悄回到默认的 UserMode。
        emit sessionBar->scopeRequested(ksword::memwb::Scope::ProcessVirtual);
        WPJ6_CHECK_NOTE(
            harness.view->target().session().channel == ksword::memwb::Channel::StandardDriver,
            QStringLiteral("切回进程范围应恢复记忆的标准驱动通道，不是停在内核范围用过的 HVM"));
    }

    // TestAddToAddressBookTwiceWarnsOnDuplicate：G4 去重规则——同 targetKey 且
    // 解析后的绝对地址相同视为重复，提示而不阻止。两次按 Ctrl+B（本类不对外
    // 公开 addInsertionPointToAddressBook，只能走真实快捷键这条公开入口）加
    // 同一个地址，断言第二次的状态条摘要里出现了"已在地址簿中"的提示。
    void TestAddToAddressBookTwiceWarnsOnDuplicate()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x50ULL));
        pane->jumpTo(0x50ULL);

        const auto addToBookKeys = harness.view->findChildren<QShortcut*>();
        QShortcut* ctrlB = nullptr;
        for (auto* shortcut : addToBookKeys)
        {
            if (shortcut->key() == QKeySequence(QStringLiteral("Ctrl+B")))
            {
                ctrlB = shortcut;
                break;
            }
        }
        WPJ6_CHECK_NOTE(ctrlB != nullptr, QStringLiteral("未找到 Ctrl+B 快捷键"));
        if (ctrlB == nullptr)
        {
            return;
        }

        emit ctrlB->activated();
        emit ctrlB->activated();

        auto* statusBar = harness.view->statusBarForTest();
        WPJ6_CHECK(statusBar != nullptr);
        if (statusBar != nullptr)
        {
            WPJ6_CHECK_NOTE(
                statusBar->summaryText().contains(QStringLiteral("已在地址簿中")),
                QStringLiteral("重复添加同一地址应提示，而不是悄悄再加一条却什么都不说"));
        }
    }

    // TestProtectionProviderIsQueriedOnInsertionPointChange（零覆盖路径，审核
    // 报告 wpJ6/wave3 B 节）：setProtectionProvider 注入的回调必须在注入时
    // 立即按当前插入点查询一次，并在插入点变化（onHexPaneInsertionPointChanged
    // → refreshProtectionDisplay）后用新地址重新查询——这条接线此前 7 个测试
    // 文件里从未被任何断言覆盖。WorkbenchStatusBar 的保护徽章没有白盒访问器
    // （rebuildSummary 明确把保护段排除在 summaryText() 之外，避免和彩色徽章
    // 重复显示），因此用回调是否被调用、调用时收到的地址是否正确来核对，不
    // 读状态条内部状态。
    void TestProtectionProviderIsQueriedOnInsertionPointChange()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x44ULL));
        pane->jumpTo(0x44ULL);

        int callCount = 0;
        std::uint64_t lastQueriedAddress = 0;
        harness.view->setProtectionProvider(
            [&](std::uint64_t address) -> std::optional<ks::ui::WorkbenchProtectionInfo> {
                ++callCount;
                lastQueriedAddress = address;
                ks::ui::WorkbenchProtectionInfo info;
                info.text = QStringLiteral("RWX");
                info.role = ks::ui::StatusRole::Error;
                return info;
            });
        WPJ6_CHECK_NOTE(callCount >= 1, QStringLiteral("注入回调应立即按当前插入点刷新一次"));
        WPJ6_CHECK(lastQueriedAddress == pane->insertionAddress());

        callCount = 0;
        pane->jumpTo(0x50ULL);
        WPJ6_CHECK_NOTE(
            callCount >= 1,
            QStringLiteral("插入点变化（onHexPaneInsertionPointChanged）应重新查询保护信息"));
        WPJ6_CHECK(lastQueriedAddress == 0x50ULL);

        // 返回 std::nullopt 时不应崩溃，且确实被调用到（不是继续沿用上一个
        // 回调留下的旧值）。
        callCount = 0;
        harness.view->setProtectionProvider(
            [&](std::uint64_t) -> std::optional<ks::ui::WorkbenchProtectionInfo> {
                ++callCount;
                return std::nullopt;
            });
        WPJ6_CHECK(callCount >= 1);
    }

    // TestAttachedProcessInfoProviderIsQueriedWithCurrentPid（零覆盖路径）：
    // setAttachedProcessInfoProvider 必须用当前已附加的 pid 查询，查询结果
    // 用于刷新会话条目标 chip 与确认框正文（refreshTargetDisplays）；
    // WorkbenchSessionBar 的目标 chip 没有白盒访问器，同样用回调调用次数/
    // 收到的参数核对接线确实生效。
    void TestAttachedProcessInfoProviderIsQueriedWithCurrentPid()
    {
        Harness harness;
        harness.AttachProcess(4321U, 1ULL);
        PumpUntil([&]() { return true; }, 10);

        int callCount = 0;
        std::uint32_t lastQueriedPid = 0;
        harness.view->setAttachedProcessInfoProvider(
            [&](std::uint32_t pid) -> std::optional<ks::ui::AttachedProcessDisplayInfo> {
                ++callCount;
                lastQueriedPid = pid;
                ks::ui::AttachedProcessDisplayInfo info;
                info.processName = QStringLiteral("fake.exe");
                info.canReadWrite = true;
                return info;
            });
        WPJ6_CHECK_NOTE(callCount >= 1, QStringLiteral("注入回调应立即刷新一次目标展示"));
        WPJ6_CHECK_NOTE(
            lastQueriedPid == 4321U,
            QStringLiteral("应该用当前已附加的 pid 查询，实际=%1").arg(lastQueriedPid));
    }

    // TestHexPaneEditRejectedShowsReasonInStatusBar（零覆盖路径）：画布拒绝
    // 一次编辑时，onHexPaneEditRejected 必须把拒绝原因真的写进状态条（此前
    // 7 个测试文件里没有一处核对 editRejected 转发到 View 之后的落点，
    // wpJ5_tests.Wiring.cpp 只核对了画布→HexPane 这一段转发，没有核对
    // HexPane→View 这一段）。
    void TestHexPaneEditRejectedShowsReasonInStatusBar()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        auto* statusBar = harness.view->statusBarForTest();
        WPJ6_CHECK(statusBar != nullptr);

        // 显式置为不可编辑（构造后的默认可编辑状态取决于身份变化的时序，
        // 不假设哪一种，直接摆成确定会被拒绝的状态）。
        pane->setEditable(false);
        QString reason;
        const bool staged = pane->canvas()->stageBytes(0x10ULL, QByteArray(1, '\x00'), &reason);
        WPJ6_CHECK(!staged);
        WPJ6_CHECK(!reason.isEmpty());

        WPJ6_CHECK_NOTE(
            statusBar != nullptr && statusBar->summaryText().contains(reason),
            QStringLiteral("editRejected 的原因应该出现在状态条摘要里，实际=%1")
                .arg(statusBar != nullptr ? statusBar->summaryText() : QString()));
    }

    // TestModulesFailedShowsReadResultText（零覆盖路径）：WorkbenchTarget::
    // modulesFailed 必须在状态条留下一句可见提示（装配接口文档 §6 末行要求
    // 的接线），此前 7 个测试文件里没有一处触发过这个信号。
    void TestModulesFailedShowsReadResultText()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* statusBar = harness.view->statusBarForTest();
        WPJ6_CHECK(statusBar != nullptr);

        emit harness.view->target().modulesFailed(false);
        WPJ6_CHECK_NOTE(
            statusBar != nullptr && !statusBar->summaryText().isEmpty(),
            QStringLiteral("modulesFailed 应该在状态条留下一句可见提示"));
    }

    void RunIdentityTests()
    {
        TestIdentityChangeAsksGuardExactlyOnce();
        TestGuardRefusalKeepsSessionUnchanged();
        TestReloadKeepsPatchesIdentityChangeClears();
        TestScopeSwitchRestoresRememberedChannel();
        TestAddToAddressBookTwiceWarnsOnDuplicate();
        TestProtectionProviderIsQueriedOnInsertionPointChange();
        TestAttachedProcessInfoProviderIsQueriedWithCurrentPid();
        TestHexPaneEditRejectedShowsReasonInStatusBar();
        TestModulesFailedShowsReadResultText();
    }
}
