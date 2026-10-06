// ============================================================
// wpJ2_tests.Supp.cpp
// 作用：并入独立审核者（Wave 3 review-wpJ2.md）验证过的补测。原报告把这些
// 用例分成两组——16 条"当前实现上已经全绿"的缺口覆盖、9 条描述"应有行为"
// 当时预期失败的 Repro（对应 §1 的 D1-D11）。本文件把两组合并成一份，全部
// 作为"必须通过"的回归确认跑在默认套件里：
// - 16 条缺口覆盖原样迁移，只做了两处机械调整：字段名
//   fx.alignedRevision -> fx.canvasRevision（Wave 3 决策 2 删除了强制对齐
//   辅助函数 AlignCanvasRevisionToTarget，详见 wpJ2_common.h）；取消
//   WPJ2_REPRO 环境变量门槛。
// - 9 条 Repro 按 D1-D11 的实际修法改写断言（不是简单取反"预期失败"）：
//   D2 让 channelFailed 的页保持 Pending 而不是被 cancelPages，所以"重绘
//   不自动重试"现在是"页从不回到 NotLoaded，paintEvent 的自动补读条件
//   天然不成立"；D7/D11 的 retryFailedRanges 会把未页对齐的范围先对齐再读，
//   所以"未对齐范围会白读目标"变成"对齐后真的读到并交付所在的那一页"；
//   D10 把撞上闩锁的信号从"借用 channelUnavailable"改成专门的
//   retryBlockedByLatch，相应的断言也改用新信号。
// ============================================================

#include "wpJ2_common.h"

