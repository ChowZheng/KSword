// wpJ3_tests.Debounce.cpp
// 作用：验证 WorkbenchBaselineFeeder 的防抖与 hasInFlightRequests 门控——
// noteDirty 的 50 ms 单次防抖（真的是"重置式"防抖，不是节流）、在途页读取期间不
// 起算、flushNow 跳过防抖且取消待定的那一次（不会重复喂两次）、flushNow 在从未
// 调用过 noteDirty 时仍能用默认锚点 0 工作。

#include "wpJ3_common.h"

#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

namespace memwb_wpJ3_test
{
    namespace
    {
        // Fixture：每个测试独立构造一套画布+提供者+叠加层+Feeder+录像机，互不干扰。
        // 地址空间 [0, 0xFFFFFFFF]，插入点固定在地址 0，并显式把页 0 喂成 Valid
        // （覆盖掉 setAddressSpace 自动产生的 Pending 状态），这样"插入点所在页已
        // 落定"这个前提始终成立，测试只关心防抖本身的时序，不受页状态干扰。
        struct Fixture
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;

            Fixture()
            {
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
                DeliverValidPage(canvas, 0, 0x7A);
                feeder.setCanvas(&canvas);
                feeder.setOverlay(&overlay);
                feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
                recorder.Attach(feeder, feeder);
            }
        };

