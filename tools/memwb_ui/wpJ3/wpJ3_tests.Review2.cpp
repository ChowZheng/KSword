// wpJ3_tests.Review2.cpp
// 作用：第二轮独立审核报告 §7 给出的补测（本文件按本包现有命名风格誊写并接入
// 夹具，断言逻辑与审核者原文一致，未改动判定条件）。
// 两组：
// - RunReview2Tests：核心组（10 个函数），对修复后的当前实现全部应该通过，专门
//   堵审核者新变异 C1/C2/C2b/C7b/C8/C9/C13/C14/C42（§5）与 V04 节流重放（§3）。
// - RunReview2DefectTests：第二轮确认的新缺陷 N1（恢复无视在途位）/N3（flushNow
//   不受挂起门控）/N2（恢复后用挤页后的旧缓存重算，不含挂起期间脏事件的变体）
//   的回归测试——本次修复之前会失败，修复之后应该全部通过，故转绿后并入默认
//   运行（本文件 main.cpp 两组都调用，不再单独区分"预期失败的缺陷组"）。
// 另外补一条本包自己的回归测试（CheckWpJ3_DirtySinceSuspendDoesNotLeakAcrossCycles），
// 验证"挂起期间是否有需要补回的脏事件"这个标记不会跨越两轮独立的挂起/恢复周期
// 残留——这是本次修复新增状态机的边界情形，审核者的补测没有单独覆盖。

#include "wpJ3_common.h"

#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <string>
#include <vector>

namespace memwb_wpJ3_test
{
    namespace
    {
        // R2Fx：本文件专用 Fixture，地址空间 [0, 0xFFFFFFFF]，测试页放在远离地址 0
        // 的 kBase 附近（与 Regression.cpp/Decision.cpp 的做法一致），避开
        // setAddressSpace 自动产生的 Pending 页干扰。
        struct R2Fx
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            static constexpr std::uint64_t kBase = 0x60000000ULL;
            R2Fx()
            {
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
                feeder.setCanvas(&canvas);
                feeder.setOverlay(&overlay);
                feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
                recorder.Attach(feeder, feeder);
            }
            // Page：第 index 页（可正可负）的起始地址。
            static std::uint64_t Page(int index) { return kBase + static_cast<std::uint64_t>(index) * kPageBytes; }
        };

        // ====================== 核心组：堵审核者新变异 C 系列 ======================