#include <QCoreApplication>
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

        // 让画布真正可见：之后它会自己 PlanFetch + MarkInFlight（页进入 Pending）。
        void ShowCanvas(ProviderFixture& fx)
        {
            fx.canvas->resize(900, 300);
            fx.canvas->show();
            QCoreApplication::processEvents();
        }

        // 关掉视口重绘，避免"绘制后发现可见页 NotLoaded 就自动补读"的路径干扰 Pending 观测。
        void FreezePaint(ProviderFixture& fx)
        {
            fx.canvas->viewport()->setUpdatesEnabled(false);
        }

        // 不处理事件循环地等端口被调用 n 次（让"完成"事件留在队列里）。
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
        // S1（杀 N07）一次请求多个 range 都要读
        // ------------------------------------------------------------------
        void Test_Supp_MultiRangeRequestReadsEveryRange()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript({MakeOk(OnePage(0x10)), MakeOk(OnePage(0x20)), MakeOk(OnePage(0x30))});
            const HexFetchRange a{0, 1, 0}, b{kPageSize, 1, 0}, c{2 * kPageSize, 1, 0};
            fx.provider->RequestPages({a, b, c}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(2 * kPageSize).byteState == State::Valid; }, 2000));
            WPJ2_CHECK(fx.portPtr->CallCount() == 3);
        }

        // ------------------------------------------------------------------
        // S2（杀 N05）NotAttempted 保持 NotLoaded，不得标不可读
        // ------------------------------------------------------------------
        void Test_Supp_NotAttemptedPagesStayNotLoaded()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(0x41)), MakeOk(OnePage(0x42))});
            fx.provider->RequestPages({HexFetchRange{0, 2, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000));
            PumpUntil([] { return false; }, 100);
            WPJ2_CHECK(fx.portPtr->CallCount() == 1);
            WPJ2_CHECK(fx.canvas->cellStateAt(kPageSize).byteState == State::NotLoaded);
        }

        // ------------------------------------------------------------------
        // S3（杀 N06）一页不可读不得牵连相邻页
        // ------------------------------------------------------------------
        void Test_Supp_UnreadablePageDoesNotSpillToNeighbour()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeUnreadable("pg")});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Unreadable; }, 2000));
            PumpUntil([] { return false; }, 100);
            WPJ2_CHECK(fx.canvas->cellStateAt(kPageSize).byteState == State::NotLoaded);
        }

        // ------------------------------------------------------------------
        // S4a（杀 N01）Gate 不可用时 cancelPages 必须真的撤销画布的在途登记
        // ------------------------------------------------------------------
        void Test_Supp_GateUnavailable_ReleasesPendingRegistrations()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            // 不注入 Gate 输入：一律不可用。
            ShowCanvas(fx);
            FreezePaint(fx);
            PumpUntil([] { return false; }, 100);
            WPJ2_CHECK(fx.canvas->cellStateAt(0).byteState == State::NotLoaded);
        }

        // ------------------------------------------------------------------
        // S4b（杀 N02）陈旧结果必须撤销在途登记
        // ------------------------------------------------------------------
        void Test_Supp_StaleResult_ReleasesPendingRegistrations()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetDelayMs(60);
            std::vector<IoReadResult> script;
            for (int i = 0; i < 8; ++i)
            {
                script.push_back(MakeOk(OnePage(static_cast<std::uint8_t>(i))));
            }
            fx.portPtr->SetScript(script);
            ShowCanvas(fx);
            FreezePaint(fx);
            fx.targetFixture.target->requestReload(); // 只推进目标轴，画布轴不变。
            PumpUntil([&] { return fx.portPtr->CallCount() >= 3; }, 3000);
            PumpUntil([] { return false; }, 300);
            WPJ2_CHECK(fx.canvas->cellStateAt(0).byteState == State::NotLoaded);
        }

        // ------------------------------------------------------------------
        // S4c（杀 N03）channelFailed 必须整批撤销——**D2 决策改法**：不再
        // cancelPages，页保持 Pending（不是 NotLoaded），allowing 之后显式
        // retryFailedRanges。
        // ------------------------------------------------------------------
        void Test_Supp_ChannelFailed_KeepsWholeBatchPending()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript({MakeFailed("boom"), MakeOk(OnePage(1)), MakeOk(OnePage(2)), MakeOk(OnePage(3))});
            bool failed = false;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&] { failed = true; });
            ShowCanvas(fx);
            FreezePaint(fx);
            WPJ2_CHECK(PumpUntil([&] { return failed; }, 3000));
            PumpUntil([] { return false; }, 200);
            const State p1 = fx.canvas->cellStateAt(kPageSize).byteState;
            const State p2 = fx.canvas->cellStateAt(2 * kPageSize).byteState;
            WPJ2_CHECK_NOTE(
                p1 == State::Pending && p2 == State::Pending,
                QStringLiteral("D2：channelFailed 之后应整批保持 Pending，不 cancelPages，实际 %1/%2")
                    .arg(static_cast<int>(p1))
                    .arg(static_cast<int>(p2)));
        }

        // ------------------------------------------------------------------
        // S5（杀 N08/N09）画布自己换代次、target 不变：旧结果必须被画布拒收
        // ------------------------------------------------------------------
        void Test_Supp_CanvasRevisionChangedWhileInFlight_OldPageNotShown()
        {
            for (int variant = 0; variant < 2; ++variant)
            {
                ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
                GateOk(fx, Channel::StandardDriver);
                fx.portPtr->SetDelayMs(100);
                fx.portPtr->SetScript({variant == 0 ? MakeOk(OnePage(0x51)) : MakeUnreadable("pg")});
                fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
                // 画布在读取期间换了地址空间（画布轴 +1），target 的来源代次保持不变。
                fx.canvas->setAddressSpace(kFixtureFirstAddress, kFixtureLastAddress);
                PumpUntil([&] { return fx.portPtr->CallCount() >= 1; }, 2000);
                PumpUntil([] { return false; }, 300);
                const State state = fx.canvas->cellStateAt(0).byteState;
                const State forbidden = (variant == 0) ? State::Valid : State::Unreadable;
                WPJ2_CHECK_NOTE(
                    state != forbidden,
                    QStringLiteral("variant %1：旧画布代次结果不得进入新空间，实际状态 %2")
                        .arg(variant)
                        .arg(static_cast<int>(state)));
            }
        }

        // ------------------------------------------------------------------
        // S6（杀 N10）闩锁按目标轴释放：requestReload 之后同一通道必须恢复读取
        // ------------------------------------------------------------------
        void Test_Supp_LatchReleasedWhenTargetRevisionAdvances()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(0x61))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return latched == 1; }, 2000));
            // 目标轴前进（用户重读）；闩锁属于旧目标代次，画布轴不需要变，
            // provider 内部会自己重新 capture() 拿到新目标代次。
            fx.targetFixture.target->requestReload();
            fx.portPtr->SetScript({MakeOk(OnePage(0x62))});
            const int before = fx.portPtr->CallCount();
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(
                PumpUntil([&] { return fx.portPtr->CallCount() > before; }, 1000),
                QStringLiteral("目标轴前进后闩锁不应再拦截"));
        }

        // ------------------------------------------------------------------
        // S7（杀 N11）非 DDMA 通道不置闩锁
        // ------------------------------------------------------------------
        void Test_Supp_NonDdmaChannelNeverLatches()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            int latched = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(0x71)), MakeOk(OnePage(0x72))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 2000));
            PumpUntil([] { return false; }, 100);
            WPJ2_CHECK(latched == 0);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(kPageSize).byteState == State::Valid; }, 2000));
            WPJ2_CHECK(fx.portPtr->CallCount() == 2);
        }

        // ------------------------------------------------------------------
        // S8（杀 N12，按 D3 改写）：同一次读取**同时**报 channelFailed 与
        // scratchAreaDirty（真实 DDMA 端口就是这么映射的，见 D1）时，脏扇区
        // 信号不能因为走了失败分支就漏报。
        //
        // 原版用两个**不同**的 range（第一个 dirty、第二个 failed）构造这个
        // 场景；D3 落实之后，第一个 range 的 dirty 会立刻把共享的"通道中毒"
        // 标志置位，第二个 range 还没真的问端口就被跳过——不会再产生一个
        // 独立的 channelFailed=true 结果，两个信号不可能再通过"两个 range"
        // 凑到一起。改成单个 range 的单次 Read 同时携带两个标记（这正是 D1
        // 要修的真实场景本身），在"真正会发生"的那种组合下继续验证。
        // ------------------------------------------------------------------
        void Test_Supp_DirtyLatchesEvenWhenSameRangeAlsoFails()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            IoReadResult dirtyAndFailed = MakeFailed("scratch not restored, channel also failed");
            dirtyAndFailed.scratchAreaDirty = true;
            fx.portPtr->SetScript({dirtyAndFailed});
            int latched = 0, failed = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&] { ++failed; });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return failed == 1; }, 2000));
            WPJ2_CHECK_NOTE(latched == 1, QStringLiteral("脏扇区信号不能因为同一次读取也 channelFailed 而漏报，实际 %1").arg(latched));
        }

        // ------------------------------------------------------------------
        // S9（杀 N13）取消发生在同批较晚的 range 时，整批仍要作废
        // ------------------------------------------------------------------
        void Test_Supp_CancelDuringLaterRangeDropsWholeJob()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetDelayMs(120);
            fx.portPtr->SetScript({MakeOk(OnePage(0x91)), MakeOk(OnePage(0x92))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}, HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            BusyWaitCalls(fx.portPtr, 1, 2000);
            fx.provider->cancelAllInFlight();
            PumpUntil([] { return false; }, 600);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(0).byteState != State::Valid,
                QStringLiteral("取消之后第一个 range 的数据也不应展示，实际状态 %1")
                    .arg(static_cast<int>(fx.canvas->cellStateAt(0).byteState)));
        }

        // ------------------------------------------------------------------
        // S10（杀 N14）背靠背多次请求、不处理事件时，每个任务的结果都必须各自落地
        // ------------------------------------------------------------------
        void Test_Supp_BackToBackRequestsEachLand()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0xA1)), MakeOk(OnePage(0xA2)), MakeOk(OnePage(0xA3))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{2 * kPageSize, 1, 0}}, fx.canvasRevision);
            BusyWaitCalls(fx.portPtr, 3, 2000);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            WPJ2_CHECK_NOTE(
                PumpUntil(
                    [&]
                    {
                        return fx.canvas->cellStateAt(0).byteState == State::Valid
                            && fx.canvas->cellStateAt(kPageSize).byteState == State::Valid
                            && fx.canvas->cellStateAt(2 * kPageSize).byteState == State::Valid;
                    },
                    2000),
                QStringLiteral("三个任务的结果应各自落地，不得互相覆盖"));
        }

        // ------------------------------------------------------------------
        // S11（杀 N15）DDMA 会话未就绪 -> 不可用，且不问端口
        // ------------------------------------------------------------------
        void Test_Supp_DdmaGateNeedsSessionReady()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            fx.provider->setGateInputsProvider(
                []
                {
                    GateInputs inputs;
                    inputs.driverLoaded = true;
                    inputs.ddmaSessionReady = false;
                    return inputs;
                });
            GateVerdict verdict;
            bool got = false;
            QObject::connect(
                fx.provider.get(), &WorkbenchPageProvider::channelUnavailable, [&](GateVerdict v) { got = true; verdict = v; });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(got);
            WPJ2_CHECK(verdict.reason == GateReason::SessionNotReady);
            WPJ2_CHECK(fx.portPtr->CallCount() == 0);
        }

        // ------------------------------------------------------------------
        // S12（杀 N16）进程范围没有目标进程 -> NeedsPid
        // ------------------------------------------------------------------
        void Test_Supp_ProcessScopeWithoutPidIsNeedsPid()
        {
            auto* services = new FakeServices();
            std::unique_ptr<IWorkbenchServices> owned(services);
            auto target = std::make_unique<WorkbenchTarget>(std::move(owned)); // 默认会话：进程范围、pid=0、UserMode。
            auto* rawPort = new FakeAsyncMemoryIoPort();
            WorkbenchPageProvider::IoPortFactory factory =
                [rawPort]() -> std::unique_ptr<IMemoryIoPort> { return std::unique_ptr<IMemoryIoPort>(rawPort); };
            auto provider = std::make_unique<WorkbenchPageProvider>(std::move(factory), target.get());
            provider->setGateInputsProvider(
                []
                {
                    GateInputs inputs;
                    inputs.driverLoaded = true;
                    inputs.hasProcessTarget = false;
                    return inputs;
                });
            GateVerdict verdict;
            bool got = false;
            QObject::connect(
                provider.get(), &WorkbenchPageProvider::channelUnavailable, [&](GateVerdict v) { got = true; verdict = v; });
            provider->RequestPages({HexFetchRange{0, 1, 0}}, target->capture().rev.source);
            WPJ2_CHECK(got);
            WPJ2_CHECK(verdict.reason == GateReason::NeedsPid);
            WPJ2_CHECK(rawPort->CallCount() == 0);
        }

        // ------------------------------------------------------------------
        // S14（杀 N22）readFailed 带的是第一条失败文本
        // ------------------------------------------------------------------
        void Test_Supp_FirstFailureTextWins()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript({MakeFailed("first"), MakeFailed("second")});
            QString text;
            QObject::connect(
                fx.provider.get(), &WorkbenchPageProvider::readFailed, [&](quint64, QString t) { text = t; });
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}, HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return !text.isEmpty(); }, 2000));
            WPJ2_CHECK(text == QStringLiteral("first"));
        }

        // ------------------------------------------------------------------
        // S16（杀 N17）析构不得等完整个分块读取：取消标志要在析构里被置位
        // ------------------------------------------------------------------
        void Test_Supp_DestructorCancelsChunkedRead()
        {
            KernelTargetFixture tf = MakeKernelTarget(Channel::StandardDriver);
            auto* rawPort = new FakeAsyncMemoryIoPort();
            rawPort->SetLimits(IoLimits{kPageSize, 0});
            rawPort->SetDelayMs(20);
            std::vector<IoReadResult> script;
            for (int i = 0; i < 50; ++i)
            {
                script.push_back(MakeOk(OnePage(1)));
            }
            rawPort->SetScript(script);
            WorkbenchPageProvider::IoPortFactory factory =
                [rawPort]() -> std::unique_ptr<IMemoryIoPort> { return std::unique_ptr<IMemoryIoPort>(rawPort); };
            auto provider = std::make_unique<WorkbenchPageProvider>(std::move(factory), tf.target.get());
            provider->setGateInputsProvider([] { return MakeAvailableGateInputs(Channel::StandardDriver); });
            provider->RequestPages({HexFetchRange{0, 50, 0}}, tf.target->capture().rev.source);
            BusyWaitCalls(rawPort, 1, 2000);
            QElapsedTimer timer;
            timer.start();
            provider.reset();
            const qint64 elapsed = timer.elapsed();
            WPJ2_CHECK_NOTE(
                elapsed < 400, QStringLiteral("析构耗时 %1 ms：50 次 20ms 的读取应在第二次读之前被取消").arg(elapsed));
        }

        // ================================================================
        // 原 Repro 组：按 D1-D11 的实际修法改写为"必须通过"。
        // ================================================================

        // R1（D1，已在 MemoryPageReader 层修复）：真实端口把"暂存区弄脏"报成
        // Failed + scratchAreaDirty，闩锁必须照样置位并发信号。
        void Test_Supp_R1_DirtyReportedAsFailedMustLatch()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int latched = 0, failed = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&] { ++failed; });
            IoReadResult dirtyFail = MakeFailed("scratch not restored");
            dirtyFail.scratchAreaDirty = true;
            fx.portPtr->SetScript({dirtyFail});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return failed > 0 || latched > 0; }, 2000));
            WPJ2_CHECK_NOTE(latched == 1, QStringLiteral("Failed+scratchAreaDirty 必须置闩锁并发信号，实际 %1").arg(latched));
            const int before = fx.portPtr->CallCount();
            fx.portPtr->SetScript({MakeOk(OnePage(2))});
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            PumpUntil([] { return false; }, 150);
            WPJ2_CHECK_NOTE(fx.portPtr->CallCount() == before, QStringLiteral("闩锁命中后同目标代次请求不得再碰端口"));
        }

        // R2a/R2b（D3）：同一批请求里，第一个 range 报脏/失败后，后续 range
        // 必须被跳过，一次端口调用都不发起。
        void Test_Supp_R2_NoReadsAfterDirtyOrFailedInSameJob()
        {
            {
                ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
                GateOk(fx, Channel::Ddma);
                fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
                fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1)), MakeOk(OnePage(2)), MakeOk(OnePage(3))});
                fx.provider->RequestPages(
                    {HexFetchRange{0, 1, 0}, HexFetchRange{kPageSize, 1, 0}, HexFetchRange{2 * kPageSize, 1, 0}},
                    fx.canvasRevision);
                PumpUntil([] { return false; }, 400);
                WPJ2_CHECK_NOTE(
                    fx.portPtr->CallCount() == 1,
                    QStringLiteral("R2a：第一个 range 报暂存区弄脏后，同批后续 range 不得再读，实际端口调用 %1")
                        .arg(fx.portPtr->CallCount()));
            }
            {
                ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
                GateOk(fx, Channel::StandardDriver);
                fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
                fx.portPtr->SetScript({MakeFailed("dead"), MakeOk(OnePage(2)), MakeOk(OnePage(3))});
                fx.provider->RequestPages(
                    {HexFetchRange{0, 1, 0}, HexFetchRange{kPageSize, 1, 0}, HexFetchRange{2 * kPageSize, 1, 0}},
                    fx.canvasRevision);
                PumpUntil([] { return false; }, 400);
                WPJ2_CHECK_NOTE(
                    fx.portPtr->CallCount() == 1,
                    QStringLiteral("R2b：第一个 range 通道失败后，同批后续 range 不得再读，实际端口调用 %1")
                        .arg(fx.portPtr->CallCount()));
            }
        }

        // R3（D3）：第一个任务落地置闩锁之前，已经排队的第二个任务也不得
        // 再碰暂存区——共享的"通道中毒"标志在工作线程里就会拦住它，不需要
        // 等到 UI 线程落地更新 scratchLatched_。
        void Test_Supp_R3_ScratchDirtyStopsAlreadyQueuedJobs()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            fx.portPtr->SetDelayMs(60);
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1)), MakeOk(OnePage(2))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision); // 通过前置判定，已排队。
            PumpUntil([] { return false; }, 500);
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == 1,
                QStringLiteral("R3：第一个任务落地置闩锁后，已排队的第二个任务不得再碰暂存区，实际端口调用 %1")
                    .arg(fx.portPtr->CallCount()));
        }

        // R4（D4）：cancelAllInFlight 之后，已读完但完成事件还卡在队列里的
        // 结果也必须丢弃，不得交付成 Valid。
        void Test_Supp_R4_CancelAllDropsFinishedButUndeliveredResult()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0x44))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            BusyWaitCalls(fx.portPtr, 1, 2000);
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 工作线程已读完，完成事件还在队列里。
            fx.provider->cancelAllInFlight();
            PumpUntil([] { return false; }, 300);
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(0).byteState != State::Valid,
                QStringLiteral("R4：cancelAllInFlight 之后，已读完但未落地的结果也必须丢弃"));
        }

        // R5（D2）：channelFailed 之后，普通重绘不得触发端口重试——现在的
        // 机制是页保持 Pending（不是 NotLoaded），paintEvent 的自动补读
        // 条件天然不成立，不再依赖"画布从不重绘"这个脆弱前提。
        void Test_Supp_R5_RepaintMustNotRetryAfterChannelFailed()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            int failed = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::readFailed, [&] { ++failed; });
            ShowCanvas(fx);
            WPJ2_CHECK(PumpUntil([&] { return failed > 0; }, 2000));
            PumpUntil([] { return false; }, 150);
            const int before = fx.portPtr->CallCount();
            for (int i = 0; i < 3; ++i)
            {
                fx.canvas->viewport()->repaint();
                PumpUntil([] { return false; }, 100);
            }
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == before,
                QStringLiteral("R5：channelFailed 之后普通重绘不得触发端口重试，端口调用 %1 -> %2")
                    .arg(before)
                    .arg(fx.portPtr->CallCount()));
        }

        // R6（D5/D6）：陈旧任务落地时的 cancelPages 不得抹掉新地址空间自己
        // 刚登记的在途页。
        void Test_Supp_R6_StaleCancelMustNotWipeNewSpacePending()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetDelayMs(150);
            std::vector<IoReadResult> script;
            for (int i = 0; i < 40; ++i)
            {
                script.push_back(MakeOk(OnePage(static_cast<std::uint8_t>(i))));
            }
            fx.portPtr->SetScript(script);
            QElapsedTimer clock;
            clock.start();
            ShowCanvas(fx);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            QCoreApplication::processEvents();
            fx.targetFixture.target->requestReload();
            fx.canvas->refresh(); // 新空间重新登记在途，R2 排在 R1 后面（R1=陈旧，R2=新鲜）。
            WPJ2_CHECK(PumpUntil([&] { return clock.elapsed() >= 560; }, 3000));
            WPJ2_CHECK_NOTE(
                fx.canvas->cellStateAt(0).byteState == State::Pending,
                QStringLiteral("R6：旧任务的 cancelPages 不得抹掉新空间的在途登记，实际 %1")
                    .arg(static_cast<int>(fx.canvas->cellStateAt(0).byteState)));
        }

        // R7（D10）：撞上闩锁不再是"什么都不提示"——改发专门的
        // retryBlockedByLatch 信号（不复用 channelUnavailable，Core 的
        // GateReason 里没有合适的"暂存区脏"原因）。
        void Test_Supp_R7_LatchHitEmitsRetryBlockedByLatch()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::Ddma);
            GateOk(fx, Channel::Ddma);
            int blocked = 0, latched = 0;
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::retryBlockedByLatch, [&] { ++blocked; });
            QObject::connect(fx.provider.get(), &WorkbenchPageProvider::scratchAreaDirtyLatched, [&] { ++latched; });
            fx.portPtr->SetScript({MakeOkWithScratchDirty(OnePage(1))});
            fx.provider->RequestPages({HexFetchRange{0, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK(PumpUntil([&] { return latched > 0; }, 2000));
            fx.provider->RequestPages({HexFetchRange{kPageSize, 1, 0}}, fx.canvasRevision);
            WPJ2_CHECK_NOTE(
                blocked >= 1, QStringLiteral("R7：撞上 DDMA 闩锁应该发 retryBlockedByLatch，实际发了 %1 次").arg(blocked));
        }

        // R8（D7/D11）：retryFailedRanges 对未页对齐的范围先对齐再读——不再
        // 是"白读目标然后被画布整页拒收"，而是真的读到并交付所在的那一整页。
        void Test_Supp_R8_UnalignedRetryRangeGetsAlignedAndDelivered()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            GateOk(fx, Channel::StandardDriver);
            // 0x1234 这个"范围"按字面（起点 0x1234、1 页）覆盖到 0x1234+4095=0x2233，
            // 跨了两个真实页（0x1000 与 0x2000）；归一化之后应该变成
            // {0x1000, 2, 0}。每次 Read 限制到一页，逼着端口按页分两次调用，
            // 两条脚本分别对应这两页。
            fx.portPtr->SetLimits(IoLimits{kPageSize, 0});
            fx.portPtr->SetScript({MakeOk(OnePage(1)), MakeOk(OnePage(2))});
            fx.provider->retryFailedRanges({HexFetchRange{0x1234, 1, 0}});
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0x1234).byteState == State::Valid; }, 1000));
            WPJ2_CHECK_NOTE(
                fx.portPtr->CallCount() == 2,
                QStringLiteral("R8：未页对齐的范围应该先对齐到所在页再读一次，实际端口调用 %1")
                    .arg(fx.portPtr->CallCount()));
        }

        // R9（D8）：canvas 轴与 target 轴不对齐时，retryFailedRanges 仍应能
        // 把页回填成功（回填用画布轴，不是 target 轴）。
        void Test_Supp_R9_RetryWorksWhenCanvasAndTargetAxesDiffer()
        {
            ProviderFixture fx = MakeProviderFixture(Channel::StandardDriver);
            fx.canvas->setAddressSpace(0, kFixtureLastAddress); // 此时 Gate 未开：不产生端口调用。
            fx.canvas->setAddressSpace(0, kFixtureLastAddress);
            GateOk(fx, Channel::StandardDriver);
            fx.portPtr->SetScript({MakeOk(OnePage(0x88))});
            fx.provider->retryFailedRanges({HexFetchRange{0, 1, 0}});
            WPJ2_CHECK(PumpUntil([&] { return fx.canvas->cellStateAt(0).byteState == State::Valid; }, 1000));
        }
    }

    void RunSuppTests()
    {
        Test_Supp_MultiRangeRequestReadsEveryRange();
        Test_Supp_NotAttemptedPagesStayNotLoaded();
        Test_Supp_UnreadablePageDoesNotSpillToNeighbour();
        Test_Supp_GateUnavailable_ReleasesPendingRegistrations();
        Test_Supp_StaleResult_ReleasesPendingRegistrations();
        Test_Supp_ChannelFailed_KeepsWholeBatchPending();
        Test_Supp_CanvasRevisionChangedWhileInFlight_OldPageNotShown();
        Test_Supp_LatchReleasedWhenTargetRevisionAdvances();
        Test_Supp_NonDdmaChannelNeverLatches();
        Test_Supp_DirtyLatchesEvenWhenSameRangeAlsoFails();
        Test_Supp_CancelDuringLaterRangeDropsWholeJob();
        Test_Supp_BackToBackRequestsEachLand();
        Test_Supp_DdmaGateNeedsSessionReady();
        Test_Supp_ProcessScopeWithoutPidIsNeedsPid();
        Test_Supp_FirstFailureTextWins();
        Test_Supp_DestructorCancelsChunkedRead();

        Test_Supp_R1_DirtyReportedAsFailedMustLatch();
        Test_Supp_R2_NoReadsAfterDirtyOrFailedInSameJob();
        Test_Supp_R3_ScratchDirtyStopsAlreadyQueuedJobs();
        Test_Supp_R4_CancelAllDropsFinishedButUndeliveredResult();
        Test_Supp_R5_RepaintMustNotRetryAfterChannelFailed();
        Test_Supp_R6_StaleCancelMustNotWipeNewSpacePending();
        Test_Supp_R7_LatchHitEmitsRetryBlockedByLatch();
        Test_Supp_R8_UnalignedRetryRangeGetsAlignedAndDelivered();
        Test_Supp_R9_RetryWorksWhenCanvasAndTargetAxesDiffer();
    }
}
