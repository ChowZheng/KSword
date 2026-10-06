// ============================================================
// wpJ5_tests.Write.cpp
// 作用：覆盖"写入走真实 WriteController+假端口、写入后画布显示新值"、
// "Immediate 模式 Stage 后立即提交"、"Staged 模式只暂存"、"提交期间画布只读
// （hook）"。接线方式见 wpJ5_common.cpp 的 Harness 构造函数注释（canvas
// editStaged→controller.onEditCompleted、controller 的局部重读回调→
// pane.rereadWindow()，两条都是本 Harness 代办的"最小 View"接线）。
// ============================================================

#include "wpJ5_common.h"

namespace wpj5_test
{
    namespace
    {
        // WR1：Immediate 模式下，Stage 一个字节必须立即触发一次完整 Commit
        // （不需要调用方再手动调用任何"应用"），写入落到假端口的后备存储，
        // 并且随后画布（经 rereadWindow 的局部重读）显示新值——不是停在页
        // 缓存的旧字节上。
        void TestImmediateCommitShowsNewValueOnCanvas()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x10));
            h.AttachProcess();
            h.controller.setUiConfirmSuppressed(true); // 进程范围+立即写：ux 策略本就不弹
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0x40, 2000));
            WPJ5_CHECK(h.pane.canvas()->cellStateAt(0x40).value == 0x10);

            // stageBytes 要求地址落在 overlay 的基线窗口内（不是只要求画布
            // 页缓存里有值）——把插入点移到目标地址、flushNow 让
            // BaselineFeeder 真正把窗口喂给 overlay，走的是真实链路而不是
            // 直接手工 LoadBaseline。
            h.pane.canvas()->setCaretAddress(0x40, false, false);
            h.feeder.flushNow();
            WPJ5_CHECK(h.pane.overlay().HasBaseline());

            h.pane.setEditable(true);
            WPJ5_CHECK(h.controller.mode() == ksword::memwb::WriteMode::Immediate);

            QString rejectReason;
            const bool staged = h.pane.canvas()->stageBytes(0x40, QByteArray::fromHex("99"), &rejectReason);
            WPJ5_CHECK_NOTE(staged, QString("stageBytes 被拒绝：%1").arg(rejectReason));

            WPJ5_CHECK_NOTE(h.commitFinishedCount == 1, "Immediate 模式下 Stage 必须恰好触发一次 Commit");
            WPJ5_CHECK(h.backing->writeCallCount >= 1);
            {
                // Commit() 本身同步执行（不变式 1），这一行断言时写入早已真正
                // 落到假端口的后备存储——不需要等待任何事件循环。
                std::lock_guard<std::mutex> lock(h.backing->mutex);
                WPJ5_CHECK_NOTE(
                    h.backing->bytes[0x40] == 0x99,
                    "Commit 必须在函数返回前就把新字节真正写进目标（假端口后备存储）");
            }

            // 局部重读由 rereadRangeCallback_ -> pane.rereadWindow() 驱动，是异步
            // 的（经 WorkbenchPageProvider 的读线程），必须真正等到它落地。
            WPJ5_CHECK_NOTE(
                PumpUntil([&h]() { return h.pane.canvas()->cellStateAt(0x40).value == 0x99; }, 2000),
                "提交之后画布必须显示新值 0x99，不能停在页缓存的旧字节 0x10 上");
        }

        // WR2：Staged 模式只暂存——Stage 之后不应该发生任何写入（端口 Write
        // 调用次数恒为 0），直到调用方显式 commitPendingNow()。
        void TestStagedModeOnlyStages()
        {
            Harness h(0x1000, std::vector<std::uint8_t>(0x1000, 0x20));
            h.AttachProcess();
            h.controller.setUiConfirmSuppressed(true);
            h.SetAddressSpace(0x1000, 0x1FFF);
            WPJ5_CHECK(h.WaitUntilSettled(0x1040, 2000));
            h.pane.canvas()->setCaretAddress(0x1040, false, false);
            h.feeder.flushNow();
            WPJ5_CHECK(h.pane.overlay().HasBaseline());

            const ksword::memwb::ModeSwitchStatus switchStatus =
                h.controller.requestModeSwitch(ksword::memwb::WriteMode::StagedThenApply);
            WPJ5_CHECK(switchStatus == ksword::memwb::ModeSwitchStatus::Switched);

            h.pane.setEditable(true);
            const int writesBefore = h.backing->writeCallCount;

            QString rejectReason;
            const bool staged = h.pane.canvas()->stageBytes(0x1040, QByteArray::fromHex("AB"), &rejectReason);
            WPJ5_CHECK_NOTE(staged, QString("stageBytes 被拒绝：%1").arg(rejectReason));
            WPJ5_CHECK_NOTE(h.commitFinishedCount == 0, "Staged 模式下 Stage 本身绝不触发 Commit");
            WPJ5_CHECK_NOTE(h.backing->writeCallCount == writesBefore, "Staged 模式下 Stage 本身绝不写入端口");

            const ks::ui::CommitAttempt attempt = h.controller.commitPendingNow();
            WPJ5_CHECK(attempt.status == ks::ui::CommitEntryStatus::Started);
            WPJ5_CHECK(attempt.report.outcome == ksword::memwb::CommitOutcome::Committed);
            WPJ5_CHECK_NOTE(h.commitFinishedCount == 1, "显式应用之后才应该发生一次 Commit");
            WPJ5_CHECK(h.backing->writeCallCount > writesBefore);
        }

        // WR3：提交期间画布只读——canvasReadOnlyHook 恰好被调用两次（true 后
        // false），且提交结束后 canvas_->isEditable() 恢复成提交前的状态
        // （本测试提交前后都应为 true）。
        void TestCanvasReadOnlyDuringCommit()
        {
            Harness h(0, std::vector<std::uint8_t>(0x1000, 0x30));
            h.AttachProcess();
            h.controller.setUiConfirmSuppressed(true);
            h.SetAddressSpace(0, 0xFFF);
            WPJ5_CHECK(h.WaitUntilSettled(0x10, 2000));
            h.pane.canvas()->setCaretAddress(0x10, false, false);
            h.feeder.flushNow();
            WPJ5_CHECK(h.pane.overlay().HasBaseline());

            h.pane.setEditable(true);
            WPJ5_CHECK(h.pane.canvas()->isEditable());

            QString rejectReason;
            const bool staged = h.pane.canvas()->stageBytes(0x10, QByteArray(1, char(0x44)), &rejectReason);
            WPJ5_CHECK_NOTE(staged, QString("stageBytes 被拒绝：%1").arg(rejectReason));

            WPJ5_CHECK_NOTE(
                h.readOnlyHookCalls.size() == 2,
                QString("canvasReadOnlyHook 应该恰好被调用两次，实际 %1 次").arg(h.readOnlyHookCalls.size()));
            if (h.readOnlyHookCalls.size() == 2)
            {
                WPJ5_CHECK(h.readOnlyHookCalls[0] == true);
                WPJ5_CHECK(h.readOnlyHookCalls[1] == false);
            }
            WPJ5_CHECK_NOTE(h.pane.canvas()->isEditable(), "提交结束后画布应恢复可编辑（提交前就是可编辑的）");

            // commitSuspendHook 同样应该恰好两次、true 后 false，且 feeder 的
            // suspended 状态在提交结束后恢复为 false。
            WPJ5_CHECK(h.suspendHookCalls.size() == 2);
            WPJ5_CHECK(!h.feeder.isSuspended());
        }
    }

    void RunWriteTests()
    {
        TestImmediateCommitShowsNewValueOnCanvas();
        TestStagedModeOnlyStages();
        TestCanvasReadOnlyDuringCommit();
    }
}
