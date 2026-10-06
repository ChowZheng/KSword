// ============================================================
// wpJ2_tests.Decision2.cpp
// 作用：Wave 3 审核决策 2（两条数轴）落实之后的专项覆盖，以及本包自己新增的
// 8 个变异点（D3 的 ResetPoison、D4/D6 的 cancelAllInFlight 画布代次守卫、
// D7/D11 的 retryFailedRanges 容量夹取、rereadByteRange 的去重/闩锁/
// pendingJobs_ 清理）。每条测试对应的变异见 fix-wpJ2.md 的变异重放表，这里
// 只写清楚"这条测试在保护哪一行代码的哪一种写法"。
// ============================================================

#include "wpJ2_common.h"

#include <QElapsedTimer>

#include <chrono>
#include <thread>

using namespace ks::ui;
using namespace ksword::memwb;

namespace wpJ2_test
{
    namespace
    {
        using State = HexCanvas::ByteState;

        std::vector<std::uint8_t> OnePage(std::uint8_t seed)
        {
            return MakePattern(seed, static_cast<std::size_t>(kPageSize));
        }

        void GateOk(ProviderFixture& fx, Channel channel)
        {
            fx.provider->setGateInputsProvider([channel]() { return MakeAvailableGateInputs(channel); });
        }

        void BusyWaitCalls(FakeAsyncMemoryIoPort* p, int n, int ms)
        {
            QElapsedTimer t;
            t.start();
            while (p->CallCount() < n && t.elapsed() < ms)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // ------------------------------------------------------------------
        // N-1：两条数轴故意错开的端到端往返——画布连续调用两次 setAddressSpace
        // （画布轴从 1 推到 3），target 只 requestReload 一次（目标轴从初始值
        // 推进一步，数值上几乎不可能与画布轴相等）。断言：只要两条轴各自没有
        // 在"请求"与"落地"之间变化，回填照常成功——这是决策 2 之后唯一需要
        // 成立的不变式，不要求两条轴数值相等。
        // 保护点：如果 RequestPages/onJobFinishedOnUiThread 哪里把两条轴的值
        // 弄混了（例如回填时误用目标轴，或闩锁误用画布轴），这条测试会因为
        // "两条轴数值恰好不相等"而必然失败，不会像旧夹具那样被意外对齐掩盖。
        // ------------------------------------------------------------------
        void Test_MismatchedAxes_RoundTripStillDelivers()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            // 画布轴多推两步、目标轴只推一步，故意制造一个不对称的差值——
            // 避免"两边各自从不同起点出发、但推进次数恰好相同"这种凑巧对齐
            // （两条轴各自的换代规则都是"每次 +1"，推同样的次数会撞上同一个
            // 数，这不是对齐机制生效，纯粹是巧合，之前就因为这个巧合漏判过
            // 一次，故意错开推进次数以后不会再犯）。
            fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            fx.targetFixture.target->requestReload();
            GateOk(fx, Channel::StandardDriver);

            const std::uint64_t canvasRevision = fx.canvas->sourceRevision();
            const std::uint64_t targetRevision = fx.targetFixture.target->capture().rev.source;
            WPJ2_CHECK_NOTE(
                canvasRevision != targetRevision,
                QStringLiteral("测试前提：两条轴此刻应该不相等，实际都是 %1").arg(canvasRevision));

            fx.portPtr->SetScript({MakeOk(OnePage(0x20))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000));
            WPJ2_CHECK(fx.canvas->cellStateAt(0).value == 0x20);
        }

