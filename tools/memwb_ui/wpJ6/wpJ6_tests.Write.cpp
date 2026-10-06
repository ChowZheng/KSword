// ============================================================
// wpJ6_tests.Write.cpp
// 作用：写入 Immediate/Staged 端到端（假端口）；确认框期间的重入不嵌套提交
//       （D3/D4 的装配层落点——View 自己不新增重入保护，但接线不能破坏
//       WorkbenchWriteController 已有的 commitDepth_ 保护）。
// Wave 3 复核新增（审核报告 wpJ6/wave3 B 节列出的零覆盖路径）：
// writeFailureText 信号、setGlobalSkipDangerousConfirmProvider、地址簿
// PendingStage 端到端（P4 探针并入）。
// ============================================================

#include "wpJ6_common.h"

#include "../../../Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchShared.h"

#include <cstdio>
#include <optional>

namespace wpj6_test
{
    // TestImmediateWriteEndToEnd：立即模式下 stageBytes 应同步触发一次完整
    // 提交，假端口真的被写入，重读能看到新值。
    void TestImmediateWriteEndToEnd()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x30ULL));

        int writesBefore = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            writesBefore = backend.backing->writeCallCount;
        }

        QString reason;
        const bool staged = pane->canvas()->stageBytes(0x30ULL, QByteArray(1, '\x42'), &reason);
        WPJ6_CHECK_NOTE(staged, reason);

        int writesAfter = 0;
        std::uint8_t backingValue = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            writesAfter = backend.backing->writeCallCount;
            backingValue = backend.backing->bytes[0x30ULL - backend.backing->base];
        }
        WPJ6_CHECK_NOTE(writesAfter == writesBefore + 1, QStringLiteral("立即模式应恰好触发一次端口写入"));
        WPJ6_CHECK_NOTE(backingValue == 0x42, QStringLiteral("假内存应已写入新值"));
        WPJ6_CHECK(!pane->overlay().HasPendingPatches());

        // 状态条"写入结果"段必须反映这次真实提交的 CommitReport（块数/字节
        // 数），不能是默认构造的空报告——onWriteControllerCommitFinished 从
        // lastCommitReport_ 这个成员读数据，commitFinished 信号的 lambda 必须
        // 真的把参数存进去；只核对假内存与 overlay 状态测不出这一步被绕过（那
        // 两处都不经过 lastCommitReport_）。真实 Committed 分支才会拼"共 N
        // 字节（M 块）"，NoChange（默认报告的 outcome）不会提到"块"。
        auto* statusBar = harness.view->statusBarForTest();
        WPJ6_CHECK(statusBar != nullptr);
        if (statusBar != nullptr)
        {
            WPJ6_CHECK_NOTE(
                statusBar->summaryText().contains(QStringLiteral("块")),
                QStringLiteral("写入结果段应反映真实提交结果，实际：%1").arg(statusBar->summaryText()));
        }
    }

    // TestStagedWriteOnlyStagesUntilApply：暂存模式下 stageBytes 只暂存，端口
    // 写入次数恒为 0，直到点"应用"（commitPendingNow）才真正写入。
    void TestStagedWriteOnlyStagesUntilApply()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        auto* controller = harness.view->writeControllerForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x34ULL));
        WPJ6_CHECK(controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply) ==
                   ksword::memwb::ModeSwitchStatus::Switched);

        int writesBefore = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            writesBefore = backend.backing->writeCallCount;
        }

        QString reason;
        const bool staged = pane->canvas()->stageBytes(0x34ULL, QByteArray(1, '\x55'), &reason);
        WPJ6_CHECK_NOTE(staged, reason);
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            WPJ6_CHECK_NOTE(
                backend.backing->writeCallCount == writesBefore,
                QStringLiteral("暂存模式下 stageBytes 不应触发任何端口写入"));
        }
        WPJ6_CHECK(pane->overlay().HasPendingPatches());

        const auto attempt = controller->commitPendingNow();
        WPJ6_CHECK(attempt.status == ks::ui::CommitEntryStatus::Started);
        WPJ6_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            WPJ6_CHECK(backend.backing->writeCallCount == writesBefore + 1);
        }
        WPJ6_CHECK(!pane->overlay().HasPendingPatches());

        controller->requestModeSwitch(ksword::memwb::WriteMode::Immediate);
    }

    // TestReentrancyDuringConfirmDoesNotNestCommit：Commit() 进行期间
    // （commitDepth_>0，用 canvasReadOnlyHook_ 恰好在这段时间内把画布置为
    // 只读这一事实做锚点——装配层把这个钩子接到了 hexPane_->setEditable，
    // HexCanvas::editableChanged(false) 发生的时刻就是"确认框/审计等嵌套
    // 事件循环仍可能在转"的那一刻）重入触发第二次 onEditCompleted，必须被
    // commitDepth_ 拒绝为 Busy（发 commitRejectedBusy），不嵌套执行、不吞掉
    // 用户这次编辑（字节仍留在 overlay 里，下一次正常提交会带上它）。
    void TestReentrancyDuringConfirmDoesNotNestCommit()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        auto* controller = harness.view->writeControllerForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x38ULL));
        WPJ6_CHECK(WaitForStageable(pane, 0x39ULL));

        int busyCount = 0;
        QObject::connect(controller, &ks::ui::WorkbenchWriteController::commitRejectedBusy, [&busyCount](QString) {
            ++busyCount;
        });

        bool reentrantStaged = false;
        QObject::connect(pane->canvas(), &ks::ui::HexCanvas::editableChanged, [&](bool editable) {
            if (!editable && !reentrantStaged)
            {
                reentrantStaged = true;
                QString innerReason;
                // 此刻 commitDepth_ 必然 > 0（canvasReadOnlyHook_ 刚把画布置为
                // 只读，恰好对应最外层 Commit 开始的那一刻）；画布此刻本身也是
                // "只读"，stageBytes 会先被只读挡住——用写控制器的
                // onEditCompleted 更贴近真实重入路径：直接在 overlay 里暂存
                // （绕开画布只读检查，模拟"暂存动作已经落进 overlay，只是
                // editStaged 的处理函数被重入调用"这个更底层的时序）。
                pane->overlay().Stage(0x39ULL, std::vector<std::uint8_t>{0x22});
                controller->onEditCompleted();
                Q_UNUSED(innerReason);
            }
        });

        // 立即模式（默认）下 stageBytes 本身就会同步触发 Commit()；画布进入
        // 只读的那一刻（canvasReadOnlyHook_(true)）正是 commitDepth_>0 的窗口。
        WPJ6_CHECK(controller->mode() == ksword::memwb::WriteMode::Immediate);
        QString reason;
        WPJ6_CHECK(pane->canvas()->stageBytes(0x38ULL, QByteArray(1, '\x11'), &reason));

        WPJ6_CHECK(reentrantStaged);
        // 重入那一次编辑应该被 commitDepth_ 拒绝（发了 commitRejectedBusy），
        // 不是被静默吞掉、也不是被嵌套执行成另一次独立提交。
        WPJ6_CHECK_NOTE(busyCount >= 1, QStringLiteral("重入编辑应触发 commitRejectedBusy"));
    }

    // TestWriteFailureTextSignalFiresExactlyOnceOnFailure（零覆盖路径，审核
    // 报告 wpJ6/wave3 B 节）：写入失败（outcome 既非 Committed 也非 NoChange，
    // 且 failureText 非空）时 writeFailureText 必须发一次且仅一次；此前 7 个
    // 测试文件里没有任何一处订阅过这个信号。用"暂存后、提交前把假内存后备
    // 存储收缩到覆盖不了目标地址"制造一次真实的端口写入失败，不伪造
    // CommitReport。
    void TestWriteFailureTextSignalFiresExactlyOnceOnFailure()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* pane = harness.view->hexPaneForTest();
        auto* controller = harness.view->writeControllerForTest();
        const std::uint64_t address = 0x3EULL;
        WPJ6_CHECK(WaitForStageable(pane, address));
        WPJ6_CHECK(controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply) ==
                   ksword::memwb::ModeSwitchStatus::Switched);

        QString stageReason;
        WPJ6_CHECK(pane->canvas()->stageBytes(address, QByteArray(1, '\x77'), &stageReason));

        int signalCount = 0;
        QString lastFailureText;
        QObject::connect(harness.view.get(), &ks::ui::MemoryWorkbenchView::writeFailureText,
            [&](const QString& text) {
                ++signalCount;
                lastFailureText = text;
            });

        std::size_t originalSize = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            originalSize = backend.backing->bytes.size();
            backend.backing->bytes.resize(static_cast<std::size_t>(address));
        }

        const auto attempt = controller->commitPendingNow();
        WPJ6_CHECK_NOTE(
            attempt.report.outcome != ksword::memwb::CommitOutcome::Committed &&
                attempt.report.outcome != ksword::memwb::CommitOutcome::NoChange,
            QStringLiteral("收缩后备存储后提交应该失败，实际 outcome=%1")
                .arg(static_cast<int>(attempt.report.outcome)));
        WPJ6_CHECK_NOTE(
            signalCount == 1, QStringLiteral("writeFailureText 应恰好发一次，实际 %1 次").arg(signalCount));
        WPJ6_CHECK(!lastFailureText.isEmpty());

        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            backend.backing->bytes.resize(originalSize);
        }
        controller->requestModeSwitch(ksword::memwb::WriteMode::Immediate);
    }

    // TestGlobalSkipDangerousConfirmProviderAffectsSuppression（零覆盖路径）：
    // 注入的"全局跳过危险确认"开关必须真的参与 MemoryWritePolicy::Decide 的
    // suppressed 判断，并转发给 writeController_->setUiConfirmSuppressed；
    // 未注入时恒 false（安全默认）。用内核范围+立即写入这个 RiskyImmediate
    // 组合核对：未注入/关闭时必须要求确认，打开后必须真的抑制。
    void TestGlobalSkipDangerousConfirmProviderAffectsSuppression()
    {
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        auto* controller = harness.view->writeControllerForTest();
        WPJ6_CHECK(controller != nullptr);
        if (controller == nullptr)
        {
            return;
        }

        const bool scopeOk = harness.view->target().requestScope(ksword::memwb::Scope::KernelVirtual);
        WPJ6_CHECK(scopeOk);
        WPJ6_CHECK_NOTE(
            !controller->uiConfirmSuppressed(),
            QStringLiteral("未注入全局跳过开关时，内核范围立即写入（RiskyImmediate）应要求确认"));

        harness.view->setGlobalSkipDangerousConfirmProvider([]() { return true; });
        WPJ6_CHECK_NOTE(
            controller->uiConfirmSuppressed(), QStringLiteral("全局跳过开关打开后应该真的抑制了确认"));

        harness.view->setGlobalSkipDangerousConfirmProvider([]() { return false; });
        WPJ6_CHECK_NOTE(
            !controller->uiConfirmSuppressed(), QStringLiteral("关掉全局跳过开关后应恢复要求确认"));
    }

    // TestAddressBookValueEditEndToEndWritesRealBytes（并入审核报告 P4 探针，
    // 零覆盖路径）：AddressBookPanel::valueEditRequested →
    // onAddressBookValueEditRequested → beginPendingStage 的完整端到端链路，
    // 此前 7 个测试文件里 0 次出现 beginPendingStage/valueEditRequested 字样。
    // 地址故意选在画布默认可见窗口之外，核对 PendingStage 票据真的负责移动
    // 窗口覆盖——真值核对假内存字节，不只看 overlay/界面状态。
    void TestAddressBookValueEditEndToEndWritesRealBytes()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess();
        PumpUntil([&]() { return true; }, 10);

        const auto& session = harness.view->target().session();
        const std::string targetKey =
            "pid:" + std::to_string(session.pid) + "@" + std::to_string(session.processCreateTime100ns);
        const std::uint64_t targetAddress = 0x1F000ULL;
        const auto draft = ksword::memwb::MemoryAddressBook::FromAbsolute(
            targetKey, ksword::memwb::EntryKind::Watch, targetAddress);
        WPJ6_CHECK(draft.has_value());
        if (!draft.has_value())
        {
            return;
        }
        auto& store = ks::ui::WorkbenchShared::Instance().AddressBook();
        const std::uint64_t entryId = store.add(*draft);
        WPJ6_CHECK(entryId != 0);

        auto* panel = harness.view->addressBookPanelForTest();
        WPJ6_CHECK(panel != nullptr);
        if (panel == nullptr)
        {
            return;
        }
        // EncodeValue 对整数类型按"有无 0x/0X 前缀"决定进制（见
        // MemoryValueDecode.cpp::ParseInteger），不带前缀按十进制解析——"7F"
        // 没有前缀会被当成十进制解析，'F' 不是合法十进制数字直接 BadNumber；
        // 必须传 "0x7F" 才会真的按十六进制解析成 0x7F。
        emit panel->valueEditRequested(entryId, ksword::memwb::ValueType::Hex8, QStringLiteral("0x7F"));

        const bool written = PumpUntil(
            [&]() {
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                return backend.backing->bytes[static_cast<std::size_t>(targetAddress - backend.backing->base)] ==
                    0x7F;
            },
            3000);
        WPJ6_CHECK_NOTE(
            written, QStringLiteral("窗口外地址的地址簿编辑应该最终真的写入假内存（PendingStage 移动窗口覆盖）"));

        store.removeMany({entryId});
    }

    // 地址簿异步暂存成功只说明 overlay 接收了编辑；暂存模式和立即提交失败都
    // 不能被 pendingStageResolved 的后续信号覆盖为“已经写入”。
    void TestPendingStagePreservesActualWriteFeedback()
    {
        auto& backend = ConfigureSharedOnce();
        for (int scenario = 0; scenario < 2; ++scenario)
        {
            Harness harness;
            harness.AttachProcess();
            auto* pane = harness.view->hexPaneForTest();
            auto* controller = harness.view->writeControllerForTest();
            auto* statusBar = harness.view->statusBarForTest();
            const std::uint64_t address = 0x58ULL + static_cast<std::uint64_t>(scenario);
            WPJ6_CHECK(WaitForStageable(pane, address));

            int writesBefore = 0;
            std::uint8_t oldValue = 0;
            {
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                writesBefore = backend.backing->writeCallCount;
                oldValue = backend.backing->bytes[static_cast<std::size_t>(address - backend.backing->base)];
                if (scenario == 1)
                {
                    // 在已读取的基线之后修改目标，触发真实 Commit 的原值冲突。
                    backend.backing->bytes[static_cast<std::size_t>(address - backend.backing->base)] =
                        static_cast<std::uint8_t>(oldValue ^ 1U);
                }
            }
            if (scenario == 0)
            {
                WPJ6_CHECK(controller->requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply) ==
                    ksword::memwb::ModeSwitchStatus::Switched);
            }

            int resolvedCount = 0;
            bool stageOk = false;
            int failures = 0;
            QObject::connect(controller, &ks::ui::WorkbenchWriteController::pendingStageResolved,
                [&](quint64, bool ok, QString) { ++resolvedCount; stageOk = ok; });
            QObject::connect(harness.view.get(), &ks::ui::MemoryWorkbenchView::writeFailureText,
                [&](QString) { ++failures; });

            const char edited = static_cast<char>(oldValue ^ 0x80U);
            WPJ6_CHECK(controller->beginPendingStage(address, QByteArray(1, edited)) != 0);
            WPJ6_CHECK(resolvedCount == 1 && stageOk);
            WPJ6_CHECK(pane->overlay().HasPendingPatches());
            WPJ6_CHECK_NOTE(
                !statusBar->summaryText().contains(QStringLiteral("地址簿编辑已写入")),
                QStringLiteral("暂存成功不能覆盖真实写入状态：%1").arg(statusBar->summaryText()));
            WPJ6_CHECK(scenario == 0 ? failures == 0 : failures == 1);
            {
                std::lock_guard<std::mutex> lock(backend.backing->mutex);
                WPJ6_CHECK(backend.backing->writeCallCount == writesBefore);
                backend.backing->bytes[static_cast<std::size_t>(address - backend.backing->base)] = oldValue;
            }
        }
    }

    // 全局地址簿能显示其它目标的条目，但编辑与跳转不能把这些地址解释成当前 PID。
    void TestAddressBookRejectsDifferentTarget()
    {
        auto& backend = ConfigureSharedOnce();
        Harness harness;
        harness.AttachProcess(5101, 1);
        const auto session = harness.view->target().session();
        const std::string targetKey =
            "pid:" + std::to_string(session.pid) + "@" + std::to_string(session.processCreateTime100ns);
        const auto draft = ksword::memwb::MemoryAddressBook::FromAbsolute(
            targetKey, ksword::memwb::EntryKind::Watch, 0x1E000ULL);
        WPJ6_CHECK(draft.has_value());
        if (!draft.has_value())
        {
            return;
        }
        auto& store = ks::ui::WorkbenchShared::Instance().AddressBook();
        const std::uint64_t entryId = store.add(*draft);
        WPJ6_CHECK(entryId != 0);
        harness.AttachProcess(5102, 2);
        auto* pane = harness.view->hexPaneForTest();
        WPJ6_CHECK(WaitForStageable(pane, 0x60ULL));
        pane->canvas()->setCaretAddress(0x60ULL);
        const std::uint64_t caretBefore = pane->canvas()->caretAddress();
        auto* panel = harness.view->addressBookPanelForTest();
        auto* statusBar = harness.view->statusBarForTest();
        int writesBefore = 0;
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            writesBefore = backend.backing->writeCallCount;
        }
        emit panel->valueEditRequested(entryId, ksword::memwb::ValueType::Hex8, QStringLiteral("0x7F"));
        WPJ6_CHECK_NOTE(statusBar->summaryText().contains(QStringLiteral("目标身份与请求不一致")),
            statusBar->summaryText());
        emit panel->jumpRequested(entryId);
        PumpFor(100);
        WPJ6_CHECK(pane->canvas()->caretAddress() == caretBefore);
        WPJ6_CHECK(!pane->overlay().HasPendingPatches());
        WPJ6_CHECK_NOTE(statusBar->summaryText().contains(QStringLiteral("目标身份与请求不一致")),
            statusBar->summaryText());
        {
            std::lock_guard<std::mutex> lock(backend.backing->mutex);
            WPJ6_CHECK(backend.backing->writeCallCount == writesBefore);
        }
        store.removeMany({entryId});
    }

    void RunWriteTests()
    {
        TestImmediateWriteEndToEnd();
        TestStagedWriteOnlyStagesUntilApply();
        TestReentrancyDuringConfirmDoesNotNestCommit();
        TestWriteFailureTextSignalFiresExactlyOnceOnFailure();
        TestGlobalSkipDangerousConfirmProviderAffectsSuppression();
        TestAddressBookValueEditEndToEndWritesRealBytes();
        TestPendingStagePreservesActualWriteFeedback();
        TestAddressBookRejectsDifferentTarget();
    }
}