        // ---- 多次 noteDirty 在 50 ms 内连续到达，必须合并成一次喂入。----
        void CheckMultipleNoteDirtyCollapseToOneFeed()
        {
            Fixture fx;
            for (int i = 0; i < 5; ++i)
            {
                fx.feeder.noteDirty(0, 1, false);
            }
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000);
            WPJ3_CHECK_NOTE(fired, QStringLiteral("5 次连续 noteDirty 之后应该在 50ms 防抖后喂入一次"));
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("5 次连续 noteDirty 必须只触发 1 次喂入，不是 5 次"));
        }

        // ---- 真正的"重置式"防抖：第二次 noteDirty 必须把计时器重新从 0 开始算，
        //      不是第一次 noteDirty 之后固定 50ms 就触发（节流语义）。
        //      独立审核第二轮报告 N5：本测试原来在 t≈60ms 取一个"瞬时快照"
        //      （断言此刻 refreshedCount 还是 0），只留 20ms 余量——负载下调度延迟
        //      能让第二次 noteDirty 本身晚到 20ms 以上，产生约 10% 的随机误报（不是
        //      真的防抖出了问题，是测试的时间点卡得太紧，空载连跑 30 次就失败 1 次，
        //      带其它补测的二进制里甚至失败 8 次）。改成不依赖墙钟竞速的写法：第二次
        //      noteDirty 之后直接读 QTimer::remainingTime()——如果计时器被真正重置，
        //      剩余时间应该接近完整的 50ms；如果退化成节流（第一轮 V04 变异：
        //      start() 改成"已在跑则不重启"），剩余时间会是"第一次起算后已经流逝掉
        //      一部分"的那个更小的数，不需要真的等到触发那一刻才能分辨，没有竞速
        //      窗口。----
        void CheckDebounceResetsOnEachCall()
        {
            Fixture fx;
            QTimer* timer = fx.feeder.findChild<QTimer*>();
            WPJ3_CHECK(timer != nullptr);
            if (timer == nullptr) { return; }

            fx.feeder.noteDirty(0, 1, false); // 第一次，若不重置将在 t≈50ms 触发
            QThread::msleep(30);
            fx.feeder.noteDirty(0, 1, false); // 第二次，重置计时器
            WPJ3_CHECK(timer->isActive());
            WPJ3_CHECK_NOTE(
                timer->remainingTime() >= 25,
                QStringLiteral("第二次 noteDirty 必须把计时器重新从 0 开始算，不是节流——剩余时间应接近完整的 50ms"));

            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000);
            WPJ3_CHECK(fired);
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("最终也只应该喂入一次，两次 noteDirty 没有各自独立触发"));
        }

        // ---- 独立审核 F1 指出：上面那条测试在 t≈60ms 取"瞬时快照"只留 20ms 余量，
        //      负载下调度延迟能让第二次 noteDirty 本身晚到 20ms 以上，产生约 10% 的
        //      随机误报（不是真的防抖出了问题，是测试的时间点卡得太紧）。这里补一条
        //      用真实耗时测量代替瞬时快照的版本：调度只会让耗时变长，不会让它变短，
        //      所以不会反过来放过真的缺陷，也不会被调度抖动误报。----
        void CheckRv_DebounceResetsMeasuredInterval()
        {
            Fixture fx;
            fx.feeder.noteDirty(0, 1, false); // 第一次，若不重置将在 t≈50ms 触发
            QThread::msleep(30);
            fx.feeder.noteDirty(0, 1, false); // 第二次，重置计时器
            QElapsedTimer sinceSecond;
            sinceSecond.start();
            const bool fired = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000);
            WPJ3_CHECK(fired);
            WPJ3_CHECK_NOTE(
                sinceSecond.elapsed() >= 40,
                QStringLiteral("第二次 noteDirty 之后应该再等满一个防抖间隔才触发，证明确实被重置过"));
        }

        // ---- hasInFlightRequests 为真时：无论等多久都不应该起算防抖、不应该喂入；
        //      等它变回假之后才重新计时。----
        void CheckInFlightRequestsGating()
        {
            Fixture fx;
            fx.feeder.noteDirty(0, 1, true); // 在途页读取还没结束
            // 等待明显超过 50ms 的防抖间隔，确认计时器确实没有被起算。
            const bool firedWhileInFlight = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 150);
            WPJ3_CHECK_NOTE(
                !firedWhileInFlight,
                QStringLiteral("hasInFlightRequests=true 时，即便等了 150ms 也不该喂入——计时器从未起算"));
            WPJ3_CHECK(fx.recorder.refreshedCount == 0);

            // 换成 hasInFlightRequests=false：这一次才真正起算 50ms 防抖。
            fx.feeder.noteDirty(0, 1, false);
            const bool firedAfter = PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000);
            WPJ3_CHECK(firedAfter);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
        }

        // ---- 触发必须有时间上界：防抖间隔不能被悄悄放大（独立审核 V01 变异：把
        //      50ms 改成 400ms 之后，既有测试全部只断言"最终会不会触发"而不断言
        //      "多久之内触发"，一直等到超时都算通过，完全没有牙）。这里明确断言
        //      必须在远小于 400ms 的时间窗口内触发。----
        void CheckRv_DebounceFiresWithinBound()
        {
            Fixture fx;
            fx.feeder.noteDirty(0, 1, false);
            WPJ3_CHECK_NOTE(
                PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 250),
                QStringLiteral("防抖间隔是 50ms 量级，250ms 内必须已经触发；否则说明间隔被意外放大了"));
        }

        // ---- 防抖定时器必须是单次的（独立审核 V02 变异：setSingleShot(true) 改成
        //      false 之后，既有测试都没有在"喂入一次之后、不再有新脏事件的情况下"
        //      继续等待并断言"没有第二次喂入"，所以周期性定时器的多余触发完全没被
        //      发现）。----
        void CheckRv_TimerIsSingleShot()
        {
            Fixture fx;
            fx.feeder.noteDirty(0, 1, false);
            WPJ3_CHECK(PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000));

            // 喂完之后不再调用 noteDirty，只是让画布状态变一下（换代次），再等一个
            // 远超 50ms 的时间窗口：如果定时器是周期性的，它会在下一个 50ms 刻度
            // 上again 调用 recomputeAndFeed，用旧的 pending 锚点/代次重新判定一次，
            // 产生多余的信号（Keep 时不发信号，所以这里用 refreshedCount/
            // unavailableCount 均不变来证明"确实什么都没再发生"）。
            fx.canvas.refresh();
            PumpUntil([]() { return false; }, 250);
            WPJ3_CHECK_NOTE(
                fx.recorder.unavailableCount == 0 && fx.recorder.refreshedCount == 1,
                QStringLiteral("定时器必须是单次的；周期性定时器会在没有新脏事件时也反复触发判定"));
        }

        // ---- flushNow：跳过防抖立即执行；并且必须取消掉已经起算、尚未到期的那次
        //      防抖定时器，否则稍后它到期还会再喂一次（重复喂入）。----
        void CheckFlushNowSkipsDebounceAndCancelsPending()
        {
            Fixture fx;
            fx.feeder.noteDirty(0, 1, false); // 起算 50ms 防抖
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 0,
                QStringLiteral("noteDirty 本身是异步的，调用后立即检查不应该已经喂入"));

            fx.feeder.flushNow(); // 应该立刻同步执行一次
            WPJ3_CHECK_NOTE(fx.recorder.refreshedCount == 1, QStringLiteral("flushNow 应该立刻同步喂入一次"));

            // 继续等待超过原定的 50ms，确认被取消的那次计时器没有再额外触发一次。
            PumpUntil([]() { return false; }, 200);
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("flushNow 必须取消掉待定的防抖定时器，否则这里会变成 2 次"));
        }

        // ---- 独立审核 V03 指出上一条测试实际没有牙：被取消的旧定时器即便到期，
        //      第二次判定也恒为 Keep（位置/代次都没变），不会被 refreshedCount 增量
        //      发现。这里在 flushNow 之后立刻换代次（canvas.refresh()），如果旧定
        //      时器没被真正取消，它到期时会拿着 flushNow 之前记下的 pending 代次去
        //      判定——与 refresh 之后的新代次不一致，恰好触发可观察的
        //      baselineUnavailable 或多余的一次喂入。----
        void CheckRv_FlushNowCancelsPendingTimerObservably()
        {
            Fixture fx;
            fx.feeder.noteDirty(0, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            fx.canvas.refresh(); // 换代次，不调用 noteDirty：旧定时器若没取消，到期后仍用旧 pending 值判定
            PumpUntil([]() { return false; }, 250);
            WPJ3_CHECK_NOTE(
                fx.recorder.unavailableCount == 0 && fx.recorder.refreshedCount == 1,
                QStringLiteral("flushNow 必须真正取消待定的防抖定时器，而不仅仅是让它的触发结果恰好是 Keep"));
        }

        // ---- 独立审核 D4 修复后的新契约：flushNow 在从未调用过 noteDirty 时必须是
        //      空操作，不能再用没有意义的默认锚点/代次 0 去判定。旧契约（本测试的
        //      历史版本）曾要求"用默认锚点 0 完成一次喂入"——那在地址 0 恰好落在
        //      当前地址空间内时凑巧能用，但换到内核这类不含地址 0 的地址空间后，
        //      会报出一个虚假的"该处尚未读取"（独立审核报告 D4，探针 Q6）。----
        void CheckFlushNowWithoutPriorNoteDirty()
        {
            Fixture fx;
            // 从未调用过 fx.feeder.noteDirty(...)。
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 0 && fx.recorder.unavailableCount == 0,
                QStringLiteral("从未 noteDirty 过时 flushNow 必须是空操作，不能用没有意义的默认锚点 0 去判定（D4）"));

            // 真正的 noteDirty 到来之后，flushNow 才应该正常工作。
            fx.feeder.noteDirty(0, fx.canvas.sourceRevision(), false);
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("真正调用过 noteDirty 之后，flushNow 必须能正常喂入"));
            WPJ3_CHECK(fx.recorder.lastBase == 0);
        }

        // ---- noteDirty/flushNow 在 canvas 或 overlay 任一为空时必须是完全空操作：
        //      不崩溃、不记录、不喂入（即便之后把指针补上再 flushNow 也不会用到
        //      之前那次被忽略的脏事件）。----
        void CheckNoteDirtyIgnoredWhenCanvasOrOverlayMissing()
        {
            // 场景 1：canvas 未设置。
            {
                ksword::memwb::MemoryDiffOverlay overlay;
                WorkbenchBaselineFeeder feeder;
                BaselineRecorder recorder;
                feeder.setOverlay(&overlay); // 只设 overlay，canvas 留空
                recorder.Attach(feeder, feeder);
                feeder.noteDirty(123, 1, false);
                feeder.flushNow();
                PumpUntil([]() { return false; }, 100);
                WPJ3_CHECK_NOTE(recorder.refreshedCount == 0, QStringLiteral("canvas 为空时不应该有任何喂入"));
                WPJ3_CHECK(recorder.unavailableCount == 0);
            }
            // 场景 2：overlay 未设置。
            {
                HexCanvas canvas;
                NoOpPageProvider provider;
                SetupCanvas(canvas, provider, 0, 0xFFFFULL);
                WorkbenchBaselineFeeder feeder;
                BaselineRecorder recorder;
                feeder.setCanvas(&canvas); // 只设 canvas，overlay 留空
                recorder.Attach(feeder, feeder);
                feeder.noteDirty(0, 1, false);
                feeder.flushNow();
                PumpUntil([]() { return false; }, 100);
                WPJ3_CHECK_NOTE(recorder.refreshedCount == 0, QStringLiteral("overlay 为空时不应该有任何喂入"));
            }
        }
    }

    void RunDebounceTests()
    {
        CheckMultipleNoteDirtyCollapseToOneFeed();
        CheckDebounceResetsOnEachCall();
        CheckRv_DebounceResetsMeasuredInterval();
        CheckRv_DebounceFiresWithinBound();
        CheckRv_TimerIsSingleShot();
        CheckInFlightRequestsGating();
        CheckFlushNowSkipsDebounceAndCancelsPending();
        CheckRv_FlushNowCancelsPendingTimerObservably();
        CheckFlushNowWithoutPriorNoteDirty();
        CheckNoteDirtyIgnoredWhenCanvasOrOverlayMissing();
    }
}