        // ------------------------------------------------------------------
        // N-2（保护 submitJob 里的 readPool_->ResetPoison() 调用）：通道失败
        // 一次之后显式 retryFailedRanges，重试必须真的问端口，不能被 D3 的
        // "通道中毒"标志永久封死——它只应该拦住"已经排在坏任务后面的旧任务"，
        // 不应该拦住"全新提交的一次请求"。如果 ResetPoison() 被删掉或挪错
        // 位置，这条测试会因为端口调用次数不增加而失败。
        // ------------------------------------------------------------------
        void Test_RetryAfterChannelFailed_PoisonFlagDoesNotStickForNewSubmission()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeFailed("boom")});
            bool failed = false;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&] { failed = true; });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return failed; }, 2000));

            const int before = fx.portPtr->CallCount();
            fx.portPtr->SetScript({MakeOk(OnePage(0x30))});
            fx.provider->retryFailedRanges({HexFetchRange{0, 1, 0}});
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000),
                QStringLiteral("显式重试不应被 D3 的通道中毒标志拦住"));
            WPJ2_CHECK(fx.portPtr->CallCount() == before + 1);
        }

        // ------------------------------------------------------------------
        // N-3（保护 normalizeRangeForRetry 的容量夹取）：retryFailedRanges
        // 传入一个远超缓存容量的页数，端口调用次数必须被夹到
        // HexViewport::kMaxCachedPages，不应该按原始页数一直读下去。
        // ------------------------------------------------------------------
        void Test_RetryFailedRanges_ClampsHugePageCount()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0}); // 每次只读一页，逼着逐页计数。
            std::vector<IoReadResult> script;
            // 故意比容量上限多准备几条：如果夹取被删掉，端口会一直读到脚本
            // 用尽（260 条）或更久；如果夹取生效，恰好读 256 条就停。
            for (int i = 0; i < 260; ++i)
            {
                script.push_back(MakeOk(OnePage(static_cast<std::uint8_t>(i))));
            }
            fx.portPtr->SetScript(script);

            fx.provider->retryFailedRanges({HexFetchRange{0, 100000, 0}});
            WPJ2_CHECK(PumpUntil([&] { return fx.portPtr->CallCount() >= 256; }, 5000));
            PumpUntil([] { return false; }, 200);
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == 256,
                QStringLiteral("retryFailedRanges 应该把页数夹到缓存容量 256，实际端口调用 %1 次")
                    .arg(fx.portPtr->CallCount()));
        }

        // ------------------------------------------------------------------
        // N-4（保护 rereadByteRange 的去重逻辑）：对同一段字节范围背靠背调用
        // 两次 rereadByteRange（第一次还没落地），第二次应该因为"这一页已经
        // 在某个在途任务的记账范围内"而被跳过，不应该再提交第二个任务。
        // ------------------------------------------------------------------
        void Test_RereadByteRange_DedupsAgainstInFlightJob()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(150);
            fx.portPtr->SetScript({MakeOk(OnePage(0x55)), MakeOk(OnePage(0x56))});

            fx.provider->rereadByteRange(16, 8); // 字节 [16,24) 落在第 0 页。
            fx.provider->rereadByteRange(32, 8); // 同一页（第 0 页），应该被去重跳过。

            PumpUntil([] { return false; }, 400);
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == 1,
                QStringLiteral("同一页在途时第二次 rereadByteRange 不应该再提交新任务，实际端口调用 %1 次")
                    .arg(fx.portPtr->CallCount()));
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000));
        }

        // ------------------------------------------------------------------
        // N-5（保护 pendingJobs_.erase(jobTicket) 没有被漏掉）：第一次
        // rereadByteRange 完整落地之后（事件循环已经跑过，pendingJobs_
        // 应该已经摘掉这条记账），第二次对**同一页**的 rereadByteRange 必须
        // 是一次全新的请求，真的再问一次端口——如果落地时忘了把自己的记账
        // 从 pendingJobs_ 摘掉，这条陈旧记账会被下一次调用的去重逻辑误认成
        // "还在途"，第二次调用会被错误跳过。
        // ------------------------------------------------------------------
        void Test_RereadByteRange_SecondCallAfterFirstLandsActuallyReads()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0x61)), MakeOk(OnePage(0x62))});

            fx.provider->rereadByteRange(16, 8);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000));
            PumpUntil([] { return false; }, 100); // 确保落地回调已经跑完，pendingJobs_ 应该已清空这一条。

            fx.provider->rereadByteRange(16, 8);
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.portPtr->CallCount() >= 2; }, 2000),
                QStringLiteral("第一次已经落地之后，第二次 rereadByteRange 不应该被当成仍在途而跳过"));
        }

        // ------------------------------------------------------------------
        // N-6（保护 RequestPages 的 DDMA 闩锁分支里 rereadByteRange 共用的
        // retryBlockedByLatch 发射点）：rereadByteRange 撞上闩锁时同样要发
        // retryBlockedByLatch，不是只有直接调用 RequestPages 才会发。
        // ------------------------------------------------------------------
        void Test_RereadByteRange_BlockedByLatchEmitsSignal()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0, blocked = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::retryBlockedByLatch, [&] { ++blocked; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return latched > 0; }, 2000));

            fx.provider->rereadByteRange(kPageSize + 16, 8); // 另一页，但同一目标代次，仍应被闩锁拦住。
            WPJ2_CHECK_NOTE(blocked >= 1, QStringLiteral("rereadByteRange 撞上闩锁也应该发 retryBlockedByLatch"));
        }

        // ------------------------------------------------------------------
        // N-7（保护 onJobFinishedOnUiThread 新增的 canvasStale 整段跳过
        // 守卫，D6 的同族延伸）：job1 在旧画布代次下发起（第 0 页
        // Ok+scratchAreaDirty，第 1 页因此 NotAttempted——NotAttempted 分支
        // 会调用 cancelPages，而 cancelPages 本身没有任何代次校验）；job1
        // 发起后画布立刻换新地址空间（画布轴前进，target 不变，isSourceFresh
        // 会判定"新鲜"）。job1 落地时，它的 NotAttempted 分支不应该用"旧画布
        // 代次"按地址去 cancelPages 新空间刚刚重新登记在途的同一页。
        // ------------------------------------------------------------------
        void Test_StaleCanvasJob_DoesNotWipeNewSpacePendingViaCancelPages()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetDelayMs(80);
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(0x70)), MakeOk(OnePage(0x71))});
            // job1：两页一批，第 0 页 Ok+dirty，第 1 页因此 NotAttempted。
            fx.provider->RequestPages({HexFetchRange{0, 2, 0}}, fx.canvasRevision);

            // 画布换新地址空间：画布轴前进，target 不变；新空间自己的
            // requestVisiblePages() 会重新登记可见页 + 前后预取页在途。
            fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
            fx.canvas->viewport()->setUpdatesEnabled(false); // 冻结重绘，避免干扰下面的状态观测。

            WPJ2_CHECK(PumpUntil([&] { return fx.portPtr->CallCount() >= 1; }, 2000));
            PumpUntil([] { return false; }, 300); // 等 job1 真正落地（target 没变，不会被判定代次不符丢弃）。

            const State state = fx.canvas->cellStateAt(kPageSize).byteState;
            WPJ2_CHECK_NOTE(
                state != State::NotLoaded,
                QStringLiteral("job1（旧画布代次）落地时按地址发出的 cancelPages 不应该抹掉新地址空间的"
                               "在途登记，实际状态 %1")
                    .arg(static_cast<int>(state)));
        }

        // ------------------------------------------------------------------
        // N-7b（保护 RequestPages 里 lastGateUnavailableReason_ 的去抖逻辑，
        // D2 续）：同一个 GateReason 连续命中只应该发一次 channelUnavailable；
        // Gate 恢复可用之后再变不可用（哪怕原因相同）要重新发一次。
        // ------------------------------------------------------------------
        void Test_GateUnavailable_SameReasonDebouncedButAvailableResetsIt()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            // 构造阶段的 setAddressSpace 已经触发过一次内部请求，当时也是
            // "未注入"状态，已经消耗了一次去抖记忆——先用一次"可用"请求把它
            // 重置（与 Gate.cpp 的 Test_GateUnavailable_NoPortCalls 同一处理），
            // 确保接下来观察的是一组全新的、从"可用"切回"不可用"的事件。
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0x01))});
            fx.provider->RequestPages({HexFetchRange{4 * kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.portPtr->CallCount() >= 1; }, 1000));
            fx.provider->setGateInputsProvider(nullptr);

            int unavailableCount = 0;
            QObject::connect(
                fx.provider.get(), &WorkbenchPageProvider::channelUnavailable, [&] { ++unavailableCount; });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(
                unavailableCount == 1,
                QStringLiteral("同一个 GateReason 连续命中应该只发一次信号，实际发了 %1 次").arg(unavailableCount));

            // 切到可用再切回不可用（原因相同）：应该重新发一次。
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0x40))});
            fx.provider->RequestPages({HexFetchRange{2 * kPageSize, 1, 0}}, fx.canvasRevision);
            fx.provider->setGateInputsProvider(nullptr);
            fx.provider->RequestPages({HexFetchRange{3 * kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(
                unavailableCount == 2,
                QStringLiteral("Gate 恢复可用之后再次不可用应该重新发信号，实际累计 %1 次").arg(unavailableCount));
        }

        // ------------------------------------------------------------------
        // N-3b（保护 RequestPages 自己入口处的 pageCount 夹取，与
        // normalizeRangeForRetry 内部的夹取是两处独立代码，分别覆盖"直接调用
        // RequestPages"与"经 retryFailedRanges/rereadByteRange 规整后再调用"
        // 两条路径）：直接传一个远超容量的 pageCount 给 RequestPages，端口
        // 调用次数也必须被夹到 256。
        // ------------------------------------------------------------------
        void Test_RequestPages_ClampsHugePageCountDirectly()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            std::vector<IoReadResult> script;
            for (int i = 0; i < 260; ++i)
            {
                script.push_back(MakeOk(OnePage(static_cast<std::uint8_t>(i))));
            }
            fx.portPtr->SetScript(script);

            fx.provider->RequestPages({HexFetchRange{0, 100000, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.portPtr->CallCount() >= 256; }, 5000));
            PumpUntil([] { return false; }, 200);
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == 256,
                QStringLiteral("RequestPages 自己入口也应该把 pageCount 夹到 256，实际端口调用 %1 次")
                    .arg(fx.portPtr->CallCount()));
        }

        // ------------------------------------------------------------------
        // N-8（保护 D4 的 jobGeneration 核对，独立于 R4 的"读完未落地"场景，
        // 覆盖"仍在排队、从未开始跑"这一种更直接的丢弃路径）：发起两个请求，
        // 第二个还没来得及开始跑就 cancelAllInFlight，第二个的页不应该在
        // 任何后续时刻被交付。
        // ------------------------------------------------------------------
        void Test_CancelAllInFlight_DropsQueuedNeverRunJob()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetDelayMs(200);
            fx.portPtr->SetScript({MakeOk(OnePage(0x80)), MakeOk(OnePage(0x81))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision); // 排在队列里，还没开始跑。
            fx.provider->cancelAllInFlight();
            PumpUntil([] { return false; }, 600);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(kPageSize).byteState != State::Valid,
                QStringLiteral("排队中、从未开始跑的任务被 cancelAllInFlight 丢弃之后不应该再被交付"));
        }
    }

    void RunDecision2Tests()
    {
        Test_MismatchedAxes_RoundTripStillDelivers();
        Test_RetryAfterChannelFailed_PoisonFlagDoesNotStickForNewSubmission();
        Test_RetryFailedRanges_ClampsHugePageCount();
        Test_RequestPages_ClampsHugePageCountDirectly();
        Test_RereadByteRange_DedupsAgainstInFlightJob();
        Test_RereadByteRange_SecondCallAfterFirstLandsActuallyReads();
        Test_RereadByteRange_BlockedByLatchEmitsSignal();
        Test_GateUnavailable_SameReasonDebouncedButAvailableResetsIt();
        Test_StaleCanvasJob_DoesNotWipeNewSpacePendingViaCancelPages();
        Test_CancelAllInFlight_DropsQueuedNeverRunJob();
    }
}