        // R2-1：恢复必须按一个防抖间隔起算（堵 C2、C2b：恢复时同步重算/用 start(0)
        // 不留防抖窗口）；只断言下界，调度只会让耗时更长，不会让它变短，所以不会
        // 反过来放过真的缺陷。同时这是本类新增 dirtySinceSuspend_ 状态位最直接的
        // 验证：noteDirty 之后立刻挂起（此时防抖定时器还在跑，尚未到期），恢复必须
        // 把这次被打断的判定补回来，不能因为"挂起期间没有新的 noteDirty"而误判成
        // "什么都没发生"。
        void CheckR2_ResumeWaitsOneDebounceInterval()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.setSuspended(true);
            QElapsedTimer sinceResume;
            sinceResume.start();
            fx.feeder.setSuspended(false);
            WPJ3_CHECK_NOTE(fx.recorder.refreshedCount == 0, QStringLiteral("恢复不得同步重算"));
            WPJ3_CHECK(PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000));
            WPJ3_CHECK_NOTE(sinceResume.elapsed() >= 40, QStringLiteral("恢复后要再等满一个防抖间隔才喂入"));
        }

        // R2-2：真实提交序——先成功喂入一次并 AcceptWrite 标记 SelfWritten，随后
        // 挂起、挂起期间大量页涌入挤掉窗口页但没有任何新的 noteDirty，恢复后紧跟一次
        // 在途的 noteDirty（局部重读刚开始，还没读完）。恢复本身不应该凭"挂起前就有
        // 过一次有效请求"这条旧线索重新喂入一次（那会被挤页后的旧内容覆盖刚确认的
        // 基线字节），必须同时看"这一轮挂起期间到底有没有发生需要补回的事"（堵 C2）。
        void CheckR2_ResumeThenInFlightCancelsRearmedTimer()
        {
            R2Fx fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 4});
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            DeliverValidPage(fx.canvas, R2Fx::Page(1), 0x20);
            const std::uint64_t target = R2Fx::Page(1) + 8;
            fx.feeder.noteDirty(target, 1, false);
            fx.feeder.flushNow();
            ksword::memwb::DiffBlock block;
            block.address = target;
            block.before = {0x20};
            block.after = {0x77};
            WPJ3_CHECK(fx.overlay.AcceptWrite(block, {0x77}) == ksword::memwb::AcceptWriteStatus::Ok);
            fx.feeder.setSuspended(true);
            for (int index = 0; index < 255; ++index)
            {
                DeliverValidPage(fx.canvas, R2Fx::kBase + 0x10000000ULL + static_cast<std::uint64_t>(index) * kPageBytes, 0x33);
            }
            fx.feeder.setSuspended(false);
            fx.feeder.noteDirty(target, 1, true);
            PumpUntil([]() { return false; }, 150);
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);
            WPJ3_CHECK(fx.overlay.BaselineByte(target).value_or(0) == 0x77);
            WPJ3_CHECK(fx.overlay.ChangeKind(target) == ksword::memwb::ByteChangeKind::SelfWritten);
        }

        // R2-3：对账必须发现"只有 base 变了"（堵 C7b：去掉 BaseAddress 分量、C9：
        // base/size 由 || 改 &&）。
        void CheckR2_ReconcileDetectsBaseOnlyChange()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.overlay.BaseAddress() == R2Fx::Page(0) && fx.overlay.BaselineSize() == kPageBytes);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(kPageBytes), 0x10);
            std::vector<std::uint8_t> mask(static_cast<std::size_t>(kPageBytes), 1);
            fx.overlay.LoadBaseline(std::string(), R2Fx::Page(9), bytes, mask); // 同身份、同大小、不同 base
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(fx.overlay.BaseAddress() == R2Fx::Page(0), QStringLiteral("只有 base 变了也必须对账失效并重喂"));
        }

        // R2-4：对账必须发现"只有大小变了"（堵 C8：去掉 BaselineSize 分量、C9）。
        void CheckR2_ReconcileDetectsSizeOnlyChange()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.flushNow();
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(2 * kPageBytes), 0x10);
            std::vector<std::uint8_t> mask(static_cast<std::size_t>(2 * kPageBytes), 1);
            fx.overlay.LoadBaseline(std::string(), R2Fx::Page(0), bytes, mask); // 同身份、同 base，大小翻倍
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(fx.overlay.BaselineSize() == kPageBytes, QStringLiteral("只有大小变了也必须对账失效并重喂"));
        }

        // R2-5：setPolicy 只让记录失效，不丢待处理请求（堵 C13：setPolicy 也清
        // pendingValid_）。
        void CheckR2_SetPolicyKeepsPendingRequest()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 8});
            WPJ3_CHECK_NOTE(
                PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000),
                QStringLiteral("setPolicy 不应吞掉此前已起算的脏事件"));
        }

        // R2-6：S2 诊断必须只在 pageSize 真不一致时告警（堵 C14：条件取反）。
        int g_r2Warnings = 0;
        void R2CountingHandler(QtMsgType type, const QMessageLogContext&, const QString&)
        {
            if (type == QtWarningMsg) { ++g_r2Warnings; }
        }
        void CheckR2_PolicyPageSizeMismatchWarns()
        {
            R2Fx fx;
            const QtMessageHandler previous = qInstallMessageHandler(R2CountingHandler);
            g_r2Warnings = 0;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 4});
            const int goodWarnings = g_r2Warnings;
            g_r2Warnings = 0;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{8192, 4});
            const int badWarnings = g_r2Warnings;
            qInstallMessageHandler(previous);
            WPJ3_CHECK(goodWarnings == 0);
            WPJ3_CHECK(badWarnings >= 1);
        }

        // R2-7：未挂起时 setSuspended(false) 必须是空操作（堵 C1：幂等守卫删除）。
        void CheckR2_ResumeWithoutSuspendIsNoOp()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.flushNow();
            fx.canvas.refresh(); // 窗口页不再落定：若被误判定就会发 baselineUnavailable
            fx.feeder.setSuspended(false);
            PumpUntil([]() { return false; }, 200);
            WPJ3_CHECK(fx.recorder.unavailableCount == 0 && fx.recorder.refreshedCount == 1);
        }

        // R2-8：挂起期间的脏事件必须被记住（堵 C42：挂起期间 noteDirty 不更新待处理
        // 三项）。
        void CheckR2_DirtyDuringSuspensionIsRemembered()
        {
            R2Fx fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 1});
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            DeliverValidPage(fx.canvas, R2Fx::Page(1), 0x20);
            fx.feeder.noteDirty(R2Fx::Page(0) + 5, 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1 && fx.recorder.lastBase == R2Fx::Page(0));
            fx.feeder.setSuspended(true);
            fx.feeder.noteDirty(R2Fx::Page(1) + 5, 1, false);
            fx.feeder.setSuspended(false);
            WPJ3_CHECK(PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000));
            WPJ3_CHECK(fx.recorder.lastBase == R2Fx::Page(1));
        }

        // R2-9：第二次 noteDirty 必须重新起算（堵第一轮 V04 节流式防抖重放）。不依赖
        // 触发时刻，直接读定时器剩余时间，不会被调度延迟误报（独立审核第二轮报告 N5：
        // 原有的瞬时快照断言在负载下约 10% 概率误报，这里改成读 QTimer::remainingTime()，
        // 不需要真的等到触发那一刻才判断）。
        void CheckR2_SecondNoteDirtyRestartsTimer()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            QTimer* timer = fx.feeder.findChild<QTimer*>();
            WPJ3_CHECK(timer != nullptr);
            if (timer == nullptr) { return; }
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            QThread::msleep(35);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            WPJ3_CHECK(timer->isActive());
            WPJ3_CHECK_NOTE(timer->remainingTime() >= 25, QStringLiteral("第二次 noteDirty 必须重新起算"));
        }

        // R2-10：换目标必须真的停掉定时器（把 D4 修复里 setAddressSpaceBounds/
        // setIdentityKey 的 stop() 变成可观察——第一轮回注 A4a/A4b 显示这两处单靠
        // pendingValid_ 早退已经等价兜底，这里用 isActive() 直接验证 stop() 本身也
        // 确实发生，防止将来有人因为"反正有早退兜底"而删掉它）。
        void CheckR2_RedirectStopsTimerObservably()
        {
            R2Fx fx;
            DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10);
            QTimer* timer = fx.feeder.findChild<QTimer*>();
            WPJ3_CHECK(timer != nullptr);
            if (timer == nullptr) { return; }
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            WPJ3_CHECK(timer->isActive());
            fx.feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
            WPJ3_CHECK_NOTE(!timer->isActive(), QStringLiteral("setAddressSpaceBounds 必须停掉定时器"));
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            WPJ3_CHECK(timer->isActive());
            fx.feeder.setIdentityKey("other-target");
            WPJ3_CHECK_NOTE(!timer->isActive(), QStringLiteral("setIdentityKey 换值必须停掉定时器"));
        }

        // ====================== 本包自补：跨周期不残留 ======================

        // 自补回归：dirtySinceSuspend_ 这个新状态位只应该反映"这一轮挂起/恢复周期"
        // 发生过什么，不能跨到下一轮残留。第一轮里 noteDirty 之后立刻挂起（此时定时
        // 器还在跑），恢复必须补回一次喂入；消费掉这次喂入之后进入第二轮挂起——这一
        // 轮完全没有新的脏事件，恢复后必须是空操作，不能凭上一轮已经用掉的标记位再
        // 多喂一次。本类当前实现没有专门清零这个标记（见 setSuspended 恢复分支旁的
        // 注释：进入挂起永远会先用 isActive() 重新赋值一次，所以不需要），这条测试
        // 就是用来证明"不清零"这个决定确实不会引入跨周期残留的问题。
        void CheckWpJ3_DirtySinceSuspendDoesNotLeakAcrossCycles()
        {
            R2Fx fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10) == HexCanvas::PageResult::Accepted);

            // 第一轮：挂起时定时器还在跑，恢复必须补回一次喂入。
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.setSuspended(true);
            fx.feeder.setSuspended(false);
            WPJ3_CHECK(PumpUntil([&]() { return fx.recorder.refreshedCount >= 1; }, 1000));
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            // 第二轮：挂起期间完全没有新的脏事件（上一轮喂入之后 record_ 与来源代次
            // 都已经对齐），恢复后必须保持是空操作。
            fx.feeder.setSuspended(true);
            fx.feeder.setSuspended(false);
            PumpUntil([]() { return false; }, 200);
            WPJ3_CHECK_NOTE(
                fx.recorder.refreshedCount == 1,
                QStringLiteral("第二轮挂起期间没有任何脏事件，恢复不应凭上一轮用过的标记位再多喂一次"));
        }

        // 自补回归：setSuspended 的幂等守卫（"挂起状态与当前值相同时整个方法是空
        // 操作"）必须真的存在，不是可以删掉的冗余代码。宿主的确认框/审计阶段可能
        // 各自调用一次 setSuspended(true)（互相不清楚对方是否已经挂起过），如果
        // 守卫被删掉，第二次"挂起"会重新执行进入挂起那段代码，用当时（此刻已经被
        // 第一次挂起停掉）的计时器状态覆盖掉第一次挂起期间 noteDirty 记下的
        // dirtySinceSuspend_——把一个合法的、等着恢复时补喂的脏事件标记错误地清
        // 掉。本测试先挂起一次、记一次合法脏事件，再重复调用一次 setSuspended(true)
        // （期望是空操作），验证恢复后那次合法脏事件仍然会被补喂。
        void CheckWpJ3_RedundantSuspendPreservesRememberedDirty()
        {
            R2Fx fx;
            WPJ3_CHECK(DeliverValidPage(fx.canvas, R2Fx::Page(0), 0x10) == HexCanvas::PageResult::Accepted);
            fx.feeder.noteDirty(R2Fx::Page(0), 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK(fx.recorder.refreshedCount == 1);

            fx.feeder.setSuspended(true);
            fx.feeder.noteDirty(R2Fx::Page(0), 2, false); // 挂起期间的合法脏事件
            fx.feeder.setSuspended(true); // 宿主重复调用挂起：幂等守卫应让这是空操作
            fx.feeder.setSuspended(false);
            WPJ3_CHECK_NOTE(
                PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000),
                QStringLiteral("重复调用 setSuspended(true) 不得冲掉已经记住的脏事件标记"));
        }
    }

    void RunReview2Tests()
    {
        CheckR2_SecondNoteDirtyRestartsTimer();
        CheckR2_RedirectStopsTimerObservably();
        CheckR2_ResumeWaitsOneDebounceInterval();
        CheckR2_ResumeThenInFlightCancelsRearmedTimer();
        CheckR2_ReconcileDetectsBaseOnlyChange();
        CheckR2_ReconcileDetectsSizeOnlyChange();
        CheckR2_SetPolicyKeepsPendingRequest();
        CheckR2_PolicyPageSizeMismatchWarns();
        CheckR2_ResumeWithoutSuspendIsNoOp();
        CheckR2_DirtyDuringSuspensionIsRemembered();
        CheckWpJ3_DirtySinceSuspendDoesNotLeakAcrossCycles();
        CheckWpJ3_RedundantSuspendPreservesRememberedDirty();
    }

    namespace
    {
        // DFx：缺陷回归组专用 Fixture，与 R2Fx 结构相同，分开命名避免匿名命名空间
        // 里两个同名类型互相遮蔽（两组历史上分别来自审核者补测文件的两个独立 TU）。
        struct DFx
        {
            HexCanvas canvas;
            NoOpPageProvider provider;
            ksword::memwb::MemoryDiffOverlay overlay;
            WorkbenchBaselineFeeder feeder;
            BaselineRecorder recorder;
            static constexpr std::uint64_t kBase = 0x60000000ULL;
            DFx()
            {
                SetupCanvas(canvas, provider, 0, 0xFFFFFFFFULL);
                feeder.setCanvas(&canvas);
                feeder.setOverlay(&overlay);
                feeder.setAddressSpaceBounds(ksword::memwb::AddressSpaceBounds{0, 0xFFFFFFFFULL});
                recorder.Attach(feeder, feeder);
            }
            static std::uint64_t Page(int index) { return kBase + static_cast<std::uint64_t>(index) * kPageBytes; }
        };

        // N1 回归：恢复不得无视"最后一次 noteDirty 是在途"——在途期间恢复不能喂入，
        // 真正读完之后的 noteDirty 才能喂入最终字节。
        void CheckR2Defect_ResumeHonoursInFlight()
        {
            DFx fx;
            QByteArray before(static_cast<int>(kPageBytes), static_cast<char>(0x10));
            DeliverCustomPage(fx.canvas, DFx::Page(0), before);
            fx.feeder.noteDirty(DFx::Page(0) + 1, 1, false);
            fx.feeder.flushNow();
            fx.feeder.setSuspended(true);
            fx.feeder.noteDirty(DFx::Page(0) + 1, 2, true);
            fx.feeder.setSuspended(false);
            PumpUntil([]() { return false; }, 150);
            WPJ3_CHECK_NOTE(fx.recorder.refreshedCount == 1, QStringLiteral("恢复后读取仍在途，不得喂入"));
            QByteArray after = before;
            after[5] = static_cast<char>(0x99);
            DeliverCustomPage(fx.canvas, DFx::Page(0), after);
            fx.feeder.noteDirty(DFx::Page(0) + 1, 2, false);
            WPJ3_CHECK(PumpUntil([&]() { return fx.recorder.refreshedCount >= 2; }, 1000));
            WPJ3_CHECK(fx.overlay.BaselineByte(DFx::Page(0) + 5).value_or(0) == 0x99);
        }

        // N3 回归：挂起期间 flushNow 不得重算。
        void CheckR2Defect_FlushNowRespectsSuspension()
        {
            DFx fx;
            DeliverValidPage(fx.canvas, DFx::Page(0), 0x10);
            fx.feeder.setSuspended(true);
            fx.feeder.noteDirty(DFx::Page(0), 1, false);
            fx.feeder.flushNow();
            WPJ3_CHECK_NOTE(fx.recorder.refreshedCount == 0 && fx.recorder.unavailableCount == 0,
                QStringLiteral("挂起期间 flushNow 必须是空操作"));
            fx.feeder.setSuspended(false);
        }

        // N2 回归（无挂起期脏事件的变体）：恢复本身不得凭提交前的旧请求起算——这个
        // 变体装配层无需任何配合即可由本类自行堵住（挂起期间完全没有 noteDirty 时，
        // 恢复必须是纯粹的空操作）。
        void CheckR2Defect_ResumeDoesNotClobberAfterEviction()
        {
            DFx fx;
            fx.feeder.setPolicy(ksword::memwb::BaselineWindowPolicy{kPageBytes, 4});
            DeliverValidPage(fx.canvas, DFx::Page(0), 0x10);
            DeliverValidPage(fx.canvas, DFx::Page(1), 0x20);
            const std::uint64_t target = DFx::Page(1) + 8;
            fx.feeder.noteDirty(target, 1, false);
            fx.feeder.flushNow();
            ksword::memwb::DiffBlock block;
            block.address = target;
            block.before = {0x20};
            block.after = {0x77};
            WPJ3_CHECK(fx.overlay.AcceptWrite(block, {0x77}) == ksword::memwb::AcceptWriteStatus::Ok);
            fx.feeder.setSuspended(true);
            for (int index = 0; index < 255; ++index)
            {
                DeliverValidPage(fx.canvas, DFx::kBase + 0x10000000ULL + static_cast<std::uint64_t>(index) * kPageBytes, 0x33);
            }
            fx.feeder.setSuspended(false);
            PumpUntil([]() { return false; }, 200);
            WPJ3_CHECK_NOTE(fx.overlay.BaselineByte(target).value_or(0) == 0x77,
                QStringLiteral("恢复后 50ms 不得用挤页后的旧画布缓存覆盖已确认写入的基线字节"));
            WPJ3_CHECK(fx.overlay.ChangeKind(target) == ksword::memwb::ByteChangeKind::SelfWritten);
        }
    }

    void RunReview2DefectTests()
    {
        CheckR2Defect_ResumeHonoursInFlight();
        CheckR2Defect_FlushNowRespectsSuspension();
        CheckR2Defect_ResumeDoesNotClobberAfterEviction();
    }
}
